// The estimation engine (see estimator.h and docs/GRADING.md). Pure and deterministic: the Kotlin
// twin (android/app/src/main/java/org/sworrl/beaconfix/estimate/Estimator.kt) is a line-by-line
// port, checked against tests/fixtures/estimator_golden.json. Change both or neither.
#include "estimator.h"
#include <QSet>
#include <vector>

namespace Estimator {

namespace {
constexpr double LN10 = 2.302585092994046;
constexpr double ACC68 = 1.515;              // 68 % radius → per-axis σ of a circular normal
constexpr double CHI2_2_999 = 13.815510557964274;

struct Cl {                                   // one place (cluster of samples)
    double x = 0, y = 0;                      // local metres
    double level = 0;                         // median corrected level (dBm)
    double W = 1;                             // mean age/status weight
    double a = 10;                            // per-axis fix σ (m)
    double sh2 = 36;                          // shadowing variance of the aggregated level
    int    m = 0;
    double fading = 0;
    double sx = 0, sy = 0;                    // seed position (clustering)
};
struct Rg { double x = 0, y = 0, r = 0, sd2 = 1; };
struct Theta { double x = 0, y = 0, p0 = -40, n = 2.4; };

double median(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t h = v.size() / 2;
    return v.size() % 2 ? v[h] : 0.5 * (v[h - 1] + v[h]);
}

double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// 2×2 symmetric eigen-decomposition: λ1 ≥ λ2, e1 the unit eigenvector of λ1
void eig2(double cxx, double cxy, double cyy, double *l1, double *l2, double *ex, double *ey)
{
    const double tr = cxx + cyy, det = cxx * cyy - cxy * cxy;
    const double disc = std::sqrt(std::max(0.0, tr * tr / 4 - det));
    *l1 = tr / 2 + disc; *l2 = tr / 2 - disc;
    if (std::fabs(cxy) > 1e-12) {
        const double vx = *l1 - cyy, vy = cxy, nn = std::sqrt(vx * vx + vy * vy);
        *ex = vx / nn; *ey = vy / nn;
    } else if (cxx >= cyy) { *ex = 1; *ey = 0; }
    else { *ex = 0; *ey = 1; }
}

// C ← the larger of C and D along C's principal axes
void eigMax(double C[3], const double D[3])
{
    double l1, l2, ex, ey;
    eig2(C[0], C[1], C[2], &l1, &l2, &ex, &ey);
    const double fx = -ey, fy = ex;
    const double d1 = ex * ex * D[0] + 2 * ex * ey * D[1] + ey * ey * D[2];
    const double d2 = fx * fx * D[0] + 2 * fx * fy * D[1] + fy * fy * D[2];
    l1 = std::max(l1, d1); l2 = std::max(l2, d2);
    C[0] = l1 * ex * ex + l2 * fx * fx;
    C[1] = l1 * ex * ey + l2 * fx * fy;
    C[2] = l1 * ey * ey + l2 * fy * fy;
}

bool invert4(const double A[4][4], int k, double out[4][4])
{
    for (int col = 0; col < k; ++col) {
        double Ac[4][4]; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) Ac[i][j] = A[i][j];
        double e[4] = {0, 0, 0, 0}; e[col] = 1; double o[4];
        if (!solve(Ac, e, k, o)) return false;
        for (int i = 0; i < k; ++i) out[i][col] = o[i];
    }
    return true;
}

// ── the model at one cluster ──
struct At { double d, rho, ell, s2, r, J[4]; };
At evalAt(const Cl &c, const Theta &t, double h)
{
    At a;
    const double dx = t.x - c.x, dy = t.y - c.y;
    a.rho = std::sqrt(dx * dx + dy * dy);
    a.d = std::max(1.0, std::sqrt(a.rho * a.rho + h * h));
    a.ell = -10.0 * std::log10(a.d);
    const double b = 10.0 * t.n / LN10;
    const double grad = b * a.rho / (a.d * a.d);             // |∂model/∂p|
    a.s2 = c.sh2 + grad * grad * c.a * c.a;
    a.r = c.level - (t.p0 + t.n * a.ell);
    a.J[0] = b * dx / (a.d * a.d); a.J[1] = b * dy / (a.d * a.d); a.J[2] = -1.0; a.J[3] = -a.ell;
    return a;
}

struct Solver {
    const std::vector<Cl> &cl;
    const std::vector<Rg> &rg;
    const Options &o;
    std::vector<double> mult;                 // per-cluster multiplicity (jackknife/bootstrap)
    double tau = 1;
    bool robust = false;
    Solver(const std::vector<Cl> &c, const std::vector<Rg> &r, const Options &op) : cl(c), rg(r), o(op), mult(c.size(), 1.0) {}

    // Robust: a Gaussian + uniform outlier mixture (the EM responsibilities are the IRLS weights), so a gross
    // outlier costs a constant instead of growing without bound; otherwise plain least squares.
    double outlierDensity() const { return o.outlierPrior * std::min(1.0, o.sigmaDb * tau / o.outlierSpanDb); }
    double rhoFn(double z) const
    {
        if (!robust) return 0.5 * z * z;
        const double c = outlierDensity(), k = (1 - o.outlierPrior) / std::sqrt(2 * M_PI);
        return -std::log(k * std::exp(-0.5 * z * z) + c) + std::log(k + c);
    }
    double uFn(double z) const
    {
        if (!robust) return 1.0;
        const double c = outlierDensity(), g = (1 - o.outlierPrior) / std::sqrt(2 * M_PI) * std::exp(-0.5 * z * z);
        return g / (g + c);
    }

    double cost(const Theta &t) const
    {
        double c = 0;
        for (size_t k = 0; k < cl.size(); ++k) {
            if (mult[k] <= 0) continue;
            const At a = evalAt(cl[k], t, o.heightM);
            c += mult[k] * cl[k].W * rhoFn(a.r / (std::sqrt(a.s2) * tau));
        }
        for (const Rg &g : rg) {
            const double dx = t.x - g.x, dy = t.y - g.y, d = std::sqrt(dx * dx + dy * dy + o.heightM * o.heightM);
            c += rhoFn((g.r - d) / (std::sqrt(g.sd2) * tau));
        }
        const double zp = (t.p0 - o.p0Mean) / o.p0Sd, zn = (t.n - o.defaultN) / o.nSd;
        return c + 0.5 * zp * zp + 0.5 * zn * zn;
    }

    // Normal equations at t: A = Σψ JᵀJ + prior, g = Σψ J r + prior gradient
    void normal(const Theta &t, double tauUse, double A[4][4], double g[4]) const
    {
        for (int i = 0; i < 4; ++i) { g[i] = 0; for (int j = 0; j < 4; ++j) A[i][j] = 0; }
        for (size_t k = 0; k < cl.size(); ++k) {
            if (mult[k] <= 0) continue;
            const At a = evalAt(cl[k], t, o.heightM);
            const double z = a.r / (std::sqrt(a.s2) * tauUse);
            const double psi = mult[k] * cl[k].W * uFn(z) / (a.s2 * tauUse * tauUse);
            for (int i = 0; i < 4; ++i) { g[i] += psi * a.J[i] * a.r; for (int j = 0; j < 4; ++j) A[i][j] += psi * a.J[i] * a.J[j]; }
        }
        for (const Rg &gg : rg) {
            const double dx = t.x - gg.x, dy = t.y - gg.y, d = std::sqrt(dx * dx + dy * dy + o.heightM * o.heightM);
            const double r = gg.r - d, z = r / (std::sqrt(gg.sd2) * tauUse);
            const double psi = uFn(z) / (gg.sd2 * tauUse * tauUse);
            const double J0 = -dx / d, J1 = -dy / d;
            A[0][0] += psi * J0 * J0; A[0][1] += psi * J0 * J1; A[1][0] += psi * J0 * J1; A[1][1] += psi * J1 * J1;
            g[0] += psi * J0 * r; g[1] += psi * J1 * r;
        }
        A[2][2] += 1.0 / (o.p0Sd * o.p0Sd); g[2] += (t.p0 - o.p0Mean) / (o.p0Sd * o.p0Sd);
        A[3][3] += 1.0 / (o.nSd * o.nSd);   g[3] += (t.n - o.defaultN) / (o.nSd * o.nSd);
    }

