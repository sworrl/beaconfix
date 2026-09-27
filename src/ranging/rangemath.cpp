// BeaconFix ranging maths — see rangemath.h and docs/RANGING.md. Keep in lock-step with
// android/app/src/main/java/org/sworrl/beaconfix/ranging/RangeMath.kt: same operations, same order.
#include "rangemath.h"
#include <algorithm>
#include <limits>
#include <map>
#include <QCryptographicHash>

namespace RangeMath {

double fsplDb(double dM, double fMHz) { return 20.0 * std::log10(dM) + 20.0 * std::log10(fMHz) - 27.55; }
double priorP0Ble(int txPower) { return txPower == 127 ? -59.0 : double(txPower) - 41.0; }
double priorP0Wifi(double fMHz) { return -40.0 - 20.0 * std::log10(fMHz / 2437.0); }
double modelLevel(double p0, double n, double dM) { return p0 - 10.0 * n * std::log10(std::max(dM, 0.05)); }
double modelDistance(double p0, double n, double level) { return std::pow(10.0, (p0 - level) / (10.0 * n)); }

double quantile7(std::vector<double> v, double p)
{
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(v.begin(), v.end());
    const int N = int(v.size());
    if (N == 1) return v[0];
    const double h = (N - 1) * p;
    const int lo = int(std::floor(h));
    const int hi = std::min(lo + 1, N - 1);
    return v[lo] + (h - lo) * (v[hi] - v[lo]);
}
double median(const std::vector<double> &v) { return quantile7(v, 0.5); }

double chi2Quantile(double m, double z)
{
    const double a = 2.0 / (9.0 * m);
    const double b = 1.0 - a + z * std::sqrt(a);
    return b <= 0 ? 0.0 : m * b * b * b;
}

// ── level ────────────────────────────────────────────────────────────────────
Level levelFromSamples(const std::vector<double> &rssi, int channels, double spanS, bool moving)
{
    Level L;
    const int N = int(rssi.size());
    if (N == 0) return L;
    std::vector<double> x(N);
    for (int i = 0; i < N; ++i) x[i] = std::pow(10.0, rssi[i] / 10.0);
    double t = std::numeric_limits<double>::infinity(), c = 1.0;
    if (N >= 8) { t = quantile7(x, 0.95); c = 0.95; }
    double sum = 0;
    for (int i = 0; i < N; ++i) sum += std::min(x[i], t);
    L.dbm = 10.0 * std::log10((sum / N) / c);
    const double tau = moving ? 0.2 : 30.0;
    L.nEff = std::min(double(N), double(std::max(1, channels)) * (1.0 + std::max(0.0, spanS) / tau));
    L.sigma = kDbPerNeper / std::sqrt(L.nEff);
    L.n = N;
    L.valid = true;
    return L;
}

// ── range filter ─────────────────────────────────────────────────────────────
RangeFilter::RangeFilter(int offsets, double u0, double puu0, double offsetVar0, double rttOffsetVar0)
{
    nx = 2 + std::max(0, std::min(offsets, kMax - 2));
    x[0] = u0;
    P[0][0] = puu0;
    for (int k = 1; k < nx - 1; ++k) P[k][k] = offsetVar0;
    P[nx - 1][nx - 1] = rttOffsetVar0;
    offsetVarCap = offsetVar0;
}

void RangeFilter::predict(double dtS, bool moving)
{
    if (!(dtS > 0)) return;
    P[0][0] += (moving ? 0.0025 : 4e-6) * dtS;
    // Moving more than λ/2 draws a new multipath fade: the link offsets lose that part of their knowledge.
    for (int k = 1; k < nx - 1; ++k) P[k][k] = moving ? std::min(P[k][k] + kFrozenFadeVar, std::max(P[k][k], offsetVarCap))
                                                    : P[k][k] + 1e-4 * dtS;
    P[nx - 1][nx - 1] += 1e-6 * dtS;
}

static void joseph(int nx, double P[RangeFilter::kMax][RangeFilter::kMax], const double *K, const double *H, double R)
{
    double A[RangeFilter::kMax][RangeFilter::kMax], T[RangeFilter::kMax][RangeFilter::kMax], Q[RangeFilter::kMax][RangeFilter::kMax];
    for (int i = 0; i < nx; ++i) for (int j = 0; j < nx; ++j) A[i][j] = (i == j ? 1.0 : 0.0) - K[i] * H[j];
    for (int i = 0; i < nx; ++i) for (int j = 0; j < nx; ++j) { double s = 0; for (int m = 0; m < nx; ++m) s += A[i][m] * P[m][j]; T[i][j] = s; }
    for (int i = 0; i < nx; ++i) for (int j = 0; j < nx; ++j) { double s = 0; for (int m = 0; m < nx; ++m) s += T[i][m] * A[j][m]; Q[i][j] = s + K[i] * R * K[j]; }
    for (int i = 0; i < nx; ++i) for (int j = 0; j < nx; ++j) P[i][j] = 0.5 * (Q[i][j] + Q[j][i]);
}

double RangeFilter::scalarUpdate(const double *H, double y, double R, double huberK)
{
    double PHt[kMax];
    for (int i = 0; i < nx; ++i) { double s = 0; for (int j = 0; j < nx; ++j) s += P[i][j] * H[j]; PHt[i] = s; }
    double HPHt = 0;
    for (int i = 0; i < nx; ++i) HPHt += H[i] * PHt[i];
    double S = HPHt + R;
    const double z = y / std::sqrt(S);
    double Reff = R;
    if (std::fabs(z) > huberK) { Reff = R * (std::fabs(z) / huberK); S = HPHt + Reff; }
    double K[kMax];
    for (int i = 0; i < nx; ++i) K[i] = PHt[i] / S;
    K[nx - 1] = 0.0;                                            // e_c is a Schmidt "consider" state: never updated
    for (int i = 0; i < nx; ++i) x[i] += K[i] * y;
    joseph(nx, P, K, H, Reff);
    x[0] = std::max(-2.0, std::min(4.0, x[0]));
    return z;
}

double RangeFilter::updateRssi(int k, double level, double sigmaL, double p0, double n)
{
    if (k < 0 || 1 + k >= nx - 1) return 0;
    double H[kMax] = {0, 0, 0, 0, 0};
    H[0] = -10.0 * n;
    H[1 + k] = 1.0;
    const double h = p0 - 10.0 * n * x[0] + x[1 + k];
    return scalarUpdate(H, level - h, sigmaL * sigmaL, 2.5);
}

double RangeFilter::updateRtt(double rangeM, double sigmaM, double offsetM)
{
    const double s = std::max(sigmaM, kRttFloor);
    double x0[kMax], xi[kMax], P0[kMax][kMax];
    for (int i = 0; i < nx; ++i) { x0[i] = x[i]; xi[i] = x[i]; for (int j = 0; j < nx; ++j) P0[i][j] = P[i][j]; }
    const int c = nx - 1;
    double H[kMax] = {0, 0, 0, 0, 0}, K[kMax] = {0, 0, 0, 0, 0}, R = s * s, z = 0;
    for (int it = 0; it < 20; ++it) {                         // iterate to the MAP (a 17× jump from the prior needs it)
        const double uPrev = xi[0], cPrev = xi[c];
        const double d = std::pow(10.0, xi[0]);
        const double resid = rangeM - (d + offsetM + xi[c]);
        H[0] = kLn10 * d;
        H[c] = 1.0;
        R = s * s;                                              // symmetric Gaussian core: unbiased for LOS
        const double y = resid - H[0] * (x0[0] - xi[0]) - H[c] * (x0[c] - xi[c]);
        double PHt[kMax];
        for (int i = 0; i < nx; ++i) PHt[i] = P0[i][0] * H[0] + P0[i][c] * H[c];
        const double HPHt = H[0] * PHt[0] + H[c] * PHt[c];
        double S = HPHt + R;
        z = resid / std::sqrt(S);
        if (z > 2.0) {                                          // late arrival (NLOS): redescending weight + NLOS variance
            R = (s * s + kSigmaNlos * kSigmaNlos) * (z / 2.0) * (z / 2.0);
            S = HPHt + R;
        } else if (z < -3.0) {                                  // early arrival: plain Huber
            R = R * (-z / 3.0);
            S = HPHt + R;
        }
        for (int i = 0; i < nx; ++i) K[i] = PHt[i] / S;
        // Schmidt–Kalman: the RTT offset error keeps its uncertainty (in P, via Joseph) but its mean is
        // never estimated. Estimating it lets the log-domain linearisation drift leak into the distance.
        K[c] = 0.0;
        for (int i = 0; i < nx; ++i) xi[i] = x0[i] + K[i] * y;
        xi[0] = std::max(-2.0, std::min(4.0, xi[0]));
        if (std::fabs(xi[0] - uPrev) < 1e-10 && std::fabs(xi[c] - cPrev) < 1e-10) break;
    }
    for (int i = 0; i < nx; ++i) { x[i] = xi[i]; for (int j = 0; j < nx; ++j) P[i][j] = P0[i][j]; }
    joseph(nx, P, K, H, R);
    x[0] = std::max(-2.0, std::min(4.0, x[0]));
    return z;
}

double RangeFilter::updateLogRange(double u, double sigmaU)
{
    double H[kMax] = {1, 0, 0, 0, 0};
    return scalarUpdate(H, u - x[0], sigmaU * sigmaU, 2.5);
}

static void resetState(int nx, double *x, double P[RangeFilter::kMax][RangeFilter::kMax], int i, double value, double var)
{
    x[i] = value;
    for (int j = 0; j < nx; ++j) { P[i][j] = 0; P[j][i] = 0; }
    P[i][i] = var;
}
void RangeFilter::resetOffset(int k, double value, double var)
{
    if (k < 0 || 1 + k >= nx - 1) return;
    resetState(nx, x, P, 1 + k, value, var);
}
void RangeFilter::resetRttOffset(double value, double var) { resetState(nx, x, P, nx - 1, value, var); }

double RangeFilter::distanceM() const { return std::pow(10.0, x[0]); }
double RangeFilter::sigmaM() const { return kLn10 * std::pow(10.0, x[0]) * std::sqrt(P[0][0]); }
double RangeFilter::lowM() const { return std::pow(10.0, x[0] - std::sqrt(P[0][0])); }
double RangeFilter::highM() const { return std::pow(10.0, x[0] + std::sqrt(P[0][0])); }

// ── RLS / batch fit ──────────────────────────────────────────────────────────
Rls2::Rls2(double p0Prior, double nPrior, double varP0, double varN, double lam)
    : p0(p0Prior), n(nPrior), lambda(lam)
{
    S[0][0] = varP0; S[0][1] = 0; S[1][0] = 0; S[1][1] = varN;
}

void Rls2::update(double log10d, double level, double R)
{
    const double h0 = 1.0, h1 = -10.0 * log10d;
    const double Sh0 = S[0][0] * h0 + S[0][1] * h1;
    const double Sh1 = S[1][0] * h0 + S[1][1] * h1;
    const double s = h0 * Sh0 + h1 * Sh1 + R;
    const double K0 = Sh0 / s, K1 = Sh1 / s;
    const double e = level - (p0 * h0 + n * h1);
    p0 += K0 * e;
    n += K1 * e;
    const double a = (S[0][0] - K0 * Sh0) / lambda;
    const double b = (S[0][1] - K0 * Sh1) / lambda;
    const double c = (S[1][0] - K1 * Sh0) / lambda;
    const double d = (S[1][1] - K1 * Sh1) / lambda;
    S[0][0] = a; S[1][1] = d; S[0][1] = S[1][0] = 0.5 * (b + c);
    n = std::max(1.5, std::min(4.5, n));
}

PathLossFit fitPathLoss(const std::vector<CalPoint> &pts, double p0Prior, double nPrior, double varP0, double varN, double noiseDb)
{
    double A00 = 1.0 / varP0, A01 = 0, A11 = 1.0 / varN;
    double b0 = p0Prior / varP0, b1 = nPrior / varN;
    const double wn = 1.0 / (noiseDb * noiseDb);
    for (const CalPoint &p : pts) {
        const double x = std::log10(std::max(p.distM, 0.05));
        const double h1 = -10.0 * x, w = p.weight * wn;
        A00 += w; A01 += w * h1; A11 += w * h1 * h1;
        b0 += w * p.level; b1 += w * h1 * p.level;
    }
    const double det = A00 * A11 - A01 * A01;
    PathLossFit f;
    f.varP0 = A11 / det; f.varN = A00 / det; f.covP0N = -A01 / det;
    f.p0 = f.varP0 * b0 + f.covP0N * b1;
    f.n = f.covP0N * b0 + f.varN * b1;
    f.n = std::max(1.5, std::min(4.5, f.n));
    double sw = 0, sr = 0;
    for (const CalPoint &p : pts) {
        const double r = p.level - modelLevel(f.p0, f.n, p.distM);
        sw += p.weight; sr += p.weight * r * r;
    }
    f.rmsDb = sw > 0 ? std::sqrt(sr / sw) : 0;
    f.used = int(pts.size());
    return f;
}

// ── fingerprint distance ─────────────────────────────────────────────────────
double fingerprintExcessModel(double deltaM)
{
    const double g = 10.0 * kNWifi / (kLn10 * 15.0 * std::sqrt(2.0));
    return 2.0 * kShadowWifi * kShadowWifi * (1.0 - std::exp(-deltaM / kDecorrM)) + g * g * deltaM * deltaM;
}

double fingerprintInvert(double target)
{
    if (!(target > 0)) return 0;
    if (fingerprintExcessModel(200.0) < target) return 200.0;
    double lo = 0, hi = 200.0;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (fingerprintExcessModel(mid) < target) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

Fingerprint fingerprintDistance(const std::vector<DiffPair> &pairs)
{
    Fingerprint fp;
    std::map<QString, std::pair<double, double>> agg;           // group → (Σw, Σw·Δ); std::map = sorted names
    for (const DiffPair &p : pairs) {
        const double w = 1.0 / (p.sigmaA * p.sigmaA + p.sigmaB * p.sigmaB);
        auto &a = agg[p.group];
        a.first += w;
        a.second += w * (p.levelB - p.levelA);
    }
    const int m = int(agg.size());
    fp.groups = m;
    if (m < 3) return fp;
    std::vector<double> d, var;
    for (const auto &kv : agg) { d.push_back(kv.second.second / kv.second.first); var.push_back(1.0 / kv.second.first); }
    fp.gainDb = median(d);
    std::vector<double> e(m), ae(m);
    for (int i = 0; i < m; ++i) { e[i] = d[i] - fp.gainDb; ae[i] = std::fabs(e[i]); }
    if (m >= 5) { const double mad = 1.482602218505602 * median(ae); fp.d2 = mad * mad; }
    else { double s = 0; for (int i = 0; i < m; ++i) s += e[i] * e[i]; fp.d2 = s / m; }
    double nz = 0;
    for (int i = 0; i < m; ++i) nz += var[i] + kDevGain * kDevGain;
    fp.noise2 = nz / m;
    fp.excess = std::max(0.0, fp.d2 - fp.noise2);
    fp.deltaM = fingerprintInvert(fp.excess);
    const double d2lo = fp.d2 * m / chi2Quantile(m, kZ84);
    const double q16 = chi2Quantile(m, -kZ84);
    const double d2hi = q16 > 0 ? fp.d2 * m / q16 : 1e12;
    fp.lowM = fingerprintInvert(std::max(0.0, d2lo - fp.noise2));
    fp.highM = fingerprintInvert(std::max(0.0, d2hi - fp.noise2));
    fp.ok = true;
    return fp;
}

double fingerprintLogLik(const Fingerprint &fp, double r)
{
    const double s = fingerprintExcessModel(r) + fp.noise2;
    const double m = fp.groups;
    return -(m / 2.0) * std::log(s) - m * fp.d2 / (2.0 * s);
}

// ── geometric differential solve ─────────────────────────────────────────────
static bool inv3(const double A[3][3], double out[3][3])
{
    const double c00 = A[1][1] * A[2][2] - A[1][2] * A[2][1];
    const double c01 = A[1][2] * A[2][0] - A[1][0] * A[2][2];
    const double c02 = A[1][0] * A[2][1] - A[1][1] * A[2][0];
    const double det = A[0][0] * c00 + A[0][1] * c01 + A[0][2] * c02;
    if (!(std::fabs(det) > 1e-300)) return false;
    out[0][0] = c00 / det;
    out[0][1] = (A[0][2] * A[2][1] - A[0][1] * A[2][2]) / det;
    out[0][2] = (A[0][1] * A[1][2] - A[0][2] * A[1][1]) / det;
    out[1][0] = c01 / det;
    out[1][1] = (A[0][0] * A[2][2] - A[0][2] * A[2][0]) / det;
    out[1][2] = (A[0][2] * A[1][0] - A[0][0] * A[1][2]) / det;
    out[2][0] = c02 / det;
    out[2][1] = (A[0][1] * A[2][0] - A[0][0] * A[2][1]) / det;
    out[2][2] = (A[0][0] * A[1][1] - A[0][1] * A[1][0]) / det;
    return true;
}

GeoSolve solveDifferential(const std::vector<GeoPair> &pairs)
{
    GeoSolve g;
    std::vector<GeoPair> use;
    for (const GeoPair &p : pairs) if (std::sqrt(p.apE * p.apE + p.apN * p.apN) >= 1.0) use.push_back(p);
    const int m = int(use.size());
    g.used = m;
    std::vector<double> br;
    for (const GeoPair &p : use) { double b = std::atan2(p.apE, p.apN) * 180.0 / M_PI; if (b < 0) b += 360.0; br.push_back(b); }
    std::sort(br.begin(), br.end());
    double maxGap = 360.0;
    if (m >= 2) {
        maxGap = br[0] + 360.0 - br[m - 1];
        for (int i = 1; i < m; ++i) maxGap = std::max(maxGap, br[i] - br[i - 1]);
    }
    g.spreadDeg = m >= 2 ? 360.0 - maxGap : 0.0;
    if (m < 3 || g.spreadDeg < 60.0) return g;
    std::vector<double> dl;
    for (const GeoPair &p : use) dl.push_back(p.delta);
    double th[3] = {0.0, 0.0, median(dl)};
    auto accumulate = [&](double N[3][3], double gv[3], double &chi2) {
        for (int i = 0; i < 3; ++i) { gv[i] = 0; for (int j = 0; j < 3; ++j) N[i][j] = 0; }
        chi2 = 0;
        const double dist = std::sqrt(th[0] * th[0] + th[1] * th[1]);
        for (const GeoPair &p : use) {
            const double dx = th[0] - p.apE, dy = th[1] - p.apN;
            const double dB = std::max(0.5, std::sqrt(dx * dx + dy * dy));
            const double dA = std::sqrt(p.apE * p.apE + p.apN * p.apN);
            const double pred = -10.0 * p.pathloss * std::log10(dB / dA) + th[2];
            const double r = p.delta - pred;
            const double k = -10.0 * p.pathloss / kLn10;
            const double J[3] = {k * dx / (dB * dB), k * dy / (dB * dB), 1.0};
            const double grad = 10.0 * p.pathloss / (kLn10 * dB);
            const double var = p.sigmaDelta * p.sigmaDelta + kDevGain * kDevGain
                             + 2.0 * kShadowWifi * kShadowWifi * (1.0 - std::exp(-dist / kDecorrM))
                             + (grad * p.sigmaPos) * (grad * p.sigmaPos);
            const double w = 1.0 / var;
            for (int i = 0; i < 3; ++i) { gv[i] += w * J[i] * r; for (int j = 0; j < 3; ++j) N[i][j] += w * J[i] * J[j]; }
            chi2 += w * r * r;
        }
    };
    double N[3][3], gv[3], chi2 = 0;
    for (int it = 0; it < 20; ++it) {
        accumulate(N, gv, chi2);
        double D[3][3], Di[3][3];
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) D[i][j] = N[i][j] + (i == j ? 1e-3 * N[i][i] : 0.0);
        if (!inv3(D, Di)) return g;
        double step[3];
        for (int i = 0; i < 3; ++i) step[i] = Di[i][0] * gv[0] + Di[i][1] * gv[1] + Di[i][2] * gv[2];
        for (int i = 0; i < 3; ++i) th[i] += step[i];
        g.iterations = it + 1;
        if (std::fabs(step[0]) < 1e-6 && std::fabs(step[1]) < 1e-6 && std::fabs(step[2]) < 1e-6) break;
    }
    accumulate(N, gv, chi2);
    double Ni[3][3];
    if (!inv3(N, Ni)) return g;
    const double scale = m > 3 ? std::max(1.0, chi2 / (m - 3)) : 1.0;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) g.cov[i][j] = Ni[i][j] * scale;
    g.dE = th[0]; g.dN = th[1]; g.gainDb = th[2]; g.chi2 = chi2;
    g.ok = true;
    return g;
}

