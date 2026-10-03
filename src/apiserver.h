#pragma once
#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QJsonArray>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <functional>
#include "hub.h"
#include "linking.h"

class Locator;
class Mdns;
class QTcpServer;
class QTcpSocket;
class QTemporaryFile;

// LAN API: a small HTTP/1.1 + JSON server on every interface so other devices
// on the local network (a photo frame, a phone, a script) can ask "where are we?".
//
//   GET  /api/v1/hello                 no auth · name, version, hostname, identity {id,name}, kind, mdns, pairing open?
//   GET  /api/v1/peers[?scan=1]        read    · BeaconFix devices on this network (mDNS; scan=1 probes the /24s too — control scope)
//   POST /api/v1/link                  no auth · linking v3 (docs/LINKING.md): QR {sid,name,kind,pub,mac} → approved at once;
//                                                mDNS {name,kind,commit[,proximity]} → 202 {sid, pub, status:"commit"}
//   POST /api/v1/link/<sid>            no auth · mDNS: {pub} (must match the commitment) → pending; the PC shows the code + Link / Reject
//   GET  /api/v1/link/<sid>            no auth · {status: pending|denied|approved[, sealed — once]}
//   POST /api/v1/link/hub-invite       control · a fresh hub invite for the calling device {"hub"}
//   POST /api/v1/pair, GET /pair/<id>  no auth · pairing v2 (old apps; only while pairing is open — no UI opens it any more)
//   GET  /api/v1/location              read    · the fix, place, elevation, sun, geo: URI, map links
//   GET  /api/v1/state                 read    · everything (Locator::StateJson)
//   GET  /api/v1/events?since=<id>     read    · beacon/fix/stop events newer than <id>
//   GET  /api/v1/aps | pois | track | trip     read
//   GET  /api/v1/pois[?cat=&group=&radius=]    read    · places (+ categories, note, origin, pedsOrigin; pediatric ERs merged in)
//   GET  /api/v1/emergency             read    · nearest police / fire / ER / pediatric ER (+ closer, urgent care, note) and the local number
//   GET  /api/v1/stream                read    · Server-Sent Events: fix, beacon, ping (30 s)
//   POST /api/v1/refresh | prefetch    control
//   GET  /api/v1/db/changes?since=&limit=   read · sync feed: AP positions (+fit), observations, fixes after a cursor
//   POST /api/v1/db/sync               control · a peer's {device, observations, aps, fixes} merged; refits queued; returns cursor
//   POST /api/v1/db/observations       control · samples from one device (256 KB); /db/sync allows 1 MB bodies
//
// Security: only private / link-local peers are answered at all; everything
// but hello, linking and pairing needs "Authorization: Bearer <token>". Tokens are
// 256-bit random and only their SHA-256 is stored (~/.config/sworrl/beaconfix-devices.json).
// A phone gets its token by linking (docs/LINKING.md: the QR's MAC, or a six-digit code
// compared on both screens and Link tapped here), sealed to the session key. Per-IP rate
// limit, connection cap, and an access log for the Devices tab. If ~/.config/sworrl/beaconfix.crt + .key
// exist the server speaks TLS instead of plain HTTP.
//
// Hub mode (beaconfix --server, Locator::hubRole()): the network listener (BEACONFIX_HUB_LISTEN, TLS from
// BEACONFIX_HUB_CERT / _KEY, full chain sent) serves only /api/v3/* (BFS3, docs/SECURE-API.md: each route is the
// /api/v1 handler for the device's identity and scopes, sealed both ways) and GET /healthz; /api/v1 is served only
// on the loopback admin listener (BEACONFIX_HUB_ADMIN) and only with the admin token from <state>/hub-admin.json.
class ApiServer : public QObject {
    Q_OBJECT
public:
    struct Device {
        QString id, name;
        QDateTime created, lastSeen;
        QString lastIp;
        QStringList scopes;               // "read" [, "control"]
        QByteArray hash;                  // SHA-256 of the token, hex
        bool revoked = false;
        QString identity;                 // set when the token was issued through identity auth (docs/IDENTITY.md)
        QString kind;                     // android | laptop | desktop | device (from the pairing request / identity sign-in)
        QJsonObject toJson(bool full = true) const;
    };
    struct Pending {
        enum State { Waiting, Approved, Denied, Cancelled };
        QString id, name, code, ip, kind;
        QStringList scopes;
        QDateTime created, expires;
        State state = Waiting;
        QString token;                    // approved: handed out once, then cleared
        // pairing v2 (pairing.h): picture match + proximity
        QString identityId, identityName; QByteArray identityPub;
        QByteArray sasPriv, sasPub, theirSasPub, sas;
        QList<int> icons;                 // the real triple
        QList<QList<int>> triples;        // shown on the desktop: real + decoys, shuffled
        int realIndex = -1;
        QJsonObject proximity, theirPosition;
        bool picked = false, wrongPick = false, lifted = false, knownDevice = false, autoApproved = false;
        QString deviceId;                 // approved: the Device the token went to (grantControl)
        QJsonObject toJson() const;
    };
    struct AccessEntry { QDateTime time; QString ip, method, path; int status = 0; };
    // One of OUR client devices (from the UniFi export, or added by hand). A peer is "known" when its
    // MAC (via the neighbour table) matches, its address matches ip / fixed_ip, or mac is a glob that matches.
    struct Known {
        QString mac, name, hostname, fixedIp, ip, network;
        bool online = false, wired = false, ours = true;
        QDateTime lastSeen;
        QStringList scopes;               // optional: what a pairing from it is auto-approved with
        QJsonObject toJson() const;
    };

