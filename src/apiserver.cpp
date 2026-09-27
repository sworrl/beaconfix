#include "apiserver.h"
#include "locator.h"
#include "identity.h"
#include "mapdb.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <openssl/rand.h>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

static const int   PAIR_MINUTES   = 10;
static const int   MAX_PENDING    = 5;
static const int   MAX_STREAMS    = 8;
static const int   MAX_OPEN       = 32;
static const int   MAX_BODY       = 4096;
static const int   MAX_BODY_SYNC  = 1024 * 1024;         // /db/sync: a phone reconnecting after a day pushes thousands of samples
static const int   MAX_BODY_OBS   = 256 * 1024;          // /db/observations
static int bodyLimitFor(const QString &path)
{
    if (path == QLatin1String("/api/v1/db/sync")) return MAX_BODY_SYNC;
    if (path == QLatin1String("/api/v1/db/observations")) return MAX_BODY_OBS;
    return MAX_BODY;
}
static const int   RATE_PER_MIN   = 60;
static const int   LOG_KEEP       = 100;

// ── Helpers ───────────────────────────────────────────────────────────────────
static QString devicesFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/sworrl/beaconfix-devices.json");
}
static QString knownFile()   { return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/sworrl/beaconfix-known.json"); }
static QString knownSeed()   { return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/sworrl/known-devices.json"); }
static QString normMac(const QString &m) { return m.trimmed().toLower().replace(QLatin1Char('-'), QLatin1Char(':')); }

QString ApiServer::randomId(int bytes)
{
    QByteArray b(bytes, 0);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(b.data()), bytes / 4);
    for (int i = (bytes / 4) * 4; i < bytes; ++i) b[i] = char(QRandomGenerator::system()->bounded(256));
    return QString::fromLatin1(b.toHex());
}

QString ApiServer::newToken()
{
    QByteArray b(32, 0);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(b.data()), 8);
    return QString::fromLatin1(b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray ApiServer::tokenHash(const QString &token)
{
    return QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex();
}

bool ApiServer::constantTimeEqual(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size()) return false;
    volatile unsigned char acc = 0;
    for (int i = 0; i < a.size(); ++i) acc |= static_cast<unsigned char>(a[i] ^ b[i]);
    return acc == 0;
}

bool ApiServer::isLanAddress(const QHostAddress &addr)
{
    bool v4ok = false;
    const quint32 v4 = addr.toIPv4Address(&v4ok);              // also unwraps ::ffff:a.b.c.d
    if (v4ok) {
        if ((v4 & 0xff000000u) == 0x0a000000u) return true;    // 10/8
        if ((v4 & 0xfff00000u) == 0xac100000u) return true;    // 172.16/12
        if ((v4 & 0xffff0000u) == 0xc0a80000u) return true;    // 192.168/16
        if ((v4 & 0xff000000u) == 0x7f000000u) return true;    // 127/8
        if ((v4 & 0xffff0000u) == 0xa9fe0000u) return true;    // 169.254/16 link-local
        return false;
    }
    if (addr.protocol() != QAbstractSocket::IPv6Protocol) return false;
    if (addr == QHostAddress::LocalHostIPv6) return true;
    const Q_IPV6ADDR v6 = addr.toIPv6Address();
    if (v6[0] == 0xfe && (v6[1] & 0xc0) == 0x80) return true;   // fe80::/10
    if ((v6[0] & 0xfe) == 0xfc) return true;                    // fc00::/7
    return false;
}

QString ApiServer::clientIp(QTcpSocket *s)
{
    const QHostAddress a = s->peerAddress();
    bool ok = false;
    const quint32 v4 = a.toIPv4Address(&ok);
    return ok ? QHostAddress(v4).toString() : a.toString();
}

QJsonObject ApiServer::Device::toJson(bool full) const
{
    // (identity is added below when set)
    QJsonObject o{{"id", id}, {"name", name}, {"created", created.toString(Qt::ISODate)},
                  {"lastSeen", lastSeen.isValid() ? QJsonValue(lastSeen.toString(Qt::ISODate)) : QJsonValue()},
                  {"lastIp", lastIp}, {"scopes", QJsonArray::fromStringList(scopes)}, {"revoked", revoked}};
    if (full) o["hash"] = QString::fromLatin1(hash);
    if (!identity.isEmpty()) o["identity"] = identity;
    return o;
}

QJsonObject ApiServer::Known::toJson() const
{
    return {{"mac", mac}, {"name", name}, {"hostname", hostname}, {"fixed_ip", fixedIp.isEmpty() ? QJsonValue() : QJsonValue(fixedIp)},
            {"ip", ip.isEmpty() ? QJsonValue() : QJsonValue(ip)}, {"network", network}, {"online", online}, {"wired", wired}, {"ours", ours},
            {"last_seen", lastSeen.isValid() ? QJsonValue(double(lastSeen.toSecsSinceEpoch())) : QJsonValue()},
            {"scopes", QJsonArray::fromStringList(scopes)}};
}

QJsonObject ApiServer::Pending::toJson() const
{
    return {{"id", id}, {"name", name}, {"code", code}, {"ip", ip}, {"scopes", QJsonArray::fromStringList(scopes)},
            {"created", created.toString(Qt::ISODate)}, {"expires", expires.toString(Qt::ISODate)},
            {"status", state == Waiting ? "pending" : state == Approved ? "approved" : "denied"}};
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
ApiServer::ApiServer(Locator *loc, QObject *parent) : QObject(parent), m_loc(loc)
{
    QSettings s;
    m_enabled = s.value("apiEnabled", true).toBool();
    m_port = qBound(1024, s.value("apiPort", 47822).toInt(), 65535);
    m_pairingUntil = s.value("apiPairingUntil").toDateTime();
    m_knownOnly = s.value("apiKnownOnly", true).toBool();
    load();
    loadKnown();

    m_pingTimer.setInterval(30000);
    connect(&m_pingTimer, &QTimer::timeout, this, [this] {
        broadcast("ping", QJsonObject{{"ts", QDateTime::currentDateTime().toString(Qt::ISODate)}});
    });
    m_saveTimer.setSingleShot(true); m_saveTimer.setInterval(5000);
    connect(&m_saveTimer, &QTimer::timeout, this, &ApiServer::save);
    m_sweepTimer.setInterval(30000);
    connect(&m_sweepTimer, &QTimer::timeout, this, [this] {
        const QDateTime now = QDateTime::currentDateTime();
        bool changedAny = false;
        for (int i = m_pending.size() - 1; i >= 0; --i)
            if (m_pending[i].expires < now) { m_pending.removeAt(i); changedAny = true; }
        static bool wasOpen = false;
        if (wasOpen != pairingOpen()) { wasOpen = pairingOpen(); changedAny = true; updateDiscovery(); }
        for (auto it = m_hits.begin(); it != m_hits.end();) {
            QList<qint64> &l = it.value();
            const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - 60000;
            while (!l.isEmpty() && l.first() < cutoff) l.removeFirst();
            if (l.isEmpty()) it = m_hits.erase(it); else ++it;
        }
        if (changedAny) emit changed();
    });
    m_sweepTimer.start();

    connect(m_loc, &Locator::FixChanged, this, [this] { if (!m_streams.isEmpty()) broadcast("fix", locationJson()); });
    connect(m_loc, &Locator::eventLogged, this, [this](const QString &json) {
        if (!m_streams.isEmpty()) broadcast("beacon", QJsonDocument::fromJson(json.toUtf8()).object());
    });
    restart();
}

ApiServer::~ApiServer()
{
    if (m_dirty) save();
    if (m_avahi) { m_avahi->kill(); m_avahi->waitForFinished(500); }
}

bool ApiServer::listening() const { return m_server && m_server->isListening(); }
int  ApiServer::boundPort() const { return listening() ? m_server->serverPort() : 0; }

void ApiServer::setEnabled(bool on)
{
    if (m_enabled == on) return;
    m_enabled = on;
    QSettings().setValue("apiEnabled", on);
    restart();
}

void ApiServer::setPort(int p)
{
    p = qBound(1024, p, 65535);
    if (m_port == p) return;
    m_port = p;
    QSettings().setValue("apiPort", p);
    restart();
}

void ApiServer::restart()
{
    for (const Stream &st : m_streams) if (st.sock) st.sock->disconnectFromHost();
    m_streams.clear();
    m_pingTimer.stop();
    if (m_server) { m_server->close(); m_server->deleteLater(); m_server = nullptr; }
    m_error.clear();
    m_tls = false;
    if (m_enabled) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/sworrl/");
        QFile crt(dir + QStringLiteral("beaconfix.crt")), key(dir + QStringLiteral("beaconfix.key"));
        if (crt.exists() && key.exists() && crt.open(QIODevice::ReadOnly) && key.open(QIODevice::ReadOnly)) {
            const QSslCertificate cert(&crt, QSsl::Pem);
            const QSslKey pkey(&key, QSsl::Rsa, QSsl::Pem);
            const QSslKey ekey = pkey.isNull() ? QSslKey(&key, QSsl::Ec, QSsl::Pem) : pkey;
            if (!cert.isNull() && !ekey.isNull()) {
                auto *ssl = new QSslServer(this);
                QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
                cfg.setLocalCertificate(cert); cfg.setPrivateKey(ekey);
                ssl->setSslConfiguration(cfg);
                m_server = ssl; m_tls = true;
            } else m_error = QStringLiteral("beaconfix.crt/.key unreadable — serving plain HTTP");
        }
        if (!m_server) m_server = new QTcpServer(this);
        m_server->setMaxPendingConnections(16);
        connect(m_server, &QTcpServer::pendingConnectionAvailable, this, &ApiServer::onConnection);
        bool ok = false;
        for (int p = m_port; p < m_port + 10 && !ok; ++p) ok = m_server->listen(QHostAddress::Any, quint16(p));
        if (!ok) { m_error = QStringLiteral("cannot listen on port %1: %2").arg(m_port).arg(m_server->errorString()); m_server->deleteLater(); m_server = nullptr; }
        else m_pingTimer.start();
    }
    updateDiscovery();
    emit changed();
}

// ── Pairing / devices ─────────────────────────────────────────────────────────
bool ApiServer::pairingOpen() const { return m_pairingUntil.isValid() && m_pairingUntil > QDateTime::currentDateTime(); }

void ApiServer::openPairing(int minutes)
{
    m_pairingUntil = QDateTime::currentDateTime().addSecs(qBound(1, minutes, 120) * 60);
    QSettings().setValue("apiPairingUntil", m_pairingUntil);
    updateDiscovery();
    emit changed();
}

void ApiServer::closePairing()
{
    m_pairingUntil = QDateTime();
    QSettings().remove("apiPairingUntil");
    updateDiscovery();
    emit changed();
}

QList<ApiServer::Pending> ApiServer::pending() const
{
    QList<Pending> out;
    const QDateTime now = QDateTime::currentDateTime();
    for (const Pending &p : m_pending) if (p.expires > now) out << p;
    return out;
}

bool ApiServer::approve(const QString &id)
{
    const QDateTime now = QDateTime::currentDateTime();
    for (Pending &p : m_pending) {
        if (p.id != id || p.state != Pending::Waiting || p.expires < now) continue;
        Device d; d.id = randomId(6); d.name = p.name; d.created = now; d.scopes = p.scopes;
        p.token = newToken(); d.hash = tokenHash(p.token);
        m_devices << d;
        p.state = Pending::Approved; p.expires = now.addSecs(PAIR_MINUTES * 60);
        save();
        emit deviceApproved(d.name);
        emit changed();
        return true;
    }
    return false;
}

bool ApiServer::deny(const QString &id)
{
    for (Pending &p : m_pending) {
        if (p.id != id || p.state != Pending::Waiting) continue;
        p.state = Pending::Denied; p.expires = QDateTime::currentDateTime().addSecs(120);
        emit changed();
        return true;
    }
    return false;
}

bool ApiServer::revoke(const QString &nameOrId)
{
    bool any = false;
    for (Device &d : m_devices) {
        if (d.revoked || (d.id != nameOrId && d.name.compare(nameOrId, Qt::CaseInsensitive) != 0)) continue;
        d.revoked = true; any = true;
        for (int i = m_streams.size() - 1; i >= 0; --i)
            if (m_streams[i].device == d.id) { if (m_streams[i].sock) m_streams[i].sock->disconnectFromHost(); m_streams.removeAt(i); }
    }
    if (any) { save(); emit changed(); }
    return any;
}

bool ApiServer::remove(const QString &id)
{
    for (int i = 0; i < m_devices.size(); ++i)
        if (m_devices[i].id == id) { m_devices.removeAt(i); save(); emit changed(); return true; }
    return false;
}

QString ApiServer::createToken(const QString &name, const QStringList &scopes, const QString &identity)
{
    Device d; d.id = randomId(6); d.name = name.trimmed().isEmpty() ? QStringLiteral("Device") : name.trimmed().left(64);
    d.identity = identity;
    d.created = QDateTime::currentDateTime();
    d.scopes = QStringList{QStringLiteral("read")};
    if (scopes.contains(QStringLiteral("control"))) d.scopes << QStringLiteral("control");
    const QString token = newToken();
    d.hash = tokenHash(token);
    m_devices << d;
    save();
    emit changed();
    return token;
}

QJsonObject ApiServer::statusJson() const
{
    QJsonArray devs, pend, addrs;
    for (const Device &d : m_devices) devs.append(d.toJson(false));
    for (const Pending &p : pending()) pend.append(p.toJson());
    for (const QHostAddress &a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && isLanAddress(a)) addrs.append(a.toString());
    return {{"enabled", m_enabled}, {"listening", listening()}, {"port", boundPort()}, {"configuredPort", m_port}, {"tls", m_tls},
            {"error", m_error}, {"pairingOpen", pairingOpen()},
            {"pairingUntil", pairingOpen() ? QJsonValue(m_pairingUntil.toString(Qt::ISODate)) : QJsonValue()},
            {"hostname", QHostInfo::localHostName()}, {"addresses", addrs}, {"devices", devs}, {"pending", pend},
            {"streams", m_streams.size()}, {"knownOnly", m_knownOnly}, {"knownCount", m_known.size()}};
}

void ApiServer::load()
{
    QFile f(devicesFile());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).object()["devices"].toArray();
    m_devices.clear();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Device d; d.id = o["id"].toString(); d.name = o["name"].toString(); d.identity = o["identity"].toString();
        d.created = QDateTime::fromString(o["created"].toString(), Qt::ISODate);
        d.lastSeen = QDateTime::fromString(o["lastSeen"].toString(), Qt::ISODate);
        d.lastIp = o["lastIp"].toString(); d.hash = o["hash"].toString().toLatin1(); d.revoked = o["revoked"].toBool();
        for (const QJsonValue &sv : o["scopes"].toArray()) d.scopes << sv.toString();
        if (!d.scopes.contains(QStringLiteral("read"))) d.scopes.prepend(QStringLiteral("read"));
        if (!d.id.isEmpty() && d.hash.size() == 64) m_devices << d;
    }
}

