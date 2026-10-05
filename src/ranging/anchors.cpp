// BeaconFix anchors — see anchors.h and docs/RANGING.md §4.
#include "anchors.h"
#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Anchors {

const QStringList kKinds = {QStringLiteral("this-computer"), QStringLiteral("wifi-ap"), QStringLiteral("ble"),
                            QStringLiteral("rtt-responder"), QStringLiteral("gnss"), QStringLiteral("fixed-point"),
                            QStringLiteral("esp32-node"), QStringLiteral("custom")};
static constexpr double kMPerDeg = 111320.0;
static const double kNaN = std::numeric_limits<double>::quiet_NaN();

static double normDeg(double d) { d = std::fmod(d, 360.0); return d < 0 ? d + 360.0 : d; }
static QDateTime isoTime(const QString &s) { return QDateTime::fromString(s, Qt::ISODateWithMs); }

QString normalizeBssid(const QString &b)
{
    QString h = b.trimmed().toUpper();
    h.remove(QLatin1Char(':')); h.remove(QLatin1Char('-')); h.remove(QLatin1Char('.'));
    static const QRegularExpression hex(QStringLiteral("^[0-9A-F]{12}$"));
    if (!hex.match(h).hasMatch()) return QString();
    QStringList p;
    for (int i = 0; i < 12; i += 2) p << h.mid(i, 2);
    return p.join(QLatin1Char(':'));
}

QString schemaSql()
{
    return QStringLiteral(
        "CREATE TABLE IF NOT EXISTS anchors (id TEXT PRIMARY KEY, json TEXT NOT NULL, kind TEXT, lat REAL, lon REAL, "
        "rv INTEGER NOT NULL DEFAULT 0, ref INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0, updated TEXT, seq INTEGER);\n"
        "CREATE INDEX IF NOT EXISTS anchors_seq ON anchors(seq);");
}

// ── JSON ─────────────────────────────────────────────────────────────────────
static const QStringList kKnownKeys = {
    QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("kind"), QStringLiteral("lat"), QStringLiteral("lon"),
    QStringLiteral("alt"), QStringLiteral("heightM"), QStringLiteral("floor"), QStringLiteral("accM"), QStringLiteral("bssids"),
    QStringLiteral("ble"), QStringLiteral("rv"), QStringLiteral("rvOffset"), QStringLiteral("ref"), QStringLiteral("headingDeg"),
    QStringLiteral("placedBy"), QStringLiteral("placedAt"), QStringLiteral("source"), QStringLiteral("headingAssumed"),
    QStringLiteral("deleted"), QStringLiteral("deletedAt"), QStringLiteral("seq")};

Anchor Anchor::fromJson(const QJsonObject &o, QString *error)
{
    Anchor a;
    auto fail = [&](const QString &e) { if (error) *error = e; };
    for (auto it = o.begin(); it != o.end(); ++it) if (!kKnownKeys.contains(it.key())) a.extra.insert(it.key(), it.value());
    a.id = o.value(QStringLiteral("id")).toString().trimmed();
    if (a.id.isEmpty()) a.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    a.deleted = o.value(QStringLiteral("deleted")).toBool(false);
    a.deletedAt = o.value(QStringLiteral("deletedAt")).toString();
    a.seq = qint64(o.value(QStringLiteral("seq")).toDouble(0));
    a.headingAssumed = o.value(QStringLiteral("headingAssumed")).toBool(false);
    if (a.deleted) return a;                                             // a tombstone needs nothing else
    a.name = o.value(QStringLiteral("name")).toString().left(120);
    a.kind = o.value(QStringLiteral("kind")).toString(QStringLiteral("custom"));
    if (a.kind.isEmpty()) a.kind = QStringLiteral("custom");
    if (!kKinds.contains(a.kind)) { fail(QStringLiteral("unknown kind '%1'").arg(a.kind)); a.kind = QStringLiteral("custom"); }
    const QJsonValue la = o.value(QStringLiteral("lat")), lo = o.value(QStringLiteral("lon"));
    if (!la.isDouble() || !lo.isDouble() || !std::isfinite(la.toDouble()) || !std::isfinite(lo.toDouble())
        || std::fabs(la.toDouble()) > 90.0 || std::fabs(lo.toDouble()) > 180.0) {
        fail(QStringLiteral("lat/lon missing or out of range"));
    }
    a.lat = la.toDouble(); a.lon = lo.toDouble();
    if (o.value(QStringLiteral("alt")).isDouble()) { a.hasAlt = true; a.alt = o.value(QStringLiteral("alt")).toDouble(); }
    if (o.value(QStringLiteral("heightM")).isDouble()) { a.hasHeight = true; a.heightM = o.value(QStringLiteral("heightM")).toDouble(); }
    if (o.value(QStringLiteral("floor")).isDouble()) { a.hasFloor = true; a.floor = o.value(QStringLiteral("floor")).toInt(); }
    const double acc = o.value(QStringLiteral("accM")).toDouble(1.0);
    a.accM = (std::isfinite(acc) && acc > 0) ? std::max(0.05, std::min(500.0, acc)) : 1.0;
    for (const QJsonValue &v : o.value(QStringLiteral("bssids")).toArray()) {
        const QString b = normalizeBssid(v.toString());
        if (!b.isEmpty() && !a.bssids.contains(b)) a.bssids << b;
    }
    a.ble = o.value(QStringLiteral("ble")).toString();
    a.rv = o.value(QStringLiteral("rv")).toBool(false);
    const QJsonObject off = o.value(QStringLiteral("rvOffset")).toObject();
    if (!off.isEmpty()) {
        a.hasRvOffset = true;
        a.rvOffset = {off.value(QStringLiteral("eastM")).toDouble(), off.value(QStringLiteral("northM")).toDouble(), off.value(QStringLiteral("upM")).toDouble()};
    }
    a.ref = o.value(QStringLiteral("ref")).toBool(false);
    if (o.value(QStringLiteral("headingDeg")).isDouble()) { a.hasHeading = true; a.headingDeg = normDeg(o.value(QStringLiteral("headingDeg")).toDouble()); }
    a.placedBy = o.value(QStringLiteral("placedBy")).toString(QStringLiteral("desktop"));
    a.placedAt = o.value(QStringLiteral("placedAt")).toString();
    if (a.placedAt.isEmpty()) a.placedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    a.source = o.value(QStringLiteral("source")).toString(QStringLiteral("map-pick"));
    return a;
}

