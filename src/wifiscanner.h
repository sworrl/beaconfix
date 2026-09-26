#pragma once
#include <QObject>
#include <QList>
#include <QString>
#include <QTimer>
#include <QVariantMap>

struct AccessPoint {
    QString bssid;      // upper-case, colon separated
    QString ssid;
    int     strength = 0;   // NetworkManager percent
    int     dbm      = -100;
    int     frequency = 0;  // MHz
    bool    active   = false;   // the AP we're currently associated with
    // Security, from NetworkManager's AccessPoint Flags / WpaFlags / RsnFlags (NM80211ApSecurityFlags)
    int     secFlags = 0;       // NM_802_11_AP_FLAGS_PRIVACY = 0x1, WPS bits
    int     wpaFlags = 0;       // WPA1
    int     rsnFlags = 0;       // WPA2/3
    int     maxKbps  = 0;
    bool    adhoc    = false;   // Mode == NM_802_11_MODE_ADHOC
    QString security;           // open owe wep wpa1 wpa2-tkip wpa2 wpa2-eap wpa3 wpa2/3 wpa3-eap192
    static QString classify(int secFlags, int wpaFlags, int rsnFlags);
    static bool insecure(const QString &security) { return security == QLatin1String("open") || security == QLatin1String("wep") || security == QLatin1String("wpa1") || security == QLatin1String("wpa2-tkip"); }
};

// Scans through NetworkManager's D-Bus API. Asks for a fresh scan and waits for
// LastScan to change (NM refuses back-to-back scans; then the cached list is used).
class WifiScanner : public QObject {
    Q_OBJECT
public:
    explicit WifiScanner(QObject *parent = nullptr);
    bool    available() const { return !m_device.isEmpty(); }
    QString interfaceName() const { return m_iface; }
    void    scan();
    bool    busy() const { return m_busy; }

signals:
    void scanFinished(const QList<AccessPoint> &aps);
    void scanFailed(const QString &message);

private slots:
    void onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);
    void collect();

private:
    QString findWifiDevice();

    QString m_device;   // NM device object path
    QString m_iface;    // e.g. wlx0024a5219bbb
    QTimer  m_timeout;
    bool    m_busy = false;
};