void ApiServer::save()
{
    m_dirty = false;
    QDir().mkpath(QFileInfo(devicesFile()).path());
    QJsonArray arr;
    for (const Device &d : m_devices) arr.append(d.toJson(true));
    QSaveFile f(devicesFile());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(QJsonObject{{"devices", arr}}).toJson(QJsonDocument::Indented));
    f.commit();
}

// ── Known devices ─────────────────────────────────────────────────────────────
static ApiServer::Known knownFromJson(const QJsonObject &o)
{
    ApiServer::Known k;
    k.mac = normMac(o["mac"].toString()); k.name = o["name"].toString(); k.hostname = o["hostname"].toString();
    k.fixedIp = o["fixed_ip"].toString(); k.ip = o["ip"].toString(); k.network = o["network"].toString();
    k.online = o["online"].toBool(); k.wired = o["wired"].toBool(); k.ours = o.contains("ours") ? o["ours"].toBool() : true;
    if (o["last_seen"].isDouble()) k.lastSeen = QDateTime::fromSecsSinceEpoch(qint64(o["last_seen"].toDouble()));
    else if (o["last_seen"].isString()) k.lastSeen = QDateTime::fromString(o["last_seen"].toString(), Qt::ISODate);
    for (const QJsonValue &v : o["scopes"].toArray()) if (v.toString() == QLatin1String("read") || v.toString() == QLatin1String("control")) k.scopes << v.toString();
    return k;
}

