// BeaconFix ranging maths: synthetic scenarios with accuracy tables, plus the cross-language
// test vectors of docs/RANGING.md §11.
//
//   g++ -std=c++17 -O2 tests/ranging_math_test.cpp src/ranging/rangemath.cpp src/ranging/anchors.cpp
//       $(pkg-config --cflags --libs Qt6Core) -o /tmp/ranging_math_test
//   /tmp/ranging_math_test            # scenarios + asserts
//   /tmp/ranging_math_test --vectors  # the §11 vectors as JSON
#include "../src/ranging/anchors.h"
#include "../src/ranging/rangemath.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace RangeMath;

// Deterministic generator shared with RangeMathTest.kt (SplitMix64 + Box–Muller, one draw per call).
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() { uint64_t z = (s += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
    double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
    double normal() { double u1 = uniform(); if (u1 < 1e-300) u1 = 1e-300; const double u2 = uniform(); return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2); }
    double rayleighDb() { double u = uniform(); if (u < 1e-300) u = 1e-300; return 10.0 * std::log10(-std::log(u)); }
    double expo(double mean) { double u = uniform(); if (u < 1e-300) u = 1e-300; return -mean * std::log(u); }
};

static int failures = 0;
static void check(bool ok, const char *what) { if (!ok) { ++failures; std::printf("  FAIL: %s\n", what); } }

// ── a simulated device pair ──────────────────────────────────────────────────
struct LinkSim {
    double p0True, nTrue, shadow;            // the real link
    double frozen[3];                        // per-channel frozen fading (static devices)
};
struct Scenario {
    const char *name;
    double d;                                // true distance
    double seconds;
    bool rtt; double rttSingle; double rttOffsetTrue; double nlosP; double rttUntilS;
    bool ble;
    double p0DownPrior, p0UpPrior;           // what the filter starts from (from TX power)
    bool calibrate; double calDist, calSeconds;
    uint64_t seed;
    double driftDb2PerS = 0;                 // the links' true offsets random-walk at this rate (dB²/s) after calibrating
};
struct Result { double est, low, high; QString cls; double rttOffset; };

