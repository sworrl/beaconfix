// BeaconFix BLE link — see blelink.h and docs/RANGING.md §9.1.
#include "blelink.h"
#include "rangemath.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDateTime>

using InterfaceMap = QMap<QString, QVariantMap>;
using ManagedObjects = QMap<QDBusObjectPath, InterfaceMap>;
Q_DECLARE_METATYPE(InterfaceMap)
Q_DECLARE_METATYPE(ManagedObjects)

static const QString kAdvPath = QStringLiteral("/org/sworrl/beaconfix/ble/adv0");
static const QString kBluez = QStringLiteral("org.bluez");
static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

BleLink::BleLink(QObject *parent) : QObject(parent)
{
    qDBusRegisterMetaType<InterfaceMap>();
    qDBusRegisterMetaType<ManagedObjects>();
    m_rotate.setInterval(30 * 1000);
    connect(&m_rotate, &QTimer::timeout, this, &BleLink::rotate);
    m_duty.setInterval(1000);
    connect(&m_duty, &QTimer::timeout, this, &BleLink::dutyTick);
    m_hold.setInterval(1000);
    connect(&m_hold, &QTimer::timeout, this, &BleLink::holdTick);
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        if (m_started && !m_advertising && !m_registering) probeTxPower();   // re-read the TX caps, then register
    });
}

bool BleLink::shapeError(const QString &name, const QString &message)
{
    if (name.endsWith(QLatin1String(".InvalidArguments")) || name.endsWith(QLatin1String(".InvalidLength"))) return true;
    const QString m = message.toLower();
    return m.contains(QLatin1String("parse")) || m.contains(QLatin1String("too long")) || m.contains(QLatin1String("length"))
        || m.contains(QLatin1String("invalid"));
}

void BleLink::scheduleRetry()
{
    if (!m_started) return;
    m_retry.start(m_retryDelayMs);
    m_retryDelayMs = std::min(kRetryMaxMs, m_retryDelayMs * 2);
}

// bluetoothd went away or came back (a restart, a crash, the OOM killer): a new one knows nothing of our advertisement
// or discovery session, and we cannot learn that from a reply we never get. Start both again.
void BleLink::onBluezOwnerChanged(const QString &, const QString &, const QString &newOwner)
{
    if (!m_started) return;
    qInfo("beaconfix: BlueZ %s: advertising and scanning start again", newOwner.isEmpty() ? "went away" : "(re)started");
    ++m_regGen;                                                 // replies to the old daemon no longer count
    const bool was = m_advertising;
    m_advertising = m_registering = m_reregister = false;
    m_scanning = false; m_scanQuietMs = 0;
    m_devs.clear();
    if (was) emit advertisingChanged(false);
    m_retry.stop(); m_retryDelayMs = kRetryMinMs;
    if (newOwner.isEmpty()) return;                              // wait for it to come back
    QTimer::singleShot(2000, this, [this] {
        if (!m_started) return;
        setDiscovery(true);
        if (!m_advertising && !m_registering) probeTxPower();
    });
}

BleLink::~BleLink() { stop(); }

void BleLink::setIdentity(const QString &identityId)
{
    if (identityId == m_identity) return;
    m_identity = identityId;
    if (m_advertising) { unregisterAdvert(); registerAdvert(); }
    else if (m_registering) m_reregister = true;
}
void BleLink::setFlags(bool rttResponder, bool apiReachable, int kind, bool calibrating)
{
    const int f = RangeMath::bleFlags(rttResponder, apiReachable, kind, calibrating);
    if (f == m_flags) return;
    m_flags = f;
    if (m_advertising) { unregisterAdvert(); registerAdvert(); }
    else if (m_registering) m_reregister = true;
}
void BleLink::setTxPower(int dbm)
{
    m_txWant = dbm;
    if (m_started) probeTxPower();
}

