#pragma once
// Position estimation from signal-strength samples.
//
// Model: RSSI = P0 − 10·n·log10(d)   (log-distance path loss)
//   P0  reference level at 1 m (fitted per AP: radios differ by 20 dB)
//   n   path-loss exponent (2.4 outdoors; fitted in [1.8, 4] once there are ≥ 8 samples)
//   d   distance between the observer's fix and the AP
//
// Fit: iteratively reweighted nonlinear least squares over (x, y, P0[, n]) in a local
// metric frame — Gauss–Newton with Levenberg damping, Huber weights on the dB residuals,
// each sample also weighted by the observer's fix accuracy, its age and its status.
// The seed is the signal-weighted centroid. The 1-σ error ellipse comes from
// (JᵀWJ)⁻¹·σ²; the circular accuracy reported is its semi-major axis (conservative).
//
// Guard: distinct vantage points are counted on a 25 m grid. Fewer than three, or all
// of them inside 1.5× the worst fix accuracy, and no position is claimed — the geometry
// cannot separate "the AP is here" from "we were standing here".
//
// Everything here is pure (no Qt I/O, no state), so it is unit-testable and shared by
// the AP fitter, the self-locator and the incremental update.
#include <QList>
#include <QString>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace Estimator {

struct Obs {
    double lat = 0, lon = 0;          // where the observer was
    double acc = 30;                  // its fix accuracy (m, 1-σ-ish)
    int    dbm = -100;
    qint64 t = 0;                     // seconds since the epoch (0 = unknown)
    double weight = 1;                // extra factor: 0.3 for "travels with you" statuses etc.
    QString device;                   // who heard it ("" = this host)
};

struct Fit {
    bool   valid = false;
    double lat = 0, lon = 0;
    double acc = 0;                   // circular 1-σ (m) — semi-major axis of the error ellipse
    double semiMajor = 0, semiMinor = 0, orientDeg = 0;   // error ellipse
    double rms = 0;                   // weighted residual RMS in dB
    double p0 = -40;                  // fitted reference level at 1 m
    double pathloss = 2.4;            // path-loss exponent used / fitted
    bool   fittedN = false;
    int    n = 0;                     // samples used
    int    vantage = 0;               // distinct vantage points (25 m cells)
    int    rejected = 0;              // outliers down-weighted to ~0
    QString quality;                  // good | fair | poor | none
    qint64 updated = 0;               // seconds since the epoch
};

struct Options {
    double defaultN = 2.4;
    int    minSamples = 3;
    int    minVantage = 3;
    int    fitNFrom = 8;              // fit the exponent from this many samples
    double huberDb = 6.0;             // residuals beyond this many dB get linear (not quadratic) weight
    double ageDays = 30;              // older samples fade
    double geomFactor = 1.5;          // vantage spread must exceed this × worst fix accuracy
    int    maxIter = 40;
};

// A known transmitter for self-location
struct Known { double lat = 0, lon = 0, acc = 25; int dbm = -100; double p0 = -40, pathloss = 2.4; bool haveModel = false; QString bssid; };

struct SelfFix { bool valid = false; double lat = 0, lon = 0, acc = 0, rms = 0; int used = 0, rejected = 0; };

// ── helpers ──────────────────────────────────────────────────────────────────
inline double distanceM(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0, d2r = M_PI / 180.0;
    const double dLat = (lat2 - lat1) * d2r, dLon = (lon2 - lon1) * d2r;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) + std::cos(lat1 * d2r) * std::cos(lat2 * d2r) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * R * std::asin(std::sqrt(a));
}
inline double modelDbm(double p0, double n, double d) { return p0 - 10.0 * n * std::log10(std::max(1.0, d)); }
inline double modelDistance(double p0, double n, int dbm) { return std::pow(10.0, (p0 - dbm) / (10.0 * n)); }
inline double huberWeight(double r, double k) { const double a = std::fabs(r); return a <= k ? 1.0 : k / a; }