// ── relative posterior ───────────────────────────────────────────────────────
QString classify(bool evidence, double lowM, double highM)
{
    if (!evidence) return QStringLiteral("unknown");
    if (highM <= 2.0) return QStringLiteral("adjacent");
    if (highM <= 6.0) return QStringLiteral("room");
    if (lowM > 30.0) return QStringLiteral("far");
    if (highM <= 30.0) return QStringLiteral("near");
    return QStringLiteral("unknown");
}

static double gaussLog(const Gauss2 &g, double e, double n)
{
    const double det = g.sEE * g.sNN - g.sEN * g.sEN;
    if (!(det > 0)) return 0;
    const double de = e - g.muE, dn = n - g.muN;
    const double q = (g.sNN * de * de - 2.0 * g.sEN * de * dn + g.sEE * dn * dn) / det;
    return -0.5 * q;
}

RelOutput relativePosterior(const RelInput &in)
{
    RelOutput out;
    const bool evidence = in.haveRange || (in.haveFp && in.fp.ok) || in.fix.valid || in.geo.valid;
    if (!evidence) return out;
    constexpr int NR = 64, NB = 180;
    double uLo, uHi;
    if (in.haveRange) {
        const double sd = std::sqrt(in.puu);
        uLo = in.u - 4.0 * sd; uHi = in.u + 4.0 * sd;
    } else {
        double rmax = 200.0;
        if (in.fix.valid) rmax = std::max(rmax, std::sqrt(in.fix.muE * in.fix.muE + in.fix.muN * in.fix.muN) + 4.0 * std::sqrt(std::max(in.fix.sEE, in.fix.sNN)));
        if (in.geo.valid) rmax = std::max(rmax, std::sqrt(in.geo.muE * in.geo.muE + in.geo.muN * in.geo.muN) + 4.0 * std::sqrt(std::max(in.geo.sEE, in.geo.sNN)));
        uLo = std::log10(0.05); uHi = std::log10(rmax);
    }
    uLo = std::max(uLo, -2.0); uHi = std::min(uHi, std::log10(20000.0));
    if (!(uHi > uLo)) uHi = uLo + 1e-3;
    const double du = (uHi - uLo) / (NR - 1);
    std::vector<double> logwv(size_t(NR) * NB);
    auto logw = [&](int i, int k) -> double & { return logwv[size_t(i) * NB + k]; };
    double maxLog = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < NR; ++i) {
        const double ui = uLo + i * du, r = std::pow(10.0, ui);
        double base = in.haveRange ? -0.5 * (ui - in.u) * (ui - in.u) / in.puu : 2.0 * std::log(r);
        if (in.haveFp && in.fp.ok) base += fingerprintLogLik(in.fp, r);
        for (int k = 0; k < NB; ++k) {
            const double th = 2.0 * k * M_PI / 180.0;
            const double e = r * std::sin(th), n = r * std::cos(th);
            double lw = base;
            if (in.geo.valid) lw += gaussLog(in.geo, e, n);
            if (in.fix.valid) lw += gaussLog(in.fix, e, n);
            logw(i, k) = lw;
            if (lw > maxLog) maxLog = lw;
        }
    }
    double Wr[NR], W = 0, C = 0, S = 0;
    for (int i = 0; i < NR; ++i) {
        double wi = 0;
        for (int k = 0; k < NB; ++k) {
            const double w = std::exp(logw(i, k) - maxLog);
            const double th = 2.0 * k * M_PI / 180.0;
            wi += w; C += w * std::cos(th); S += w * std::sin(th);
        }
        Wr[i] = wi; W += wi;
    }
    auto uq = [&](double p) {
        const double target = p * W;
        double cum = 0;
        for (int i = 0; i < NR; ++i) {
            if (cum + Wr[i] >= target && Wr[i] > 0) {
                const double frac = (target - cum) / Wr[i];
                return (uLo + i * du - 0.5 * du) + frac * du;
            }
            cum += Wr[i];
        }
        return uHi + 0.5 * du;
    };
    out.distanceM = std::pow(10.0, uq(0.5));
    out.lowM = std::pow(10.0, uq(0.16));
    out.highM = std::pow(10.0, uq(0.84));
    out.sigmaM = 0.5 * (out.highM - out.lowM);
    out.resultantLength = std::sqrt(C * C + S * S) / W;
    if (out.resultantLength >= 0.5) {
        double b = std::atan2(S, C) * 180.0 / M_PI;
        if (b < 0) b += 360.0;
        out.haveBearing = true;
        out.bearingDeg = b;
        out.bearingSigmaDeg = std::sqrt(-2.0 * std::log(out.resultantLength)) * 180.0 / M_PI;
    }
    out.cls = classify(true, out.lowM, out.highM);
    out.valid = true;
    return out;
}