    explicit ApiServer(Locator *loc, QObject *parent = nullptr);
    ~ApiServer() override;

    bool      enabled() const { return m_enabled; }
    void      setEnabled(bool on);
    int       port() const { return m_port; }              // configured
    int       boundPort() const;                            // actual (0 when not listening)
    void      setPort(int p);
    bool      listening() const;
    bool      tls() const { return m_tls; }
    QString   error() const { return m_error; }

    bool      pairingOpen() const;
    QDateTime pairingUntil() const { return m_pairingUntil; }
    void      openPairing(int minutes);
    void      closePairing();

    QList<Device>      devices() const { return m_devices; }
    QList<Pending>     pending() const;                     // unexpired
    QList<AccessEntry> accessLog() const { return m_log; }
    bool      approve(const QString &id);
    bool      deny(const QString &id);
    bool      approveByPick(const QString &id, int tripleIndex);   // picture match: right triple → approved, wrong → denied
    void      liftProximity(const QString &id);                     // "pair anyway": drop the proximity gate for this request
    bool      cancelPending(const QString &id);
    bool      grantControl(const QString &pendingId);              // approved request: add control to the token it produced
    bool      grantControlDevice(const QString &nameOrId);         // a paired device (id or name): add control to its existing token
    QJsonObject pendingDetail(const QString &id) const;             // everything the pairing dialog shows (no secrets)
    QString   pairPolicy() const { return m_pairPolicy; }           // required | warn | off  (apiPairProximity)
    void      setPairPolicy(const QString &p);
    bool      revoke(const QString &nameOrId);
    bool      remove(const QString &id);
    QString   createToken(const QString &name, const QStringList &scopes, const QString &identity = QString());   // returns the token (shown once)

    // Linking v3 (docs/LINKING.md): QR offers for the Link dialog; mDNS requests answered with Link / Reject
    QString     linkName() const;                           // this PC in the QR, the TXT record and the payload (the hostname)
    QStringList linkHosts() const;                          // LAN IPv4s (real interfaces) + "<avahi host>.local"
    QString     linkOffer();                                // a fresh QR session → sid; a hub invite is fetched into it in the background
    QString     linkQr(const QString &sid) const;           // its "bflink:" text ("" once used / gone)
    qint64      linkExpires(const QString &sid) const;      // unix s, 0 = gone
    void        linkCancel(const QString &sid);             // the dialog closed or renewed: that QR no longer links
    bool        linkApprove(const QString &sid);            // mDNS: the user tapped Link (codes match)
    bool        linkReject(const QString &sid);
    QList<Link::Session> linkSessions() const;              // the live ones (the UI shows name, kind, ip, code, proximity, state)
    QString   holdIdentityExport(const QString &bundle);   // LAN hand-off: keeps the bundle 10 min under a one-time 6-digit code
    QJsonObject statusJson() const;

    // Discovery (mdns.h): the other BeaconFix instances on this network, with our identity's view of them
    Mdns       *mdns() const { return m_mdns; }
    QJsonArray  peersJson(bool includeSelf = false) const;  // + sameIdentity / linked flags
    void        scanPeers(std::function<void()> done);      // probe the local /24s for /api/v1/hello, then done()

    // Known devices (allowlist)
    bool      knownOnly() const { return m_knownOnly; }
    void      setKnownOnly(bool on);
    QList<Known> known() const { return m_known; }
    QJsonObject knownJson() const;
    bool      knownAdd(const QString &mac, const QString &name);
    bool      knownRemove(const QString &mac);
    int       knownImport(const QString &path, QString *error = nullptr);   // merge by MAC; returns how many were new
    const Known *knownFor(const QString &ip);                              // nullptr when the peer is not ours
    static QString macForIp(const QString &ip);                            // from the kernel neighbour table ("" if unknown)

    Hub      *hub() const { return m_hub; }                  // hub mode only
    QString   adminFile() const;                             // hub mode: {port, token} for the local CLI (0600)

