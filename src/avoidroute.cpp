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

QUrl requestUrl(Provider p, const QString &key)
{
    if (p == Provider::Ors) return QUrl(QStringLiteral("https://api.openrouteservice.org/v2/directions/driving-car/geojson"));
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

QByteArray requestBody(Provider p, LatLon a, LatLon b, const QList<Area> &areas)
{
    const QJsonArray pts{QJsonArray{a.lon, a.lat}, QJsonArray{b.lon, b.lat}};
    QJsonObject body;
    if (p == Provider::Ors) {
        body = QJsonObject{{"coordinates", pts}, {"instructions", false}, {"units", "m"}};
        if (!areas.isEmpty()) {
            QJsonArray polys;
            for (const Area &x : areas) polys.append(QJsonArray{ringJson(x.ring)});
            body["options"] = QJsonObject{{"avoid_polygons", QJsonObject{{"type", "MultiPolygon"}, {"coordinates", polys}}}};
        }
    } else {
        body = QJsonObject{{"points", pts}, {"profile", "car"}, {"points_encoded", false}, {"instructions", false}, {"calc_points", true}, {"ch.disable", true}};
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
            const QJsonObject s = ft.value(QLatin1String("properties")).toObject().value(QLatin1String("summary")).toObject();
            r.distanceM = s.value(QLatin1String("distance")).toDouble();
            r.durationS = s.value(QLatin1String("duration")).toDouble();
        }
    } else {
        msg = o.value(QLatin1String("message")).toString();
        const QJsonArray paths = o.value(QLatin1String("paths")).toArray();
        if (http == 200 && !paths.isEmpty()) {
            const QJsonObject pa = paths.first().toObject();
            coords(pa.value(QLatin1String("points")).toObject().value(QLatin1String("coordinates")).toArray());
            r.distanceM = pa.value(QLatin1String("distance")).toDouble();
            r.durationS = pa.value(QLatin1String("time")).toDouble() / 1000.0;
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
    QSet<QString> avoided;
    for (const Area &a : plan.areas) avoided.insert(a.cameraId);
    const QString attribution = p == Provider::Ors ? QStringLiteral("© openrouteservice.org by HeiGIT · map data © OpenStreetMap contributors (ODbL)")
                                                   : QStringLiteral("Powered by GraphHopper · map data © OpenStreetMap contributors (ODbL)");
    return QJsonObject{{"provider", providerId(p)}, {"providerName", providerName(p)}, {"route", QJsonObject{{"type", "LineString"}, {"coordinates", coords}}},
                       {"distanceM", std::round(r.distanceM)}, {"durationS", std::round(r.durationS)},
                       {"avoided", QJsonObject{{"areas", int(plan.areas.size())}, {"cameras", int(avoided.size())}, {"corridorCameras", int(plan.corridor.size())},
                                               {"capped", plan.capped}, {"corridorM", std::round(plan.corridorM)}}},
                       {"passes", ps}, {"passesInCone", inCone}, {"attribution", attribution}};
}

} // namespace AvoidRoute
