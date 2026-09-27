#include "mdns.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSet>
#include <QUrl>
#include <algorithm>

static const char *AVAHI = "org.freedesktop.Avahi";
static const char *AVAHI_SERVER = "org.freedesktop.Avahi.Server";
static const char *AVAHI_GROUP = "org.freedesktop.Avahi.EntryGroup";
static const char *AVAHI_BROWSER = "org.freedesktop.Avahi.ServiceBrowser";
static const char *SERVICE_TYPE = "_beaconfix._tcp";
static const int   IF_UNSPEC = -1, PROTO_UNSPEC = -1;
static const uint  LOOKUP_RESULT_LOCAL = 0x8;           // AvahiLookupResultFlags: the record is ours
static const int   GROUP_ESTABLISHED = 2, GROUP_COLLISION = 3, GROUP_FAILURE = 4;
static const int   SERVER_RUNNING = 2;
static const int   SCAN_TTL_S = 600, MDNS_STALE_S = 1800;

using TxtList = QList<QByteArray>;

// ── Peer ──────────────────────────────────────────────────────────────────────
QString Mdns::Peer::url() const
{
    if (addresses.isEmpty() || port <= 0) return {};
    const QString a = addresses.first();
    const bool v6 = a.contains(QLatin1Char(':'));
    return QStringLiteral("%1://%2:%3").arg(tls ? QStringLiteral("https") : QStringLiteral("http"), v6 ? QStringLiteral("[%1]").arg(a) : a).arg(port);
}

QJsonObject Mdns::Peer::toJson() const
{
    return {{"name", serviceName}, {"host", host}, {"addresses", QJsonArray::fromStringList(addresses)}, {"port", port}, {"url", url()},
            {"identityId", identityId}, {"identityName", identityName}, {"version", version}, {"kind", kind}, {"api", api},
            {"features", QJsonArray::fromStringList(features)}, {"pairing", pairing}, {"tls", tls}, {"self", self},
            {"interface", iface}, {"lastSeen", lastSeen.toString(Qt::ISODate)}, {"source", source}};
}

// ── Interfaces ────────────────────────────────────────────────────────────────
static bool virtualName(const QString &n)
{
    static const QRegularExpression re(QStringLiteral("^(docker|virbr|veth|br-|tun|tap|wg|vnet|lxc|lxd|zt|tailscale|utun|ppp|vmnet|cni|flannel|kube|ifb|dummy|bond_slave|macvtap|vboxnet|nm-bridge)"));
    return re.match(n).hasMatch();
}

QList<QPair<QString, int>> Mdns::realInterfaces()
{
    QList<QPair<QString, int>> out, p2p;
    for (const QNetworkInterface &nif : QNetworkInterface::allInterfaces()) {
        const auto fl = nif.flags();
        if (!(fl & QNetworkInterface::IsUp) || !(fl & QNetworkInterface::IsRunning) || (fl & QNetworkInterface::IsLoopBack)) continue;
        if (virtualName(nif.name())) continue;
        bool hasAddr = false;
        for (const QNetworkAddressEntry &e : nif.addressEntries())
            if (!e.ip().isLoopback() && !e.ip().isLinkLocal()) hasAddr = true;
        if (!hasAddr) continue;
        if (fl & QNetworkInterface::IsPointToPoint) p2p << qMakePair(nif.name(), nif.index()); else out << qMakePair(nif.name(), nif.index());
    }
    return out.isEmpty() ? p2p : out;                      // a VPN is better than nothing
}

