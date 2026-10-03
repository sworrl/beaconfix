// Golden vectors for the estimator (docs/GRADING.md §7): the desktop engine (src/estimator.cpp) and its
// Kotlin twin (android/…/estimate/Estimator.kt) must produce the same fits, scores and grades.
//
//   estimator_golden --write tests/fixtures/estimator_golden.json   regenerate (after a deliberate change to BOTH engines)
//   estimator_golden --check tests/fixtures/estimator_golden.json   verify this build (CTest runs this)
//
// Android: app/src/test/…/estimate/EstimatorGoldenTest.kt reads the same file. Synthetic data only
// (origin 40.0 / −75.0, devices "phone" / "tablet").
//   g++ -std=c++17 -O2 $(pkg-config --cflags Qt6Core) tests/estimator_golden.cpp src/estimator.cpp -o build/estimator_golden $(pkg-config --libs Qt6Core)
#include "../src/estimator.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <random>

using namespace Estimator;

static const double AP_LAT = 40.0030, AP_LON = -75.0680, P0 = -38.0, N = 2.6;
static const qint64 T0 = 1790000000;
static std::mt19937 rng(20260928);

static double mLat(double m) { return m / 111320.0; }
static double mLon(double m) { return m / (111320.0 * std::cos(AP_LAT * M_PI / 180)); }
static Obs at(double east, double north, double acc, double db, qint64 t = T0, const QString &dev = QString(), double offset = 0)
{
    Obs o; o.lat = AP_LAT + mLat(north); o.lon = AP_LON + mLon(east); o.acc = acc; o.t = t; o.device = dev;
    const double d = std::sqrt(std::pow(distanceM(o.lat, o.lon, AP_LAT, AP_LON), 2) + 9.0);
    std::normal_distribution<double> nd(0.0, db), pn(0.0, acc / 1.515 * 0.6);
    o.dbm = int(std::lround(modelDbm(P0, N, d) + offset + nd(rng)));
    o.lat += mLat(pn(rng)); o.lon += mLon(pn(rng));
    return o;
}