QJsonObject Anchor::toJson(bool outputFields) const
{
    if (deleted) {
        QJsonObject t{{"id", id}, {"deleted", true}, {"deletedAt", deletedAt}};
        if (outputFields) t.insert(QStringLiteral("seq"), double(seq));
        return t;
    }
    QJsonObject o = extra;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("kind"), kind);
    o.insert(QStringLiteral("lat"), lat);
    o.insert(QStringLiteral("lon"), lon);
    if (hasAlt) o.insert(QStringLiteral("alt"), alt);
    if (hasHeight) o.insert(QStringLiteral("heightM"), heightM);
    if (hasFloor) o.insert(QStringLiteral("floor"), floor);
    o.insert(QStringLiteral("accM"), accM);
    o.insert(QStringLiteral("bssids"), QJsonArray::fromStringList(bssids));
    if (!ble.isEmpty()) o.insert(QStringLiteral("ble"), ble);
    o.insert(QStringLiteral("rv"), rv);
    if (hasRvOffset) o.insert(QStringLiteral("rvOffset"), QJsonObject{{"eastM", rvOffset.e}, {"northM", rvOffset.n}, {"upM", rvOffset.u}});
    o.insert(QStringLiteral("ref"), ref);
    if (hasHeading) o.insert(QStringLiteral("headingDeg"), headingDeg);
    o.insert(QStringLiteral("placedBy"), placedBy);
    o.insert(QStringLiteral("placedAt"), placedAt);
    o.insert(QStringLiteral("source"), source);
    if (outputFields) {
        o.insert(QStringLiteral("headingAssumed"), headingAssumed);
        o.insert(QStringLiteral("seq"), double(seq));
    }
    return o;
}

// ── local frame ──────────────────────────────────────────────────────────────
Enu enu(double refLat, double refLon, double refAlt, double lat, double lon, double alt)
{
    Enu o;
    o.e = (lon - refLon) * kMPerDeg * std::cos(refLat * M_PI / 180.0);
    o.n = (lat - refLat) * kMPerDeg;
    o.u = (std::isnan(alt) || std::isnan(refAlt)) ? 0.0 : alt - refAlt;
    return o;
}

void fromEnu(double refLat, double refLon, double refAlt, const Enu &o, double *lat, double *lon, double *alt)
{
    if (lat) *lat = refLat + o.n / kMPerDeg;
    if (lon) *lon = refLon + o.e / (kMPerDeg * std::cos(refLat * M_PI / 180.0));
    if (alt) *alt = std::isnan(refAlt) ? kNaN : refAlt + o.u;
}

void rotate(double e, double n, double deg, double *e2, double *n2)
{
    const double t = deg * M_PI / 180.0;
    *e2 = e * std::cos(t) + n * std::sin(t);
    *n2 = -e * std::sin(t) + n * std::cos(t);
}

double distanceM(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0, d2r = M_PI / 180.0;
    const double dLat = (lat2 - lat1) * d2r, dLon = (lon2 - lon1) * d2r;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) + std::cos(lat1 * d2r) * std::cos(lat2 * d2r) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * R * std::asin(std::min(1.0, std::sqrt(a)));
}

