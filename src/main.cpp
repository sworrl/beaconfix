#include "locator.h"
#include <algorithm>
#include <QHostInfo>
#include <QStandardPaths>
#include <QProcess>
#include "identity.h"
#include "mdns.h"
#include "mainwindow.h"
#include "beaconview.h"
#include "tilesource.h"
#include "tray.h"
#include "apiserver.h"
#include "mapdb.h"
#include "poiclassify.h"
#include "ranging/rangingservice.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTextStream>
#include <QEventLoop>
#include <QFileInfo>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QTimer>
#include <cstdio>
#include <unistd.h>
#include <QDate>
#include <QLocale>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

static const char *SVC  = "org.sworrl.BeaconFix";
static const char *PATH = "/org/sworrl/BeaconFix";

// --import: prints the tray's importProgress signals while the D-Bus call is in flight
class ImportProgressPrinter : public QObject {
    Q_OBJECT
public:
    ImportProgressPrinter(bool tty, const QString &file) : m_tty(tty), m_file(file) {}
public slots:
    void onProgress(const QString &json)
    {
        const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
        if (o["file"].toString() != m_file || o["done"].toBool()) return;
        if (m_tty) fprintf(stderr, "\r\033[K  %3d%%  %s", o["percent"].toInt(), qPrintable(o["stage"].toString()));
        else if (o["percent"].toInt() / 10 != m_lastTen) fprintf(stderr, "  %3d%%  %s\n", o["percent"].toInt(), qPrintable(o["stage"].toString()));
        m_lastTen = o["percent"].toInt() / 10;
        fflush(stderr);
    }
private:
    bool m_tty; QString m_file; int m_lastTen = -1;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("sworrl"));
    QCoreApplication::setApplicationName(QStringLiteral("beaconfix"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BEACONFIX_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("beaconfix"));
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("Where am I? Position fix from Wi-Fi beacons (BeaconDB), Starlink dish GPS, or IP."));
    p.addHelpOption(); p.addVersionOption();
    QCommandLineOption tray(QStringLiteral("tray"), QStringLiteral("Start in the system tray without showing the window."));
    QCommandLineOption once(QStringLiteral("once"), QStringLiteral("Run one probe standalone, print the result as JSON, exit."));
    QCommandLineOption json(QStringLiteral("json"), QStringLiteral("Print the current fix (from the running instance or the state file) as JSON, exit."));
    QCommandLineOption refresh(QStringLiteral("refresh"), QStringLiteral("Ask the running instance to re-check now (starts it if needed), exit."));
    QCommandLineOption snapshot(QStringLiteral("snapshot"), QStringLiteral("Render the window to <file> after one probe and exit (for docs/tests)."), QStringLiteral("file"));
    QCommandLineOption gpx(QStringLiteral("gpx"), QStringLiteral("Write the trip log as GPX to <file> (via the running instance, or from the state files), exit."), QStringLiteral("file"));
    QCommandLineOption copy(QStringLiteral("copy"), QStringLiteral("Copy the fix to the clipboard as <what>: coords | geo | osm | google | apple | text (needs the running instance), exit."), QStringLiteral("what"));
    QCommandLineOption newTrip(QStringLiteral("new-trip"), QStringLiteral("Start a new trip now (trip distance / stops reset from here), exit."));
    QCommandLineOption prefetch(QStringLiteral("prefetch"), QStringLiteral("Save map tiles around the fix for offline use, exit."));
    QCommandLineOption apiStatus(QStringLiteral("api-status"), QStringLiteral("Print the LAN API status (port, pairing, devices) as JSON, exit."));
    QCommandLineOption devices(QStringLiteral("devices"), QStringLiteral("List paired devices and pending pairing requests, exit."));
    QCommandLineOption approve(QStringLiteral("approve"), QStringLiteral("Approve the pending pairing request <id>, exit."), QStringLiteral("id"));
    QCommandLineOption deny(QStringLiteral("deny"), QStringLiteral("Deny the pending pairing request <id>, exit."), QStringLiteral("id"));
    QCommandLineOption revoke(QStringLiteral("revoke"), QStringLiteral("Revoke the device <name-or-id>'s token, exit."), QStringLiteral("name"));
    QCommandLineOption token(QStringLiteral("token"), QStringLiteral("Create a token for a device called <name> (read scope; add --control), print it ONCE, exit."), QStringLiteral("name"));
    QCommandLineOption control(QStringLiteral("control"), QStringLiteral("With --token: also grant the control scope (refresh, prefetch)."));
    QCommandLineOption pairing(QStringLiteral("pairing"), QStringLiteral("Allow pairing requests for <minutes> (0 closes), exit."), QStringLiteral("minutes"));
    QCommandLineOption homeAdd(QStringLiteral("home-add"), QStringLiteral("Add <pattern> (SSID or BSSID glob) to the home networks, exit."), QStringLiteral("pattern"));
    QCommandLineOption homeRemove(QStringLiteral("home-remove"), QStringLiteral("Remove <pattern> from the home networks, exit."), QStringLiteral("pattern"));
    QCommandLineOption homeList(QStringLiteral("home-list"), QStringLiteral("List the home networks and whether we are at home, exit."));
    QCommandLineOption homeSync(QStringLiteral("home-sync"), QStringLiteral("Pull the home networks from another BeaconFix's LAN API: --home-sync <http://host:47822> --home-token <token>."), QStringLiteral("url"));
    QCommandLineOption homeToken(QStringLiteral("home-token"), QStringLiteral("Bearer token for --home-sync."), QStringLiteral("token"));
    QCommandLineOption homeImport(QStringLiteral("home-import"), QStringLiteral("Merge the patterns and BSSIDs of a home-networks.json (UniFi export) into the home networks, exit."), QStringLiteral("file"));
    QCommandLineOption knownImport(QStringLiteral("known-import"), QStringLiteral("Merge a known-devices.json (UniFi client export) into the known devices (by MAC), exit."), QStringLiteral("file"));
    QCommandLineOption knownList(QStringLiteral("known-list"), QStringLiteral("List the known (our) devices, exit."));
    QCommandLineOption knownAdd(QStringLiteral("known-add"), QStringLiteral("Add a known device: --known-add <mac> --known-name <name>, exit."), QStringLiteral("mac"));
    QCommandLineOption knownName(QStringLiteral("known-name"), QStringLiteral("Name for --known-add."), QStringLiteral("name"));
    QCommandLineOption knownRemove(QStringLiteral("known-remove"), QStringLiteral("Remove the known device <mac>, exit."), QStringLiteral("mac"));
    QCommandLineOption dbStats(QStringLiteral("db-stats"), QStringLiteral("Print the internal map database's statistics as JSON, exit."));
    QCommandLineOption dbExport(QStringLiteral("db-export"), QStringLiteral("Export the internal map database (JSON dump) to <file>, exit."), QStringLiteral("file"));
    QCommandLineOption dbImport(QStringLiteral("db-import"), QStringLiteral("Merge a JSON dump (from --db-export) into the internal map database, exit."), QStringLiteral("file"));
    QCommandLineOption importOpt(QStringLiteral("import"), QStringLiteral("Import your location history into the map database: Google Timeline.json / Records.json / Semantic Location History, WiGLE CSV, GPX, KML or a BeaconFix export (docs/DATABASE.md), exit."), QStringLiteral("file"));
    QCommandLineOption importFrom(QStringLiteral("from"), QStringLiteral("With --import: only entries at or after <date> (ISO 8601, e.g. 2024-01-01)."), QStringLiteral("date"));
    QCommandLineOption importTo(QStringLiteral("to"), QStringLiteral("With --import: only entries at or before <date>."), QStringLiteral("date"));
    QCommandLineOption importWhat(QStringLiteral("what"), QStringLiteral("With --import: what to take, comma-separated from positions,wifi,places (default all)."), QStringLiteral("list"));
    QCommandLineOption refit(QStringLiteral("refit"), QStringLiteral("Re-estimate every beacon's position from all its samples (least squares), print the count, exit."));
    QCommandLineOption sync(QStringLiteral("sync"), QStringLiteral("Sync samples, positions and stops with another BeaconFix: --sync <peer name | host | http://host:47822> [--sync-token <token>]; without a token our identity signs in when the peer shares or links it; exit."), QStringLiteral("peer"));
    QCommandLineOption noMdns(QStringLiteral("no-mdns"), QStringLiteral("With --tray: do not advertise this BeaconFix on the network (mDNS); browsing for others still works. Test instances with a non-default XDG_CONFIG_HOME never advertise."));
    QCommandLineOption peers(QStringLiteral("peers"), QStringLiteral("List the BeaconFix devices on this network (mDNS), exit."));
    QCommandLineOption scan(QStringLiteral("scan"), QStringLiteral("With --peers: also probe every host of the local /24 networks for the API (for networks that block multicast)."));
    QCommandLineOption allPeers(QStringLiteral("all"), QStringLiteral("With --peers: include this computer's own records (debugging several instances on one host)."));
    QCommandLineOption identity(QStringLiteral("identity"), QStringLiteral("Show this BeaconFix's identity (id, name, devices, links, pending link requests), exit."));
    QCommandLineOption identityNew(QStringLiteral("identity-new"), QStringLiteral("Create a new identity called <name> (Ed25519 key pair sealed with the map-database key), exit."), QStringLiteral("name"));
    QCommandLineOption identityExport(QStringLiteral("identity-export"), QStringLiteral("Export the identity as an encrypted BFID1 bundle (text + QR if qrencode is installed); asks for a passphrase or use --words, exit."));
    QCommandLineOption identityFile(QStringLiteral("file"), QStringLiteral("With --identity-export: also write the bundle to <file>."), QStringLiteral("file"));
    QCommandLineOption identityWords(QStringLiteral("words"), QStringLiteral("With --identity-export: protect the bundle with a generated 6-word code (shown once) instead of asking for a passphrase."));
    QCommandLineOption identityPass(QStringLiteral("passphrase"), QStringLiteral("Passphrase for --identity-export / --identity-import (otherwise asked on the terminal)."), QStringLiteral("text"));
    QCommandLineOption identityImport(QStringLiteral("identity-import"), QStringLiteral("Import an identity from <file> or a BFID1: text (asks for the passphrase / word code), exit."), QStringLiteral("file-or-text"));
    QCommandLineOption identityLinkQr(QStringLiteral("identity-link-qr"), QStringLiteral("Print our link payload (beaconfix://link/… text + QR, from the running tray) for another identity to scan and co-sign, exit."));
    QCommandLineOption identitySelftest(QStringLiteral("identity-selftest"), QStringLiteral("Print the fixed-seed test vectors (seed = 32×0x01), exit."));
    QCommandLineOption nearby(QStringLiteral("nearby"), QStringLiteral("List places near the fix: <what> is a category (police, fire, health, urgent, peds_er, peds_urgent, pharmacy, library, playground, park, dogpark, pool, …), a group (civic, kids, services), 'pediatric' for pediatric ERs and urgent care, 'emergency' for the nearest help, or 'all'; exit."), QStringLiteral("what"));
    QCommandLineOption radius(QStringLiteral("radius"), QStringLiteral("With --nearby: only places within <km>."), QStringLiteral("km"));
    QCommandLineOption tz(QStringLiteral("tz"), QStringLiteral("Print the IANA time zone for the current fix, exit."));
    QCommandLineOption applyOs(QStringLiteral("apply-os"), QStringLiteral("Apply the OS integration now (time zone, GeoClue, Night Light) and print the report, exit."));
    QCommandLineOption dryRun(QStringLiteral("dry-run"), QStringLiteral("With --apply-os: only report what would change."));
    QCommandLineOption syncToken(QStringLiteral("sync-token"), QStringLiteral("Bearer token for --sync (remembered for that peer once given)."), QStringLiteral("token"));
    QCommandLineOption anchorsOpt(QStringLiteral("anchors"), QStringLiteral("Print the anchors (surveyed transmitters / places, docs/RANGING.md §4) as JSON, exit."));
    QCommandLineOption anchorSet(QStringLiteral("anchor-set"), QStringLiteral("Create or update an anchor from <json> or b64:<standard base64 of the UTF-8 JSON>; prints its id, exit."), QStringLiteral("json"));
    QCommandLineOption anchorRemove(QStringLiteral("anchor-remove"), QStringLiteral("Remove the anchor <id>, exit."), QStringLiteral("id"));
    QCommandLineOption grantControl(QStringLiteral("grant-control"), QStringLiteral("Add the control scope to the paired device <name-or-id>'s existing token (no new token), exit."), QStringLiteral("name-or-id"));
    QCommandLineOption rangingOpt(QStringLiteral("ranging"), QStringLiteral("Print the device ranging state (responder, BLE, every ranged device) as JSON, exit."));
    QCommandLineOption rangingCal(QStringLiteral("ranging-calibrate"), QStringLiteral("Calibrate ranging with a device lying at a known distance: <device>@<metres>[@<seconds>], e.g. \"Pixel 10@0.61\", exit."), QStringLiteral("device@metres"));
    p.addOptions({anchorsOpt, anchorSet, anchorRemove, grantControl, rangingOpt, rangingCal});
    p.addOptions({tray, once, json, refresh, snapshot, gpx, copy, newTrip, prefetch, apiStatus, devices, approve, deny, revoke, token, control, pairing,
                  homeAdd, homeRemove, homeList, homeSync, homeToken, homeImport, knownImport, knownList, knownAdd, knownName, knownRemove, dbStats, dbExport, dbImport, importOpt, importFrom, importTo, importWhat, refit, sync, syncToken, noMdns, peers, scan, allPeers,
                  identity, identityNew, identityExport, identityFile, identityWords, identityPass, identityImport, identityLinkQr, identitySelftest, tz, applyOs, dryRun, nearby, radius});
    p.process(app);

    QTextStream out(stdout);
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (p.isSet(noMdns)) qputenv("BEACONFIX_NO_MDNS", "1");

    if (p.isSet(identitySelftest)) { out << QJsonDocument(Identity::selftest()).toJson(QJsonDocument::Indented); out.flush(); return 0; }

    // ── identity: works on the file directly (sealed with the map-database key); a running tray is told to reload ──
    if (p.isSet(identity) || p.isSet(identityNew) || p.isSet(identityExport) || p.isSet(identityImport) || p.isSet(identityLinkQr)) {
        Locator loc(true);
        Identity *idn = loc.identity();
        if (!idn->unlocked() && loc.mapDb() && loc.mapDb()->key().isEmpty()) {   // first use before the tray ever ran: make the key now
            QString src; const QByteArray k = MapDb::bootstrapKey(&src);
            if (!k.isEmpty()) idn->load(k);
        }
        auto reloadTray = [&] { QDBusInterface iface(SVC, PATH, SVC, bus); if (iface.isValid()) iface.call(QStringLiteral("IdentityReload")); };
        auto askPass = [&](const QString &prompt) -> QString {
            if (p.isSet(identityPass)) return p.value(identityPass);
            fprintf(stderr, "%s", qPrintable(prompt)); fflush(stderr);
            QTextStream in(stdin); return in.readLine().trimmed();
        };
        auto qr = [&](const QString &text) {
            const QString bin = QStandardPaths::findExecutable(QStringLiteral("qrencode"));
            if (bin.isEmpty()) { out << "(install qrencode to see a QR here)\n"; return; }
            QProcess q; q.start(bin, {QStringLiteral("-t"), QStringLiteral("ANSIUTF8"), QStringLiteral("-m"), QStringLiteral("1"), text});
            q.waitForFinished(5000); out << QString::fromUtf8(q.readAllStandardOutput());
        };
        int rc = 0;
        if (p.isSet(identityNew)) {
            if (idn->exists()) { fprintf(stderr, "beaconfix: an identity already exists (%s). Export it or --identity first; forget it from the app to replace it.\n", qPrintable(idn->groupedId())); return 1; }
            QString err;
            if (!idn->create(p.value(identityNew), QHostInfo::localHostName(), QStringLiteral("desktop"), &err)) { fprintf(stderr, "beaconfix: %s\n", qPrintable(err)); return 1; }
            out << "Identity created: " << idn->name() << "  " << idn->groupedId() << "\n";
            reloadTray();
        }
        if (p.isSet(identityImport)) {
            QString text = p.value(identityImport).trimmed();
            if (QFile::exists(text)) { QFile f(text); if (f.open(QIODevice::ReadOnly)) text = QString::fromUtf8(f.readAll()).trimmed(); }
            const QString pass = askPass(QStringLiteral("Passphrase / word code for the bundle: "));
            QString err;
            if (!idn->importBundle(text, pass, QHostInfo::localHostName(), QStringLiteral("desktop"), &err)) { fprintf(stderr, "beaconfix: import failed: %s\n", qPrintable(err)); return 1; }
            out << "Identity imported: " << idn->name() << "  " << idn->groupedId() << "\n";
            reloadTray();
        }
        if (p.isSet(identityExport)) {
            if (!idn->unlocked()) { fprintf(stderr, "beaconfix: no unlocked identity here (create one with --identity-new <name>)\n"); return 1; }
            QString pass;
            if (p.isSet(identityWords)) { pass = Identity::wordCode(); out << "Word code (shown once — the other device needs it):\n\n    " << pass << "\n\n"; }
            else pass = askPass(QStringLiteral("Passphrase to protect the bundle (8+ characters): "));
            QString err; const QString bundle = idn->exportBundle(pass, &err);
            if (bundle.isEmpty()) { fprintf(stderr, "beaconfix: %s\n", qPrintable(err)); return 1; }
            if (p.isSet(identityFile)) { QFile f(p.value(identityFile)); if (f.open(QIODevice::WriteOnly)) { f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); f.write(bundle.toUtf8() + "\n"); out << "Written to " << p.value(identityFile) << "\n"; } else { fprintf(stderr, "beaconfix: cannot write %s\n", qPrintable(p.value(identityFile))); rc = 1; } }
            out << bundle << "\n\n"; qr(bundle);
        }
        if (p.isSet(identityLinkQr)) {
            if (!idn->exists()) { fprintf(stderr, "beaconfix: no identity here\n"); return 1; }
            // The tray co-signs only statements bound to an offer IT displayed, so the payload must come from the tray when it runs
            QString payload;
            if (bus.isConnected() && bus.interface() && bus.interface()->isServiceRegistered(SVC)) {
                QDBusInterface iface(SVC, PATH, SVC, bus);
                QDBusReply<QString> r = iface.call(QStringLiteral("IdentityLinkPayload"));
                if (r.isValid()) payload = r.value();
                if (payload.isEmpty()) { fprintf(stderr, "beaconfix: the running BeaconFix has no identity (or could not answer)\n"); return 1; }
                out << "(offer registered with the running BeaconFix; valid 10 minutes)\n";
            } else {
                payload = Identity::encodeUri(QStringLiteral("link"), idn->linkPayload());
                out << "(no BeaconFix running: this offer is only valid for links completed by this command's process — start the tray and run again to link a device)\n";
            }
            out << payload << "\n\n"; qr(payload);
        }
        if (p.isSet(identity) || rc == 0) {
            if (!idn->exists()) { out << "No identity on this BeaconFix yet. Create one: beaconfix --identity-new \"Your name\"  (or import a bundle)\n"; }
            else {
                out << "Identity: " << idn->name() << "\n  id:       " << idn->groupedId() << "\n  pub:      " << idn->pub().toBase64() << "\n  created:  " << idn->created().toString(Qt::ISODate)
                    << "\n  key:      " << (idn->unlocked() ? "unlocked (sealed with the map-database key)" : "LOCKED (map-database key missing or changed)") << "\n  devices:  ";
                QStringList devs; for (const IdentityDevice &d : idn->devices()) devs << d.name + " (" + d.kind + ")"; out << devs.join(", ") << "\n";
                const QStringList linked = idn->linkedIds(); out << "  linked:   " << (linked.isEmpty() ? QStringLiteral("none") : linked.join(", ")) << "\n";
                if (!idn->pending().isEmpty()) { out << "  pending link requests:\n"; for (const PendingLink &pl : idn->pending()) out << "    " << Identity::groupId(pl.id) << "  " << pl.deviceName << " (" << pl.deviceKind << ") from " << pl.ip << "  " << pl.time.toString(Qt::ISODate) << "\n"; }
                out << "  file:     " << Identity::filePath() << "\n";
            }
        }
        out.flush();
        return rc;
    }

    if (p.isSet(nearby)) {
        QJsonObject st;
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (iface.isValid()) { QDBusReply<QString> r = iface.call(QStringLiteral("StateJson")); st = QJsonDocument::fromJson(r.value().toUtf8()).object(); }
        else { Locator loc(true); st = QJsonDocument::fromJson(loc.StateJson().toUtf8()).object(); }
        const QString what = p.value(nearby).trimmed().toLower();
        const double km = p.isSet(radius) ? p.value(radius).toDouble() : 0;
        auto fmt = [](const QJsonObject &q) {
            QString line = QStringLiteral("%1 %2").arg(q["icon"].toString(), q["name"].toString().isEmpty() ? q["label"].toString() : q["name"].toString());
            const int tier = q.contains("tier") ? q["tier"].toInt() : q["peds"].toInt();
            if (tier > 0) line += QStringLiteral("  [tier %1: %2]").arg(tier).arg(PoiClassify::tierLabel(tier));
            if (!q["campusEr"].toString().isEmpty()) line += QStringLiteral("  ·  ER on campus: ") + q["campusEr"].toString();
            if (q.contains("d")) line += QStringLiteral("  ·  %1 km %2").arg(q["d"].toDouble() / 1000.0, 0, 'f', 1).arg(Locator::compass(q["brg"].toDouble()));
            if (q["driveS"].toInt() > 0) {
                const int min = q["driveS"].toInt() / 60;
                line += QStringLiteral("  ·  ~%1%2").arg(min < 60 ? QStringLiteral("%1 min").arg(min) : min % 60 ? QStringLiteral("%1 h %2 min").arg(min / 60).arg(min % 60) : QStringLiteral("%1 h").arg(min / 60),
                                                   q["driveEst"].toBool(true) ? QStringLiteral(" (est.)") : QString());
            }
            if (!q["phone"].toString().isEmpty()) line += QStringLiteral("  ·  ☎ ") + q["phone"].toString();
            if (!q["address"].toString().isEmpty()) line += QStringLiteral("  ·  ") + q["address"].toString();
            if (!q["hours"].toString().isEmpty()) line += QStringLiteral("  ·  🕑 ") + q["hours"].toString();
            return line;
        };
        if (what == QLatin1String("emergency")) {
            const QJsonObject e = st["emergency"].toObject();
            out << "Emergency number here: " << e["number"].toString() << (e["countryCode"].toString().isEmpty() ? "" : "  (" + e["countryCode"].toString().toUpper() + ")") << "\n";
            for (const char *k : {"police", "fire", "hospital", "urgent", "pharmacy", "vet"}) {
                const QJsonValue v = e[k];
                if (v.isNull() || v.isUndefined()) { out << "  " << k << ": none mapped nearby\n"; continue; }
                QJsonObject q = v.toObject(); q["label"] = QString::fromLatin1(k); q["icon"] = QString();
                out << "  " << QString::fromLatin1(k).leftJustified(9) << fmt(q).trimmed() << "\n";
            }
            if (e.contains("pediatric")) {                                // a 3.8+ tray (or this binary standalone)
                const QList<QPair<const char *, QString>> peds{{"pediatric", QStringLiteral("Pediatric ER")}, {"pediatricCloser", QStringLiteral("Closer")},
                                                               {"pediatricUrgent", QStringLiteral("Pediatric urgent care (not an ER)")}};
                for (const auto &k : peds) {
                    const QJsonValue v = e[QLatin1String(k.first)];
                    if (v.isNull() || v.isUndefined()) {
                        if (qstrcmp(k.first, "pediatric") == 0) {
                            if (e["pediatricTime"].toString().isEmpty()) out << "  " << k.second << ": not searched yet\n";
                            else out << "  " << k.second << ": none mapped within " << e["pediatricSearchKm"].toInt() << " km\n";
                        }
                        continue;
                    }
                    QJsonObject q = v.toObject(); q["icon"] = QString();
                    out << "  " << k.second << ":  " << fmt(q).trimmed() << "\n";
                }
                if (!e["pediatricNote"].toString().isEmpty()) out << "  " << e["pediatricNote"].toString() << "\n";
            }
            out.flush(); return 0;
        }
        int n = 0;
        QList<QJsonObject> rows;
        const bool pediatric = what == QLatin1String("pediatric") || what == QLatin1String("peds");
        for (const QJsonValue &v : st["pois"].toArray()) {
            const QJsonObject q = v.toObject();
            const QString cat = q["cat"].toString().toLower();
            if (pediatric ? !(cat == QLatin1String("peds_er") || cat == QLatin1String("peds_urgent"))
                          : !(what == QLatin1String("all") || what == cat || what == q["group"].toString().toLower())) continue;
            if (km > 0 && q.contains("d") && q["d"].toDouble() > km * 1000) continue;
            rows << q;
        }
        std::sort(rows.begin(), rows.end(), [](const QJsonObject &a, const QJsonObject &b) { return a["d"].toDouble() < b["d"].toDouble(); });
        for (const QJsonObject &q : rows) { out << fmt(q) << "\n"; ++n; }
        if (n == 0) out << "No places matching '" << what << "'" << (km > 0 ? QStringLiteral(" within %1 km").arg(km) : QString()) << ". Categories: police fire health urgent peds_er peds_urgent pharmacy dentist vet library townhall court dmv school community playground park dogpark pool splash zoo museum themepark icecream cinema bowling arcade trampoline skate beach picnic trail fuel propane charging grocery food cafe camp water dump shower toilets laundry repair hardware wifi post rest carwash; groups: civic kids services; or pediatric / emergency / all.\n";
        out.flush(); return n ? 0 : 1;
    }

    if (p.isSet(tz) || p.isSet(applyOs)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (iface.isValid()) {
            if (p.isSet(applyOs)) { QDBusReply<QString> r = iface.call(QStringLiteral("ApplyOs"), p.isSet(dryRun)); out << (r.isValid() ? r.value() : QStringLiteral("{\"error\":\"call failed\"}")) << "\n"; }
            if (p.isSet(tz)) { QDBusReply<QString> r = iface.call(QStringLiteral("TimeZoneForFix")); out << (r.isValid() && !r.value().isEmpty() ? r.value() : QStringLiteral("(no zone resolved yet)")) << "\n"; }
            out.flush(); return 0;
        }
        Locator loc(true);
        if (p.isSet(applyOs)) { fprintf(stderr, "beaconfix: --apply-os needs the running instance\n"); return 1; }
        const QString z = loc.TimeZoneForFix();
        out << (z.isEmpty() ? QStringLiteral("(no fix / no zone)") : z) << "\n"; out.flush();
        return z.isEmpty() ? 1 : 0;
    }

    if (p.isSet(importOpt)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (!iface.isValid()) { fprintf(stderr, "beaconfix: --import needs the running instance (beaconfix --tray)\n"); return 1; }
        const QString file = QFileInfo(p.value(importOpt)).absoluteFilePath();
        if (!QFileInfo::exists(file)) { fprintf(stderr, "beaconfix: no such file: %s\n", qPrintable(file)); return 1; }
        QJsonObject opts;
        if (p.isSet(importFrom)) opts["from"] = p.value(importFrom);
        if (p.isSet(importTo)) opts["to"] = p.value(importTo);
        if (p.isSet(importWhat)) opts["what"] = QJsonArray::fromStringList(p.value(importWhat).split(QLatin1Char(','), Qt::SkipEmptyParts));
        for (const QString &d : {p.value(importFrom), p.value(importTo)}) if (!d.isEmpty() && !QDateTime::fromString(d, Qt::ISODate).isValid() && !QDate::fromString(d, Qt::ISODate).isValid()) { fprintf(stderr, "beaconfix: bad date: %s (use ISO 8601)\n", qPrintable(d)); return 1; }
        if (opts["from"].isString() && QDate::fromString(opts["from"].toString(), Qt::ISODate).isValid()) opts["from"] = QDate::fromString(opts["from"].toString(), Qt::ISODate).startOfDay().toString(Qt::ISODate);
        if (opts["to"].isString() && QDate::fromString(opts["to"].toString(), Qt::ISODate).isValid()) opts["to"] = QDate::fromString(opts["to"].toString(), Qt::ISODate).endOfDay().toString(Qt::ISODate);
        // Progress arrives as a signal while the (long) call is in flight
        const bool tty = isatty(2);
        ImportProgressPrinter printer(tty, file);
        bus.connect(SVC, PATH, SVC, QStringLiteral("importProgress"), &printer, SLOT(onProgress(QString)));
        iface.setTimeout(30 * 60 * 1000);
        QDBusPendingCall call = iface.asyncCall(QStringLiteral("Import"), file, QString::fromUtf8(QJsonDocument(opts).toJson(QJsonDocument::Compact)));
        QEventLoop loop;
        QDBusPendingCallWatcher w(call);
        QObject::connect(&w, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        loop.exec();
        QDBusPendingReply<QString> r = call;
        if (tty) fprintf(stderr, "\r\033[K");
        if (!r.isValid()) { fprintf(stderr, "beaconfix: import failed: %s\n", qPrintable(r.error().message())); return 1; }
        const QJsonObject o = QJsonDocument::fromJson(r.value().toUtf8()).object(), sum = o["summary"].toObject();
        if (!o["ok"].toBool()) { fprintf(stderr, "beaconfix: import failed: %s\n", qPrintable(o["error"].toString())); return 1; }
        if (p.isSet(json)) { out << QJsonDocument(sum).toJson(QJsonDocument::Indented); out.flush(); return 0; }
        out << "Imported " << QFileInfo(file).fileName() << " (" << sum["format"].toString() << ", " << QLocale().formattedDataSize(qint64(sum["bytes"].toDouble())) << ") in " << QString::number(sum["seconds"].toDouble(), 'f', 1) << " s\n"
            << "  positions " << sum["positions"].toInt() << " · track points " << sum["tracks"].toInt() << " · Wi-Fi scans " << sum["wifiScans"].toInt() << " → observations " << sum["observations"].toInt()
            << " on " << sum["beaconsTouched"].toInt() << " beacons · visits " << sum["visits"].toInt() << " · skipped " << sum["skipped"].toInt() << (sum["errors"].toInt() ? QStringLiteral(" · errors %1").arg(sum["errors"].toInt()) : QString()) << "\n";
        if (sum["first"].isString()) out << "  from " << sum["first"].toString().left(10) << " to " << sum["last"].toString().left(10) << " (device \"" << sum["device"].toString() << "\"; positions become history only, never the live fix)\n";
        out.flush();
        return 0;
    }

    if (p.isSet(once)) {
        Locator loc(true);
        QObject::connect(&loc, &Locator::probeFinished, &app, [&](bool ok, const QString &msg) {
            if (!ok) {
                // Probe failed: hand back the last good fix (if any) rather than nothing
                QJsonObject o = loc.fix().valid ? loc.fix().toJson() : QJsonObject{{"valid", false}, {"source", "none"}};
                o["probeError"] = msg;
                out << QJsonDocument(o).toJson(QJsonDocument::Compact) << "\n";
                out.flush();
                QCoreApplication::quit();
                return;
            }
            // Wait (briefly) for the Nominatim place name so callers get a label, not raw coordinates
            auto *poll = new QTimer(&app);
            auto *waited = new int(0);
            QObject::connect(poll, &QTimer::timeout, &app, [&, poll, waited] {
                *waited += 200;
                if (loc.geocodePending() && *waited < 6000) return;
                poll->stop();
                QJsonObject o = loc.fix().toJson();          // the accepted fix, not a coarse candidate
                o["probeSource"] = loc.lastProbe().source;
                o["probeAccuracy"] = loc.lastProbe().accuracy;
                if (!loc.coarseNote().isEmpty()) o["note"] = loc.coarseNote();
                QJsonArray ev; for (const BeaconEvent &e : loc.events()) ev.append(e.toJson()); o["events"] = ev;
                out << QJsonDocument(o).toJson(QJsonDocument::Compact) << "\n";
                out.flush();
                QCoreApplication::quit();
            });
            poll->start(200);
        });
        loc.Refresh();
        return app.exec();
    }

    if (p.isSet(snapshot)) {
        Locator loc(true);
        TileSource tiles;
        MainWindow win(&loc, &tiles);
        win.resize(1100, 720);
        win.show();
        const QString file = p.value(snapshot);
        QObject::connect(&loc, &Locator::probeFinished, &app, [&, file](bool, const QString &) {
            // Let tiles, the place name and nearby places arrive (places can take a while)
            auto *poll = new QTimer(&app);
            auto *waited = new int(0);
            QObject::connect(poll, &QTimer::timeout, &app, [&, file, poll, waited] {
                *waited += 500;
                if (*waited < 7000 || (loc.poisLoading() && *waited < 45000)) return;
                poll->stop();
                // Test hooks: BEACONFIX_SNAPSHOT_ZOOM=17 BEACONFIX_SNAPSHOT_LAYER=0..3
                bool ok = false;
                const double z = qEnvironmentVariable("BEACONFIX_SNAPSHOT_ZOOM").toDouble(&ok);
                if (ok && loc.fix().valid) win.map()->focusOn(loc.fix().lat, loc.fix().lon, z);
                const int layer = qEnvironmentVariable("BEACONFIX_SNAPSHOT_LAYER").toInt(&ok);
                if (ok) win.map()->setLayer(BeaconView::Layer(layer));
                QTimer::singleShot(ok || qEnvironmentVariableIsSet("BEACONFIX_SNAPSHOT_ZOOM") ? 9000 : 4000, &app, [&, file] {  // tiles for the final view
                    win.grab().save(file);
                    QCoreApplication::quit();
                });
            });
            poll->start(500);
        });
        loc.Refresh();
        return app.exec();
    }

    // ── anchors / ranging / grant-control: through the running tray (D-Bus activation starts it) ──
    if (p.isSet(anchorsOpt) && !p.isSet(anchorSet) && !p.isSet(anchorRemove)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (iface.isValid()) { QDBusReply<QString> r = iface.call(QStringLiteral("Anchors")); if (r.isValid()) { out << QJsonDocument::fromJson(r.value().toUtf8()).toJson(QJsonDocument::Indented); out.flush(); return 0; } }
        Locator loc(true);
        out << QJsonDocument(loc.anchorsJson()).toJson(QJsonDocument::Indented); out.flush();
        return 0;
    }
    if (p.isSet(anchorSet) || p.isSet(anchorRemove) || p.isSet(grantControl) || p.isSet(rangingOpt) || p.isSet(rangingCal)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (!iface.isValid()) { fprintf(stderr, "beaconfix: no running instance and D-Bus activation failed\n"); return 1; }
        int rc = 0;
        if (p.isSet(anchorSet)) {
            QString arg = p.value(anchorSet).trimmed();
            if (arg.startsWith(QLatin1String("b64:"))) {
                const auto dec = QByteArray::fromBase64Encoding(arg.mid(4).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
                if (!dec) { fprintf(stderr, "beaconfix: --anchor-set: invalid base64\n"); return 1; }
                arg = QString::fromUtf8(*dec);
            } else if (QFile::exists(arg)) { QFile f(arg); if (f.open(QIODevice::ReadOnly)) arg = QString::fromUtf8(f.readAll()); }
            // A JSON array sets several at once
            const QJsonDocument d = QJsonDocument::fromJson(arg.toUtf8());
            QList<QJsonObject> items;
            if (d.isArray()) for (const QJsonValue &v : d.array()) items << v.toObject(); else items << d.object();
            if (!d.isArray() && !d.isObject()) { fprintf(stderr, "beaconfix: --anchor-set: not JSON\n"); return 1; }
            for (const QJsonObject &o : items) {
                QDBusReply<QString> r = iface.call(QStringLiteral("SetAnchor"), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
                if (!r.isValid()) { fprintf(stderr, "beaconfix: %s\n", qPrintable(r.error().message())); rc = 1; continue; }
                if (r.value().startsWith(QLatin1String("error:"))) { fprintf(stderr, "beaconfix: %s\n", qPrintable(r.value())); rc = 1; continue; }
                out << r.value() << "\n";
            }
        }
        if (p.isSet(anchorRemove)) { QDBusReply<bool> r = iface.call(QStringLiteral("RemoveAnchor"), p.value(anchorRemove)); out << (r.isValid() && r.value() ? "removed\n" : "no such anchor\n"); if (!(r.isValid() && r.value())) rc = 1; }
        if (p.isSet(grantControl)) {
            QDBusReply<bool> r = iface.call(QStringLiteral("GrantControl"), p.value(grantControl));
            out << (r.isValid() && r.value() ? QStringLiteral("control granted to %1\n").arg(p.value(grantControl)) : QStringLiteral("no such device\n"));
            if (!(r.isValid() && r.value())) rc = 1;
        }
        if (p.isSet(rangingCal)) {
            const QStringList parts = p.value(rangingCal).split(QLatin1Char('@'));
            if (parts.size() < 2) { fprintf(stderr, "beaconfix: --ranging-calibrate <device>@<metres>[@<seconds>]\n"); return 1; }
            QDBusReply<QString> r = iface.call(QStringLiteral("RangingCalibrate"), parts[0], parts[1].toDouble(), parts.size() > 2 ? parts[2].toInt() : 20);
            out << (r.isValid() ? r.value() : r.error().message()) << "\n";
        }
        if (p.isSet(rangingOpt)) {
            QDBusReply<QString> info = iface.call(QStringLiteral("RangingInfo")), list = iface.call(QStringLiteral("Ranging"));
            QJsonObject o = QJsonDocument::fromJson(list.value().toUtf8()).object();
            o["info"] = QJsonDocument::fromJson(info.value().toUtf8()).object();
            out << QJsonDocument(o).toJson(QJsonDocument::Indented);
        }
        if (p.isSet(anchorsOpt)) { QDBusReply<QString> r = iface.call(QStringLiteral("Anchors")); out << QJsonDocument::fromJson(r.value().toUtf8()).toJson(QJsonDocument::Indented); }
        out.flush();
        return rc;
    }

    if (p.isSet(json)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (iface.isValid()) {
            QDBusReply<QString> r = iface.call(QStringLiteral("StateJson"));
            if (r.isValid()) { out << r.value() << "\n"; return 0; }
        }
        Locator loc(true);
        out << loc.StateJson() << "\n";
        return 0;
    }

    if (p.isSet(gpx)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (iface.isValid()) {
            QDBusReply<bool> r = iface.call(QStringLiteral("ExportGpx"), p.value(gpx));
            if (r.isValid()) return r.value() ? 0 : 1;
        }
        Locator loc(true);
        QString err;
        if (!loc.exportGpx(p.value(gpx), &err)) { fprintf(stderr, "beaconfix: %s\n", qPrintable(err)); return 1; }
        return 0;
    }

    if (p.isSet(copy)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);    // the running instance owns the clipboard (it outlives us)
        if (iface.isValid()) { QDBusReply<bool> r = iface.call(QStringLiteral("CopyToClipboard"), p.value(copy)); return r.isValid() && r.value() ? 0 : 1; }
        fprintf(stderr, "beaconfix: no running instance and D-Bus activation failed\n");
        return 1;
    }

    if (p.isSet(newTrip) || p.isSet(prefetch)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (!iface.isValid()) { fprintf(stderr, "beaconfix: no running instance and D-Bus activation failed\n"); return 1; }
        if (p.isSet(newTrip)) iface.call(QStringLiteral("StartTrip"));
        if (p.isSet(prefetch)) iface.call(QStringLiteral("PrefetchTiles"));
        return 0;
    }

    if (p.isSet(peers)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        QJsonObject o;
        if (iface.isValid() && !p.isSet(allPeers)) { iface.setTimeout(20000); QDBusReply<QString> r = iface.call(QStringLiteral("Peers"), p.isSet(scan)); o = QJsonDocument::fromJson(r.value().toUtf8()).object(); }
        else {                                               // no tray (or --all): browse for a moment ourselves
            Mdns m; QEventLoop loop; QTimer::singleShot(2500, &loop, &QEventLoop::quit);
            if (p.isSet(scan)) m.scanSubnets(47822, [&loop] { loop.quit(); });
            loop.exec();
            const QJsonArray arr = m.peersJson(p.isSet(allPeers));
            o = QJsonObject{{"peers", arr}, {"count", arr.size()}, {"scanned", p.isSet(scan)}, {"mdns", m.available()}};
        }
        const QJsonArray arr = o["peers"].toArray();
        if (arr.isEmpty()) { out << (o["mdns"].toBool() || iface.isValid() ? "No other BeaconFix on this network" : "No other BeaconFix found (Avahi not reachable — try --scan)") << "\n"; out.flush(); return 0; }
        out << arr.size() << " BeaconFix device(s) on this network" << (o["scanned"].toBool() ? " (mDNS + scan)" : "") << ":\n";
        for (const QJsonValue &v : arr) {
            const QJsonObject q = v.toObject();
            QString rel = q["self"].toBool() ? QStringLiteral("this computer") : q["sameIdentity"].toBool() ? QStringLiteral("same identity") : q["linked"].toBool() ? QStringLiteral("linked identity") : q["identityId"].toString().isEmpty() ? QStringLiteral("no identity") : QStringLiteral("other identity");
            out << QStringLiteral("  %1  %2  %3  v%4 %5  %6%7\n").arg(q["host"].toString(), -18).arg(q["url"].toString(), -34).arg(q["identityName"].toString().isEmpty() ? QStringLiteral("—") : q["identityName"].toString(), -16)
                       .arg(q["version"].toString(), q["kind"].toString(), rel, q["pairing"].toBool() ? QStringLiteral(" · pairing open") : QString());
            QStringList more; for (const QJsonValue &a : q["addresses"].toArray()) more << a.toString();
            if (more.size() > 1) out << "      also: " << more.mid(1).join(QStringLiteral(", ")) << "\n";
        }
        out.flush();
        return 0;
    }

    if (p.isSet(refit) || p.isSet(sync)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (!iface.isValid()) { fprintf(stderr, "beaconfix: %s needs the running instance\n", p.isSet(refit) ? "--refit" : "--sync"); return 1; }
        if (p.isSet(sync) && p.value(sync).trimmed().isEmpty()) { fprintf(stderr, "beaconfix: --sync needs a peer (name, host or URL); see --peers\n"); return 2; }
        int rc = 0;
        if (p.isSet(refit)) { QDBusReply<int> r = iface.call(QStringLiteral("Refit")); if (!r.isValid()) { fprintf(stderr, "beaconfix: refit failed\n"); rc = 1; } else out << "Positioned " << r.value() << " beacon(s) from their samples\n"; }
        if (p.isSet(sync)) {
            iface.setTimeout(200000);
            QDBusReply<QString> r = iface.call(QStringLiteral("Sync"), p.value(sync).trimmed(), p.value(syncToken).trimmed());
            const QJsonObject o = QJsonDocument::fromJson(r.value().toUtf8()).object();
            if (!r.isValid() || !o["ok"].toBool()) { fprintf(stderr, "beaconfix: sync failed: %s\n", qPrintable(o["message"].toString().isEmpty() ? o["error"].toString() : o["message"].toString())); rc = 1; }
            else out << "Synced with " << o["peer"].toString() << ": " << o["message"].toString() << "\n";
        }
        out.flush();
        return rc;
    }
    if (p.isSet(dbStats) || p.isSet(dbExport) || p.isSet(dbImport)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (iface.isValid()) {
            if (p.isSet(dbImport)) { QDBusReply<int> r = iface.call(QStringLiteral("DbImport"), QFileInfo(p.value(dbImport)).absoluteFilePath()); if (!r.isValid() || r.value() < 0) { fprintf(stderr, "beaconfix: import failed\n"); return 1; } out << "Imported " << r.value() << " row(s)\n"; }
            if (p.isSet(dbExport)) { QDBusReply<bool> r = iface.call(QStringLiteral("DbExport"), QFileInfo(p.value(dbExport)).absoluteFilePath()); if (!r.isValid() || !r.value()) { fprintf(stderr, "beaconfix: export failed\n"); return 1; } out << "Exported to " << p.value(dbExport) << "\n"; }
            if (p.isSet(dbStats)) { QDBusReply<QString> r = iface.call(QStringLiteral("DbStats")); out << r.value() << "\n"; }
            out.flush();
            return 0;
        }
        Locator loc(true);                                   // no tray: read-only copy of the database
        if (p.isSet(dbImport)) { fprintf(stderr, "beaconfix: --db-import needs the running instance\n"); return 1; }
        if (p.isSet(dbExport) && !loc.DbExport(QFileInfo(p.value(dbExport)).absoluteFilePath())) { fprintf(stderr, "beaconfix: export failed: %s\n", qPrintable(loc.mapDb() ? loc.mapDb()->error() : QString())); return 1; }
        if (p.isSet(dbStats)) out << loc.DbStats() << "\n";
        out.flush();
        return 0;
    }

    if (p.isSet(knownImport) || p.isSet(knownList) || p.isSet(knownAdd) || p.isSet(knownRemove)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (!iface.isValid()) { fprintf(stderr, "beaconfix: no running instance and D-Bus activation failed\n"); return 1; }
        int rc = 0;
        if (p.isSet(knownImport)) { QDBusReply<int> r = iface.call(QStringLiteral("KnownImport"), QFileInfo(p.value(knownImport)).absoluteFilePath()); if (!r.isValid() || r.value() < 0) { fprintf(stderr, "beaconfix: import failed\n"); rc = 1; } else out << "Imported: " << r.value() << " new device(s)\n"; }
        if (p.isSet(knownAdd))    { QDBusReply<bool> r = iface.call(QStringLiteral("KnownAdd"), p.value(knownAdd), p.value(knownName)); out << (r.isValid() && r.value() ? "added\n" : "invalid MAC\n"); if (!(r.isValid() && r.value())) rc = 1; }
        if (p.isSet(knownRemove)) { QDBusReply<bool> r = iface.call(QStringLiteral("KnownRemove"), p.value(knownRemove)); out << (r.isValid() && r.value() ? "removed\n" : "no such device\n"); if (!(r.isValid() && r.value())) rc = 1; }
        if (p.isSet(knownList) || p.isSet(knownImport) || p.isSet(knownAdd) || p.isSet(knownRemove)) {
            QDBusReply<QString> r = iface.call(QStringLiteral("KnownDevices"));
            const QJsonObject o = QJsonDocument::fromJson(r.value().toUtf8()).object();
            out << "Known devices (" << o["count"].toInt() << ", allowlist " << (o["knownOnly"].toBool() ? "ON" : "off") << "):\n";
            for (const QJsonValue &v : o["devices"].toArray()) {
                const QJsonObject d = v.toObject();
                const QString ip = d["fixed_ip"].isString() ? d["fixed_ip"].toString() : d["ip"].toString();
                out << "  " << d["mac"].toString() << "  " << d["name"].toString().leftJustified(28) << (ip.isEmpty() ? QString() : ip).leftJustified(16)
                    << (d["online"].toBool() ? "online" : (d["last_seen"].isDouble() ? "seen " + QDateTime::fromSecsSinceEpoch(qint64(d["last_seen"].toDouble())).toString(QStringLiteral("d MMM yyyy")) : QString())) << "\n";
            }
        }
        out.flush();
        return rc;
    }

    if (p.isSet(homeAdd) || p.isSet(homeRemove) || p.isSet(homeList) || p.isSet(homeSync) || p.isSet(homeImport)) {
        // Through the running instance when there is one; else straight to the settings file
        QDBusInterface iface(SVC, PATH, SVC, bus);
        auto getList = [&]() -> QStringList {
            if (iface.isValid()) { QDBusReply<QStringList> r = iface.call(QStringLiteral("HomeNetworks")); if (r.isValid()) return r.value(); }
            return QSettings().value("homeNetworks").toStringList();
        };
        auto setList = [&](const QStringList &l) {
            if (iface.isValid()) { iface.call(QStringLiteral("SetHomeNetworks"), l); return; }
            QSettings().setValue("homeNetworks", l);
        };
        QStringList l = getList();
        if (p.isSet(homeSync)) {
            if (!p.isSet(homeToken)) { fprintf(stderr, "beaconfix: --home-sync needs --home-token <token>\n"); return 2; }
            QNetworkAccessManager nam;
            QNetworkRequest req(QUrl(p.value(homeSync).trimmed() + QStringLiteral("/api/v1/home")));
            req.setRawHeader("Authorization", "Bearer " + p.value(homeToken).trimmed().toUtf8());
            req.setTransferTimeout(10000);
            QNetworkReply *rep = nam.get(req);
            QEventLoop loop; QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit); loop.exec();
            const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
            if (rep->error() != QNetworkReply::NoError || !o["patterns"].isArray()) { fprintf(stderr, "beaconfix: sync failed: %s\n", qPrintable(rep->error() != QNetworkReply::NoError ? rep->errorString() : o["error"].toString())); return 1; }
            l.clear(); for (const QJsonValue &v : o["patterns"].toArray()) l << v.toString();
            setList(l);
            out << "Synced " << l.size() << " home network pattern(s) from " << p.value(homeSync) << "\n";
            if (o["homeLat"].isDouble()) out << "The RV was last fixed at " << o["homeLat"].toDouble() << ", " << o["homeLon"].toDouble() << " (" << o["homeTime"].toString() << ")\n";
        }
        if (p.isSet(homeImport)) {
            QString err; const QStringList imp = Locator::parseHomeNetworksFile(QFileInfo(p.value(homeImport)).absoluteFilePath(), &err);
            if (imp.isEmpty()) { fprintf(stderr, "beaconfix: %s\n", qPrintable(err)); return 1; }
            int added = 0; for (const QString &x : imp) if (!l.contains(x, Qt::CaseInsensitive)) { l << x; ++added; }
            setList(l); out << "Imported " << added << " new pattern(s) (" << imp.size() << " in the file)\n";
        }
        if (p.isSet(homeAdd)) { const QString v = p.value(homeAdd).trimmed(); if (!v.isEmpty() && !l.contains(v, Qt::CaseInsensitive)) l << v; setList(l); }
        if (p.isSet(homeRemove)) { QStringList k; for (const QString &x : l) if (x.compare(p.value(homeRemove).trimmed(), Qt::CaseInsensitive) != 0) k << x; l = k; setList(l); }
        if (p.isSet(homeList) || p.isSet(homeAdd) || p.isSet(homeRemove) || p.isSet(homeImport)) {
            out << "Home networks (" << l.size() << "):\n";
            for (const QString &x : l) out << "  " << x << "\n";
            if (iface.isValid()) {
                QDBusReply<QString> r = iface.call(QStringLiteral("StateJson"));
                const QJsonObject st = QJsonDocument::fromJson(r.value().toUtf8()).object()["stats"].toObject();
                out << (st["atHome"].toBool() ? "At home (a home network is in range)" : QStringLiteral("Not at home — %1").arg(st["awayText"].toString())) << "\n";
            }
        }
        out.flush();
        return 0;
    }

    if (p.isSet(apiStatus) || p.isSet(devices) || p.isSet(approve) || p.isSet(deny) || p.isSet(revoke) || p.isSet(token) || p.isSet(pairing)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (!iface.isValid()) { fprintf(stderr, "beaconfix: no running instance and D-Bus activation failed\n"); return 1; }
        int rc = 0;
        if (p.isSet(pairing)) {
            const int m = p.value(pairing).toInt();
            QDBusReply<bool> r = iface.call(QStringLiteral("OpenPairing"), m);
            if (!r.isValid() || !r.value()) { fprintf(stderr, "beaconfix: LAN API not available\n"); rc = 1; }
            else out << (m > 0 ? QStringLiteral("Pairing open for %1 min\n").arg(m) : QStringLiteral("Pairing closed\n"));
        }
        if (p.isSet(approve)) { QDBusReply<bool> r = iface.call(QStringLiteral("ApproveDevice"), p.value(approve)); out << (r.isValid() && r.value() ? "approved\n" : "no such pending request\n"); if (!(r.isValid() && r.value())) rc = 1; }
        if (p.isSet(deny))    { QDBusReply<bool> r = iface.call(QStringLiteral("DenyDevice"), p.value(deny)); out << (r.isValid() && r.value() ? "denied\n" : "no such pending request\n"); if (!(r.isValid() && r.value())) rc = 1; }
        if (p.isSet(revoke))  { QDBusReply<bool> r = iface.call(QStringLiteral("RevokeDevice"), p.value(revoke)); out << (r.isValid() && r.value() ? "revoked\n" : "no such device\n"); if (!(r.isValid() && r.value())) rc = 1; }
        if (p.isSet(token)) {
            QDBusReply<QString> r = iface.call(QStringLiteral("CreateToken"), p.value(token), p.isSet(control) ? QStringLiteral("read,control") : QStringLiteral("read"));
            if (!r.isValid() || r.value().isEmpty()) { fprintf(stderr, "beaconfix: could not create a token\n"); rc = 1; }
            else out << "Token for \"" << p.value(token) << "\" (shown once, store it now):\n" << r.value() << "\n";
        }
        if (p.isSet(apiStatus) || p.isSet(devices)) {
            QDBusReply<QString> r = iface.call(QStringLiteral("ApiStatus"));
            if (!r.isValid()) { fprintf(stderr, "beaconfix: could not read the API status\n"); return 1; }
            if (p.isSet(apiStatus)) out << r.value() << "\n";
            else {
                const QJsonObject st = QJsonDocument::fromJson(r.value().toUtf8()).object();
                out << "LAN API: " << (st["listening"].toBool() ? QStringLiteral("listening on port %1%2").arg(st["port"].toInt()).arg(st["tls"].toBool() ? " (TLS)" : "") : QStringLiteral("not listening — ") + st["error"].toString())
                    << " · pairing " << (st["pairingOpen"].toBool() ? "OPEN until " + st["pairingUntil"].toString() : "closed") << "\n";
                const QJsonArray pend = st["pending"].toArray();
                if (!pend.isEmpty()) {
                    out << "Pending requests:\n";
                    for (const QJsonValue &v : pend) { const QJsonObject o = v.toObject(); out << "  " << o["id"].toString() << "  " << o["name"].toString() << "  " << o["ip"].toString() << "  code " << o["code"].toString() << "  " << o["status"].toString() << "\n"; }
                }
                out << "Devices:\n";
                for (const QJsonValue &v : st["devices"].toArray()) {
                    const QJsonObject o = v.toObject();
                    QStringList sc; for (const QJsonValue &x : o["scopes"].toArray()) sc << x.toString();
                    out << "  " << o["id"].toString() << "  " << o["name"].toString() << "  [" << sc.join(",") << "]  last seen " << (o["lastSeen"].isNull() ? QStringLiteral("never") : o["lastSeen"].toString()) << " " << o["lastIp"].toString() << (o["revoked"].toBool() ? "  REVOKED" : "") << "\n";
                }
            }
        }
        out.flush();
        return rc;
    }

    if (p.isSet(refresh)) {
        QDBusInterface iface(SVC, PATH, SVC, bus);   // D-Bus activation starts the tray if needed
        if (iface.isValid()) { iface.call(QStringLiteral("Refresh")); return 0; }
        fprintf(stderr, "beaconfix: no running instance and D-Bus activation failed\n");
        return 1;
    }

    // Single instance: hand off to a running one
    if (!bus.registerService(SVC)) {
        if (qEnvironmentVariableIsSet("BEACONFIX_DEBUG")) fprintf(stderr, "beaconfix: another instance owns %s (%s) — handing off\n", SVC, qPrintable(bus.lastError().message()));
        QDBusInterface iface(SVC, PATH, SVC, bus);
        if (!p.isSet(tray)) iface.call(QStringLiteral("ShowWindow"));
        return 0;
    }

    auto *loc = new Locator(false, &app);
    bus.registerObject(PATH, loc, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSignals);

    auto *tiles = new TileSource(&app);
    if (tiles->listen()) loc->setTileBase(tiles->baseUrl());   // the Plasma widget fetches its map from here
    auto *api = new ApiServer(loc, &app);                       // LAN API for other devices (frame, phone, laptop)
    loc->setApiServer(api);
    auto *ranging = new RangingService(loc, &app);              // device ranging: BLE advert + scan, RTT reports, fusion (docs/RANGING.md)
    loc->setRanging(ranging);
    MainWindow win(loc, tiles);
    Tray trayIcon(loc, &app);
    auto showWin = [&win] { win.show(); win.raise(); win.activateWindow(); };
    QObject::connect(&trayIcon, &Tray::openWindowRequested, &app, showWin);
    QObject::connect(&trayIcon, &Tray::openIdentityRequested, &app, [&win] { win.showIdentity(); });
    QObject::connect(&trayIcon, &Tray::openEmergencyRequested, &app, [&win] { win.showEmergency(); });
    QObject::connect(loc, &Locator::showWindowRequested, &app, showWin);
    // Pairing: a request opens its picture-match dialog; the notification's buttons land there too
    QObject::connect(loc, &Locator::pairingRequested, &app, [&win](const QString &json) {
        const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
        if (o["status"].toString() == QLatin1String("pending")) win.showPairRequest(o["id"].toString());
    });
    QObject::connect(loc, &Locator::pairingOpenRequested, &app, [&win](const QString &id) { win.showPairRequest(id); });
    QObject::connect(&trayIcon, &Tray::quitRequested, &app, &QCoreApplication::quit);
    // Offline map: warm the tile cache around every new stop (and on request)
    auto doPrefetch = [loc, tiles](bool force) {
        if (!loc->fix().valid || (!force && !loc->prefetchTiles())) return;
        const int layer = qBound(0, QSettings().value("map/layer", 0).toInt(), int(TileSource::Topo));
        tiles->prefetch(loc->fix().lat, loc->fix().lon, TileSource::Layer(layer), 10, 15, 10, force);
    };
    QObject::connect(loc, &Locator::stopAdded, &app, [doPrefetch, loc](double, double) { if (loc->fix().precise()) doPrefetch(false); });
    QObject::connect(loc, &Locator::prefetchRequested, &app, [doPrefetch] { doPrefetch(true); });
    QObject::connect(tiles, &TileSource::prefetchFinished, loc, [loc](int fetched, int) { loc->notePrefetchDone(fetched); });
    QObject::connect(tiles, &TileSource::prefetchProgress, loc, [loc, tiles](int done, int total) {
        if (total && done % 50 == 0) emit loc->statusMessage(QStringLiteral("Saving map for offline use… %1 / %2 tiles").arg(done).arg(total));
        Q_UNUSED(tiles);
    });
    if (!p.isSet(tray)) showWin();

    loc->start();
    // Test instances (their own XDG_CONFIG_HOME, maybe a copy of the identity) never advertise over BLE
    const bool defaultConfig = QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)) == QDir::cleanPath(QDir::homePath() + QStringLiteral("/.config"));
    if (defaultConfig && !qEnvironmentVariableIsSet("BEACONFIX_NO_BLE")) ranging->start();
    return app.exec();
}

#include "main.moc"
