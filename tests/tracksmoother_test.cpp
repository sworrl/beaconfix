// Synthetic-track tests for src/tracksmoother.{h,cpp}. Build — or use the CMake target tracksmoother_test
// (cmake -DBEACONFIX_TESTS=ON):
//   g++ -std=c++17 -O2 -Wall -Wextra -fPIC $(pkg-config --cflags Qt6Core) tests/tracksmoother_test.cpp src/tracksmoother.cpp -o build/tracksmoother_test $(pkg-config --libs Qt6Core)
// The noise is drawn from the model itself (a Gauss–Markov bias + a correlated "white" part, both from
// Options), so consistency checks (NEES) mean something.
#include "../src/tracksmoother.h"
#include "../src/estimator.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using namespace TrackSmoother;
static int fails = 0;
#define CHECK(cond, fmt, ...) do { if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } else { std::printf("ok   " fmt "\n", ##__VA_ARGS__); } } while (0)

// Synthetic origin (docs/DEVELOPMENT.md): never real places
static const double LAT0 = 40.0030, LON0 = -75.0680;
static const qint64 T0 = 1790000000000LL;
static const double ACC68 = 1.515;
static const Estimator::Frame FR(LAT0, LON0);

struct Truth { double t, x, y; };                    // s, m east, m north

// Fixes along a truth track, with noise drawn from the smoother's own model
struct Noise {
    std::mt19937 rng;
    double bx = 0, by = 0, wx = 0, wy = 0, last = -1;
    explicit Noise(unsigned seed) : rng(seed) {}
    double g() { return std::normal_distribution<double>(0, 1)(rng); }
    void draw(double t, double acc, const Options &o, double *ex, double *ey)
    {
        const double tot2 = (std::pow(o.accScale * acc, 2) + o.accFloor * o.accFloor) / (ACC68 * ACC68);
        const double sb = o.sigmaB, sw = std::sqrt(std::max(tot2 - sb * sb, 0.25));
        if (last < 0) { bx = sb * g(); by = sb * g(); wx = sw * g(); wy = sw * g(); }
        else {
            const double dt = t - last, pb = std::exp(-dt / o.tauB), pw = o.whiteTauS > 0 ? std::exp(-dt / o.whiteTauS) : 0;
            bx = pb * bx + sb * std::sqrt(1 - pb * pb) * g(); by = pb * by + sb * std::sqrt(1 - pb * pb) * g();
            wx = pw * wx + sw * std::sqrt(1 - pw * pw) * g(); wy = pw * wy + sw * std::sqrt(1 - pw * pw) * g();
        }
        last = t;
        *ex = bx + wx; *ey = by + wy;
    }
};

static QList<Fix> observe(const std::vector<Truth> &tr, Noise &nz, double acc, const QString &src, const Options &o = Options())
{
    QList<Fix> f;
    for (const Truth &p : tr) {
        double ex, ey; nz.draw(p.t, acc, o, &ex, &ey);
        Fix x; x.tMs = T0 + qint64(std::llround(p.t * 1000)); x.lat = FR.lat(p.y + ey); x.lon = FR.lon(p.x + ex); x.acc = acc; x.source = src;
        f << x;
    }
    return f;
}
static double errM(double lat, double lon, const Truth &p) { return std::hypot(FR.x(lon) - p.x, FR.y(lat) - p.y); }
static double rms(const std::vector<double> &v) { double s = 0; for (double x : v) s += x * x; return v.empty() ? 0 : std::sqrt(s / v.size()); }

// A walk: legs of a polygon at 1.4 m/s, a fix every dt seconds
static std::vector<Truth> walk(double t0, double x0, double y0, double dur, double dt, double speed = 1.4)
{
    std::vector<Truth> v;
    const double legs[][2] = {{1, 0}, {0, 1}, {-0.7, 0.7}, {-1, 0}, {0, -1}, {0.7, -0.7}};
    double x = x0, y = y0, t = t0, legT = 0; int leg = 0;
    for (double s = 0; s <= dur + 1e-9; s += dt) {
        v.push_back({t0 + s, x, y});
        x += legs[leg][0] * speed * dt; y += legs[leg][1] * speed * dt; t += dt; legT += dt;
        if (legT >= 90) { legT = 0; leg = (leg + 1) % 6; }
    }
    return v;
}