// BlueZ ≥ 5.6x on an extended-advertising controller offers "CanSetTxPower" and the controller's
// [MinTxPower, MaxTxPower]. Without a request the controller picks its own level (the AX210 said 10 dBm
// while reporting max 7), so byte 8 had to say 127 and every peer fell back to a −59 dBm reference.
// Asynchronous: this runs on the tray's GUI thread (start(), setTxPower()), and a busy bluetoothd must not
// freeze it. The advert is registered once the answer (or the 2 s timeout) is in.
void BleLink::probeTxPower()
{
    const int gen = ++m_probeGen;
    if (m_txWant == 127 || m_adapter.isEmpty()) { applyTxCaps(false, -127, 20); return; }
    QDBusMessage call = QDBusMessage::createMethodCall(kBluez, m_adapter, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    call << QStringLiteral("org.bluez.LEAdvertisingManager1");
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 2000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, gen](QDBusPendingCallWatcher *c) {
        c->deleteLater();
        if (gen != m_probeGen || !m_started) return;             // superseded by a newer probe, or stopped meanwhile
        const QDBusMessage r = c->reply();
        bool settable = false; int lo = -127, hi = 20;
        const QString en = r.errorName();
        if (r.type() == QDBusMessage::ErrorMessage && (en.endsWith(QLatin1String(".NoReply")) || en.endsWith(QLatin1String(".Timeout"))
                                                       || en.endsWith(QLatin1String(".ServiceUnknown")) || en.endsWith(QLatin1String(".NameHasNoOwner"))
                                                       || en.endsWith(QLatin1String(".Disconnected")))) {
            // bluetoothd busy (NoReply) or not there: no answer about the caps is not "cannot set TX power"
            m_error = QStringLiteral("LEAdvertisingManager1: %1").arg(r.errorMessage());
            if (!m_advertising && !m_registering) scheduleRetry();
            return;
        }
        if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty()) {
            const QVariantMap props = qdbus_cast<QVariantMap>(r.arguments().at(0));
            settable = props.value(QStringLiteral("SupportedFeatures")).toStringList().contains(QStringLiteral("CanSetTxPower"));
            QVariant capsV = props.value(QStringLiteral("SupportedCapabilities"));
            const QVariantMap caps = capsV.canConvert<QDBusArgument>() ? qdbus_cast<QVariantMap>(capsV.value<QDBusArgument>()) : capsV.toMap();
            if (caps.contains(QStringLiteral("MinTxPower"))) lo = caps.value(QStringLiteral("MinTxPower")).toInt();
            if (caps.contains(QStringLiteral("MaxTxPower"))) hi = caps.value(QStringLiteral("MaxTxPower")).toInt();
        }
        applyTxCaps(settable, lo, hi);
    });
}

void BleLink::applyTxCaps(bool settable, int lo, int hi)
{
    const int before = m_tx;
    m_txSettable = settable && lo <= hi;
    m_tx = m_txSettable ? std::max(lo, std::min(hi, m_txWant)) : 127;
    if (m_tx != before) { m_txConfirmed = false; m_txAdoptions = 0; emit txPowerChanged(m_tx); }
    if (!m_started) return;
    if (!m_advertising && !m_registering) registerAdvert();         // the first registration waits for the probe
    else if (m_tx != before && m_advertising) { unregisterAdvert(); registerAdvert(); }
}

// BlueZ writes the level the controller actually selected (HCI LE Set Extended Advertising Parameters →
// Selected_TX_Power, via MGMT Add Ext Adv Params) back into our advertisement's TxPower property. That, not
// the request, is what byte 8 must carry: adopt it and re-register once so the peers get it.
void BleLink::onTxSelected(int dbm)
{
    if (dbm < -127 || dbm > 20) return;
    m_txConfirmed = true;
    if (dbm == m_tx || m_txAdoptions >= 2) return;              // (a controller that never settles must not make us re-register forever)
    ++m_txAdoptions;
    m_tx = dbm;
    emit txPowerChanged(m_tx);
    // BlueZ writes it before it answers RegisterAdvertisement: then re-register when that answer is in
    if (m_advertising) { unregisterAdvert(); registerAdvert(); }
    else if (m_registering) m_reregister = true;
}
void BleLink::setIntervalMs(int ms)
{
    const int v = std::max(100, std::min(10000, ms));
    if (v == m_intervalMs) return;
    m_intervalMs = v;
    if (m_advertising) { unregisterAdvert(); registerAdvert(); }   // 200 ms while someone ranges, 1 s otherwise
    else if (m_registering) m_reregister = true;
}
void BleLink::setScanDuty(int onSeconds, int periodSeconds)
{
    m_scanPeriod = std::max(1, periodSeconds);
    m_scanOn = std::max(1, std::min(onSeconds, m_scanPeriod));
}