    Theta run(Theta t, int iters, double *costOut) const
    {
        double lambda = 1e-2;
        for (int it = 0; it < iters; ++it) {
            double A[4][4], g[4];
            normal(t, tau, A, g);
            const double c0 = cost(t);
            for (int i = 0; i < 4; ++i) A[i][i] *= (1.0 + lambda);
            double rhs[4] = {-g[0], -g[1], -g[2], -g[3]}, dx[4];
            if (!solve(A, rhs, 4, dx)) break;
            const double step = std::sqrt(dx[0] * dx[0] + dx[1] * dx[1]);
            if (step > 300.0) { dx[0] *= 300.0 / step; dx[1] *= 300.0 / step; }
            Theta nt{t.x + dx[0], t.y + dx[1], std::clamp(t.p0 + dx[2], -90.0, 10.0), std::clamp(t.n + dx[3], 1.5, 4.5)};
            const double c1 = cost(nt);
            if (c1 <= c0) {
                const bool small = std::sqrt(dx[0] * dx[0] + dx[1] * dx[1]) < 0.05 && c0 - c1 < 1e-7 * std::max(1.0, c0);
                t = nt; lambda = std::max(1e-6, lambda / 3);
                if (small) break;
            } else {
                lambda *= 8;
                if (lambda > 1e6) break;
            }
        }
        if (costOut) *costOut = cost(t);
        return t;
    }
};

// Closed-form marginal log-likelihood of a position with (P0, n) integrated out (Gaussian prior), plus
// ranges and misses. Returns the posterior mean of (P0, n) at that position too.
double gridLogL(const std::vector<Cl> &cl, const std::vector<Rg> &rg, const QList<Miss> &misses, const std::vector<double> &mxs,
                const std::vector<double> &mys, double px, double py, const Options &o, double *b0, double *b1)
{
    const double bn = 10.0 * o.defaultN / LN10;
    double S00 = 0, S01 = 0, S11 = 0, T0 = 0, T1 = 0, Syy = 0, logdet = 0;
    for (const Cl &c : cl) {
        const double dx = px - c.x, dy = py - c.y, rho = std::sqrt(dx * dx + dy * dy);
        const double d = std::max(1.0, std::sqrt(rho * rho + o.heightM * o.heightM)), ell = -10.0 * std::log10(d);
        const double grad = bn * rho / (d * d), s2 = c.sh2 + grad * grad * c.a * c.a;
        const double w = c.W / s2;
        S00 += w; S01 += w * ell; S11 += w * ell * ell; T0 += w * c.level; T1 += w * ell * c.level; Syy += w * c.level * c.level;
        logdet += std::log(s2 / c.W);
    }
    const double iP = 1.0 / (o.p0Sd * o.p0Sd), iN = 1.0 / (o.nSd * o.nSd);
    const double L00 = S00 + iP, L01 = S01, L11 = S11 + iN;
    const double e0 = T0 + o.p0Mean * iP, e1 = T1 + o.defaultN * iN;
    const double det = L00 * L11 - L01 * L01;
    const double m0 = (L11 * e0 - L01 * e1) / det, m1 = (L00 * e1 - L01 * e0) / det;
    const double quad = Syy + o.p0Mean * o.p0Mean * iP + o.defaultN * o.defaultN * iN - (e0 * m0 + e1 * m1);
    double ll = -0.5 * quad - 0.5 * std::log(det) - 0.5 * logdet;
    for (const Rg &g : rg) {
        const double dx = px - g.x, dy = py - g.y, d = std::sqrt(dx * dx + dy * dy + o.heightM * o.heightM);
        ll += -0.5 * (g.r - d) * (g.r - d) / g.sd2;
    }
    for (int j = 0; j < misses.size(); ++j) {
        const double dx = px - mxs[j], dy = py - mys[j], d = std::max(1.0, std::sqrt(dx * dx + dy * dy + o.heightM * o.heightM));
        const double mu = m0 + m1 * (-10.0 * std::log10(d));
        const double pDet = (1.0 - o.missFloor) * normCdf((mu - o.sensitivity) / o.sigmaDb);
        ll += std::min(misses[j].count, 3) * std::log(std::max(1e-12, 1.0 - pDet));
    }
    if (b0) *b0 = m0;
    if (b1) *b1 = m1;
    return ll;
}

double chi2Quantile99(int dof)                // Wilson–Hilferty
{
    if (dof < 1) dof = 1;
    const double k = dof, z = 2.3263478740408408, t = 1 - 2 / (9 * k) + z * std::sqrt(2 / (9 * k));
    return k * t * t * t;
}

void ranks(const std::vector<double> &v, std::vector<double> *out)
{
    const int n = int(v.size());
    std::vector<int> idx(n); for (int i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return v[a] < v[b] || (v[a] == v[b] && a < b); });
    out->assign(n, 0);
    int i = 0;
    while (i < n) {
        int j = i;
        while (j + 1 < n && v[idx[j + 1]] == v[idx[i]]) ++j;
        const double r = 0.5 * (i + j) + 1;
        for (int q = i; q <= j; ++q) (*out)[idx[q]] = r;
        i = j + 1;
    }
}

double pearson(const std::vector<double> &a, const std::vector<double> &b)
{
    const int n = int(a.size());
    double ma = 0, mb = 0; for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < n; ++i) { sab += (a[i] - ma) * (b[i] - mb); saa += (a[i] - ma) * (a[i] - ma); sbb += (b[i] - mb) * (b[i] - mb); }
    return saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : 0;
}

bool pointInHull(const std::vector<Cl> &cl, double px, double py)
{
    const int n = int(cl.size());
    if (n < 3) return false;
    std::vector<int> idx(n); for (int i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return cl[a].x < cl[b].x || (cl[a].x == cl[b].x && (cl[a].y < cl[b].y || (cl[a].y == cl[b].y && a < b))); });
    auto cross = [&](int o, int a, int b) { return (cl[a].x - cl[o].x) * (cl[b].y - cl[o].y) - (cl[a].y - cl[o].y) * (cl[b].x - cl[o].x); };
    std::vector<int> h(2 * n);
    int k = 0;
    for (int i = 0; i < n; ++i) { while (k >= 2 && cross(h[k - 2], h[k - 1], idx[i]) <= 0) --k; h[k++] = idx[i]; }
    for (int i = n - 2, t = k + 1; i >= 0; --i) { while (k >= t && cross(h[k - 2], h[k - 1], idx[i]) <= 0) --k; h[k++] = idx[i]; }
    const int m = k - 1;
    if (m < 3) return false;
    double area = 0;
    for (int i = 0; i < m; ++i) { const Cl &a = cl[h[i]], &b = cl[h[i + 1]]; area += a.x * b.y - b.x * a.y; }
    if (std::fabs(area) < 1.0) return false;                  // collinear: no inside
    for (int i = 0; i < m; ++i) {
        const Cl &a = cl[h[i]], &b = cl[h[i + 1]];
        if ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x) < -1e-9) return false;
    }
    return true;
}

void setEllipse(Fit &f, const double C[3])
{
    f.cxx = C[0]; f.cxy = C[1]; f.cyy = C[2];
    ellipse(C[0], C[1], C[2], &f.semiMajor, &f.semiMinor, &f.orientDeg);
    f.acc = std::max(8.0, f.semiMajor);
    f.r95 = radiusFor(f.semiMajor, f.semiMinor, 0.95);
    f.cep50 = radiusFor(f.semiMajor, f.semiMinor, 0.5);
    f.pWithin25 = probWithin(f.semiMajor, f.semiMinor, 25.0);
}

double freshness(qint64 newest, qint64 now)
{
    if (now <= 0 || newest <= 0) return 1.0;
    const double days = std::max(0.0, double(now - newest) / 86400.0);
    return std::max(0.3, std::pow(0.5, days / 180.0));
}

double precisionComp(double r95) { return clamp01(std::log(300.0 / std::max(1e-6, r95)) / std::log(30.0)); }

double nisFactor(double nis) { return nis > 0 ? std::exp(-std::max(0.0, nis - 3.0) / 3.0) : 1.0; }

} // namespace

// ── helpers ──────────────────────────────────────────────────────────────────
bool solve(double A[4][4], double b[4], int k, double out[4])
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