// Local metric frame around (lat0, lon0)
struct Frame {
    double lat0 = 0, lon0 = 0, mx = 1, my = 111320.0;
    Frame() = default;
    Frame(double lat, double lon) : lat0(lat), lon0(lon), mx(111320.0 * std::cos(lat * M_PI / 180.0)) {}
    double x(double lon) const { return (lon - lon0) * mx; }
    double y(double lat) const { return (lat - lat0) * my; }
    double lat(double y) const { return lat0 + y / my; }
    double lon(double x) const { return lon0 + x / mx; }
};

// Distinct vantage points. Two samples are the same place when they are closer than
// 25 m or than 1.5× the larger of their fix errors — jitter of the fix must not count as
// having moved.
inline int vantageCount(const QList<Obs> &obs, double cellM = 25.0)
{
    QList<Obs> cells;
    for (const Obs &o : obs) {
        bool found = false;
        for (const Obs &c : cells) if (distanceM(c.lat, c.lon, o.lat, o.lon) < std::max(cellM, 1.5 * std::max(c.acc, o.acc))) { found = true; break; }
        if (!found) cells.append(o);
    }
    return cells.size();
}

inline double spreadM(const QList<Obs> &obs)
{
    double best = 0;
    for (int i = 0; i < obs.size(); ++i)
        for (int j = i + 1; j < obs.size(); ++j) best = std::max(best, distanceM(obs[i].lat, obs[i].lon, obs[j].lat, obs[j].lon));
    return best;
}

// Solve the symmetric positive system A·dx = b (k ≤ 4) by Gaussian elimination with pivoting
inline bool solve(double A[4][4], double b[4], int k, double out[4])
{
    double M[4][5];
    for (int i = 0; i < k; ++i) { for (int j = 0; j < k; ++j) M[i][j] = A[i][j]; M[i][k] = b[i]; }
    for (int c = 0; c < k; ++c) {
        int piv = c;
        for (int r = c + 1; r < k; ++r) if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
        if (std::fabs(M[piv][c]) < 1e-12) return false;
        if (piv != c) for (int j = 0; j <= k; ++j) std::swap(M[c][j], M[piv][j]);
        for (int r = 0; r < k; ++r) {
            if (r == c) continue;
            const double f = M[r][c] / M[c][c];
            for (int j = c; j <= k; ++j) M[r][j] -= f * M[c][j];
        }
    }
    for (int i = 0; i < k; ++i) out[i] = M[i][k] / M[i][i];
    return true;
}

// 2×2 symmetric inverse
inline bool invert2(double a, double b, double d, double out[3])   // [[a b][b d]] → [[o0 o1][o1 o2]]
{
    const double det = a * d - b * b;
    if (std::fabs(det) < 1e-12) return false;
    out[0] = d / det; out[1] = -b / det; out[2] = a / det;
    return true;
}

inline void ellipse(double cxx, double cxy, double cyy, double *major, double *minor, double *orientDeg)
{
    const double tr = cxx + cyy, det = cxx * cyy - cxy * cxy;
    const double disc = std::sqrt(std::max(0.0, tr * tr / 4 - det));
    const double l1 = std::max(0.0, tr / 2 + disc), l2 = std::max(0.0, tr / 2 - disc);
    *major = std::sqrt(l1); *minor = std::sqrt(l2);
    *orientDeg = std::atan2(l1 - cxx, cxy) * 180.0 / M_PI;
    if (std::fabs(cxy) < 1e-12) *orientDeg = cxx >= cyy ? 90.0 : 0.0;   // x = east: major axis along east → 90° bearing
}

// ── the AP fitter ────────────────────────────────────────────────────────────
inline double tukeyWeight(double r, double c) { const double a = std::fabs(r); if (a >= c) return 0.0; const double q = 1 - (a / c) * (a / c); return q * q; }
// ρ functions (the objective actually minimised): Huber grows linearly past k, Tukey saturates at c²/6 —
// so a solution that "explains" nothing by rejecting every sample has the WORST cost, not zero.
inline double huberRho(double r, double k) { const double a = std::fabs(r); return a <= k ? 0.5 * r * r : k * (a - 0.5 * k); }
inline double tukeyRho(double r, double c) { const double a = std::fabs(r); if (a >= c) return c * c / 6.0; const double q = 1 - (a / c) * (a / c); return c * c / 6.0 * (1 - q * q * q); }

