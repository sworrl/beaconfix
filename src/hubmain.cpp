// beaconfix --server: the hub (docs/SECURE-API.md, deploy/README.md). Headless — QCoreApplication, no window, no tray,
// no Wi-Fi scanning, no session bus / KWallet (the key file), no mDNS. Holds the master map database, serves BFS3.
//   beaconfix --server                                   run (systemd: deploy/beaconfix-hub.service)
//   beaconfix --server --invite <name> [--kind k] [--scopes read,sync,control] [--minutes 15]
//   beaconfix --server --devices | --status [--json] | --revoke <deviceId-or-name>
// The admin commands talk to the running hub over its loopback admin listener with the token it wrote to
// <state>/hub-admin.json — run them as the hub's user with the same environment (deploy/beaconfix-hub-cli).
//
// beaconfix --node: a headless compute / scanning node (the RV VM, a workstation without a session) — the Locator as
// usual (Starlink, Wi-Fi via NetworkManager, BeaconDB …), the hub link (sync, positions, jobs), no window / tray /
// session bus. --node --enroll 'bfs3:…' enrols it (with the node stopped: it owns the database while it runs).
#include "apiserver.h"
#include "hub.h"
#include "locator.h"
#include "mapdb.h"
#include "hubclient.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <csignal>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

static int s_sigFd[2] = {-1, -1};
static void onSignal(int) { const char c = 1; if (::write(s_sigFd[0], &c, 1) < 0) {} }

static bool adminCall(const QByteArray &method, const QString &ep, const QJsonObject &body, QJsonObject *out, QString *err)
{
    QFile f(Locator::stateDir() + QStringLiteral("/hub-admin.json"));
    if (!f.open(QIODevice::ReadOnly)) { *err = QStringLiteral("no running hub here (%1: %2) — start it (systemctl start beaconfix-hub) and run this as its user with its environment").arg(f.fileName(), f.errorString()); return false; }
    const QJsonObject a = QJsonDocument::fromJson(f.readAll()).object();
    QNetworkAccessManager nam;
    QNetworkRequest r(QUrl(a["url"].toString() + QStringLiteral("/api/v1/") + ep));
    r.setRawHeader("X-BF-Admin", a["token"].toString().toLatin1());
    r.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    r.setTransferTimeout(15000);
    QNetworkReply *rep = method == "GET" ? nam.get(r) : nam.post(r, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop; QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit); loop.exec();
    const int status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    *out = QJsonDocument::fromJson(rep->readAll()).object();
    rep->deleteLater();
    if (status == 0) { *err = QStringLiteral("cannot reach the hub's admin listener (%1): %2").arg(a["url"].toString(), rep->errorString()); return false; }
    if (status != 200) { *err = QStringLiteral("HTTP %1: %2").arg(status).arg(out->value("error").toString()); return false; }
    return true;
}

static void printDevices(QTextStream &out, const QJsonArray &devs)
{
    if (devs.isEmpty()) { out << "  (no devices enrolled yet — beaconfix --server --invite <name>)\n"; return; }
    for (const QJsonValue &v : devs) {
        const QJsonObject d = v.toObject();
        QStringList sc; for (const QJsonValue &x : d["scopes"].toArray()) sc << x.toString();
        out << "  " << d["id"].toString() << "  " << d["name"].toString().leftJustified(22) << " " << d["kind"].toString().leftJustified(8)
            << " [" << sc.join(QLatin1Char(',')) << "]  last seen " << (d["lastSeen"].isNull() ? QStringLiteral("never") : d["lastSeen"].toString())
            << (d["lastIp"].toString().isEmpty() ? QString() : QStringLiteral(" from ") + d["lastIp"].toString())
            << "  counter " << qint64(d["counter"].toDouble()) << (d["revoked"].toBool() ? "  REVOKED" : "") << "\n";
        const QJsonObject c = d["capabilities"].toObject();
        if (!c.isEmpty())
            out << "      " << c["role"].toString() << " v" << c["version"].toString() << " · compute " << c["compute"].toInt() << " cores"
                << (c["gpu"].isString() ? QStringLiteral(" · gpu ") + c["gpu"].toString() : QString()) << " · wifi " << c["wifiScan"].toInt()
                << (c["mobile"].toBool() ? " · mobile" : "") << (c["tensor"].toBool() ? " · tensor" : "")
                << " · jobs done " << d["jobsDone"].toInt() << (d["jobsFailed"].toInt() ? QStringLiteral(", failed %1").arg(d["jobsFailed"].toInt()) : QString())
                << (d["lastLease"].isString() ? QStringLiteral(" · last lease ") + d["lastLease"].toString() : QString()) << "\n";
    }
}