QStringList Mdns::lanAddresses()
{
    QStringList v4, v6;
    for (const auto &nm : realInterfaces()) {
        const QNetworkInterface nif = QNetworkInterface::interfaceFromIndex(nm.second);
        for (const QNetworkAddressEntry &e : nif.addressEntries()) {
            const QHostAddress a = e.ip();
            if (a.isLoopback() || a.isLinkLocal()) continue;
            if (a.protocol() == QAbstractSocket::IPv4Protocol) v4 << a.toString();
            else if (a.protocol() == QAbstractSocket::IPv6Protocol && !a.isLinkLocal()) v6 << QHostAddress(a.toIPv6Address()).toString();
        }
    }
    return v4 + v6;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
Mdns::Mdns(QObject *parent) : QObject(parent)
{
    qDBusRegisterMetaType<TxtList>();
    m_watcher = new QDBusServiceWatcher(QString::fromLatin1(AVAHI), QDBusConnection::systemBus(),
                                        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] { connectAvahi(); if (m_port > 0) registerGroup(); });
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] { dropAvahi(); if (m_port > 0) publishFallback(); });
    m_republish.setSingleShot(true); m_republish.setInterval(800);
    connect(&m_republish, &QTimer::timeout, this, &Mdns::registerGroup);
    m_sweep.setInterval(60000);
    connect(&m_sweep, &QTimer::timeout, this, [this] {
        const QDateTime now = QDateTime::currentDateTime();
        bool changed = false;
        for (auto it = m_peers.begin(); it != m_peers.end();) {
            const qint64 age = it->lastSeen.secsTo(now);
            if ((it->source == QLatin1String("scan") && age > SCAN_TTL_S) || (it->source == QLatin1String("mdns") && age > MDNS_STALE_S)) { it = m_peers.erase(it); changed = true; }
            else ++it;
        }
        if (changed) emit peersChanged();
    });
    m_sweep.start();
    connectAvahi();
}

Mdns::~Mdns()
{
    withdraw();
    dropAvahi();
}

void Mdns::connectAvahi()
{
    if (m_server) return;
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected() || !bus.interface() || !bus.interface()->isServiceRegistered(QString::fromLatin1(AVAHI))) { m_available = false; return; }
    m_server = new QDBusInterface(QString::fromLatin1(AVAHI), QStringLiteral("/"), QString::fromLatin1(AVAHI_SERVER), bus, this);
    if (!m_server->isValid()) { delete m_server; m_server = nullptr; m_available = false; m_error = QStringLiteral("Avahi D-Bus interface unavailable"); return; }
    m_available = true; m_error.clear();
    bus.connect(QString::fromLatin1(AVAHI), QStringLiteral("/"), QString::fromLatin1(AVAHI_SERVER), QStringLiteral("StateChanged"), this, SLOT(onServerStateChanged(int,QString)));
    startBrowser();
    emit stateChanged();
}

void Mdns::dropAvahi()
{
    if (!m_server) return;
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!m_browserPath.isEmpty()) {
        QDBusMessage m = QDBusMessage::createMethodCall(QString::fromLatin1(AVAHI), m_browserPath, QString::fromLatin1(AVAHI_BROWSER), QStringLiteral("Free"));
        bus.call(m, QDBus::NoBlock);
        m_browserPath.clear();
    }
    if (!m_groupPath.isEmpty()) {
        QDBusMessage m = QDBusMessage::createMethodCall(QString::fromLatin1(AVAHI), m_groupPath, QString::fromLatin1(AVAHI_GROUP), QStringLiteral("Free"));
        bus.call(m, QDBus::NoBlock);
        m_groupPath.clear();
    }
    delete m_server; m_server = nullptr;
    m_available = false; m_published = false;
    bool changed = false;
    for (auto it = m_peers.begin(); it != m_peers.end();) { if (it->source == QLatin1String("mdns")) { it = m_peers.erase(it); changed = true; } else ++it; }
    if (changed) emit peersChanged();
    emit stateChanged();
}

void Mdns::onServerStateChanged(int state, const QString &error)
{
    if (state == SERVER_RUNNING) { m_error.clear(); if (m_port > 0) m_republish.start(); if (m_browserPath.isEmpty()) startBrowser(); }
    else { m_published = false; m_error = error.isEmpty() ? QStringLiteral("Avahi state %1").arg(state) : error; }
    emit stateChanged();
}

// ── Publishing ────────────────────────────────────────────────────────────────
void Mdns::publish(int port, const QStringList &txt, const QString &instanceName)
{
    m_port = port; m_txt = txt; m_ownPort = port;
    m_instance = instanceName.isEmpty() ? QStringLiteral("BeaconFix on %1").arg(QHostInfo::localHostName()) : instanceName;
    m_renames = 0;
    if (port <= 0) { withdraw(); return; }
    if (m_available) registerGroup(); else publishFallback();
}

void Mdns::withdraw()
{
    m_port = 0; m_published = false;
    if (m_fallback) { m_fallback->kill(); m_fallback->waitForFinished(300); m_fallback->deleteLater(); m_fallback = nullptr; }
    if (!m_groupPath.isEmpty() && m_server) {
        QDBusMessage m = QDBusMessage::createMethodCall(QString::fromLatin1(AVAHI), m_groupPath, QString::fromLatin1(AVAHI_GROUP), QStringLiteral("Free"));
        QDBusConnection::systemBus().call(m, QDBus::NoBlock);
        m_groupPath.clear();
    }
    emit stateChanged();
}