static Result runScenario(const Scenario &sc, bool print)
{
    Rng rng(sc.seed);
    LinkSim down{-50.0, 2.0, 0, {0, 0, 0}}, up{-53.0, 2.0, 0, {0, 0, 0}};
    down.shadow = kShadowBle * rng.normal() * 0.5;   // a static link: half the log-normal spread realised
    up.shadow = kShadowBle * rng.normal() * 0.5;
    for (int c = 0; c < 3; ++c) { down.frozen[c] = rng.rayleighDb(); up.frozen[c] = rng.rayleighDb(); }
    RangeFilter f(2);
    Rls2 rlsDown(sc.p0DownPrior, 2.0), rlsUp(sc.p0UpPrior, 2.0);
    double rttOffset = 0;                    // learned by calibration
    std::vector<double> winDown, winUp, calRtt, calDown, calUp;
    double winStart = 0; bool firstFlush = true;
    auto sampleLevel = [&](LinkSim &L, int ch, double dist) {
        return L.p0True - 10.0 * L.nTrue * std::log10(dist) + L.shadow + L.frozen[ch] + 1.5 * rng.normal();
    };
    const double calEnd = sc.calibrate ? sc.calSeconds : 0;
    for (int t = 1; t <= int(sc.seconds + calEnd); ++t) {
        const bool calPhase = t <= calEnd;
        const double dNow = calPhase ? sc.calDist : sc.d;       // the devices sit at calDist while calibrating
        f.predict(1.0, false);
        if (!calPhase && sc.driftDb2PerS > 0) {                 // a still link is not frozen for hours: people, doors, temperature
            down.shadow += std::sqrt(sc.driftDb2PerS) * rng.normal();
            up.shadow += std::sqrt(sc.driftDb2PerS) * rng.normal();
        }
        if (sc.rtt && (t <= sc.rttUntilS + calEnd)) {
            double m = dNow + sc.rttOffsetTrue + sc.rttSingle / std::sqrt(8.0) * rng.normal();
            if (rng.uniform() < sc.nlosP) m += rng.expo(2.0);
            if (calPhase) calRtt.push_back(m);
            else f.updateRtt(m, sc.rttSingle / std::sqrt(8.0), rttOffset);
        }
        if (sc.ble) {
            for (int s = 0; s < 5; ++s) {
                const int ch = (t * 5 + s) % 3;
                const double a = sampleLevel(down, ch, dNow), b = sampleLevel(up, ch, dNow);
                if (calPhase) { calDown.push_back(a); calUp.push_back(b); }
                else { winDown.push_back(a); winUp.push_back(b); }
            }
            if (!calPhase) {
                if (winStart == 0) winStart = t;
                const double span = t - winStart + 1;
                if (span >= (firstFlush ? 5 : 30)) {
                    const Level la = levelFromSamples(winDown, 3, span, false), lb = levelFromSamples(winUp, 3, span, false);
                    f.updateRssi(0, la.dbm, la.sigma, rlsDown.p0, rlsDown.n);
                    f.updateRssi(1, lb.dbm, lb.sigma, rlsUp.p0, rlsUp.n);
                    winDown.clear(); winUp.clear(); winStart = 0; firstFlush = false;
                }
            }
        }
        if (sc.calibrate && t == int(calEnd)) {                     // one tap: "these are calDist apart"
            // Putting the phone down again moves it by more than λ/2 (6 cm): the multipath fade is new,
            // only the shadowing and the P0 carry over from the calibration.
            for (int c = 0; c < 3; ++c) { down.frozen[c] = rng.rayleighDb(); up.frozen[c] = rng.rayleighDb(); }
            if (!calRtt.empty()) {
                rttOffset = median(calRtt) - sc.calDist;
                // the median of N bursts: σ ≈ 1.2533·σ_burst/√N, plus a 5 cm floor for the tape measure
                const double sb = std::max(kRttFloor, sc.rttSingle / std::sqrt(8.0));
                const double sc2 = 1.2533 * sb / std::sqrt(double(calRtt.size()));
                f.resetRttOffset(0, sc2 * sc2 + 0.05 * 0.05);
            }
            if (!calDown.empty()) {
                const Level la = levelFromSamples(calDown, 3, sc.calSeconds, false), lb = levelFromSamples(calUp, 3, sc.calSeconds, false);
                rlsDown.update(std::log10(sc.calDist), la.dbm, la.sigma * la.sigma);
                rlsUp.update(std::log10(sc.calDist), lb.dbm, lb.sigma * lb.sigma);
                // What the calibration pins: P0 and shadowing. What it cannot: the fade at the next spot.
                // A still calibration cannot know its own spot's fade better than σ_ff either.
                f.resetOffset(0, 0, std::max(la.sigma * la.sigma, kFrozenFadeVar) + kFrozenFadeVar);
                f.resetOffset(1, 0, std::max(lb.sigma * lb.sigma, kFrozenFadeVar) + kFrozenFadeVar);
            }
        }
    }
    RelInput in; in.haveRange = true; in.u = f.u(); in.puu = f.puu();
    const RelOutput o = relativePosterior(in);
    Result r{o.distanceM, o.lowM, o.highM, o.cls, rttOffset};
    if (print)
        std::printf("  %-44s true %6.2f m  est %6.2f m  [%5.2f, %6.2f]  err %+6.2f m (%+5.0f%%)  %-8s\n", sc.name, sc.d, r.est, r.low, r.high,
                    r.est - sc.d, 100.0 * (r.est - sc.d) / sc.d, qPrintable(r.cls));
    return r;
}

