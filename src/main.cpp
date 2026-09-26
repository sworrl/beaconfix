#include "locator.h"
#include "mainwindow.h"
#include "beaconview.h"
#include "tilesource.h"
#include "tray.h"
#include "apiserver.h"
#include "mapdb.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTextStream>
#include <QEventLoop>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QTimer>
#include <cstdio>

static const char *SVC  = "org.sworrl.BeaconFix";
static const char *PATH = "/org/sworrl/BeaconFix";

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
    p.addOptions({tray, once, json, refresh, snapshot, gpx, copy, newTrip, prefetch, apiStatus, devices, approve, deny, revoke, token, control, pairing,
                  homeAdd, homeRemove, homeList, homeSync, homeToken, homeImport, knownImport, knownList, knownAdd, knownName, knownRemove, dbStats, dbExport, dbImport});
    p.process(app);

    QTextStream out(stdout);
    QDBusConnection bus = QDBusConnection::sessionBus();

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
    MainWindow win(loc, tiles);
    Tray trayIcon(loc, &app);
    auto showWin = [&win] { win.show(); win.raise(); win.activateWindow(); };
    QObject::connect(&trayIcon, &Tray::openWindowRequested, &app, showWin);
    QObject::connect(loc, &Locator::showWindowRequested, &app, showWin);
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
    return app.exec();
}