void Mdns::registerGroup()
{
    if (!m_server || m_port <= 0) return;
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!m_groupPath.isEmpty()) {                          // reuse: Reset() then re-add
        QDBusMessage r = QDBusMessage::createMethodCall(QString::fromLatin1(AVAHI), m_groupPath, QString::fromLatin1(AVAHI_GROUP), QStringLiteral("Reset"));
        bus.call(r, QDBus::Block, 2000);
    } else {
        QDBusReply<QDBusObjectPath> g = m_server->call(QStringLiteral("EntryGroupNew"));
        if (!g.isValid()) { m_error = QStringLiteral("EntryGroupNew: %1").arg(g.error().message()); m_published = false; publishFallback(); emit stateChanged(); return; }
        m_groupPath = g.value().path();
        bus.connect(QString::fromLatin1(AVAHI), m_groupPath, QString::fromLatin1(AVAHI_GROUP), QStringLiteral("StateChanged"), this, SLOT(onGroupStateChanged(int,QString)));
    }
    TxtList txt;
    for (const QString &t : m_txt) if (!t.isEmpty()) txt << t.toUtf8().left(255);
    QList<QPair<QString, int>> ifs = realInterfaces();
    if (ifs.isEmpty()) ifs << qMakePair(QStringLiteral("*"), IF_UNSPEC);
    int added = 0;
    for (const auto &nm : ifs) {
        QDBusMessage m = QDBusMessage::createMethodCall(QString::fromLatin1(AVAHI), m_groupPath, QString::fromLatin1(AVAHI_GROUP), QStringLiteral("AddService"));
        m.setArguments({nm.second, PROTO_UNSPEC, quint32(0), m_instance, QString::fromLatin1(SERVICE_TYPE), QString(), QString(), QVariant::fromValue(quint16(m_port)), QVariant::fromValue(txt)});
        const QDBusMessage rep = bus.call(m, QDBus::Block, 3000);
        if (rep.type() == QDBusMessage::ErrorMessage) {
            // Another BeaconFix on this very host already registered the name (a second instance): take the next one
            if (rep.errorName().contains(QLatin1String("Collision")) || rep.errorMessage().contains(QLatin1String("collision"), Qt::CaseInsensitive)) {
                if (++m_renames < 10) { m_instance = QStringLiteral("BeaconFix on %1 (%2)").arg(QHostInfo::localHostName()).arg(m_renames + 1); m_republish.start(); }
                else m_error = QStringLiteral("AddService: %1").arg(rep.errorMessage());
                return;
            }
            m_error = QStringLiteral("AddService(%1): %2").arg(nm.first, rep.errorMessage()); continue;
        }
        ++added;
    }
    if (!added) { m_published = false; emit stateChanged(); return; }
    QDBusMessage c = QDBusMessage::createMethodCall(QString::fromLatin1(AVAHI), m_groupPath, QString::fromLatin1(AVAHI_GROUP), QStringLiteral("Commit"));
    const QDBusMessage rep = bus.call(c, QDBus::Block, 3000);
    if (rep.type() == QDBusMessage::ErrorMessage) { m_error = QStringLiteral("Commit: %1").arg(rep.errorMessage()); m_published = false; }
    else { m_error.clear(); if (m_fallback) { m_fallback->kill(); m_fallback->waitForFinished(300); m_fallback->deleteLater(); m_fallback = nullptr; } }
    emit stateChanged();
}

void Mdns::onGroupStateChanged(int state, const QString &error)
{
    if (state == GROUP_ESTABLISHED) { m_published = true; m_error.clear(); }
    else if (state == GROUP_COLLISION) {                   // someone else uses our instance name: append a number and try again
        m_published = false;
        if (++m_renames < 10) { m_instance = QStringLiteral("BeaconFix on %1 (%2)").arg(QHostInfo::localHostName()).arg(m_renames + 1); m_republish.start(); }
    } else if (state == GROUP_FAILURE) { m_published = false; m_error = error; }
    emit stateChanged();
}