static void scenarios()
{
    std::printf("Ranging scenarios (deterministic, SplitMix64)\n");
    //                    name                                            d     T    rtt  σ1   c     nlos until ble  p0↓  p0↑  cal  calD  calT seed
    const Scenario S[] = {
        {"0.6 m, BLE only, uncalibrated",                                 0.6,  120, false, 0,   0,    0,   0,  true, -59, -48, false, 0,   0,  11},
        {"0.6 m, BLE only, after one-tap calibration at 0.61 m",          0.6,  120, false, 0,   0,    0,   0,  true, -59, -48, true, 0.61, 20, 12},
        {"0.6 m, RTT 80 MHz (+0.55 m pair offset) + BLE, calibrated",     0.6,  120, true,  0.7, 0.55, 0,   1e9, true, -59, -48, true, 0.61, 20, 13},
        {"3 m, RTT 20 MHz + BLE, calibrated at 0.61 m",                   3.0,  120, true,  2.5, 0.55, 0,   1e9, true, -59, -48, true, 0.61, 20, 14},
        {"12 m, RTT 20 MHz, 30% NLOS bursts + BLE, calibrated",           12.0, 120, true,  2.5, 0.55, 0.3, 1e9, true, -59, -48, true, 0.61, 20, 15},
        {"4 m, BLE prior 8 dB off, RTT for 60 s then gone",               4.0,  180, true,  0.7, 0.0,  0,   60, true, -51, -40, false, 0,   0,  16},
        {"0.6 m for 2 h: RTT 10 min, then BLE only, links drift",         0.6, 7200, true,  0.7, 0.55, 0,  600, true, -59, -48, true, 0.61, 20, 17, 2.5e-3},
    };
    Result R[7];
    for (int i = 0; i < 7; ++i) R[i] = runScenario(S[i], true);
    check(R[1].high < 2.0, "calibrated BLE at 0.6 m should classify adjacent");
    check(std::fabs(R[2].est - 0.6) < 0.25, "RTT+BLE at 0.6 m within 25 cm");
    check(std::fabs(R[3].est - 3.0) < 1.0, "3 m within 1 m");
    check(std::fabs(R[4].est - 12.0) < 3.0, "12 m with NLOS within 3 m");
    check(std::fabs(R[5].est - 4.0) / 4.0 < 0.3, "offset learned away: 4 m within 30 % after RTT is gone");
    check(R[6].low <= 0.6 && 0.6 <= R[6].high, "2 h with drifting links: the interval still holds the truth");

    // Honest error bars: how often does the [low, high] (16–84 %) interval contain the truth?
    {
        const Scenario base[] = {
            {"cov: 0.6 m BLE calibrated", 0.6, 120, false, 0, 0, 0, 0, true, -59, -48, true, 0.61, 20, 0},
            {"cov: 0.6 m RTT 80 MHz + BLE", 0.6, 120, true, 0.7, 0.55, 0, 1e9, true, -59, -48, true, 0.61, 20, 0},
            {"cov: 3 m RTT 20 MHz + BLE", 3.0, 120, true, 2.5, 0.55, 0, 1e9, true, -59, -48, true, 0.61, 20, 0},
            {"cov: 12 m RTT 20 MHz NLOS + BLE", 12.0, 120, true, 2.5, 0.55, 0.3, 1e9, true, -59, -48, true, 0.61, 20, 0},
            // A long run: the filter must not trust its link offsets more than the drifting world allows
            {"cov: 0.6 m 2 h, RTT 10 min then BLE, drift", 0.6, 7200, true, 0.7, 0.55, 0, 600, true, -59, -48, true, 0.61, 20, 0, 2.5e-3}};
        for (const Scenario &b : base) {
            int inside = 0; double se = 0; const int T = 200;
            for (int k = 0; k < T; ++k) {
                Scenario sc = b; sc.seed = 1000 + k;
                const Result r = runScenario(sc, false);
                if (r.low <= sc.d && sc.d <= r.high) ++inside;
                se += (r.est - sc.d) * (r.est - sc.d);
            }
            std::printf("  %-44s RMS error %.3f m, 16–84%% interval covers the truth in %3.0f%% of %d runs (ideal 68%%)\n",
                        b.name, std::sqrt(se / T), 100.0 * inside / T, T);
            check(inside >= T * 45 / 100, "interval coverage at least 45 % (not badly over-confident)");
        }
        double se = 0, sig = 0; const int T = 200;
        for (int k = 0; k < T; ++k) {
            Rng h(5000 + k);
            std::vector<Anchors::RangeToAnchor> rr;
            const double aE[3] = {-4, 6, 1}, aN[3] = {2, 3, -7};
            for (int i = 0; i < 3; ++i) {
                double la, lo; Anchors::fromEnu(40.0030, -75.0680, std::nan(""), Anchors::Enu{aE[i], aN[i], 0}, &la, &lo, nullptr);
                Anchors::RangeToAnchor a; a.lat = la; a.lon = lo; a.anchorAccM = 0.2; a.rangeM = std::hypot(aE[i], aN[i]) + 0.5 * h.normal(); a.sigmaM = 0.5;
                rr.push_back(a);
            }
            double qLat, qLon; Anchors::fromEnu(40.0030, -75.0680, std::nan(""), Anchors::Enu{30, 25, 0}, &qLat, &qLon, nullptr);
            const Anchors::Trilat tl = Anchors::trilaterate(rr, qLat, qLon, 40.0);
            const double e = Anchors::distanceM(tl.lat, tl.lon, 40.0030, -75.0680);
            se += e * e; sig += tl.sigmaM * tl.sigmaM;
        }
        std::printf("  Phone from RTT rings to 3 anchors: RMS position error %.2f m vs reported RMS σ·√2 %.2f m over %d runs\n",
                    std::sqrt(se / T), std::sqrt(2.0 * sig / T), T);
    }

    // Heavy fading: which level estimator is unbiased?
    Rng rng(99);
    std::vector<double> s;
    for (int i = 0; i < 4000; ++i) s.push_back(-60.0 + rng.rayleighDb());
    double meanDb = 0; for (double v : s) meanDb += v; meanDb /= s.size();
    const Level L = levelFromSamples(s, 3, 3600, true);
    std::printf("  Rayleigh fading, 4000 samples of a −60.00 dBm mean: mean of dB %.2f (bias %+.2f), median %.2f, Q0.632 %.2f, winsorised linear mean %.2f (bias %+.2f)\n",
                meanDb, meanDb + 60.0, median(s), quantile7(s, 1 - std::exp(-1.0)), L.dbm, L.dbm + 60.0);
    check(std::fabs(L.dbm + 60.0) < 0.3, "winsorised linear mean unbiased under Rayleigh");
    check(std::fabs(meanDb + 60.0 + 2.51) < 0.3, "mean of dB is 2.5 dB low under Rayleigh");

    // Differential RSSI: 12 APs outside the RV (4–40 m), true offset 1.5 m at 70°, gain offset −4 dB.
    // Both devices look at each AP through the same shell, so shadowing is correlated over ~10 m
    // (the solver still assumes the pessimistic indoor 3 m, i.e. it over-states its own noise).
    Rng g(7);
    const double dTrue = 1.5, brg = 70.0 * M_PI / 180.0, dE = dTrue * std::sin(brg), dN = dTrue * std::cos(brg), gain = -4.0;
    const double rho = std::exp(-dTrue / 10.0);
    std::vector<GeoPair> geo; std::vector<DiffPair> fpp;
    for (int i = 0; i < 12; ++i) {
        const double b = g.uniform() * 2 * M_PI, r = 4.0 + 36.0 * g.uniform();
        const double ae = r * std::sin(b), an = r * std::cos(b);
        const double sA = kShadowWifi * g.normal(), sB = rho * sA + std::sqrt(1 - rho * rho) * kShadowWifi * g.normal();
        const double dA = std::sqrt(ae * ae + an * an), dB = std::sqrt((dE - ae) * (dE - ae) + (dN - an) * (dN - an));
        double la = 0, lb = 0;
        for (int k = 0; k < 20; ++k) { la += -40 - 24 * std::log10(dA) + sA + g.rayleighDb(); lb += -40 - 24 * std::log10(dB) + sB + gain + g.rayleighDb(); }
        la /= 20; lb /= 20;
        GeoPair p; p.group = QStringLiteral("ap%1").arg(i); p.apE = ae + 3 * g.normal(); p.apN = an + 3 * g.normal(); p.pathloss = 2.4; p.sigmaPos = 3;
        p.delta = lb - la; p.sigmaDelta = std::sqrt(2.0) * kRayleighDbStd / std::sqrt(20.0);
        geo.push_back(p);
        fpp.push_back(DiffPair{p.group, la, kRayleighDbStd / std::sqrt(20.0), lb, kRayleighDbStd / std::sqrt(20.0)});
    }
    const GeoSolve gs = solveDifferential(geo);
    const Fingerprint fp = fingerprintDistance(fpp);
    std::printf("  Differential RSSI, 12 APs, true δ = %.2f m at 70°: geometric δ = (%.2f, %.2f) m → %.2f m at %.0f° ± %.2f m, gain %.2f dB (true −4); fingerprint %.2f m [%.2f, %.2f]\n",
                dTrue, gs.dE, gs.dN, std::hypot(gs.dE, gs.dN), std::fmod(std::atan2(gs.dE, gs.dN) * 180 / M_PI + 360, 360),
                std::sqrt(0.5 * (gs.cov[0][0] + gs.cov[1][1])), gs.gainDb, fp.deltaM, fp.lowM, fp.highM);
    check(gs.ok && std::hypot(gs.dE - dE, gs.dN - dN) < 3.0, "geometric differential solve within 3 m");
    check(std::fabs(gs.gainDb - gain) < 2.0, "gain offset recovered within 2 dB");

    // Phone position from RTT rings to three anchors (±0.5 m ranges, anchors ±0.2 m), prior 40 m off
    std::vector<Anchors::RangeToAnchor> rr;
    const double pLat = 40.0030, pLon = -75.0680;
    const double aE[3] = {-4, 6, 1}, aN[3] = {2, 3, -7};
    Rng h(21);
    for (int i = 0; i < 3; ++i) {
        double la, lo; Anchors::fromEnu(pLat, pLon, std::nan(""), Anchors::Enu{aE[i], aN[i], 0}, &la, &lo, nullptr);
        Anchors::RangeToAnchor a; a.lat = la; a.lon = lo; a.anchorAccM = 0.2; a.rangeM = std::hypot(aE[i], aN[i]) + 0.5 * h.normal(); a.sigmaM = 0.5;
        rr.push_back(a);
    }
    double qLat, qLon; Anchors::fromEnu(pLat, pLon, std::nan(""), Anchors::Enu{30, 25, 0}, &qLat, &qLon, nullptr);
    const Anchors::Trilat tl = Anchors::trilaterate(rr, qLat, qLon, 40.0);
    const double err = Anchors::distanceM(tl.lat, tl.lon, pLat, pLon);
    std::printf("  Phone from RTT rings to 3 anchors, prior 39 m off: error %.2f m (reported σ %.2f m, %d iterations)\n", err, tl.sigmaM, tl.iterations);
    check(tl.valid && err < 1.5, "trilateration within 1.5 m");
}

