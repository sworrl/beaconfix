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