void Mdns::publishFallback()
{
    if (m_fallback) { m_fallback->kill(); m_fallback->waitForFinished(300); m_fallback->deleteLater(); m_fallback = nullptr; }
    if (m_port <= 0) return;
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("avahi-publish-service"));
    if (exe.isEmpty()) { if (m_error.isEmpty()) m_error = QStringLiteral("no Avahi D-Bus and no avahi-publish-service: not advertised"); return; }
    QStringList args{QStringLiteral("-s"), m_instance, QString::fromLatin1(SERVICE_TYPE), QString::number(m_port)};
    args += m_txt;
    m_fallback = new QProcess(this);
    m_fallback->setProgram(exe); m_fallback->setArguments(args);
    m_fallback->setStandardOutputFile(QProcess::nullDevice()); m_fallback->setStandardErrorFile(QProcess::nullDevice());
    m_fallback->start();
    m_published = m_fallback->waitForStarted(500);
}

// ── Browsing ──────────────────────────────────────────────────────────────────
void Mdns::startBrowser()
{
    if (!m_server || !m_browserPath.isEmpty()) return;
    QDBusConnection bus = QDBusConnection::systemBus();
    // Avahi ≥ 0.7: Prepare, connect the signals, then Start — no races. Older: ServiceBrowserNew.
    QDBusReply<QDBusObjectPath> b = m_server->call(QStringLiteral("ServiceBrowserPrepare"), IF_UNSPEC, PROTO_UNSPEC, QString::fromLatin1(SERVICE_TYPE), QString(), quint32(0));
    bool prepared = b.isValid();
    if (!prepared) b = m_server->call(QStringLiteral("ServiceBrowserNew"), IF_UNSPEC, PROTO_UNSPEC, QString::fromLatin1(SERVICE_TYPE), QString(), quint32(0));
    if (!b.isValid()) { m_error = QStringLiteral("ServiceBrowser: %1").arg(b.error().message()); return; }
    m_browserPath = b.value().path();
    bus.connect(QString::fromLatin1(AVAHI), m_browserPath, QString::fromLatin1(AVAHI_BROWSER), QStringLiteral("ItemNew"), this, SLOT(onItemNew(int,int,QString,QString,QString,uint)));
    bus.connect(QString::fromLatin1(AVAHI), m_browserPath, QString::fromLatin1(AVAHI_BROWSER), QStringLiteral("ItemRemove"), this, SLOT(onItemRemove(int,int,QString,QString,QString,uint)));
    bus.connect(QString::fromLatin1(AVAHI), m_browserPath, QString::fromLatin1(AVAHI_BROWSER), QStringLiteral("Failure"), this, SLOT(onBrowserFailure(QString)));
    if (prepared) {
        QDBusMessage s = QDBusMessage::createMethodCall(QString::fromLatin1(AVAHI), m_browserPath, QString::fromLatin1(AVAHI_BROWSER), QStringLiteral("Start"));
        bus.call(s, QDBus::NoBlock);
    }
}

QString Mdns::peerKey(int iface, int proto, const QString &name) { return QStringLiteral("%1/%2/%3").arg(iface).arg(proto).arg(name); }

void Mdns::onItemNew(int iface, int proto, const QString &name, const QString &type, const QString &domain, uint flags)
{
    Q_UNUSED(flags)
    resolve(iface, proto, name, type, domain);
}

void Mdns::onItemRemove(int iface, int proto, const QString &name, const QString &, const QString &, uint)
{
    if (m_peers.remove(peerKey(iface, proto, name))) emit peersChanged();
}

void Mdns::onBrowserFailure(const QString &error)
{
    m_error = QStringLiteral("browse: %1").arg(error);
    m_browserPath.clear();
    QTimer::singleShot(5000, this, &Mdns::startBrowser);
}

