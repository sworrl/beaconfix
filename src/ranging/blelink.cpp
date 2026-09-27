// BeaconFix BLE link — see blelink.h and docs/RANGING.md §9.1.
#include "blelink.h"
#include "rangemath.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
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
}

BleLink::~BleLink() { stop(); }

void BleLink::setIdentity(const QString &identityId)
{
    if (identityId == m_identity) return;
    m_identity = identityId;
    if (m_advertising) { unregisterAdvert(); registerAdvert(); }
}
void BleLink::setFlags(bool rttResponder, bool apiReachable, int kind, bool calibrating)
{
    const int f = RangeMath::bleFlags(rttResponder, apiReachable, kind, calibrating);
    if (f == m_flags) return;
    m_flags = f;
    if (m_advertising) { unregisterAdvert(); registerAdvert(); }
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
    m_started = true;
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
    ++m_probeGen;
    m_rotate.stop(); m_duty.stop(); m_hold.stop();
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
        connect(m_adv, &BleAdvertisement::released, this, [this] { m_advertising = false; emit advertisingChanged(false); });
        connect(m_adv, &BleAdvertisement::txSelected, this, &BleLink::onTxSelected, Qt::QueuedConnection);
        if (!bus.registerObject(kAdvPath, m_adv, QDBusConnection::ExportAdaptors)) {
            m_error = QStringLiteral("cannot export %1 on the system bus").arg(kAdvPath);
            emit error(m_error);
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
    auto attempt = [this](bool withTx, auto &&self) -> void {
        m_adv->includes = withTx ? QStringList{QStringLiteral("tx-power")} : QStringList{};
        QDBusMessage call = QDBusMessage::createMethodCall(kBluez, m_adapter, QStringLiteral("org.bluez.LEAdvertisingManager1"),
                                                           QStringLiteral("RegisterAdvertisement"));
        call << QVariant::fromValue(QDBusObjectPath(kAdvPath)) << QVariantMap();
        auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 10000), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, withTx, self](QDBusPendingCallWatcher *c) {
            QDBusPendingReply<> r = *c;
            c->deleteLater();
            if (r.isError()) {
                if (withTx) { self(false, self); return; }
                if (m_txSettable) {                              // this BlueZ refused the TxPower request: go without it
                    m_txSettable = false; m_tx = 127; m_txConfirmed = false;
                    emit txPowerChanged(m_tx);
                    registerAdvert();
                    return;
                }
                m_registering = false; m_reregister = false;
                m_error = QStringLiteral("RegisterAdvertisement: %1").arg(r.error().message());
                emit error(m_error);
                return;
            }
            m_registering = false;
            m_advertising = true;
            m_error.clear();
            emit advertisingChanged(true);
            if (m_reregister) { m_reregister = false; unregisterAdvert(); registerAdvert(); }   // byte 8 := the selected level
        });
    };
    attempt(true, attempt);
}

void BleLink::unregisterAdvert()
{
    if (!m_advertising) return;
    QDBusMessage call = QDBusMessage::createMethodCall(kBluez, m_adapter, QStringLiteral("org.bluez.LEAdvertisingManager1"),
                                                       QStringLiteral("UnregisterAdvertisement"));
    call << QVariant::fromValue(QDBusObjectPath(kAdvPath));
    QDBusConnection::systemBus().call(call, QDBus::BlockWithGui, 3000);
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
}

void BleLink::dutyTick()
{
    if (m_scanOn >= m_scanPeriod) { if (!m_scanning) setDiscovery(true); return; }
    m_dutyPhase = (m_dutyPhase + 1) % m_scanPeriod;
    const bool want = m_dutyPhase < m_scanOn;
    if (want != m_scanning) setDiscovery(want);
}

// BlueZ only emits RSSI when it changes; a still link repeats values that never arrive as events.
// Re-emit the held value once a second while the device keeps advertising (last event < 10 s).
void BleLink::holdTick()
{
    const qint64 now = nowMs();
    for (auto it = m_devs.begin(); it != m_devs.end(); ++it) {
        Dev &d = it.value();
        if (!d.ours || d.rssi == 0 || now - d.lastEvent > 10000 || now - d.lastEvent < 1000) continue;
        const RangeMath::BleAdvert a = RangeMath::parseServiceData(d.serviceData);
        if (!a.valid) continue;
        emit sample(a.tag, d.rssi, d.txPower != 127 ? d.txPower : a.txPower, a.kind, int(quint8(d.serviceData.at(9))), now,
                    it.key().section(QLatin1Char('/'), -1).mid(4).replace(QLatin1Char('_'), QLatin1Char(':')));
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
    if (!d.ours) return;
    const RangeMath::BleAdvert a = RangeMath::parseServiceData(d.serviceData);
    if (!a.valid) return;
    emit sample(a.tag, d.rssi, d.txPower != 127 ? d.txPower : a.txPower, a.kind, int(quint8(d.serviceData.at(9))), d.lastEvent,
                path.section(QLatin1Char('/'), -1).mid(4).replace(QLatin1Char('_'), QLatin1Char(':')));
}