int main()
{
    const Options O;

    // 1. Parked 3 h, a fix a minute: smoothed no worse than raw, σ does not collapse below ~σ_b, NEES ≈ 2
    {
        std::vector<double> raw, sm, sig; double nees = 0; int nn = 0;
        for (unsigned seed = 1; seed <= 20; ++seed) {
            std::vector<Truth> tr;
            for (double t = 0; t <= 3 * 3600; t += 60) tr.push_back({t, 10, -5});
            Noise nz(seed);
            const QList<Fix> f = observe(tr, nz, 3.0, QStringLiteral("phone-stationary"));
            const QList<Out> o = smooth(f);
            for (int i = 0; i < f.size(); ++i) {
                raw.push_back(errM(f[i].lat, f[i].lon, tr[i]));
                const double e = errM(o[i].lat, o[i].lon, tr[i]);
                sm.push_back(e); sig.push_back(std::sqrt(o[i].cxx));
                nees += e * e / o[i].cxx; ++nn;
            }
        }
        nees /= nn;
        std::vector<double> s2 = sig; std::sort(s2.begin(), s2.end());
        CHECK(rms(sm) <= rms(raw), "parked 3 h: smoothed RMS %.2f m ≤ raw %.2f m", rms(sm), rms(raw));
        CHECK(s2.front() >= 0.6 * O.sigmaB, "parked 3 h: per-axis σ ≥ 0.6·σ_b (min %.2f, median %.2f m; σ_b %.1f)", s2.front(), s2[s2.size() / 2], O.sigmaB);
        CHECK(nees > 1.0 && nees < 3.5, "parked 3 h: NEES %.2f (2 = consistent)", nees);
    }

    // 2. A walk (1.4 m/s, 90° and 45° turns), a fix every 2 s. With the model's own noise (a fused provider's
    //    error is correlated over ~15 s, and the bias over 20 min) there is little to average, so the gain
    //    is modest; with independent noise (whiteTauS 0) it is large
    for (const bool white : {false, true}) {
        Options o = O;
        if (white) o.whiteTauS = 0;
        std::vector<double> raw, sm; double nees = 0; int nn = 0;
        for (unsigned seed = 1; seed <= 10; ++seed) {
            const std::vector<Truth> tr = walk(0, 0, 0, 1200, 2);
            Noise nz(100 + seed);
            const QList<Fix> f = observe(tr, nz, 8.0, QStringLiteral("phone-foot"), o);
            Stats st;
            const QList<Out> out = smooth(f, {}, o, &st);
            for (int i = 0; i < f.size(); ++i) {
                raw.push_back(errM(f[i].lat, f[i].lon, tr[i]));
                const double e = errM(out[i].lat, out[i].lon, tr[i]);
                sm.push_back(e); nees += e * e / out[i].cxx; ++nn;
            }
            if (seed == 1) CHECK(st.segments == 1 && st.outliers <= 1, "walk%s: one segment (%d), outliers %d", white ? " (white noise)" : "", st.segments, st.outliers);
        }
        const double lim = white ? 0.6 : 0.9;
        CHECK(rms(sm) < lim * rms(raw) && nees / nn > 0.7 && nees / nn < 3.5, "walk%s: smoothed RMS %.2f m < %.1f × raw %.2f m (NEES %.2f)", white ? " (white noise)" : "", rms(sm), lim, rms(raw), nees / nn);
    }

    // 3. One 80 m spike in a parked hour: flagged, and it does not drag the estimate
    {
        std::vector<Truth> tr;
        for (double t = 0; t <= 3600; t += 60) tr.push_back({t, 0, 0});
        Noise nz(7);
        QList<Fix> f = observe(tr, nz, 3.0, QStringLiteral("phone-stationary"));
        const QList<Out> clean = smooth(f);
        f[30].lon = FR.lon(FR.x(f[30].lon) + 80);
        Stats st;
        const QList<Out> o = smooth(f, {}, O, &st);
        double worst = 0;
        for (int i = 0; i < f.size(); ++i) worst = std::max(worst, Estimator::distanceM(o[i].lat, o[i].lon, clean[i].lat, clean[i].lon));
        CHECK(o[30].outlier && st.outliers == 1, "spike: flagged (outliers %d)", st.outliers);
        CHECK(worst < 1.0, "spike: largest pull on the track %.2f m (only the spike's own information is missing; error there %.2f m)", worst, errM(o[30].lat, o[30].lon, tr[30]));
    }

    // 4. Surveyed point (refs) for 30 min, then a 10-min walk: the bias learnt there helps the walk's start
    {
        std::vector<double> with, without, raw;
        for (unsigned seed = 1; seed <= 40; ++seed) {
            std::vector<Truth> tr;
            for (double t = 0; t < 1800; t += 60) tr.push_back({t, 0, 0});
            const std::vector<Truth> w = walk(1800, 0, 0, 600, 5);
            tr.insert(tr.end(), w.begin(), w.end());
            Noise nz(500 + seed);
            QList<Fix> f;
            for (const Truth &p : tr) {
                double ex, ey; nz.draw(p.t, 3.0, O, &ex, &ey);
                Fix x; x.tMs = T0 + qint64(p.t * 1000); x.lat = FR.lat(p.y + ey); x.lon = FR.lon(p.x + ex); x.acc = 3.0;
                x.source = p.t < 1800 ? QStringLiteral("phone-stationary") : QStringLiteral("phone-foot");
                f << x;
            }
            QList<Ref> refs;
            for (double t = 0; t < 1800; t += 120) { Ref r; r.tMs = T0 + qint64(t * 1000); r.lat = LAT0; r.lon = LON0; r.sd = 1; refs << r; }
            const QList<Out> a = smooth(f, refs), b = smooth(f);
            for (int i = 0; i < f.size(); ++i) {
                if (tr[i].t < 1800 || tr[i].t > 1800 + 120) continue;
                with.push_back(errM(a[i].lat, a[i].lon, tr[i])); without.push_back(errM(b[i].lat, b[i].lon, tr[i]));
                raw.push_back(errM(f[i].lat, f[i].lon, tr[i]));
            }
        }
        CHECK(rms(with) < 0.9 * rms(without), "ref then walk: first 2 min RMS %.2f m with refs < 0.9 × %.2f m without (raw %.2f m)", rms(with), rms(without), rms(raw));
    }

    // 5. A 2-h gap splits the segments: nothing is smoothed across it
    {
        std::vector<Truth> A, B;
        for (double t = 0; t <= 3600; t += 60) A.push_back({t, 0, 0});
        for (double t = 3 * 3600; t <= 4 * 3600; t += 60) B.push_back({t, 500, 200});
        Noise na(11), nb(12);
        const QList<Fix> fa = observe(A, na, 3.0, QStringLiteral("phone-stationary")), fb = observe(B, nb, 3.0, QStringLiteral("phone-stationary"));
        Stats st;
        const QList<Out> all = smooth(fa + fb, {}, O, &st), alone = smooth(fa);
        double d = 0;
        for (int i = 0; i < fa.size(); ++i) d = std::max({d, std::fabs(all[i].lat - alone[i].lat), std::fabs(all[i].lon - alone[i].lon), std::fabs(all[i].cxx - alone[i].cxx)});
        CHECK(st.segments == 2 && all[fa.size()].segmentStart && all[0].segmentStart && !all[1].segmentStart, "gap: two segments (%d), second starts at the first fix after the gap", st.segments);
        CHECK(d < 1e-12, "gap: the first segment is untouched by the second (max diff %.1e)", d);
    }

    // 6. calibrateScale recovers an over-confidence factor (and the factor beside a floor)
    {
        std::mt19937 rng(99);
        std::uniform_real_distribution<double> ua(3, 10);
        std::normal_distribution<double> g(0, 1);
        QList<Fix> f1, f2;
        for (int i = 0; i < 400; ++i) {
            const double acc = ua(rng);
            double s = 2.3 * acc / ACC68;
            Fix x; x.acc = acc; x.lat = FR.lat(s * g(rng)); x.lon = FR.lon(s * g(rng)); f1 << x;
            s = std::sqrt(std::pow(1.2 * acc, 2) + 36) / ACC68;
            Fix y; y.acc = acc; y.lat = FR.lat(s * g(rng)); y.lon = FR.lon(s * g(rng)); f2 << y;
        }
        const double k1 = calibrateScale(f1, LAT0, LON0), k2 = calibrateScale(f2, LAT0, LON0, 6.0);
        CHECK(std::fabs(k1 / 2.3 - 1) < 0.15, "calibrateScale: %.2f for a true 2.3", k1);
        CHECK(std::fabs(k2 / 1.2 - 1) < 0.15, "calibrateScale with a 6 m floor: %.2f for a true 1.2", k2);
    }

    // 7. Allan deviation: white noise falls as 1/√τ; irregular sampling is binned
    {
        std::mt19937 rng(5);
        std::normal_distribution<double> g(0, 3);
        std::uniform_real_distribution<double> jit(0, 20);
        QList<Fix> f;
        for (int i = 0; i < 6000; ++i) { Fix x; x.tMs = T0 + qint64((i * 60 + jit(rng)) * 1000); x.lat = FR.lat(g(rng)); x.lon = FR.lon(g(rng)); f << x; }
        const QList<AllanPoint> a = allanDeviation(f, 60);
        CHECK(a.size() >= 6 && std::fabs(a[0].adevE / 3 - 1) < 0.1 && std::fabs(a[4].adevN / (3 / 4.0) - 1) < 0.25,
              "Allan: white σ 3 → %.2f at 1 min, %.2f at 16 min (expect 0.75)", a.size() ? a[0].adevE : 0, a.size() > 4 ? a[4].adevN : 0);
    }

    // 8. Duplicates (sync echoes) count once; unsorted input gives the same answer
    {
        const std::vector<Truth> tr = walk(0, 0, 0, 600, 5);
        Noise nz(21);
        const QList<Fix> f = observe(tr, nz, 6.0, QStringLiteral("phone-foot"));
        QList<Fix> d;
        for (const Fix &x : f) { d << x; d << x; }
        QList<Fix> r = f; std::reverse(r.begin(), r.end());
        Stats st;
        const QList<Out> a = smooth(f), b = smooth(d, {}, O, &st), c = smooth(r);
        double dd = 0, dr = 0;
        for (int i = 0; i < f.size(); ++i) {
            dd = std::max({dd, std::fabs(a[i].lat - b[2 * i].lat), std::fabs(a[i].lat - b[2 * i + 1].lat), std::fabs(a[i].cxx - b[2 * i].cxx)});
            dr = std::max({dr, std::fabs(a[i].lon - c[f.size() - 1 - i].lon), std::fabs(a[i].cxx - c[f.size() - 1 - i].cxx)});
        }
        CHECK(dd < 1e-12 && st.duplicates == f.size(), "duplicates count once (%d dropped, max diff %.1e)", st.duplicates, dd);
        CHECK(dr < 1e-12, "unsorted input: same answer (max diff %.1e)", dr);
    }

    // 9. A drive logged as plain "phone-gps": recognised as vehicle, not chopped into outliers
    {
        std::vector<Truth> tr;
        double x = 0, y = 0, hd = 0;
        for (double t = 0; t <= 900; t += 2) {
            tr.push_back({t, x, y});
            if (std::fmod(t, 120) >= 100) hd += M_PI / 2 / 10;        // a 90° turn every 2 min
            x += 25 * std::cos(hd) * 2; y += 25 * std::sin(hd) * 2;
        }
        Noise nz(31);
        const QList<Fix> f = observe(tr, nz, 10.0, QStringLiteral("phone-gps"));
        Stats st;
        const QList<Out> o = smooth(f, {}, O, &st);
        std::vector<double> raw, sm; int veh = 0;
        for (int i = 0; i < f.size(); ++i) { raw.push_back(errM(f[i].lat, f[i].lon, tr[i])); sm.push_back(errM(o[i].lat, o[i].lon, tr[i])); veh += o[i].mode == Mode::Vehicle; }
        CHECK(veh > 0.9 * f.size() && st.outliers <= 2 && st.restarts == 0, "drive: %d/%d fixes vehicle, %d outliers, %d restarts", veh, int(f.size()), st.outliers, st.restarts);
        CHECK(rms(sm) < rms(raw), "drive: smoothed RMS %.2f m < raw %.2f m", rms(sm), rms(raw));
    }

    // 10. Dropped fixes (IP, coarser than maxAcc) keep their raw position and do not count
    {
        const std::vector<Truth> tr = walk(0, 0, 0, 300, 10);
        Noise nz(41);
        QList<Fix> f = observe(tr, nz, 5.0, QStringLiteral("phone-foot"));
        f[5].source = QStringLiteral("ip"); f[5].acc = 50000; f[9].acc = 900;
        Stats st;
        const QList<Out> o = smooth(f, {}, O, &st);
        CHECK(st.dropped == 2 && o[5].dropped && o[9].dropped && o[9].lat == f[9].lat && !o[6].dropped, "dropped: IP and a 900 m fix (%d)", st.dropped);
    }

    // 11. Speed: 10 000 fixes
    {
        std::vector<Truth> tr = walk(0, 0, 0, 10000 * 3 - 3, 3);
        Noise nz(51);
        const QList<Fix> f = observe(tr, nz, 8.0, QStringLiteral("phone-gps"));
        const auto a = std::chrono::steady_clock::now();
        Stats st;
        const QList<Out> o = smooth(f, {}, O, &st);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        CHECK(o.size() == f.size() && ms < 200, "10 000 fixes in %.1f ms (%d segments)", ms, st.segments);
    }

    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