void Mdns::resolve(int iface, int proto, const QString &name, const QString &type, const QString &domain)
{
    if (!m_server) return;
    QDBusPendingCall call = m_server->asyncCall(QStringLiteral("ResolveService"), iface, proto, name, type, domain, PROTO_UNSPEC, quint32(0));
    auto *w = new QDBusPendingCallWatcher(call, this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, iface, proto, name] {
        w->deleteLater();
        const QDBusMessage rep = w->reply();
        if (rep.type() == QDBusMessage::ErrorMessage) return;   // gone again, or unresolvable
        const QList<QVariant> a = rep.arguments();
        if (a.size() < 11) return;
        Peer p;
        p.serviceName = name;
        p.source = QStringLiteral("mdns");
        p.iface = QNetworkInterface::interfaceFromIndex(iface).name();
        const QString hostLocal = a[5].toString(), address = a[7].toString();
        p.port = a[8].toUInt();
        const uint flags = a[10].toUInt();
        TxtList txt = qdbus_cast<TxtList>(a[9]);
        QStringList addrTxt;
        for (const QByteArray &t : txt) {
            const int eq = t.indexOf('=');
            if (eq <= 0) continue;
            const QString k = QString::fromUtf8(t.left(eq)), v = QString::fromUtf8(t.mid(eq + 1));
            if (k == QLatin1String("id")) p.identityId = v;
            else if (k == QLatin1String("name")) p.identityName = v;
            else if (k == QLatin1String("host")) p.host = v;
            else if (k == QLatin1String("v")) p.version = v;
            else if (k == QLatin1String("kind")) p.kind = v;
            else if (k == QLatin1String("api")) p.api = v.toInt();
            else if (k == QLatin1String("pair")) p.pairing = v == QLatin1String("1");
            else if (k == QLatin1String("tls")) p.tls = v == QLatin1String("1");
            else if (k == QLatin1String("features")) p.features = v.split(QLatin1Char(','), Qt::SkipEmptyParts);
            else if (k == QLatin1String("addr")) addrTxt = v.split(QLatin1Char(','), Qt::SkipEmptyParts);
        }
        if (p.host.isEmpty()) { p.host = hostLocal; if (p.host.endsWith(QLatin1String(".local"))) p.host.chop(6); }
        if (p.kind.isEmpty()) p.kind = QStringLiteral("desktop");
        // Addresses: the TXT list first (real interfaces, chosen by the peer), the resolved one as a fallback,
        // IPv4 before IPv6, link-local v6 last (it needs a scope id to be usable)
        QStringList v4, v6, ll;
        auto add = [&](const QString &s) { const QHostAddress h(s); if (h.isNull() || h.isLoopback()) return;
            const QString n = h.protocol() == QAbstractSocket::IPv4Protocol ? QHostAddress(h.toIPv4Address()).toString() : QHostAddress(h.toIPv6Address()).toString();
            if (h.isLinkLocal()) { if (!ll.contains(n)) ll << n; } else if (h.protocol() == QAbstractSocket::IPv4Protocol) { if (!v4.contains(n)) v4 << n; } else if (!v6.contains(n)) v6 << n; };
        for (const QString &s : addrTxt) add(s);
        add(address);
        p.addresses = v4 + v6 + ll;
        const QStringList mine = lanAddresses();
        bool local = (flags & LOOKUP_RESULT_LOCAL) || p.host.compare(QHostInfo::localHostName(), Qt::CaseInsensitive) == 0;
        for (const QString &s : p.addresses) if (mine.contains(s)) local = true;
        p.local = local;
        p.self = local && (m_ownPort == 0 || p.port == m_ownPort);   // re-evaluated in peers() once our own port is known
        p.lastSeen = QDateTime::currentDateTime();
        m_peers.insert(peerKey(iface, proto, name), p);
        emit peersChanged();
    });
}

// ── Peers ─────────────────────────────────────────────────────────────────────
QList<Mdns::Peer> Mdns::peers(bool includeSelf) const
{
    // One entry per device: an instance seen on several interfaces / protocols / scans is merged
    QHash<QString, Peer> merged;
    for (Peer p : m_peers) {
        p.self = p.local && (m_ownPort == 0 || p.port == m_ownPort);   // another instance on this host (other port) is a peer
        if (!includeSelf && p.self) continue;
        const QString key = !p.identityId.isEmpty() ? QStringLiteral("id:") + p.identityId + QLatin1Char('/') + p.host.toLower()
                          : !p.host.isEmpty() ? QStringLiteral("host:") + p.host.toLower() : QStringLiteral("addr:") + p.addresses.join(QLatin1Char(','));
        auto it = merged.find(key);
        if (it == merged.end()) { merged.insert(key, p); continue; }
        for (const QString &a : p.addresses) if (!it->addresses.contains(a)) it->addresses << a;
        if (it->lastSeen < p.lastSeen) { const QStringList addrs = it->addresses; const bool self = it->self || p.self; *it = p; it->addresses = addrs; it->self = self; }
        else it->self = it->self || p.self;
        if (it->source != p.source) it->source = QStringLiteral("mdns");
    }
    QList<Peer> out = merged.values();
    std::sort(out.begin(), out.end(), [](const Peer &a, const Peer &b) { if (a.self != b.self) return b.self; return a.host.toLower() < b.host.toLower(); });
    return out;
}