int hubMain(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("sworrl"));
    QCoreApplication::setApplicationName(QStringLiteral("beaconfix"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BEACONFIX_VERSION));

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("BeaconFix hub: the master map database for every node, served over BFS3 (docs/SECURE-API.md)."));
    p.addHelpOption(); p.addVersionOption();
    QCommandLineOption server(QStringLiteral("server"), QStringLiteral("Run as the hub (headless). Environment: BEACONFIX_HUB_LISTEN, _CERT, _KEY, _URL, _ADMIN, _ALLOW (deploy/beaconfix-hub.env)."));
    QCommandLineOption invite(QStringLiteral("invite"), QStringLiteral("With --server: make a single-use invite (15 min) for a device called <name>; prints it as text + QR."), QStringLiteral("name"));
    QCommandLineOption kind(QStringLiteral("kind"), QStringLiteral("With --invite: the device kind (android, desktop, laptop, deck, pi …)."), QStringLiteral("kind"));
    QCommandLineOption scopes(QStringLiteral("scopes"), QStringLiteral("With --invite: comma-separated from read,sync,control (default all)."), QStringLiteral("list"));
    QCommandLineOption minutes(QStringLiteral("minutes"), QStringLiteral("With --invite: validity, 1–15 minutes (default 15)."), QStringLiteral("n"));
    QCommandLineOption devices(QStringLiteral("devices"), QStringLiteral("With --server: list the enrolled devices."));
    QCommandLineOption revoke(QStringLiteral("revoke"), QStringLiteral("With --server: revoke the device <id or name> (its next request fails)."), QStringLiteral("id"));
    QCommandLineOption status(QStringLiteral("status"), QStringLiteral("With --server: the hub's state (fingerprint, listeners, devices, invites)."));
    QCommandLineOption json(QStringLiteral("json"), QStringLiteral("With --status / --devices / --invite: print JSON."));
    p.addOptions({server, invite, kind, scopes, minutes, devices, revoke, status, json});
    p.process(app);
    QTextStream out(stdout);

    // ── admin commands: through the running hub ──
    if (p.isSet(invite) || p.isSet(devices) || p.isSet(revoke) || p.isSet(status)) {
        QJsonObject o; QString err; int rc = 0;
        if (p.isSet(invite)) {
            QJsonArray sc; for (const QString &s : p.value(scopes).split(QLatin1Char(','), Qt::SkipEmptyParts)) sc.append(s.trimmed());
            if (!adminCall("POST", QStringLiteral("hub/invite"), QJsonObject{{"name", p.value(invite)}, {"kind", p.value(kind)}, {"scopes", sc}, {"minutes", p.isSet(minutes) ? p.value(minutes).toInt() : 15}}, &o, &err)) { fprintf(stderr, "beaconfix: %s\n", qPrintable(err)); return 1; }
            if (p.isSet(json)) out << QJsonDocument(o).toJson(QJsonDocument::Indented);
            else {
                out << "Invite for \"" << o["name"].toString() << "\"" << (o["kind"].toString().isEmpty() ? QString() : QStringLiteral(" (%1)").arg(o["kind"].toString()))
                    << " — single use, valid until " << o["expires"].toString() << "\nHub " << o["url"].toString() << "  fingerprint " << o["fingerprint"].toString() << "\n\n"
                    << o["invite"].toString() << "\n\n";
                const QString bin = QStandardPaths::findExecutable(QStringLiteral("qrencode"));
                if (bin.isEmpty()) out << "(apt install qrencode to see it as a QR code here)\n";
                else { QProcess q; q.start(bin, {QStringLiteral("-t"), QStringLiteral("ANSIUTF8"), QStringLiteral("-m"), QStringLiteral("2"), o["invite"].toString()}); q.waitForFinished(5000); out << QString::fromUtf8(q.readAllStandardOutput()); }
                out << "On a desktop:  beaconfix --enroll '" << o["invite"].toString() << "'\n";
            }
        }
        if (p.isSet(revoke)) {
            if (!adminCall("POST", QStringLiteral("hub/revoke"), QJsonObject{{"id", p.value(revoke)}}, &o, &err)) { fprintf(stderr, "beaconfix: %s\n", qPrintable(err)); rc = 1; }
            else out << "revoked " << p.value(revoke) << "\n";
        }
        if (p.isSet(devices) || p.isSet(status)) {
            if (!adminCall("GET", QStringLiteral("hub/status"), {}, &o, &err)) { fprintf(stderr, "beaconfix: %s\n", qPrintable(err)); return 1; }
            if (p.isSet(json)) out << QJsonDocument(p.isSet(status) ? o : QJsonObject{{"devices", o["devices"]}, {"fingerprint", o["fingerprint"]}}).toJson(QJsonDocument::Indented);
            else {
                out << "Hub " << o["url"].toString() << "  fingerprint " << o["fingerprint"].toString() << "\n";
                if (p.isSet(status)) {
                    const QJsonObject l = o["listen"].toObject();
                    out << "  listening: " << (l["listening"].toBool() ? QStringLiteral("port %1%2").arg(l["port"].toInt()).arg(l["tls"].toBool() ? " (TLS)" : " (plain HTTP)") : QStringLiteral("NO — ") + l["error"].toString()) << "\n";
                    for (const QJsonValue &v : o["invites"].toArray()) { const QJsonObject i = v.toObject(); out << "  invite " << i["id"].toString() << "  " << i["name"].toString() << "  " << i["status"].toString() << "  until " << i["expires"].toString() << "\n"; }
                    for (const QJsonValue &v : o["blocked"].toArray()) out << "  blocked source: " << v.toString() << "\n";
                    const QJsonObject j = o["jobs"].toObject();
                    out << "  jobs: " << j["queued"].toInt() << " queued, " << j["leased"].toInt() << " leased, last lease " << (j["lastLease"].isString() ? j["lastLease"].toString() : QStringLiteral("never"))
                        << (j["selfComputeIdleMinutes"].toInt() > 0 ? QStringLiteral(", self-compute after %1 min idle").arg(j["selfComputeIdleMinutes"].toInt()) : QStringLiteral(", self-compute off")) << "\n";
                }
                out << "Devices:\n"; printDevices(out, o["devices"].toArray());
            }
        }
        out.flush();
        return rc;
    }

    // ── the hub itself ──
    Locator::setHubRole(true);
    qputenv("BEACONFIX_NO_MDNS", "1"); qputenv("BEACONFIX_NO_BLE", "1"); qputenv("BEACONFIX_NO_KWALLET", "1");
    auto *loc = new Locator(false, &app);
    if (!loc->mapDb() || !loc->mapDb()->isOpen() || loc->mapDb()->readOnly()) {
        fprintf(stderr, "beaconfix: hub: the map database cannot be opened for writing: %s\n", loc->mapDb() ? qPrintable(loc->mapDb()->error()) : "no database");
        delete loc; return 1;
    }
    auto *api = new ApiServer(loc, &app);
    loc->setApiServer(api);
    if (!api->error().isEmpty() || !api->listening() || !api->hub() || !api->hub()->ready()) {
        fprintf(stderr, "beaconfix: %s\n", qPrintable(api->error().isEmpty() ? QStringLiteral("hub: not listening") : api->error()));
        delete api; delete loc; return 1;
    }
    const QJsonObject st = api->hub()->statusJson();
    fprintf(stderr, "beaconfix %s hub: %s on port %d, admin %s\n  url %s\n  fingerprint %s\n  database %s (key: %s)\n  %d device(s) enrolled\n",
            BEACONFIX_VERSION, api->tls() ? "TLS" : "plain HTTP", api->boundPort(), qPrintable(api->adminFile()), qPrintable(api->hub()->url()),
            qPrintable(api->hub()->fingerprint()), qPrintable(loc->mapDb()->path()), qPrintable(loc->mapDb()->keySource()), int(st["devices"].toArray().size()));
    // SIGTERM / SIGINT (systemctl stop): leave the event loop so the database is flushed (encrypted) on the way out
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, s_sigFd) == 0) {
        auto *sn = new QSocketNotifier(s_sigFd[1], QSocketNotifier::Read, &app);
        QObject::connect(sn, &QSocketNotifier::activated, &app, [] { char c; if (::read(s_sigFd[1], &c, 1) < 0) {} QCoreApplication::quit(); });
        struct sigaction sa {}; sa.sa_handler = onSignal; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &sa, nullptr); sigaction(SIGINT, &sa, nullptr);
    }
    signal(SIGPIPE, SIG_IGN);
    loc->start();
    const int rc = app.exec();
    fprintf(stderr, "beaconfix: hub stopping, flushing the database\n");
    delete api;                                               // before the Locator: the hub writes through its database
    delete loc;
    return rc;
}

