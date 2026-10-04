// SPDX-License-Identifier: Apache-2.0
// Unit tests for camera-avoidance routing (docs/SIGHTINGS.md §8): the avoid polygons (cones, discs), the corridor and the
// provider caps, both providers' request bodies, their answers (mocked: no API key is used here), the cameras a
// route still passes, and RoutePlanner's whole flow over a fake transport — including "no key, nothing sent".
#include "../src/avoidroute.h"
#include "../src/routeplanner.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } \
    else std::printf("ok   " fmt "\n", ##__VA_ARGS__); \
} while (0)

using namespace AvoidRoute;

static constexpr double kLat0 = 40.0, kLon0 = -75.0;
static LatLon at(double dx, double dy) { return {kLat0 + dy / 111194.93, kLon0 + dx / (111194.93 * std::cos(kLat0 * M_PI / 180.0))}; }

static Cam cam(const QString &id, double dx, double dy, QList<double> dirs, const QString &model = QStringLiteral("Falcon"))
{
    PlateEvents::Camera c;
    const LatLon p = at(dx, dy);
    c.id = id; c.lat = p.lat; c.lon = p.lon; c.model = model; c.type = QStringLiteral("alpr");
    QStringList d; for (double x : dirs) d << QString::number(x);
    c.direction = d.join(QLatin1Char(';'));
    return fromCamera(c);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // ── polygons ──
    {
        const Cam c = cam(QStringLiteral("osm:node/1"), 0, 0, {90});
        const QList<Area> as = areasFor(c);
        CHECK(as.size() == 1 && !as.first().disc, "one direction: one cone");
        const Ring &r = as.first().ring;
        CHECK(r.first().lat == r.last().lat && r.first().lon == r.last().lon, "the ring is closed");
        CHECK(pointInRing(r, at(15, 0).lat, at(15, 0).lon), "15 m east (in front, inside the Falcon's 25 m) is inside the cone");
        CHECK(!pointInRing(r, at(-15, 0).lat, at(-15, 0).lon) && !pointInRing(r, at(0, 15).lat, at(0, 15).lon), "behind it and to the side are not");
        CHECK(!pointInRing(r, at(40, 0).lat, at(40, 0).lon), "beyond the range is not");
        const double half = (10.0 + PlateEvents::kConeMarginDeg) * M_PI / 180.0;
        const double sector = half * 25.0 * 25.0;   // ½ r² · 2θ
        CHECK(as.first().areaM2 > sector * 0.8 && as.first().areaM2 < sector * 1.4, "cone area %.0f m² ≈ the sector's %.0f m²", as.first().areaM2, sector);
        const Cam two = cam(QStringLiteral("osm:node/2"), 0, 0, {90, 270});
        CHECK(areasFor(two).size() == 2, "two directions: two cones");
        const Cam none = cam(QStringLiteral("osm:node/3"), 0, 0, {});
        const QList<Area> d = areasFor(none);
        CHECK(d.size() == 1 && d.first().disc && pointInRing(d.first().ring, at(50, 0).lat, at(50, 0).lon) && !pointInRing(d.first().ring, at(70, 0).lat, at(70, 0).lon),
              "no direction: a 60 m disc");
        CHECK(std::fabs(d.first().areaM2 - M_PI * 3600) / (M_PI * 3600) < 0.06, "disc area ≈ π·60² (%.0f m²)", d.first().areaM2);
    }

    // ── corridor and caps ──
    const LatLon A = at(0, 0), B = at(10000, 0);                  // 10 km east
    {
        CHECK(std::fabs(corridorM(A, B) - 2000) < 1, "corridor: 20 %% of 10 km = 2 km");
        CHECK(corridorM(A, at(1000, 0)) == kMinCorridorM, "short trips: at least 1.5 km");
        QList<Cam> cams{cam(QStringLiteral("osm:node/10"), 5000, 30, {90}), cam(QStringLiteral("osm:node/11"), 5000, 1900, {}),
                        cam(QStringLiteral("osm:node/12"), 5000, 2500, {90}), cam(QStringLiteral("osm:node/13"), -3000, 0, {90})};
        const Plan p = plan(cams, A, B, Provider::Ors);
        CHECK(p.corridor.size() == 2 && p.areas.size() == 2 && p.capped == 0, "2 of 4 cameras in the corridor (2.5 km off and behind A are not)");
        CHECK(p.areas.first().cameraId == QLatin1String("osm:node/10"), "nearest the A–B line first");
        QList<Cam> many;
        for (int i = 0; i < 150; ++i) many << cam(QStringLiteral("osm:node/%1").arg(100 + i), 50.0 * i, 10, {});
        const Plan pg = plan(many, A, B, Provider::GraphHopper), po = plan(many, A, B, Provider::Ors);
        CHECK(pg.areas.size() == limits(Provider::GraphHopper).maxAreas && pg.capped == 150 - limits(Provider::GraphHopper).maxAreas,
              "GraphHopper capped at %d areas (%d cameras left out)", int(pg.areas.size()), pg.capped);
        CHECK(int(po.areas.size()) <= limits(Provider::Ors).maxAreas && po.capped > 0, "ORS capped at %d areas", int(po.areas.size()));
    }

    // ── request bodies ──
    const QList<Area> two = areasFor(cam(QStringLiteral("osm:node/20"), 5000, 0, {90})) + areasFor(cam(QStringLiteral("osm:node/21"), 7000, 0, {}));
    {
        const QJsonObject o = QJsonDocument::fromJson(requestBody(Provider::Ors, A, B, two)).object();
        const QJsonArray coords = o.value(QLatin1String("coordinates")).toArray();
        CHECK(coords.size() == 2 && std::fabs(coords[0].toArray()[0].toDouble() - A.lon) < 1e-9, "ORS: [lon, lat] coordinates");
        const QJsonObject ap = o.value(QLatin1String("options")).toObject().value(QLatin1String("avoid_polygons")).toObject();
        CHECK(ap.value(QLatin1String("type")).toString() == QLatin1String("MultiPolygon") && ap.value(QLatin1String("coordinates")).toArray().size() == 2,
              "ORS: options.avoid_polygons is a MultiPolygon of 2");
        const QJsonArray ring = ap.value(QLatin1String("coordinates")).toArray()[0].toArray()[0].toArray();
        CHECK(ring.size() >= 4 && ring.first() == ring.last(), "ORS: each polygon is one closed ring");
        CHECK(requestUrl(Provider::Ors, QStringLiteral("k")).toString().endsWith(QLatin1String("/v2/directions/driving-car/geojson")), "ORS endpoint");
        bool auth = false;
        for (const auto &h : requestHeaders(Provider::Ors, QStringLiteral("secret"))) if (h.first == "Authorization" && h.second == "secret") auth = true;
        CHECK(auth, "ORS: the key goes in the Authorization header");

        const QJsonObject g = QJsonDocument::fromJson(requestBody(Provider::GraphHopper, A, B, two)).object();
        const QJsonObject cm = g.value(QLatin1String("custom_model")).toObject();
        const QJsonArray feats = cm.value(QLatin1String("areas")).toObject().value(QLatin1String("features")).toArray();
        CHECK(feats.size() == 2 && feats[0].toObject().value(QLatin1String("id")).toString() == QLatin1String("cam_0"), "GraphHopper: custom_model.areas features cam_0, cam_1");
        const QJsonObject pr = cm.value(QLatin1String("priority")).toArray().first().toObject();
        CHECK(pr.value(QLatin1String("if")).toString() == QLatin1String("in_cam_0 || in_cam_1") && pr.value(QLatin1String("multiply_by")).toString() == QLatin1String("0"),
              "GraphHopper: priority × 0 inside the areas");
        CHECK(g.value(QLatin1String("profile")).toString() == QLatin1String("car") && g.value(QLatin1String("points_encoded")).toBool(true) == false, "GraphHopper: car, plain points");
        CHECK(QUrlQuery(requestUrl(Provider::GraphHopper, QStringLiteral("abc")).query()).queryItemValue(QStringLiteral("key")) == QLatin1String("abc"), "GraphHopper: key in the query");
        CHECK(!QJsonDocument::fromJson(requestBody(Provider::Ors, A, B, {})).object().contains(QLatin1String("options")), "nothing to avoid: no options");
    }

    // ── answers (mocked) ──
    // the route: east along y = 0, then a detour 300 m north around x = 5000, back to y = 0, on to B
    const QList<LatLon> detour{at(0, 0), at(4800, 0), at(4800, 300), at(5200, 300), at(5200, 0), at(7000, 0), at(10000, 0)};
    auto lineJson = [](const QList<LatLon> &pts) { QJsonArray a; for (const LatLon &p : pts) a.append(QJsonArray{p.lon, p.lat}); return a; };
    const QByteArray orsOk = QJsonDocument(QJsonObject{{"type", "FeatureCollection"}, {"features", QJsonArray{QJsonObject{
        {"type", "Feature"}, {"geometry", QJsonObject{{"type", "LineString"}, {"coordinates", lineJson(detour)}}},
        {"properties", QJsonObject{{"summary", QJsonObject{{"distance", 10600.0}, {"duration", 720.0}}}}}}}}}).toJson();
    const QByteArray ghOk = QJsonDocument(QJsonObject{{"paths", QJsonArray{QJsonObject{{"distance", 10600.0}, {"time", 720000.0},
        {"points", QJsonObject{{"type", "LineString"}, {"coordinates", lineJson(detour)}}}}}}}).toJson();
    {
        const Route r = parseResponse(Provider::Ors, 200, orsOk);
        CHECK(r.ok && r.points.size() == 7 && r.distanceM == 10600 && r.durationS == 720, "ORS answer parsed");
        const QByteArray orsWithSteps = QJsonDocument(QJsonObject{{"type", "FeatureCollection"}, {"features", QJsonArray{QJsonObject{
            {"type", "Feature"}, {"geometry", QJsonObject{{"type", "LineString"}, {"coordinates", lineJson(detour)}}},
            {"properties", QJsonObject{
                {"summary", QJsonObject{{"distance", 10600.0}, {"duration", 720.0}}},
                {"segments", QJsonArray{QJsonObject{
                    {"steps", QJsonArray{
                        QJsonObject{{"distance", 4800.0}, {"duration", 300.0}, {"type", 10}, {"instruction", "Head east on Main St"}, {"name", "Main St"}, {"way_points", QJsonArray{0, 1}}},
                        QJsonObject{{"distance", 300.0}, {"duration", 30.0}, {"type", 0}, {"instruction", "Turn left onto Oak St avoiding ALPR"}, {"name", "Oak St"}, {"way_points", QJsonArray{1, 2}}}
                    }}
                }}}
            }}}}}}).toJson();
        const Route rSteps = parseResponse(Provider::Ors, 200, orsWithSteps);
        CHECK(rSteps.ok && rSteps.steps.size() == 2 && rSteps.steps[1].instruction.contains(QLatin1String("avoiding ALPR")), "turn-by-turn steps parsed correctly from ORS");
        const Route g = parseResponse(Provider::GraphHopper, 200, ghOk);
        CHECK(g.ok && g.points.size() == 7 && std::fabs(g.durationS - 720) < 1e-9, "GraphHopper answer parsed (time in ms)");
        const Route e = parseResponse(Provider::Ors, 400, R"({"error":{"code":2004,"message":"Request parameters exceed the server configuration limits."}})");
        CHECK(!e.ok && e.error.contains(QLatin1String("exceed")), "ORS error message: %s", qPrintable(e.error));
        const Route k = parseResponse(Provider::GraphHopper, 401, R"({"message":"Wrong credentials. Register and get a valid API key"})");
        CHECK(!k.ok && k.error.contains(QLatin1String("refused the API key")), "a refused key says so: %s", qPrintable(k.error));
    }

    // ── what the route still passes ──
    {
        QList<Cam> cams{cam(QStringLiteral("osm:node/30"), 5000, 0, {90}),      // on the avoided stretch: not passed
                        cam(QStringLiteral("osm:node/31"), 7500, 3, {90}),      // looks east along the route at 7.5 km: passed, in the cone
                        cam(QStringLiteral("osm:node/32"), 8500, 20, {10}),     // 20 m off the route, looking north: near, not facing
                        cam(QStringLiteral("osm:node/33"), 9000, 40, {})};      // no direction, 40 m off: inside its 60 m disc
        const QList<Passed> p = camerasPassed(detour, cams);
        QStringList ids; for (const Passed &x : p) ids << x.cam.id + (x.inCone ? QStringLiteral("+") : QStringLiteral("-"));
        CHECK(ids == (QStringList{QStringLiteral("osm:node/31+"), QStringLiteral("osm:node/32-"), QStringLiteral("osm:node/33+")}),
              "passed: %s", qPrintable(ids.join(QLatin1Char(' '))));
    }

    // ── RoutePlanner over a fake transport ──
    {
        QList<Cam> world{cam(QStringLiteral("osm:node/40"), 5000, 0, {90}), cam(QStringLiteral("osm:node/41"), 7500, 3, {90})};
        int sent = 0;
        QByteArray lastBody; QUrl lastUrl;
        RoutePlanner rp([&](double, double, double, double) { return world; });
        rp.setTransport([&](const QUrl &u, const QList<QPair<QByteArray, QByteArray>> &, const QByteArray &body, RoutePlanner::Reply reply) {
            ++sent; lastUrl = u; lastBody = body;
            reply(200, u.host().contains(QLatin1String("graphhopper")) ? ghOk : orsOk, QString());
        });
        QString orsKey, ghKey;
        rp.setKeySource([&](Provider p) { return p == Provider::Ors ? orsKey : ghKey; });
        int code = 0; QJsonObject res;
        auto done = [&](int c, const QJsonObject &o) { code = c; res = o; };
        rp.route(A, B, QString(), done);
        CHECK(code == 412 && res.value(QLatin1String("needsKey")).toBool() && sent == 0, "no key: 412, says why, nothing sent (%s)",
              qPrintable(res.value(QLatin1String("error")).toString()));
        CHECK(!rp.status().value(QLatin1String("ready")).toBool(), "status: not ready without a key");
        ghKey = QStringLiteral("gh-test");
        rp.route(A, B, QStringLiteral("ors"), done);
        CHECK(code == 412 && sent == 0, "asking for ORS without its key: 412");
        rp.route(A, B, QString(), done);
        CHECK(code == 200 && sent == 1 && lastUrl.host() == QLatin1String("graphhopper.com") && res.value(QLatin1String("provider")).toString() == QLatin1String("graphhopper"),
              "only a GraphHopper key: GraphHopper is used");
        CHECK(res.value(QLatin1String("avoided")).toObject().value(QLatin1String("cameras")).toInt() == 2 && lastBody.contains("in_cam_1"), "both corridor cameras avoided");
        const QJsonArray passes = res.value(QLatin1String("passes")).toArray();
        CHECK(passes.size() == 1 && passes[0].toObject().value(QLatin1String("id")).toString() == QLatin1String("osm:node/41"),
              "the result lists the camera the route still passes");
        CHECK(res.value(QLatin1String("route")).toObject().value(QLatin1String("coordinates")).toArray().size() == 7
              && res.value(QLatin1String("attribution")).toString().contains(QLatin1String("GraphHopper")), "route geometry and attribution");
        orsKey = QStringLiteral("ors-test");
        rp.route(A, B, QStringLiteral("ors"), done);
        CHECK(code == 200 && lastUrl.host() == QLatin1String("api.openrouteservice.org") && res.value(QLatin1String("distanceM")).toDouble() == 10600, "ORS on request");
        rp.route(A, {91, 0}, QString(), done);
        CHECK(code == 400, "a bad destination: 400");
        rp.route(A, B, QStringLiteral("bing"), done);
        CHECK(code == 400, "an unknown provider: 400");
        rp.setTransport([&](const QUrl &, const QList<QPair<QByteArray, QByteArray>> &, const QByteArray &, RoutePlanner::Reply reply) {
            reply(403, R"({"error":"Access to this API has been disallowed"})", QString());
        });
        rp.route(A, B, QStringLiteral("ors"), done);
        CHECK(code == 412 && res.value(QLatin1String("error")).toString().contains(QLatin1String("refused")), "a refused key: 412 (%s)",
              qPrintable(res.value(QLatin1String("error")).toString()));
        rp.setTransport([&](const QUrl &, const QList<QPair<QByteArray, QByteArray>> &, const QByteArray &, RoutePlanner::Reply reply) {
            reply(200, QByteArray(), QStringLiteral("blocked by NextDNS (DNS filter)"));
        });
        rp.route(A, B, QStringLiteral("ors"), done);
        CHECK(code == 502 && res.value(QLatin1String("error")).toString().contains(QLatin1String("NextDNS")), "a blocked provider: 502, says so");
    }

    // ── inspect unseen (docs/SIGHTINGS.md §9) ──
    {
        const Cam target = cam(QStringLiteral("osm:node/99"), 0, 0, {90}); // Facing East
        const QList<Area> inspectAreas = inspectAvoidRegions({target});
        CHECK(inspectAreas.size() == 2, "inspect avoid regions: cone + 15 m pole disc");
        CHECK(pointInRing(inspectAreas[1].ring, at(-10, 0).lat, at(-10, 0).lon), "10 m behind camera is inside the 15 m pole disc");
        CHECK(!pointInRing(inspectAreas[0].ring, at(-30, 0).lat, at(-30, 0).lon) &&
              !pointInRing(inspectAreas[1].ring, at(-30, 0).lat, at(-30, 0).lon), "30 m behind camera is outside all avoid regions");

        // Vantage points on ways
        RoadSnap::Way behindWay;
        behindWay.id = 1;
        behindWay.highway = QStringLiteral("footway");
        behindWay.pts = {{at(-30, -50).lat, at(-30, -50).lon}, {at(-30, 50).lat, at(-30, 50).lon}};

        RoadSnap::Way fovWay;
        fovWay.id = 2;
        fovWay.highway = QStringLiteral("primary");
        fovWay.pts = {{at(20, -50).lat, at(20, -50).lon}, {at(20, 50).lat, at(20, 50).lon}};

        const QList<Vantage> vantages = findVantages(target, {behindWay, fovWay}, inspectAreas, QStringLiteral("foot"), 20.0, 60.0);
        CHECK(!vantages.isEmpty(), "found vantage candidates on behind way");
        CHECK(vantages.first().side == QLatin1String("behind"), "best vantage is behind camera (side: %s)", qPrintable(vantages.first().side));
        CHECK(std::fabs(vantages.first().distanceM - 30.0) < 5.0, "vantage distance %.1f m ≈ 30 m", vantages.first().distanceM);
        CHECK(std::fabs(vantages.first().bearingToCamera - 90.0) < 15.0, "looking towards camera at bearing ~90° (actual %.1f°)", vantages.first().bearingToCamera);

        // Exposure check
        QList<LatLon> cleanRoute{at(-30, -50), at(-30, 50)};
        QList<Exposure> cleanExp = checkExposures(cleanRoute, {target});
        CHECK(cleanExp.isEmpty(), "route 30 m behind camera has 0 exposures");

        QList<LatLon> dirtyRoute{at(15, -50), at(15, 50)}; // passes through Falcon's 25 m cone
        QList<Exposure> dirtyExp = checkExposures(dirtyRoute, {target});
        CHECK(dirtyExp.size() == 1 && dirtyExp.first().cameraId == QLatin1String("osm:node/99"), "route through cone flagged as exposure");

        // GPX export
        const QString gpx = toGpx(cleanRoute, cleanRoute, vantages.first(), target);
        CHECK(gpx.contains(QLatin1String("<gpx")) && gpx.contains(QLatin1String("ALPR: osm:node/99")) && gpx.contains(QLatin1String("Approach to Vantage")), "valid GPX output generated");

        // RoutePlanner inspect flow
        RoutePlanner rp([&](double, double, double, double) { return QList<Cam>{target}; });
        int inspectCode = 0; QJsonObject inspectRes;
        rp.inspect(target, at(-100, -100), std::nullopt, QStringLiteral("foot"), 20.0, 60.0, QString(), {behindWay}, QJsonArray(),
                   [&](int c, const QJsonObject &o) { inspectCode = c; inspectRes = o; });
        CHECK(inspectCode == 200 && inspectRes.value(QLatin1String("safe")).toBool() && !inspectRes.value(QLatin1String("vantages")).toArray().isEmpty(),
              "inspect without API key returns 200 with safe vantages and note");
    }

    std::printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