void ApiServer::loadKnown()
{
    m_known.clear();
    QFile f(knownFile());
    bool seeded = false;
    if (!f.exists()) { f.setFileName(knownSeed()); seeded = true; }          // first run: the UniFi export
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object()["devices"].toArray()) {
        const Known k = knownFromJson(v.toObject());
        if (!k.mac.isEmpty()) m_known << k;
    }
    if (seeded && !m_known.isEmpty()) { qInfo("beaconfix: known devices seeded from %s (%d devices)", qPrintable(knownSeed()), int(m_known.size())); saveKnown(); }
}

void ApiServer::saveKnown()
{
    QDir().mkpath(QFileInfo(knownFile()).path());
    QJsonArray arr; for (const Known &k : m_known) arr.append(k.toJson());
    QSaveFile f(knownFile());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(QJsonObject{{"source", "beaconfix"}, {"exported", QDateTime::currentDateTime().toString(Qt::ISODate)}, {"devices", arr}}).toJson(QJsonDocument::Indented));
    f.commit();
}

void ApiServer::setKnownOnly(bool on) { if (m_knownOnly == on) return; m_knownOnly = on; QSettings().setValue("apiKnownOnly", on); emit changed(); }

QJsonObject ApiServer::knownJson() const
{
    QJsonArray arr; for (const Known &k : m_known) arr.append(k.toJson());
    return {{"knownOnly", m_knownOnly}, {"count", m_known.size()}, {"file", knownFile()}, {"devices", arr}};
}

bool ApiServer::knownAdd(const QString &mac, const QString &name)
{
    const QString m = normMac(mac);
    if (m.isEmpty()) return false;
    for (Known &k : m_known) if (k.mac == m) { if (!name.trimmed().isEmpty()) k.name = name.trimmed(); k.ours = true; saveKnown(); emit changed(); return true; }
    Known k; k.mac = m; k.name = name.trimmed().isEmpty() ? m : name.trimmed(); k.ours = true;
    m_known << k; saveKnown(); emit changed();
    return true;
}

bool ApiServer::knownRemove(const QString &mac)
{
    const QString m = normMac(mac);
    for (int i = 0; i < m_known.size(); ++i) if (m_known[i].mac == m) { m_known.removeAt(i); saveKnown(); emit changed(); return true; }
    return false;
}

int ApiServer::knownImport(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = f.errorString(); return -1; }
    QJsonParseError pe;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll(), &pe).object()["devices"].toArray();
    if (pe.error != QJsonParseError::NoError) { if (error) *error = pe.errorString(); return -1; }
    int added = 0;
    for (const QJsonValue &v : arr) {
        const Known k = knownFromJson(v.toObject());
        if (k.mac.isEmpty()) continue;
        bool found = false;
        for (Known &e : m_known) if (e.mac == k.mac) {           // refresh what the controller knows, keep our own name/scopes
            e.hostname = k.hostname; e.fixedIp = k.fixedIp; e.ip = k.ip; e.network = k.network; e.online = k.online; e.wired = k.wired; e.lastSeen = k.lastSeen;
            if (e.name.isEmpty() || e.name == e.mac) e.name = k.name;
            if (e.scopes.isEmpty()) e.scopes = k.scopes;
            found = true; break;
        }
        if (!found) { m_known << k; ++added; }
    }
    saveKnown(); emit changed();
    return added;
}

// The kernel's neighbour table: who is behind that IP? (IPv4 via /proc/net/arp, IPv6 via `ip -j neigh`)
QString ApiServer::macForIp(const QString &ip)
{
    if (ip.contains(QLatin1Char('.'))) {
        QFile f(QStringLiteral("/proc/net/arp"));
        if (f.open(QIODevice::ReadOnly)) {
            f.readLine();
            while (!f.atEnd()) {
                const QList<QByteArray> cols = f.readLine().simplified().split(' ');
                if (cols.size() >= 4 && cols[0] == ip.toLatin1() && cols[3] != "00:00:00:00:00:00") return normMac(QString::fromLatin1(cols[3]));
            }
        }
        return {};
    }
    QProcess p; p.start(QStringLiteral("ip"), {QStringLiteral("-j"), QStringLiteral("-6"), QStringLiteral("neigh"), QStringLiteral("show"), ip});
    if (!p.waitForFinished(1500)) return {};
    for (const QJsonValue &v : QJsonDocument::fromJson(p.readAllStandardOutput()).array()) {
        const QJsonObject o = v.toObject();
        if (o["dst"].toString() == ip && !o["lladdr"].toString().isEmpty()) return normMac(o["lladdr"].toString());
    }
    return {};
}

