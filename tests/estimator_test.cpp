// Synthetic-geometry tests for src/estimator.{h,cpp} (docs/GRADING.md). Build — or use the CMake target
// estimator_test (cmake -DBEACONFIX_TESTS=ON):
//   g++ -std=c++17 -O2 -Wall -Wextra -fPIC $(pkg-config --cflags Qt6Core) tests/estimator_test.cpp src/estimator.cpp -o build/estimator_test $(pkg-config --libs Qt6Core)
#include "../src/estimator.h"
#include <QString>
#include <cstdio>
#include <random>
#include <algorithm>
#include <vector>

using namespace Estimator;
static int fails = 0;
#define CHECK(cond, fmt, ...) do { if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } else { std::printf("ok   " fmt "\n", ##__VA_ARGS__); } } while (0)

// Synthetic origin (docs/DEVELOPMENT.md): never real places
static const double AP_LAT = 40.0030, AP_LON = -75.0680, P0 = -38.0, N = 2.6;
static const qint64 T0 = 1790000000;
static std::mt19937 rng(42);

static double mLat(double north) { return north / 111320.0; }
static double mLon(double east) { return east / (111320.0 * std::cos(AP_LAT * M_PI / 180)); }

static Obs sample(double lat, double lon, double acc, double dbNoise, qint64 t = T0, double jitter = 0.6, const QString &dev = QString(), double offset = 0)
{
    Obs o; o.lat = lat; o.lon = lon; o.acc = acc; o.t = t; o.device = dev;
    const double d = std::sqrt(std::pow(distanceM(lat, lon, AP_LAT, AP_LON), 2) + 9.0);
    std::normal_distribution<double> nd(0.0, dbNoise);
    o.dbm = int(std::lround(modelDbm(P0, N, d) + offset + nd(rng)));
    std::normal_distribution<double> pn(0.0, acc / 1.515 * jitter);
    o.lat += mLat(pn(rng)); o.lon += mLon(pn(rng));
    return o;
}
static Obs at(double east, double north, double acc, double db, qint64 t = T0, const QString &dev = QString(), double offset = 0)
{
    return sample(AP_LAT + mLat(north), AP_LON + mLon(east), acc, db, t, 0.6, dev, offset);
}
static double err(const Fit &f) { return distanceM(f.lat, f.lon, AP_LAT, AP_LON); }

