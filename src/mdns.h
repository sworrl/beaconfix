#pragma once
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <functional>

class QDBusInterface;
class QDBusServiceWatcher;
class QNetworkAccessManager;

// Zero-configuration discovery of BeaconFix instances on the LAN through Avahi's D-Bus API
// (no libavahi): we register `_beaconfix._tcp` on every real network interface and browse
// for the others, so a phone or laptop can list "BeaconFix devices on this network" and
// pair / link / sync with them by name.
//
// TXT record: v=<version> api=3 id=<identity id> name=<PC name> iname=<identity name> host=<hostname> port=<API port>
//             link=1 kind=desktop pair=<0|1> features=<a,b,c> addr=<ip,ip,…> tls=<0|1>   (link/api/port/name: docs/LINKING.md)
// `addr` carries the addresses of the real interfaces (docker0, virbr, veth, tun… skipped),
// so clients never depend on the .local A record — which, with Docker on the host, may
// resolve to the bridge. Registration is done per interface for the same reason.
//
// Without Avahi's D-Bus service we fall back to `avahi-publish-service` (publish only).
// On networks that block multicast the subnet scan probes /api/v1/hello on every host of
// the local /24s (on demand only, ≤ 254 hosts per subnet, 300 ms each, in parallel).
class Mdns : public QObject {
    Q_OBJECT
public:
    struct Peer {
        QString serviceName;              // "BeaconFix on <host>" (mDNS instance name)
        QString host;                     // hostname (from TXT host=, else the .local name)
        QStringList addresses;            // usable addresses, best first
        int port = 0;
        QString identityId, identityName, version, kind, iface;
        QStringList features;
        bool pairing = false, tls = false, self = false, local = false;   // local: this host; self: our very own record
        int api = 0;
        QDateTime lastSeen;
        QString source;                   // "mdns" | "scan"
        QString url() const;              // http(s)://<best address>:<port>
        QJsonObject toJson() const;
    };

    explicit Mdns(QObject *parent = nullptr);
    ~Mdns() override;

    bool available() const { return m_available; }       // Avahi reachable over D-Bus
    bool published() const { return m_published; }
    QString hostFqdn() const;                             // "<avahi host name>.local" (what .local resolves to), hostname.local without Avahi
    QString error() const { return m_error; }

    // Advertise (or re-advertise) our service. txt: "k=v" entries. port 0 = withdraw.
    void publish(int port, const QStringList &txt, const QString &instanceName = QString());
    void withdraw();

    QList<Peer> peers(bool includeSelf = true) const;
    QJsonArray  peersJson(bool includeSelf = true) const;
    const Peer *find(const QString &nameOrHost) const;    // by identity name, hostname, service name or address (case-insensitive)

    // Probe every host of the local /24 subnets for /api/v1/hello (port 47822 and the ports we
    // know from mDNS); calls done() when finished. Results merge into the peer list as "scan".
    void scanSubnets(int port, std::function<void()> done);
    bool scanning() const { return m_scanPending > 0; }

    // The interfaces we consider "real" (up, has an address, not docker/virbr/veth/tun/wg…).
    static QList<QPair<QString, int>> realInterfaces();   // (name, index)
    static QStringList lanAddresses();                    // their addresses, IPv4 first, no link-local v6

signals:
    void peersChanged();
    void stateChanged();

private slots:
    void onServerStateChanged(int state, const QString &error);
    void onGroupStateChanged(int state, const QString &error);
    void onItemNew(int iface, int proto, const QString &name, const QString &type, const QString &domain, uint flags);
    void onItemRemove(int iface, int proto, const QString &name, const QString &type, const QString &domain, uint flags);
    void onBrowserFailure(const QString &error);

private:
    void connectAvahi();
    void dropAvahi();
    void startBrowser();
    void registerGroup();
    void resolve(int iface, int proto, const QString &name, const QString &type, const QString &domain);
    void publishFallback();
    void mergeScanHello(const QString &address, int port, const QJsonObject &hello);
    static QString peerKey(int iface, int proto, const QString &name);

    QDBusInterface *m_server = nullptr;
    QDBusServiceWatcher *m_watcher = nullptr;
    QString m_groupPath, m_browserPath;
    bool m_available = false, m_published = false;
    QString m_error;
    int m_port = 0;
    QStringList m_txt;
    QString m_instance;
    int m_renames = 0;
    int m_ownPort = 0;                                     // "self" = our own records only; another instance on this host is a peer
    QHash<QString, Peer> m_peers;                          // key: iface/proto/name (mdns) or "scan:<address>"
    QTimer m_republish, m_sweep;
    QProcess *m_fallback = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    int m_scanPending = 0;
    std::function<void()> m_scanDone;
};