// ── vectors ──────────────────────────────────────────────────────────────────
static QJsonObject vectors()
{
    QJsonObject v;
    v["fspl_1m_2437"] = fsplDb(1, 2437);
    v["fspl_0.61m_5180"] = fsplDb(0.61, 5180);
    v["prior_ble_-7"] = priorP0Ble(-7);
    v["prior_ble_127"] = priorP0Ble(127);
    v["prior_wifi_5180"] = priorP0Wifi(5180);
    v["model_level"] = modelLevel(-50, 2, 0.61);
    v["model_distance"] = modelDistance(-50, 2, -45.7);
    const std::vector<double> q = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5};
    v["q7_0.95"] = quantile7(q, 0.95);
    v["q7_0.632"] = quantile7(q, 0.632);
    v["median"] = median(q);
    v["chi2_6_+"] = chi2Quantile(6, kZ84);
    v["chi2_6_-"] = chi2Quantile(6, -kZ84);
    const Level L1 = levelFromSamples({-51, -49, -55, -62, -50, -48, -53, -47, -58, -50, -52, -49}, 3, 20, false);
    const Level L2 = levelFromSamples({-60, -61, -59}, 1, 2, true);
    v["level1"] = QJsonArray{L1.dbm, L1.sigma, L1.nEff};
    v["level2"] = QJsonArray{L2.dbm, L2.sigma, L2.nEff};

    RangeFilter f(2);
    f.predict(1, false);
    const double z1 = f.updateRtt(1.2, 0.6, 0.0);
    const double z2 = f.updateRssi(0, -52, 2.0, -48, 2.0);
    const double z3 = f.updateRssi(1, -55, 2.5, -50, 2.0);
    f.predict(5, false);
    const double z4 = f.updateRtt(0.9, 0.5, 0.3);
    v["filter"] = QJsonArray{f.x[0], f.x[1], f.x[2], f.P[0][0], f.P[0][1], f.P[1][1], f.P[1][2], f.P[2][2],
                             f.distanceM(), f.sigmaM(), f.lowM(), f.highM(), z1, z2, z3, z4};
    RangeFilter f2(2);
    f2.updateRtt(25.0, 0.4, 0.0);                               // far outlier vs the 10 m prior, NLOS side
    f2.updateRtt(4.0, 0.4, 0.0);
    v["filter_rtt_robust"] = QJsonArray{f2.x[0], f2.P[0][0], f2.distanceM()};
    RangeFilter f3(2);                                          // the process model over an hour still, then moving
    f3.updateRssi(0, -60, 2.0, -48, 2.0);
    f3.predict(3600, false);
    const double z5 = f3.updateRssi(0, -58, 2.0, -48, 2.0);
    f3.predict(2, true);
    v["filter_drift"] = QJsonArray{f3.x[0], f3.x[1], f3.P[0][0], f3.P[0][1], f3.P[1][1], f3.P[2][2], f3.P[3][3], z5};

    Rls2 r(-59, 2.0);
    r.update(std::log10(0.61), -44.3, 16);
    r.update(std::log10(2.0), -54.1, 16);
    r.update(std::log10(5.0), -62.0, 16);
    v["rls"] = QJsonArray{r.p0, r.n, r.S[0][0], r.S[0][1], r.S[1][1]};

    const PathLossFit pf = fitPathLoss({{1, -40.5}, {2, -47.0}, {4, -53.2}, {8, -60.1}, {16, -66.8}}, -40, 2.4);
    v["fit"] = QJsonArray{pf.p0, pf.n, pf.varP0, pf.varN, pf.covP0N, pf.rmsDb};

    const std::vector<DiffPair> dp = {
        {QStringLiteral("a"), -50, 2.0, -53.1, 2.2}, {QStringLiteral("b"), -61, 2.5, -64.9, 2.5}, {QStringLiteral("c"), -70, 3.0, -72.2, 3.0},
        {QStringLiteral("d"), -45, 1.8, -49.8, 1.9}, {QStringLiteral("e"), -66, 2.8, -68.5, 2.9}, {QStringLiteral("f"), -58, 2.1, -62.7, 2.0},
        {QStringLiteral("a"), -51, 2.0, -53.9, 2.2}};
    const Fingerprint fp = fingerprintDistance(dp);
    v["fingerprint"] = QJsonArray{fp.gainDb, fp.d2, fp.noise2, fp.excess, fp.deltaM, fp.lowM, fp.highM, double(fp.groups),
                                  fingerprintLogLik(fp, 1.0), fingerprintLogLik(fp, 10.0)};
    const std::vector<DiffPair> dp2 = {
        {QStringLiteral("a"), -50, 2.0, -41.0, 2.0}, {QStringLiteral("b"), -61, 2.0, -70.0, 2.0}, {QStringLiteral("c"), -70, 2.0, -66.0, 2.0},
        {QStringLiteral("d"), -45, 2.0, -52.0, 2.0}, {QStringLiteral("e"), -66, 2.0, -59.0, 2.0}};
    const Fingerprint fq = fingerprintDistance(dp2);
    v["fingerprint_far"] = QJsonArray{fq.gainDb, fq.d2, fq.excess, fq.deltaM, fq.lowM, fq.highM};

    std::vector<GeoPair> gp;
    const double ge[5] = {8, -12, 3, -6, 20}, gn[5] = {5, 7, -15, -9, -2};
    for (int i = 0; i < 5; ++i) {
        GeoPair p; p.group = QStringLiteral("g%1").arg(i); p.apE = ge[i]; p.apN = gn[i]; p.pathloss = 2.4; p.sigmaPos = 2; p.sigmaDelta = 1.5;
        const double dA = std::hypot(ge[i], gn[i]), dB = std::hypot(1.2 - ge[i], -0.8 - gn[i]);
        p.delta = -24.0 * std::log10(dB / dA) - 3.0;
        gp.push_back(p);
    }
    const GeoSolve gs = solveDifferential(gp);
    v["geo"] = QJsonArray{gs.dE, gs.dN, gs.gainDb, double(gs.iterations), gs.spreadDeg, gs.cov[0][0], gs.cov[1][1], gs.cov[2][2], gs.chi2};

    RelInput a; a.haveRange = true; a.u = std::log10(0.61); a.puu = 0.01;
    const RelOutput oa = relativePosterior(a);
    v["rel_range"] = QJsonArray{oa.distanceM, oa.lowM, oa.highM, oa.sigmaM, oa.haveBearing ? 1.0 : 0.0, oa.resultantLength, oa.cls};
    RelInput b; b.haveRange = true; b.u = std::log10(50.0); b.puu = 0.04; b.fix = Gauss2{true, 30, 40, 400, 0, 400};
    const RelOutput ob = relativePosterior(b);
    v["rel_range_fix"] = QJsonArray{ob.distanceM, ob.lowM, ob.highM, ob.bearingDeg, ob.bearingSigmaDeg, ob.resultantLength, ob.cls};
    RelInput c; c.haveFp = true; c.fp = fp;
    const RelOutput oc = relativePosterior(c);
    v["rel_fp"] = QJsonArray{oc.distanceM, oc.lowM, oc.highM, oc.cls};
    v["classify"] = QJsonArray{classify(false, 0, 1), classify(true, 0.4, 1.9), classify(true, 1, 5), classify(true, 31, 80),
                               classify(true, 8, 25), classify(true, 10, 45)};

    const QByteArray tag = bleTag(QStringLiteral("6htgz65xb7yfs53dmhdanfmk7c"), 1790478720);
    const int flags = bleFlags(true, true, KindAndroid, false);
    const QByteArray sd = bleServiceData(tag, -7, flags);
    const BleAdvert ad = parseServiceData(sd);
    v["ble"] = QJsonArray{QString::fromLatin1(tag.toHex()), flags, QString::fromLatin1(sd.toHex()), ad.txPower, ad.kind, ad.rtt, ad.api, ad.version};
    v["ble_window_neg"] = QString::fromLatin1(bleTag(QStringLiteral("x"), -1).toHex());

    const Anchors::Enu e = Anchors::enu(40.002937, -75.06806, 120.0, 40.002955, -75.068040, 121.2);
    v["enu"] = QJsonArray{e.e, e.n, e.u};
    double re, rn; Anchors::rotate(3, 4, 90, &re, &rn);
    v["rotate"] = QJsonArray{re, rn};
    const Anchors::Reprojected p1 = Anchors::reproject(Anchors::Enu{3.2, -1.5, 0.4}, true, 212, 40.0030, -75.0680, true, 120, true, 302);
    const Anchors::Reprojected p2 = Anchors::reproject(Anchors::Enu{3.2, -1.5, 0.4}, true, 212, 40.0030, -75.0680, true, 120, false, 0);
    v["reproject_known"] = QJsonArray{p1.lat, p1.lon, p1.alt, p1.headingAssumed};
    v["reproject_unknown"] = QJsonArray{p2.lat, p2.lon, p2.alt, p2.headingAssumed};
    std::vector<Anchors::RangeToAnchor> rr;
    const double ra[3][3] = {{40.00290, -75.06810, 4.1}, {40.00298, -75.06800, 6.9}, {40.00284, -75.06799, 7.7}};
    for (auto &x : ra) { Anchors::RangeToAnchor t; t.lat = x[0]; t.lon = x[1]; t.rangeM = x[2]; t.sigmaM = 0.5; t.anchorAccM = 0.2; rr.push_back(t); }
    const Anchors::Trilat tl = Anchors::trilaterate(rr, 40.0031, -75.0679, 40.0);
    v["trilat"] = QJsonArray{tl.lat, tl.lon, tl.sigmaM, tl.rmsM, double(tl.iterations)};
    Anchors::Anchor ga, ta;
    ga.hasRvOffset = true; ga.rvOffset = {-2.0, 5.5, 1.8}; ga.hasHeading = true; ga.headingDeg = 212; ga.accM = 0.3;
    ta.hasRvOffset = true; ta.rvOffset = {0, 0, 0}; ta.accM = 0.5;
    const Anchors::RvGnss g1 = Anchors::rvGnssPosition(40.00300, -75.06810, 1.8, &ga, &ta, true, 302);
    const Anchors::RvGnss g2 = Anchors::rvGnssPosition(40.00300, -75.06810, 1.8, nullptr, nullptr, false, 0);
    v["rv_gnss"] = QJsonArray{g1.lat, g1.lon, g1.sigmaM, g1.valid, g2.lat, g2.lon, g2.sigmaM};
    const PathLossFit af = fitPathLoss({{2.1, -43.8}, {5.4, -55.2}, {9.8, -61.9}, {15.2, -66.0}}, priorP0Wifi(2437), kNWifi);
    v["anchor_calibration"] = QJsonArray{af.p0, af.n, af.rmsDb};
    return v;
}

