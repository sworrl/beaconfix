// Synthetic-fixture tests for src/importers.{h,cpp}. The importer feeds a Locator, so link the app's
// objects (everything but main.cpp.o) after a normal build — from the repository root:
//   OBJS=$(ls build/CMakeFiles/beaconfix.dir/src/*.o | grep -v main.cpp.o); MOC=build/CMakeFiles/beaconfix.dir/beaconfix_autogen/mocs_compilation.cpp.o
//   g++ -std=c++17 -O2 -Wall -Wextra -fPIC $(pkg-config --cflags Qt6Widgets Qt6DBus Qt6Network Qt6Sql) -Isrc tests/importers_test.cpp $OBJS $MOC -o build/importers_test $(pkg-config --libs Qt6Widgets Qt6DBus Qt6Network Qt6Sql) -lcrypto
//   ./build/importers_test        (reads tests/fixtures/*)
#include "../src/importers.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonValue>
#include <cstdio>

using namespace Importers;
static int fails = 0;
#define CHECK(cond, fmt, ...) do { if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } else { std::printf("ok   " fmt "\n", ##__VA_ARGS__); } } while (0)

struct Collected {
    QList<PosSample> positions; QList<WifiScan> scans; QList<Visit> visits;
    QList<QString> wigleMacs, wigleSsids, wigleSec; QList<PosSample> wiglePos;
    Sinks sinks() {
        Sinks s;
        s.positions = [this](const QList<PosSample> &l) { positions += l; };
        s.scans = [this](const QList<WifiScan> &l) { scans += l; };
        s.visits = [this](const QList<Visit> &l) { visits += l; };
        s.wigle = [this](const QList<WifiRec> &r, const PosSample &p, const QString &ssid, const QString &sec) { for (const WifiRec &w : r) { wigleMacs << w.bssid; wigleSsids << ssid; wigleSec << sec; wiglePos << p; } };
        return s;
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // ── helpers ──
    CHECK(macFromValue(QJsonValue(1234567890123.0)) == QStringLiteral("00:00:01:1F:71:FB:04:CB").mid(6), "macFromValue decimal → %s", qPrintable(macFromValue(QJsonValue(1234567890123.0))));
    CHECK(macFromValue(QJsonValue(QStringLiteral("1234567890123"))) == QStringLiteral("01:1F:71:FB:04:CB"), "macFromValue decimal string");
    CHECK(macFromValue(QJsonValue(QStringLiteral("aa:bb:cc:dd:ee:01"))) == QStringLiteral("AA:BB:CC:DD:EE:01"), "macFromValue colon string upper-cased");
    CHECK(macFromValue(QJsonValue(QStringLiteral("AABBCCDDEE01"))) == QStringLiteral("AA:BB:CC:DD:EE:01"), "macFromValue bare hex");
    CHECK(macFromValue(QJsonValue(QStringLiteral("nonsense"))).isEmpty(), "macFromValue garbage → empty");
    double la = 0, lo = 0;
    CHECK(parseLatLng(QStringLiteral("40.0029°, -75.0681°"), &la, &lo) && qAbs(la - 40.0029) < 1e-9 && qAbs(lo + 75.0681) < 1e-9, "parseLatLng with degree signs");
    CHECK(parseLatLng(QStringLiteral("40.0, -75.1"), &la, &lo), "parseLatLng plain");
    CHECK(!parseLatLng(QStringLiteral("95, 10"), &la, &lo), "parseLatLng rejects lat > 90");
    CHECK(parseTime(QJsonValue(QStringLiteral("2024-03-10T08:00:00.000-04:00"))) == 1710072000, "parseTime ISO with offset → %lld", (long long)parseTime(QJsonValue(QStringLiteral("2024-03-10T08:00:00.000-04:00"))));
    CHECK(parseTime(QJsonValue(QStringLiteral("1622548860000"))) == 1622548860, "parseTime epoch ms string");
    CHECK(parseTime(QJsonValue(1622548860.0)) == 1622548860, "parseTime epoch s number");
    CHECK(parseTime(QJsonValue(QStringLiteral("2024-05-05 10:00:00"))) == QDateTime(QDate(2024, 5, 5), QTime(10, 0)).toSecsSinceEpoch(), "parseTime WiGLE local format");
    CHECK(wigleSecurity(QStringLiteral("[WPA2-PSK-CCMP][ESS]")) == QStringLiteral("wpa2"), "wigleSecurity wpa2");
    CHECK(wigleSecurity(QStringLiteral("[WPA2-PSK-CCMP][RSN-SAE-CCMP][ESS]")) == QStringLiteral("wpa2/3"), "wigleSecurity mixed sae+psk");
    CHECK(wigleSecurity(QStringLiteral("[ESS]")) == QStringLiteral("open"), "wigleSecurity open");
    CHECK(wigleSecurity(QStringLiteral("[WEP][ESS]")) == QStringLiteral("wep"), "wigleSecurity wep");
    CHECK(wigleSecurity(QStringLiteral("[WPA2-EAP-CCMP][ESS]")) == QStringLiteral("wpa2-eap"), "wigleSecurity eap");
    // ── positionAt ──
    QList<PosSample> ps{{1000, 40.00, -75.06, 10, 0}, {1100, 40.01, -75.07, 10, 0}, {2000, 40.10, -75.10, 500, 0}, {5000, 40.20, -75.20, 10, 0}};
    PosSample at;
    CHECK(positionAt(ps, 1050, &at) && qAbs(at.lat - 40.005) < 1e-9 && qAbs(at.lon + 75.065) < 1e-9, "positionAt interpolates between neighbours ≤ 2 min apart → %.4f,%.4f", at.lat, at.lon);
    CHECK(positionAt(ps, 1130, &at) && qAbs(at.lat - 40.01) < 1e-9, "positionAt nearest within 60 s (both neighbours not within 2 min)");
    CHECK(!positionAt(ps, 2010, &at), "positionAt skips a position with acc > 100 m");
    CHECK(!positionAt(ps, 3500, &at), "positionAt nothing within 60 s");
    CHECK(positionAt(ps, 5059, &at) && at.t == 5059, "positionAt edge: 59 s after the last sample");
    // ── detectFormat ──
    CHECK(detectFormat(QStringLiteral("tests/fixtures/timeline.json")) == QStringLiteral("timeline"), "detectFormat timeline");
    CHECK(detectFormat(QStringLiteral("tests/fixtures/records.json")) == QStringLiteral("records"), "detectFormat records");
    CHECK(detectFormat(QStringLiteral("tests/fixtures/semantic.json")) == QStringLiteral("semantic"), "detectFormat semantic");
    CHECK(detectFormat(QStringLiteral("tests/fixtures/wigle.csv")) == QStringLiteral("wigle"), "detectFormat wigle");
    CHECK(detectFormat(QStringLiteral("tests/fixtures/track.gpx")) == QStringLiteral("gpx"), "detectFormat gpx");
    CHECK(detectFormat(QStringLiteral("tests/fixtures/track.kml")) == QStringLiteral("kml"), "detectFormat kml");
    CHECK(detectFormat(QStringLiteral("tests/fixtures/unknown.txt")).isEmpty(), "detectFormat unknown → empty");
    // ── JsonArrayStream: an element split across the 1 MB chunk boundary ──
    {
        QByteArray big = "{\"pad\":\"" + QByteArray(1024 * 1024 - 30, 'x') + "\",\"locations\":[{\"a\":1},{\"b\":\"]}\\\"\"},[1,2],3,{\"c\":{\"d\":[1]}}]}";
        QFile f(QStringLiteral("build/_stream_test.json")); CHECK(f.open(QIODevice::WriteOnly), "write stream fixture"); f.write(big); f.close();
        CHECK(f.open(QIODevice::ReadOnly), "reopen stream fixture");
        JsonArrayStream st(&f);
        QByteArray el; QList<QByteArray> els;
        CHECK(st.seekKey("locations"), "stream seeks the key past a 1 MB value");
        while (st.next(&el)) els << el;
        CHECK(els.size() == 5, "stream yields 5 elements (got %d)", int(els.size()));
        CHECK(els.value(1) == "{\"b\":\"]}\\\"\"}", "stream ignores brackets inside strings: %s", els.value(1).constData());
        CHECK(els.value(3) == "3", "stream yields scalars");
        CHECK(els.value(4) == "{\"c\":{\"d\":[1]}}", "stream tracks nesting");
        f.remove();
    }
    // ── Timeline.json ──
    {
        Collected c; Summary s; QFile f(QStringLiteral("tests/fixtures/timeline.json")); CHECK(f.open(QIODevice::ReadOnly), "open %s", "tests/fixtures/timeline.json");
        Options o;
        CHECK(parseTimeline(&f, o, c.sinks(), &s), "parseTimeline ok");
        CHECK(s.positions == 4, "timeline: 4 raw positions (got %d)", s.positions);
        CHECK(s.wifiScans == 4, "timeline: 4 wifi scans (got %d)", s.wifiScans);
        CHECK(s.visits == 2, "timeline: 2 visits (got %d)", s.visits);
        CHECK(s.tracks == 3, "timeline: 3 timelinePath points (got %d)", s.tracks);
        CHECK(c.scans.value(0).devices.size() == 2 && c.scans.value(0).devices[0].bssid == QStringLiteral("01:1F:71:FB:04:CB") && c.scans.value(0).devices[0].dbm == -50, "timeline: wifi scan devices decoded");
        CHECK(c.visits.value(0).type == QStringLiteral("HOME") && qAbs(c.visits.value(0).lat - 40.003) < 1e-9, "timeline: visit HOME at placeLocation");
        // pairing: 08:00:20 sits between 08:00:00 (acc 12) and 08:01:00 (acc 20) → interpolated 1/3 of the way
        QList<PosSample> sorted = c.positions; std::sort(sorted.begin(), sorted.end(), [](const PosSample &a, const PosSample &b) { return a.t < b.t; });
        PosSample at; CHECK(positionAt(sorted, c.scans[0].t, &at) && qAbs(at.lat - (40.003 + 0.001 / 3)) < 1e-6, "timeline: scan paired by interpolation (%.6f)", at.lat);
        CHECK(!positionAt(sorted, c.scans[2].t, &at), "timeline: scan next to a 900 m position is skipped");
        CHECK(!positionAt(sorted, c.scans[3].t, &at), "timeline: scan with no position within 60 s is skipped");
        // window filter
        Collected c2; Summary s2; f.seek(0); Options w; w.from = QDateTime::fromString(QStringLiteral("2025-01-01T00:00:00Z"), Qt::ISODate);
        parseTimeline(&f, w, c2.sinks(), &s2);
        CHECK(s2.positions == 1 && s2.wifiScans == 0 && s2.visits == 0, "timeline: --from window keeps only the 2025 position (%d/%d/%d)", s2.positions, s2.wifiScans, s2.visits);
        Collected c3; Summary s3; f.seek(0); Options only; only.positions = false; only.places = false;
        parseTimeline(&f, only, c3.sinks(), &s3);
        CHECK(s3.wifiScans == 4 && s3.visits == 0 && s3.tracks == 0 && c3.positions.size() == 4, "timeline: --what wifi still reads positions for pairing but emits no visits/tracks");
    }
    // ── Records.json ──
    {
        Collected c; Summary s; QFile f(QStringLiteral("tests/fixtures/records.json")); CHECK(f.open(QIODevice::ReadOnly), "open %s", "tests/fixtures/records.json");
        CHECK(parseRecords(&f, Options(), c.sinks(), &s), "parseRecords ok");
        CHECK(s.positions == 3 && s.skipped == 1, "records: 3 positions, 1 skipped (got %d/%d)", s.positions, s.skipped);
        CHECK(s.wifiScans == 2 && c.scans[0].devices.size() == 2 && c.scans[0].devices[1].bssid == QStringLiteral("59:D3:9E:7F:3B:34"), "records: wifiScan decoded (%s)", qPrintable(c.scans.value(0).devices.value(1).bssid));
        CHECK(c.positions[1].t == 1622548860 && qAbs(c.positions[1].alt + 9999) < 1, "records: timestampMs string, no altitude");
        CHECK(qAbs(c.positions[0].alt - 121) < 1e-9 && qAbs(c.positions[0].lat - 40.003) < 1e-9, "records: E7 + altitude");
    }
    // ── Semantic Location History ──
    {
        Collected c; Summary s; QFile f(QStringLiteral("tests/fixtures/semantic.json")); CHECK(f.open(QIODevice::ReadOnly), "open %s", "tests/fixtures/semantic.json");
        CHECK(parseSemantic(&f, Options(), c.sinks(), &s), "parseSemantic ok");
        CHECK(s.visits == 2 && c.visits[0].name == QStringLiteral("Home") && c.visits[0].address == QStringLiteral("1 Main St") && c.visits[1].start == 1643793600, "semantic: placeVisits (ms and ISO durations)");
        CHECK(s.tracks == 2 && s.positions == 1, "semantic: 2 path points + activity start (got %d/%d)", s.tracks, s.positions);
    }
    // ── WiGLE ──
    {
        Collected c; Summary s; QFile f(QStringLiteral("tests/fixtures/wigle.csv")); CHECK(f.open(QIODevice::ReadOnly), "open %s", "tests/fixtures/wigle.csv");
        CHECK(parseWigle(&f, Options(), c.sinks(), &s), "parseWigle ok");
        CHECK(s.wifiScans == 3 && s.skipped == 2, "wigle: 3 WIFI rows, BT + 0/0 skipped (got %d/%d)", s.wifiScans, s.skipped);
        CHECK(c.wigleSsids.value(0) == QStringLiteral("Home, sweet"), "wigle: quoted SSID with a comma");
        CHECK(c.wigleSec.value(0) == QStringLiteral("wpa2") && c.wigleSec.value(1) == QStringLiteral("open") && c.wigleSec.value(2) == QStringLiteral("wpa2/3"), "wigle: security mapped");
        CHECK(s.positions == 2, "wigle: one position per distinct time (got %d)", s.positions);
        CHECK(qAbs(c.wiglePos.value(0).acc - 5) < 1e-9 && qAbs(c.wiglePos.value(0).alt - 120) < 1e-9, "wigle: accuracy + altitude");
    }
    // ── GPX ──
    {
        Collected c; Summary s; QFile f(QStringLiteral("tests/fixtures/track.gpx")); CHECK(f.open(QIODevice::ReadOnly), "open %s", "tests/fixtures/track.gpx");
        CHECK(parseGpx(&f, Options(), c.sinks(), &s), "parseGpx ok");
        CHECK(s.tracks == 3 && s.skipped == 1 && s.visits == 1, "gpx: 3 timed trkpts, 1 untimed skipped, 1 wpt (got %d/%d/%d)", s.tracks, s.skipped, s.visits);
        CHECK(qAbs(c.positions[1].acc - 10) < 1e-9 && qAbs(c.positions[1].alt - 121) < 1e-9, "gpx: hdop → acc, ele → alt");
        CHECK(c.visits[0].name == QStringLiteral("Start"), "gpx: waypoint name");
    }
    // ── KML ──
    {
        Collected c; Summary s; QFile f(QStringLiteral("tests/fixtures/track.kml")); CHECK(f.open(QIODevice::ReadOnly), "open %s", "tests/fixtures/track.kml");
        CHECK(parseKml(&f, Options(), c.sinks(), &s), "parseKml ok");
        CHECK(s.tracks == 3 && s.visits == 1 && s.skipped == 1, "kml: gx:Track 3 points, 1 timed placemark, untimed line skipped (got %d/%d/%d)", s.tracks, s.visits, s.skipped);
        CHECK(qAbs(c.positions[1].lat - 40.01) < 1e-9 && qAbs(c.positions[1].lon + 75.07) < 1e-9 && qAbs(c.positions[1].alt - 125) < 1e-9, "kml: coord order lon lat alt");
        CHECK(c.visits[0].name == QStringLiteral("Coffee") && c.visits[0].start == 1690876800, "kml: placemark with TimeStamp");
    }
    // ── Summary JSON contract (the numbers both apps show) ──
    {
        Summary s; s.format = QStringLiteral("timeline"); s.positions = 4; s.wifiScans = 4; s.observations = 3; s.visits = 2; s.tracks = 3; s.skipped = 1; s.beaconsTouched = 2; s.bytes = 1234; s.seconds = 0.5;
        const QJsonObject o = s.toJson();
        for (const char *k : {"format", "file", "device", "positions", "wifiScans", "observations", "visits", "tracks", "skipped", "errors", "beaconsTouched", "bytes", "first", "last", "seconds", "error"})
            CHECK(o.contains(QLatin1String(k)), "summary has %s", k);
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