QByteArray BleLink::currentServiceData() const
{
    const qint64 now = nowMs() / 1000;
    const QByteArray tag = m_identity.isEmpty() ? QByteArray(8, '\0') : RangeMath::bleTag(m_identity, now);
    return RangeMath::bleServiceData(tag, m_tx, m_flags);
}

bool BleLink::start(const QString &adapterPath)
{
    if (m_started) return true;
    m_adapter = adapterPath;
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) { m_error = QStringLiteral("no system bus"); emit error(m_error); return false; }
    bus.connect(kBluez, QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("InterfacesAdded"),
                this, SLOT(onInterfacesAdded(QDBusObjectPath, QMap<QString, QVariantMap>)));
    bus.connect(kBluez, QString(), QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
                this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList, QDBusMessage)));
    // Devices BlueZ already knows
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(QDBusMessage::createMethodCall(kBluez, QStringLiteral("/"),
                                          QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"))), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *c) {
        QDBusPendingReply<ManagedObjects> r = *c;
        c->deleteLater();
        if (r.isError()) return;
        const ManagedObjects mo = r.value();
        for (auto it = mo.begin(); it != mo.end(); ++it)
            if (it.value().contains(QStringLiteral("org.bluez.Device1"))) consider(it.key().path(), it.value().value(QStringLiteral("org.bluez.Device1")), false);
    });
    if (!m_bluezWatch) {
        m_bluezWatch = new QDBusServiceWatcher(kBluez, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(m_bluezWatch, &QDBusServiceWatcher::serviceOwnerChanged, this, &BleLink::onBluezOwnerChanged);
    }
    m_started = true;
    m_retryDelayMs = kRetryMinMs;
    m_window = nowMs() / 1000 / 900;
    probeTxPower();                                             // registers the advert when BlueZ has answered
    setDiscovery(true);
    m_dutyPhase = 0;
    m_rotate.start(); m_duty.start(); m_hold.start();
    return true;
}

void BleLink::stop()
{
    if (!m_started) return;
    ++m_probeGen; ++m_regGen;
    m_rotate.stop(); m_duty.stop(); m_hold.stop(); m_retry.stop();
    setDiscovery(false);
    unregisterAdvert();
    if (m_adv) { QDBusConnection::systemBus().unregisterObject(kAdvPath); m_adv->deleteLater(); m_adv = nullptr; }
    m_started = false;
}