QJsonArray Mdns::peersJson(bool includeSelf) const
{
    QJsonArray arr;
    for (const Peer &p : peers(includeSelf)) arr.append(p.toJson());
    return arr;
}

const Mdns::Peer *Mdns::find(const QString &nameOrHost) const
{
    static Peer hit;
    const QString q = nameOrHost.trimmed().toLower();
    if (q.isEmpty()) return nullptr;
    QString qh = q; if (qh.endsWith(QLatin1String(".local"))) qh.chop(6);
    QString qid = q; qid.remove(QLatin1Char('-'));
    for (const Peer &p : peers(false)) {
        if (p.identityName.toLower() == q || p.host.toLower() == qh || p.serviceName.toLower() == q || p.addresses.contains(q)
            || (!p.identityId.isEmpty() && p.identityId == qid)) { hit = p; return &hit; }
    }
    return nullptr;
}

// ── Subnet scan ───────────────────────────────────────────────────────────────
void Mdns::scanSubnets(int port, std::function<void()> done)
{
    if (m_scanPending > 0) { if (done) { auto prev = m_scanDone; m_scanDone = [prev, done] { if (prev) prev(); done(); }; } return; }
    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    QSet<int> ports{port > 0 ? port : 47822};
    for (const Peer &p : m_peers) if (p.port > 0) ports.insert(p.port);
    const QStringList mine = lanAddresses();
    QStringList targets;
    for (const auto &nm : realInterfaces()) {
        const QNetworkInterface nif = QNetworkInterface::interfaceFromIndex(nm.second);
        for (const QNetworkAddressEntry &e : nif.addressEntries()) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol || e.ip().isLoopback()) continue;
            const quint32 base = e.ip().toIPv4Address() & 0xffffff00u;   // our own /24 only — a /20 would be 4096 probes
            for (quint32 h = 1; h < 255; ++h) { const QString a = QHostAddress(base | h).toString(); if (!mine.contains(a) && !targets.contains(a)) targets << a; }
        }
        if (targets.size() >= 508) break;
    }
    if (targets.isEmpty()) { if (done) done(); return; }
    m_scanDone = done;
    for (const QString &a : targets)
        for (int p : ports) {
            QNetworkRequest req(QUrl(QStringLiteral("http://%1:%2/api/v1/hello").arg(a).arg(p)));
            req.setTransferTimeout(300);
            req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("BeaconFix-scan"));
            ++m_scanPending;
            QNetworkReply *rep = m_nam->get(req);
            connect(rep, &QNetworkReply::finished, this, [this, rep, a, p] {
                rep->deleteLater();
                if (rep->error() == QNetworkReply::NoError) {
                    const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
                    if (o["name"].toString() == QLatin1String("BeaconFix")) mergeScanHello(a, p, o);
                }
                if (--m_scanPending <= 0) { m_scanPending = 0; auto d = m_scanDone; m_scanDone = nullptr; emit peersChanged(); if (d) d(); }
            });
        }
}

void Mdns::mergeScanHello(const QString &address, int port, const QJsonObject &hello)
{
    Peer p;
    p.serviceName = QStringLiteral("BeaconFix on %1").arg(hello["hostname"].toString());
    p.host = hello["hostname"].toString(); p.addresses = QStringList{address}; p.port = port;
    p.version = hello["version"].toString(); p.kind = hello["kind"].toString(); if (p.kind.isEmpty()) p.kind = QStringLiteral("desktop");
    p.api = hello["api"].toInt(); p.pairing = hello["pairing"].toBool(); p.tls = hello["tls"].toBool();
    for (const QJsonValue &v : hello["features"].toArray()) p.features << v.toString();
    const QJsonObject idn = hello["identity"].toObject();
    p.identityId = idn["id"].toString(); p.identityName = idn["name"].toString();
    p.local = lanAddresses().contains(address) || p.host.compare(QHostInfo::localHostName(), Qt::CaseInsensitive) == 0;
    p.self = p.local && (m_ownPort == 0 || port == m_ownPort);
    p.lastSeen = QDateTime::currentDateTime(); p.source = QStringLiteral("scan");
    // An mDNS entry for the same host wins; otherwise keep the scan result
    for (const Peer &q : m_peers) if (q.source == QLatin1String("mdns") && (q.addresses.contains(address) || (!q.host.isEmpty() && q.host.compare(p.host, Qt::CaseInsensitive) == 0))) return;
    m_peers.insert(QStringLiteral("scan:%1:%2").arg(address).arg(port), p);
}