// ── BLE advertisement format (§9.1) ──────────────────────────────────────────
QByteArray bleTag(const QString &identityId, qint64 unixSeconds)
{
    const qint64 w = unixSeconds >= 0 ? unixSeconds / 900 : -((-unixSeconds + 899) / 900);
    const QByteArray msg = QStringLiteral("beaconfix-ble-v1|%1|%2").arg(identityId).arg(w).toUtf8();
    return QCryptographicHash::hash(msg, QCryptographicHash::Sha256).left(8);
}
int bleFlags(bool rtt, bool api, int kind, bool calibrating)
{
    return (rtt ? 1 : 0) | (api ? 2 : 0) | ((kind & 7) << 2) | (calibrating ? 0x20 : 0);
}
QByteArray bleServiceData(const QByteArray &tag, int txPower, int flags)
{
    QByteArray d = tag.left(8);
    while (d.size() < 8) d.append(char(0));
    d.append(char(qint8(std::max(-128, std::min(127, txPower)))));
    d.append(char(flags & 0xFF));
    return d;
}
BleAdvert parseServiceData(const QByteArray &data)
{
    BleAdvert a;
    if (data.size() < 10) return a;
    a.tag = data.left(8);
    a.txPower = int(qint8(data[8]));
    const int f = int(quint8(data[9]));
    a.rtt = f & 1; a.api = f & 2; a.kind = (f >> 2) & 7; a.calibrating = f & 0x20; a.version = (f >> 6) & 3;
    a.valid = a.version == 0;
    return a;
}

} // namespace RangeMath
