// SPDX-License-Identifier: Apache-2.0
// Unit tests for camera-pass detection (docs/SIGHTINGS.md §2): directions, classification, polyline closest
// approach, speed / heading, facing, confidence, run splitting, cross-device merging, the live tracker, uids.
#include "../src/plateevents.h"
#include <QJsonDocument>
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } \
    else std::printf("ok   " fmt "\n", ##__VA_ARGS__); \
} while (0)

using namespace PlateEvents;

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static bool sameList(const QList<double> &a, const QList<double> &b)
{
    if (a.size() != b.size()) return false;
    for (qsizetype i = 0; i < a.size(); ++i) if (!near(a[i], b[i], 1e-6)) return false;
    return true;
}

// a point dx metres east, dy metres north of (lat, lon)
static void offset(double lat, double lon, double dx, double dy, double *olat, double *olon)
{
    *olat = lat + dy / 111194.93;
    *olon = lon + dx / (111194.93 * std::cos(lat * M_PI / 180.0));
}

// West → east, 30 m north of the camera, 200 m between fixes, 10 s apart (72 km/h): no fix within 65 m
static QList<TrackFix> eastbound(const Camera &cam, qint64 t0, const QString &device = QString(), double northM = 30.0, double acc = 10.0)
{
    QList<TrackFix> out;
    for (int i = -3; i <= 3; ++i) {
        TrackFix f; f.ms = t0 + qint64(i) * 10000; f.acc = acc; f.device = device; f.source = QStringLiteral("gps");
        offset(cam.lat, cam.lon, i * 200.0 - 100.0, northM, &f.lat, &f.lon);   // the camera is half-way between fixes 0 and 1
        out << f;
    }
    return out;
}

