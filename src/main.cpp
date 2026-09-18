#include "locator.h"
#include "mainwindow.h"
#include "tray.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
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
    p.addOptions({tray, once, json, refresh});
    p.process(app);

    QTextStream out(stdout);
    QDBusConnection bus = QDBusConnection::sessionBus();

    if (p.isSet(once)) {
        Locator loc(true);
        QObject::connect(&loc, &Locator::probeFinished, &app, [&](bool ok, const QString &msg) {
            if (!ok) {
                QJsonObject o; o["valid"] = false; o["source"] = QStringLiteral("none"); o["error"] = msg;
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
                QJsonObject o = loc.lastProbe().toJson();
                o["place"] = loc.fix().place;
                out << QJsonDocument(o).toJson(QJsonDocument::Compact) << "\n";
                out.flush();
                QCoreApplication::quit();
            });
            poll->start(200);
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

    MainWindow win(loc);
    Tray trayIcon(loc, &app);
    auto showWin = [&win] { win.show(); win.raise(); win.activateWindow(); };
    QObject::connect(&trayIcon, &Tray::openWindowRequested, &app, showWin);
    QObject::connect(loc, &Locator::showWindowRequested, &app, showWin);
    QObject::connect(&trayIcon, &Tray::quitRequested, &app, &QCoreApplication::quit);
    if (!p.isSet(tray)) showWin();

    loc->start();
    return app.exec();
}