// ── JSON ──
static QJsonObject obsJson(const Obs &o)
{
    QJsonObject j{{"lat", o.lat}, {"lon", o.lon}, {"acc", o.acc}, {"dbm", o.dbm}, {"t", double(o.t)}};
    if (o.weight != 1) j["weight"] = o.weight;
    if (!o.device.isEmpty()) j["device"] = o.device;
    if (o.rangeM > 0) { j["rangeM"] = o.rangeM; j["rangeSd"] = o.rangeSd; }
    return j;
}
static Obs obsFrom(const QJsonObject &j)
{
    Obs o; o.lat = j["lat"].toDouble(); o.lon = j["lon"].toDouble(); o.acc = j["acc"].toDouble(); o.dbm = j["dbm"].toInt(); o.t = qint64(j["t"].toDouble());
    o.weight = j["weight"].toDouble(1); o.device = j["device"].toString(); o.rangeM = j["rangeM"].toDouble(-1); o.rangeSd = j["rangeSd"].toDouble(0);
    return o;
}
static QJsonObject fitJson(const Fit &f)
{
    return QJsonObject{
        {"valid", f.valid}, {"kind", f.kind}, {"grade", f.grade}, {"pendingGrade", f.pendingGrade}, {"quality", f.quality}, {"score", f.score},
        {"lat", f.lat}, {"lon", f.lon}, {"acc", f.acc}, {"r95", f.r95}, {"cep50", f.cep50}, {"pWithin25", f.pWithin25},
        {"cxx", f.cxx}, {"cxy", f.cxy}, {"cyy", f.cyy}, {"semiMajor", f.semiMajor}, {"semiMinor", f.semiMinor}, {"orient", f.orientDeg},
        {"p0", f.p0}, {"pathloss", f.pathloss}, {"n", f.n}, {"vantage", f.vantage}, {"rejected", f.rejected}, {"rms", f.rms},
        {"rssDop", f.rssDop}, {"crlbR95", f.crlbR95}, {"rbar", f.rbar}, {"maxGapDeg", f.maxGapDeg}, {"inHull", f.inHull}, {"linRatio", f.linRatio},
        {"chi2nu", f.chi2nu}, {"sigmaDb", f.sigmaDb}, {"outlierFrac", f.outlierFrac}, {"ess", f.ess}, {"sessions", f.sessions}, {"devices", f.devices},
        {"spearman", f.spearman}, {"p0RangeCorr", f.p0RangeCorr}, {"dminRatio", f.dminRatio}, {"ambiguous", f.ambiguous}, {"modes", f.modes},
        {"driftD2", f.driftD2}, {"extD2", f.extD2}, {"jackMax", f.jackMax}, {"nisEwma", f.nisEwma}, {"fadingDb", f.fadingDb}, {"moved", f.moved},
        {"newest", double(f.newest)}, {"suggestLat", f.suggestLat}, {"suggestLon", f.suggestLon}, {"suggestGain", f.suggestGain},
        {"cP", f.cP}, {"cG", f.cG}, {"cE", f.cE}, {"cF", f.cF}, {"cFfit", f.cFfit}, {"cS", f.cS}, {"cT", f.cT}, {"cX", f.cX}, {"updated", double(f.updated)}};
}
static Fit fitFrom(const QJsonObject &j)
{
    Fit f;
    f.valid = j["valid"].toBool(); f.kind = j["kind"].toString(); f.grade = j["grade"].toString(); f.pendingGrade = j["pendingGrade"].toString();
    f.quality = j["quality"].toString(); f.score = j["score"].toDouble(); f.lat = j["lat"].toDouble(); f.lon = j["lon"].toDouble(); f.acc = j["acc"].toDouble();
    f.r95 = j["r95"].toDouble(); f.cep50 = j["cep50"].toDouble(); f.pWithin25 = j["pWithin25"].toDouble();
    f.cxx = j["cxx"].toDouble(); f.cxy = j["cxy"].toDouble(); f.cyy = j["cyy"].toDouble(); f.semiMajor = j["semiMajor"].toDouble(); f.semiMinor = j["semiMinor"].toDouble();
    f.orientDeg = j["orient"].toDouble(); f.p0 = j["p0"].toDouble(); f.pathloss = j["pathloss"].toDouble(); f.n = j["n"].toInt(); f.vantage = j["vantage"].toInt();
    f.rejected = j["rejected"].toInt(); f.rms = j["rms"].toDouble(); f.rssDop = j["rssDop"].toDouble(); f.crlbR95 = j["crlbR95"].toDouble();
    f.rbar = j["rbar"].toDouble(); f.maxGapDeg = j["maxGapDeg"].toDouble(); f.inHull = j["inHull"].toBool(); f.linRatio = j["linRatio"].toDouble();
    f.chi2nu = j["chi2nu"].toDouble(); f.sigmaDb = j["sigmaDb"].toDouble(); f.outlierFrac = j["outlierFrac"].toDouble(); f.ess = j["ess"].toDouble();
    f.sessions = j["sessions"].toInt(); f.devices = j["devices"].toInt(); f.spearman = j["spearman"].toDouble(); f.p0RangeCorr = j["p0RangeCorr"].toDouble();
    f.dminRatio = j["dminRatio"].toDouble(); f.ambiguous = j["ambiguous"].toBool(); f.modes = j["modes"].toInt(); f.driftD2 = j["driftD2"].toDouble();
    f.extD2 = j["extD2"].toDouble(); f.jackMax = j["jackMax"].toDouble(); f.nisEwma = j["nisEwma"].toDouble(); f.fadingDb = j["fadingDb"].toDouble();
    f.moved = j["moved"].toBool(); f.newest = qint64(j["newest"].toDouble()); f.suggestLat = j["suggestLat"].toDouble(); f.suggestLon = j["suggestLon"].toDouble();
    f.suggestGain = j["suggestGain"].toDouble(); f.cP = j["cP"].toDouble(); f.cG = j["cG"].toDouble(); f.cE = j["cE"].toDouble(); f.cF = j["cF"].toDouble();
    f.cFfit = j["cFfit"].toDouble(); f.cS = j["cS"].toDouble(); f.cT = j["cT"].toDouble(); f.cX = j["cX"].toDouble(); f.updated = qint64(j["updated"].toDouble());
    return f;
}
static QJsonObject optJson(const Options &o)
{
    return QJsonObject{{"defaultN", o.defaultN}, {"nSd", o.nSd}, {"p0Mean", o.p0Mean}, {"p0Sd", o.p0Sd}, {"kappa", o.kappa}, {"bootstrap", o.bootstrap}, {"devOffsetSd", o.devOffsetSd}};
}
static Options optFrom(const QJsonObject &j)
{
    Options o; o.defaultN = j["defaultN"].toDouble(o.defaultN); o.nSd = j["nSd"].toDouble(o.nSd); o.p0Mean = j["p0Mean"].toDouble(o.p0Mean);
    o.p0Sd = j["p0Sd"].toDouble(o.p0Sd); o.kappa = j["kappa"].toDouble(o.kappa); o.bootstrap = j["bootstrap"].toInt(o.bootstrap);
    o.devOffsetSd = j["devOffsetSd"].toDouble(o.devOffsetSd);
    return o;
}
static QJsonObject ctxJson(const Context &c)
{
    QJsonObject j;
    QJsonObject off; for (auto it = c.deviceOffset.constBegin(); it != c.deviceOffset.constEnd(); ++it) off[it.key()] = it.value();
    j["deviceOffset"] = off;
    QJsonArray m; for (const Miss &x : c.misses) m.append(QJsonObject{{"lat", x.lat}, {"lon", x.lon}, {"count", x.count}});
    j["misses"] = m;
    if (c.external.has) j["external"] = QJsonObject{{"lat", c.external.lat}, {"lon", c.external.lon}, {"acc", c.external.acc}};
    j["mobile"] = c.mobile;
    if (c.hasPrev) j["prev"] = fitJson(c.prev);
    return j;
}
static Context ctxFrom(const QJsonObject &j)
{
    Context c;
    const QJsonObject off = j["deviceOffset"].toObject(); for (auto it = off.begin(); it != off.end(); ++it) c.deviceOffset.insert(it.key(), it.value().toDouble());
    for (const QJsonValue &v : j["misses"].toArray()) { Miss m; m.lat = v["lat"].toDouble(); m.lon = v["lon"].toDouble(); m.count = v["count"].toInt(1); c.misses << m; }
    if (j.contains("external")) { const QJsonObject e = j["external"].toObject(); c.external.has = true; c.external.lat = e["lat"].toDouble(); c.external.lon = e["lon"].toDouble(); c.external.acc = e["acc"].toDouble(); }
    c.mobile = j["mobile"].toBool();
    if (j.contains("prev")) { c.hasPrev = true; c.prev = fitFrom(j["prev"].toObject()); }
    return c;
}