// ── uses ─────────────────────────────────────────────────────────────────────
const Anchor *reference(const QList<Anchor> &all)
{
    const Anchor *best = nullptr;
    for (const Anchor &a : all) if (!a.deleted && a.ref && (!best || isoTime(a.placedAt) > isoTime(best->placedAt))) best = &a;
    if (best) return best;
    for (const Anchor &a : all) if (!a.deleted && a.kind == QLatin1String("this-computer") && (!best || isoTime(a.placedAt) > isoTime(best->placedAt))) best = &a;
    return best;
}

void placeRv(Anchor &a, const Anchor &ref, bool haveHeading, double headingDeg)
{
    a.rvOffset = enu(ref.lat, ref.lon, ref.hasAlt ? ref.alt : kNaN, a.lat, a.lon, a.hasAlt ? a.alt : kNaN);
    a.hasRvOffset = true;
    a.rv = true;
    if (haveHeading) { a.hasHeading = true; a.headingDeg = normDeg(headingDeg); }
    a.headingAssumed = false;
}

Reprojected reproject(const Enu &offset, bool hasHeadingPlaced, double headingPlacedDeg,
                      double newRefLat, double newRefLon, bool newRefHasAlt, double newRefAlt,
                      bool haveNewHeading, double newHeadingDeg)
{
    Reprojected r;
    const bool rotateIt = haveNewHeading && hasHeadingPlaced;
    double e = offset.e, n = offset.n;
    if (rotateIt) rotate(offset.e, offset.n, newHeadingDeg - headingPlacedDeg, &e, &n);
    double alt = 0;
    fromEnu(newRefLat, newRefLon, newRefHasAlt ? newRefAlt : kNaN, Enu{e, n, offset.u}, &r.lat, &r.lon, &alt);
    r.hasAlt = newRefHasAlt;
    r.alt = newRefHasAlt ? alt : 0;
    r.headingAssumed = !rotateIt;
    return r;
}

QList<Anchor> reprojectAll(const QList<Anchor> &all, double newRefLat, double newRefLon, bool newRefHasAlt, double newRefAlt,
                           bool haveNewHeading, double newHeadingDeg)
{
    QList<Anchor> out = all;
    const Anchor *ref = reference(all);
    const QString refId = ref ? ref->id : QString();
    for (Anchor &a : out) {
        if (a.deleted) continue;
        if (a.id == refId) {
            a.lat = newRefLat; a.lon = newRefLon;
            if (newRefHasAlt) { a.hasAlt = true; a.alt = newRefAlt; }
            a.headingAssumed = false;
            continue;
        }
        if (!a.rv || !a.hasRvOffset) continue;
        const Reprojected r = reproject(a.rvOffset, a.hasHeading, a.headingDeg, newRefLat, newRefLon, newRefHasAlt, newRefAlt,
                                        haveNewHeading, newHeadingDeg);
        a.lat = r.lat; a.lon = r.lon;
        if (r.hasAlt) { a.hasAlt = true; a.alt = r.alt; }
        a.headingAssumed = r.headingAssumed;
    }
    return out;
}

bool shouldReproject(double oldRefLat, double oldRefLon, double stopLat, double stopLon)
{
    return distanceM(oldRefLat, oldRefLon, stopLat, stopLon) > 250.0;
}

QList<Anchor> normalizeSet(QList<Anchor> all)
{
    QList<int> refs;
    for (int i = 0; i < all.size(); ++i) if (!all[i].deleted && all[i].ref) refs << i;
    std::sort(refs.begin(), refs.end(), [&](int a, int b) { return isoTime(all[a].placedAt) > isoTime(all[b].placedAt); });
    QList<int> kept;
    for (int i : refs) {
        bool clash = false;
        for (int k : kept) if (distanceM(all[i].lat, all[i].lon, all[k].lat, all[k].lon) <= 100.0) clash = true;
        if (clash) all[i].ref = false; else kept << i;
    }
    return all;
}

AnchorFix thisComputerFix(const QList<Anchor> &all, double ownLat, double ownLon)
{
    AnchorFix f;
    double best = 1e18;
    for (const Anchor &a : all) {
        if (a.deleted || a.kind != QLatin1String("this-computer")) continue;
        const double d = distanceM(ownLat, ownLon, a.lat, a.lon);
        if (d <= 100.0 && d < best) {
            best = d;
            f.valid = true; f.lat = a.lat; f.lon = a.lon; f.acc = a.accM; f.hasAlt = a.hasAlt; f.alt = a.alt; f.anchorId = a.id;
        }
    }
    return f;
}