bool invert2(double a, double b, double d, double out[3])
{
    const double det = a * d - b * b;
    if (std::fabs(det) < 1e-12) return false;
    out[0] = d / det; out[1] = -b / det; out[2] = a / det;
    return true;
}

void ellipse(double cxx, double cxy, double cyy, double *major, double *minor, double *orientDeg)
{
    const double tr = cxx + cyy, det = cxx * cyy - cxy * cxy;
    const double disc = std::sqrt(std::max(0.0, tr * tr / 4 - det));
    const double l1 = std::max(0.0, tr / 2 + disc), l2 = std::max(0.0, tr / 2 - disc);
    *major = std::sqrt(l1); *minor = std::sqrt(l2);
    *orientDeg = std::atan2(l1 - cxx, cxy) * 180.0 / M_PI;
    if (std::fabs(cxy) < 1e-12) *orientDeg = cxx >= cyy ? 90.0 : 0.0;   // x = east: major axis along east → 90° bearing
}

// erfc with fractional error < 1.2e-7 (Numerical Recipes erfcc): identical in the Kotlin twin
double normCdf(double z)
{
    const double x = -z / std::sqrt(2.0);
    const double t = 1.0 / (1.0 + 0.5 * std::fabs(x));
    const double r = t * std::exp(-x * x - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 + t * (-0.18628806 + t * (0.27886807
                     + t * (-1.13520398 + t * (1.48851587 + t * (-0.82215223 + t * 0.17087277)))))))));
    const double erfc = x >= 0 ? r : 2.0 - r;
    return 0.5 * erfc;
}

double probWithin(double s1, double s2, double r)
{
    // P = ∫ φ(x/σa)/σa · (2Φ(√(r²−x²)/σb) − 1) dx over |x| < r, σa ≥ σb, with x = r·sin t (Simpson, 256 intervals)
    if (r <= 0) return 0;
    const double sa = std::max(1e-9, std::max(s1, s2)), sb = std::max(1e-9, std::min(s1, s2));
    const int N = 256;
    const double h = M_PI / N;
    double sum = 0;
    for (int i = 0; i <= N; ++i) {
        const double t = -M_PI / 2 + i * h, x = r * std::sin(t), ct = std::cos(t);
        const double fv = std::exp(-0.5 * (x / sa) * (x / sa)) / (sa * std::sqrt(2 * M_PI)) * (2 * normCdf(r * ct / sb) - 1) * r * ct;
        sum += (i == 0 || i == N) ? fv : (i % 2 ? 4 * fv : 2 * fv);
    }
    return std::clamp(sum * h / 3, 0.0, 1.0);
}

double radiusFor(double s1, double s2, double p)
{
    double lo = 0, hi = 5.0 * std::max(1e-6, std::max(s1, s2));
    for (int i = 0; i < 50; ++i) { const double mid = 0.5 * (lo + hi); if (probWithin(s1, s2, mid) < p) lo = mid; else hi = mid; }
    return 0.5 * (lo + hi);
}

QString letterFor(double score)
{
    return score >= 85 ? QStringLiteral("A") : score >= 70 ? QStringLiteral("B") : score >= 55 ? QStringLiteral("C")
         : score >= 40 ? QStringLiteral("D") : score >= 20 ? QStringLiteral("E") : QStringLiteral("F");
}

static double lowerBound(const QString &l)
{
    return l == QLatin1String("A") ? 85 : l == QLatin1String("B") ? 70 : l == QLatin1String("C") ? 55 : l == QLatin1String("D") ? 40 : l == QLatin1String("E") ? 20 : 0;
}

QString qualityFor(const QString &g)
{
    if (g == QLatin1String("A") || g == QLatin1String("B")) return QStringLiteral("good");
    if (g == QLatin1String("C") || g == QLatin1String("D")) return QStringLiteral("fair");
    if (g == QLatin1String("E") || g == QLatin1String("F") || g == QLatin1String("R")) return QStringLiteral("poor");
    return QStringLiteral("none");
}

int vantageCount(const QList<Obs> &obs, double clusterMinM)
{
    if (obs.isEmpty()) return 0;
    std::vector<double> accs; for (const Obs &o : obs) accs.push_back(o.acc);
    const double R = std::max(clusterMinM, median(accs));
    const Frame fr(obs[0].lat, obs[0].lon);
    std::vector<std::pair<double, double>> seeds;
    for (const Obs &o : obs) {
        const double x = fr.x(o.lon), y = fr.y(o.lat);
        bool found = false;
        for (const auto &s : seeds) if (std::hypot(x - s.first, y - s.second) < R) { found = true; break; }
        if (!found) seeds.push_back({x, y});
    }
    return int(seeds.size());
}

// ── grading ──────────────────────────────────────────────────────────────────
void grade(Fit &f, const Fit *prev)
{
    if (f.kind == QLatin1String("mobile")) { f.score = 0; f.grade = QStringLiteral("M"); f.pendingGrade.clear(); f.quality = QStringLiteral("none"); return; }
    if (!f.valid) { f.score = 0; f.grade.clear(); f.pendingGrade.clear(); f.quality = QStringLiteral("none"); return; }
    if (f.kind == QLatin1String("region")) {
        f.score = std::min(39.0, 100.0 * f.cP);
        f.grade = QStringLiteral("R"); f.pendingGrade.clear(); f.quality = QStringLiteral("poor");
        return;
    }
    const double comps[7] = {f.cP, f.cG, f.cE, f.cF, f.cS, f.cT, f.cX};
    const double w[7] = {0.30, 0.20, 0.15, 0.15, 0.10, 0.05, 0.05};
    double sw = 0, sl = 0;
    for (int i = 0; i < 7; ++i) {
        if (i == 6 && f.cX < 0) continue;
        sw += w[i]; sl += w[i] * std::log(std::max(1e-3, comps[i]));
    }
    double score = 100.0 * std::exp(sl / sw);
    if (f.ambiguous || f.modes >= 2) score = std::min(score, 49.0);
    if (!f.inHull && f.linRatio < 0.02) score = std::min(score, 59.0);
    f.score = score;
    const QString raw = letterFor(score);
    QString letter = raw; f.pendingGrade.clear();
    if (prev && prev->valid && prev->kind == QLatin1String("fix") && !prev->grade.isEmpty() && prev->grade != QLatin1String("R")
        && prev->grade != QLatin1String("M") && raw != prev->grade) {
        const bool better = lowerBound(raw) > lowerBound(prev->grade);
        const bool clear = better ? score >= lowerBound(raw) + 3 : score <= lowerBound(prev->grade) - 3;
        if (!clear && prev->pendingGrade != raw) { letter = prev->grade; f.pendingGrade = raw; }
    }
    f.grade = letter;
    f.quality = qualityFor(letter);
}