struct Case { QString name; QList<Obs> obs; qint64 now = T0; Options opt; Context ctx; };

static QList<Case> scenarios()
{
    QList<Case> cs;
    auto ringOf = [](int n, double r0, double dr, double acc, double db) { QList<Obs> o; for (int i = 0; i < n; ++i) { const double a = i * 2 * M_PI / n, r = r0 + (i % 4) * dr; o << at(r * std::cos(a), r * std::sin(a), acc, db); } return o; };
    { Case c; c.name = "ring-24"; c.obs = ringOf(24, 50, 40, 8, 3); cs << c; }
    { Case c; c.name = "ring-8-noisy"; c.obs = ringOf(8, 60, 30, 15, 6); cs << c; }
    { Case c; c.name = "three-places"; c.obs << at(-60, 20, 12, 4) << at(50, 55, 12, 4) << at(15, -70, 12, 4); cs << c; }
    { Case c; c.name = "road"; for (int i = 0; i < 14; ++i) c.obs << at(-220 + 34 * i, -45, 8, 3); cs << c; }
    { Case c; c.name = "one-sample"; c.obs << at(70, 10, 10, 3); cs << c; }
    { Case c; c.name = "two-samples-misses"; c.obs << at(70, 10, 10, 3) << at(-10, 75, 10, 3);
      for (int i = 0; i < 6; ++i) { Miss m; m.lat = AP_LAT + mLat(-180 + 40 * (i % 3)); m.lon = AP_LON + mLon(-160 + 60 * (i / 3)); m.count = 2 + i % 3; c.ctx.misses << m; } cs << c; }
    { Case c; c.name = "one-spot"; for (int i = 0; i < 10; ++i) c.obs << at(90, 40, 20, 3); cs << c; }
    { Case c; c.name = "coarse-only"; for (int i = 0; i < 4; ++i) c.obs << at(-150 + 100 * i, 30, 180, 4); cs << c; }
    { Case c; c.name = "outliers"; c.obs = ringOf(20, 50, 40, 8, 3); for (int i = 0; i < 3; ++i) { Obs o; o.lat = AP_LAT + 0.0027; o.lon = AP_LON + 0.0005 * i; o.acc = 8; o.dbm = -40; o.t = T0; c.obs << o; } cs << c; }
    { Case c; c.name = "two-devices-offset"; for (int i = 0; i < 16; ++i) { const double a = i * 2 * M_PI / 16, r = 60 + (i % 3) * 40; c.obs << at(r * std::cos(a), r * std::sin(a), 8, 2.5, T0 - (i % 5) * 86400, i % 2 ? QStringLiteral("phone") : QString(), i % 2 ? 7.0 : 0.0); }
      c.ctx.deviceOffset.insert(QStringLiteral("phone"), 7.0); cs << c; }
    // per-AP device deviations δ: the phone is the reference (most places), the tablet hears this AP 7 dB quieter than its
    // calibration, the host is calibrated; places never mix devices
    { Case c; c.name = "three-devices-deviation";
      for (int i = 0; i < 21; ++i) { const double a = i * 2 * M_PI / 21, r = 45 + (i % 4) * 30;
          const QString dev = i % 3 == 0 ? QStringLiteral("tablet") : i % 3 == 1 ? QStringLiteral("phone") : (i % 2 ? QStringLiteral("phone") : QString());
          const double off = dev == QLatin1String("tablet") ? -4.0 - 7.0 : dev == QLatin1String("phone") ? 3.0 : 0.0;
          c.obs << at(r * std::cos(a), r * std::sin(a), 6, 3, T0 - (i % 3) * 86400, dev, off); }
      c.obs << at(20, 25, 6, 3, T0, QStringLiteral("tablet"), -11.0) << at(22, 27, 6, 3, T0, QStringLiteral("phone"), 3.0);   // two devices at one spot
      c.ctx.deviceOffset.insert(QStringLiteral("phone"), 3.0); c.ctx.deviceOffset.insert(QStringLiteral("tablet"), -4.0); cs << c; }
    // more devices than δ slots: the four with the fewest places beyond the reference … the sixth stays pinned; a tighter prior
    { Case c; c.name = "six-devices-cap"; c.opt.devOffsetSd = 6;
      const char *names[6] = {"phone", "tablet", "deck", "laptop", "pi", "watch"};
      for (int i = 0; i < 30; ++i) { const double a = i * 2 * M_PI / 30, r = 50 + (i % 4) * 20; const int d = i % 3 == 0 ? 0 : 1 + (i % 5);
          c.obs << at(r * std::cos(a), r * std::sin(a), 8, 3, T0, QString::fromLatin1(names[d]), (d - 2) * 2.5); }
      cs << c; }
    { Case c; c.name = "rtt"; for (int i = 0; i < 4; ++i) { const double a = i * 2 * M_PI / 4 + 0.4; Obs o = at(70 * std::cos(a), 70 * std::sin(a), 8, 4); o.rangeM = std::sqrt(std::pow(distanceM(o.lat, o.lon, AP_LAT, AP_LON), 2) + 9) + 1.5; o.rangeSd = 2; c.obs << o; } cs << c; }
    { Case c; c.name = "moved"; for (int i = 0; i < 10; ++i) { const double a = i * 2 * M_PI / 10; Obs o = at(80 * std::cos(a), 80 * std::sin(a) + 300, 8, 2, T0 - 200 * 86400);
          o.dbm = int(std::lround(modelDbm(P0, N, std::sqrt(std::pow(distanceM(o.lat, o.lon, AP_LAT + mLat(300), AP_LON), 2) + 9)))); c.obs << o; }
      for (int i = 0; i < 10; ++i) { const double a = i * 2 * M_PI / 10; c.obs << at(80 * std::cos(a), 80 * std::sin(a), 8, 2, T0 - 4 * 86400); } cs << c; }
    { Case c; c.name = "mobile-5km"; for (int i = 0; i < 3; ++i) c.obs << at(40 * i, 20, 10, 3); for (int i = 0; i < 3; ++i) { Obs o = at(7000 + 40 * i, 20, 10, 3); o.dbm = -62; c.obs << o; } cs << c; }
    { Case c; c.name = "stale-external-clash"; for (int i = 0; i < 12; ++i) { const double a = i * 2 * M_PI / 12; c.obs << at(90 * std::cos(a), 90 * std::sin(a), 10, 3, T0 - 300 * 86400); }
      c.ctx.external.has = true; c.ctx.external.lat = AP_LAT + mLat(400); c.ctx.external.lon = AP_LON; c.ctx.external.acc = 25; cs << c; }
    { Case c; c.name = "weights-kappa-priors"; c.obs = ringOf(10, 40, 30, 12, 4); c.obs[0].weight = 0.3; c.obs[3].weight = 0.3;
      c.opt.kappa = 1.4; c.opt.p0Mean = -47; c.opt.defaultN = 2.7; c.opt.bootstrap = 16; cs << c; }
    // hysteresis/drift: the previous fit is the ring-24 fit moved 30 m, graded B
    {
        Case c; c.name = "prev-drift";
        c.obs = ringOf(18, 50, 40, 8, 3);
        Fit prev = fitAp(c.obs, T0); prev.lat += mLat(30); prev.grade = QStringLiteral("B"); prev.driftD2 = 1.0;
        c.ctx.hasPrev = true; c.ctx.prev = prev; cs << c;
    }
    return cs;
}

