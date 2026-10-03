// SPDX-License-Identifier: Apache-2.0
#include "plateevents.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>

namespace PlateEvents {

static constexpr double kEarthR = 6371000.0;
static constexpr double kD2R = M_PI / 180.0;

double distanceM(double lat1, double lon1, double lat2, double lon2)
{
    const double dLat = (lat2 - lat1) * kD2R, dLon = (lon2 - lon1) * kD2R;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) + std::cos(lat1 * kD2R) * std::cos(lat2 * kD2R) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * kEarthR * std::asin(std::sqrt(std::min(1.0, a)));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2)
{
    const double y = std::sin((lon2 - lon1) * kD2R) * std::cos(lat2 * kD2R);
    const double x = std::cos(lat1 * kD2R) * std::sin(lat2 * kD2R) - std::sin(lat1 * kD2R) * std::cos(lat2 * kD2R) * std::cos((lon2 - lon1) * kD2R);
    return std::fmod(std::atan2(y, x) / kD2R + 360.0, 360.0);
}

double angleDiff(double a, double b)
{
    const double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

// ── §2.0 ──────────────────────────────────────────────────────────────────────
bool isPlateReaderModel(const QString &model)
{
    static const QRegularExpression re(QStringLiteral("\\b(alpr|lpr|anpr)\\b|plate|falcon|sparrow|autovu|sharpv|vigilant|elsag|platesmart|neology|rekor|l5f|l6q"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re.match(model).hasMatch();
}

bool isOsmId(const QString &id) { return id.startsWith(QLatin1String("osm:")); }

static QStringList typeTokens(const QString &v)
{
    QStringList out;
    for (const QString &t : v.split(QRegularExpression(QStringLiteral("[;,]")), Qt::SkipEmptyParts)) out << t.trimmed().toUpper();
    return out;
}

static bool hasToken(const QStringList &toks, std::initializer_list<const char *> want)
{
    for (const char *w : want) if (toks.contains(QLatin1String(w))) return true;
    return false;
}

QString classifyNote(const QJsonObject &tags)
{
    const QStringList st = typeTokens(tags.value(QLatin1String("surveillance:type")).toString());
    QStringList notes;
    if (hasToken(st, {"ALRP", "APLR", "AMPR"}) && !hasToken(st, {"ALPR", "ANPR"}))
        notes << QStringLiteral("surveillance:type is a misspelt ALPR (%1): reduced confidence").arg(tags.value(QLatin1String("surveillance:type")).toString());
    const bool alpr = hasToken(st, {"ALPR", "ANPR", "ALRP", "APLR", "AMPR"}) || tags.value(QLatin1String("camera:type")).toString().compare(QLatin1String("ALPR"), Qt::CaseInsensitive) == 0
                   || tags.value(QLatin1String("surveillance")).toString().compare(QLatin1String("ANPR"), Qt::CaseInsensitive) == 0;
    if (alpr && !webcamUrl(tags).isEmpty()) notes << QStringLiteral("contact:webcam on an ALPR: a tagging conflict");
    return notes.join(QStringLiteral("; "));
}

QString classifyCamera(const QString &model, const QString &source, const QString &id, const QString &detectionMethod, const QJsonObject &tags)
{
    Q_UNUSED(detectionMethod);
    const bool osm = isOsmId(id) || source.compare(QLatin1String("osm"), Qt::CaseInsensitive) == 0;
    // Non-OSM rows: the community / suspected lists, RF detections of Flock hardware, imports — plate-reader lists
    if (!osm) {
        // Flock's Raven is a gunshot detector (an RF detection of it, or a list's label): not a camera at all
        static const QRegularExpression gunshot(QStringLiteral("\\braven\\b|gunshot"), QRegularExpression::CaseInsensitiveOption);
        if (gunshot.match(model).hasMatch() && !isPlateReaderModel(model)) return QStringLiteral("not_camera");
        static const QRegularExpression notPlate(QStringLiteral("^\\s*camera\\s*$|other surveillance|traffic cam|cctv|webcam"), QRegularExpression::CaseInsensitiveOption);
        return notPlate.match(model).hasMatch() && !isPlateReaderModel(model) ? QStringLiteral("cctv") : QStringLiteral("alpr");
    }
    if (!tags.isEmpty()) {
        const QStringList st = typeTokens(tags.value(QLatin1String("surveillance:type")).toString());
        // 1. enforcement
        if (tags.value(QLatin1String("highway")).toString() == QLatin1String("speed_camera") || tags.value(QLatin1String("_enforcement")).toBool()
            || tags.value(QLatin1String("enforcement")).toString().size() > 0)
            return QStringLiteral("enforcement");
        // 2. not a camera
        if (hasToken(st, {"GUNSHOT_DETECTOR", "GUARD"})
            || (tags.value(QLatin1String("man_made")).toString() == QLatin1String("monitoring_station") && !tags.value(QLatin1String("monitoring:traffic")).toString().isEmpty()))
            return QStringLiteral("not_camera");
        // 3. ALPR
        if (hasToken(st, {"ALPR", "ANPR", "ALRP", "APLR", "AMPR"}) || tags.value(QLatin1String("camera:type")).toString().compare(QLatin1String("ALPR"), Qt::CaseInsensitive) == 0
            || tags.value(QLatin1String("surveillance")).toString().compare(QLatin1String("ANPR"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("alpr");
        const bool camera = hasToken(st, {"CAMERA"});
        // 4. a public webcam
        if (camera && !webcamUrl(tags).isEmpty()) return QStringLiteral("webcam");
        // 5. Flock's PTZ (Condor): a video camera, not a plate reader
        if (camera && (tags.value(QLatin1String("manufacturer")).toString().contains(QLatin1String("Flock"), Qt::CaseInsensitive)
                       || tags.value(QLatin1String("brand")).toString().contains(QLatin1String("Flock"), Qt::CaseInsensitive)))
            return QStringLiteral("ptz");
        // 6. anything else
        return QStringLiteral("cctv");
    }
    // OSM row whose tags are not stored: DeFlock lists only man_made=surveillance + surveillance:type=ALPR; an older
    // import is what it made of them, until the camera-photo lookup fetches the node (src/platewatch.cpp)
    if (source == QLatin1String("deflock")) return QStringLiteral("alpr");
    if (isPlateReaderModel(model)) return QStringLiteral("alpr");
    if (!webcamUrl(tags).isEmpty()) return QStringLiteral("webcam");
    return QStringLiteral("cctv");
}

bool suspectedOnly(const QString &source, const QString &model)
{
    return source.contains(QLatin1String("suspected"), Qt::CaseInsensitive) || model.contains(QLatin1String("(suspected)"), Qt::CaseInsensitive);
}

QString webcamUrl(const QJsonObject &tags)
{
    for (const char *k : {"contact:webcam", "webcam", "surveillance:webcam"}) {
        const QString v = tags.value(QLatin1String(k)).toString().trimmed();
        if (v.startsWith(QLatin1String("http://")) || v.startsWith(QLatin1String("https://"))) return v.section(QLatin1Char(';'), 0, 0).trimmed();
    }
    return {};
}

// ── §2.3 ──────────────────────────────────────────────────────────────────────
static double cardinal(const QString &s)
{
    static const QStringList names{QStringLiteral("N"), QStringLiteral("NNE"), QStringLiteral("NE"), QStringLiteral("ENE"), QStringLiteral("E"), QStringLiteral("ESE"),
                                   QStringLiteral("SE"), QStringLiteral("SSE"), QStringLiteral("S"), QStringLiteral("SSW"), QStringLiteral("SW"), QStringLiteral("WSW"),
                                   QStringLiteral("W"), QStringLiteral("WNW"), QStringLiteral("NW"), QStringLiteral("NNW")};
    QString u = s.trimmed().toUpper();
    static const QHash<QString, QString> words{{QStringLiteral("NORTH"), QStringLiteral("N")}, {QStringLiteral("SOUTH"), QStringLiteral("S")},
                                               {QStringLiteral("EAST"), QStringLiteral("E")}, {QStringLiteral("WEST"), QStringLiteral("W")},
                                               {QStringLiteral("NORTHEAST"), QStringLiteral("NE")}, {QStringLiteral("SOUTHEAST"), QStringLiteral("SE")},
                                               {QStringLiteral("SOUTHWEST"), QStringLiteral("SW")}, {QStringLiteral("NORTHWEST"), QStringLiteral("NW")},
                                               {QStringLiteral("NB"), QStringLiteral("N")}, {QStringLiteral("SB"), QStringLiteral("S")},
                                               {QStringLiteral("EB"), QStringLiteral("E")}, {QStringLiteral("WB"), QStringLiteral("W")}};
    u = words.value(u, u);
    const qsizetype i = names.indexOf(u);
    return i < 0 ? nan() : double(i) * 22.5;
}

static double oneDirection(const QString &tok, bool zeroUnknown = true)
{
    const QString t = tok.trimmed();
    if (t.isEmpty()) return nan();
    bool ok = false;
    const double d = t.toDouble(&ok);
    if (ok) {
        if (zeroUnknown && d == 0.0) return nan();               // direction=0: unknown (the DeFlock app writes 0 when the mapper gave none)
        return std::fmod(std::fmod(d, 360.0) + 360.0, 360.0);
    }
    return cardinal(t);
}

QString directionText(const Camera &cam)
{
    const QString d = cam.tags.value(QLatin1String("direction")).toString().trimmed();
    if (!d.isEmpty()) return d;
    const QString cd = cam.tags.value(QLatin1String("camera:direction")).toString().trimmed();
    if (!cd.isEmpty()) return cd;
    return cam.direction;
}

// The tag grammar follows DeFlock's parser (github.com/FoggedLens/deflock-data data/cameras/lib.mjs, MIT): numbers,
// 16-point cardinals, spelled-out and "bound" (NB, EB …) directions, clockwise ranges "a-b" (their centre), and lists
// separated by ';' or ','. Unlike DeFlock, a bare 0 is unknown (§2.3) and a full circle is no direction.
QList<double> parseDirections(const QString &s)
{
    QList<double> out;
    static const QRegularExpression sep(QStringLiteral("[;,]"));
    for (const QString &part : s.split(sep, Qt::SkipEmptyParts)) {
        const QString p = part.trimmed();
        // a range "a-b" (degrees or cardinals): its centre, going clockwise from a to b ("0-360" = no direction)
        const qsizetype dash = p.indexOf(QLatin1Char('-'), 1);
        if (dash > 0) {
            const double a = oneDirection(p.left(dash), false), b = oneDirection(p.mid(dash + 1), false);
            if (std::isnan(a) || std::isnan(b)) continue;
            double span = std::fmod(b - a + 360.0, 360.0);
            const double rawA = p.left(dash).toDouble(), rawB = p.mid(dash + 1).toDouble();
            if (span == 0.0 && rawB - rawA >= 359.0) continue;            // all round: not a direction
            out << std::fmod(a + span / 2.0, 360.0);
            continue;
        }
        const double d = oneDirection(p);
        if (!std::isnan(d)) out << d;
    }
    return out;
}

// ── §2.1 / §2.2 ───────────────────────────────────────────────────────────────
namespace {
struct Xy { double x = 0, y = 0; };
struct Proj {
    double lat0, lon0, kx;
    Proj(double lat, double lon) : lat0(lat), lon0(lon), kx(std::cos(lat * kD2R) * kEarthR * kD2R) {}
    Xy to(double lat, double lon) const { return {(lon - lon0) * kx, (lat - lat0) * kEarthR * kD2R}; }
};
// closest point of segment a→b to the origin: parameter t ∈ [0,1] and the distance
double segClosest(const Xy &a, const Xy &b, double *t)
{
    const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    double u = l2 > 0 ? -(a.x * dx + a.y * dy) / l2 : 0.0;
    u = std::clamp(u, 0.0, 1.0);
    if (t) *t = u;
    return std::hypot(a.x + u * dx, a.y + u * dy);
}
// the part [t0, t1] of segment a→b inside the circle of radius r round the origin (false: none)
bool segInside(const Xy &a, const Xy &b, double r, double *t0, double *t1)
{
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double A = dx * dx + dy * dy, B = 2 * (a.x * dx + a.y * dy), C = a.x * a.x + a.y * a.y - r * r;
    if (A <= 0) { if (C <= 0) { *t0 = 0; *t1 = 1; return true; } return false; }
    const double disc = B * B - 4 * A * C;
    if (disc < 0) return false;
    const double s = std::sqrt(disc);
    const double u0 = std::max(0.0, (-B - s) / (2 * A)), u1 = std::min(1.0, (-B + s) / (2 * A));
    if (u1 < u0) return false;
    *t0 = u0; *t1 = u1;
    return true;
}
bool segValid(const TrackFix &a, const TrackFix &b)
{
    const qint64 dt = b.ms - a.ms;
    return dt >= 0 && dt <= kRunGapMs && distanceM(a.lat, a.lon, b.lat, b.lon) <= kMaxSegmentM;
}
// Where the device was at time ms: interpolated between the fixes around it, else extrapolated along the
// closest-approach segment (velocity v, m/s in the projection)
Xy positionAt(const QList<TrackFix> &f, const QList<Xy> &xy, qint64 ms, const Xy &pc, qint64 tc, double vx, double vy)
{
    for (qsizetype k = 0; k + 1 < f.size(); ++k) {
        if (f[k].ms <= ms && f[k + 1].ms >= ms && segValid(f[k], f[k + 1])) {
            const qint64 dt = f[k + 1].ms - f[k].ms;
            const double u = dt > 0 ? double(ms - f[k].ms) / double(dt) : 0.0;
            return {xy[k].x + u * (xy[k + 1].x - xy[k].x), xy[k].y + u * (xy[k + 1].y - xy[k].y)};
        }
    }
    const double s = double(ms - tc) / 1000.0;
    return {pc.x + vx * s, pc.y + vy * s};
}
double bearingOf(const Xy &p)   // camera (origin) → p, compass degrees
{
    if (std::hypot(p.x, p.y) < 0.5) return nan();
    return std::fmod(std::atan2(p.x, p.y) / kD2R + 360.0, 360.0);
}
}

QList<Pass> detectPasses(const QList<TrackFix> &fixes, const Camera &cam)
{
    QList<Pass> out;
    const qsizetype n = fixes.size();
    if (n == 0) return out;
    const Proj pj(cam.lat, cam.lon);
    QList<Xy> xy; xy.reserve(n);
    QList<double> d; d.reserve(n);
    for (const TrackFix &f : fixes) { xy << pj.to(f.lat, f.lon); d << std::hypot(xy.last().x, xy.last().y); }
    // segments k = (k, k+1) near the camera → runs of linked fixes
    struct Run { qsizetype a, b; };
    QList<Run> runs;
    QList<bool> covered(n, false);
    for (qsizetype k = 0; k + 1 < n; ++k) {
        if (!segValid(fixes[k], fixes[k + 1])) continue;
        if (segClosest(xy[k], xy[k + 1], nullptr) > kPassRadiusM) continue;
        if (!runs.isEmpty() && runs.last().b == k) runs.last().b = k + 1;
        else runs.append({k, k + 1});
        covered[k] = covered[k + 1] = true;
    }
    for (qsizetype i = 0; i < n; ++i) if (!covered[i] && d[i] <= kPassRadiusM) runs.append({i, i});   // a lone fix (gaps on both sides)
    std::sort(runs.begin(), runs.end(), [](const Run &x, const Run &y) { return x.a < y.a; });
    const QList<double> dirs = parseDirections(directionText(cam));
    const Cone cone = coneFor(cam);

    for (const Run &r : runs) {
        Pass p;
        p.cameraId = cam.id;
        p.directions = dirs;
        // the closest approach on the polyline
        qsizetype seg = -1; double bestT = 0, best = 1e18;
        if (r.a == r.b) { best = d[r.a]; }
        for (qsizetype k = r.a; k < r.b; ++k) {
            double t = 0; const double dk = segClosest(xy[k], xy[k + 1], &t);
            if (dk < best) { best = dk; seg = k; bestT = t; }
        }
        const TrackFix &fa = fixes[seg < 0 ? r.a : seg];
        const TrackFix &fb = fixes[seg < 0 ? r.a : seg + 1];
        const double t = seg < 0 ? 0.0 : bestT;
        p.ms = fa.ms + qint64(std::llround(t * double(fb.ms - fa.ms)));
        p.lat = fa.lat + t * (fb.lat - fa.lat);
        p.lon = fa.lon + t * (fb.lon - fa.lon);
        if (fa.acc >= 0 && fb.acc >= 0) p.acc = fa.acc + t * (fb.acc - fa.acc);
        else p.acc = fa.acc >= 0 ? fa.acc : fb.acc;
        p.distanceM = best;
        const Xy pc = seg < 0 ? xy[r.a] : Xy{xy[seg].x + t * (xy[seg + 1].x - xy[seg].x), xy[seg].y + t * (xy[seg + 1].y - xy[seg].y)};
        double vx = 0, vy = 0;
        if (seg >= 0) {
            const double len = distanceM(fa.lat, fa.lon, fb.lat, fb.lon), dt = double(fb.ms - fa.ms) / 1000.0;
            const TrackFix &at = t < 0.5 ? fa : fb;
            if ((t == 0.0 || t == 1.0) && at.speedMps >= 0) p.speedKmh = at.speedMps * 3.6;
            else if (dt > 0) p.speedKmh = len / dt * 3.6;
            if (len >= 3.0) p.headingDeg = bearingDeg(fa.lat, fa.lon, fb.lat, fb.lon);
            if (dt > 0) { vx = (xy[seg + 1].x - xy[seg].x) / dt; vy = (xy[seg + 1].y - xy[seg].y) / dt; }
        } else if (fa.speedMps >= 0) p.speedKmh = fa.speedMps * 3.6;
        const double ab = bearingOf(pc);
        p.approachBearingDeg = std::isnan(ab) ? 0.0 : ab;
        // the run: fixes inside, time inside, entry / exit
        QStringList devs, srcs;
        for (qsizetype i = r.a; i <= r.b; ++i) {
            if (d[i] <= kPassRadiusM) ++p.fixes;
            if (!devs.contains(fixes[i].device)) devs << fixes[i].device;
            if (!fixes[i].source.isEmpty() && !srcs.contains(fixes[i].source)) srcs << fixes[i].source;
        }
        for (qsizetype k = r.a; k < r.b; ++k) {
            double t0 = 0, t1 = 0;
            if (segInside(xy[k], xy[k + 1], kPassRadiusM, &t0, &t1)) p.dwellS += (t1 - t0) * double(fixes[k + 1].ms - fixes[k].ms) / 1000.0;
        }
        p.devices = devs; p.sources = srcs;
        p.enterDistanceM = d[r.a]; p.exitDistanceM = d[r.b];
        p.startMs = fixes[r.a].ms; p.endMs = fixes[r.b].ms;
        // §2.2: camera → us 10 s before and after the closest approach
        const Xy before = positionAt(fixes, xy, p.ms - kFacingOffsetMs, pc, p.ms, vx, vy);
        const Xy after = positionAt(fixes, xy, p.ms + kFacingOffsetMs, pc, p.ms, vx, vy);
        p.frontBearingDeg = bearingOf(before); if (std::isnan(p.frontBearingDeg)) p.frontBearingDeg = p.approachBearingDeg;
        p.rearBearingDeg = bearingOf(after); if (std::isnan(p.rearBearingDeg)) p.rearBearingDeg = p.approachBearingDeg;
        // §2.3: the run's polyline sampled every ~0.2 s; inside the cone and the range before / after the closest approach
        p.cone = cone;
        struct Smp { double x, y, dt; bool before, in65; };
        QList<Smp> sm;
        if (r.a == r.b) sm.append({xy[r.a].x, xy[r.a].y, 0.0, true, d[r.a] <= kPassRadiusM});
        for (qsizetype k = r.a; k < r.b; ++k) {
            const double T = double(fixes[k + 1].ms - fixes[k].ms) / 1000.0;
            const double len = std::hypot(xy[k + 1].x - xy[k].x, xy[k + 1].y - xy[k].y);
            const int n = std::clamp(int(std::ceil(std::max(T / 0.2, len / 1.0))), 1, 2000);   // every 0.2 s and every metre
            for (int i2 = 0; i2 < n; ++i2) {
                const double u = (i2 + 0.5) / n;
                const double x = xy[k].x + u * (xy[k + 1].x - xy[k].x), y = xy[k].y + u * (xy[k + 1].y - xy[k].y);
                const double tms = double(fixes[k].ms) + u * double(fixes[k + 1].ms - fixes[k].ms);
                sm.append({x, y, T / n, tms < double(p.ms), std::hypot(x, y) <= kPassRadiusM});
            }
        }
        const double half = cone.halfDeg + kConeMarginDeg;
        auto inRange = [&](double x, double y) { const double dd = std::hypot(x, y); return dd >= cone.minM && dd <= cone.maxM; };
        auto inConeDir = [&](double x, double y, double dir) {
            if (!inRange(x, y)) return false;
            const double b = bearingOf({x, y});
            return !std::isnan(b) && angleDiff(b, dir) <= half;
        };
        auto inCone = [&](double x, double y) { for (double dir : dirs) if (inConeDir(x, y, dir)) return true; return false; };
        if (!dirs.isEmpty()) {
            for (const Smp &q : std::as_const(sm)) {
                if (!inCone(q.x, q.y)) continue;
                p.inConeS += q.dt;
                if (r.a == r.b) { p.frontVisible = p.rearVisible = true; }   // a lone fix: which plate is unknown
                else if (q.before) p.frontVisible = true; else p.rearVisible = true;
            }
            p.facing = (p.frontVisible || p.rearVisible) ? 1 : 0;
            double seen = nan();
            for (double dir : dirs) {
                for (const Smp &q : std::as_const(sm)) if (inConeDir(q.x, q.y, dir)) { seen = dir; break; }
                if (!std::isnan(seen)) break;
            }
            p.cameraDirDeg = std::isnan(seen) ? dirs.first() : seen;
            // §2.4: P(in cone and range) with the fix error integrated — the whole track shifted by a Gaussian offset
            // (fix errors are correlated over a pass), 7×7 Gauss–Hermite quadrature
            static const double gx[7] = {-2.651961356835233, -1.673551628767471, -0.816287882858965, 0.0, 0.816287882858965, 1.673551628767471, 2.651961356835233};
            static const double gw[7] = {0.0009717812450995, 0.05451558281912703, 0.4256072526101278, 0.8102646175568073, 0.4256072526101278, 0.05451558281912703, 0.0009717812450995};
            const double sigma = std::max(1.0, (p.acc > 0 ? p.acc : 8.0) / 1.515);   // accuracy = the 68 % radius; per-axis σ
            double P = 0;
            for (int ix = 0; ix < 7; ++ix)
                for (int iy = 0; iy < 7; ++iy) {
                    const double ox = std::sqrt(2.0) * sigma * gx[ix], oy = std::sqrt(2.0) * sigma * gx[iy];
                    bool hit = false;
                    for (const Smp &q : std::as_const(sm)) if (inCone(q.x + ox, q.y + oy)) { hit = true; break; }
                    if (hit) P += gw[ix] * gw[iy] / M_PI;
                }
            p.pInCone = std::clamp(P, 0.0, 1.0);
        } else {
            // no known direction: the fraction of the run (inside 65 m) within the range, × 0.5
            double within = 0, total = 0;
            for (const Smp &q : std::as_const(sm)) {
                if (!q.in65) continue;
                total += q.dt;
                if (inRange(q.x, q.y)) within += q.dt;
            }
            const double frac = total > 0 ? within / total : (inRange(xy[r.a].x, xy[r.a].y) ? 1.0 : 0.0);
            p.inConeS = within;
            p.pInCone = frac * 0.5;
        }
        p.pReadBase = p.pInCone * kCapture * kRead;
        p.trustFactor = cam.trust >= 0 ? cam.trust : 1.0;
        rescore(p, cam);
        out << p;
    }
    return out;
}

QList<Pass> mergePasses(QList<Pass> passes)
{
    std::sort(passes.begin(), passes.end(), [](const Pass &a, const Pass &b) { return a.cameraId != b.cameraId ? a.cameraId < b.cameraId : a.ms < b.ms; });
    QList<Pass> out;
    for (const Pass &p : std::as_const(passes)) {
        if (!out.isEmpty() && out.last().cameraId == p.cameraId && std::llabs(p.ms - out.last().ms) <= kMergeMs) {
            Pass &m = out.last();
            Pass keep = p.distanceM < m.distanceM ? p : m;
            const Pass &other = p.distanceM < m.distanceM ? m : p;
            for (const QString &s : other.devices) if (!keep.devices.contains(s)) keep.devices << s;
            for (const QString &s : other.sources) if (!keep.sources.contains(s)) keep.sources << s;
            keep.fixes = m.fixes + p.fixes;
            keep.dwellS = std::max(m.dwellS, p.dwellS);
            keep.startMs = std::min(m.startMs, p.startMs); keep.endMs = std::max(m.endMs, p.endMs);
            m = keep;
            continue;
        }
        out << p;
    }
    std::sort(out.begin(), out.end(), [](const Pass &a, const Pass &b) { return a.ms < b.ms; });
    return out;
}

QList<Pass> detectAll(const QList<TrackFix> &fixes, const Camera &cam)
{
    QHash<QString, QList<TrackFix>> byDev;
    for (const TrackFix &f : fixes) byDev[f.device].append(f);
    QList<Pass> all;
    for (auto it = byDev.begin(); it != byDev.end(); ++it) {
        std::stable_sort(it->begin(), it->end(), [](const TrackFix &a, const TrackFix &b) { return a.ms < b.ms; });
        all += detectPasses(*it, cam);
    }
    return mergePasses(all);
}

// ── §2.3 / §2.4 ───────────────────────────────────────────────────────────────
Cone coneFor(const Camera &cam)
{
    QStringList parts{cam.model, cam.manufacturer};
    for (const char *k : {"manufacturer", "brand", "camera:model", "model", "operator"}) parts << cam.tags.value(QLatin1String(k)).toString();
    parts << cam.operatorName;
    const QString t = parts.join(QLatin1Char(' '));
    auto has = [&](const char *re) { return QRegularExpression(QLatin1String(re), QRegularExpression::CaseInsensitiveOption).match(t).hasMatch(); };
    if (has("falcon[ -]*(lr|long)|long[- ]range")) return {7.0, 15.0, 76.0, QStringLiteral("Flock Falcon LR")};
    if (has("motorola|vigilant|\\bl5f\\b|\\bl6q\\b")) return {12.0, 8.0, 23.0, QStringLiteral("Motorola / Vigilant L5F, L6Q")};
    if (has("genetec|autovu|sharpv")) return {12.0, 3.0, 45.0, QStringLiteral("Genetec AutoVu SharpV")};
    if (has("verkada")) return {22.0, 3.0, 20.0, QStringLiteral("Verkada")};
    if (has("flock|falcon|sparrow")) return {10.0, 6.0, 25.0, QStringLiteral("Flock Falcon")};
    return Cone{};
}

int confidenceFor(const Pass &p, const Camera &cam, bool asAlpr)
{
    double c = 100.0 * p.pRead;
    if (cam.trust < 0 && suspectedOnly(cam.source, cam.model)) c *= 0.7;   // without a trust (§2.7) a suspected-only camera keeps the old factor
    if (classifyNote(cam.tags).contains(QLatin1String("misspelt"))) c *= 0.85;   // a misspelt ALPR tag: reduced confidence
    int v = int(std::lround(c));
    if (!asAlpr) v = std::min(40, v);
    return v;
}

void rescore(Pass &p, const Camera &cam)
{
    p.pRead = p.pReadBase * p.snapFactor * p.trustFactor;
    p.confidenceAlpr = confidenceFor(p, cam, true);
    p.confidence = cam.type == QLatin1String("alpr") || cam.type.isEmpty() ? p.confidenceAlpr : confidenceFor(p, cam, false);
}

int rescoreMetrics(QJsonObject &m, const QString &cameraType, double snapFactor, const QJsonObject &snap, double trustFactor, const QJsonObject &trustDetail)
{
    double base = m.value(QLatin1String("pReadBase")).toDouble(-1);
    if (base < 0) base = m.contains(QLatin1String("pInCone")) ? m.value(QLatin1String("pInCone")).toDouble() * kCapture * kRead : m.value(QLatin1String("pRead")).toDouble();
    const double pRead = base * snapFactor * trustFactor;
    double c = 100.0 * pRead;
    if (m.value(QLatin1String("typeNote")).toString().contains(QLatin1String("misspelt"))) c *= 0.85;
    const int alpr = int(std::lround(c));
    m["pReadBase"] = std::round(base * 1000) / 1000;
    m["snapFactor"] = std::round(snapFactor * 1000) / 1000;
    if (!snap.isEmpty()) m["snap"] = snap;
    m["trustFactor"] = std::round(trustFactor * 1000) / 1000;
    if (!trustDetail.isEmpty()) m["trust"] = trustDetail;
    m["pRead"] = std::round(pRead * 1000) / 1000;
    m["confidenceAlpr"] = alpr;
    return cameraType == QLatin1String("alpr") || cameraType.isEmpty() ? alpr : std::min(40, alpr);
}

QString typeLabel(const QString &type)
{
    if (type == QLatin1String("alpr") || type.isEmpty()) return QStringLiteral("ALPR");
    if (type == QLatin1String("webcam")) return QStringLiteral("public traffic webcam");
    if (type == QLatin1String("ptz")) return QStringLiteral("Flock PTZ video camera");
    if (type == QLatin1String("enforcement")) return QStringLiteral("speed / red-light camera");
    if (type == QLatin1String("not_camera")) return QStringLiteral("not a camera");
    return QStringLiteral("surveillance camera");
}

bool alertable(const QString &cameraType) { return cameraType == QLatin1String("alpr"); }

// ── §1.1 ──────────────────────────────────────────────────────────────────────
QString passUid(const QString &cameraId, qint64 closestMs)
{
    const qint64 minute = closestMs >= 0 ? closestMs / 60000 : -((-closestMs + 59999) / 60000);
    return QStringLiteral("pass:%1:%2").arg(cameraId).arg(minute);
}

static QString field(const QJsonObject &o, const char *k)
{
    const QJsonValue v = o.value(QLatin1String(k));
    if (v.isNull() || v.isUndefined()) return {};
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (d == std::floor(d) && std::fabs(d) < 9e15) return QString::number(qint64(d));
        return QString::number(d, 'g', 17);
    }
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return v.toString();
}

QString searchUid(const QJsonObject &row)
{
    const QString key = QStringList{field(row, "org_id"), field(row, "search_time_utc"), field(row, "license_plate_hash"), field(row, "case_number"),
                                    field(row, "reason"), field(row, "upload_id")}.join(QLatin1Char('|'));
    return QStringLiteral("hibf:") + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
}

// ── event rows ────────────────────────────────────────────────────────────────
QString isoLocal(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms).toString(Qt::ISODate); }

static QJsonValue num(double v, int decimals = -1)
{
    if (std::isnan(v)) return QJsonValue();
    if (decimals >= 0) { const double k = std::pow(10.0, decimals); return std::round(v * k) / k; }
    return v;
}

QString passDetails(const QString &cameraType, const QString &operatorName, const QString &model, double distanceM, int facing)
{
    const QString d = QString::number(qRound(distanceM));
    const QString op = operatorName.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(operatorName);
    if (cameraType == QLatin1String("webcam")) return QStringLiteral("Passed a public traffic webcam%1 %2 m away — it does not read plates.").arg(op, d);
    if (cameraType == QLatin1String("ptz")) return QStringLiteral("Passed a Flock PTZ video camera%1 %2 m away — a video camera, not a plate reader.").arg(op, d);
    if (cameraType == QLatin1String("enforcement")) return QStringLiteral("Passed a speed / red-light enforcement camera%1 %2 m away — it photographs only vehicles it catches.").arg(op, d);
    if (cameraType == QLatin1String("not_camera")) return QStringLiteral("Passed a sensor that is not a camera%1 %2 m away.").arg(op, d);
    if (cameraType != QLatin1String("alpr") && !cameraType.isEmpty()) return QStringLiteral("Passed a surveillance camera%1 %2 m away — it does not read plates.").arg(op, d);
    static const QRegularExpression generic(QStringLiteral("^(alpr|anpr|lpr|alpr camera|camera)$"), QRegularExpression::CaseInsensitiveOption);
    const QString who = QStringList{operatorName, generic.match(model.trimmed()).hasMatch() ? QString() : model}.join(QLatin1Char(' ')).simplified();
    const QString face = facing == 1 ? QStringLiteral("camera faced you") : facing == 0 ? QStringLiteral("camera faced away") : QStringLiteral("facing unknown");
    return QStringLiteral("Passed an ALPR camera%1 %2 m away: your plate was likely read (%3).")
        .arg(who.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(who), d, face);
}

static void cameraSourceLink(const Camera &cam, QString *url, QString *name)
{
    static const QRegularExpression osm(QStringLiteral("^osm:(node|way|relation)/(\\d+)$"));
    const auto m = osm.match(cam.id);
    if (m.hasMatch()) {
        *url = QStringLiteral("https://www.openstreetmap.org/%1/%2").arg(m.captured(1), m.captured(2));
        *name = QStringLiteral("OpenStreetMap");
        return;
    }
    if (cam.id.startsWith(QLatin1String("det:"))) {
        *url = QStringLiteral("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=19/%1/%2").arg(cam.lat, 0, 'f', 6).arg(cam.lon, 0, 'f', 6);
        *name = QStringLiteral("BeaconFix detection (%1)").arg(cam.source);
        return;
    }
    *url = QStringLiteral("https://deflock.me/map#map=19/%1/%2").arg(cam.lat, 0, 'f', 6).arg(cam.lon, 0, 'f', 6);
    *name = cam.source.isEmpty() ? QStringLiteral("DeFlock camera map") : QStringLiteral("DeFlock / flocklocations · %1").arg(cam.source);
}

QJsonObject passEvent(const Pass &p, const Camera &cam, const QString &plate, const QString &source, const QString &device, bool leaky, const QString &leakyMatch)
{
    QJsonArray dirs; for (double d : p.directions) dirs.append(d);
    QJsonObject metrics{{"dwellS", std::round(p.dwellS * 10) / 10}, {"fixes", p.fixes}, {"fixDevices", QJsonArray::fromStringList(p.devices)},
                        {"fixSources", QJsonArray::fromStringList(p.sources)}, {"enterDistanceM", num(p.enterDistanceM, 1)}, {"exitDistanceM", num(p.exitDistanceM, 1)},
                        {"frontBearingDeg", num(p.frontBearingDeg, 1)}, {"rearBearingDeg", num(p.rearBearingDeg, 1)},
                        {"frontVisible", p.directions.isEmpty() ? QJsonValue() : QJsonValue(p.frontVisible)},
                        {"rearVisible", p.directions.isEmpty() ? QJsonValue() : QJsonValue(p.rearVisible)},
                        {"cameraDirections", dirs}, {"plateInferred", true}, {"cameraSource", cam.source}, {"confidenceAlpr", p.confidenceAlpr},
                        {"suspectedOnly", suspectedOnly(cam.source, cam.model)}, {"startTime", isoLocal(p.startMs)}, {"endTime", isoLocal(p.endMs)},
                        {"coneHalfDeg", p.cone.halfDeg}, {"coneMarginDeg", kConeMarginDeg}, {"rangeM", QJsonArray{p.cone.minM, p.cone.maxM}}, {"coneModel", p.cone.name},
                        {"inConeS", std::round(p.inConeS * 10) / 10}, {"pInCone", std::round(p.pInCone * 1000) / 1000}, {"pRead", std::round(p.pRead * 1000) / 1000},
                        {"sourceConfidence", cam.sourceConfidence}, {"pReadBase", std::round(p.pReadBase * 1000) / 1000},
                        {"snapFactor", std::round(p.snapFactor * 1000) / 1000}, {"trustFactor", std::round(p.trustFactor * 1000) / 1000}};
    if (!p.snap.isEmpty()) metrics["snap"] = p.snap;
    if (!cam.trustDetail.isEmpty()) metrics["trust"] = cam.trustDetail;
    if (leaky) metrics["leakyMatch"] = leakyMatch;
    if (const QString note = classifyNote(cam.tags); !note.isEmpty()) metrics["typeNote"] = note;
    QJsonObject camRaw{{"id", cam.id}, {"lat", cam.lat}, {"lon", cam.lon}, {"source", cam.source}, {"model", cam.model}, {"operator", cam.operatorName},
                       {"direction", directionText(cam)}, {"type", cam.type}};
    if (!cam.webcam.isEmpty()) camRaw["webcam"] = cam.webcam;
    QJsonObject raw{{"camera", camRaw}};
    if (!cam.tags.isEmpty()) raw["osmTags"] = cam.tags;
    QString url, name;
    cameraSourceLink(cam, &url, &name);
    const QString type = cam.type.isEmpty() ? QStringLiteral("alpr") : cam.type;
    return QJsonObject{
        {"uid", passUid(cam.id, p.ms)}, {"kind", "camera_pass"}, {"plate", plate}, {"time", isoLocal(p.ms)},
        {"lat", p.lat}, {"lon", p.lon}, {"acc", p.acc >= 0 ? QJsonValue(std::round(p.acc * 10) / 10) : QJsonValue()},
        {"camera_id", cam.id}, {"camera_lat", cam.lat}, {"camera_lon", cam.lon},
        {"distance_m", std::round(p.distanceM * 10) / 10}, {"speed_kmh", num(p.speedKmh, 1)}, {"heading_deg", num(p.headingDeg, 1)},
        {"approach_bearing_deg", std::round(p.approachBearingDeg * 10) / 10}, {"camera_dir_deg", num(p.cameraDirDeg, 1)},
        {"facing", p.facing < 0 ? QJsonValue() : QJsonValue(p.facing)},
        {"operator", cam.operatorName}, {"agency", QJsonValue()}, {"model", cam.model}, {"camera_type", type},
        {"source", source}, {"source_url", url}, {"source_name", name}, {"confidence", p.confidence}, {"leaky", leaky ? 1 : 0},
        {"details", passDetails(type, cam.operatorName, cam.model, p.distanceM, p.facing)},
        {"metrics", metrics}, {"raw", raw}, {"device", device}};
}

// ── live ──────────────────────────────────────────────────────────────────────
static constexpr qint64 kFinishAfterMs = 120000;   // 2 min after the closest approach
static constexpr qint64 kKeepMs = 10 * 60 * 1000;

void LiveTracker::trim(qint64 nowMs)
{
    qint64 keepFrom = nowMs - kKeepMs;
    for (const Active &a : std::as_const(m_active)) keepFrom = std::min(keepFrom, a.enteredMs - kRunGapMs);
    while (m_buf.size() > 2 && m_buf.first().ms < keepFrom) m_buf.removeFirst();
}

QList<Camera> LiveTracker::activeCameras() const { QList<Camera> out; for (const Active &a : m_active) out << a.cam; return out; }
const Camera *LiveTracker::camera(const QString &id) const { for (const Active &a : m_active) if (a.cam.id == id) return &a.cam; return nullptr; }

QList<Pass> LiveTracker::finish(const QString &id)
{
    QList<Pass> out;
    for (qsizetype i = 0; i < m_active.size(); ++i) {
        if (m_active[i].cam.id != id) continue;
        const Active a = m_active.takeAt(i);
        const QList<Pass> ps = detectPasses(m_buf, a.cam);
        const Pass *best = nullptr;
        for (const Pass &p : ps) if (!best || std::llabs(p.ms - a.bestMs) < std::llabs(best->ms - a.bestMs)) best = &p;
        if (best) { out << *best; m_done[id] = Done{best->ms, !a.left}; }
        else m_done[id] = Done{a.bestMs, !a.left};
        break;
    }
    return out;
}

QList<Pass> LiveTracker::addFix(const TrackFix &f, const QList<Camera> &nearby)
{
    QList<Pass> out;
    if (!m_buf.isEmpty() && f.ms < m_buf.last().ms) return out;          // out of order: the backfill will see it
    if (!m_buf.isEmpty() && f.ms == m_buf.last().ms && f.lat == m_buf.last().lat && f.lon == m_buf.last().lon) return out;
    m_buf.append(f);
    const TrackFix *prev = m_buf.size() > 1 ? &m_buf[m_buf.size() - 2] : nullptr;
    const bool linked = prev && segValid(*prev, f);
    auto nearNow = [&](const Camera &c, double *dist, qint64 *atMs) {
        const Proj pj(c.lat, c.lon);
        const Xy b = pj.to(f.lat, f.lon);
        double dd = std::hypot(b.x, b.y); qint64 when = f.ms;
        if (linked) {
            double t = 0; const Xy a = pj.to(prev->lat, prev->lon);
            const double ds = segClosest(a, b, &t);
            if (ds < dd) { dd = ds; when = prev->ms + qint64(std::llround(t * double(f.ms - prev->ms))); }
        }
        *dist = dd; *atMs = when;
        return dd <= kPassRadiusM;
    };
    // the passes in progress
    QStringList toFinish;
    for (Active &a : m_active) {
        double dd = 0; qint64 when = 0;
        nearNow(a.cam, &dd, &when);
        if (dd < a.bestD) { a.bestD = dd; a.bestMs = when; }
        if (distanceM(f.lat, f.lon, a.cam.lat, a.cam.lon) > kPassRadiusM) { a.left = true; toFinish << a.cam.id; }   // left the circle
        else if (f.ms - a.bestMs >= kFinishAfterMs) toFinish << a.cam.id;
    }
    for (auto it = m_done.begin(); it != m_done.end(); ) {             // left the circle: a later pass may start
        const Camera *c = nullptr;
        for (const Camera &n : nearby) if (n.id == it.key()) c = &n;
        if (!c || distanceM(f.lat, f.lon, c->lat, c->lon) > kPassRadiusM) it->inside = false;
        if (!it->inside && f.ms - it->ms > kMergeMs) it = m_done.erase(it); else ++it;
    }
    // new ones
    for (const Camera &c : nearby) {
        if (camera(c.id) || toFinish.contains(c.id)) continue;
        const auto d = m_done.constFind(c.id);
        if (d != m_done.constEnd() && (d->inside || f.ms - d->ms <= kMergeMs)) continue;
        double dd = 0; qint64 when = 0;
        if (!nearNow(c, &dd, &when)) continue;
        Active a; a.cam = c; a.enteredMs = linked ? prev->ms : f.ms; a.bestD = dd; a.bestMs = when;
        m_active.append(a);
        if (distanceM(f.lat, f.lon, c.lat, c.lon) > kPassRadiusM) { m_active.last().left = true; toFinish << c.id; }   // crossed between two fixes
    }
    for (const QString &id : std::as_const(toFinish)) out += finish(id);
    trim(f.ms);
    return out;
}

QList<Pass> LiveTracker::tick(qint64 nowMs)
{
    QList<Pass> out;
    QStringList ids;
    for (const Active &a : std::as_const(m_active)) if (nowMs - a.bestMs >= kFinishAfterMs) ids << a.cam.id;
    for (const QString &id : std::as_const(ids)) out += finish(id);
    return out;
}

} // namespace PlateEvents