// ── the AP fitter ────────────────────────────────────────────────────────────
Fit fitAp(const QList<Obs> &samples, qint64 now, const Options &opt, const Context &ctx)
{
    Fit f; f.n = samples.size(); f.pathloss = opt.defaultN; f.p0 = opt.p0Mean; f.updated = now;
    if (ctx.mobile) {
        f.kind = QStringLiteral("mobile");
        if (!samples.isEmpty()) { f.lat = samples.last().lat; f.lon = samples.last().lon; }
        grade(f, nullptr);
        return f;
    }
    // Usable samples: precise enough; coarse ones only when nothing better exists
    QList<Obs> use;
    for (const Obs &o : samples) if (o.acc > 0 && o.acc <= opt.geomAcc) use << o;
    if (use.isEmpty()) for (const Obs &o : samples) if (o.acc > 0 && o.acc <= opt.maxAcc) use << o;
    f.n = use.size();
    if (use.isEmpty()) { grade(f, nullptr); return f; }

    // Did the AP move? Fit the last 30 days against everything older (docs/GRADING.md §3.8)
    if (!ctx.noEpochSplit) {
        qint64 tmin = 0, tmax = 0;
        for (const Obs &o : use) if (o.t > 0) { tmin = tmin == 0 ? o.t : std::min(tmin, o.t); tmax = std::max(tmax, o.t); }
        if (tmax - tmin > 60 * 86400) {
            QList<Obs> recent, old;
            for (const Obs &o : use) ((o.t > 0 && o.t >= tmax - 30 * 86400) ? recent : old) << o;
            if (recent.size() >= 3 && old.size() >= 3) {
                Options q = opt; q.bootstrap = 0; q.jackMaxK = 0;
                Context c2; c2.deviceOffset = ctx.deviceOffset; c2.noEpochSplit = true;
                const Fit a = fitAp(old, now, q, c2), b = fitAp(recent, now, q, c2);
                if (a.kind == QLatin1String("fix") && b.kind == QLatin1String("fix")) {
                    const Frame fa(a.lat, a.lon);
                    const double dx = fa.x(b.lon), dy = fa.y(b.lat);
                    double inv[3];
                    if (invert2(a.cxx + b.cxx, a.cxy + b.cxy, a.cyy + b.cyy, inv)) {
                        const double d2 = dx * dx * inv[0] + 2 * dx * dy * inv[1] + dy * dy * inv[2];
                        if (d2 > CHI2_2_999) {
                            Context c3 = ctx; c3.noEpochSplit = true;
                            Fit r = fitAp(recent, now, opt, c3);
                            r.moved = true;
                            return r;
                        }
                    }
                }
            }
        }
    }

    // ── samples → places ──
    const Frame fr(use[0].lat, use[0].lon);
    struct Sm { double x, y, level, w0, acc; int idx; };
    std::vector<Sm> sm;
    QSet<QString> sessions, devices;
    for (int i = 0; i < use.size(); ++i) {
        const Obs &o = use[i];
        double w0 = 1.0;
        if (now > 0 && o.t > 0) w0 = std::max(0.15, std::exp(-std::max(0.0, double(now - o.t) / 86400.0) / opt.ageTauDays));
        w0 *= std::max(0.05, o.weight);
        sm.push_back({fr.x(o.lon), fr.y(o.lat), double(o.dbm) - ctx.deviceOffset.value(o.device, 0.0), w0, o.acc, i});
        sessions.insert(o.device + QLatin1Char('|') + QString::number(o.t > 0 ? o.t / 86400 : -1));
        devices.insert(o.device);
        f.newest = std::max(f.newest, o.t);
    }
    f.sessions = sessions.size(); f.devices = devices.size();
    std::vector<double> accs; for (const Sm &s : sm) accs.push_back(s.acc);
    double Rc = std::max(opt.clusterMinM, median(accs));
    std::vector<std::vector<int>> members;
    std::vector<Cl> cl;
    for (;;) {
        members.clear(); cl.clear();
        for (int i = 0; i < int(sm.size()); ++i) {
            int found = -1;
            for (int k = 0; k < int(cl.size()); ++k) if (std::hypot(sm[i].x - cl[k].sx, sm[i].y - cl[k].sy) < Rc) { found = k; break; }
            if (found < 0) { Cl c; c.sx = sm[i].x; c.sy = sm[i].y; cl.push_back(c); members.push_back({i}); }
            else members[found].push_back(i);
        }
        if (int(cl.size()) <= opt.maxClusters) break;
        Rc *= 1.5;
    }
    const int K = int(cl.size());
    for (int k = 0; k < K; ++k) {
        double sw = 0, sx = 0, sy = 0; std::vector<double> lv, ac;
        for (int i : members[k]) { sw += sm[i].w0; sx += sm[i].w0 * sm[i].x; sy += sm[i].w0 * sm[i].y; lv.push_back(sm[i].level); ac.push_back(sm[i].acc); }
        Cl &c = cl[k];
        c.x = sx / sw; c.y = sy / sw; c.m = int(members[k].size()); c.W = sw / c.m;
        c.level = median(lv); c.a = median(ac) / ACC68;
        c.sh2 = opt.sigmaDb * opt.sigmaDb * (opt.rhoIn + (1 - opt.rhoIn) / c.m);
        if (c.m >= 2) { double mu = 0; for (double v : lv) mu += v; mu /= c.m; double ss = 0; for (double v : lv) ss += (v - mu) * (v - mu); c.fading = std::sqrt(ss / (c.m - 1)); }
    }
    {   // age/status weights are relative: they shift influence between places, not the total information
        double mw = 0; for (const Cl &c : cl) mw += c.W; mw /= K;
        if (mw > 0) for (Cl &c : cl) c.W /= mw;
    }
    f.vantage = K;
    { double s = 0; int q = 0; for (const Cl &c : cl) if (c.m >= 2) { s += c.fading; ++q; } f.fadingDb = q ? s / q : 0; }
    std::vector<Rg> rg;
    for (const Obs &o : use) if (o.rangeM > 0) { const double a = o.acc / ACC68; rg.push_back({fr.x(o.lon), fr.y(o.lat), o.rangeM, std::max(0.25, o.rangeSd * o.rangeSd) + a * a}); }

    // A Wi-Fi AP heard at places more than 5 km apart travels (Ichnaea's rule)
    double maxPair = 0;
    for (int a = 0; a < K; ++a) for (int b = a + 1; b < K; ++b) maxPair = std::max(maxPair, std::hypot(cl[a].x - cl[b].x, cl[a].y - cl[b].y));
    if (maxPair > 5000) {
        f.kind = QStringLiteral("mobile"); f.lat = use.last().lat; f.lon = use.last().lon;
        grade(f, nullptr);
        return f;
    }

    // ── grid posterior ──
    double cx = 0, cy = 0, cw = 0, maxLevel = -200;
    for (const Cl &c : cl) { const double w = c.W * std::pow(10.0, c.level / 40.0); cw += w; cx += w * c.x; cy += w * c.y; maxLevel = std::max(maxLevel, c.level); }
    cx /= cw; cy /= cw;
    const double wcx = cx, wcy = cy;
    double reach = 0; for (const Cl &c : cl) reach = std::max(reach, std::hypot(c.x - cx, c.y - cy));
    const double dPlaus = std::clamp(modelDistance(opt.p0Mean + opt.p0Sd, std::max(1.6, opt.defaultN - opt.nSd), int(std::lround(maxLevel))), 50.0, 400.0);
    const double W = std::clamp(reach + dPlaus, opt.minHalfWidth, opt.maxHalfWidth);
    const int G = opt.gridN;
    const double cs = 2 * W / G;
    std::vector<double> mxs, mys;
    for (const Miss &m : ctx.misses) { mxs.push_back(fr.x(m.lon)); mys.push_back(fr.y(m.lat)); }
    std::vector<double> ll(G * G), post(G * G);
    std::vector<int> order(G * G);
    double llMax = -1e300, psum = 0, pmx = 0, pmy = 0, hpdR95 = 0;
    double gC[3] = {0, 0, 0};
    auto runGrid = [&](const std::vector<Cl> &cls) {
        for (int j = 0; j < G; ++j)
            for (int i = 0; i < G; ++i)
            {
                const double px = cx - W + (i + 0.5) * cs, py = cy - W + (j + 0.5) * cs;
                // range prior: an AP is rarely far beyond the nearest place it was heard from
                double near = 1e300; for (const Cl &c : cls) near = std::min(near, std::hypot(px - c.x, py - c.y));
                ll[j * G + i] = gridLogL(cls, rg, ctx.misses, mxs, mys, px, py, opt, nullptr, nullptr) - near / opt.rangePriorM;
            }
        llMax = -1e300; for (double v : ll) llMax = std::max(llMax, v);
        psum = 0; pmx = 0; pmy = 0;
        for (int j = 0; j < G; ++j) for (int i = 0; i < G; ++i) {
            const double p = std::exp(ll[j * G + i] - llMax); post[j * G + i] = p; psum += p;
            pmx += p * (cx - W + (i + 0.5) * cs); pmy += p * (cy - W + (j + 0.5) * cs);
        }
        pmx /= psum; pmy /= psum;
        gC[0] = gC[1] = gC[2] = 0;
        for (int j = 0; j < G; ++j) for (int i = 0; i < G; ++i) {
            const double p = post[j * G + i] / psum, dx = cx - W + (i + 0.5) * cs - pmx, dy = cy - W + (j + 0.5) * cs - pmy;
            gC[0] += p * dx * dx; gC[1] += p * dx * dy; gC[2] += p * dy * dy;
        }
        gC[0] += cs * cs / 12; gC[2] += cs * cs / 12;
        for (int i = 0; i < G * G; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return ll[a] > ll[b] || (ll[a] == ll[b] && a < b); });
        std::vector<char> hpd(G * G, 0); double acc95 = 0; int nHpd = 0;
        for (int idx : order) { if (acc95 >= 0.95 * psum) break; hpd[idx] = 1; acc95 += post[idx]; ++nHpd; }
        hpdR95 = std::sqrt(nHpd * cs * cs / M_PI);
        // significant modes: 8-connected components of the HPD region holding ≥ 5 % of the mass
        std::vector<int> comp(G * G, -1); int modes = 0;
        for (int idx : order) {
            if (!hpd[idx] || comp[idx] >= 0) continue;
            std::vector<int> stack{idx}; comp[idx] = idx; double mass = 0;
            while (!stack.empty()) {
                const int c = stack.back(); stack.pop_back(); mass += post[c];
                const int ci = c % G, cj = c / G;
                for (int dj = -1; dj <= 1; ++dj) for (int di = -1; di <= 1; ++di) {
                    const int ni = ci + di, nj = cj + dj;
                    if (ni < 0 || nj < 0 || ni >= G || nj >= G) continue;
                    const int nidx = nj * G + ni;
                    if (hpd[nidx] && comp[nidx] < 0) { comp[nidx] = idx; stack.push_back(nidx); }
                }
            }
            if (mass >= 0.05 * psum) ++modes;
        }
        f.modes = modes;
    };
    runGrid(cl);
    // Seeds: up to three grid maxima (non-maximum suppression over 3 cells), best first
    std::vector<int> seeds;
    for (int idx : order) {
        if (ll[idx] < llMax - 8 || seeds.size() >= 3) break;
        bool near = false;
        for (int s : seeds) if (std::abs(s % G - idx % G) <= 3 && std::abs(s / G - idx / G) <= 3) { near = true; break; }
        if (!near) seeds.push_back(idx);
    }

    // Place geometry (independent of the solution)
    double gsw = 0, gmx = 0, gmy = 0;
    for (const Cl &c : cl) { gsw += c.W; gmx += c.W * c.x; gmy += c.W * c.y; }
    gmx /= gsw; gmy /= gsw;
    double sxx = 0, sxy = 0, syy = 0;
    for (const Cl &c : cl) { sxx += c.W * (c.x - gmx) * (c.x - gmx); sxy += c.W * (c.x - gmx) * (c.y - gmy); syy += c.W * (c.y - gmy) * (c.y - gmy); }
    double mu1, mu2, e1x, e1y;
    eig2(sxx / gsw, sxy / gsw, syy / gsw, &mu1, &mu2, &e1x, &e1y);
    f.linRatio = mu1 > 1e-9 ? std::max(0.0, mu2) / mu1 : 0;
    {   double s1 = 0, s2 = 0; for (const Cl &c : cl) { const double w = c.W / c.sh2; s1 += w; s2 += w * w; } f.ess = s2 > 0 ? s1 * s1 / s2 : 0; }

    Theta best; bool haveLm = false; double tauEff = 1;
    Solver sv(cl, rg, opt);
    if (K >= 3) {
        // Stage A (robust, nominal scale) from every seed and the mirror of the best; Stage B (robust at the
        // pooled scale τ) from each result
        std::vector<Theta> starts;
        std::vector<std::pair<double, double>> pts;
        for (int s : seeds) pts.push_back({cx - W + (s % G + 0.5) * cs, cy - W + (s / G + 0.5) * cs});
        {   // and the classic centroids, which a few loud outliers cannot all capture: signal-weighted, plain, loudest quarter
            double ux = 0, uy = 0; for (const Cl &c : cl) { ux += c.x; uy += c.y; }
            std::vector<int> loud(K); for (int k = 0; k < K; ++k) loud[k] = k;
            std::sort(loud.begin(), loud.end(), [&](int a, int b) { return cl[a].level > cl[b].level || (cl[a].level == cl[b].level && a < b); });
            const int q = std::max(3, K / 4); double lx = 0, ly = 0;
            for (int i = 0; i < q; ++i) { lx += cl[loud[i]].x; ly += cl[loud[i]].y; }
            pts.push_back({wcx, wcy}); pts.push_back({ux / K, uy / K}); pts.push_back({lx / q, ly / q});
        }
        for (const auto &pt : pts) {
            bool dup = false; for (const Theta &t : starts) if (std::hypot(t.x - pt.first, t.y - pt.second) < 10) { dup = true; break; }
            if (dup) continue;
            Theta t; t.x = pt.first; t.y = pt.second;
            double b0, b1; gridLogL(cl, rg, ctx.misses, mxs, mys, t.x, t.y, opt, &b0, &b1);
            t.p0 = std::clamp(b0, -90.0, 10.0); t.n = std::clamp(b1, 1.5, 4.5);
            starts.push_back(t);
        }
        if (!starts.empty()) {
            const Theta &b = starts[0];
            const double vx = b.x - gmx, vy = b.y - gmy, along = vx * e1x + vy * e1y;
            Theta m = b; m.x = gmx + 2 * along * e1x - vx; m.y = gmy + 2 * along * e1y - vy;
            if (std::hypot(m.x - b.x, m.y - b.y) > 10) starts.push_back(m);
        }
        sv.robust = true;                                   // Stage A: robust at the nominal scale
        std::vector<Theta> stA; std::vector<double> costA;
        for (const Theta &t0 : starts) { double c; stA.push_back(sv.run(t0, 10, &c)); costA.push_back(c); }
        int ia = 0; for (int i = 1; i < int(stA.size()); ++i) if (costA[i] < costA[ia]) ia = i;
        {   // robust scale: MAD of the standardised residuals, pooled with a prior of 1 (4 pseudo-observations)
            std::vector<double> z; for (const Cl &c : cl) { const At a = evalAt(c, stA[ia], opt.heightM); z.push_back(a.r / std::sqrt(a.s2)); }
            const double med = median(z); std::vector<double> dev; for (double v : z) dev.push_back(std::fabs(v - med));
            const double mad = 1.4826 * median(dev);
            sv.tau = std::max(0.5, std::sqrt((4.0 + K * mad * mad) / (4.0 + K)));
        }
        std::vector<Theta> stB; std::vector<double> costB;
        for (const Theta &t0 : stA) { double c; stB.push_back(sv.run(t0, opt.maxIter, &c)); costB.push_back(c); }
        int ib = 0; for (int i = 1; i < int(stB.size()); ++i) if (costB[i] < costB[ib]) ib = i;
        best = stB[ib]; haveLm = std::isfinite(best.x) && std::isfinite(best.y) && std::isfinite(costB[ib]);
        int ialt = -1;
        for (int i = 0; i < int(stB.size()); ++i)
            if (i != ib && std::hypot(stB[i].x - best.x, stB[i].y - best.y) > 10 && (ialt < 0 || costB[i] < costB[ialt])) ialt = i;
        f.sigmaDb = sv.tau * opt.sigmaDb;
        if (haveLm) {   // the posterior again with each place down-weighted as the robust fit did
            std::vector<Cl> rw = cl;
            for (int k = 0; k < K; ++k) { const At a = evalAt(cl[k], best, opt.heightM); const double z = a.r / (std::sqrt(a.s2) * sv.tau); rw[k].W = cl[k].W * sv.uFn(z); }
            runGrid(rw);
        }
        tauEff = std::max(1.0, sv.tau);
        if (haveLm && ialt >= 0) { f.altLat = fr.lat(stB[ialt].y); f.altLon = fr.lon(stB[ialt].x); f.ambiguous = costB[ialt] - costB[ib] < 2.0; }
    }

    // ── the answer: LM solution (≥ 3 places) or the grid posterior ──
    Theta sol;
    double C[3];
    double crlbC[3] = {0, 0, 0};
    double Finv[4][4]; bool haveFinv = false;
    if (haveLm) {
        sol = best;
        double A[4][4], g[4];
        sv.normal(sol, tauEff, A, g);
        if (invert4(A, 4, Finv)) {
            haveFinv = true;
            C[0] = Finv[0][0]; C[1] = Finv[0][1]; C[2] = Finv[1][1];
        } else { C[0] = gC[0]; C[1] = gC[1]; C[2] = gC[2]; }
        // design effect of spatially correlated shadowing: K / N_eff, N_eff = 1ᵀR⁻¹1
        {
            std::vector<double> L(K * K, 0.0);
            bool ok = true;
            for (int i = 0; i < K && ok; ++i)
                for (int j = 0; j <= i; ++j) {
                    double s = std::exp(-std::hypot(cl[i].x - cl[j].x, cl[i].y - cl[j].y) / opt.shadowCorrM) + (i == j ? 1e-9 : 0.0);
                    for (int q = 0; q < j; ++q) s -= L[i * K + q] * L[j * K + q];
                    if (i == j) { if (s <= 0) { ok = false; break; } L[i * K + i] = std::sqrt(s); }
                    else L[i * K + j] = s / L[j * K + j];
                }
            if (ok) {
                std::vector<double> z(K);
                for (int i = 0; i < K; ++i) { double s = 1.0; for (int q = 0; q < i; ++q) s -= L[i * K + q] * z[q]; z[i] = s / L[i * K + i]; }
                double neff = 0; for (double v : z) neff += v * v;
                const double deff = std::max(1.0, K / std::max(1e-9, neff));
                C[0] *= deff; C[1] *= deff; C[2] *= deff;
            }
        }
        // correlated fix error: shared within a session
        { const double am = median(accs) / ACC68; const double fl = am * am / std::max(1, f.sessions); C[0] += fl; C[2] += fl; }
        // Cramér–Rao floor with the nominal σ0
        {
            Solver s0(cl, rg, opt);
            double A0[4][4], g0[4], I0[4][4];
            s0.normal(sol, 1.0, A0, g0);
            if (invert4(A0, 4, I0)) { crlbC[0] = I0[0][0]; crlbC[1] = I0[0][1]; crlbC[2] = I0[1][1]; eigMax(C, crlbC); }
        }
        // leave-one-place-out jackknife
        if (K >= 4 && K <= opt.jackMaxK) {
            std::vector<double> px, py;
            Solver sj = sv;
            for (int k = 0; k < K; ++k) {
                std::fill(sj.mult.begin(), sj.mult.end(), 1.0); sj.mult[k] = 0;
                const Theta t = sj.run(sol, 12, nullptr);
                px.push_back(t.x); py.push_back(t.y);
                f.jackMax = std::max(f.jackMax, std::hypot(t.x - sol.x, t.y - sol.y));
            }
            double mxj = 0, myj = 0; for (int k = 0; k < K; ++k) { mxj += px[k]; myj += py[k]; } mxj /= K; myj /= K;
            double J[3] = {0, 0, 0};
            for (int k = 0; k < K; ++k) { J[0] += (px[k] - mxj) * (px[k] - mxj); J[1] += (px[k] - mxj) * (py[k] - myj); J[2] += (py[k] - myj) * (py[k] - myj); }
            for (double &v : J) v *= double(K - 1) / K;
            eigMax(C, J);
        }
        // cluster bootstrap
        if (K >= 6 && opt.bootstrap > 1) {
            quint64 st = opt.seed ^ (quint64(K) * 0x9E3779B97F4A7C15ULL);
            if (st == 0) st = 1;
            auto next = [&st]() { st ^= st >> 12; st ^= st << 25; st ^= st >> 27; return st * 2685821657736338717ULL; };
            std::vector<double> px, py;
            Solver sb = sv;
            for (int b = 0; b < opt.bootstrap; ++b) {
                std::fill(sb.mult.begin(), sb.mult.end(), 0.0);
                for (int q = 0; q < K; ++q) sb.mult[int((next() >> 11) % quint64(K))] += 1.0;
                const Theta t = sb.run(sol, 12, nullptr);
                px.push_back(t.x); py.push_back(t.y);
            }
            const int B = int(px.size());
            double mxb = 0, myb = 0; for (int b = 0; b < B; ++b) { mxb += px[b]; myb += py[b]; } mxb /= B; myb /= B;
            double Bc[3] = {0, 0, 0};
            for (int b = 0; b < B; ++b) { Bc[0] += (px[b] - mxb) * (px[b] - mxb); Bc[1] += (px[b] - mxb) * (py[b] - myb); Bc[2] += (py[b] - myb) * (py[b] - myb); }
            for (double &v : Bc) v /= (B - 1);
            eigMax(C, Bc);
        }
    } else {
        sol.x = pmx; sol.y = pmy;
        double b0, b1; gridLogL(cl, rg, ctx.misses, mxs, mys, pmx, pmy, opt, &b0, &b1);
        sol.p0 = std::clamp(b0, -90.0, 10.0); sol.n = std::clamp(b1, 1.5, 4.5);
        C[0] = gC[0]; C[1] = gC[1]; C[2] = gC[2];
    }
    const double k2 = opt.kappa * opt.kappa;
    C[0] *= k2; C[1] *= k2; C[2] *= k2;
    setEllipse(f, C);
    f.lat = fr.lat(sol.y); f.lon = fr.lon(sol.x); f.p0 = sol.p0; f.pathloss = sol.n; f.fittedN = haveLm;
    if (crlbC[0] > 0) { double a, b, o; ellipse(crlbC[0], crlbC[1], crlbC[2], &a, &b, &o); f.crlbR95 = radiusFor(a, b, 0.95); }

    // ── residual statistics ──
    {
        double sw = 0, swr = 0, chi = 0; int kin = 0, rej = 0;
        std::vector<double> lv, ld;
        for (int k = 0; k < K; ++k) {
            const At a = evalAt(cl[k], sol, opt.heightM);
            const double z = a.r / (std::sqrt(a.s2) * (haveLm ? sv.tau : 1.0));
            if (haveLm ? sv.uFn(z) < 0.5 : std::fabs(z) > 3.0) { rej += cl[k].m; continue; }
            ++kin; sw += cl[k].W; swr += cl[k].W * a.r * a.r; chi += cl[k].W * a.r * a.r / a.s2;
            lv.push_back(cl[k].level); ld.push_back(std::log(a.d));
        }
        f.rejected = rej;
        f.outlierFrac = f.n > 0 ? double(rej) / f.n : 0;
        f.rms = sw > 0 ? std::sqrt(swr / sw) : 0;
        f.chi2nu = chi / std::max(1, kin - 2);
        if (!haveLm) f.sigmaDb = f.rms;
        if (lv.size() >= 4) { std::vector<double> ra, rb; ranks(lv, &ra); ranks(ld, &rb); f.spearman = pearson(ra, rb); }
    }
    // bearings AP → places
    {
        double sc = 0, ss = 0, sw = 0; std::vector<double> th;
        for (const Cl &c : cl) { const double a = std::atan2(c.y - sol.y, c.x - sol.x); th.push_back(a); sc += c.W * std::cos(a); ss += c.W * std::sin(a); sw += c.W; }
        f.rbar = sw > 0 ? std::sqrt(sc * sc + ss * ss) / sw : 1;
        std::sort(th.begin(), th.end());
        double gap = 0;
        if (th.size() >= 2) { for (size_t i = 1; i < th.size(); ++i) gap = std::max(gap, th[i] - th[i - 1]); gap = std::max(gap, th.front() + 2 * M_PI - th.back()); }
        else gap = 2 * M_PI;
        f.maxGapDeg = gap * 180.0 / M_PI;
        f.inHull = pointInHull(cl, sol.x, sol.y);
        std::vector<double> ds; double dmin = 1e300;
        for (const Cl &c : cl) { const double d = std::hypot(c.x - sol.x, c.y - sol.y); ds.push_back(d); dmin = std::min(dmin, d); }
        const double dmed = median(ds); f.dminRatio = dmed > 0 ? dmin / dmed : 0;
    }
    // geometry-only dilution of precision (P0 profiled out) and the P0–range correlation
    if (K >= 3) {
        double vbx = 0, vby = 0, sw = 0; std::vector<double> vx, vy;
        for (const Cl &c : cl) { const double dx = sol.x - c.x, dy = sol.y - c.y, d2 = dx * dx + dy * dy + opt.heightM * opt.heightM; vx.push_back(dx / d2); vy.push_back(dy / d2); }
        for (int k = 0; k < K; ++k) { vbx += cl[k].W * vx[k]; vby += cl[k].W * vy[k]; sw += cl[k].W; }
        vbx /= sw; vby /= sw;
        double M0 = 0, M1 = 0, M2 = 0;
        for (int k = 0; k < K; ++k) { const double ax = vx[k] - vbx, ay = vy[k] - vby; M0 += cl[k].W * ax * ax; M1 += cl[k].W * ax * ay; M2 += cl[k].W * ay * ay; }
        double inv[3];
        f.rssDop = invert2(M0, M1, M2, inv) && inv[0] + inv[2] > 0 ? std::sqrt(inv[0] + inv[2]) : 1e6;
        if (haveFinv) {
            double ex = sol.x - gmx, ey = sol.y - gmy; double nn = std::hypot(ex, ey);
            if (nn < 1) { ex = e1x; ey = e1y; } else { ex /= nn; ey /= nn; }
            const double vr = ex * ex * Finv[0][0] + 2 * ex * ey * Finv[0][1] + ey * ey * Finv[1][1];
            const double cpr = ex * Finv[0][2] + ey * Finv[1][2];
            f.p0RangeCorr = vr > 0 && Finv[2][2] > 0 ? std::fabs(cpr) / std::sqrt(vr * Finv[2][2]) : 0;
        }
    }
    if (f.ambiguous) {   // the alternative must be separated by more than 2σ along the line joining them
        const double dx = fr.x(f.altLon) - sol.x, dy = fr.y(f.altLat) - sol.y, dd = std::hypot(dx, dy);
        const double s2 = dd > 0 ? (dx * dx * f.cxx + 2 * dx * dy * f.cxy + dy * dy * f.cyy) / (dd * dd) : 0;
        // inside the places' hull a far alternative is a second mode (counted by the grid), not a mirror
        if (dd <= 2 * std::sqrt(std::max(0.0, s2)) || f.inHull) f.ambiguous = false;
    }

    // ── kind ──
    const bool isFix = haveLm && f.r95 <= opt.fixMaxR95 && f.crlbR95 > 0 && f.crlbR95 <= opt.fixMaxR95;
    if (isFix) f.kind = QStringLiteral("fix");
    else {
        // a region: centred on the posterior mean (the best guess under this much uncertainty)
        f.kind = QStringLiteral("region");
        if (haveLm) {
            sol.x = pmx; sol.y = pmy;
            double b0, b1; gridLogL(cl, rg, ctx.misses, mxs, mys, pmx, pmy, opt, &b0, &b1);
            sol.p0 = std::clamp(b0, -90.0, 10.0); sol.n = std::clamp(b1, 1.5, 4.5);
            eigMax(C, gC);
            setEllipse(f, C);
            f.lat = fr.lat(sol.y); f.lon = fr.lon(sol.x); f.p0 = sol.p0; f.pathloss = sol.n;
        }
        f.r95 = std::max(f.r95, hpdR95);
        f.ambiguous = false;
    }
    if (f.r95 > opt.maxR95 || !std::isfinite(f.lat) || !std::isfinite(f.lon)) { f.kind = QStringLiteral("none"); f.valid = false; grade(f, nullptr); return f; }
    f.valid = true;
    // A static AP gets quieter with distance; a companion does not (docs/GRADING.md §3.8)
    if (isFix && K >= 5 && maxPair > 300 && f.spearman > -0.1) {
        f.kind = QStringLiteral("mobile"); f.valid = false;
        grade(f, nullptr);
        return f;
    }

    // ── drift, external agreement ──
    if (ctx.hasPrev && ctx.prev.valid && (ctx.prev.kind == QLatin1String("fix") || ctx.prev.kind == QLatin1String("region"))) {
        const double dx = fr.x(ctx.prev.lon) - sol.x, dy = fr.y(ctx.prev.lat) - sol.y;
        double pc0 = ctx.prev.cxx, pc1 = ctx.prev.cxy, pc2 = ctx.prev.cyy;
        if (pc0 <= 0 || pc2 <= 0) { pc0 = pc2 = ctx.prev.acc * ctx.prev.acc; pc1 = 0; }
        double inv[3];
        if (invert2(f.cxx + pc0, f.cxy + pc1, f.cyy + pc2, inv)) {
            const double d2 = dx * dx * inv[0] + 2 * dx * dy * inv[1] + dy * dy * inv[2];
            f.driftD2 = 0.5 * ctx.prev.driftD2 + 0.5 * d2;
        }
        f.nisEwma = 0;
    }
    if (ctx.external.has) {
        const double dx = fr.x(ctx.external.lon) - sol.x, dy = fr.y(ctx.external.lat) - sol.y;
        const double se = std::max(ctx.external.acc, 50.0) / ACC68, s2 = se * se;
        double inv[3];
        if (invert2(f.cxx + s2, f.cxy, f.cyy + s2, inv)) f.extD2 = dx * dx * inv[0] + 2 * dx * dy * inv[1] + dy * dy * inv[2];
    }

    // ── "sample here next": the spot whose sample adds the most information ──
    {
        Solver s0(cl, rg, opt);
        double A0[4][4], g0[4], I0[4][4];
        s0.normal(sol, 1.0, A0, g0);
        if (invert4(A0, 4, I0) && (f.kind == QLatin1String("region") || f.r95 > 25)) {
            const double b = 10.0 * sol.n / LN10, aFix = 10.0 / ACC68;
            double bestGain = 0, bx = 0, by = 0;
            for (int ri = 0; ri < 3; ++ri) for (int bi = 0; bi < 16; ++bi) {
                const double rad = 30.0 * (ri + 1), ang = bi * 2 * M_PI / 16;
                const double qx = sol.x + rad * std::cos(ang), qy = sol.y + rad * std::sin(ang);
                const double dx = sol.x - qx, dy = sol.y - qy, rho = std::hypot(dx, dy), d = std::sqrt(rho * rho + opt.heightM * opt.heightM);
                const double J[4] = {b * dx / (d * d), b * dy / (d * d), -1.0, 10.0 * std::log10(d)};
                double q = 0; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) q += J[i] * I0[i][j] * J[j];
                const double grad = b * rho / (d * d), s2 = opt.sigmaDb * opt.sigmaDb + grad * grad * aFix * aFix;
                const double mu = sol.p0 - 10.0 * sol.n * std::log10(d);
                const double gain = std::log(1 + q / s2) * normCdf((mu - opt.sensitivity) / opt.sigmaDb);
                if (gain > bestGain + 1e-12) { bestGain = gain; bx = qx; by = qy; }
            }
            if (bestGain > 0.01) { f.suggestLat = fr.lat(by); f.suggestLon = fr.lon(bx); f.suggestGain = bestGain; }
        }
    }

    // ── score ──
    f.cP = precisionComp(f.r95);
    f.cG = (0.6 * (1 - f.rbar) + 0.4 * std::min(1.0, 4 * f.linRatio)) * (f.inHull ? 1.0 : 0.7);
    f.cE = 1 - std::exp(-f.ess / 4);
    f.cFfit = std::exp(-std::max(0.0, f.chi2nu - 1.5) / 2) * (1 - clamp01((f.outlierFrac - 0.10) / 0.40));
    f.cF = f.cFfit * nisFactor(f.nisEwma);
    f.cS = std::exp(-f.driftD2 / 6) * std::exp(-std::pow(f.jackMax / std::max(1.0, f.semiMajor), 2) / 8) * (f.ambiguous ? 0.4 : 1.0);
    f.cT = freshness(f.newest, now);
    f.cX = f.extD2 >= 0 ? std::max(0.3, std::exp(-f.extD2 / 8)) : -1;
    grade(f, ctx.hasPrev ? &ctx.prev : nullptr);
    return f;
}