// ── beaconfix --node ──────────────────────────────────────────────────────────
static QString nodePidFile() { return Locator::stateDir() + QStringLiteral("/node.pid"); }
static qint64 runningNodePid()
{
    QFile f(nodePidFile());
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const qint64 pid = f.readAll().trimmed().toLongLong();
    return pid > 0 && pid != QCoreApplication::applicationPid() && QFile::exists(QStringLiteral("/proc/%1").arg(pid)) ? pid : 0;
}

int nodeMain(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("sworrl"));
    QCoreApplication::setApplicationName(QStringLiteral("beaconfix"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BEACONFIX_VERSION));
    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("BeaconFix headless node: positions itself, syncs with the hub, computes the hub's jobs (docs/HUB.md)."));
    p.addHelpOption(); p.addVersionOption();
    QCommandLineOption node(QStringLiteral("node"), QStringLiteral("Run as a headless node (systemd: deploy/beaconfix-node.service)."));
    QCommandLineOption enroll(QStringLiteral("enroll"), QStringLiteral("With --node: enrol with a hub ('bfs3:…' from beaconfix --server --invite), then exit. The node must not be running."), QStringLiteral("invite"));
    QCommandLineOption enrollName(QStringLiteral("enroll-name"), QStringLiteral("With --enroll: the name the hub knows us by (default: the hostname)."), QStringLiteral("name"));
    QCommandLineOption status(QStringLiteral("status"), QStringLiteral("With --node: print the hub link of this node's database (not live), exit."));
    p.addOptions({node, enroll, enrollName, status});
    p.process(app);
    QTextStream out(stdout);
    Locator::setHeadless(true);
    qputenv("BEACONFIX_NODE", "1"); qputenv("BEACONFIX_NO_KWALLET", "1"); qputenv("BEACONFIX_NO_BLE", "1");
    if (qEnvironmentVariable("BEACONFIX_NODE_MDNS") != QLatin1String("1")) qputenv("BEACONFIX_NO_MDNS", "1");

    if (p.isSet(enroll) || p.isSet(status)) {
        if (const qint64 pid = runningNodePid()) { fprintf(stderr, "beaconfix: the node is running (pid %lld) and owns the database — stop it first (systemctl stop beaconfix-node)\n", (long long)pid); return 1; }
        Locator loc(false);
        if (!loc.mapDb() || !loc.mapDb()->isOpen() || loc.mapDb()->readOnly()) { fprintf(stderr, "beaconfix: the map database cannot be opened: %s\n", loc.mapDb() ? qPrintable(loc.mapDb()->error()) : ""); return 1; }
        HubClient hc(&loc);
        if (p.isSet(enroll)) {
            QEventLoop loop; QJsonObject res; bool ok = false;
            hc.enroll(p.value(enroll).trimmed(), p.value(enrollName).trimmed(), [&](bool k, const QJsonObject &o) { ok = k; res = o; loop.quit(); });
            if (res.isEmpty()) { QTimer::singleShot(60000, &loop, &QEventLoop::quit); loop.exec(); }
            if (!ok) { fprintf(stderr, "beaconfix: enrolment failed: %s\n", qPrintable(res["error"].toString(QStringLiteral("timed out")))); return 1; }
            out << "Enrolled with " << res["url"].toString() << " as " << res["name"].toString() << " (" << res["kind"].toString() << ")\n  device   " << res["deviceId"].toString()
                << "\n  hub key  " << res["fingerprint"].toString() << " (pinned)\nStart the node: systemctl start beaconfix-node\n";
        }
        if (p.isSet(status)) out << QJsonDocument(hc.statusJson()).toJson(QJsonDocument::Indented);
        out.flush();
        loc.mapDb()->flush();
        return 0;
    }

    if (const qint64 pid = runningNodePid()) { fprintf(stderr, "beaconfix: a node is already running (pid %lld)\n", (long long)pid); return 1; }
    { QSaveFile pf(nodePidFile()); if (pf.open(QIODevice::WriteOnly)) { pf.write(QByteArray::number(QCoreApplication::applicationPid())); pf.commit(); } }
    auto *loc = new Locator(false, &app);
    if (!loc->mapDb() || !loc->mapDb()->isOpen() || loc->mapDb()->readOnly()) {
        fprintf(stderr, "beaconfix: node: the map database cannot be opened for writing: %s\n", loc->mapDb() ? qPrintable(loc->mapDb()->error()) : "");
        delete loc; QFile::remove(nodePidFile()); return 1;
    }
    ApiServer *api = nullptr;
    if (qEnvironmentVariable("BEACONFIX_NODE_API") == QLatin1String("1")) { api = new ApiServer(loc, &app); loc->setApiServer(api); }   // the LAN API (port 47822), off by default
    auto *hc = new HubClient(loc, &app);
    loc->setHubClient(hc);
    const QJsonObject st = hc->statusJson();
    fprintf(stderr, "beaconfix %s node: %s\n  database %s\n  capabilities %s\n", BEACONFIX_VERSION,
            st["enrolled"].toBool() ? qPrintable(QStringLiteral("enrolled with %1 as %2 (%3)").arg(st["url"].toString(), st["name"].toString(), st["deviceId"].toString()))
                                    : "NOT enrolled — stop, run beaconfix --node --enroll 'bfs3:…', start",
            qPrintable(loc->mapDb()->path()), QJsonDocument(HubClient::capabilities()).toJson(QJsonDocument::Compact).constData());
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, s_sigFd) == 0) {
        auto *sn = new QSocketNotifier(s_sigFd[1], QSocketNotifier::Read, &app);
        QObject::connect(sn, &QSocketNotifier::activated, &app, [] { char c; if (::read(s_sigFd[1], &c, 1) < 0) {} QCoreApplication::quit(); });
        struct sigaction sa {}; sa.sa_handler = onSignal; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &sa, nullptr); sigaction(SIGINT, &sa, nullptr);
    }
    signal(SIGPIPE, SIG_IGN);
    loc->start();
    const int rc = app.exec();
    fprintf(stderr, "beaconfix: node stopping, flushing the database\n");
    delete hc; delete api; delete loc;
    QFile::remove(nodePidFile());
    return rc;
}
