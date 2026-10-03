// SPDX-License-Identifier: Apache-2.0
// Unit tests for road snapping (docs/SIGHTINGS.md §2.6) on synthetic geometry around 40.0 / -75.0: the Overpass
// parser, the watched way, a correct pass, a parallel road, an overpass (bridge), the other carriageway of a dual
// carriageway, a nearby lane within the axis tolerance, a coarse (Wi-Fi) track, and no roads at all.
#include "../src/roadsnap.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } \
    else std::printf("ok   " fmt "\n", ##__VA_ARGS__); \
} while (0)

using namespace RoadSnap;
using PlateEvents::TrackFix;

static constexpr double kLat0 = 40.0, kLon0 = -75.0;
// a point dx metres east, dy metres north of the origin
static Pt at(double dx, double dy) { return {kLat0 + dy / 111194.93, kLon0 + dx / (111194.93 * std::cos(kLat0 * M_PI / 180.0))}; }

static Way road(qint64 id, double x0, double y0, double x1, double y1, qint64 firstNode, int layer = 0, int oneway = 0, bool bridge = false)
{
    Way w; w.id = id; w.highway = QStringLiteral("secondary"); w.layer = layer; w.oneway = oneway; w.bridge = bridge;
    for (int i = 0; i <= 6; ++i) {
        const double u = i / 6.0;
        w.pts << at(x0 + u * (x1 - x0), y0 + u * (y1 - y0));
        w.nodes << firstNode + i;
    }
    return w;
}

// A straight drive from (x0, y0) to (x1, y1) at 15 m/s, one fix a second
static QList<TrackFix> drive(double x0, double y0, double x1, double y1, double acc = 5.0, qint64 t0 = 1790000000000LL)
{
    QList<TrackFix> out;
    const double len = std::hypot(x1 - x0, y1 - y0);
    const int n = int(len / 15.0);
    for (int i = 0; i <= n; ++i) {
        const double u = double(i) / n;
        const Pt p = at(x0 + u * (x1 - x0), y0 + u * (y1 - y0));
        TrackFix f; f.ms = t0 + qint64(i) * 1000; f.lat = p.lat; f.lon = p.lon; f.acc = acc; f.source = QStringLiteral("gps");
        out << f;
    }
    return out;
}

static PlateEvents::Camera camera(double x, double y, const QString &dir, const QString &model = QStringLiteral("Falcon"))
{
    PlateEvents::Camera c;
    const Pt p = at(x, y);
    c.id = QStringLiteral("osm:node/1"); c.lat = p.lat; c.lon = p.lon; c.direction = dir; c.model = model; c.type = QStringLiteral("alpr");
    c.source = QStringLiteral("deflock");
    return c;
}

struct Outcome { Result r; PlateEvents::Pass p; bool havePass = false; };
static Outcome run(const QList<TrackFix> &track, const PlateEvents::Camera &cam, const QList<Way> &ways)
{
    Outcome o;
    const QList<PlateEvents::Pass> ps = PlateEvents::detectPasses(track, cam);
    if (ps.isEmpty()) return o;
    o.havePass = true;
    o.p = ps.first();
    const Watched w = watchedWay(ways, cam.lat, cam.lon, PlateEvents::parseDirections(PlateEvents::directionText(cam)));
    int closest = 0;
    const QList<Sample> smp = samplesFor(track, o.p, cam.lat, cam.lon, &closest);
    o.r = snap(smp, closest, ways, w);
    return o;
}