// ── incremental update (between batched refits) ──────────────────────────────
// One new sample says "the AP is d metres from here" (d from the AP's own P0/n): a 2-D Kalman
// step with H = the unit vector observer → fit, and the full covariance kept.
Fit update(const Fit &prev, const Obs &o, const Options &opt)
{
    if (!prev.valid || (prev.kind != QLatin1String("fix") && prev.kind != QLatin1String("region"))) return prev;
    Fit f = prev;
    const Frame fr(prev.lat, prev.lon);
    const double ox = fr.x(o.lon), oy = fr.y(o.lat);
    const double r = std::max(1.0, std::hypot(ox, oy));
    const double d3 = modelDistance(prev.p0, prev.pathloss, o.dbm);
    const double dm = std::sqrt(std::max(1.0, d3 * d3 - opt.heightM * opt.heightM));
    const double ux = -ox / r, uy = -oy / r;
    const double sigD = d3 * LN10 * opt.sigmaDb / (10.0 * prev.pathloss);
    const double a = o.acc / ACC68;
    const double R = sigD * sigD + a * a;
    double P0 = prev.cxx, P1 = prev.cxy, P2 = prev.cyy;
    if (P0 <= 0 || P2 <= 0) { P0 = P2 = prev.acc * prev.acc; P1 = 0; }
    const double Pux = P0 * ux + P1 * uy, Puy = P1 * ux + P2 * uy;
    const double S = ux * Pux + uy * Puy + R;
    const double innov = dm - r;
    const double nis = innov * innov / S;
    f.nisEwma = prev.nisEwma > 0 ? 0.8 * prev.nisEwma + 0.2 * nis : nis;
    double C[3] = {P0, P1, P2};
    if (nis <= 9.0) {
        const double Kx = Pux / S, Ky = Puy / S;
        f.lat = fr.lat(Ky * innov); f.lon = fr.lon(Kx * innov);
        C[0] = P0 - Pux * Pux / S; C[1] = P1 - Pux * Puy / S; C[2] = P2 - Puy * Puy / S;
    }
    setEllipse(f, C);
    if (f.kind == QLatin1String("region")) f.r95 = std::max(f.r95, radiusFor(f.semiMajor, f.semiMinor, 0.95));
    f.n = prev.n + 1;
    f.updated = std::max(prev.updated, o.t);
    f.newest = std::max(prev.newest, o.t);
    f.cP = precisionComp(f.r95);
    f.cF = f.cFfit * nisFactor(f.nisEwma);
    f.cT = freshness(f.newest, o.t > 0 ? o.t : prev.updated);
    grade(f, &prev);
    return f;
}