// Expected values, asserted here and in RangeMathTest.kt (filled from --vectors output).
static const char *kExpectedVectors = R"JSON({
"anchor_calibration":[-37.295802015063686,2.4287654109893175,0.7143219926188046],
"ble":["898131b88e0457b0",7,"898131b88e0457b0f907",-7,1,true,true,0],
"ble_window_neg":"cf6fff450eef869b",
"chi2_6_+":9.229109314951645,
"chi2_6_-":2.7560888499578042,
"classify":["unknown","adjacent","room","far","near","unknown"],
"enu":[1.7054479876969342,2.0037599996868494,1.2000000000000028],
"filter":[0.07689093392987602,-2.346415535374766,-3.216278090901163,0.031994050882637924,0.6099683097169435,15.463545696660228,11.340825811962223,16.87439220342253,1.1936882912146383,0.49163323972359524,0.7907189095276757,1.8020205656066342,-0.051605713228050784,-0.12249847699217876,-0.1944921230282392,-0.8251850909005902],
"filter_drift":[0.6298835297924379,2.2340875206714412,0.18792407716665965,3.578842109814038,82.34179352734864,91.34179352734864,0.253602,0.4333269413222595],
"filter_rtt_robust":[1.3733860610084303,0.00012094918043716869,23.62577485657404],
"fingerprint":[-3.450000000000003,2.6597122993644633,13.321666666666667,0,0,0,0,6,-10.255926479189677,-13.444994423861454],
"fingerprint_far":[4,54.952733457943324,44.702733457943324,4.666438293955714,1.9159647199595962,17.307883242697173],
"fit":[-39.853867193347156,2.271109132634109,6.269943059867058,0.09800952168791119,0.5619772550236338,0.4124708581136841],
"fspl_0.61m_5180":42.44319189512001,
"fspl_1m_2437":40.18711058369449,
"geo":[1.1999999997307187,-0.7999999997493872,-2.9999999999804126,4,242.26182037161277,18.70447781205538,20.18114356805259,5.554126396376508,4.885697637654346e-21],
"level1":[-50.42527605252986,1.9422239675774466,5],
"level2":[-59.92358370267848,2.507400360344115,3],
"median":4,
"model_distance":0.6095368972401694,
"model_level":-45.70659670021534,
"prior_ble_-7":-48,
"prior_ble_127":-59,
"prior_wifi_5180":-46.549484611210175,
"q7_0.632":5,
"q7_0.95":7.5,
"rel_fp":[7.186558590674563,1.7593465916496707,17.065026528572087,"near"],
"rel_range":[0.61,0.48511734778731724,0.7670309084950191,0.14095678035385092,0,5.4745342114606975e-17,"adjacent"],
"rel_range_fix":[45.09074549867884,31.860582597534947,60.591585411392664,36.86989764584402,26.80350483842413,0.8963513400468486,"far"],
"reproject_known":[40.0029712540424,-75.06801759070221,120.4,false],
"reproject_unknown":[40.002986525332375,-75.0679624731686,120.4,true],
"rls":[-49.265931839945814,1.9142909075068142,5.806407120458949,0.3608712634841288,0.14915744844330686],
"rotate":[4,-2.9999999999999996],
"rv_gnss":[40.0029820337765,-75.06816449924145,1.8920887928424501,true,40.003,-75.0681,5.314132102234569],
"trilat":[40.00291088304175,-75.06803931836865,0.4455765733186116,1.3221886849534632,14]
})JSON";