int main()
{
    // 0. Helpers
    {
        const double p = probWithin(10, 10, 11.774);
        CHECK(std::fabs(p - 0.5) < 0.002, "circular CEP = 1.1774σ: P = %.4f", p);
        const double r = radiusFor(10, 10, 0.95);
        CHECK(std::fabs(r - 24.477) < 0.05, "circular R95 = 2.4477σ: %.3f", r);
        const double r1 = radiusFor(10, 0.001, 0.95);
        CHECK(std::fabs(r1 - 19.6) < 0.2, "degenerate ellipse R95 → 1.96σ: %.2f", r1);
        CHECK(std::fabs(normCdf(1.959964) - 0.975) < 1e-6, "normCdf(1.96) = %.7f", normCdf(1.959964));
        CHECK(letterFor(90) == QLatin1String("A") && letterFor(70) == QLatin1String("B") && letterFor(19.9) == QLatin1String("F"), "letters from scores");
    }
    // 1. 40 noisy observations on a ring of radius 60–180 m around the AP → error < 15 m, grade A/B
    Fit ring;
    {
        QList<Obs> obs;
        for (int i = 0; i < 40; ++i) {
            const double ang = i * 2 * M_PI / 40, rad = 60 + (i % 5) * 30;
            obs << at(rad * std::cos(ang), rad * std::sin(ang), 8, 3.0);
        }
        const Fit f = fitAp(obs, T0);
        ring = f;
        CHECK(f.valid && f.kind == QLatin1String("fix") && err(f) < 15.0, "40 obs around the AP: error %.1f m (R95 %.1f, rms %.1f dB, p0 %.1f, n %.2f, places %d, %s %.0f)", err(f), f.r95, f.rms, f.p0, f.pathloss, f.vantage, qPrintable(f.grade), f.score);
        CHECK(std::fabs(f.p0 - P0) < 6 && std::fabs(f.pathloss - N) < 0.5, "model recovered: p0 %.1f (true %.1f), n %.2f (true %.2f)", f.p0, P0, f.pathloss, N);
        CHECK(f.r95 >= err(f), "R95 %.1f covers the error %.1f", f.r95, err(f));
        CHECK(f.grade == QLatin1String("A") || f.grade == QLatin1String("B"), "surrounded, many places: grade %s (score %.1f, P %.2f G %.2f E %.2f F %.2f S %.2f T %.2f)", qPrintable(f.grade), f.score, f.cP, f.cG, f.cE, f.cF, f.cS, f.cT);
        CHECK(f.inHull && f.rbar < 0.2 && f.linRatio > 0.5 && f.modes == 1 && !f.ambiguous, "geometry metrics: inHull %d rbar %.2f lin %.2f modes %d", f.inHull, f.rbar, f.linRatio, f.modes);
        // 3 dB of noise against σ0 = 6: the noise-scale posterior takes the R95 below what the CRLB allows at σ0
        CHECK(f.rssDop > 0 && f.rssDop < 200 && f.crlbR95 > 0 && f.r95 < f.crlbR95, "DOP %.1f m, R95 %.1f < CRLB R95 at σ0 %.1f (σ %.1f dB)", f.rssDop, f.r95, f.crlbR95, f.sigmaDb);
        CHECK(f.spearman < -0.5, "distance decay: Spearman %.2f", f.spearman);
        CHECK(f.quality == QLatin1String("good"), "compat quality %s", qPrintable(f.quality));
    }
    // 2. Degenerate geometry: 12 samples from one spot → at most a region
    {
        QList<Obs> obs;
        for (int i = 0; i < 12; ++i) obs << sample(AP_LAT + 0.0008, AP_LON + 0.0003, 20, 3.0, T0, 0.3);
        const Fit f = fitAp(obs, T0);
        CHECK(f.kind != QLatin1String("fix") && (f.grade == QLatin1String("R") || !f.valid), "one place: %s grade %s (R95 %.0f)", qPrintable(f.kind), qPrintable(f.grade), f.r95);
    }
    // 2b. Three spots, all inside the fix error → not a fix
    {
        QList<Obs> obs;
        for (int i = 0; i < 9; ++i) obs << sample(AP_LAT + 0.0008 + (i % 3) * 0.0003, AP_LON, 90, 3.0, T0, 0.15);
        const Fit f = fitAp(obs, T0);
        CHECK(f.kind != QLatin1String("fix"), "spread inside the fix error: %s (places %d, R95 %.0f)", qPrintable(f.kind), f.vantage, f.r95);
    }
    // 3. Outliers: 3 samples at −40 dBm from 300 m away must not move the fit by more than a few metres
    {
        QList<Obs> obs;
        for (int i = 0; i < 40; ++i) {
            const double ang = i * 2 * M_PI / 40, rad = 50 + (i % 4) * 40;
            obs << at(rad * std::cos(ang), rad * std::sin(ang), 8, 3.0);
        }
        const Fit clean = fitAp(obs, T0);
        for (int i = 0; i < 3; ++i) { Obs o; o.lat = AP_LAT + 0.0027; o.lon = AP_LON + 0.0005 * i; o.acc = 8; o.dbm = -40; o.t = T0; obs << o; }
        const Fit f = fitAp(obs, T0);
        CHECK(clean.valid && err(clean) < 25.0, "baseline without outliers: error %.1f m (R95 %.1f)", err(clean), clean.r95);
        CHECK(f.valid && f.rejected >= 2 && err(f) < err(clean) + 10.0 && err(f) < 30.0, "outliers rejected: error %.1f m (was %.1f), rejected %d, outliers %.2f", err(f), err(clean), f.rejected, f.outlierFrac);
    }
    // 4. Self-locate from 5 known APs (their positions ±20 m) → < 30 m
    {
        const double me_lat = 40.0040, me_lon = -75.0690;
        QList<Known> known;
        for (int i = 0; i < 5; ++i) {
            const double ang = i * 2 * M_PI / 5 + 0.3, rad = 70 + i * 15;
            Known k; k.lat = me_lat + rad * std::sin(ang) / 111320.0; k.lon = me_lon + rad * std::cos(ang) / (111320.0 * std::cos(me_lat * M_PI / 180));
            k.acc = 20; k.p0 = -38; k.pathloss = 2.6; k.haveModel = true;
            std::normal_distribution<double> nd(0.0, 3.0);
            k.dbm = int(std::lround(modelDbm(k.p0, k.pathloss, rad) + nd(rng)));
            known << k;
        }
        const SelfFix s = selfLocate(known);
        const double e = distanceM(s.lat, s.lon, me_lat, me_lon);
        CHECK(s.valid && e < 30.0, "self-locate from 5 APs: error %.1f m (acc ±%.1f, R95 %.1f, used %d, %s)", e, s.acc, s.r95, s.used, qPrintable(s.integrity));
        CHECK(s.acc >= e * 0.5, "self-locate accuracy honest: acc %.1f vs err %.1f", s.acc, e);
        // one AP moved 400 m: the integrity check excludes it
        known[2].lat += 400.0 / 111320.0;
        const SelfFix s2 = selfLocate(known);
        const double e2 = distanceM(s2.lat, s2.lon, me_lat, me_lon);
        CHECK(s2.valid && s2.excluded >= 1 && e2 < 45.0, "integrity: a moved AP is excluded (%d, %s, error %.1f m)", s2.excluded, qPrintable(s2.integrity), e2);
    }
    // 5. Incremental update nudges towards the truth, keeps the covariance, and never explodes
    {
        QList<Obs> obs;
        for (int i = 0; i < 10; ++i) { const double ang = i * 2 * M_PI / 10; obs << at(90 * std::cos(ang), 90 * std::sin(ang), 10, 3.0); }
        Fit f = fitAp(obs, T0);
        f.lat += mLat(20);                                                 // perturb the fit 20 m north
        const double before = err(f), r95 = f.r95;
        for (int i = 0; i < 30; ++i) f = update(f, sample(AP_LAT + 0.0009 * std::sin(i), AP_LON + 0.0009 * std::cos(i), 8, 2.0));
        CHECK(err(f) < before && err(f) < 40, "incremental updates: %.1f m → %.1f m (R95 %.1f → %.1f, n %d, NIS %.2f, %s)", before, err(f), r95, f.r95, f.n, f.nisEwma, qPrintable(f.grade));
        CHECK(f.r95 <= r95 && f.cxx > 0 && f.cyy > 0, "the covariance shrinks and stays positive");
    }
    // 6. One or two samples: a region, never a fix
    {
        QList<Obs> obs; obs << at(80, 0, 8, 3);
        const Fit f1 = fitAp(obs, T0);
        CHECK(f1.valid && f1.kind == QLatin1String("region") && f1.grade == QLatin1String("R") && err(f1) < f1.r95, "one sample: region R95 %.0f m covers error %.0f m", f1.r95, err(f1));
        obs << at(0, 80, 8, 3);
        const Fit f2 = fitAp(obs, T0);
        CHECK(f2.valid && f2.kind == QLatin1String("region") && err(f2) < f2.r95, "two samples: region R95 %.0f m covers error %.0f m", f2.r95, err(f2));
        // not heard 200 m on the far side: the region shrinks towards where it was heard
        Context c; for (int i = 0; i < 8; ++i) { Miss m; m.lat = AP_LAT + mLat(-200 + 50 * (i % 3)); m.lon = AP_LON + mLon(-200 + 40 * (i / 3)); m.count = 3; c.misses << m; }
        const Fit f3 = fitAp(obs, T0, Options(), c);
        CHECK(f3.valid && f3.r95 < f2.r95, "misses shrink the region: R95 %.0f → %.0f m", f2.r95, f3.r95);
    }
    // 7. Drive-by: a straight road past the AP → mirror ambiguity or extrapolation, never an A
    {
        QList<Obs> obs;
        for (int i = 0; i < 16; ++i) obs << at(-240 + 32 * i, -50, 8, 3.0);    // the road runs 50 m south of the AP
        const Fit f = fitAp(obs, T0);
        CHECK(f.valid && f.linRatio < 0.02 && !f.inHull, "road: collinear places (lin %.4f, inHull %d)", f.linRatio, f.inHull);
        CHECK(f.score <= 59.0 && f.grade != QLatin1String("A") && f.grade != QLatin1String("B"), "road: capped score %.1f grade %s (ambiguous %d, modes %d, error %.1f m)", f.score, qPrintable(f.grade), f.ambiguous, f.modes, err(f));
        CHECK(f.suggestGain > 0, "road: a place to sample next is suggested (gain %.2f)", f.suggestGain);
    }
    // 8. The AP travels: heard 8 km apart → mobile, grade M
    {
        QList<Obs> obs;
        for (int i = 0; i < 4; ++i) obs << at(40 * i, 30, 10, 3.0);
        for (int i = 0; i < 4; ++i) { Obs o = at(8000 + 40 * i, 30, 10, 3.0); o.dbm = -60; obs << o; }
        const Fit f = fitAp(obs, T0);
        CHECK(f.kind == QLatin1String("mobile") && f.grade == QLatin1String("M") && !f.valid, "heard 8 km apart: %s %s", qPrintable(f.kind), qPrintable(f.grade));
        Context c; c.mobile = true;
        const Fit g = fitAp(obs, T0, Options(), c);
        CHECK(g.grade == QLatin1String("M"), "locator says it travels: grade %s", qPrintable(g.grade));
    }
    // 9. The AP moved 300 m: the recent epoch wins
    {
        QList<Obs> obs;
        for (int i = 0; i < 12; ++i) { const double ang = i * 2 * M_PI / 12; obs << sample(AP_LAT + 0.0027 + mLat(80 * std::sin(ang)), AP_LON + mLon(80 * std::cos(ang)), 8, 2.0, T0 - 200 * 86400); }
        // those were generated relative to the true AP: shift their levels as if the AP had been 300 m north
        for (Obs &o : obs) { const double d = std::sqrt(std::pow(distanceM(o.lat, o.lon, AP_LAT + 0.0027, AP_LON), 2) + 9); o.dbm = int(std::lround(modelDbm(P0, N, d))); }
        for (int i = 0; i < 12; ++i) { const double ang = i * 2 * M_PI / 12; obs << sample(AP_LAT + mLat(80 * std::sin(ang)), AP_LON + mLon(80 * std::cos(ang)), 8, 2.0, T0 - 5 * 86400); }
        const Fit f = fitAp(obs, T0);
        CHECK(f.valid && f.moved && err(f) < 25, "moved AP: moved %d, error %.1f m from the new place", f.moved, err(f));
    }
    // 10. A second device hearing 8 dB louder: its per-AP deviation δ absorbs the offset even before it is calibrated, so
    //     the uncalibrated fit matches the calibrated one
    {
        QList<Obs> obs;
        for (int i = 0; i < 24; ++i) { const double ang = i * 2 * M_PI / 24, rad = 60 + (i % 3) * 40; obs << at(rad * std::cos(ang), rad * std::sin(ang), 8, 2.5, T0, i % 2 ? QStringLiteral("phone") : QString(), i % 2 ? 8.0 : 0.0); }
        const Fit raw = fitAp(obs, T0);
        Context c; c.deviceOffset.insert(QStringLiteral("phone"), 8.0);
        const Fit fixd = fitAp(obs, T0, Options(), c);
        CHECK(fixd.valid && raw.valid && std::fabs(raw.rms - fixd.rms) < 0.5 && err(raw) < err(fixd) + 3 && fixd.devices == 2,
              "device offset absorbed: rms %.1f vs %.1f dB, error %.1f vs %.1f m", raw.rms, fixd.rms, err(raw), err(fixd));
    }
    // 10b. A second device that hears THIS AP 7 dB quieter than its calibration says, at a third of the places: δ takes it,
    //      the position does not move (was: those places dragged the fit — the Steam Deck vs the hidden AP at the desk)
    {
        std::vector<double> e0, e1;
        for (int trial = 0; trial < 20; ++trial) {
            QList<Obs> one, two;
            for (int i = 0; i < 24; ++i) {
                const double ang = i * 2 * M_PI / 24, rad = 40 + (i % 4) * 30;
                const bool deck = i % 3 == 0;
                one << at(rad * std::cos(ang), rad * std::sin(ang), 6, 4.0, T0, QStringLiteral("phone"));
                two << at(rad * std::cos(ang), rad * std::sin(ang), 6, 4.0, T0, deck ? QStringLiteral("deck") : QStringLiteral("phone"), deck ? -7.0 : 0.0);
            }
            e0.push_back(err(fitAp(one, T0))); e1.push_back(err(fitAp(two, T0)));
        }
        std::sort(e0.begin(), e0.end()); std::sort(e1.begin(), e1.end());
        CHECK(e1[10] < e0[10] + 4, "a quieter second device does not drag the fit: median error %.1f m (one device %.1f m)", e1[10], e0[10]);
    }
    // 10c. Incremental update: a device's calibrated offset is removed from its level, as the batch fit does
    {
        Fit base = ring;
        const Frame fr(base.lat, base.lon);
        Obs o = at(100, 0, 5, 0.0, T0, QStringLiteral("phone"), 8.0);      // heard 8 dB louder than this host would
        const Fit raw = update(base, o, Options(), 0.0), cal = update(base, o, Options(), 8.0);
        Obs h = at(100, 0, 5, 0.0, T0); h.lat = o.lat; h.lon = o.lon; h.dbm = o.dbm - 8;
        const Fit host = update(base, h, Options(), 0.0);
        CHECK(std::fabs(distanceM(cal.lat, cal.lon, host.lat, host.lon)) < 1e-6 && distanceM(raw.lat, raw.lon, host.lat, host.lon) > 0.01,
              "update with the device offset equals the host's update (uncorrected differs by %.2f m)", distanceM(raw.lat, raw.lon, host.lat, host.lon));
        (void)fr;
    }
    // 11. Wi-Fi RTT ranges tighten a thin fit
    {
        QList<Obs> obs;
        for (int i = 0; i < 4; ++i) { const double ang = i * 2 * M_PI / 4 + 0.4; obs << at(70 * std::cos(ang), 70 * std::sin(ang), 8, 4.0); }
        const Fit plain = fitAp(obs, T0);
        for (Obs &o : obs) { o.rangeM = std::sqrt(std::pow(distanceM(o.lat, o.lon, AP_LAT, AP_LON), 2) + 9) + 1.0; o.rangeSd = 2.0; }
        const Fit rtt = fitAp(obs, T0);
        CHECK(rtt.valid && rtt.r95 < plain.r95 && err(rtt) < 25, "RTT: R95 %.1f → %.1f m, error %.1f → %.1f m", plain.r95, rtt.r95, err(plain), err(rtt));
    }
    // 12. Hysteresis: a score just across a boundary keeps the letter once, flips on the second refit
    {
        Fit prev = ring; prev.grade = QStringLiteral("B"); prev.pendingGrade.clear();
        Fit f = ring; f.cP = f.cG = f.cE = f.cF = f.cS = f.cT = 0.855; f.cX = -1; f.ambiguous = false; f.modes = 1; f.inHull = true;   // score 85.5: an A by 0.5
        grade(f, &prev);
        CHECK(f.grade == QLatin1String("B") && f.pendingGrade == QLatin1String("A"), "hysteresis holds B at %.1f (pending %s)", f.score, qPrintable(f.pendingGrade));
        Fit g = f; grade(g, &f);
        CHECK(g.grade == QLatin1String("A"), "second refit confirms A (%s)", qPrintable(g.grade));
    }
    // 13. Stale data fades the freshness component; an external position that disagrees costs points
    {
        QList<Obs> obs;
        for (int i = 0; i < 20; ++i) { const double ang = i * 2 * M_PI / 20; obs << at(80 * std::cos(ang), 80 * std::sin(ang), 8, 3.0, T0 - 400 * 86400); }
        const Fit old = fitAp(obs, T0);
        CHECK(old.cT < 0.3 + 1e-9 + 0.01 && old.cT >= 0.3, "400-day-old samples: freshness %.2f", old.cT);
        Context agree; agree.external.has = true; agree.external.lat = AP_LAT + mLat(10); agree.external.lon = AP_LON; agree.external.acc = 25;
        Context clash = agree; clash.external.lat = AP_LAT + mLat(600);
        const Fit a = fitAp(obs, T0, Options(), agree), b = fitAp(obs, T0, Options(), clash);
        CHECK(a.extD2 >= 0 && a.extD2 < 3 && b.extD2 > 20 && b.score < a.score, "external agreement: D² %.1f vs %.1f, score %.1f vs %.1f", a.extD2, b.extD2, a.score, b.score);
    }
    // 14. Calibration: honest coverage on random ring geometries (≥ 85 % inside R95)
    {
        int inside = 0, total = 0;
        for (int trial = 0; trial < 60; ++trial) {
            QList<Obs> obs;
            std::uniform_real_distribution<double> ua(0, 2 * M_PI), ur(30, 200);
            const int n = 5 + trial % 12;
            for (int i = 0; i < n; ++i) { const double a = ua(rng), r = ur(rng); obs << at(r * std::cos(a), r * std::sin(a), 12, 6.0); }
            const Fit f = fitAp(obs, T0);
            if (f.kind != QLatin1String("fix")) continue;
            ++total; if (err(f) <= f.r95) ++inside;
        }
        CHECK(total >= 20 && inside >= 0.85 * total, "R95 coverage on random geometry: %d / %d", inside, total);
    }
    // 14b. Places along one road: the AP and its mirror across the road explain the levels equally; R95 must reach
    //      whichever is the AP (it was 70 %: half the fits sat on the mirror with R95 around one side only)
    {
        int inside = 0, total = 0;
        std::uniform_real_distribution<double> ux(-150, 150);
        for (int trial = 0; trial < 60; ++trial) {
            QList<Obs> obs;
            for (int i = 0; i < 30; ++i) obs << at(ux(rng), -30, 5, 5.0);
            const Fit f = fitAp(obs, T0);
            if (f.kind != QLatin1String("fix")) continue;
            ++total; if (err(f) <= f.r95) ++inside;
        }
        CHECK(total >= 20 && inside >= 0.85 * total, "one road 30 m off: R95 covers %d / %d", inside, total);
    }
    // 14c. One absurd reading (+60 dBm) among clean ones must not void a region: at the floor scale (τ 0.5) its robust weight
    //      underflowed to 0, ln(s²/0) = ∞ made the reweighted posterior NaN, and a region (centred on that posterior) became "none"
    {
        QList<Obs> obs;
        for (int i = 0; i < 4; ++i) {                     // exact levels (no noise, no jitter): τ sits at its 0.5 floor
            const double x = -30 + i * 20, y = 40, d = std::sqrt(x * x + y * y + 9.0);
            Obs o; o.lat = AP_LAT + mLat(y); o.lon = AP_LON + mLon(x); o.acc = 6; o.t = T0; o.dbm = int(std::lround(modelDbm(P0, N, d))); obs << o;
        }
        Obs wild; wild.lat = AP_LAT + mLat(340); wild.lon = AP_LON; wild.acc = 6; wild.t = T0; wild.dbm = 60; obs << wild;
        const Fit f = fitAp(obs, T0);
        CHECK(f.valid && f.kind == QLatin1String("region") && std::isfinite(f.lat) && std::isfinite(f.lon), "a +60 dBm reading: still a %s (valid %d, R95 %.0f m)", qPrintable(f.kind), int(f.valid), f.r95);
    }
    // 15. The error bars follow the data: many well-fitting places shrink them, a handful do not (fixes ±2 m, so the
    //     shared GPS error, which no number of places averages away, stays small)
    {
        auto ringAt = [&](int places, double db) {
            QList<Obs> obs;
            for (int i = 0; i < places; ++i) { const double ang = i * 2 * M_PI / places, rad = 50 + (i % 3) * 25; for (int r = 0; r < 3; ++r) obs << at(rad * std::cos(ang), rad * std::sin(ang), 2, db); }
            return fitAp(obs, T0);
        };
        const Fit tight = ringAt(36, 1.5), loose = ringAt(36, 6.0), few = ringAt(4, 1.5);
        CHECK(tight.kind == QLatin1String("fix") && tight.r95 < 0.6 * loose.r95 && err(tight) <= tight.r95, "36 places at 1.5 dB: R95 %.1f vs %.1f at 6 dB (error %.1f)", tight.r95, loose.r95, err(tight));
        CHECK(few.valid && few.r95 > 0.8 * few.crlbR95, "4 places at 1.5 dB: σ stays near σ0 (R95 %.1f, CRLB R95 at σ0 %.1f)", few.r95, few.crlbR95);
    }
    // 16. A walk around a building (smoothed fixes ±5 m): the walk keeps its places, and the AP inside is found to the
    //     building with an honest R95 — spatially correlated shadowing (8 m), one wall 8 dB lossier, a shared GPS bias
    {
        int inside = 0, total = 0; std::vector<double> errs; int places = 0;
        for (int trial = 0; trial < 40; ++trial) {
            std::normal_distribution<double> g(0, 1); std::uniform_real_distribution<double> u(0, 1);
            std::vector<double> wx, wy, ph;                                   // random Fourier features: exp(−d/8 m), σ 5 dB
            for (int m = 0; m < 300; ++m) { const double c = std::max(1e-6, std::fabs(g(rng))); wx.push_back(g(rng) / (8 * c)); wy.push_back(g(rng) / (8 * c)); ph.push_back(2 * M_PI * u(rng)); }
            auto field = [&](double x, double y) { double v = 0; for (size_t m = 0; m < wx.size(); ++m) v += std::cos(wx[m] * x + wy[m] * y + ph[m]); return 5.0 * std::sqrt(2.0 / wx.size()) * v; };
            const double bx = 3 * g(rng), by = 3 * g(rng), hw = 20, hh = 14;    // GPS bias; the walk, 8 m from a 24 × 12 m building
            const double ax = (u(rng) - 0.5) * 16, ay = (u(rng) - 0.5) * 8;     // the AP somewhere inside
            QList<Obs> obs;
            for (double s = 0; s < 4 * (hw + hh); s += 3.6) {
                double t = s, x, y;
                if (t < 2 * hw) { x = -hw + t; y = -hh; } else if ((t -= 2 * hw) < 2 * hh) { x = hw; y = -hh + t; } else if ((t -= 2 * hh) < 2 * hw) { x = hw - t; y = hh; } else { t -= 2 * hw; x = -hw; y = hh - t; }
                const double d = std::sqrt((x - ax) * (x - ax) + (y - ay) * (y - ay) + 9.0);
                Obs o; o.lat = AP_LAT + mLat(y + by - ay); o.lon = AP_LON + mLon(x + bx - ax); o.acc = 5; o.t = T0 + qint64(s); o.device = QStringLiteral("phone");
                o.dbm = int(std::lround(modelDbm(P0, N, d) + field(x, y) - (x > hw - 1 ? 8.0 : 0.0) + 4 * g(rng)));
                obs << o;
            }
            const Fit f = fitAp(obs, T0 + 400);
            places += f.vantage;
            if (f.kind != QLatin1String("fix")) continue;
            ++total; if (err(f) <= f.r95) ++inside; errs.push_back(err(f));
        }
        std::sort(errs.begin(), errs.end());
        const double med = errs.empty() ? 1e9 : errs[errs.size() / 2];
        CHECK(places / 40 >= 11, "a 136 m walk keeps %d places (one radius of 15 m kept ~7)", places / 40);
        CHECK(total >= 30 && med < 12 && inside >= 0.85 * total, "walk around a building: median error %.1f m, R95 covers %d / %d", med, inside, total);
    }
    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