// ── self-location: where are WE, from beacons with known positions ───────────
SelfFix selfLocate(const QList<Known> &known, const Options &opt)
{
    SelfFix out;
    if (known.size() < 2) return out;
    const Frame fr(known[0].lat, known[0].lon);
    struct S { double x, y, w, d, sigD, c0, c1, c2, acc; bool on; };
    std::vector<S> s;
    for (const Known &k : known) {
        const double p0 = k.haveModel ? k.p0 : -40.0, n = k.haveModel ? k.pathloss : opt.defaultN;
        const double d = std::clamp(modelDistance(p0, n, k.dbm), 1.0, 1500.0);
        const double sigD = d * LN10 * opt.sigmaDb / (10.0 * n);
        s.push_back({fr.x(k.lon), fr.y(k.lat), std::max(0.05, k.weight), d, sigD, k.cxx, k.cxy, k.cyy, k.acc, true});
    }
    auto sig2 = [](const S &q, double ux, double uy) {
        const double apVar = (q.c0 > 0 && q.c2 > 0) ? ux * ux * q.c0 + 2 * ux * uy * q.c1 + uy * uy * q.c2 : q.acc * q.acc;
        return q.sigD * q.sigD + apVar;
    };
    double x = 0, y = 0;
    {
        double sw = 0;
        for (size_t i = 0; i < s.size(); ++i) { const double cw = std::pow(10.0, known[int(i)].dbm / 20.0) / std::max(10.0, s[i].acc); sw += cw; x += cw * s[i].x; y += cw * s[i].y; }
        x /= sw; y /= sw;
    }
    const double x0 = x, y0 = y;
    auto solveActive = [&](double *px, double *py) {
        double X = x0, Y = y0, lambda = 1e-2;
        int active = 0; for (const S &q : s) if (q.on) ++active;
        if (active < 3) return;
        auto costAt = [&](double cx, double cy) {
            double c = 0;
            for (const S &q : s) {
                if (!q.on) continue;
                const double dx = cx - q.x, dy = cy - q.y, r = std::max(1.0, std::hypot(dx, dy));
                const double v = sig2(q, dx / r, dy / r), res = r - q.d, h = huberWeight(res / std::sqrt(v), 2.0);
                c += q.w * h * res * res / v;
            }
            return c;
        };
        for (int it = 0; it < opt.maxIter; ++it) {
            double A[4][4] = {{0}}, b[4] = {0};
            for (const S &q : s) {
                if (!q.on) continue;
                const double dx = X - q.x, dy = Y - q.y, r = std::max(1.0, std::hypot(dx, dy));
                const double jx = dx / r, jy = dy / r, v = sig2(q, jx, jy);
                const double res = r - q.d, h = huberWeight(res / std::sqrt(v), 2.0), w = q.w * h / v;
                A[0][0] += w * jx * jx; A[0][1] += w * jx * jy; A[1][0] += w * jx * jy; A[1][1] += w * jy * jy;
                b[0] -= w * jx * res; b[1] -= w * jy * res;
            }
            const double cost = costAt(X, Y);
            A[0][0] *= 1 + lambda; A[1][1] *= 1 + lambda;
            double dx[4]; if (!solve(A, b, 2, dx)) break;
            const double nx = X + dx[0], ny = Y + dx[1], nc = costAt(nx, ny);
            if (nc <= cost) { X = nx; Y = ny; lambda = std::max(1e-6, lambda / 3); if (std::hypot(dx[0], dx[1]) < 0.2) break; }
            else { lambda *= 8; if (lambda > 1e6) break; }
        }
        *px = X; *py = Y;
    };
    if (known.size() >= 3) solveActive(&x, &y);
    // Integrity (RAIM-like): a χ² test on the normalised range residuals; exclude the worst, retry
    QString integrity = known.size() >= 4 ? QStringLiteral("ok") : QStringLiteral("unverified");
    if (known.size() >= 4) {
        for (int round = 0; round < 3; ++round) {
            double chi = 0; int act = 0, worst = -1; double worstZ = 0;
            for (int i = 0; i < int(s.size()); ++i) {
                if (!s[i].on) continue;
                const double dx = x - s[i].x, dy = y - s[i].y, r = std::max(1.0, std::hypot(dx, dy));
                const double z = (r - s[i].d) / std::sqrt(sig2(s[i], dx / r, dy / r));
                chi += z * z; ++act;
                if (std::fabs(z) > worstZ) { worstZ = std::fabs(z); worst = i; }
            }
            if (act < 4 || chi <= chi2Quantile99(act - 2)) { if (round > 0) integrity = act >= 4 ? QStringLiteral("repaired") : QStringLiteral("unverified"); break; }
            if (round == 2 || act - 1 < 3) { integrity = QStringLiteral("failed"); break; }
            s[worst].on = false; ++out.excluded;
            solveActive(&x, &y);
        }
    }
    double A[3] = {0, 0, 0}, wsum = 0, wres = 0; int rejected = 0, used = 0;
    for (const S &q : s) {
        if (!q.on) continue;
        const double dx = x - q.x, dy = y - q.y, r = std::max(1.0, std::hypot(dx, dy));
        const double jx = dx / r, jy = dy / r, v = sig2(q, jx, jy);
        const double res = r - q.d, h = huberWeight(res / std::sqrt(v), 2.0), w = q.w * h / v;
        if (h < 0.5) ++rejected;
        ++used;
        A[0] += w * jx * jx; A[1] += w * jx * jy; A[2] += w * jy * jy;
        wsum += w; wres += w * res * res;
    }
    const double chiNorm = wres / std::max(1, used - 2);                 // reduced χ² of the normalised ranges
    const double spread = std::sqrt(wres / std::max(1e-12, wsum));      // weighted RMS range residual (m)
    double acc, r95 = 0;
    double inv[3];
    if (used >= 3 && invert2(A[0], A[1], A[2], inv)) {
        double maj, mn, o;
        const double scale = std::max(1.0, chiNorm);
        ellipse(inv[0] * scale, inv[1] * scale, inv[2] * scale, &maj, &mn, &o);
        acc = maj; r95 = radiusFor(maj, mn, 0.95);
    } else { acc = 0; for (const S &q : s) if (q.on) acc = std::max(acc, std::sqrt(q.sigD * q.sigD + q.acc * q.acc)); r95 = 2.45 * acc; }
    out.valid = true; out.lat = fr.lat(y); out.lon = fr.lon(x);
    out.acc = std::max(15.0, std::max(acc, spread * 0.7));
    out.r95 = std::max(out.acc * 2.45, r95);
    out.rms = spread;
    out.used = used - rejected; out.rejected = rejected + out.excluded;
    out.integrity = integrity;
    return out;
}

} // namespace Estimator