static QList<Obs> updateStream()
{
    QList<Obs> s; for (int i = 0; i < 12; ++i) s << at(90 * std::sin(i * 0.7), 90 * std::cos(i * 0.7), 8, 2.5, T0 + 60 * i);
    return s;
}

static QList<Known> selfScenario(bool moved)
{
    const double me_lat = 40.0040, me_lon = -75.0690;
    QList<Known> known;
    for (int i = 0; i < 6; ++i) {
        const double ang = i * 2 * M_PI / 6 + 0.3, rad = 60 + i * 15;
        Known k; k.lat = me_lat + rad * std::sin(ang) / 111320.0; k.lon = me_lon + rad * std::cos(ang) / (111320.0 * std::cos(me_lat * M_PI / 180));
        k.acc = 20; k.p0 = -38; k.pathloss = 2.6; k.haveModel = i % 3 != 0; k.cxx = i % 2 ? 300 : 0; k.cyy = i % 2 ? 150 : 0; k.cxy = i % 2 ? 40 : 0; k.weight = i == 4 ? 0.5 : 1;
        std::normal_distribution<double> nd(0.0, 3.0);
        k.dbm = int(std::lround(modelDbm(k.p0, k.pathloss, rad) + nd(rng)));
        known << k;
    }
    if (moved) known[2].lat += 500.0 / 111320.0;
    return known;
}
static QJsonObject knownJson(const Known &k)
{
    return QJsonObject{{"lat", k.lat}, {"lon", k.lon}, {"acc", k.acc}, {"dbm", k.dbm}, {"p0", k.p0}, {"pathloss", k.pathloss}, {"haveModel", k.haveModel},
                       {"cxx", k.cxx}, {"cxy", k.cxy}, {"cyy", k.cyy}, {"weight", k.weight}};
}
static Known knownFrom(const QJsonObject &j)
{
    Known k; k.lat = j["lat"].toDouble(); k.lon = j["lon"].toDouble(); k.acc = j["acc"].toDouble(); k.dbm = j["dbm"].toInt(); k.p0 = j["p0"].toDouble();
    k.pathloss = j["pathloss"].toDouble(); k.haveModel = j["haveModel"].toBool(); k.cxx = j["cxx"].toDouble(); k.cxy = j["cxy"].toDouble(); k.cyy = j["cyy"].toDouble();
    k.weight = j["weight"].toDouble(1);
    return k;
}
static QJsonObject selfJson(const SelfFix &s)
{
    return QJsonObject{{"valid", s.valid}, {"lat", s.lat}, {"lon", s.lon}, {"acc", s.acc}, {"rms", s.rms}, {"used", s.used}, {"rejected", s.rejected},
                       {"excluded", s.excluded}, {"integrity", s.integrity}, {"r95", s.r95}};
}

