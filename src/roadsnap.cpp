// SPDX-License-Identifier: Apache-2.0
#include "roadsnap.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

namespace RoadSnap {

static constexpr double kEarthR = 6371000.0;
static constexpr double kD2R = M_PI / 180.0;
static constexpr double kInf = std::numeric_limits<double>::infinity();

namespace {
struct Xy { double x = 0, y = 0; };
struct Proj {
    double lat0, lon0, kx;
    Proj(double lat, double lon) : lat0(lat), lon0(lon), kx(std::cos(lat * kD2R) * kEarthR * kD2R) {}
    Xy to(double lat, double lon) const { return {(lon - lon0) * kx, (lat - lat0) * kEarthR * kD2R}; }
    Pt back(const Xy &p) const { return {lat0 + p.y / (kEarthR * kD2R), lon0 + p.x / kx}; }
};
double compass(double dx, double dy) { return std::fmod(std::atan2(dx, dy) / kD2R + 360.0, 360.0); }
double axial(double a, double b) { const double d = PlateEvents::angleDiff(a, b); return d > 90.0 ? 180.0 - d : d; }   // 0…90: direction-free
}

// ── Overpass ──────────────────────────────────────────────────────────────────
static const char *kMotor = "motorway|trunk|primary|secondary|tertiary|unclassified|residential|living_street|service|road";

bool isMotorRoad(const QString &highway)
{
    static const QRegularExpression re(QStringLiteral("^(%1)(_link)?$").arg(QLatin1String(kMotor)));
    return re.match(highway).hasMatch();
}

QString overpassQuery(double lat, double lon, double radiusM)
{
    return QStringLiteral("[out:json][timeout:25];way(around:%1,%2,%3)[\"highway\"~\"^(%4)(_link)?$\"][\"area\"!=\"yes\"];out body geom;")
        .arg(qRound(radiusM)).arg(lat, 0, 'f', 7).arg(lon, 0, 'f', 7).arg(QLatin1String(kMotor));
}

static bool yes(const QString &v) { return !v.isEmpty() && v != QLatin1String("no") && v != QLatin1String("0") && v != QLatin1String("false"); }

QList<Way> parseOverpass(const QByteArray &json, QString *error)
{
    QList<Way> out;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (!doc.isObject() || !doc.object().value(QLatin1String("elements")).isArray()) {
        if (error) *error = pe.error != QJsonParseError::NoError ? pe.errorString() : QStringLiteral("no elements in the Overpass answer");
        return out;
    }
    for (const QJsonValue &v : doc.object().value(QLatin1String("elements")).toArray()) {
        const QJsonObject e = v.toObject();
        if (e.value(QLatin1String("type")).toString() != QLatin1String("way")) continue;
        const QJsonObject t = e.value(QLatin1String("tags")).toObject();
        Way w;
        w.id = qint64(e.value(QLatin1String("id")).toDouble());
        w.highway = t.value(QLatin1String("highway")).toString();
        if (w.id <= 0 || !isMotorRoad(w.highway) || t.value(QLatin1String("area")).toString() == QLatin1String("yes")) continue;
        w.name = t.value(QLatin1String("name")).toString();
        w.ref = t.value(QLatin1String("ref")).toString();
        const QString br = t.value(QLatin1String("bridge")).toString(), tu = t.value(QLatin1String("tunnel")).toString();
        w.bridge = yes(br);
        w.tunnel = yes(tu) && tu != QLatin1String("building_passage");
        bool ok = false;
        const int layer = t.value(QLatin1String("layer")).toString().trimmed().toInt(&ok);
        w.layer = ok ? layer : (w.bridge ? 1 : w.tunnel ? -1 : 0);
        const QString ow = t.value(QLatin1String("oneway")).toString();
        if (ow == QLatin1String("yes") || ow == QLatin1String("true") || ow == QLatin1String("1")) w.oneway = 1;
        else if (ow == QLatin1String("-1") || ow == QLatin1String("reverse")) w.oneway = -1;
        else if (ow.isEmpty() && (w.highway == QLatin1String("motorway") || w.highway == QLatin1String("motorway_link")
                                  || t.value(QLatin1String("junction")).toString() == QLatin1String("roundabout")))
            w.oneway = 1;                                                        // implied by the road type
        for (const QJsonValue &n : e.value(QLatin1String("nodes")).toArray()) w.nodes << qint64(n.toDouble());
        for (const QJsonValue &g : e.value(QLatin1String("geometry")).toArray()) {
            const QJsonObject p = g.toObject();
            if (!p.contains(QLatin1String("lat"))) continue;                      // a node outside the bbox can be null
            w.pts.append({p.value(QLatin1String("lat")).toDouble(), p.value(QLatin1String("lon")).toDouble()});
        }
        if (w.pts.size() < 2) continue;
        out << w;
    }
    return out;
}

QJsonObject toJson(const Way &w)
{
    QJsonArray nodes, geom;
    for (qint64 n : w.nodes) nodes.append(double(n));
    for (const Pt &p : w.pts) geom.append(QJsonArray{std::round(p.lat * 1e7) / 1e7, std::round(p.lon * 1e7) / 1e7});
    return QJsonObject{{"id", double(w.id)}, {"highway", w.highway}, {"name", w.name}, {"ref", w.ref}, {"layer", w.layer}, {"bridge", w.bridge},
                       {"tunnel", w.tunnel}, {"oneway", w.oneway}, {"nodes", nodes}, {"geom", geom}};
}

Way fromJson(const QJsonObject &o)
{
    Way w;
    w.id = qint64(o.value(QLatin1String("id")).toDouble());
    w.highway = o.value(QLatin1String("highway")).toString();
    w.name = o.value(QLatin1String("name")).toString();
    w.ref = o.value(QLatin1String("ref")).toString();
    w.layer = o.value(QLatin1String("layer")).toInt();
    w.bridge = o.value(QLatin1String("bridge")).toBool();
    w.tunnel = o.value(QLatin1String("tunnel")).toBool();
    w.oneway = o.value(QLatin1String("oneway")).toInt();
    for (const QJsonValue &n : o.value(QLatin1String("nodes")).toArray()) w.nodes << qint64(n.toDouble());
    for (const QJsonValue &g : o.value(QLatin1String("geom")).toArray()) {
        const QJsonArray a = g.toArray();
        if (a.size() >= 2) w.pts.append({a[0].toDouble(), a[1].toDouble()});
    }
    return w;
}

// ── geometry ──────────────────────────────────────────────────────────────────
OnWay project(const Way &w, double lat, double lon)
{
    OnWay best;
    if (w.pts.size() < 2) return best;
    const Proj pj(lat, lon);
    for (int k = 0; k + 1 < int(w.pts.size()); ++k) {
        const Xy a = pj.to(w.pts[k].lat, w.pts[k].lon), b = pj.to(w.pts[k + 1].lat, w.pts[k + 1].lon);
        const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
        if (l2 <= 0) continue;
        const double u = std::clamp(-(a.x * dx + a.y * dy) / l2, 0.0, 1.0);
        const Xy c{a.x + u * dx, a.y + u * dy};
        const double d = std::hypot(c.x, c.y);
        if (d < best.distM) { best.distM = d; best.seg = k; best.bearingDeg = compass(dx, dy); best.at = pj.back(c); }
    }
    return best;
}

QJsonObject Watched::toJson() const
{
    return QJsonObject{{"wayId", double(wayId)}, {"distanceM", std::isnan(distM) ? QJsonValue() : QJsonValue(std::round(distM * 10) / 10)},
                       {"bearingDeg", std::isnan(bearingDeg) ? QJsonValue() : QJsonValue(std::round(bearingDeg * 10) / 10)},
                       {"layer", layer}, {"oneway", oneway}, {"basis", basis}};
}

Watched watchedWay(const QList<Way> &ways, double camLat, double camLon, const QList<double> &dirs, const QJsonObject &camTags)
{
    Watched out;
    bool explicitLayer = false;
    const int camLayer = camTags.value(QLatin1String("layer")).toString().trimmed().toInt(&explicitLayer);
    struct Cand { int i; OnWay on; bool dirOk; };
    QList<Cand> cands;
    for (int i = 0; i < int(ways.size()); ++i) {
        const OnWay on = project(ways[i], camLat, camLon);
        if (on.distM > kWatchRadiusM) continue;
        bool dirOk = false;
        for (double d : dirs) if (axial(on.bearingDeg, d) <= kWatchDirTolDeg) dirOk = true;
        cands.append({i, on, dirOk});
    }
    if (cands.isEmpty()) return out;
    // the camera's layer first (a camera without a layer tag is on the ground: layer 0, but any layer when nothing there qualifies)
    auto pick = [&](bool needDir, bool sameLayer) -> const Cand * {
        const Cand *b = nullptr;
        for (const Cand &c : std::as_const(cands)) {
            if (needDir && !c.dirOk) continue;
            if (sameLayer && ways[c.i].layer != (explicitLayer ? camLayer : 0)) continue;
            // ALPRs mostly watch public roads: a service way (driveway, parking aisle) wins only when 10 m nearer
            auto eff = [&](const Cand &x) { return x.on.distM + (ways[x.i].highway == QLatin1String("service") ? 10.0 : 0.0); };
            if (!b || eff(c) < eff(*b)) b = &c;
        }
        return b;
    };
    const Cand *c = nullptr;
    if (!dirs.isEmpty()) {
        c = pick(true, true);
        if (!c && !explicitLayer) c = pick(true, false);
        out.basis = QStringLiteral("direction");
        if (!c) { c = pick(false, true); if (!c && !explicitLayer) c = pick(false, false); out.basis = QStringLiteral("nearest_no_direction_match"); }
    } else {
        c = pick(false, true);
        if (!c && !explicitLayer) c = pick(false, false);
        out.basis = QStringLiteral("nearest");
    }
    if (!c) { out.basis = QStringLiteral("none"); return out; }
    const Way &w = ways[c->i];
    out.wayId = w.id; out.index = c->i; out.distM = c->on.distM; out.bearingDeg = c->on.bearingDeg; out.layer = w.layer; out.oneway = w.oneway;
    return out;
}

// ── the track ─────────────────────────────────────────────────────────────────
QList<Sample> samplesFor(const QList<PlateEvents::TrackFix> &f, const PlateEvents::Pass &p, double camLat, double camLon, int *closest)
{
    QList<Sample> out;
    auto linked = [&](int a, int b) {
        if (a < 0 || b >= int(f.size())) return false;
        const qint64 dt = f[b].ms - f[a].ms;
        return dt > 0 && dt <= PlateEvents::kRunGapMs && PlateEvents::distanceM(f[a].lat, f[a].lon, f[b].lat, f[b].lon) >= 3.0;
    };
    for (int i = 0; i < int(f.size()); ++i) {
        if (std::llabs(f[i].ms - p.ms) > qint64(kSampleWindowS * 1000) || std::llabs(f[i].ms - p.ms) < 500) continue;
        if (PlateEvents::distanceM(f[i].lat, f[i].lon, camLat, camLon) > kSampleRadiusM) continue;
        Sample s; s.ms = f[i].ms; s.lat = f[i].lat; s.lon = f[i].lon; s.acc = f[i].acc;
        int a = i - 1, b = i + 1;
        if (!linked(a, b)) { if (linked(i, b)) a = i; else if (linked(a, i)) b = i; else a = b = -1; }
        if (a >= 0 && b >= 0 && a != b) s.headingDeg = PlateEvents::bearingDeg(f[a].lat, f[a].lon, f[b].lat, f[b].lon);
        out << s;
    }
    Sample c; c.ms = p.ms; c.lat = p.lat; c.lon = p.lon; c.acc = p.acc; c.headingDeg = p.headingDeg;
    out << c;
    std::sort(out.begin(), out.end(), [](const Sample &x, const Sample &y) { return x.ms < y.ms; });
    if (closest) { *closest = 0; for (int i = 0; i < int(out.size()); ++i) if (out[i].ms == p.ms && out[i].lat == p.lat && out[i].lon == p.lon) *closest = i; }
    return out;
}

// ── matching ──────────────────────────────────────────────────────────────────
QJsonObject Result::toJson() const
{
    auto num = [](double v, int dec) { return std::isnan(v) ? QJsonValue() : QJsonValue(std::round(v * std::pow(10.0, dec)) / std::pow(10.0, dec)); };
    QJsonObject o{{"status", status}, {"factor", std::round(factor * 1000) / 1000}, {"samples", samples}};
    if (status != QLatin1String("ok")) return o;
    o["verdict"] = verdict;
    o["pWatched"] = std::round(pWatched * 1000) / 1000;
    o["watchedWay"] = double(watchedWay);
    o["matchedWay"] = double(matchedWay);
    o["offsetM"] = num(offsetM, 1);
    o["trackOffsetM"] = num(trackOffsetM, 1);
    o["watchedLayer"] = watchedLayer;
    o["matchedLayer"] = matchedLayer;
    o["layer"] = layerVerdict;
    o["oppositeOneway"] = oppositeOneway;
    o["candidates"] = candidates;
    return o;
}

static bool connected(const Way &a, const Way &b)
{
    if (a.nodes.isEmpty() || b.nodes.isEmpty()) return false;
    const QSet<qint64> na(a.nodes.cbegin(), a.nodes.cend());
    for (qint64 n : b.nodes) if (na.contains(n)) return true;
    return false;
}

Result snap(const QList<Sample> &samples, int closest, const QList<Way> &ways, const Watched &watched)
{
    Result r;
    r.samples = int(samples.size());
    if (ways.isEmpty()) { r.status = QStringLiteral("no_roads"); return r; }
    if (watched.index < 0 || watched.index >= int(ways.size())) { r.status = QStringLiteral("no_watched_way"); return r; }
    if (samples.isEmpty() || closest < 0 || closest >= int(samples.size())) { r.status = QStringLiteral("no_samples"); return r; }
    r.status = QStringLiteral("ok");
    const Way &W = ways[watched.index];
    r.watchedWay = W.id; r.watchedLayer = W.layer;
    const int n = int(samples.size());
    // GPS errors are correlated over a pass: the samples together count as at most ~3 independent looks
    const double wt = std::min(1.0, 3.0 / double(n));

    // candidates per sample: the ways near it, and always the watched way
    struct St { int way; OnWay on; double e; };
    QList<QList<St>> cand(n);
    for (int i = 0; i < n; ++i) {
        const Sample &s = samples[i];
        const double sigma = std::max(4.0, (s.acc > 0 ? s.acc : 8.0) / 1.515);
        const double reach = std::max(60.0, 3.0 * sigma + 10.0);
        for (int k = 0; k < int(ways.size()); ++k) {
            const OnWay on = project(ways[k], s.lat, s.lon);
            if (on.distM > reach && k != watched.index) continue;
            const double d = std::max(0.0, on.distM - 2.5);            // a lane's half-width
            double e = d * d / (2.0 * sigma * sigma);
            if (!std::isnan(s.headingDeg) && !std::isnan(on.bearingDeg)) {
                if (axial(s.headingDeg, on.bearingDeg) > kHeadingTolDeg) e += 4.0;
                if (ways[k].oneway != 0) {
                    const double fwd = std::fmod(on.bearingDeg + (ways[k].oneway < 0 ? 180.0 : 0.0), 360.0);
                    if (PlateEvents::angleDiff(s.headingDeg, fwd) > 90.0) e += 6.0;   // against a oneway
                }
            }
            cand[i].append({k, on, wt * e});
        }
    }
    auto trans = [&](int a, int b) {
        if (a == b) return 0.0;
        if (connected(ways[a], ways[b])) return 0.3;
        if (ways[a].layer != ways[b].layer) return 5.0;              // no jumping between layers
        return 2.0;
    };
    // forward / backward minimum costs (Viterbi both ways): through (closest, s) = fwd + bwd − e
    QList<QList<double>> fw(n), bw(n);
    for (int i = 0; i < n; ++i) { fw[i] = QList<double>(cand[i].size(), kInf); bw[i] = QList<double>(cand[i].size(), kInf); }
    for (int s = 0; s < int(cand[0].size()); ++s) fw[0][s] = cand[0][s].e;
    for (int i = 1; i < n; ++i)
        for (int s = 0; s < int(cand[i].size()); ++s) {
            double m = kInf;
            for (int q = 0; q < int(cand[i - 1].size()); ++q) m = std::min(m, fw[i - 1][q] + trans(cand[i - 1][q].way, cand[i][s].way));
            fw[i][s] = m + cand[i][s].e;
        }
    for (int s = 0; s < int(cand[n - 1].size()); ++s) bw[n - 1][s] = cand[n - 1][s].e;
    for (int i = n - 2; i >= 0; --i)
        for (int s = 0; s < int(cand[i].size()); ++s) {
            double m = kInf;
            for (int q = 0; q < int(cand[i + 1].size()); ++q) m = std::min(m, trans(cand[i][s].way, cand[i + 1][q].way) + bw[i + 1][q]);
            bw[i][s] = m + cand[i][s].e;
        }
    const Sample &cs = samples[closest];
    const OnWay onW = project(W, cs.lat, cs.lon);
    r.trackOffsetM = onW.distM;
    // the trip against the watched oneway: the other carriageway (or wrong-way driving): not what the camera reads
    if (W.oneway != 0 && !std::isnan(cs.headingDeg) && !std::isnan(onW.bearingDeg)) {
        const double fwd = std::fmod(onW.bearingDeg + (W.oneway < 0 ? 180.0 : 0.0), 360.0);
        r.oppositeOneway = PlateEvents::angleDiff(cs.headingDeg, fwd) > 120.0;
    }
    double costComp = kInf, costInc = kInf, best = kInf;
    int bestS = -1, bestInc = -1;
    r.candidates = int(cand[closest].size());
    QList<double> axisOff(cand[closest].size(), 0.0);
    for (int s = 0; s < int(cand[closest].size()); ++s) {
        const St &st = cand[closest][s];
        const double through = fw[closest][s] + bw[closest][s] - st.e;
        const Way &w = ways[st.way];
        double off = 0.0;
        if (st.way != watched.index) off = project(W, st.on.at.lat, st.on.at.lon).distM;
        axisOff[s] = off;
        const bool compatible = !r.oppositeOneway && (st.way == watched.index || (w.layer == W.layer && off <= kAxisOffsetM));
        if (compatible) costComp = std::min(costComp, through);
        else if (through < costInc) { costInc = through; bestInc = s; }
        if (through < best) { best = through; bestS = s; }
    }
    if (bestS >= 0) {
        const St &m = cand[closest][bestS];
        r.matchedWay = ways[m.way].id; r.matchedLayer = ways[m.way].layer;
        r.offsetM = axisOff[bestS];
        r.layerVerdict = ways[m.way].layer == W.layer ? QStringLiteral("same") : QStringLiteral("different");
    } else {
        r.layerVerdict = QStringLiteral("unknown");
    }
    // H1, "the trip was on the watched road while passing": a path on compatible ways at every sample of the stretch
    // around the closest approach (≤ 30 m from it, ±10 s) — not just at one instant, which would price the alternative
    // by its transitions alone; not the whole window either, which would forbid turning onto the road at the camera
    {
        auto compatibleAt = [&](int i, int s) {
            if (r.oppositeOneway) return false;
            const St &st = cand[i][s];
            if (st.way == watched.index) return true;
            if (ways[st.way].layer != W.layer) return false;
            return project(W, st.on.at.lat, st.on.at.lon).distM <= kAxisOffsetM;
        };
        QList<double> prev, cur;
        for (int i = 0; i < n; ++i) {
            const bool win = i == closest || (std::llabs(samples[i].ms - cs.ms) <= 10000
                                              && PlateEvents::distanceM(samples[i].lat, samples[i].lon, cs.lat, cs.lon) <= kStretchM);
            cur = QList<double>(cand[i].size(), kInf);
            for (int s2 = 0; s2 < int(cand[i].size()); ++s2) {
                if (win && !compatibleAt(i, s2)) continue;
                double m = i == 0 ? 0.0 : kInf;
                for (int q = 0; q < int(prev.size()); ++q) if (!std::isinf(prev[q])) m = std::min(m, prev[q] + trans(cand[i - 1][q].way, cand[i][s2].way));
                if (!std::isinf(m)) cur[s2] = m + cand[i][s2].e;
            }
            prev = cur;
        }
        costComp = kInf;
        for (double c : std::as_const(prev)) costComp = std::min(costComp, c);
    }
    if (std::isinf(costInc)) r.pWatched = 1.0;
    else if (std::isinf(costComp)) r.pWatched = 0.0;
    else r.pWatched = 1.0 / (1.0 + std::exp(std::clamp(costComp - costInc, -50.0, 50.0)));
    r.factor = r.pWatched + (1.0 - r.pWatched) * kOffFactor;
    if (r.pWatched >= 0.8) r.verdict = r.matchedWay == r.watchedWay || r.matchedWay == 0 ? QStringLiteral("on_watched_way") : QStringLiteral("near_axis");
    else if (r.pWatched <= 0.2) {
        const int s = bestInc >= 0 ? bestInc : bestS;
        if (r.oppositeOneway) r.verdict = QStringLiteral("opposite_direction");
        else if (s >= 0 && ways[cand[closest][s].way].layer != W.layer) r.verdict = QStringLiteral("different_layer");
        else r.verdict = QStringLiteral("parallel_road");
    } else r.verdict = QStringLiteral("ambiguous");
    return r;
}

} // namespace RoadSnap