namespace detail {
struct S { double x, y, w; int dbm; };
struct Solution { double x = 0, y = 0, p0 = -40, n = 2.4, cost = 1e300; int inliers = 0; bool ok = false; };

// IRLS from one seed. Huber for the first half of the iterations, then the redescending
// Tukey biweight (c = 2× the Huber scale) so gross outliers end up with zero weight.
inline Solution irls(const QList<S> &s, double x, double y, double p0, double n, bool fitN, const Options &opt)
{
    const int k = fitN ? 4 : 3;
    double lambda = 1e-2;
    auto weightOf = [&](double r, int it) { return it < opt.maxIter / 2 ? huberWeight(r, opt.huberDb) : tukeyWeight(r, 2.0 * opt.huberDb); };
    auto costOf = [&](double cx, double cy, double cp0, double cn, int it) {
        double c = 0;
        for (const S &q : s) { const double d = std::max(1.0, std::hypot(cx - q.x, cy - q.y)); const double r = q.dbm - modelDbm(cp0, cn, d); c += q.w * (it < opt.maxIter / 2 ? huberRho(r, opt.huberDb) : tukeyRho(r, 2.0 * opt.huberDb)); }
        return c;
    };
    Solution sol; sol.x = x; sol.y = y; sol.p0 = p0; sol.n = n;
    for (int it = 0; it < opt.maxIter; ++it) {
        double A[4][4] = {{0}}, b[4] = {0};
        for (const S &q : s) {
            const double dx = x - q.x, dy = y - q.y, d = std::max(1.0, std::hypot(dx, dy));
            const double r = q.dbm - modelDbm(p0, n, d);
            const double w = q.w * weightOf(r, it);
            const double g = 10.0 * n / std::log(10.0) / (d * d);
            double J[4] = {g * dx, g * dy, -1.0, -10.0 * std::log10(d)};
            for (int a = 0; a < k; ++a) { for (int c = 0; c < k; ++c) A[a][c] += w * J[a] * J[c]; b[a] -= w * J[a] * r; }
        }
        const double cost = costOf(x, y, p0, n, it);
        for (int a = 0; a < k; ++a) A[a][a] *= (1.0 + lambda);
        double dx[4];
        if (!solve(A, b, k, dx)) break;
        const double stepLen = std::hypot(dx[0], dx[1]);                // trust region: no leap further than 300 m per iteration
        if (stepLen > 300.0) { dx[0] *= 300.0 / stepLen; dx[1] *= 300.0 / stepLen; }
        const double nx = x + dx[0], ny = y + dx[1], np0 = std::clamp(p0 + dx[2], -90.0, 10.0);
        const double nn = fitN ? std::clamp(n + dx[3], 1.8, 4.0) : n;
        const double newCost = costOf(nx, ny, np0, nn, it);
        if (newCost <= cost) {
            const bool small = std::hypot(dx[0], dx[1]) < 0.2 && std::fabs(cost - newCost) < 1e-3 * std::max(1.0, cost);
            x = nx; y = ny; p0 = np0; n = nn; lambda = std::max(1e-6, lambda / 3);
            if (small && it >= opt.maxIter / 2) break;
            if (small) it = opt.maxIter / 2 - 1;                      // converged under Huber: switch to Tukey now
        } else { lambda *= 8; if (lambda > 1e6) { if (it < opt.maxIter / 2) it = opt.maxIter / 2 - 1; else break; lambda = 1e-2; } }
    }
    sol.x = x; sol.y = y; sol.p0 = p0; sol.n = n; sol.cost = costOf(x, y, p0, n, opt.maxIter);
    for (const S &q : s) { const double d = std::max(1.0, std::hypot(x - q.x, y - q.y)); if (tukeyWeight(q.dbm - modelDbm(p0, n, d), 2.0 * opt.huberDb) >= 0.25) ++sol.inliers; }
    sol.ok = std::isfinite(sol.cost) && std::isfinite(x) && std::isfinite(y) && sol.inliers >= std::max(opt.minSamples, int(s.size()) / 2);
    return sol;
}
} // namespace detail