static bool near(double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)); }
static void compare(const QJsonObject &got, const QJsonObject &exp)
{
    for (auto it = exp.begin(); it != exp.end(); ++it) {
        const QJsonValue g = got.value(it.key()), e = it.value();
        bool ok = true;
        if (e.isArray()) {
            const QJsonArray ga = g.toArray(), ea = e.toArray();
            ok = ga.size() == ea.size();
            for (int i = 0; ok && i < ea.size(); ++i) {
                if (ea[i].isDouble()) ok = near(ga[i].toDouble(), ea[i].toDouble());
                else ok = ga[i] == ea[i];
            }
        } else if (e.isDouble()) ok = near(g.toDouble(), e.toDouble());
        else ok = g == e;
        if (!ok) { ++failures; std::printf("  FAIL vector %s: got %s\n", qPrintable(it.key()), QJsonDocument(QJsonObject{{"v", g}}).toJson(QJsonDocument::Compact).constData()); }
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QJsonObject v = vectors();
    if (argc > 1 && std::strcmp(argv[1], "--vectors") == 0) {
        std::printf("%s\n", QJsonDocument(v).toJson(QJsonDocument::Indented).constData());
        return 0;
    }
    scenarios();
    const QJsonObject exp = QJsonDocument::fromJson(QByteArray(kExpectedVectors)).object();
    if (exp.isEmpty()) std::printf("  (no expected vectors compiled in yet)\n");
    else compare(v, exp);
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