// Match order matters: most of our devices sit on other VLANs behind the router, so the neighbour
// table shows the ROUTER's MAC for them. Their file entry must carry ip / fixed_ip, matched first;
// MACs (exact, then glob) are only consulted for peers on one of this box's own subnets.
const ApiServer::Known *ApiServer::knownFor(const QString &ip)
{
    if (m_known.isEmpty()) return nullptr;
    const QHostAddress a(ip);
    static Known self; self.mac = QStringLiteral("this host"); self.name = QStringLiteral("this computer"); self.ours = true;
    if (a.isLoopback()) return &self;                          // no MAC for ourselves
    bool sameSubnet = false;
    for (const QNetworkInterface &nif : QNetworkInterface::allInterfaces())
        for (const QNetworkAddressEntry &e : nif.addressEntries()) {
            if (e.ip() == a) return &self;
            if (e.ip().protocol() == a.protocol() && e.prefixLength() > 0 && a.isInSubnet(e.ip(), e.prefixLength())) sameSubnet = true;
        }
    for (const Known &k : m_known)                             // 1. the address the controller pinned it to
        if (k.ours && ((!k.fixedIp.isEmpty() && k.fixedIp == ip) || (!k.ip.isEmpty() && k.ip == ip))) return &k;
    if (!sameSubnet) return nullptr;                           // a different VLAN: the MAC we would see is the router's
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString mac;
    const auto cached = m_neigh.constFind(ip);
    if (cached != m_neigh.constEnd() && now - cached->second < 10000) mac = cached->first;
    else { mac = macForIp(ip); m_neigh.insert(ip, {mac, now}); }
    if (mac.isEmpty()) return nullptr;
    for (const Known &k : m_known) if (k.ours && k.mac == mac) return &k;                 // 2. exact MAC
    for (const Known &k : m_known) {                                                       // 3. MAC glob
        if (!k.ours || !(k.mac.contains(QLatin1Char('?')) || k.mac.contains(QLatin1Char('*')))) continue;
        if (QRegularExpression(QRegularExpression::wildcardToRegularExpression(k.mac), QRegularExpression::CaseInsensitiveOption).match(mac).hasMatch()) return &k;
    }
    return nullptr;
}

void ApiServer::updateDiscovery()
{
    if (m_avahi) { m_avahi->kill(); m_avahi->waitForFinished(300); m_avahi->deleteLater(); m_avahi = nullptr; }
    if (!listening()) return;
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("avahi-publish-service"));
    if (exe.isEmpty()) return;                              // no Avahi: nothing to advertise with, silently
    m_avahi = new QProcess(this);
    m_avahi->setProgram(exe);
    m_avahi->setArguments({QStringLiteral("-s"), QStringLiteral("BeaconFix on %1").arg(QHostInfo::localHostName()),
                           QStringLiteral("_beaconfix._tcp"), QString::number(boundPort()), QStringLiteral("v=1"),
                           QStringLiteral("pair=%1").arg(pairingOpen() ? 1 : 0), QStringLiteral("tls=%1").arg(m_tls ? 1 : 0)});
    m_avahi->setStandardOutputFile(QProcess::nullDevice()); m_avahi->setStandardErrorFile(QProcess::nullDevice());
    m_avahi->start();
}

// ── HTTP ──────────────────────────────────────────────────────────────────────
void ApiServer::onConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *s = m_server->nextPendingConnection();
        if (!s) break;
        if (!isLanAddress(s->peerAddress())) {                // not our network: refuse before reading anything
            logAccess(s, QStringLiteral("-"), QStringLiteral("-"), 403);
            replyRaw(s, 403, "application/json", "{\"error\":\"forbidden\"}");
            continue;
        }
        if (m_open >= MAX_OPEN) { replyRaw(s, 503, "application/json", "{\"error\":\"busy\"}"); continue; }
        ++m_open;
        connect(s, &QTcpSocket::disconnected, this, [this, s] { --m_open; s->deleteLater(); });
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadyRead(s); });
        QTimer::singleShot(10000, s, [s] { if (!s->property("handled").toBool()) s->disconnectFromHost(); });   // slowloris guard
        if (s->bytesAvailable()) onReadyRead(s);
    }
}

void ApiServer::onReadyRead(QTcpSocket *s)
{
    if (s->property("handled").toBool()) { s->readAll(); return; }
    QByteArray buf = s->property("buf").toByteArray() + s->readAll();
    const int hdrEnd = buf.indexOf("\r\n\r\n");
    if (hdrEnd < 0) {
        if (buf.size() > 8192) { s->setProperty("handled", true); replyRaw(s, 400, "application/json", "{\"error\":\"bad request\"}"); return; }
        s->setProperty("buf", buf); return;
    }
    const QList<QByteArray> lines = buf.left(hdrEnd).split('\n');
    const QList<QByteArray> reqLine = lines.value(0).trimmed().split(' ');
    if (reqLine.size() < 2) { s->setProperty("handled", true); replyRaw(s, 400, "application/json", "{\"error\":\"bad request\"}"); return; }
    Request r;
    r.method = QString::fromLatin1(reqLine[0]).toUpper();
    const QUrl u = QUrl::fromEncoded(reqLine[1], QUrl::StrictMode);
    r.path = u.path(); r.query = u.query();
    for (int i = 1; i < lines.size(); ++i) {
        const int c = lines[i].indexOf(':');
        if (c > 0) r.headers.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
    }
    const int len = r.headers.value("content-length", "0").toInt();
    if (len < 0 || len > bodyLimitFor(r.path)) { s->setProperty("handled", true); replyRaw(s, 413, "application/json", "{\"error\":\"body too large\"}"); return; }
    if (buf.size() < hdrEnd + 4 + len) { s->setProperty("buf", buf); return; }
    r.body = buf.mid(hdrEnd + 4, len);
    s->setProperty("handled", true);
    handle(s, r);
}

void ApiServer::replyRaw(QTcpSocket *s, int code, const QByteArray &type, const QByteArray &body, const QList<QByteArray> &extra)
{
    static const QHash<int, QByteArray> text{{200, "OK"}, {202, "Accepted"}, {204, "No Content"}, {400, "Bad Request"}, {401, "Unauthorized"}, {500, "Internal Server Error"},
                                             {403, "Forbidden"}, {404, "Not Found"}, {405, "Method Not Allowed"}, {413, "Payload Too Large"},
                                             {429, "Too Many Requests"}, {503, "Service Unavailable"}};
    QByteArray h = "HTTP/1.1 " + QByteArray::number(code) + " " + text.value(code, "OK") + "\r\nContent-Type: " + type
                 + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n";
    for (const QByteArray &e : extra) h += e + "\r\n";
    s->write(h + "\r\n" + body);
    s->disconnectFromHost();
}

void ApiServer::reply(QTcpSocket *s, int code, const QJsonObject &body, const QList<QByteArray> &extra)
{
    replyRaw(s, code, "application/json; charset=utf-8", QJsonDocument(body).toJson(QJsonDocument::Compact), extra);
}

void ApiServer::logAccess(QTcpSocket *s, const QString &method, const QString &path, int status)
{
    m_log.append({QDateTime::currentDateTime(), clientIp(s), method, path, status});
    while (m_log.size() > LOG_KEEP) m_log.removeFirst();
    emit accessLogged();
}

bool ApiServer::rateLimited(const QString &ip, bool authFailure)
{
    QList<qint64> &l = m_hits[ip];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    while (!l.isEmpty() && l.first() < now - 60000) l.removeFirst();
    l.append(now); if (authFailure) l.append(now);
    return l.size() > RATE_PER_MIN;
}