void BleLink::registerAdvert()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (m_adv && (m_adv->property("bfTx").toBool() != m_txSettable)) {   // TxPower appears / disappears: a new object
        bus.unregisterObject(kAdvPath);
        delete m_adv; m_adv = nullptr;
    }
    if (!m_adv) {
        m_adv = new BleAdvertisement(this);
        m_adv->setProperty("bfTx", m_txSettable);
        if (m_txSettable) new BleAdvertisementTxAdaptor(m_adv); else new BleAdvertisementAdaptor(m_adv);
        connect(m_adv, &BleAdvertisement::released, this, [this] {   // BlueZ dropped it (adapter reset, daemon going down): again later
            m_advertising = false; emit advertisingChanged(false); scheduleRetry(); });
        connect(m_adv, &BleAdvertisement::txSelected, this, &BleLink::onTxSelected, Qt::QueuedConnection);
        if (!bus.registerObject(kAdvPath, m_adv, QDBusConnection::ExportAdaptors)) {
            m_error = QStringLiteral("cannot export %1 on the system bus").arg(kAdvPath);
            emit error(m_error);
            delete m_adv; m_adv = nullptr;
            scheduleRetry();
            return;
        }
    }
    m_adv->serviceData = QVariantMap{{QString::fromLatin1(RangeMath::kBleServiceUuid), QVariant::fromValue(currentServiceData())}};
    m_adv->minInterval = quint32(m_intervalMs);
    m_adv->maxInterval = quint32(m_intervalMs);
    m_adv->txPower = qint16(m_txSettable ? m_tx : 0);
    // Legacy adverts hold 31 bytes: the 128-bit service data takes 28. Try with the TX-power AD (3 bytes)
    // first; if BlueZ says it does not fit (it may add Flags), register without it — TX power is in byte 8 anyway.
    m_registering = true;
    m_retry.stop();
    const int gen = ++m_regGen;
    auto attempt = [this, gen](bool withTx, auto &&self) -> void {
        m_adv->includes = withTx ? QStringList{QStringLiteral("tx-power")} : QStringList{};
        QDBusMessage call = QDBusMessage::createMethodCall(kBluez, m_adapter, QStringLiteral("org.bluez.LEAdvertisingManager1"),
                                                           QStringLiteral("RegisterAdvertisement"));
        call << QVariant::fromValue(QDBusObjectPath(kAdvPath)) << QVariantMap();
        auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 10000), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, withTx, self, gen](QDBusPendingCallWatcher *c) {
            QDBusPendingReply<> r = *c;
            c->deleteLater();
            if (gen != m_regGen) return;                         // stopped, or bluetoothd restarted meanwhile
            if (r.isError()) {
                const QString name = r.error().name(), msg = r.error().message();
                if (shapeError(name, msg)) {
                    if (withTx) { self(false, self); return; }   // the TX-power AD did not fit: without it (byte 8 still says it)
                    if (m_txSettable) {                          // this BlueZ refused the TxPower request: go without it
                        m_txSettable = false; m_tx = 127; m_txConfirmed = false;
                        emit txPowerChanged(m_tx);
                        registerAdvert();
                        return;
                    }
                }
                // Transient (NoReply: bluetoothd busy past our 10 s — it may still register it; AlreadyExists: it did;
                // Failed / NotPermitted): nothing about the advert's shape is wrong, so nothing is dropped. Clear our path
                // and try again; this used to give up for good and leave a stale advert on the air.
                m_registering = false; m_reregister = false;
                m_error = QStringLiteral("RegisterAdvertisement: %1").arg(msg);
                emit error(m_error);
                qWarning("beaconfix: BLE advert: %s (%s); retrying in %d s", qPrintable(msg), qPrintable(name), m_retryDelayMs / 1000);
                unregisterAdvert(true);
                scheduleRetry();
                return;
            }
            m_registering = false;
            m_advertising = true;
            m_error.clear();
            m_retryDelayMs = kRetryMinMs;
            emit advertisingChanged(true);
            if (m_reregister) { m_reregister = false; unregisterAdvert(); registerAdvert(); }   // byte 8 := the selected level
        });
    };
    attempt(true, attempt);
}

// Asynchronous, like probeTxPower(): this runs on the tray's GUI thread (every tag rotation, interval or flag change),
// and a busy bluetoothd must not freeze it. Messages on one connection are delivered in order and bluetoothd drops the
// advertisement before it answers, so a RegisterAdvertisement sent right after finds the path free.
void BleLink::unregisterAdvert(bool evenIfNotAdvertising)
{
    if (!m_advertising && !evenIfNotAdvertising) return;      // (evenIfNotAdvertising: after an error; DoesNotExist is fine)
    QDBusMessage call = QDBusMessage::createMethodCall(kBluez, m_adapter, QStringLiteral("org.bluez.LEAdvertisingManager1"),
                                                       QStringLiteral("UnregisterAdvertisement"));
    call << QVariant::fromValue(QDBusObjectPath(kAdvPath));
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, w, &QObject::deleteLater);
    if (!m_advertising) return;
    m_advertising = false;
    emit advertisingChanged(false);
}

