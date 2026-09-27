// BeaconFix BLE link harness: advertise the ranging beacon on hci0 and print what the scanner hears.
//
//   moc src/ranging/blelink.h -o /tmp/moc_blelink.cpp
//   g++ -std=c++17 -fPIC -I src/ranging tests/ranging_ble_test.cpp src/ranging/blelink.cpp /tmp/moc_blelink.cpp
//       src/ranging/rangemath.cpp $(pkg-config --cflags --libs Qt6Core Qt6DBus) -o /tmp/ranging_ble_test
//   /tmp/ranging_ble_test [seconds] [identity-id]
//   (identity-id defaults to the fixed test id of docs/RANGING.md §11; pass your own, from
//   `beaconfix --identity`, for your phone to recognise the advert)
//
// Proof that the advert goes out: run `sudo btmgmt --index 0 advinfo` (an instance appears) or
// `sudo btmon` while this runs (look for LE Set (Extended) Advertising Data with UUID 28c9f0bf…).
#include "../src/ranging/blelink.h"
#include "../src/ranging/rangemath.h"
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const int seconds = argc > 1 ? atoi(argv[1]) : 25;
    const QString id = argc > 2 ? QString::fromLatin1(argv[2]) : QStringLiteral("6htgz65xb7yfs53dmhdanfmk7c");   // §11 test id
    BleLink link;
    link.setIdentity(id);
    link.setFlags(true, true, RangeMath::KindDesktop, false);
    link.setTxPower(127);
    link.setIntervalMs(200);
    link.setScanDuty(1, 1);                                 // continuous for the test
    int ours = 0;
    QObject::connect(&link, &BleLink::sample, &app, [&](const QByteArray &tag, int rssi, int tx, int kind, int flags, qint64 t, const QString &addr) {
        ++ours;
        std::printf("  beaconfix advert from %s: tag %s rssi %d dBm tx %d kind %d flags 0x%02x t=%lld\n",
                    qPrintable(addr), tag.toHex().constData(), rssi, tx, kind, flags, (long long)t);
        std::fflush(stdout);
    });
    QObject::connect(&link, &BleLink::advertisingChanged, &app, [&](bool on) { std::printf("advertising: %s  (service data %s)\n", on ? "on" : "off", link.currentServiceData().toHex().constData()); std::fflush(stdout); });
    QObject::connect(&link, &BleLink::error, &app, [&](const QString &e) { std::printf("error: %s\n", qPrintable(e)); std::fflush(stdout); });
    QTimer tick; tick.setInterval(5000);
    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        std::printf("status: advertising=%d scanning=%d adverts seen=%d beaconfix samples=%d\n", link.advertising(), link.scanning(), link.advertsSeen(), ours);
        std::fflush(stdout);
    });
    tick.start();
    QTimer::singleShot(seconds * 1000, &app, [&] { link.stop(); QTimer::singleShot(500, &app, &QCoreApplication::quit); });
    if (!link.start()) { std::printf("start failed: %s\n", qPrintable(link.lastError())); return 1; }
    const int rc = app.exec();
    std::printf("done: adverts seen %d, beaconfix samples %d, last error '%s'\n", link.advertsSeen(), ours, qPrintable(link.lastError()));
    return rc;
}