ApiServer::Device *ApiServer::authenticate(const Request &r, const QString &ip)
{
    const QByteArray auth = r.headers.value("authorization");
    if (!auth.startsWith("Bearer ")) return nullptr;
    const QByteArray hash = tokenHash(QString::fromLatin1(auth.mid(7).trimmed()));
    Device *found = nullptr;
    for (Device &d : m_devices)                              // scan them all: no early exit on match
        if (!d.revoked && constantTimeEqual(d.hash, hash)) found = &d;
    if (found) { found->lastSeen = QDateTime::currentDateTime(); found->lastIp = ip; m_dirty = true; m_saveTimer.start(); }
    return found;
}

QJsonObject ApiServer::locationJson() const
{
    const Fix &f = m_loc->fix();
    QJsonObject o{{"valid", f.valid}};
    if (!f.valid) return o;
    o["lat"] = f.lat; o["lon"] = f.lon; o["accuracy"] = f.accuracy;
    o["source"] = f.source; o["provider"] = f.provider; o["place"] = f.place;
    o["city"] = f.city; o["region"] = f.region; o["country"] = f.country;
    o["elevation"] = f.hasElevation() ? QJsonValue(f.elevation) : QJsonValue();
    o["time"] = f.time.toString(Qt::ISODate);
    o["age_s"] = f.time.isValid() ? double(f.time.secsTo(QDateTime::currentDateTime())) : QJsonValue();
    const SunTimes su = m_loc->sun();
    if (su.valid) {
        auto iso = [](const QDateTime &d) { return d.isValid() ? QJsonValue(d.toString(Qt::ISODate)) : QJsonValue(); };
        o["sun"] = QJsonObject{{"sunrise", iso(su.sunrise)}, {"sunset", iso(su.sunset)}, {"solarNoon", iso(su.solarNoon)},
                               {"goldenMorningEnd", iso(su.goldenMorningEnd)}, {"goldenEveningStart", iso(su.goldenEveningStart)},
                               {"civilDawn", iso(su.civilDawn)}, {"civilDusk", iso(su.civilDusk)}, {"dayLengthSecs", su.dayLengthSecs},
                               {"isDay", su.isDay(QDateTime::currentDateTime())}, {"polarDay", su.polarDay}, {"polarNight", su.polarNight}};
    }
    o["geo"] = m_loc->geoUri();
    o["links"] = QJsonObject{{"osm", m_loc->osmUrl()}, {"google", m_loc->googleMapsUrl()}, {"apple", m_loc->appleMapsUrl()}};
    o["home"] = homeJson();
    return o;
}

QJsonObject ApiServer::homeJson() const
{
    const Stats st = m_loc->stats();
    QJsonObject h{{"patterns", QJsonArray::fromStringList(m_loc->homeNetworks())}, {"atHome", st.atHome}, {"awayKm", st.awayKm}, {"awayText", st.awayText}};
    if (st.homeKnown) { h["homeLat"] = st.homeLat; h["homeLon"] = st.homeLon; h["homeTime"] = st.homeTime.toString(Qt::ISODate); }
    else { h["homeLat"] = QJsonValue(); h["homeLon"] = QJsonValue(); h["homeTime"] = QJsonValue(); }
    return h;
}

QJsonObject ApiServer::stateObject() const
{
    return QJsonDocument::fromJson(m_loc->StateJson().toUtf8()).object();
}

void ApiServer::broadcast(const QByteArray &event, const QJsonObject &data)
{
    const QByteArray frame = "event: " + event + "\ndata: " + QJsonDocument(data).toJson(QJsonDocument::Compact) + "\n\n";
    for (int i = m_streams.size() - 1; i >= 0; --i) {
        QTcpSocket *s = m_streams[i].sock;
        if (!s || s->state() != QAbstractSocket::ConnectedState) { m_streams.removeAt(i); continue; }
        s->write(frame);
    }
}