// ── compare ──
static int bad = 0;
static void cmp(const QString &where, const QJsonObject &want, const QJsonObject &got)
{
    for (auto it = want.begin(); it != want.end(); ++it) {
        const QJsonValue g = got.value(it.key());
        if (it.value().isDouble()) {
            const double a = it.value().toDouble(), b = g.toDouble();
            const double tol = (it.key() == QLatin1String("lat") || it.key() == QLatin1String("lon") || it.key().startsWith(QLatin1String("suggestL"))) ? 1e-9 : 1e-6 * std::max(1.0, std::fabs(a));
            if (!(std::fabs(a - b) <= tol)) { ++bad; std::printf("MISMATCH %s.%s: want %.12g got %.12g\n", qPrintable(where), qPrintable(it.key()), a, b); }
        } else if (it.value() != g) { ++bad; std::printf("MISMATCH %s.%s: want %s got %s\n", qPrintable(where), qPrintable(it.key()), qPrintable(it.value().toVariant().toString()), qPrintable(g.toVariant().toString())); }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: estimator_golden --write|--check <file>\n"); return 2; }
    const QString mode = QString::fromLocal8Bit(argv[1]), path = QString::fromLocal8Bit(argv[2]);
    if (mode == QLatin1String("--write")) {
        QJsonArray cases, updates, selfs, helpers;
        const QList<Case> cs = scenarios();
        for (const Case &c : cs) {
            QJsonArray obs; for (const Obs &o : c.obs) obs.append(obsJson(o));
            const Fit f = fitAp(c.obs, c.now, c.opt, c.ctx);
            cases.append(QJsonObject{{"name", c.name}, {"now", double(c.now)}, {"options", optJson(c.opt)}, {"context", ctxJson(c.ctx)}, {"samples", obs}, {"expect", fitJson(f)}});
            std::printf("%-22s %-7s %-2s %5.1f  R95 %7.1f  places %d\n", qPrintable(c.name), qPrintable(f.kind), qPrintable(f.grade), f.score, f.r95, f.vantage);
        }
        {   // incremental updates starting from the ring-8-noisy fit
            Fit f = fitAp(cs[1].obs, cs[1].now, cs[1].opt, cs[1].ctx);
            QJsonArray stream, steps;
            for (const Obs &o : updateStream()) { f = update(f, o); stream.append(obsJson(o)); steps.append(fitJson(f)); }
            updates.append(QJsonObject{{"fromCase", 1}, {"stream", stream}, {"expect", steps}});
        }
        {   // the same stream heard by a device 6 dB louder than this host, with its calibrated offset passed to update()
            Fit f = fitAp(cs[1].obs, cs[1].now, cs[1].opt, cs[1].ctx);
            QJsonArray stream, steps;
            for (Obs o : updateStream()) { o.device = QStringLiteral("phone"); o.dbm += 6; f = update(f, o, Options(), 6.0); QJsonObject j = obsJson(o); j["offsetDb"] = 6.0; stream.append(j); steps.append(fitJson(f)); }
            updates.append(QJsonObject{{"fromCase", 1}, {"stream", stream}, {"expect", steps}});
        }
        for (bool moved : {false, true}) {
            const QList<Known> k = selfScenario(moved);
            QJsonArray ka; for (const Known &x : k) ka.append(knownJson(x));
            selfs.append(QJsonObject{{"known", ka}, {"expect", selfJson(selfLocate(k))}});
        }
        for (const auto &h : QList<QList<double>>{{10, 10, 0.5}, {10, 10, 0.95}, {30, 4, 0.95}, {5, 0.01, 0.5}, {100, 60, 0.95}})
            helpers.append(QJsonObject{{"s1", h[0]}, {"s2", h[1]}, {"p", h[2]}, {"r", radiusFor(h[0], h[1], h[2])}, {"pAt25", probWithin(h[0], h[1], 25.0)}});
        const QJsonObject root{{"estimatorVersion", kVersion}, {"note", "Generated by tests/estimator_golden.cpp --write; synthetic data only."},
                               {"cases", cases}, {"updates", updates}, {"self", selfs}, {"helpers", helpers}};
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) { std::fprintf(stderr, "cannot write %s\n", qPrintable(path)); return 1; }
        out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        std::printf("wrote %d cases\n", int(cases.size()));
        return 0;
    }
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "cannot read %s\n", qPrintable(path)); return 1; }
    const QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    QList<Fit> fits;
    for (const QJsonValue &v : root["cases"].toArray()) {
        const QJsonObject c = v.toObject();
        QList<Obs> obs; for (const QJsonValue &o : c["samples"].toArray()) obs << obsFrom(o.toObject());
        const Fit f = fitAp(obs, qint64(c["now"].toDouble()), optFrom(c["options"].toObject()), ctxFrom(c["context"].toObject()));
        fits << f;
        cmp(c["name"].toString(), c["expect"].toObject(), fitJson(f));
    }
    for (const QJsonValue &v : root["updates"].toArray()) {
        Fit f = fits.value(v["fromCase"].toInt());
        const QJsonArray stream = v["stream"].toArray(), steps = v["expect"].toArray();
        for (int i = 0; i < stream.size(); ++i) {
            f = update(f, obsFrom(stream[i].toObject()), Options(), stream[i].toObject()["offsetDb"].toDouble(0));
            cmp(QStringLiteral("update[%1]").arg(i), steps[i].toObject(), fitJson(f));
        }
    }
    int si = 0;
    for (const QJsonValue &v : root["self"].toArray()) {
        QList<Known> k; for (const QJsonValue &x : v["known"].toArray()) k << knownFrom(x.toObject());
        cmp(QStringLiteral("self[%1]").arg(si++), v["expect"].toObject(), selfJson(selfLocate(k)));
    }
    for (const QJsonValue &v : root["helpers"].toArray()) {
        const QJsonObject h = v.toObject();
        cmp(QStringLiteral("helper"), QJsonObject{{"r", h["r"]}, {"pAt25", h["pAt25"]}},
            QJsonObject{{"r", radiusFor(h["s1"].toDouble(), h["s2"].toDouble(), h["p"].toDouble())}, {"pAt25", probWithin(h["s1"].toDouble(), h["s2"].toDouble(), 25.0)}});
    }
    std::printf("%s: %d case(s), %d mismatch(es)\n", bad ? "FAILED" : "ALL PASSED", int(root["cases"].toArray().size()), bad);
    return bad ? 1 : 0;
}