QHash<QString, Anchor> pinnedBssids(const QList<Anchor> &all)
{
    QHash<QString, Anchor> m;
    for (const Anchor &a : all) {
        if (a.deleted) continue;
        if (a.kind != QLatin1String("wifi-ap") && a.kind != QLatin1String("rtt-responder") && a.kind != QLatin1String("this-computer")) continue;
        for (const QString &b : a.bssids) {
            auto it = m.find(b);
            if (it == m.end() || isoTime(a.placedAt) > isoTime(it->placedAt)) m.insert(b, a);
        }
    }
    return m;
}

// ── trilateration ────────────────────────────────────────────────────────────
Trilat trilaterate(const std::vector<RangeToAnchor> &ranges, double priorLat, double priorLon, double priorSigmaM)
{
    Trilat t;
    const int m = int(ranges.size());
    t.used = m;
    if (m == 0) return t;
    std::vector<double> E(m), N(m), W(m);
    for (int i = 0; i < m; ++i) {
        const Enu p = enu(priorLat, priorLon, kNaN, ranges[i].lat, ranges[i].lon, kNaN);
        E[i] = p.e; N[i] = p.n;
        const double s2 = ranges[i].sigmaM * ranges[i].sigmaM + ranges[i].anchorAccM * ranges[i].anchorAccM;
        W[i] = 1.0 / std::max(1e-6, s2);
    }
    const double w0 = 1.0 / (priorSigmaM * priorSigmaM);
    double pe = 0, pn = 0, A00 = 0, A01 = 0, A11 = 0;
    auto build = [&](double &g0, double &g1) {
        A00 = w0; A01 = 0; A11 = w0;
        g0 = -w0 * pe; g1 = -w0 * pn;
        for (int i = 0; i < m; ++i) {
            const double dx = pe - E[i], dy = pn - N[i];
            const double d = std::sqrt(dx * dx + dy * dy);
            double j0, j1;
            if (d < 0.01) { j0 = 0.0; j1 = 1.0; } else { j0 = dx / d; j1 = dy / d; }
            const double r = ranges[i].rangeM - std::max(d, 0.01);
            A00 += W[i] * j0 * j0; A01 += W[i] * j0 * j1; A11 += W[i] * j1 * j1;
            g0 += W[i] * j0 * r; g1 += W[i] * j1 * r;
        }
    };
    for (int it = 0; it < 30; ++it) {
        double g0, g1;
        build(g0, g1);
        const double det = A00 * A11 - A01 * A01;
        if (!(std::fabs(det) > 1e-300)) break;
        const double s0 = (A11 * g0 - A01 * g1) / det, s1 = (-A01 * g0 + A00 * g1) / det;
        pe += s0; pn += s1;
        t.iterations = it + 1;
        if (std::fabs(s0) < 1e-6 && std::fabs(s1) < 1e-6) break;
    }
    double g0, g1;
    build(g0, g1);
    const double det = A00 * A11 - A01 * A01;
    if (!(std::fabs(det) > 1e-300)) return t;
    t.sigmaM = std::sqrt(0.5 * (A11 / det + A00 / det));
    double ss = 0;
    for (int i = 0; i < m; ++i) { const double r = ranges[i].rangeM - std::sqrt((pe - E[i]) * (pe - E[i]) + (pn - N[i]) * (pn - N[i])); ss += r * r; }
    t.rmsM = std::sqrt(ss / m);
    fromEnu(priorLat, priorLon, kNaN, Enu{pe, pn, 0}, &t.lat, &t.lon, nullptr);
    t.valid = true;
    return t;
}

// ── rv-gnss ──────────────────────────────────────────────────────────────────
RvGnss rvGnssPosition(double gnssLat, double gnssLon, double gnssSigmaM,
                      const Anchor *gnssAnchor, const Anchor *thisAnchor, bool haveHeading, double headingDeg)
{
    RvGnss r;
    if (gnssAnchor && thisAnchor && gnssAnchor->hasRvOffset && thisAnchor->hasRvOffset) {
        double de = thisAnchor->rvOffset.e - gnssAnchor->rvOffset.e;
        double dn = thisAnchor->rvOffset.n - gnssAnchor->rvOffset.n;
        const double dz = thisAnchor->rvOffset.u - gnssAnchor->rvOffset.u;
        if (haveHeading && gnssAnchor->hasHeading) { double e2, n2; rotate(de, dn, headingDeg - gnssAnchor->headingDeg, &e2, &n2); de = e2; dn = n2; }
        fromEnu(gnssLat, gnssLon, kNaN, Enu{de, dn, dz}, &r.lat, &r.lon, nullptr);
        r.sigmaM = std::sqrt(gnssSigmaM * gnssSigmaM + gnssAnchor->accM * gnssAnchor->accM + thisAnchor->accM * thisAnchor->accM);
    } else {
        r.lat = gnssLat; r.lon = gnssLon;
        r.sigmaM = std::sqrt(gnssSigmaM * gnssSigmaM + 25.0);
    }
    r.valid = gnssSigmaM <= 5.0;
    return r;
}

} // namespace Anchors
