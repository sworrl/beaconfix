// Synthetic-geometry tests for src/estimator.h. Build (no CMake target yet):
//   g++ -std=c++17 -O2 -Wall -Wextra -fPIC $(pkg-config --cflags Qt6Core) tests/estimator_test.cpp -o build/estimator_test $(pkg-config --libs Qt6Core)
#include "../src/estimator.h"
#include <QString>
#include <cstdio>
#include <random>

using namespace Estimator;
static int fails = 0;
#define CHECK(cond, fmt, ...) do { if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } else { std::printf("ok   " fmt "\n", ##__VA_ARGS__); } } while (0)

static const double AP_LAT = 40.0030, AP_LON = -75.0680, P0 = -38.0, N = 2.6;
static std::mt19937 rng(42);

static Obs sample(double lat, double lon, double acc, double dbNoise, qint64 t = 1790000000, double jitter = 0.6)
{
    Obs o; o.lat = lat; o.lon = lon; o.acc = acc; o.t = t;
    const double d = std::max(1.0, distanceM(lat, lon, AP_LAT, AP_LON));
    std::normal_distribution<double> nd(0.0, dbNoise);
    o.dbm = int(std::lround(modelDbm(P0, N, d) + nd(rng)));
    // the observer's own fix is noisy too
    std::normal_distribution<double> pn(0.0, acc * jitter);
    o.lat += pn(rng) / 111320.0; o.lon += pn(rng) / (111320.0 * std::cos(lat * M_PI / 180));
    return o;
}

int main()
{
    // 1. 40 noisy observations on a ring of radius 60–180 m around the AP → error < 15 m
    {
        QList<Obs> obs;
        for (int i = 0; i < 40; ++i) {
            const double ang = i * 2 * M_PI / 40, rad = 60 + (i % 5) * 30;
            obs << sample(AP_LAT + rad * std::sin(ang) / 111320.0, AP_LON + rad * std::cos(ang) / (111320.0 * std::cos(AP_LAT * M_PI / 180)), 8, 3.0);
        }
        const Fit f = fitAp(obs, 1790000000);
        const double err = distanceM(f.lat, f.lon, AP_LAT, AP_LON);
        CHECK(f.valid && err < 15.0, "40 obs around the AP: error %.1f m (acc ±%.1f, rms %.1f dB, p0 %.1f, n %.2f, vantage %d, %s)", err, f.acc, f.rms, f.p0, f.pathloss, f.vantage, qPrintable(f.quality));
        CHECK(std::fabs(f.p0 - P0) < 6 && std::fabs(f.pathloss - N) < 0.5, "model recovered: p0 %.1f (true %.1f), n %.2f (true %.2f)", f.p0, P0, f.pathloss, N);
        CHECK(f.acc >= err * 0.5, "reported accuracy is not overconfident: acc %.1f vs error %.1f", f.acc, err);
    }
    // 2. Degenerate geometry: 12 samples from one spot → no position claimed
    {
        QList<Obs> obs;
        for (int i = 0; i < 12; ++i) obs << sample(AP_LAT + 0.0008, AP_LON + 0.0003, 20, 3.0, 1790000000, 0.3);
        const Fit f = fitAp(obs, 1790000000);
        CHECK(!f.valid, "one vantage point: no position (vantage %d, quality %s)", f.vantage, qPrintable(f.quality));
    }
    // 2b. Three spots, all inside the fix error → no position
    {
        QList<Obs> obs;
        for (int i = 0; i < 9; ++i) obs << sample(AP_LAT + 0.0008 + (i % 3) * 0.0003, AP_LON, 120, 3.0, 1790000000, 0.15);   // a Wi-Fi fix: same APs → same answer, little jitter
        const Fit f = fitAp(obs, 1790000000);
        CHECK(!f.valid, "spread inside 1.5× fix error: no position (vantage %d)", f.vantage);
    }
    // 3. Outliers: 3 samples at -40 dBm from 300 m away must not move the fit by more than a few metres
    {
        QList<Obs> obs;
        for (int i = 0; i < 40; ++i) {
            const double ang = i * 2 * M_PI / 40, rad = 50 + (i % 4) * 40;
            obs << sample(AP_LAT + rad * std::sin(ang) / 111320.0, AP_LON + rad * std::cos(ang) / (111320.0 * std::cos(AP_LAT * M_PI / 180)), 8, 3.0);
        }
        const Fit clean = fitAp(obs, 1790000000);
        const double errClean = distanceM(clean.lat, clean.lon, AP_LAT, AP_LON);
        for (int i = 0; i < 3; ++i) { Obs o; o.lat = AP_LAT + 0.0027; o.lon = AP_LON + 0.0005 * i; o.acc = 8; o.dbm = -40; o.t = 1790000000; obs << o; }
        const Fit f = fitAp(obs, 1790000000);
        const double err = distanceM(f.lat, f.lon, AP_LAT, AP_LON);
        CHECK(clean.valid && errClean < 25.0, "baseline without outliers: error %.1f m (acc ±%.1f)", errClean, clean.acc);
        CHECK(f.valid && f.rejected >= 2 && err < errClean + 10.0 && err < 30.0, "outliers rejected: error %.1f m (was %.1f), rejected %d, rms %.1f dB", err, errClean, f.rejected, f.rms);
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
        const double err = distanceM(s.lat, s.lon, me_lat, me_lon);
        CHECK(s.valid && err < 30.0, "self-locate from 5 APs: error %.1f m (acc ±%.1f, rms %.1f m, used %d)", err, s.acc, s.rms, s.used);
        CHECK(s.acc >= err * 0.5, "self-locate accuracy honest: acc %.1f vs err %.1f", s.acc, err);
    }
    // 5. Incremental update nudges towards the truth and never explodes
    {
        QList<Obs> obs;
        for (int i = 0; i < 10; ++i) {
            const double ang = i * 2 * M_PI / 10, rad = 90;
            obs << sample(AP_LAT + rad * std::sin(ang) / 111320.0, AP_LON + rad * std::cos(ang) / (111320.0 * std::cos(AP_LAT * M_PI / 180)), 10, 3.0);
        }
        Fit f = fitAp(obs, 1790000000);
        f.lat += 20.0 / 111320.0;                                            // perturb the fit 20 m north
        const double before = distanceM(f.lat, f.lon, AP_LAT, AP_LON);
        for (int i = 0; i < 30; ++i) f = update(f, sample(AP_LAT + 0.0009 * std::sin(i), AP_LON + 0.0009 * std::cos(i), 8, 2.0));
        const double after = distanceM(f.lat, f.lon, AP_LAT, AP_LON);
        CHECK(after < before && after < 40, "incremental updates: %.1f m → %.1f m (acc ±%.1f, n %d)", before, after, f.acc, f.n);
    }
    // 6. Too few samples
    {
        QList<Obs> obs; obs << sample(AP_LAT + 0.001, AP_LON, 8, 3) << sample(AP_LAT, AP_LON + 0.001, 8, 3);
        const Fit f = fitAp(obs);
        CHECK(!f.valid, "two samples: no fit");
    }
    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