    static bool       isLanAddress(const QHostAddress &a);
    static QByteArray tokenHash(const QString &token);
    static bool       constantTimeEqual(const QByteArray &a, const QByteArray &b);

signals:
    void changed();                                          // devices / pending / listening state
    void peersChanged();                                     // mDNS / scan results changed
    void accessLogged();
    void pairingRequested(const QString &json);
    void deviceApproved(const QString &name);
    void openPairRequested(const QString &id);               // the notification's button / body was clicked
    void linkChanged();                                      // link sessions changed (offer renewed, request, approval)
    void linkRequested(const QString &sid);                  // an mDNS request waits for Link / Reject (open the dialog)
    void linked(const QString &sid, const QString &name, const QString &code);   // a phone fetched its sealed token

private:
    struct Request {
        QString method, path, query;
        QByteArray target;                                   // the request target exactly as on the wire (BFS3 AAD)
        QHash<QByteArray, QByteArray> headers;               // lower-case names
        QByteArray body;
    };
    struct Stream { QPointer<QTcpSocket> sock; QString device; bool sealed = false; Hub::Session sess; quint64 seq = 0; };
    struct Upload { Request request; QTemporaryFile *file = nullptr; qint64 left = 0; };   // POST /db/import body on its way to disk

    void restart();
    void onConnection(QTcpServer *srv);
    void onReadyRead(QTcpSocket *s);
    void handle(QTcpSocket *s, const Request &r);
    void serve(QTcpSocket *s, const Request &r, Device *dev, const QString &ep);   // an authenticated /api/v1 route
    void handleV3(QTcpSocket *s, const Request &r);                                // hub: enrol, or open → serve → seal
    bool startHub();                                                               // hub listeners (restart())
    void reply(QTcpSocket *s, int code, const QJsonObject &body, const QList<QByteArray> &extra = {});
    void replyRaw(QTcpSocket *s, int code, const QByteArray &type, const QByteArray &body, const QList<QByteArray> &extra = {});
    void logAccess(QTcpSocket *s, const QString &method, const QString &path, int status);
    bool    overLimit(const QString &ip);              // the budget is spent (does not count this request)
    bool rateLimited(const QString &ip, bool authFailure);
    QByteArray sseFrame(Stream &st, const QByteArray &event, const QJsonObject &data);
    Device *authenticate(const Request &r, const QString &ip);
    QJsonObject locationJson() const;
    QJsonObject homeJson() const;
    QJsonObject stateObject() const;
    void broadcast(const QByteArray &event, const QJsonObject &data);
    void load();
    void save();
    void loadKnown();
    void saveKnown();
    void updateDiscovery();
    bool issueLink(const QString &sid, QString *error);     // token (Devices) + known device + sealed payload → Approved
    void fetchHubInvite(const QString &name, const QString &kind, std::function<void(const QString &invite)> done);
    void rememberKnown(const QString &ip, const QString &name, const QString &deviceId);
    static bool mdnsAllowed();               // false for test instances (non-default XDG_CONFIG_HOME), --no-mdns, apiMdns=false
    static QString randomId(int bytes);
    static QString newToken();
    static QString clientIp(QTcpSocket *s);

    Locator *m_loc;
    QTcpServer *m_server = nullptr;
    bool m_enabled = true, m_tls = false;
    int m_port = 47822;
    QString m_error;
    QDateTime m_pairingUntil;
    QList<Device> m_devices;
    QList<Pending> m_pending;
    QList<AccessEntry> m_log;
    QList<Known> m_known;
    bool m_knownOnly = true;
    QString m_pairPolicy = QStringLiteral("required");
    QHash<QString, QPair<QString, qint64>> m_neigh;            // ip → (mac, ms looked up)
    QHash<QString, QList<qint64>> m_hits;                     // ip → request timestamps (ms) in the last minute
    QList<Stream> m_streams;
    QHash<QTcpSocket *, Upload> m_uploads;
    QHash<QString, QDateTime> m_nonces;                       // identity challenges: nonce (b64) → expiry
    struct ExportHold { QString bundle; QDateTime expires; int tries = 0; };
    QHash<QString, ExportHold> m_exports;                     // one-time codes → bundle
    Link::Book m_links;                                       // linking v3 sessions
    QTimer m_pingTimer, m_saveTimer, m_sweepTimer;
    bool m_dirty = false;
    Mdns *m_mdns = nullptr;
    int m_open = 0;
    // hub mode
    Hub *m_hub = nullptr;
    QTcpServer *m_admin = nullptr;
    QByteArray m_adminToken;
    QHash<QTcpSocket *, Hub::Session> m_v3;                   // sockets whose replies are sealed
    QHash<QString, Device> m_v3dev;                           // deviceId → the Device the v1 handlers see
    QList<QPair<QHostAddress, int>> m_allow;                  // BEACONFIX_HUB_ALLOW subnets (empty = any)
    QString m_certPath, m_keyPath;
    QDateTime m_certStamp;
    QTimer m_certTimer;                                       // a renewed certificate is picked up without a restart
};