void BleLink::setDiscovery(bool on)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (on) {
        QDBusMessage f = QDBusMessage::createMethodCall(kBluez, m_adapter, QStringLiteral("org.bluez.Adapter1"), QStringLiteral("SetDiscoveryFilter"));
        f << QVariantMap{{QStringLiteral("Transport"), QStringLiteral("le")}, {QStringLiteral("DuplicateData"), true}};
        bus.asyncCall(f);
        QDBusMessage s = QDBusMessage::createMethodCall(kBluez, m_adapter, QStringLiteral("org.bluez.Adapter1"), QStringLiteral("StartDiscovery"));
        auto *w = new QDBusPendingCallWatcher(bus.asyncCall(s), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *c) {
            QDBusPendingReply<> r = *c;
            c->deleteLater();
            if (r.isError() && !r.error().name().endsWith(QLatin1String("InProgress"))) { m_error = QStringLiteral("StartDiscovery: %1").arg(r.error().message()); emit error(m_error); return; }
            m_scanning = true;
        });
    } else if (m_scanning) {
        QDBusMessage s = QDBusMessage::createMethodCall(kBluez, m_adapter, QStringLiteral("org.bluez.Adapter1"), QStringLiteral("StopDiscovery"));
        bus.asyncCall(s);
        m_scanning = false;
    }
}

void BleLink::rotate()
{
    const qint64 w = nowMs() / 1000 / 900;
    if (w == m_window) return;
    m_window = w;
    if (m_advertising) { unregisterAdvert(); registerAdvert(); }   // a new 15-minute tag
    else if (m_registering) m_reregister = true;                  // (not advertising at all: the retry registers with the new tag)
}

// Discovery restarted by the watchdog: stop, then start again a second later (filter included)
void BleLink::restartDiscovery()
{
    setDiscovery(false);
    QTimer::singleShot(1000, this, [this] { if (m_started && !m_scanning) setDiscovery(true); });
}

void BleLink::dutyTick()
{
    // Watchdog: BlueZ said Discovering, but for a minute of scanning not one LE advert of any device arrived (the
    // controller or bluetoothd wedged: it heard nothing for ~50 min while we reported a healthy scan). Say so, and
    // restart discovery once a minute until something is heard again.
    if (m_scanning) {
        m_scanQuietMs += 1000;
        const qint64 now = nowMs();
        if (m_scanQuietMs >= kScanStallMs && now - m_lastScanRestartMs >= 60000) {
            if (!m_scanStalled) {
                m_scanStalled = true;
                qWarning("beaconfix: BLE scan hears nothing for %lld s of scanning: restarting discovery", m_scanQuietMs / 1000);
                emit scanStalledChanged(true);
            }
            m_lastScanRestartMs = now;
            restartDiscovery();
            return;
        }
    }
    if (m_scanOn >= m_scanPeriod) { if (!m_scanning) setDiscovery(true); return; }
    m_dutyPhase = (m_dutyPhase + 1) % m_scanPeriod;
    const bool want = m_dutyPhase < m_scanOn;
    if (want != m_scanning) setDiscovery(want);
}

// BlueZ only emits RSSI when it changes; a still link repeats values that never arrive as events.
// Re-emit the held value once a second while the device keeps advertising (last event < 10 s), marked `held`:
// it keeps a still link's level weighted by time, but it is not a new measurement, and the ranging service does
// not count it as one (feeding it as fresh samples made the BLE filter overconfident).
void BleLink::holdTick()
{
    const qint64 now = nowMs();
    for (auto it = m_devs.begin(); it != m_devs.end(); ++it) {
        Dev &d = it.value();
        if (!d.ours || d.rssi == 0 || now - d.lastEvent > 10000 || now - d.lastEvent < 1000) continue;
        const RangeMath::BleAdvert a = RangeMath::parseServiceData(d.serviceData);
        if (!a.valid) continue;
        emit sample(a.tag, d.rssi, d.txPower != 127 ? d.txPower : a.txPower, a.kind, int(quint8(d.serviceData.at(9))), now,
                    it.key().section(QLatin1Char('/'), -1).mid(4).replace(QLatin1Char('_'), QLatin1Char(':')), true);
    }
}