void ApiServer::handle(QTcpSocket *s, const Request &r)
{
    const QString ip = clientIp(s);
    auto finish = [&](int code, const QJsonObject &body, const QList<QByteArray> &extra = {}) { logAccess(s, r.method, r.path, code); reply(s, code, body, extra); };
    auto deny401 = [&] {
        rateLimited(ip, true);
        finish(401, QJsonObject{{"error", "unauthorized"}}, {"WWW-Authenticate: Bearer realm=\"BeaconFix\""});
    };
    if (rateLimited(ip, false)) { finish(429, QJsonObject{{"error", "rate limited"}}, {"Retry-After: 60"}); return; }
    if (!r.path.startsWith(QLatin1String("/api/v1/"))) { finish(404, QJsonObject{{"error", "not found"}}); return; }
    const QString ep = r.path.mid(8);                       // after /api/v1/

    // ── no auth ──
    if (ep == QLatin1String("hello")) {
        if (r.method != QLatin1String("GET")) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        finish(200, QJsonObject{{"name", "BeaconFix"}, {"version", QStringLiteral(BEACONFIX_VERSION)}, {"hostname", QHostInfo::localHostName()},
                                {"pairing", pairingOpen()}, {"tls", m_tls}, {"ts", QDateTime::currentDateTime().toString(Qt::ISODate)},
                                {"features", QJsonArray{"sync", "locate", "home", "events", "stream", "estimates", "identity"}}, {"api", 2}});
        return;
    }
    if (ep == QLatin1String("pair")) {
        if (r.method != QLatin1String("POST")) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: POST"}); return; }
        if (!pairingOpen()) { finish(403, QJsonObject{{"error", "pairing closed"}, {"hint", "open pairing in BeaconFix → Devices, then retry"}}); return; }
        if (pending().size() >= MAX_PENDING) { finish(429, QJsonObject{{"error", "too many pending requests"}}); return; }
        const QJsonObject b = QJsonDocument::fromJson(r.body).object();
        Pending p;
        p.id = randomId(8); p.ip = ip;
        p.name = b["name"].toString().trimmed().left(64); if (p.name.isEmpty()) p.name = QStringLiteral("Device at %1").arg(ip);
        p.scopes = QStringList{QStringLiteral("read")};
        for (const QJsonValue &v : b["scopes"].toArray()) if (v.toString() == QLatin1String("control")) p.scopes << QStringLiteral("control");
        p.code = QStringLiteral("%1").arg(QRandomGenerator::system()->bounded(10000), 4, 10, QLatin1Char('0'));
        p.created = QDateTime::currentDateTime(); p.expires = p.created.addSecs(PAIR_MINUTES * 60);
        const Known *kn = knownFor(ip);
        m_pending << p;
        if (kn && kn->ours) {                                   // one of ours: approve on the spot
            QStringList sc = kn->scopes;
            if (sc.isEmpty()) sc = QStringList{QStringLiteral("read")};
            if (!sc.contains(QStringLiteral("read"))) sc.prepend(QStringLiteral("read"));
            m_pending.last().scopes = sc;
            approve(p.id);
            m_loc->notify(QStringLiteral("Auto-approved %1 (%2)").arg(p.name, ip),
                          QStringLiteral("%1 is one of our devices (%2) — access granted: %3").arg(p.name, kn->name, sc.join(QStringLiteral(", "))), QStringLiteral("network-connect"));
        } else {
            m_loc->notify(QStringLiteral("%1 (%2) wants access — code %3").arg(p.name, ip, p.code),
                          QStringLiteral("%1Approve or deny it in BeaconFix → Devices%2")
                              .arg(m_knownOnly ? QStringLiteral("UNKNOWN DEVICE (not in the known list). ") : QString(),
                                   p.scopes.contains(QStringLiteral("control")) ? QStringLiteral(" (asks for control too)") : QString()),
                          QStringLiteral("network-connect"));
        }
        emit pairingRequested(QString::fromUtf8(QJsonDocument(p.toJson()).toJson(QJsonDocument::Compact)));
        emit changed();
        finish(202, QJsonObject{{"id", p.id}, {"code", p.code}, {"expires", p.expires.toString(Qt::ISODate)}, {"poll", QStringLiteral("/api/v1/pair/%1").arg(p.id)}});
        return;
    }
    if (ep.startsWith(QLatin1String("pair/"))) {
        if (r.method != QLatin1String("GET")) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        const QString id = ep.mid(5);
        const QDateTime now = QDateTime::currentDateTime();
        for (Pending &p : m_pending) {
            if (p.id != id || p.expires < now) continue;
            if (p.state == Pending::Waiting) { finish(200, QJsonObject{{"status", "pending"}}); return; }
            if (p.state == Pending::Denied)  { finish(200, QJsonObject{{"status", "denied"}}); return; }
            QJsonObject o{{"status", "approved"}, {"scopes", QJsonArray::fromStringList(p.scopes)}};
            if (!p.token.isEmpty()) { o["token"] = p.token; p.token.clear(); }   // exactly once
            finish(200, o);
            return;
        }
        finish(404, QJsonObject{{"error", "unknown or expired pairing request"}});
        return;
    }

    // ── identity (docs/IDENTITY.md): public record, challenge/auth, link, LAN hand-off ──
    Identity *idn = m_loc->identity();
    if (ep == QLatin1String("identity")) {
        if (r.method != QLatin1String("GET")) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        if (!idn || !idn->exists()) { finish(404, QJsonObject{{"error", "no identity on this BeaconFix yet"}}); return; }
        finish(200, idn->publicJson());
        return;
    }
    if (ep == QLatin1String("identity/challenge")) {
        if (r.method != QLatin1String("GET")) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        if (!idn || !idn->exists()) { finish(404, QJsonObject{{"error", "no identity on this BeaconFix yet"}}); return; }
        const QDateTime now = QDateTime::currentDateTime();
        for (auto it = m_nonces.begin(); it != m_nonces.end();) { if (it.value() < now) it = m_nonces.erase(it); else ++it; }
        if (m_nonces.size() > 64) { finish(429, QJsonObject{{"error", "too many open challenges"}}); return; }
        QByteArray n(32, 0); if (RAND_bytes(reinterpret_cast<unsigned char *>(n.data()), 32) != 1) { finish(500, QJsonObject{{"error", "rng"}}); return; }
        const QString nonce = QString::fromLatin1(n.toBase64());
        m_nonces.insert(nonce, now.addSecs(60));
        finish(200, QJsonObject{{"nonce", nonce}, {"host", QHostInfo::localHostName()}, {"expires", now.addSecs(60).toString(Qt::ISODate)}, {"id", idn->id()}});
        return;
    }
    if (ep == QLatin1String("identity/auth")) {
        if (r.method != QLatin1String("POST")) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: POST"}); return; }
        if (!idn || !idn->exists()) { finish(404, QJsonObject{{"error", "no identity on this BeaconFix yet"}}); return; }
        const QJsonObject b = QJsonDocument::fromJson(r.body).object();
        const QString id = b["id"].toString(), nonce = b["nonce"].toString();
        const QByteArray pub = QByteArray::fromBase64(b["pub"].toString().toLatin1()), sig = QByteArray::fromBase64(b["sig"].toString().toLatin1());
        const QJsonObject dev = b["device"].toObject();
        const QString devName = dev["name"].toString().trimmed().left(64), devKind = dev["kind"].toString().trimmed().left(16);
        if (id.isEmpty() || pub.size() != 32 || sig.size() != 64 || nonce.isEmpty() || devName.isEmpty()) { finish(400, QJsonObject{{"error", "id, pub, nonce, sig and device.name required"}}); return; }
        const auto nit = m_nonces.find(nonce);
        if (nit == m_nonces.end() || nit.value() < QDateTime::currentDateTime()) { rateLimited(ip, true); finish(403, QJsonObject{{"error", "unknown or expired nonce"}}); return; }
        m_nonces.erase(nit);
        if (Identity::idFor(pub) != id) { rateLimited(ip, true); finish(403, QJsonObject{{"error", "id does not match pub"}}); return; }
        if (!Identity::verify(pub, Identity::authCanon(QHostInfo::localHostName(), nonce.toLatin1(), id, devName), sig)) { rateLimited(ip, true); finish(403, QJsonObject{{"error", "bad signature"}}); return; }
        if (!idn->isOwner(id)) {
            PendingLink p; p.id = id; p.name = b["name"].toString().left(64); p.ip = ip; p.deviceName = devName; p.deviceKind = devKind; p.pub = pub; p.time = QDateTime::currentDateTime();
            idn->addPending(p);
            m_loc->notify(QStringLiteral("%1 (%2) asked to sign in with another identity").arg(devName, ip),
                          QStringLiteral("Identity %1 is not yours or linked. Link it in BeaconFix → Devices if it is one of your own.").arg(Identity::groupId(id)), QStringLiteral("user-identity"));
            emit changed();
            finish(403, QJsonObject{{"error", "unknown identity"}, {"hint", "link this identity from the desktop (Devices → Link)"}, {"id", id}});
            return;
        }
        // Ours (or linked): issue a control token, no pairing prompt
        for (Device &d : m_devices) if (!d.revoked && d.identity == id && d.name == devName) d.revoked = true;   // one live token per device name
        const QString tok = createToken(devName, {QStringLiteral("read"), QStringLiteral("control")}, id);
        if (id == idn->id()) idn->addDevice(devName, devKind.isEmpty() ? QStringLiteral("device") : devKind, pub);
        m_loc->notify(QStringLiteral("%1 signed in").arg(devName), QStringLiteral("Identity %1 (%2) from %3").arg(idn->name(), Identity::groupId(id), ip), QStringLiteral("user-identity"));
        finish(200, QJsonObject{{"token", tok}, {"scopes", QJsonArray{"read", "control"}}, {"identity", QJsonObject{{"id", idn->id()}, {"name", idn->name()}}}, {"device", devName}});
        return;
    }
    if (ep == QLatin1String("identity/link")) {
        if (r.method != QLatin1String("POST")) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: POST"}); return; }
        if (!idn || !idn->exists()) { finish(404, QJsonObject{{"error", "no identity on this BeaconFix yet"}}); return; }
        const QJsonObject b = QJsonDocument::fromJson(r.body).object();
        const QJsonObject st = b.contains("statement") ? b["statement"].toObject() : b;
        QString err; LinkStatement done;
        if (!idn->acceptLink(LinkStatement::fromJson(st), &err, &done)) { rateLimited(ip, true); finish(403, QJsonObject{{"error", err}}); return; }
        emit changed();
        m_loc->notify(QStringLiteral("Identity linked"), QStringLiteral("%1 is now linked with %2").arg(Identity::groupId(done.a), Identity::groupId(done.b)), QStringLiteral("user-identity"));
        finish(200, QJsonObject{{"statement", done.toJson(true)}, {"linkedIds", QJsonArray::fromStringList(idn->linkedIds())}});
        return;
    }
    if (ep.startsWith(QLatin1String("identity/export/")) && r.method == QLatin1String("GET")) {   // LAN hand-off: fetch the held bundle with the one-time code
        const QString code = ep.mid(16);
        const QDateTime now = QDateTime::currentDateTime();
        for (auto it = m_exports.begin(); it != m_exports.end();) { if (it.value().expires < now) it = m_exports.erase(it); else ++it; }
        auto it = m_exports.find(code);
        if (it == m_exports.end()) {
            for (auto &h : m_exports) if (++h.tries >= 5) { m_exports.clear(); break; }   // 5 wrong guesses burn every held bundle
            rateLimited(ip, true); finish(404, QJsonObject{{"error", "no bundle under that code"}}); return;
        }
        const QString bundle = it.value().bundle; m_exports.erase(it);
        finish(200, QJsonObject{{"bundle", bundle}, {"hint", "decrypt with the passphrase / word code shown on the sending BeaconFix"}});
        return;
    }

    // ── authenticated ──
    if (m_knownOnly && !knownFor(ip)) {                          // tokens only work from OUR devices
        rateLimited(ip, true);
        finish(403, QJsonObject{{"error", "unknown device"}, {"hint", "this address is not in BeaconFix's known-device list"}});
        return;
    }
    Device *dev = authenticate(r, ip);
    if (!dev) { deny401(); return; }
    const bool control = dev->scopes.contains(QStringLiteral("control"));
    const bool get = r.method == QLatin1String("GET"), post = r.method == QLatin1String("POST");

    if (ep == QLatin1String("identity/export")) {          // hold our bundle for a device on the LAN (one-time code, 10 min)
        if (!post) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: POST"}); return; }
        if (!control) { finish(403, QJsonObject{{"error", "control scope required"}}); return; }
        if (!idn || !idn->unlocked()) { finish(404, QJsonObject{{"error", "no unlocked identity on this BeaconFix"}}); return; }
        const QJsonObject b = QJsonDocument::fromJson(r.body).object();
        QString pass = b["passphrase"].toString();
        const bool words = pass.isEmpty(); if (words) pass = Identity::wordCode();
        QString err; const QString bundle = idn->exportBundle(pass, &err);
        if (bundle.isEmpty()) { finish(400, QJsonObject{{"error", err}}); return; }
        const QString code = holdIdentityExport(bundle);
        QJsonObject o{{"code", code}, {"expires", QDateTime::currentDateTime().addSecs(600).toString(Qt::ISODate)}, {"fetch", QStringLiteral("/api/v1/identity/export/%1").arg(code)}};
        if (words) o["words"] = pass;                            // generated for the caller: shown once here, never stored
        finish(200, o);
        return;
    }
    if (ep == QLatin1String("location")) { if (!get) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; } finish(200, locationJson()); return; }
    if (ep == QLatin1String("state"))    { if (!get) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; } finish(200, stateObject()); return; }
    if (ep == QLatin1String("events")) {
        if (!get) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        const int since = QUrlQuery(r.query).queryItemValue(QStringLiteral("since")).toInt();
        QJsonArray ev;
        for (const BeaconEvent &e : m_loc->events()) if (e.id > since) ev.append(e.toJson());
        finish(200, QJsonObject{{"since", since}, {"lastEventId", m_loc->lastEventId()}, {"events", ev}});
        return;
    }
    if (ep == QLatin1String("emergency")) {                   // nearest police / fire / ER / urgent care / pharmacy / vet + the local number
        if (!get) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        QJsonObject o = m_loc->emergencyJson(); o["ts"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        finish(200, o);
        return;
    }
    if (ep == QLatin1String("pois") && (QUrlQuery(r.query).hasQueryItem(QStringLiteral("cat")) || QUrlQuery(r.query).hasQueryItem(QStringLiteral("group")) || QUrlQuery(r.query).hasQueryItem(QStringLiteral("radius")))) {
        if (!get) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        const QUrlQuery qq(r.query);
        QStringList keys = (qq.queryItemValue(QStringLiteral("cat")) + QLatin1Char(',') + qq.queryItemValue(QStringLiteral("group"))).split(QLatin1Char(','), Qt::SkipEmptyParts);
        const double radius = qq.queryItemValue(QStringLiteral("radius")).toDouble();
        const QJsonArray all = stateObject()["pois"].toArray();
        QJsonArray out2;
        for (const QJsonValue &v : all) {
            const QJsonObject p = v.toObject();
            bool match = keys.isEmpty();
            for (const QString &k : keys) if (k.compare(p["cat"].toString(), Qt::CaseInsensitive) == 0 || k.compare(p["group"].toString(), Qt::CaseInsensitive) == 0 || k == QLatin1String("all")) match = true;
            if (!match) continue;
            if (radius > 0 && p.contains("d") && p["d"].toDouble() > radius * 1000) continue;
            out2.append(p);
        }
        finish(200, QJsonObject{{"pois", out2}, {"count", out2.size()}, {"filter", QJsonArray::fromStringList(keys)}, {"radiusKm", radius}, {"ts", QDateTime::currentDateTime().toString(Qt::ISODate)}});
        return;
    }
    if (ep == QLatin1String("aps") || ep == QLatin1String("pois") || ep == QLatin1String("track") || ep == QLatin1String("trip")) {
        if (!get) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        const QJsonObject st = stateObject();
        const QString key = ep == QLatin1String("trip") ? QStringLiteral("stats") : ep;
        QJsonObject o{{ep, st[key]}};
        if (ep != QLatin1String("trip")) o["count"] = st[key].toArray().size();
        o["ts"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        finish(200, o);
        return;
    }
    if (ep == QLatin1String("home")) {
        if (get) { finish(200, homeJson()); return; }
        if (r.method != QLatin1String("PUT") && !post) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET, PUT"}); return; }
        if (!control) { finish(403, QJsonObject{{"error", "control scope required"}}); return; }
        const QJsonObject b = QJsonDocument::fromJson(r.body).object();
        if (!b["patterns"].isArray()) { finish(400, QJsonObject{{"error", "patterns[] required"}}); return; }
        QStringList l; for (const QJsonValue &v : b["patterns"].toArray()) if (v.isString()) l << v.toString();
        m_loc->setHomeNetworks(l);
        finish(200, homeJson());
        return;
    }
    if (ep == QLatin1String("locate")) {                      // the internal map locates another device from what IT hears
        if (!post) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: POST"}); return; }
        MapDb *db = m_loc->mapDb();
        if (!db || !db->isOpen()) { finish(503, QJsonObject{{"error", "map database unavailable"}}); return; }
        QList<QPair<QString, int>> heard;
        for (const QJsonValue &v : QJsonDocument::fromJson(r.body).object()["wifiAccessPoints"].toArray()) {
            const QJsonObject a = v.toObject();
            const QString mac = a["macAddress"].toString().toUpper().trimmed();
            if (mac.size() == 17) heard << qMakePair(mac, a["signalStrength"].toInt(-80));
        }
        if (heard.isEmpty()) { finish(400, QJsonObject{{"error", "wifiAccessPoints[] with macAddress required"}}); return; }
        double lat = 0, lon = 0, acc = 0; int used = 0; QStringList usedIds;
        if (!db->estimate(heard, &lat, &lon, &acc, &used, &usedIds, 2, 150)) {
            finish(404, QJsonObject{{"error", "no location could be estimated from the internal map"}, {"known", used}, {"queried", heard.size()}});
            return;
        }
        finish(200, QJsonObject{{"location", QJsonObject{{"lat", lat}, {"lng", lon}}}, {"accuracy", acc}, {"used", used}, {"queried", heard.size()},
                                {"usedBssids", QJsonArray::fromStringList(usedIds)}, {"source", "internal"}});
        return;
    }
    if (ep == QLatin1String("db/stats")) {
        if (!get) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        MapDb *db = m_loc->mapDb();
        finish(200, db ? db->stats() : QJsonObject{{"open", false}});
        return;
    }
    if (ep == QLatin1String("db/observations")) {
        if (!post) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: POST"}); return; }
        if (!control) { finish(403, QJsonObject{{"error", "control scope required"}}); return; }
        MapDb *db = m_loc->mapDb();
        if (!db || !db->isOpen() || db->readOnly()) { finish(503, QJsonObject{{"error", "map database not writable"}}); return; }
        QString err;
        const QJsonObject b = QJsonDocument::fromJson(r.body).object();
        const QString from = b["device"].toString().isEmpty() ? dev->name : b["device"].toString().left(64);
        const int n = m_loc->ingestObservations(b["observations"].toArray(), from, &err);
        if (n < 0) { finish(500, QJsonObject{{"error", err}}); return; }
        finish(200, QJsonObject{{"added", n}, {"device", from}, {"cursor", double(db->currentSeq())}, {"refitQueued", n > 0}});
        return;
    }
    if (ep == QLatin1String("db/changes")) {                  // the sync feed: everything after a cursor, oldest first
        if (!get) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        MapDb *db = m_loc->mapDb();
        if (!db || !db->isOpen()) { finish(503, QJsonObject{{"error", "map database unavailable"}}); return; }
        const QUrlQuery qq(r.query);
        const qint64 since = qq.queryItemValue(QStringLiteral("since")).toLongLong();
        const int limit = qq.hasQueryItem(QStringLiteral("limit")) ? qq.queryItemValue(QStringLiteral("limit")).toInt() : 500;
        bool more = false; qint64 cursor = 0;
        QJsonObject o = db->changesSince(since, limit, &more, &cursor);
        o["device"] = QHostInfo::localHostName();
        if (idn && idn->exists()) {                              // every row in this feed belongs to our identity
            o["identity"] = idn->id();
            for (const char *k : {"aps", "observations", "fixes"}) {
                QJsonArray arr = o[k].toArray(); QJsonArray out2;
                for (const QJsonValue &v : arr) { QJsonObject row = v.toObject(); if (!row.contains("identity")) row["identity"] = idn->id(); out2.append(row); }
                o[k] = out2;
            }
        }
        finish(200, o);
        return;
    }
    if (ep == QLatin1String("db/sync")) {                     // a peer's changes, merged; then it pulls ours from /db/changes
        if (!post) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: POST"}); return; }
        if (!control) { finish(403, QJsonObject{{"error", "control scope required"}}); return; }
        MapDb *db = m_loc->mapDb();
        if (!db || !db->isOpen() || db->readOnly()) { finish(503, QJsonObject{{"error", "map database not writable"}}); return; }
        const QJsonObject b = QJsonDocument::fromJson(r.body).object();
        if (b.isEmpty()) { finish(400, QJsonObject{{"error", "JSON body required"}}); return; }
        const QString from = b["device"].toString().isEmpty() ? dev->name : b["device"].toString().left(64);
        // Identity check: a peer that names an identity must be us or linked, unless it holds a (legacy) pairing token
        const QString peerId = b["identity"].toString();
        if (!peerId.isEmpty() && idn && idn->exists() && !idn->isOwner(peerId) && !dev->identity.isEmpty()) {
            finish(403, QJsonObject{{"error", "identity not linked"}, {"identity", peerId}}); return;
        }
        QString err;
        const int obs = m_loc->ingestObservations(b["observations"].toArray(), from, &err);
        const int aps = m_loc->mergePeerAps(b["aps"].toArray(), from);
        const int fixes = m_loc->appendPeerFixes(b["fixes"].toArray(), from);
        if (obs < 0 || aps < 0 || fixes < 0) { finish(500, QJsonObject{{"error", err.isEmpty() ? QStringLiteral("merge failed") : err}}); return; }
        QJsonObject o{{"accepted", QJsonObject{{"observations", obs}, {"aps", aps}, {"fixes", fixes}}}, {"cursor", double(db->currentSeq())}, {"refitQueued", obs > 0 || aps > 0}, {"device", from}};
        if (idn && idn->exists()) o["identity"] = idn->id();
        if (b.contains("sinceCursor")) {                        // convenience: the peer's pull in the same round trip
            bool more = false; qint64 cursor = 0;
            o["changes"] = db->changesSince(qint64(b["sinceCursor"].toDouble()), 500, &more, &cursor);
        }
        finish(200, o);
        return;
    }
    if (ep == QLatin1String("db/export")) {
        if (!get) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        if (!control) { finish(403, QJsonObject{{"error", "control scope required"}}); return; }
        MapDb *db = m_loc->mapDb();
        if (!db || !db->isOpen()) { finish(503, QJsonObject{{"error", "map database unavailable"}}); return; }
        finish(200, db->exportJson());
        return;
    }
    if (ep == QLatin1String("stream")) {
        if (!get) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: GET"}); return; }
        for (int i = m_streams.size() - 1; i >= 0; --i) if (!m_streams[i].sock || m_streams[i].sock->state() != QAbstractSocket::ConnectedState) m_streams.removeAt(i);
        if (m_streams.size() >= MAX_STREAMS) { finish(503, QJsonObject{{"error", "too many streams"}}); return; }
        logAccess(s, r.method, r.path, 200);
        s->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\n"
                 "retry: 5000\nevent: fix\ndata: " + QJsonDocument(locationJson()).toJson(QJsonDocument::Compact) + "\n\n");
        m_streams.append({s, dev->id});
        return;
    }
    if (ep == QLatin1String("refresh") || ep == QLatin1String("prefetch")) {
        if (!post) { finish(405, QJsonObject{{"error", "method not allowed"}}, {"Allow: POST"}); return; }
        if (!control) { finish(403, QJsonObject{{"error", "control scope required"}}); return; }
        if (ep == QLatin1String("refresh")) m_loc->Refresh(); else m_loc->PrefetchTiles();
        finish(202, QJsonObject{{"ok", true}, {"action", ep}});
        return;
    }
    finish(404, QJsonObject{{"error", "not found"}});
}

QString ApiServer::holdIdentityExport(const QString &bundle)
{
    const QDateTime now = QDateTime::currentDateTime();
    for (auto it = m_exports.begin(); it != m_exports.end();) { if (it.value().expires < now) it = m_exports.erase(it); else ++it; }
    QString code;
    do { code = QStringLiteral("%1").arg(QRandomGenerator::system()->bounded(1000000), 6, 10, QLatin1Char('0')); } while (m_exports.contains(code));
    m_exports.insert(code, ExportHold{bundle, now.addSecs(600), 0});
    return code;
}