int main()
{
    // ── §2.3 directions ──
    CHECK(sameList(parseDirections(QStringLiteral("330")), {330}), "direction 330");
    CHECK(sameList(parseDirections(QStringLiteral("260;148")), {260, 148}), "direction list 260;148");
    CHECK(sameList(parseDirections(QStringLiteral("NE")), {45}), "cardinal NE = 45");
    CHECK(sameList(parseDirections(QStringLiteral("NNW")), {337.5}), "cardinal NNW = 337.5");
    CHECK(sameList(parseDirections(QStringLiteral("WB")), {270}), "WB = 270");
    CHECK(sameList(parseDirections(QStringLiteral("90-180")), {135}), "range 90-180 → 135");
    CHECK(sameList(parseDirections(QStringLiteral("350-10")), {0}), "range 350-10 → 0 (through north)");
    CHECK(parseDirections(QStringLiteral("0-360")).isEmpty(), "0-360 is all round: no direction");
    CHECK(parseDirections(QStringLiteral("0")).isEmpty() && parseDirections(QStringLiteral("0;0")).isEmpty(), "direction=0 means unknown (DeFlock writes 0)");
    CHECK(sameList(parseDirections(QStringLiteral("N;S-W; 400")), {0, 225, 40}), "mixed list N;S-W;400 (an explicit N is north)");
    CHECK(parseDirections(QString()).isEmpty() && parseDirections(QStringLiteral("abc")).isEmpty(), "empty / junk");
    {
        Camera c; c.direction = QStringLiteral("90");
        c.tags = QJsonDocument::fromJson(R"({"camera:direction":"45","direction":"200"})").object();
        CHECK(directionText(c) == "200", "direction wins over camera:direction");
        c.tags = QJsonDocument::fromJson(R"({"camera:direction":"45"})").object();
        CHECK(directionText(c) == "45", "camera:direction when there is no direction");
    }

    // ── §2.0 classification, in order ──
    const QJsonObject wvdot = QJsonDocument::fromJson(R"({"camera:direction":"330","camera:type":"fixed","contact:webcam":"https://wv511.org/flowplayeri.aspx?CAMID=CAM064",
        "man_made":"surveillance","operator":"West Virginia Department of Transportation","ref":"CAM064","surveillance":"public","surveillance:type":"camera","surveillance:zone":"traffic"})").object();
    CHECK(classifyCamera(QStringLiteral("CAMERA"), QStringLiteral("osm"), QStringLiteral("osm:node/13700003267"), QStringLiteral("osm_tag"), wvdot) == "webcam", "WVDOT CAM064 (its tags) is a webcam");
    CHECK(classifyCamera(QStringLiteral("CAMERA"), QStringLiteral("osm"), QStringLiteral("osm:node/13700003267"), QStringLiteral("osm_tag"), {}) == "cctv", "WVDOT CAM064 before its tags are known: cctv, never ALPR");
    CHECK(webcamUrl(wvdot) == "https://wv511.org/flowplayeri.aspx?CAMID=CAM064", "webcam URL from contact:webcam");
    const QJsonObject flockTags = QJsonDocument::fromJson(R"({"direction":"260;148","man_made":"surveillance","surveillance":"camera","surveillance:type":"ALPR","surveillance:zone":"traffic"})").object();
    CHECK(classifyCamera(QStringLiteral("ALPR"), QStringLiteral("osm"), QStringLiteral("osm:node/13908528827"), QStringLiteral("osm_tag"), flockTags) == "alpr", "surveillance:type=ALPR (zone traffic) is an ALPR");
    auto tagType = [](const char *json) { return classifyCamera(QString(), QStringLiteral("osm"), QStringLiteral("osm:node/1"), QString(), QJsonDocument::fromJson(json).object()); };
    CHECK(tagType(R"({"highway":"speed_camera","surveillance:type":"ALPR"})") == "enforcement", "1. highway=speed_camera → enforcement (before ALPR)");
    CHECK(tagType(R"({"surveillance:type":"gunshot_detector","manufacturer":"Flock Safety"})") == "not_camera", "2. gunshot detector → not_camera");
    CHECK(tagType(R"({"surveillance:type":"guard"})") == "not_camera", "2. guard → not_camera");
    CHECK(tagType(R"({"man_made":"monitoring_station","monitoring:traffic":"yes"})") == "not_camera", "2. traffic counter → not_camera");
    CHECK(tagType(R"({"surveillance:type":"camera;ANPR"})") == "alpr", "3. a surveillance:type token ANPR");
    CHECK(tagType(R"({"surveillance":"ANPR"})") == "alpr" && tagType(R"({"camera:type":"ALPR"})") == "alpr", "3. surveillance=ANPR, camera:type=ALPR");
    CHECK(tagType(R"({"surveillance:type":"ALRP"})") == "alpr" && classifyNote(QJsonDocument::fromJson(R"({"surveillance:type":"ALRP"})").object()).contains("misspelt"), "3. the misspelling ALRP: ALPR, noted");
    CHECK(classifyNote(QJsonDocument::fromJson(R"({"surveillance:type":"ALPR","contact:webcam":"https://x.example/cam"})").object()).contains("conflict"), "3. contact:webcam on an ALPR is flagged");
    CHECK(tagType(R"({"surveillance:type":"camera","manufacturer":"Flock Safety","camera:type":"dome"})") == "ptz", "5. Flock + surveillance:type=camera → ptz (Condor)");
    CHECK(tagType(R"({"surveillance:type":"camera","operator":"Walmart"})") == "cctv" && tagType(R"({"man_made":"surveillance"})") == "cctv", "6. anything else → cctv");
    CHECK(classifyCamera(QStringLiteral("Falcon"), QStringLiteral("osm"), QStringLiteral("osm:node/1"), QStringLiteral("osm_tag"), {}) == "alpr", "untagged OSM row, model Falcon (as imported): alpr");
    CHECK(classifyCamera(QStringLiteral("ALPR Camera"), QStringLiteral("3rd Party / Suspected"), QStringLiteral("flock:1"), QStringLiteral("community_database"), {}) == "alpr", "suspected list row: alpr");
    CHECK(classifyCamera(QStringLiteral("Genetec AutoVu"), QStringLiteral("community"), QStringLiteral("flock:2"), QString(), {}) == "alpr", "community row: alpr");
    CHECK(classifyCamera(QStringLiteral("Falcon"), QStringLiteral("wifi_scan"), QStringLiteral("det:D8:F3:BC:00:00:01"), QStringLiteral("wifi_ssid"), {}) == "alpr", "an RF detection of Flock hardware: alpr");
    CHECK(suspectedOnly(QStringLiteral("3rd Party / Suspected"), QString()) && !suspectedOnly(QStringLiteral("osm"), QStringLiteral("Falcon")), "suspected-only source");
    // ── §2.3 cones ──
    {
        Camera c; c.model = QStringLiteral("Falcon");
        CHECK(coneFor(c).halfDeg == 10 && coneFor(c).minM == 6 && coneFor(c).maxM == 25, "Flock Falcon: 10°, 6–25 m");
        c.model = QStringLiteral("Falcon Long-Range"); CHECK(coneFor(c).halfDeg == 7 && coneFor(c).maxM == 76, "Falcon LR: 7°, 15–76 m");
        c.model = QStringLiteral("ALPR"); c.operatorName = QStringLiteral("Motorola Solutions"); CHECK(coneFor(c).halfDeg == 12 && coneFor(c).maxM == 23, "Motorola / Vigilant: 12°, 8–23 m");
        c.operatorName.clear(); c.tags = QJsonDocument::fromJson(R"({"manufacturer":"Genetec"})").object(); CHECK(coneFor(c).maxM == 45, "Genetec (manufacturer tag): 3–45 m");
        c.tags = {}; c.model = QStringLiteral("Verkada"); CHECK(coneFor(c).halfDeg == 22, "Verkada: 22°");
        c.model = QStringLiteral("ALPR"); CHECK(coneFor(c).halfDeg == 12.5 && coneFor(c).maxM == 35, "unknown fixed ALPR: 12.5°, 5–35 m");
    }

    // ── §2.1 / §2.2 polyline closest approach ──
    Camera cam; cam.id = QStringLiteral("osm:node/1"); cam.lat = 39.6578749; cam.lon = -79.9545898; cam.type = QStringLiteral("alpr");
    cam.model = QStringLiteral("ALPR"); cam.operatorName = QStringLiteral("Example PD"); cam.source = QStringLiteral("osm");
    const qint64 t0 = 1790000000000LL;
    {
        const QList<Pass> ps = detectPasses(eastbound(cam, t0), cam);
        CHECK(ps.size() == 1, "one pass from a polyline with no fix inside 65 m (%d)", int(ps.size()));
        if (ps.size() == 1) {
            const Pass &p = ps.first();
            CHECK(near(p.distanceM, 30.0, 0.5), "closest approach on the segment: %.2f m", p.distanceM);
            CHECK(std::llabs(p.ms - (t0 + 5000)) <= 50, "time interpolated along the segment: %+lld ms", (long long)(p.ms - (t0 + 5000)));
            CHECK(near(p.speedKmh, 72.0, 0.5), "speed from the segment: %.2f km/h", p.speedKmh);
            CHECK(near(p.headingDeg, 90.0, 0.5), "heading east: %.2f", p.headingDeg);
            CHECK(near(p.approachBearingDeg, 0.0, 0.5) || near(p.approachBearingDeg, 360.0, 0.5), "camera → us is north: %.2f", p.approachBearingDeg);
            CHECK(p.fixes == 0, "no fix inside the circle (%d)", p.fixes);
            CHECK(p.dwellS > 5.5 && p.dwellS < 6.5, "dwell inside 65 m: %.2f s (2·√(65²−30²)/20 = 5.77)", p.dwellS);
            CHECK(near(p.acc, 10.0, 1e-6), "accuracy interpolated: %.1f", p.acc);
            CHECK(p.facing == -1 && std::isnan(p.cameraDirDeg), "no direction: facing unknown");
            // §2.4 without a direction: P(in cone) = the fraction of the run within range (5–35 m: 36.1 of 115.3 m) × 0.5
            CHECK(near(p.pInCone, 0.1563, 0.006) && near(p.pRead, 0.1563 * 0.97 * 0.93, 0.006), "P(in cone) = 0.313 × 0.5 = %.4f, P(read) = %.4f", p.pInCone, p.pRead);
            CHECK(p.confidence == 14, "confidence = round(100 × P(read)) = 14 (%d)", p.confidence);
            CHECK(passUid(cam.id, p.ms) == QStringLiteral("pass:osm:node/1:%1").arg((t0 + 5000) / 60000), "uid pass:<camera>:<unix minute>: %s", qPrintable(passUid(cam.id, p.ms)));
        }
        Camera falcon = cam; falcon.model = QStringLiteral("Falcon");
        const Pass f = detectPasses(eastbound(falcon, t0), falcon).value(0);
        CHECK(f.confidence == 0 && f.inConeS == 0, "a Falcon (6–25 m) passed at 30 m: out of range, confidence 0 (%d)", f.confidence);
    }
    // ── §2.3 facing: the cone and the range before / after the closest approach (8 m off the road) ──
    {
        Camera c = cam; c.direction = QStringLiteral("270");           // looks west, down the road: sees us coming (front plate)
        Pass p = detectPasses(eastbound(c, t0, QString(), 8.0), c).value(0);
        CHECK(p.frontVisible && !p.rearVisible && p.facing == 1, "facing west, eastbound: front plate (inside the cone %.1f s)", p.inConeS);
        CHECK(p.inConeS > 0.2 && p.inConeS < 0.33, "time inside the cone and range: %.2f s (x from −34.1 to −28.8 m at 20 m/s = 0.26 s)", p.inConeS);
        CHECK(near(p.cameraDirDeg, 270, 1e-6), "camera_dir 270");
        CHECK(p.confidence >= 64 && p.confidence <= 68, "P(in cone) integrated over a 6.6 m σ: ≈ 0.728 × 0.902 → 66 (%d, P %.3f)", p.confidence, p.pInCone);
        c.direction = QStringLiteral("E");
        p = detectPasses(eastbound(c, t0, QString(), 8.0), c).value(0);
        CHECK(!p.frontVisible && p.rearVisible && p.facing == 1, "facing east, along the travel: rear plate");
        c.direction = QStringLiteral("S");
        p = detectPasses(eastbound(c, t0, QString(), 8.0), c).value(0);
        CHECK(p.facing == 0 && p.confidence <= 3, "facing south (away from the road): not seen, confidence %d", p.confidence);
        c.direction = QStringLiteral("0;270");
        p = detectPasses(eastbound(c, t0, QString(), 8.0), c).value(0);
        CHECK(p.facing == 1 && p.directions.size() == 1 && near(p.cameraDirDeg, 270, 1e-6), "\"0;270\": 0 is unknown, 270 saw us");
        Camera wide = cam; wide.direction = QStringLiteral("270"); wide.tags = QJsonDocument::fromJson(R"({"direction":"90"})").object();
        p = detectPasses(eastbound(wide, t0, QString(), 8.0), wide).value(0);
        CHECK(p.rearVisible && !p.frontVisible, "the tags' direction wins over the stored one");
    }
    // ── §2.4 confidence ──
    {
        Camera c = cam; c.source = QStringLiteral("3rd Party / Suspected");
        Pass p = detectPasses(eastbound(c, t0, QString(), 10.0), c).value(0);
        CHECK(p.confidence >= 15 && p.confidence <= 18, "suspected only: × 0.7 (%d)", p.confidence);
        Camera tc = cam; tc.type = QStringLiteral("cctv"); tc.direction = QStringLiteral("270");
        p = detectPasses(eastbound(tc, t0, QString(), 8.0), tc).value(0);
        CHECK(p.confidence == 40 && p.confidenceAlpr >= 64, "a CCTV camera: min(40, …) (%d, as ALPR %d)", p.confidence, p.confidenceAlpr);
        tc.type = QStringLiteral("webcam");
        const QJsonObject ev = passEvent(p, tc, QStringLiteral("XYZ-2345"), QStringLiteral("route_backfill"), QString());
        CHECK(ev["camera_type"].toString() == "webcam" && ev["details"].toString().contains("webcam") && ev["details"].toString().contains("does not read plates")
              && !ev["details"].toString().contains("likely read") && !alertable(QStringLiteral("webcam")), "webcam details: %s", qPrintable(ev["details"].toString()));
        const Pass pa = detectPasses(eastbound(cam, t0), cam).value(0);
        const QJsonObject ev2 = passEvent(pa, cam, QStringLiteral("XYZ-2345"), QStringLiteral("live_route"), QString());
        CHECK(ev2["details"].toString().contains("your plate was likely read") && ev2["kind"] == "camera_pass" && ev2["metrics"].toObject()["plateInferred"].toBool() && alertable(QStringLiteral("alpr")),
              "ALPR details: %s", qPrintable(ev2["details"].toString()));
        const QJsonObject m = ev2["metrics"].toObject();
        CHECK(m["coneHalfDeg"].toDouble() == 12.5 && m["rangeM"].toArray().size() == 2 && m.contains("pRead") && m.contains("inConeS"), "metrics: coneHalfDeg, rangeM, pRead, inConeS");
        CHECK(ev2["source_url"].toString() == "https://www.openstreetmap.org/node/1", "source_url for an OSM camera");
    }
    // ── runs: split at gaps > 5 min; a far track gives nothing ──
    {
        QList<TrackFix> two = eastbound(cam, t0);
        QList<TrackFix> back = eastbound(cam, t0 + 8 * 60000);
        two += back;
        QList<Pass> ps = detectPasses(two, cam);
        CHECK(ps.size() == 2, "two runs 8 min apart (a 7 min gap) = two passes (%d)", int(ps.size()));
        CHECK(mergePasses(ps).size() == 1, "…which §1.1 makes one pass (same camera within ±10 min) (%d)", int(mergePasses(ps).size()));
        QList<TrackFix> far = eastbound(cam, t0, QString(), 300.0);
        CHECK(detectPasses(far, cam).isEmpty(), "300 m away: no pass");
        // a lone fix (no neighbours within 5 min) inside the circle
        TrackFix lone; lone.ms = t0; lone.lat = cam.lat + 0.0002; lone.lon = cam.lon; lone.device = QString();
        ps = detectPasses({lone}, cam);
        CHECK(ps.size() == 1 && near(ps.first().distanceM, 22.2, 0.5) && ps.first().confidence == 45, "a lone fix inside 65 m is a pass (%.1f m, in range: 0.5 × 0.902 → %d)",
              ps.value(0).distanceM, ps.value(0).confidence);
    }
    // ── several devices: detected per device, merged within ±10 min ──
    {
        QList<TrackFix> both = eastbound(cam, t0, QStringLiteral("Pixel"), 30.0);
        both += eastbound(cam, t0 + 2000, QString(), 12.0);
        const QList<Pass> ps = detectAll(both, cam);
        CHECK(ps.size() == 1, "phone + desktop within 10 min: one pass (%d)", int(ps.size()));
        if (!ps.isEmpty()) {
            CHECK(near(ps.first().distanceM, 12.0, 0.5) && ps.first().devices.size() == 2, "the closest approach wins, both devices listed (%.1f m, %d devices)",
                  ps.first().distanceM, int(ps.first().devices.size()));
        }
    }
    // ── live: finish on leaving the circle ──
    {
        LiveTracker lt;
        QList<Pass> got;
        // 5 m/s eastbound, a fix every 5 s, 20 m north: inside the circle for several fixes
        for (int i = 0; i <= 14; ++i) {
            TrackFix f; f.ms = t0 + qint64(i) * 5000; f.acc = 8;
            offset(cam.lat, cam.lon, -175.0 + i * 25.0, 20.0, &f.lat, &f.lon);
            got += lt.addFix(f, {cam});
            if (i == 8) CHECK(got.isEmpty() && lt.activeCameras().size() == 1, "inside the circle: still active");
        }
        CHECK(got.size() == 1, "finished after leaving 65 m (%d)", int(got.size()));
        if (!got.isEmpty()) CHECK(near(got.first().distanceM, 20.0, 0.5) && got.first().fixes >= 4, "live closest approach %.1f m over %d fixes", got.first().distanceM, got.first().fixes);
        // parked inside: finishes 2 min after the closest approach, then stays quiet
        LiveTracker park;
        QList<Pass> p2;
        for (int i = 0; i <= 40; ++i) {
            TrackFix f; f.ms = t0 + qint64(i) * 10000; f.acc = 8; offset(cam.lat, cam.lon, 0, 20.0, &f.lat, &f.lon);
            p2 += park.addFix(f, {cam});
        }
        CHECK(p2.size() == 1, "parked 400 s inside: one pass (%d)", int(p2.size()));
    }
    // ── uids ──
    {
        const QJsonObject row = QJsonDocument::fromJson(R"({"org_id":"4411","search_time_utc":"2025-01-01T06:10:30.000Z",
            "license_plate_hash":"9f89b1d8ade18387e252af469ed38aa4532ba9d19727a575b5fc51f6d5ea8223","case_number":"25-0001","reason":"investigation","upload_id":2})").object();
        CHECK(searchUid(row) == "hibf:3679b82b23a3ba844fe55986", "search uid = hibf:+24 hex of the key fields: %s", qPrintable(searchUid(row)));
        CHECK(passUid(QStringLiteral("c"), 120000) == "pass:c:2", "unix minute");
    }
    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