inline Fit fitAp(const QList<Obs> &samples, qint64 now = 0, const Options &opt = Options())
{
    Fit f; f.n = samples.size(); f.pathloss = opt.defaultN; f.updated = now;
    f.quality = QStringLiteral("none");
    if (samples.size() < opt.minSamples) return f;
    f.vantage = vantageCount(samples);
    double worstAcc = 0; for (const Obs &o : samples) worstAcc = std::max(worstAcc, o.acc);
    if (f.vantage < opt.minVantage) return f;
    if (spreadM(samples) < opt.geomFactor * worstAcc) return f;      // we never really moved relative to the fix error

    // Base weights: fix accuracy, age, status
    const Frame fr(samples[0].lat, samples[0].lon);
    QList<detail::S> s; s.reserve(samples.size());
    double sw = 0, sx = 0, sy = 0, ux = 0, uy = 0;
    for (const Obs &o : samples) {
        double w = 1.0 / (1.0 + std::pow(o.acc / 25.0, 2));
        if (now > 0 && o.t > 0) { const double days = double(now - o.t) / 86400.0; if (days > opt.ageDays) w *= std::max(0.15, opt.ageDays / days); }
        w *= std::max(0.05, o.weight);
        s.append({fr.x(o.lon), fr.y(o.lat), w, o.dbm});
        const double cw = w * std::pow(10.0, o.dbm / 40.0);          // seed: louder pulls harder (gently)
        sw += cw; sx += cw * s.last().x; sy += cw * s.last().y; ux += s.last().x; uy += s.last().y;
    }
    const bool fitN = samples.size() >= opt.fitNFrom;
    // Seeds: weighted centroid, plain centroid, and the centroid of the loudest quartile.
    // Gross outliers (a loud sample far away) capture one seed; the others escape it.
    QList<QPair<double, double>> seeds{{sx / sw, sy / sw}, {ux / s.size(), uy / s.size()}};
    {
        QList<detail::S> loud = s; std::sort(loud.begin(), loud.end(), [](const detail::S &a, const detail::S &b) { return a.dbm > b.dbm; });
        const int q = std::max(3, int(loud.size()) / 4); double lx = 0, ly = 0;
        for (int i = 0; i < q && i < loud.size(); ++i) { lx += loud[i].x; ly += loud[i].y; }
        seeds.append({lx / std::min(q, int(loud.size())), ly / std::min(q, int(loud.size()))});
    }
    detail::Solution best;
    for (const auto &sd : seeds) {
        QList<double> p; for (const detail::S &q : s) p << q.dbm + 10.0 * opt.defaultN * std::log10(std::max(1.0, std::hypot(sd.first - q.x, sd.second - q.y)));
        std::sort(p.begin(), p.end());
        const detail::Solution sol = detail::irls(s, sd.first, sd.second, p[p.size() / 2], opt.defaultN, fitN, opt);
        if (sol.ok && (sol.inliers > best.inliers || (sol.inliers == best.inliers && sol.cost < best.cost))) best = sol;
    }
    if (!best.ok) return f;
    const double x = best.x, y = best.y, p0 = best.p0, n = best.n;
    const int k = fitN ? 4 : 3;
    // Statistics at the solution (Tukey weights: outliers contribute nothing)
    double A[4][4] = {{0}}, wsum = 0, wres = 0; int rejected = 0, kept = 0;
    for (const detail::S &q : s) {
        const double dx = x - q.x, dy = y - q.y, d = std::max(1.0, std::hypot(dx, dy));
        const double r = q.dbm - modelDbm(p0, n, d), h = tukeyWeight(r, 2.0 * opt.huberDb), w = q.w * h;
        if (h < 0.25) { ++rejected; continue; }
        ++kept;
        const double g = 10.0 * n / std::log(10.0) / (d * d);
        double J[4] = {g * dx, g * dy, -1.0, -10.0 * std::log10(d)};
        for (int a = 0; a < k; ++a) for (int c = 0; c < k; ++c) A[a][c] += w * J[a] * J[c];
        wsum += w; wres += w * r * r;
    }
    if (kept < opt.minSamples) return f;
    const int dof = std::max(1, kept - k);
    const double sigma2 = std::max(4.0, wres / std::max(1e-9, wsum) * double(kept) / dof);   // dB², floor 2 dB
    double cov[3] = {1e6, 0, 1e6};
    {
        double inv[4][4] = {{0}};
        bool ok = true;
        for (int col = 0; col < k && ok; ++col) {
            double e[4] = {0}; e[col] = 1; double out[4];
            double Acopy[4][4]; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) Acopy[i][j] = A[i][j];
            if (!solve(Acopy, e, k, out)) { ok = false; break; }
            for (int i = 0; i < k; ++i) inv[i][col] = out[i];
        }
        if (ok) { cov[0] = inv[0][0] * sigma2; cov[1] = inv[0][1] * sigma2; cov[2] = inv[1][1] * sigma2; }
    }
    ellipse(cov[0], cov[1], cov[2], &f.semiMajor, &f.semiMinor, &f.orientDeg);
    // The observers' own error adds in quadrature (their median accuracy)
    QList<double> accs; for (const Obs &o : samples) accs << o.acc; std::sort(accs.begin(), accs.end());
    const double medAcc = accs[accs.size() / 2];
    f.semiMajor = std::sqrt(f.semiMajor * f.semiMajor + medAcc * medAcc * 0.5);
    f.semiMinor = std::sqrt(f.semiMinor * f.semiMinor + medAcc * medAcc * 0.5);
    f.acc = std::max(8.0, f.semiMajor);
    f.rms = std::sqrt(wres / std::max(1e-9, wsum));
    f.lat = fr.lat(y); f.lon = fr.lon(x); f.p0 = p0; f.pathloss = n; f.fittedN = fitN; f.rejected = rejected;
    f.valid = std::isfinite(f.lat) && std::isfinite(f.lon) && f.acc < 1500;
    if (!f.valid) { f.quality = QStringLiteral("none"); return f; }
    f.quality = (f.acc <= 40 && f.vantage >= 5 && f.rms <= 6) ? QStringLiteral("good") : (f.acc <= 120 && f.vantage >= 3) ? QStringLiteral("fair") : QStringLiteral("poor");
    return f;
}

