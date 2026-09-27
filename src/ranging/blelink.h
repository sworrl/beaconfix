// BeaconFix BLE link: advertise our ranging beacon and hear everyone else's (docs/RANGING.md §9.1).
// QtDBus + BlueZ only (no libbluetooth). The advertisement carries the 128-bit BeaconFix service UUID
// with 10 bytes of service data (rotating identity tag, TX power, flags); scanning reports the RSSI of
// every advert that carries the same UUID, so the ranging service can measure the phone → desktop link.
#pragma once
#include <QByteArray>
#include <QDBusAbstractAdaptor>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class BleAdvertisement;

class BleLink : public QObject {
    Q_OBJECT
public:
    explicit BleLink(QObject *parent = nullptr);
    ~BleLink() override;

    // Configure before start() (changes re-register the advertisement).
    void setIdentity(const QString &identityId);           // canonical 26-char id; "" = no tag (all zero)
    void setFlags(bool rttResponder, bool apiReachable, int kind, bool calibrating);
    // Requested TX power (dBm, 127 = no preference). When BlueZ offers CanSetTxPower it is clamped to the
    // controller's [MinTxPower, MaxTxPower], requested through LEAdvertisement1.TxPower and put into byte 8;
    // otherwise the controller picks its own level and byte 8 says 127 (unknown). See txPower().
    void setTxPower(int dbm);
    void setIntervalMs(int ms);                             // 100–10000; 200 during a ranging session
    void setScanDuty(int onSeconds, int periodSeconds);     // onSeconds == periodSeconds → continuous

    bool start(const QString &adapterPath = QStringLiteral("/org/bluez/hci0"));
    void stop();

    bool advertising() const { return m_advertising; }
    bool scanning() const { return m_scanning; }
    QString lastError() const { return m_error; }
    int advertsSeen() const { return m_advertsSeen; }
    int intervalMs() const { return m_intervalMs; }       // any LE advert event (proves scanning works)
    int txPower() const { return m_tx; }                  // what byte 8 carries: the controller's selected level, the request until BlueZ reports it, or 127
    bool txPowerConfirmed() const { return m_txConfirmed; } // BlueZ reported the controller's selected level (see onTxSelected)
    QByteArray currentServiceData() const;

signals:
    // An advert with the BeaconFix UUID: its tag (8 bytes), RSSI (dBm), TX power (dBm or 127), kind, flags byte.
    // held: not a report from BlueZ but holdTick's repeat of the last RSSI (no new measurement; see holdTick).
    void sample(const QByteArray &tag, int rssi, int txPower, int kind, int flags, qint64 timeMs, const QString &address, bool held = false);
    void advertisingChanged(bool on);
    void txPowerChanged(int dbm);
    void error(const QString &message);

private slots:
    void onInterfacesAdded(const QDBusObjectPath &path, const QMap<QString, QVariantMap> &ifaces);
    void onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated, const QDBusMessage &msg);
    void rotate();
    void dutyTick();
    void holdTick();

private:
    struct Dev { QByteArray serviceData; int rssi = 0; int txPower = 127; qint64 lastEvent = 0; bool ours = false; };
    void registerAdvert();
    void probeTxPower();                                    // LEAdvertisingManager1 SupportedFeatures / SupportedCapabilities (async)
    void applyTxCaps(bool settable, int lo, int hi);
    void onTxSelected(int dbm);
    void unregisterAdvert();
    void setDiscovery(bool on);
    void consider(const QString &path, const QVariantMap &props, bool fromChange);
    static QByteArray serviceDataFor(const QVariant &serviceDataProp);

    QString m_adapter, m_identity, m_error;
    int m_tx = 127, m_txWant = 127, m_flags = 0, m_intervalMs = 1000, m_scanOn = 10, m_scanPeriod = 30;
    bool m_started = false, m_advertising = false, m_scanning = false, m_txSettable = false, m_txConfirmed = false, m_registering = false;
    bool m_reregister = false;
    int m_probeGen = 0, m_txAdoptions = 0;
    int m_advertsSeen = 0;
    qint64 m_window = -1;
    BleAdvertisement *m_adv = nullptr;
    QTimer m_rotate, m_duty, m_hold;
    int m_dutyPhase = 0;
    QHash<QString, Dev> m_devs;
};

// The org.bluez.LEAdvertisement1 object BlueZ reads our payload from.
class BleAdvertisement : public QObject {
    Q_OBJECT
public:
    explicit BleAdvertisement(QObject *parent = nullptr) : QObject(parent) {}
    QVariantMap serviceData;
    QStringList includes;
    quint32 minInterval = 200, maxInterval = 200;
    qint16 txPower = 0;
    QString type = QStringLiteral("broadcast");
signals:
    void released();
    void txSelected(int dbm);                               // BlueZ wrote the controller's selected TX power into TxPower
};

class BleAdvertisementAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.LEAdvertisement1")
    Q_PROPERTY(QString Type READ type)
    Q_PROPERTY(QVariantMap ServiceData READ serviceData)
    Q_PROPERTY(QStringList Includes READ includes)
    Q_PROPERTY(uint MinInterval READ minInterval)
    Q_PROPERTY(uint MaxInterval READ maxInterval)
public:
    explicit BleAdvertisementAdaptor(BleAdvertisement *a) : QDBusAbstractAdaptor(a), m_a(a) {}
    QString type() const { return m_a->type; }
    QVariantMap serviceData() const { return m_a->serviceData; }
    QStringList includes() const { return m_a->includes; }
    uint minInterval() const { return m_a->minInterval; }
    uint maxInterval() const { return m_a->maxInterval; }
public slots:
    Q_NOREPLY void Release() { emit m_a->released(); }
protected:
    BleAdvertisement *m_a;
};

// The same object with a requested TX power (int16 dBm; BlueZ uses it when the adapter has CanSetTxPower).
// A separate class because QtDBus exports every Q_PROPERTY of an adaptor: TxPower must be absent, not 127,
// when we have no preference (BlueZ rejects values outside −127…+20). Writable: once the controller has
// accepted the parameters, BlueZ (src/advertising.c add_adv_params_callback) sets TxPower to the level the
// controller selected; a read-only property made that write fail and the real level went unseen.
class BleAdvertisementTxAdaptor : public BleAdvertisementAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.LEAdvertisement1")
    Q_PROPERTY(qint16 TxPower READ txPower WRITE setTxPower)
public:
    explicit BleAdvertisementTxAdaptor(BleAdvertisement *a) : BleAdvertisementAdaptor(a) {}
    qint16 txPower() const { return m_a->txPower; }
    void setTxPower(qint16 dbm) { emit m_a->txSelected(int(dbm)); }
};