void BleLink::onInterfacesAdded(const QDBusObjectPath &path, const QMap<QString, QVariantMap> &ifaces)
{
    if (ifaces.contains(QStringLiteral("org.bluez.Device1"))) consider(path.path(), ifaces.value(QStringLiteral("org.bluez.Device1")), false);
}

void BleLink::onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &, const QDBusMessage &msg)
{
    if (iface != QLatin1String("org.bluez.Device1")) return;
    consider(msg.path(), changed, true);
}

QByteArray BleLink::serviceDataFor(const QVariant &prop)
{
    QVariantMap m;
    if (prop.canConvert<QDBusArgument>()) m = qdbus_cast<QVariantMap>(prop.value<QDBusArgument>());
    else m = prop.toMap();
    QVariant v;
    for (auto it = m.begin(); it != m.end(); ++it)
        if (it.key().compare(QLatin1String(RangeMath::kBleServiceUuid), Qt::CaseInsensitive) == 0) { v = it.value(); break; }
    if (!v.isValid()) return QByteArray();
    if (v.canConvert<QDBusVariant>()) v = v.value<QDBusVariant>().variant();
    if (v.canConvert<QDBusArgument>()) return qdbus_cast<QByteArray>(v.value<QDBusArgument>());
    return v.toByteArray();
}

void BleLink::consider(const QString &path, const QVariantMap &props, bool)
{
    if (!path.startsWith(m_adapter + QStringLiteral("/dev_"))) return;
    Dev &d = m_devs[path];
    if (props.contains(QStringLiteral("ServiceData"))) {
        const QByteArray sd = serviceDataFor(props.value(QStringLiteral("ServiceData")));
        if (sd.size() >= 10) { d.serviceData = sd; d.ours = true; }
    }
    if (props.contains(QStringLiteral("TxPower"))) d.txPower = props.value(QStringLiteral("TxPower")).toInt();
    if (!props.contains(QStringLiteral("RSSI"))) return;
    d.rssi = props.value(QStringLiteral("RSSI")).toInt();
    d.lastEvent = nowMs();
    ++m_advertsSeen;
    m_scanQuietMs = 0;                                          // the scanner hears (any device counts)
    if (m_scanStalled) { m_scanStalled = false; qInfo("beaconfix: BLE scan hears adverts again"); emit scanStalledChanged(false); }

    const QString addr = props.value(QStringLiteral("Address")).toString().isEmpty()
        ? path.section(QLatin1Char('/'), -1).mid(4).replace(QLatin1Char('_'), QLatin1Char(':'))
        : props.value(QStringLiteral("Address")).toString();
    const QString devName = props.value(QStringLiteral("Name")).toString();
    const QStringList devUuids = props.value(QStringLiteral("UUIDs")).toStringList();
    int mfrId = -1;
    QByteArray mfrData;
    if (props.contains(QStringLiteral("ManufacturerData"))) {
        const QVariant md = props.value(QStringLiteral("ManufacturerData"));
        if (md.canConvert<QVariantMap>()) {
            const QVariantMap m = md.toMap();
            if (!m.isEmpty()) {
                mfrId = m.firstKey().toInt();
                mfrData = m.first().toByteArray();
            }
        }
    }
    emit advertHeard(addr, devName, devUuids, mfrId, mfrData, d.rssi);

    if (!d.ours) return;
    const RangeMath::BleAdvert a = RangeMath::parseServiceData(d.serviceData);
    if (!a.valid) return;
    emit sample(a.tag, d.rssi, d.txPower != 127 ? d.txPower : a.txPower, a.kind, int(quint8(d.serviceData.at(9))), d.lastEvent,
                path.section(QLatin1Char('/'), -1).mid(4).replace(QLatin1Char('_'), QLatin1Char(':')));
}