// ── incremental update (between batched refits) ──────────────────────────────
// One new sample nudges an existing fit: the sample says "the AP is d metres from here"
// (d from the AP's own P0/n). Linearised as a 2-D Kalman step along the radial direction
// with σ_d from the dB noise, and no information tangentially.
inline Fit update(const Fit &prev, const Obs &o, double sigmaDb = 6.0)
{
    if (!prev.valid) return prev;
    Fit f = prev;
    const Frame fr(prev.lat, prev.lon);                                 // the fit sits at the origin
    const double ox = fr.x(o.lon), oy = fr.y(o.lat);                   // the observer
    const double r = std::max(1.0, std::hypot(ox, oy));                 // predicted range: fit ↔ observer
    const double d = modelDistance(prev.p0, prev.pathloss, o.dbm);      // measured range from the sample
    const double ux = -ox / r, uy = -oy / r;                            // unit vector observer → fit
    const double sigD = d * std::log(10.0) * sigmaDb / (10.0 * prev.pathloss);   // dB noise → metres at this range
    const double R = sigD * sigD + o.acc * o.acc;
    const double P = prev.acc * prev.acc;                               // isotropic prior
    const double K = P / (P + R);
    const double innov = d - r;                                         // > 0: the AP is further from the observer than the fit says
    const double nx = K * innov * ux, ny = K * innov * uy;              // step along the radial, away from the observer
    f.lat = fr.lat(ny); f.lon = fr.lon(nx);
    f.acc = std::max(8.0, std::sqrt((1 - K) * P + R * K * K * 0.5));   // shrinks along the radial only: keep it conservative
    f.n = prev.n + 1;
    f.updated = std::max(prev.updated, o.t);
    return f;
}