int main()
{
    // ── the Overpass answer ──
    {
        const QByteArray json = R"({"elements":[
          {"type":"way","id":11,"nodes":[1,2],"tags":{"highway":"primary","oneway":"yes","name":"Example Pike"},
           "geometry":[{"lat":40.0,"lon":-75.0},{"lat":40.0,"lon":-74.999}]},
          {"type":"way","id":12,"nodes":[3,4],"tags":{"highway":"secondary","bridge":"yes"},
           "geometry":[{"lat":40.0,"lon":-75.0},{"lat":40.001,"lon":-75.0}]},
          {"type":"way","id":13,"nodes":[5,6],"tags":{"highway":"footway"},"geometry":[{"lat":40.0,"lon":-75.0},{"lat":40.001,"lon":-75.0}]},
          {"type":"way","id":14,"nodes":[7,8],"tags":{"highway":"motorway_link","layer":"-1","tunnel":"yes"},
           "geometry":[{"lat":40.0,"lon":-75.0},{"lat":40.001,"lon":-75.001}]},
          {"type":"node","id":1,"lat":40.0,"lon":-75.0}]})";
        QString err;
        const QList<Way> ws = parseOverpass(json, &err);
        CHECK(err.isEmpty() && ws.size() == 3, "Overpass: 3 motor roads (footway and node dropped), got %d", int(ws.size()));
        if (ws.size() == 3) {
            CHECK(ws[0].oneway == 1 && ws[0].layer == 0 && ws[0].name == QLatin1String("Example Pike"), "oneway=yes, layer 0, name");
            CHECK(ws[1].bridge && ws[1].layer == 1, "a bridge without a layer tag is layer 1");
            CHECK(ws[2].tunnel && ws[2].layer == -1 && ws[2].oneway == 1, "motorway_link: implied oneway; tunnel layer -1");
            const Way back = fromJson(toJson(ws[0]));
            CHECK(back.id == 11 && back.nodes == ws[0].nodes && back.pts.size() == 2 && back.oneway == 1, "the cache row round-trips");
        }
        CHECK(parseOverpass("not json", &err).isEmpty() && !err.isEmpty(), "junk → error");
        CHECK(overpassQuery(40.0, -75.0).contains(QLatin1String("around:80,40.0000000,-75.0000000")), "query: 80 m around the camera");
        CHECK(isMotorRoad(QStringLiteral("residential")) && isMotorRoad(QStringLiteral("trunk_link")) && !isMotorRoad(QStringLiteral("cycleway")), "motor-road filter");
    }

    // The scene: Main St (A) east–west through the origin; a parallel road (B) 22 m south; a bridge (C) north–south at x = 10
    const Way A = road(1001, -400, 0, 400, 0, 100);
    const Way B = road(1002, -400, -22, 400, -22, 200);
    const Way C = road(1003, 10, -400, 10, 400, 300, 1, 0, true);
    const QList<Way> ways{A, B, C};

    // ── the watched way ──
    {
        const PlateEvents::Camera cam = camera(0, 6, QStringLiteral("90"));
        const Watched w = watchedWay(ways, cam.lat, cam.lon, {90.0});
        CHECK(w.wayId == 1001 && w.basis == QLatin1String("direction") && std::fabs(w.distM - 6.0) < 0.5, "camera looking east 6 m off Main St watches it (got %lld, %s)", (long long)w.wayId, qPrintable(w.basis));
        const Watched n = watchedWay(ways, cam.lat, cam.lon, {});
        CHECK(n.wayId == 1001 && n.basis == QLatin1String("nearest"), "no direction: the nearest way");
        const PlateEvents::Camera cam2 = camera(4, 6, QStringLiteral("0"));
        const Watched m = watchedWay(ways, cam2.lat, cam2.lon, {0.0});
        CHECK(m.wayId == 1003 && m.basis == QLatin1String("direction"), "camera looking north next to the bridge: watches the bridge (no ground road runs north)");
        const Watched far = watchedWay(ways, at(200, 200).lat, at(200, 200).lon, {90.0});
        CHECK(far.wayId == 0 && far.basis == QLatin1String("none"), "nothing within 40 m: no watched way");
    }

    // ── a correct pass: eastbound on Main St ──
    {
        const PlateEvents::Camera cam = camera(0, 6, QStringLiteral("90"));
        const Outcome o = run(drive(-200, 0, 200, 0), cam, ways);
        CHECK(o.havePass, "a pass on Main St");
        CHECK(o.r.status == QLatin1String("ok") && o.r.verdict == QLatin1String("on_watched_way") && o.r.factor > 0.99,
              "on the watched way: factor %.3f (%s)", o.r.factor, qPrintable(o.r.verdict));
        CHECK(o.r.matchedWay == 1001 && o.r.layerVerdict == QLatin1String("same"), "matched Main St, same layer");
        CHECK(o.r.samples >= 10, "the fixes within 100 m and ±90 s are matched (%d)", o.r.samples);
    }

    // ── a parallel road 22 m south (direction unknown, so the range alone made it a pass) ──
    {
        const PlateEvents::Camera cam = camera(0, 6, QString(), QString());   // unknown fixed ALPR: 5–35 m, no direction
        const Outcome o = run(drive(-200, -22, 200, -22), cam, ways);
        CHECK(o.havePass && o.p.pRead > 0, "the parallel road is within 65 m and in range: a pass with P(read) %.3f", o.p.pRead);
        CHECK(o.r.verdict == QLatin1String("parallel_road") && std::fabs(o.r.factor - kOffFactor) < 0.01,
              "parallel road: factor %.3f (%s)", o.r.factor, qPrintable(o.r.verdict));
        CHECK(o.r.matchedWay == 1002 && std::fabs(o.r.offsetM - 22.0) < 1.0, "matched the parallel road, %.1f m off the watched axis", o.r.offsetM);
        PlateEvents::Pass p = o.p;
        const double before = p.pRead;
        p.snapFactor = o.r.factor;
        PlateEvents::rescore(p, cam);
        CHECK(std::fabs(p.pRead - before * o.r.factor) < 1e-9 && p.confidence <= int(std::lround(100 * before * 0.1)) + 1,
              "P(read) × 0.1 → confidence %d (was %d)", p.confidence, int(std::lround(100 * before)));
        QJsonObject m{{"pInCone", o.p.pInCone}};
        const int c = PlateEvents::rescoreMetrics(m, QStringLiteral("alpr"), o.r.factor, o.r.toJson(), 1.0, QJsonObject());
        CHECK(c == p.confidence && m.value(QLatin1String("snap")).toObject().value(QLatin1String("verdict")).toString() == QLatin1String("parallel_road"),
              "a stored pass rescored the same way (metrics.snap kept)");
    }

    // ── dense GPS on a road 19 m from the watched one: decided by the whole pass, not one instant ──
    {
        const Way B19 = road(1005, -400, -19, 400, -19, 700);
        const PlateEvents::Camera cam = camera(0, 8, QString(), QString());
        const Outcome o = run(drive(-200, -19, 200, -19, 7.5), cam, {A, B19});
        CHECK(o.r.verdict == QLatin1String("parallel_road") && o.r.factor < 0.15 && o.r.samples > 10,
              "%d samples 19 m off the watched road: factor %.3f (%s)", o.r.samples, o.r.factor, qPrintable(o.r.verdict));
    }
    // ── turning onto the watched road just before the camera: still read ──
    {
        const Way side = road(1007, -30, 300, -30, 0, 900);
        const PlateEvents::Camera cam = camera(0, 6, QStringLiteral("90"));
        QList<TrackFix> t = drive(-30, 150, -30, 0);
        const QList<TrackFix> east = drive(-30, 0, 200, 0, 5.0, t.last().ms + 1000);
        t += east.mid(1);
        const Outcome o = run(t, cam, {A, side});
        CHECK(o.havePass && o.r.factor > 0.9 && o.r.matchedWay == 1001, "turned onto Main St 30 m before the camera: factor %.3f (%s)", o.r.factor, qPrintable(o.r.verdict));
    }
    // ── a driveway next to the road: the public road is the watched one ──
    {
        Way drive1 = road(1006, -400, 4, 400, 4, 800);
        drive1.highway = QStringLiteral("service");
        const PlateEvents::Camera cam = camera(0, 8, QStringLiteral("90"));
        const Watched w = watchedWay({A, drive1}, cam.lat, cam.lon, {90.0});
        CHECK(w.wayId == 1001, "a service way 4 m away does not beat the road 8 m away");
    }

    // ── an overpass: northbound on the bridge over Main St, through the camera's cone ──
    {
        const PlateEvents::Camera cam = camera(0, 6, QStringLiteral("90"));
        const Outcome o = run(drive(10, -200, 10, 200), cam, ways);
        CHECK(o.havePass && o.p.pInCone > 0, "the bridge crosses the cone: P(in cone) %.3f", o.p.pInCone);
        CHECK(o.r.verdict == QLatin1String("different_layer") && o.r.factor < 0.15 && o.r.layerVerdict == QLatin1String("different"),
              "overpass: factor %.3f (%s, layer %s)", o.r.factor, qPrintable(o.r.verdict), qPrintable(o.r.layerVerdict));
        CHECK(o.r.matchedWay == 1003 && o.r.matchedLayer == 1 && o.r.watchedLayer == 0, "matched the bridge (layer 1) over the watched road (layer 0)");
    }

    // ── a dual carriageway: the camera reads the westbound side; we drove east on the other ──
    {
        const Way east = road(2001, -400, 0, 400, 0, 400, 0, 1);       // eastbound (node order west → east)
        const Way west = road(2002, 400, -9, -400, -9, 500, 0, 1);     // westbound (node order east → west), 9 m south
        const QList<Way> dual{east, west};
        const PlateEvents::Camera cam = camera(-20, -14, QStringLiteral("270"));
        const Watched w = watchedWay(dual, cam.lat, cam.lon, {270.0});
        CHECK(w.wayId == 2002 && w.oneway == 1, "watches the westbound carriageway 5 m away");
        const Outcome o = run(drive(-200, 0, 200, 0), cam, dual);
        CHECK(o.r.verdict == QLatin1String("opposite_direction") && o.r.oppositeOneway && o.r.factor < 0.15,
              "eastbound on the other carriageway (9 m, inside the 12 m axis tolerance): factor %.3f (%s)", o.r.factor, qPrintable(o.r.verdict));
        const Outcome ok = run(drive(200, -9, -200, -9), cam, dual);
        CHECK(ok.r.verdict == QLatin1String("on_watched_way") && ok.r.factor > 0.99, "westbound on the watched carriageway: factor %.3f", ok.r.factor);
    }

    // ── a lane 8 m off the watched axis (a slip lane, a split way): still the watched road ──
    {
        const Way E = road(1004, -400, -8, 400, -8, 600);
        const QList<Way> ws{A, E};
        const PlateEvents::Camera cam = camera(0, 6, QStringLiteral("90"));
        const Outcome o = run(drive(-200, -8, 200, -8), cam, ws);
        CHECK(o.r.factor > 0.95 && (o.r.verdict == QLatin1String("near_axis") || o.r.verdict == QLatin1String("on_watched_way")),
              "8 m off the axis: factor %.3f (%s)", o.r.factor, qPrintable(o.r.verdict));
    }

    // ── a coarse track (Wi-Fi, 80 m): the parallel road is not decided ──
    {
        const PlateEvents::Camera cam = camera(0, 6, QString(), QString());
        const Outcome o = run(drive(-200, -22, 200, -22, 80.0), cam, ways);
        CHECK(o.r.verdict == QLatin1String("ambiguous") && o.r.factor > 0.3 && o.r.factor < 0.9, "80 m fixes: ambiguous, factor %.3f", o.r.factor);
    }

    // ── nothing to match against ──
    {
        const PlateEvents::Camera cam = camera(0, 6, QStringLiteral("90"));
        const Outcome o = run(drive(-200, 0, 200, 0), cam, {});
        CHECK(o.r.status == QLatin1String("no_roads") && o.r.factor == 1.0, "no roads: no change");
        const QJsonObject j = o.r.toJson();
        CHECK(j.value(QLatin1String("status")).toString() == QLatin1String("no_roads") && !j.contains(QLatin1String("verdict")), "metrics.snap for no roads");
    }

    std::printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
