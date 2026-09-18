#include "wifiscanner.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>

static const char *NM_SVC   = "org.freedesktop.NetworkManager";
static const char *NM_PATH  = "/org/freedesktop/NetworkManager";
static const char *NM_IFACE = "org.freedesktop.NetworkManager";
static const char *NM_DEV   = "org.freedesktop.NetworkManager.Device";
static const char *NM_WIFI  = "org.freedesktop.NetworkManager.Device.Wireless";
static const char *NM_AP    = "org.freedesktop.NetworkManager.AccessPoint";
static const char *DBUS_PROPS = "org.freedesktop.DBus.Properties";

WifiScanner::WifiScanner(QObject *parent) : QObject(parent)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, &WifiScanner::collect);
    findWifiDevice();
}

QString WifiScanner::findWifiDevice()
{
    QDBusInterface nm(NM_SVC, NM_PATH, NM_IFACE, QDBusConnection::systemBus());
    QDBusReply<QList<QDBusObjectPath>> reply = nm.call("GetDevices");
    if (!reply.isValid())
        return {};
    for (const QDBusObjectPath &p : reply.value()) {
        QDBusInterface dev(NM_SVC, p.path(), NM_DEV, QDBusConnection::systemBus());
        if (dev.property("DeviceType").toUInt() != 2)   // NM_DEVICE_TYPE_WIFI
            continue;
        m_device = p.path();
        m_iface  = dev.property("Interface").toString();
        QDBusConnection::systemBus().connect(NM_SVC, m_device, DBUS_PROPS, "PropertiesChanged",
                                             this, SLOT(onPropertiesChanged(QString,QVariantMap,QStringList)));
        return m_device;
    }
    return {};
}

void WifiScanner::scan()
{
    if (m_device.isEmpty())
        findWifiDevice();
    if (m_device.isEmpty()) {
        emit scanFailed(QStringLiteral("No Wi-Fi device is managed by NetworkManager"));
        return;
    }
    if (m_busy)
        return;
    m_busy = true;

    QDBusInterface wifi(NM_SVC, m_device, NM_WIFI, QDBusConnection::systemBus());
    QDBusMessage reply = wifi.call(QStringLiteral("RequestScan"), QVariantMap());
    // An error here is normally "scan not allowed immediately following previous
    // scan" — the cached list is then only seconds old, so just read it.
    m_timeout.start(reply.type() == QDBusMessage::ErrorMessage ? 500 : 9000);
}

void WifiScanner::onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &)
{
    if (!m_busy || iface != QLatin1String(NM_WIFI))
        return;
    if (changed.contains(QStringLiteral("LastScan"))) {
        m_timeout.stop();
        QTimer::singleShot(300, this, &WifiScanner::collect);
    }
}

void WifiScanner::collect()
{
    m_timeout.stop();
    if (!m_busy)
        return;
    m_busy = false;

    QDBusInterface wifi(NM_SVC, m_device, NM_WIFI, QDBusConnection::systemBus());
    const QString activePath = qvariant_cast<QDBusObjectPath>(wifi.property("ActiveAccessPoint")).path();
    QDBusReply<QList<QDBusObjectPath>> reply = wifi.call(QStringLiteral("GetAllAccessPoints"));
    if (!reply.isValid()) {
        emit scanFailed(reply.error().message());
        return;
    }

    QList<AccessPoint> aps;
    for (const QDBusObjectPath &p : reply.value()) {
        QDBusInterface ap(NM_SVC, p.path(), NM_AP, QDBusConnection::systemBus());
        if (!ap.isValid())
            continue;
        AccessPoint a;
        a.bssid     = ap.property("HwAddress").toString().toUpper();
        a.ssid      = QString::fromUtf8(ap.property("Ssid").toByteArray());
        a.strength  = int(ap.property("Strength").toUInt());
        a.frequency = int(ap.property("Frequency").toUInt());
        // Inverse of NetworkManager's dBm→percent mapping (-100 dBm = 0 %, -40 dBm = 100 %)
        a.dbm       = -40 - (100 - qBound(0, a.strength, 100)) * 60 / 100;
        a.active    = (p.path() == activePath);
        if (!a.bssid.isEmpty())
            aps.append(a);
    }
    std::sort(aps.begin(), aps.end(), [](const AccessPoint &x, const AccessPoint &y) { return x.dbm > y.dbm; });
    emit scanFinished(aps);
}