// ── self-location: where are WE, from beacons with known positions ───────────
inline SelfFix selfLocate(const QList<Known> &known, const Options &opt = Options())
{
    SelfFix out;
    if (known.size() < 2) return out;
    const Frame fr(known[0].lat, known[0].lon);
    struct S { double x, y, w, d, sig; };
    QList<S> s; double sw = 0, sx = 0, sy = 0;
    for (const Known &k : known) {
        const double p0 = k.haveModel ? k.p0 : -40.0, n = k.haveModel ? k.pathloss : opt.defaultN;
        const double d = std::clamp(modelDistance(p0, n, k.dbm), 1.0, 1500.0);
        const double sigD = d * std::log(10.0) * 6.0 / (10.0 * n);      // 6 dB of shadowing → range σ
        const double sig2 = sigD * sigD + k.acc * k.acc;
        const double w = 1.0 / sig2;
        s.append({fr.x(k.lon), fr.y(k.lat), w, d, std::sqrt(sig2)});
        const double cw = std::pow(10.0, k.dbm / 20.0) / std::max(10.0, k.acc);
        sw += cw; sx += cw * s.last().x; sy += cw * s.last().y;
    }
    double x = sx / sw, y = sy / sw;
    if (known.size() >= 3) {
        double lambda = 1e-2;
        for (int it = 0; it < opt.maxIter; ++it) {
            double A[4][4] = {{0}}, b[4] = {0}, cost = 0;
            for (const S &q : s) {
                const double dx = x - q.x, dy = y - q.y, r = std::max(1.0, std::hypot(dx, dy));
                const double res = r - q.d, h = huberWeight(res / q.sig, 2.0), w = q.w * h;
                const double jx = dx / r, jy = dy / r;
                A[0][0] += w * jx * jx; A[0][1] += w * jx * jy; A[1][0] += w * jx * jy; A[1][1] += w * jy * jy;
                b[0] -= w * jx * res; b[1] -= w * jy * res; cost += w * res * res;
            }
            A[0][0] *= 1 + lambda; A[1][1] *= 1 + lambda;
            double dx[4]; if (!solve(A, b, 2, dx)) break;
            const double nx = x + dx[0], ny = y + dx[1]; double newCost = 0;
            for (const S &q : s) { const double res = std::max(1.0, std::hypot(nx - q.x, ny - q.y)) - q.d; newCost += q.w * huberWeight(res / q.sig, 2.0) * res * res; }
            if (newCost <= cost) { x = nx; y = ny; lambda = std::max(1e-6, lambda / 3); if (std::hypot(dx[0], dx[1]) < 0.2) break; }
            else { lambda *= 8; if (lambda > 1e6) break; }
        }
    }
    double A[3] = {0, 0, 0}, wsum = 0, wres = 0; int rejected = 0;
    for (const S &q : s) {
        const double dx = x - q.x, dy = y - q.y, r = std::max(1.0, std::hypot(dx, dy));
        const double res = r - q.d, h = huberWeight(res / q.sig, 2.0), w = q.w * h;
        if (h < 0.5) ++rejected;
        const double jx = dx / r, jy = dy / r;
        A[0] += w * jx * jx; A[1] += w * jx * jy; A[2] += w * jy * jy;
        wsum += w; wres += w * res * res;
    }
    const double chi = wres / std::max(1, int(s.size()) - 2);          // unit-variance residual scaling
    double inv[3];
    double acc;
    if (known.size() >= 3 && invert2(A[0], A[1], A[2], inv)) { double maj, mn, o; ellipse(inv[0] * std::max(1.0, chi), inv[1] * std::max(1.0, chi), inv[2] * std::max(1.0, chi), &maj, &mn, &o); acc = maj; }
    else { acc = 0; for (const S &q : s) acc = std::max(acc, q.sig); }
    // and the spread of the ranges around the solution, honestly
    double spread = 0; for (const S &q : s) { const double res = std::max(1.0, std::hypot(x - q.x, y - q.y)) - q.d; spread += q.w * res * res; }
    spread = std::sqrt(spread / std::max(1e-9, wsum));
    out.valid = true; out.lat = fr.lat(y); out.lon = fr.lon(x);
    out.acc = std::max(15.0, std::max(acc, spread * 0.7));
    out.rms = std::sqrt(wres / std::max(1e-9, wsum));
    out.used = s.size() - rejected; out.rejected = rejected;
    return out;
}

} // namespace Estimator
