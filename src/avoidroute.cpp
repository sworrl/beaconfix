// SPDX-License-Identifier: Apache-2.0
#include "avoidroute.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>

namespace AvoidRoute {

static constexpr double kEarthR = 6371000.0;
static constexpr double kD2R = M_PI / 180.0;

namespace {
struct Xy { double x = 0, y = 0; };
struct Proj {
    double lat0, lon0, kx;
    Proj(double lat, double lon) : lat0(lat), lon0(lon), kx(std::cos(lat * kD2R) * kEarthR * kD2R) {}
    Xy to(double lat, double lon) const { return {(lon - lon0) * kx, (lat - lat0) * kEarthR * kD2R}; }
    LatLon back(double x, double y) const { return {lat0 + y / (kEarthR * kD2R), lon0 + x / kx}; }
};
double signedArea(const Ring &r)
{
    if (r.size() < 3) return 0;
    const Proj pj(r.first().lat, r.first().lon);
    double a = 0;
    for (int i = 0; i + 1 < int(r.size()); ++i) {
        const Xy p = pj.to(r[i].lat, r[i].lon), q = pj.to(r[i + 1].lat, r[i + 1].lon);
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2.0;
}
Ring closeCcw(Ring r)
{
    if (r.isEmpty()) return r;
    if (r.first().lat != r.last().lat || r.first().lon != r.last().lon) r.append(r.first());
    if (signedArea(r) < 0) std::reverse(r.begin(), r.end());
    return r;
}
}

Cam fromCamera(const PlateEvents::Camera &c)
{
    Cam k;
    k.id = c.id; k.operatorName = c.operatorName; k.model = c.model; k.lat = c.lat; k.lon = c.lon;
    k.dirs = PlateEvents::parseDirections(PlateEvents::directionText(c));
    k.cone = PlateEvents::coneFor(c);
    k.trust = c.trust >= 0 ? c.trust : 1.0;
    return k;
}

Ring conePolygon(double lat, double lon, double dirDeg, double halfDeg, double rangeM, int arcPoints)
{
    const Proj pj(lat, lon);
    const double t = dirDeg * kD2R;
    Ring r;
    r.append(pj.back(-std::sin(t) * kApexBackM, -std::cos(t) * kApexBackM));
    arcPoints = std::max(2, arcPoints);
    for (int i = 0; i <= arcPoints; ++i) {
        const double b = (dirDeg - halfDeg + 2.0 * halfDeg * double(i) / double(arcPoints)) * kD2R;
        r.append(pj.back(std::sin(b) * rangeM, std::cos(b) * rangeM));
    }
    return closeCcw(r);
}

Ring discPolygon(double lat, double lon, double radiusM, int points)
{
    const Proj pj(lat, lon);
    Ring r;
    points = std::max(6, points);
    for (int i = 0; i < points; ++i) {
        const double b = 2.0 * M_PI * double(i) / double(points);
        r.append(pj.back(std::sin(b) * radiusM, std::cos(b) * radiusM));
    }
    return closeCcw(r);
}

double ringAreaM2(const Ring &r) { return std::fabs(signedArea(r)); }

bool pointInRing(const Ring &r, double lat, double lon)
{
    bool in = false;
    for (int i = 0, j = int(r.size()) - 1; i < int(r.size()); j = i++) {
        const LatLon &a = r[i], &b = r[j];
        if ((a.lat > lat) != (b.lat > lat) && lon < (b.lon - a.lon) * (lat - a.lat) / (b.lat - a.lat) + a.lon) in = !in;
    }
    return in;
}

QList<Area> areasFor(const Cam &c)
{
    QList<Area> out;
    if (c.dirs.isEmpty()) {
        Area a; a.cameraId = c.id; a.disc = true; a.ring = discPolygon(c.lat, c.lon, kDiscM); a.areaM2 = ringAreaM2(a.ring);
        out << a;
        return out;
    }
    for (double d : c.dirs) {
        Area a; a.cameraId = c.id; a.dirDeg = d;
        a.ring = conePolygon(c.lat, c.lon, d, c.cone.halfDeg + PlateEvents::kConeMarginDeg, c.cone.maxM);
        a.areaM2 = ringAreaM2(a.ring);
        out << a;
    }
    return out;
}

double distanceToSegmentM(LatLon p, LatLon a, LatLon b)
{
    const Proj pj(p.lat, p.lon);
    const Xy A = pj.to(a.lat, a.lon), B = pj.to(b.lat, b.lon);
    const double dx = B.x - A.x, dy = B.y - A.y, l2 = dx * dx + dy * dy;
    const double u = l2 > 0 ? std::clamp(-(A.x * dx + A.y * dy) / l2, 0.0, 1.0) : 0.0;
    return std::hypot(A.x + u * dx, A.y + u * dy);
}

double corridorM(LatLon a, LatLon b)
{
    return std::clamp(0.2 * PlateEvents::distanceM(a.lat, a.lon, b.lat, b.lon), kMinCorridorM, kMaxCorridorM);
}

QString providerId(Provider p) { return p == Provider::Ors ? QStringLiteral("ors") : QStringLiteral("graphhopper"); }
QString providerName(Provider p) { return p == Provider::Ors ? QStringLiteral("OpenRouteService") : QStringLiteral("GraphHopper"); }
bool providerFromId(const QString &id, Provider *p)
{
    const QString s = id.trimmed().toLower();
    if (s == QLatin1String("ors") || s == QLatin1String("openrouteservice")) { *p = Provider::Ors; return true; }
    if (s == QLatin1String("graphhopper") || s == QLatin1String("gh")) { *p = Provider::GraphHopper; return true; }
    return false;
}

// Conservative caps, below what the providers document or answer with "request too large": ORS checks the avoid
// polygons' total area (200 km² on its public API); GraphHopper's custom model has no published area count limit.
Limits limits(Provider p)
{
    if (p == Provider::Ors) return {100, 2000, 200.0};
    return {60, 1200, 200.0};
}

Plan plan(const QList<Cam> &cams, LatLon a, LatLon b, Provider p)
{
    Plan out;
    out.corridorM = corridorM(a, b);
    QList<QPair<double, int>> order;
    for (int i = 0; i < int(cams.size()); ++i) {
        const double d = distanceToSegmentM({cams[i].lat, cams[i].lon}, a, b);
        if (d <= out.corridorM) order.append({d, i});
    }
    std::sort(order.begin(), order.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
    const Limits lim = limits(p);
    int vertices = 0;
    double km2 = 0;
    for (const auto &o : std::as_const(order)) {
        const Cam &c = cams[o.second];
        out.corridor << c;
        const QList<Area> as = areasFor(c);
        int v = 0; double m2 = 0;
        for (const Area &x : as) { v += int(x.ring.size()); m2 += x.areaM2; }
        if (int(out.areas.size() + as.size()) > lim.maxAreas || vertices + v > lim.maxVertices || km2 + m2 / 1e6 > lim.maxTotalKm2) { ++out.capped; continue; }
        out.areas += as;
        vertices += v; km2 += m2 / 1e6;
    }
    return out;
}

QUrl requestUrl(Provider p, const QString &key, const QString &profile)
{
    if (p == Provider::Ors) {
        if (profile.toLower() == QLatin1String("foot"))
            return QUrl(QStringLiteral("https://api.openrouteservice.org/v2/directions/foot-walking/geojson"));
        return QUrl(QStringLiteral("https://api.openrouteservice.org/v2/directions/driving-car/geojson"));
    }
    QUrl u(QStringLiteral("https://graphhopper.com/api/1/route"));
    QUrlQuery q; q.addQueryItem(QStringLiteral("key"), key);
    u.setQuery(q);
    return u;
}

QList<QPair<QByteArray, QByteArray>> requestHeaders(Provider p, const QString &key)
{
    QList<QPair<QByteArray, QByteArray>> h{{"Content-Type", "application/json"}};
    if (p == Provider::Ors) {
        h.append(qMakePair(QByteArray("Authorization"), key.toUtf8()));
        h.append(qMakePair(QByteArray("Accept"), QByteArray("application/geo+json, application/json")));
    } else h.append(qMakePair(QByteArray("Accept"), QByteArray("application/json")));
    return h;
}

static QJsonArray ringJson(const Ring &r)
{
    QJsonArray a;
    for (const LatLon &p : r) a.append(QJsonArray{std::round(p.lon * 1e6) / 1e6, std::round(p.lat * 1e6) / 1e6});
    return a;
}

QByteArray requestBody(Provider p, LatLon a, LatLon b, const QList<Area> &areas, const QString &profile)
{
    const QJsonArray pts{QJsonArray{a.lon, a.lat}, QJsonArray{b.lon, b.lat}};
    QJsonObject body;
    if (p == Provider::Ors) {
        body = QJsonObject{{"coordinates", pts}, {"instructions", true}, {"units", "m"}};
        if (!areas.isEmpty()) {
            QJsonArray polys;
            for (const Area &x : areas) polys.append(QJsonArray{ringJson(x.ring)});
            body["options"] = QJsonObject{{"avoid_polygons", QJsonObject{{"type", "MultiPolygon"}, {"coordinates", polys}}}};
        }
    } else {
        const QString ghProfile = profile.toLower() == QLatin1String("foot") ? QStringLiteral("foot") : QStringLiteral("car");
        body = QJsonObject{{"points", pts}, {"profile", ghProfile}, {"points_encoded", false}, {"instructions", true}, {"calc_points", true}, {"ch.disable", true}};
        if (!areas.isEmpty()) {
            QJsonArray feats;
            QStringList cond;
            for (int i = 0; i < int(areas.size()); ++i) {
                const QString id = QStringLiteral("cam_%1").arg(i);
                feats.append(QJsonObject{{"type", "Feature"}, {"id", id}, {"properties", QJsonObject()},
                                         {"geometry", QJsonObject{{"type", "Polygon"}, {"coordinates", QJsonArray{ringJson(areas[i].ring)}}}}});
                cond << QStringLiteral("in_%1").arg(id);
            }
            body["custom_model"] = QJsonObject{{"priority", QJsonArray{QJsonObject{{"if", cond.join(QStringLiteral(" || "))}, {"multiply_by", "0"}}}},
                                               {"areas", QJsonObject{{"type", "FeatureCollection"}, {"features", feats}}}};
        }
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

Route parseResponse(Provider p, int http, const QByteArray &bytes)
{
    Route r;
    const QJsonObject o = QJsonDocument::fromJson(bytes).object();
    auto coords = [&](const QJsonArray &a) {
        for (const QJsonValue &v : a) { const QJsonArray c = v.toArray(); if (c.size() >= 2) r.points.append({c[1].toDouble(), c[0].toDouble()}); }
    };
    QString msg;
    if (p == Provider::Ors) {
        const QJsonValue e = o.value(QLatin1String("error"));
        msg = e.isObject() ? e.toObject().value(QLatin1String("message")).toString() : e.toString();
        const QJsonArray f = o.value(QLatin1String("features")).toArray();
        if (http == 200 && !f.isEmpty()) {
            const QJsonObject ft = f.first().toObject();
            coords(ft.value(QLatin1String("geometry")).toObject().value(QLatin1String("coordinates")).toArray());
            const QJsonObject props = ft.value(QLatin1String("properties")).toObject();
            const QJsonObject s = props.value(QLatin1String("summary")).toObject();
            r.distanceM = s.value(QLatin1String("distance")).toDouble();
            r.durationS = s.value(QLatin1String("duration")).toDouble();
            const QJsonArray segments = props.value(QLatin1String("segments")).toArray();
            if (!segments.isEmpty()) {
                const QJsonArray stepsArr = segments.first().toObject().value(QLatin1String("steps")).toArray();
                for (const QJsonValue &sv : stepsArr) {
                    const QJsonObject so = sv.toObject();
                    Step step;
                    step.distanceM = so.value(QLatin1String("distance")).toDouble();
                    step.durationS = so.value(QLatin1String("duration")).toDouble();
                    step.type = so.value(QLatin1String("type")).toInt();
                    step.instruction = so.value(QLatin1String("instruction")).toString();
                    step.streetName = so.value(QLatin1String("name")).toString();
                    const QJsonArray wp = so.value(QLatin1String("way_points")).toArray();
                    if (!wp.isEmpty()) {
                        const int idx = wp.first().toInt();
                        if (idx >= 0 && idx < r.points.size()) step.start = r.points[idx];
                    }
                    r.steps.append(step);
                }
            }
        }
    } else {
        msg = o.value(QLatin1String("message")).toString();
        const QJsonArray paths = o.value(QLatin1String("paths")).toArray();
        if (http == 200 && !paths.isEmpty()) {
            const QJsonObject pa = paths.first().toObject();
            coords(pa.value(QLatin1String("points")).toObject().value(QLatin1String("coordinates")).toArray());
            r.distanceM = pa.value(QLatin1String("distance")).toDouble();
            r.durationS = pa.value(QLatin1String("time")).toDouble() / 1000.0;
            const QJsonArray instrs = pa.value(QLatin1String("instructions")).toArray();
            for (const QJsonValue &iv : instrs) {
                const QJsonObject io = iv.toObject();
                Step step;
                step.distanceM = io.value(QLatin1String("distance")).toDouble();
                step.durationS = io.value(QLatin1String("time")).toDouble() / 1000.0;
                step.type = io.value(QLatin1String("sign")).toInt();
                step.instruction = io.value(QLatin1String("text")).toString();
                step.streetName = io.value(QLatin1String("street_name")).toString();
                const QJsonArray interval = io.value(QLatin1String("interval")).toArray();
                if (!interval.isEmpty()) {
                    const int idx = interval.first().toInt();
                    if (idx >= 0 && idx < r.points.size()) step.start = r.points[idx];
                }
                r.steps.append(step);
            }
        }
    }
    if (r.points.size() >= 2) { r.ok = true; return r; }
    if (http == 401 || http == 403) r.error = QStringLiteral("%1 refused the API key (HTTP %2)%3").arg(providerName(p)).arg(http).arg(msg.isEmpty() ? QString() : QStringLiteral(": ") + msg);
    else if (http == 429) r.error = QStringLiteral("%1 rate limit reached (HTTP 429): try again later").arg(providerName(p));
    else if (!msg.isEmpty()) r.error = QStringLiteral("%1: %2").arg(providerName(p), msg);
    else r.error = QStringLiteral("%1 answered HTTP %2 without a route").arg(providerName(p)).arg(http);
    return r;
}

QList<Passed> camerasPassed(const QList<LatLon> &route, const QList<Cam> &cams)
{
    QList<Passed> out;
    if (route.size() < 2) return out;
    QList<double> cum{0.0};
    for (int i = 1; i < int(route.size()); ++i) cum << cum.last() + PlateEvents::distanceM(route[i - 1].lat, route[i - 1].lon, route[i].lat, route[i].lon);
    for (const Cam &c : cams) {
        const double reach = std::max(kDiscM, c.cone.maxM + kApexBackM) + 5.0;
        double best = 1e18, along = 0;
        for (int i = 0; i + 1 < int(route.size()); ++i) {
            const double d = distanceToSegmentM({c.lat, c.lon}, route[i], route[i + 1]);
            if (d < best) { best = d; along = cum[i]; }
        }
        if (best > reach) continue;
        const QList<Area> areas = areasFor(c);
        bool in = false;
        for (int i = 0; i + 1 < int(route.size()) && !in; ++i) {
            if (distanceToSegmentM({c.lat, c.lon}, route[i], route[i + 1]) > reach) continue;
            const double len = PlateEvents::distanceM(route[i].lat, route[i].lon, route[i + 1].lat, route[i + 1].lon);
            const int n = std::clamp(int(std::ceil(len / 2.0)), 1, 5000);
            for (int k = 0; k <= n && !in; ++k) {
                const double u = double(k) / double(n);
                const double lat = route[i].lat + u * (route[i + 1].lat - route[i].lat), lon = route[i].lon + u * (route[i + 1].lon - route[i].lon);
                for (const Area &a : areas) if (pointInRing(a.ring, lat, lon)) { in = true; break; }
            }
        }
        if (!in && best > PlateEvents::kPassRadiusM) continue;
        out.append({c, best, along, in});
    }
    std::sort(out.begin(), out.end(), [](const Passed &a, const Passed &b) { return a.alongM < b.alongM; });
    return out;
}

QJsonObject toJson(const Route &r, const Plan &plan, const QList<Passed> &passed, Provider p)
{
    QJsonArray coords;
    for (const LatLon &x : r.points) coords.append(QJsonArray{std::round(x.lon * 1e6) / 1e6, std::round(x.lat * 1e6) / 1e6});
    QJsonArray ps;
    int inCone = 0;
    for (const Passed &x : passed) {
        if (x.inCone) ++inCone;
        ps.append(QJsonObject{{"id", x.cam.id}, {"lat", x.cam.lat}, {"lon", x.cam.lon}, {"operator", x.cam.operatorName}, {"model", x.cam.model},
                              {"distanceM", std::round(x.distanceM * 10) / 10}, {"alongM", std::round(x.alongM)}, {"inCone", x.inCone},
                              {"trust", std::round(x.cam.trust * 1000) / 1000}});
    }
    QJsonArray stepsArr;
    for (const Step &s : r.steps) {
        stepsArr.append(QJsonObject{{"distanceM", std::round(s.distanceM)},
                                    {"durationS", std::round(s.durationS)},
                                    {"instruction", s.instruction},
                                    {"streetName", s.streetName},
                                    {"type", s.type},
                                    {"lat", s.start.lat},
                                    {"lon", s.start.lon}});
    }
    QSet<QString> avoided;
    for (const Area &a : plan.areas) avoided.insert(a.cameraId);
    const QString attribution = p == Provider::Ors ? QStringLiteral("© openrouteservice.org by HeiGIT · map data © OpenStreetMap contributors (ODbL)")
                                                   : QStringLiteral("Powered by GraphHopper · map data © OpenStreetMap contributors (ODbL)");
    return QJsonObject{{"provider", providerId(p)}, {"providerName", providerName(p)}, {"route", QJsonObject{{"type", "LineString"}, {"coordinates", coords}}},
                       {"distanceM", std::round(r.distanceM)}, {"durationS", std::round(r.durationS)},
                       {"steps", stepsArr},
                       {"avoided", QJsonObject{{"areas", int(plan.areas.size())}, {"cameras", int(avoided.size())}, {"corridorCameras", int(plan.corridor.size())},
                                               {"capped", plan.capped}, {"corridorM", std::round(plan.corridorM)}}},
                       {"passes", ps}, {"passesInCone", inCone}, {"attribution", attribution}};
}

// ── Inspect a camera unseen (docs/SIGHTINGS.md §9) ──

QList<Area> inspectAvoidRegions(const QList<Cam> &cams)
{
    QList<Area> out;
    for (const Cam &c : cams) {
        const bool isAlpr = (c.cone.maxM > 0 && c.trust > 0);
        if (!isAlpr) {
            // Non-ALPR camera (webcam, CCTV, PTZ): disc of 60 m (§9.1)
            Area a; a.cameraId = c.id; a.disc = true;
            a.ring = discPolygon(c.lat, c.lon, 60.0);
            a.areaM2 = ringAreaM2(a.ring);
            out << a;
            continue;
        }
        if (c.dirs.isEmpty()) {
            // No known direction: a disc of radius range * 1.5 + 10 m (§9.1)
            const double r = c.cone.maxM * 1.5 + 10.0;
            Area a; a.cameraId = c.id; a.disc = true;
            a.ring = discPolygon(c.lat, c.lon, r);
            a.areaM2 = ringAreaM2(a.ring);
            out << a;
        } else {
            // Each direction: cone with half-angle + 15° and range * 1.5 + 10 m
            const double half = c.cone.halfDeg + 15.0;
            const double r = c.cone.maxM * 1.5 + 10.0;
            for (double d : c.dirs) {
                Area a; a.cameraId = c.id; a.dirDeg = d;
                a.ring = conePolygon(c.lat, c.lon, d, half, r);
                a.areaM2 = ringAreaM2(a.ring);
                out << a;
            }
            // Plus 15 m disc around the pole
            Area pole; pole.cameraId = c.id; pole.disc = true;
            pole.ring = discPolygon(c.lat, c.lon, 15.0);
            pole.areaM2 = ringAreaM2(pole.ring);
            out << pole;
        }
    }
    return out;
}

QList<Vantage> findVantages(const Cam &target, const QList<RoadSnap::Way> &ways,
                            const QList<Area> &avoidAreas,
                            const QString &profile,
                            double minM, double maxM)
{
    struct Cand {
        LatLon pt;
        double dist = 0;
        double score = 0;
        double bearingToCam = 0;
        QString side;
        QString highway;
        QString name;
    };
    QList<Cand> cands;
    const bool isFoot = (profile.toLower() == QLatin1String("foot") || profile.toLower() == QLatin1String("walk"));

    auto isWayAccepted = [isFoot](const QString &hw) {
        if (hw.isEmpty()) return true;
        if (isFoot) {
            return RoadSnap::isMotorRoad(hw) || hw == QLatin1String("footway") || hw == QLatin1String("sidewalk") ||
                   hw == QLatin1String("path") || hw == QLatin1String("pedestrian") || hw == QLatin1String("steps") ||
                   hw == QLatin1String("cycleway") || hw == QLatin1String("track");
        }
        if (hw == QLatin1String("steps") || hw == QLatin1String("pedestrian") || hw == QLatin1String("footway")) return false;
        return RoadSnap::isMotorRoad(hw) || hw == QLatin1String("parking") || hw == QLatin1String("service");
    };

    auto evaluatePoint = [&](LatLon pt, const QString &hw, const QString &name) {
        const double d = PlateEvents::distanceM(target.lat, target.lon, pt.lat, pt.lon);
        if (d < minM || d > maxM) return;
        for (const Area &a : avoidAreas) {
            if (pointInRing(a.ring, pt.lat, pt.lon)) return;
        }
        double sc = 0;
        // Distance score: preferred 25–45 m band
        if (d < 25.0) sc += (25.0 - d);
        else if (d > 45.0) sc += (d - 45.0);

        // Bearing camera -> point
        const double bCamToPt = PlateEvents::bearingDeg(target.lat, target.lon, pt.lat, pt.lon);
        QString side = QStringLiteral("behind");
        if (!target.dirs.isEmpty()) {
            double bestAngleScore = 1000.0;
            QString bestSide = QStringLiteral("front");
            for (double dir : target.dirs) {
                const double opp = std::fmod(dir + 180.0, 360.0);
                const double diffOpp = PlateEvents::angleDiff(bCamToPt, opp);
                const double diffDir = PlateEvents::angleDiff(bCamToPt, dir);
                if (diffOpp <= 90.0) {
                    const double s = diffOpp * 0.01;
                    if (s < bestAngleScore) { bestAngleScore = s; bestSide = QStringLiteral("behind"); }
                } else if (diffDir > 90.0) {
                    const double s = 10.0 + (180.0 - diffDir) * 0.01;
                    if (s < bestAngleScore) { bestAngleScore = s; bestSide = QStringLiteral("beside"); }
                } else {
                    const double s = 25.0 + diffDir * 0.01;
                    if (s < bestAngleScore) { bestAngleScore = s; bestSide = QStringLiteral("front"); }
                }
            }
            sc += bestAngleScore;
            side = bestSide;
        } else {
            side = QStringLiteral("any");
        }

        if (hw == QLatin1String("service")) sc += 10.0;
        if (hw == QLatin1String("sidewalk") || hw == QLatin1String("footway") || hw == QLatin1String("path") ||
            hw == QLatin1String("pedestrian") || hw == QLatin1String("parking")) {
            sc -= 5.0;
        }
        const double bToCam = PlateEvents::bearingDeg(pt.lat, pt.lon, target.lat, target.lon);
        cands.append({pt, d, sc, bToCam, side, hw, name});
    };

    for (const RoadSnap::Way &w : ways) {
        if (!isWayAccepted(w.highway)) continue;
        for (int i = 0; i + 1 < w.pts.size(); ++i) {
            const RoadSnap::Pt &p1 = w.pts[i], &p2 = w.pts[i + 1];
            const double segLen = PlateEvents::distanceM(p1.lat, p1.lon, p2.lat, p2.lon);
            const int n = std::max(1, int(std::ceil(segLen / 5.0)));
            for (int s = 0; s <= n; ++s) {
                const double u = double(s) / double(n);
                evaluatePoint({p1.lat + u * (p2.lat - p1.lat), p1.lon + u * (p2.lon - p1.lon)}, w.highway, w.name);
            }
        }
    }

    if (cands.isEmpty()) {
        const double radii[] = {30.0, 40.0, 50.0};
        for (double r : radii) {
            for (int deg = 0; deg < 360; deg += 15) {
                const double b = deg * M_PI / 180.0;
                const double dLat = (r * std::cos(b)) / 111194.93;
                const double dLon = (r * std::sin(b)) / (111194.93 * std::cos(target.lat * M_PI / 180.0));
                evaluatePoint({target.lat + dLat, target.lon + dLon}, QStringLiteral("ground"), QString());
            }
        }
    }

    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
        if (std::fabs(a.score - b.score) > 0.001) return a.score < b.score;
        return a.dist < b.dist;
    });

    QList<Vantage> out;
    for (const Cand &c : cands) {
        bool tooClose = false;
        for (const Vantage &v : out) {
            if (PlateEvents::distanceM(c.pt.lat, c.pt.lon, v.pt.lat, v.pt.lon) < 10.0) {
                tooClose = true; break;
            }
        }
        if (tooClose) continue;
        QString spotDesc = c.name.isEmpty() ? (c.highway.isEmpty() ? QStringLiteral("way") : c.highway) : c.name;
        QString sideCap = c.side;
        if (!sideCap.isEmpty()) sideCap[0] = sideCap[0].toUpper();
        QString reason = QStringLiteral("%1 camera, outside field of view (%2 m on %3)")
            .arg(sideCap).arg(qRound(c.dist)).arg(spotDesc);
        out.append({c.pt, c.dist, c.bearingToCam, c.side, reason, c.score});
        if (out.size() >= 3) break;
    }
    return out;
}

QList<Exposure> checkExposures(const QList<LatLon> &route, const QList<Cam> &allCams)
{
    QList<Exposure> out;
    if (route.size() < 2) return out;
    QList<QPair<Cam, QList<Area>>> camAreas;
    for (const Cam &c : allCams) camAreas.append({c, areasFor(c)});

    struct SamplePoint { LatLon pt; double distAlong = 0; };
    QList<SamplePoint> samples;
    double totalD = 0;
    samples.append({route.first(), 0});
    for (int i = 0; i + 1 < route.size(); ++i) {
        const double len = PlateEvents::distanceM(route[i].lat, route[i].lon, route[i + 1].lat, route[i + 1].lon);
        const int n = std::max(1, int(std::ceil(len / 5.0)));
        for (int s = 1; s <= n; ++s) {
            const double u = double(s) / double(n);
            const LatLon p{route[i].lat + u * (route[i + 1].lat - route[i].lat),
                           route[i].lon + u * (route[i + 1].lon - route[i].lon)};
            totalD += len / n;
            samples.append({p, totalD});
        }
    }

    for (const auto &pair : camAreas) {
        const Cam &c = pair.first;
        const QList<Area> &areas = pair.second;
        bool inExposure = false;
        LatLon entryPt;
        double startD = 0;
        for (int i = 0; i < samples.size(); ++i) {
            bool in = false;
            for (const Area &a : areas) {
                if (pointInRing(a.ring, samples[i].pt.lat, samples[i].pt.lon)) {
                    in = true; break;
                }
            }
            if (in && !inExposure) {
                inExposure = true;
                entryPt = samples[i].pt;
                startD = samples[i].distAlong;
            } else if (!in && inExposure) {
                inExposure = false;
                const double expLen = samples[i].distAlong - startD;
                out.append({c.id, c.operatorName, c.model, entryPt, samples[i].pt, expLen});
            }
        }
        if (inExposure) {
            const double expLen = samples.last().distAlong - startD;
            out.append({c.id, c.operatorName, c.model, entryPt, samples.last().pt, expLen});
        }
    }
    return out;
}

QString toGpx(const QList<LatLon> &toVantage, const QList<LatLon> &away, const Vantage &vantage, const Cam &cam,
              const QList<Step> &approachSteps)
{
    QString xml;
    xml += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml += QStringLiteral("<gpx version=\"1.1\" creator=\"BeaconFix\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n");
    xml += QStringLiteral("  <wpt lat=\"%1\" lon=\"%2\">\n    <name>ALPR: %3</name>\n    <desc>%4 (%5)</desc>\n  </wpt>\n")
        .arg(cam.lat, 0, 'f', 6).arg(cam.lon, 0, 'f', 6)
        .arg(cam.id.toHtmlEscaped(), cam.model.toHtmlEscaped(), cam.operatorName.toHtmlEscaped());
    xml += QStringLiteral("  <wpt lat=\"%1\" lon=\"%2\">\n    <name>Vantage: %3</name>\n    <desc>%4 (look %5°)</desc>\n  </wpt>\n")
        .arg(vantage.pt.lat, 0, 'f', 6).arg(vantage.pt.lon, 0, 'f', 6)
        .arg(vantage.side.toHtmlEscaped(), vantage.reason.toHtmlEscaped())
        .arg(qRound(vantage.bearingToCamera));
    if (!approachSteps.isEmpty()) {
        xml += QStringLiteral("  <rte>\n    <name>Turn-by-turn to Vantage</name>\n");
        for (const Step &s : approachSteps) {
            xml += QStringLiteral("    <rtept lat=\"%1\" lon=\"%2\">\n"
                                  "      <name>%3</name>\n"
                                  "      <desc>%4 (%5 m)</desc>\n"
                                  "      <sym>navigation</sym>\n"
                                  "    </rtept>\n")
                .arg(s.start.lat, 0, 'f', 6).arg(s.start.lon, 0, 'f', 6)
                .arg(s.instruction.toHtmlEscaped(), s.streetName.toHtmlEscaped())
                .arg(qRound(s.distanceM));
        }
        xml += QStringLiteral("  </rte>\n");
    }
    if (!toVantage.isEmpty()) {
        xml += QStringLiteral("  <trk>\n    <name>Approach to Vantage</name>\n    <trkseg>\n");
        for (const LatLon &p : toVantage) {
            xml += QStringLiteral("      <trkpt lat=\"%1\" lon=\"%2\"/>\n").arg(p.lat, 0, 'f', 6).arg(p.lon, 0, 'f', 6);
        }
        xml += QStringLiteral("    </trkseg>\n  </trk>\n");
    }
    if (!away.isEmpty()) {
        xml += QStringLiteral("  <trk>\n    <name>Departure from Vantage</name>\n    <trkseg>\n");
        for (const LatLon &p : away) {
            xml += QStringLiteral("      <trkpt lat=\"%1\" lon=\"%2\"/>\n").arg(p.lat, 0, 'f', 6).arg(p.lon, 0, 'f', 6);
        }
        xml += QStringLiteral("    </trkseg>\n  </trk>\n");
    }
    xml += QStringLiteral("</gpx>\n");
    return xml;
}

QJsonObject inspectToJson(const InspectPlan &ip, const QString &limitsText, const QJsonArray &photos)
{
    QJsonObject camObj{{"id", ip.camera.id}, {"lat", ip.camera.lat}, {"lon", ip.camera.lon},
                       {"operator", ip.camera.operatorName}, {"model", ip.camera.model},
                       {"trust", std::round(ip.camera.trust * 1000) / 1000}};
    QJsonArray dirs;
    for (double d : ip.camera.dirs) dirs.append(d);
    camObj["directions"] = dirs;

    QJsonArray vArr;
    for (const Vantage &v : ip.vantages) {
        vArr.append(QJsonObject{{"lat", v.pt.lat}, {"lon", v.pt.lon},
                                {"distanceM", std::round(v.distanceM * 10) / 10},
                                {"bearingToCamera", std::round(v.bearingToCamera)},
                                {"side", v.side}, {"reason", v.reason}, {"score", std::round(v.score * 10) / 10}});
    }

    auto routeJson = [](const Route &r) {
        QJsonArray coords;
        for (const LatLon &x : r.points) coords.append(QJsonArray{std::round(x.lon * 1e6) / 1e6, std::round(x.lat * 1e6) / 1e6});
        QJsonArray stepsArr;
        for (const Step &s : r.steps) {
            stepsArr.append(QJsonObject{{"distanceM", std::round(s.distanceM)},
                                        {"durationS", std::round(s.durationS)},
                                        {"instruction", s.instruction},
                                        {"streetName", s.streetName},
                                        {"type", s.type},
                                        {"lat", s.start.lat},
                                        {"lon", s.start.lon}});
        }
        return QJsonObject{{"route", QJsonObject{{"type", "LineString"}, {"coordinates", coords}}},
                           {"distanceM", std::round(r.distanceM)}, {"durationS", std::round(r.durationS)},
                           {"steps", stepsArr},
                           {"ok", r.ok}, {"error", r.error}};
    };

    QJsonArray expArr;
    for (const Exposure &e : ip.exposures) {
        expArr.append(QJsonObject{{"cameraId", e.cameraId}, {"operator", e.operatorName}, {"model", e.model},
                                  {"entry", QJsonArray{e.entry.lat, e.entry.lon}},
                                  {"exit", QJsonArray{e.exit.lat, e.exit.lon}},
                                  {"meters", std::round(e.meters)}});
    }

    QJsonArray avoidArr;
    for (const Area &a : ip.avoidRegions) {
        avoidArr.append(QJsonObject{{"cameraId", a.cameraId}, {"disc", a.disc},
                                    {"coordinates", ringJson(a.ring)},
                                    {"areaM2", std::round(a.areaM2)}});
    }

    return QJsonObject{{"camera", camObj}, {"vantages", vArr},
                       {"legs", QJsonObject{{"toVantage", routeJson(ip.toVantage)}, {"away", routeJson(ip.away)}}},
                       {"exposures", expArr}, {"avoidRegions", avoidArr},
                       {"safe", ip.safe}, {"limits", limitsText},
                       {"provider", providerId(ip.provider)}, {"providerName", providerName(ip.provider)},
                       {"photos", photos}, {"note", ip.note}};
}

} // namespace AvoidRoute
