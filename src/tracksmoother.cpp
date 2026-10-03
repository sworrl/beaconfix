// The track smoother (see tracksmoother.h): a 3-state-per-axis Kalman filter with a Rauch–Tung–Striebel
// backward pass. Pure and deterministic.
#include "tracksmoother.h"
#include "estimator.h"            // Frame, distanceM (header-only)
#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

namespace TrackSmoother {

namespace {
constexpr double ACC68 = 1.515;                       // 68 % radius → per-axis σ (as estimator.cpp)
constexpr double RAYLEIGH_MED = 1.1774100225154747;   // √(2 ln 2): median radius of a circular normal / σ
constexpr double CHI2_2_99 = 9.210340371976184;       // χ²(2) 99 %
constexpr double MAX_INFLATE = 50;                    // cap of the correlated-noise inflation (dt → 0)

struct M3 { double a[3][3]; };

inline void symm(M3 &P)
{
    for (int i = 0; i < 3; ++i) for (int j = i + 1; j < 3; ++j) P.a[i][j] = P.a[j][i] = 0.5 * (P.a[i][j] + P.a[j][i]);
}

// A = L·Lᵀ; false when A is not (numerically) positive definite
bool chol3(const M3 &A, double L[3][3])
{
    const double scale = std::max({A.a[0][0], A.a[1][1], A.a[2][2], 1e-300});
    for (int i = 0; i < 3; ++i) for (int j = 0; j <= i; ++j) {
        double s = A.a[i][j];
        for (int k = 0; k < j; ++k) s -= L[i][k] * L[j][k];
        if (i == j) { if (!(s > 1e-14 * scale)) return false; L[i][i] = std::sqrt(s); }
        else L[i][j] = s / L[j][j];
    }
    L[0][1] = L[0][2] = L[1][2] = 0;
    return true;
}

// Cyclic Jacobi eigen-decomposition of a symmetric 3×3: A = V·diag(w)·Vᵀ (columns of V)
void jacobi3(M3 A, double w[3], double V[3][3])
{
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) V[i][j] = i == j;
    for (int sweep = 0; sweep < 12; ++sweep) {
        const double off = A.a[0][1] * A.a[0][1] + A.a[0][2] * A.a[0][2] + A.a[1][2] * A.a[1][2];
        if (off < 1e-30 * (A.a[0][0] * A.a[0][0] + A.a[1][1] * A.a[1][1] + A.a[2][2] * A.a[2][2] + 1e-300)) break;
        for (int p = 0; p < 2; ++p) for (int q = p + 1; q < 3; ++q) {
            if (A.a[p][q] == 0) continue;
            const double th = (A.a[q][q] - A.a[p][p]) / (2 * A.a[p][q]);
            const double t = (th >= 0 ? 1 : -1) / (std::fabs(th) + std::sqrt(th * th + 1));
            const double c = 1 / std::sqrt(t * t + 1), s = t * c;
            for (int k = 0; k < 3; ++k) {              // A ← Jᵀ A J
                const double akp = A.a[k][p], akq = A.a[k][q];
                A.a[k][p] = c * akp - s * akq; A.a[k][q] = s * akp + c * akq;
            }
            for (int k = 0; k < 3; ++k) {
                const double apk = A.a[p][k], aqk = A.a[q][k];
                A.a[p][k] = c * apk - s * aqk; A.a[q][k] = s * apk + c * aqk;
            }
            for (int k = 0; k < 3; ++k) {
                const double vkp = V[k][p], vkq = V[k][q];
                V[k][p] = c * vkp - s * vkq; V[k][q] = s * vkp + c * vkq;
            }
        }
    }
    for (int i = 0; i < 3; ++i) w[i] = A.a[i][i];
}

// Symmetrise; when not positive definite, floor the eigenvalues at 1e-12·trace (tiny/negative ones are
// round-off, never information)
void guard(M3 &P)
{
    symm(P);
    double L[3][3];
    if (chol3(P, L)) return;
    double w[3], V[3][3];
    jacobi3(P, w, V);
    const double fl = 1e-12 * std::max(1e-12, std::fabs(w[0]) + std::fabs(w[1]) + std::fabs(w[2]));
    for (double &x : w) x = std::max(x, fl);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
        P.a[i][j] = V[i][0] * w[0] * V[j][0] + V[i][1] * w[1] * V[j][1] + V[i][2] * w[2] * V[j][2];
    symm(P);
}

// Solve A·X = B (A symmetric positive definite, 3 right-hand sides)
void spdSolve(M3 A, const M3 &B, M3 &X)
{
    double L[3][3];
    if (!chol3(A, L)) {
        guard(A);
        if (!chol3(A, L)) { const double j = 1e-9 * (A.a[0][0] + A.a[1][1] + A.a[2][2]) + 1e-12; for (int i = 0; i < 3; ++i) A.a[i][i] += j; chol3(A, L); }
    }
    for (int c = 0; c < 3; ++c) {
        double y[3];
        for (int i = 0; i < 3; ++i) { double s = B.a[i][c]; for (int k = 0; k < i; ++k) s -= L[i][k] * y[k]; y[i] = s / L[i][i]; }
        for (int i = 2; i >= 0; --i) { double s = y[i]; for (int k = i + 1; k < 3; ++k) s -= L[k][i] * X.a[k][c]; X.a[i][c] = s / L[i][i]; }
    }
}

// Transition over dt for one axis: F = [[1 a 0][0 e 0][0 0 φ]], Q = [[qpp qpv 0][qpv qvv 0][0 0 qbb]]
struct Tr { double a = 0, e = 1, phi = 1, qpp = 0, qpv = 0, qvv = 0, qbb = 0; };

Tr transition(double dt, const Motion &m, const Options &o)
{
    Tr t;
    if (dt <= 0) return t;
    const double q = m.sigmaA * m.sigmaA;
    if (m.tauV <= 0) {                                   // CWNA (τ_v = ∞)
        t.a = dt; t.e = 1;
        t.qpp = q * dt * dt * dt / 3; t.qpv = q * dt * dt / 2; t.qvv = q * dt;
    } else {                                              // integrated OU, exact
        const double tau = m.tauV, x = dt / tau, em1 = -std::expm1(-x);      // 1 − e^(−x)
        t.e = 1 - em1; t.a = tau * em1;
        t.qvv = q * tau / 2 * -std::expm1(-2 * x);
        t.qpv = q / 2 * (tau * em1) * (tau * em1);
        t.qpp = x < 1e-2 ? q * dt * dt * dt * (1.0 / 3 - x / 4 + 7 * x * x / 60)   // series: no cancellation
                         : q * tau * tau * (dt - 2 * tau * em1 + tau / 2 * -std::expm1(-2 * x));
    }
    if (o.tauB > 0) { t.phi = std::exp(-dt / o.tauB); t.qbb = o.sigmaB * o.sigmaB * -std::expm1(-2 * dt / o.tauB); }
    return t;
}

// P ← F·P·Fᵀ + Q
M3 predictCov(const M3 &P, const Tr &t)
{
    const double F[3][3] = {{1, t.a, 0}, {0, t.e, 0}, {0, 0, t.phi}};
    double FP[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) FP[i][j] = F[i][0] * P.a[0][j] + F[i][1] * P.a[1][j] + F[i][2] * P.a[2][j];
    M3 R;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R.a[i][j] = FP[i][0] * F[j][0] + FP[i][1] * F[j][1] + FP[i][2] * F[j][2];
    R.a[0][0] += t.qpp; R.a[0][1] += t.qpv; R.a[1][0] += t.qpv; R.a[1][1] += t.qvv; R.a[2][2] += t.qbb;
    guard(R);
    return R;
}

int dynOf(Mode m) { return m == Mode::Stationary ? 0 : m == Mode::Vehicle ? 2 : 1; }
Mode modeOfDyn(int d) { return d == 0 ? Mode::Stationary : d == 2 ? Mode::Vehicle : Mode::Foot; }
const Motion &motionOf(int d, const Options &o) { return d == 0 ? o.stationary : d == 2 ? o.vehicle : o.foot; }
double sigmaV2(const Motion &m) { return m.tauV > 0 ? m.sigmaA * m.sigmaA * m.tauV / 2 : 1e4; }

struct Ev {
    double t = 0;                 // s since the first event
    double lat = 0, lon = 0;
    double var = 0;               // fix: σ_tot² per axis; ref: sd²
    int fix = -1, ref = -1;       // index into the inputs
    int dyn = 1;                  // 0 stationary, 1 foot, 2 vehicle
};
struct Node {
    double xf[2][3], xp[2][3], xs[2][3];
    M3 Pf, Pp, Ps;
    Tr tr;                        // transition into this event
    bool gated = false;
};

double median(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t h = v.size() / 2;
    return v.size() % 2 ? v[h] : 0.5 * (v[h - 1] + v[h]);
}
} // namespace

Mode modeFor(const QString &source)
{
    const QString s = source.toLower();
    if (s.contains(QLatin1String("stationary")) || s.contains(QLatin1String("average")) || s.contains(QLatin1String("still"))) return Mode::Stationary;
    if (s.contains(QLatin1String("vehicle")) || s.contains(QLatin1String("bicycle")) || s.contains(QLatin1String("driving"))) return Mode::Vehicle;
    if (s.contains(QLatin1String("foot")) || s.contains(QLatin1String("walk")) || s.contains(QLatin1String("running"))) return Mode::Foot;
    return Mode::Unknown;
}

QList<Out> smooth(const QList<Fix> &fixes, const QList<Ref> &refs, const Options &opt, Stats *stats)
{
    Stats st;
    const int n = int(fixes.size());
    QList<Out> out(n);
    if (n == 0 && refs.isEmpty()) { if (stats) *stats = st; return out; }

    // ── 1. per fix: σ_tot², used or dropped, duplicates ──
    std::vector<double> var(n, 0);
    std::vector<char> use(n, 0);
    std::vector<int> dupOf(n, -1);
    std::vector<Mode> mode0(n, Mode::Unknown);
    for (int i = 0; i < n; ++i) {
        const Fix &f = fixes[i];
        const QString s = f.source.toLower();
        mode0[i] = modeFor(s);
        const bool ip = s == QLatin1String("ip") || s.startsWith(QLatin1String("ip-")) || s.startsWith(QLatin1String("geoip"));
        const double k = s.startsWith(QLatin1String("wifi")) ? opt.wifiScale : 1.0;
        const double a = opt.accScale * f.acc;
        var[i] = std::pow(k * std::sqrt(a * a + opt.accFloor * opt.accFloor) / ACC68, 2);
        const bool ok = std::isfinite(f.lat) && std::isfinite(f.lon) && std::fabs(f.lat) <= 90 && std::fabs(f.lon) <= 180
                     && std::isfinite(f.acc) && f.acc > 0 && f.acc <= opt.maxAcc && (!ip || opt.useIp);
        use[i] = ok;
        if (!ok) {
            Out &o = out[i]; o.lat = f.lat; o.lon = f.lon; o.dropped = true;
            o.cxx = o.cyy = std::isfinite(var[i]) ? var[i] : 0; o.acc = ACC68 * std::sqrt(o.cxx);
            ++st.dropped;
        }
    }
    std::vector<int> ord;                                  // used fixes by time (stable)
    for (int i = 0; i < n; ++i) if (use[i]) ord.push_back(i);
    std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) { return fixes[a].tMs < fixes[b].tMs; });
    {
        std::vector<int> u;                                // exact duplicates (same time and place) count once
        for (int i : ord) {
            int d = -1;
            for (int j = int(u.size()) - 1; j >= 0 && fixes[u[j]].tMs == fixes[i].tMs; --j)
                if (fixes[u[j]].lat == fixes[i].lat && fixes[u[j]].lon == fixes[i].lon) { d = u[j]; break; }
            if (d >= 0) { dupOf[i] = d; ++st.duplicates; } else u.push_back(i);
        }
        ord.swap(u);
    }
    const qint64 t0 = std::min(ord.empty() ? LLONG_MAX : fixes[ord.front()].tMs,
                               refs.isEmpty() ? LLONG_MAX : std::min_element(refs.begin(), refs.end(), [](const Ref &a, const Ref &b) { return a.tMs < b.tMs; })->tMs);
    auto secs = [t0](qint64 ms) { return double(ms - t0) / 1000.0; };

    // ── 2. motion modes; Unknown → Vehicle where the fixes themselves move fast (majority of 5) ──
    const int m = int(ord.size());
    std::vector<int> dyn(m, 1);
    {
        std::vector<char> fast(m, 0);
        const double W = opt.speedWindowS;
        int lo = 0, hi = 0;
        for (int i = 0; i < m; ++i) {
            const double ti = secs(fixes[ord[i]].tMs);
            while (lo + 1 < i && secs(fixes[ord[lo + 1]].tMs) <= ti - W) ++lo;
            if (hi < i) hi = i;
            while (hi < m - 1 && secs(fixes[ord[hi]].tMs) < ti + W) ++hi;
            int a = lo, b = hi;
            if (secs(fixes[ord[a]].tMs) > ti - W || ti - secs(fixes[ord[a]].tMs) > opt.gapResetS) a = i;
            if (secs(fixes[ord[b]].tMs) < ti + W || secs(fixes[ord[b]].tMs) - ti > opt.gapResetS) b = i;
            if (a == b) continue;
            const Fix &fa = fixes[ord[a]], &fb = fixes[ord[b]];
            const double d = Estimator::distanceM(fa.lat, fa.lon, fb.lat, fb.lon), dt = secs(fb.tMs) - secs(fa.tMs);
            fast[i] = dt > 0 && d * d > CHI2_2_99 * (var[ord[a]] + var[ord[b]]) && d / dt > opt.vehicleSpeed;
        }
        for (int i = 0; i < m; ++i) {
            const Mode md = mode0[ord[i]];
            if (md != Mode::Unknown) { dyn[i] = dynOf(md); continue; }
            int votes = 0, cnt = 0;
            for (int j = std::max(0, i - 2); j <= std::min(m - 1, i + 2); ++j) { votes += fast[j]; ++cnt; }
            dyn[i] = 2 * votes > cnt ? 2 : 1;
        }
    }

    // ── 3. events: the used fixes and the refs, by time ──
    std::vector<Ev> ev;
    ev.reserve(m + refs.size());
    for (int i = 0; i < m; ++i) { Ev e; e.t = secs(fixes[ord[i]].tMs); e.lat = fixes[ord[i]].lat; e.lon = fixes[ord[i]].lon; e.var = var[ord[i]]; e.fix = ord[i]; e.dyn = dyn[i]; ev.push_back(e); }
    for (int r = 0; r < refs.size(); ++r) {
        const Ref &rf = refs[r];
        if (!std::isfinite(rf.lat) || !std::isfinite(rf.lon) || !(rf.sd > 0)) continue;
        Ev e; e.t = secs(rf.tMs); e.lat = rf.lat; e.lon = rf.lon; e.var = rf.sd * rf.sd; e.ref = r; e.dyn = -1; ev.push_back(e);
    }
    std::stable_sort(ev.begin(), ev.end(), [](const Ev &a, const Ev &b) { return a.t < b.t; });
    {   // a ref moves like the fix before it (or after it)
        int last = -1;
        for (Ev &e : ev) { if (e.fix >= 0) last = e.dyn; else if (last >= 0) e.dyn = last; }
        last = 1;
        for (int k = int(ev.size()) - 1; k >= 0; --k) { if (ev[k].fix >= 0) last = ev[k].dyn; else if (ev[k].dyn < 0) ev[k].dyn = last; }
    }

    // ── 4. segments: forward filter, then RTS ──
    const int N = int(ev.size());
    std::vector<Node> nd(N);
    int s = 0;
    while (s < N) {
        const Estimator::Frame fr(ev[s].lat, ev[s].lon);
        auto zOf = [&](const Ev &e, double z[2]) { z[0] = fr.x(e.lon); z[1] = fr.y(e.lat); };
        // diffuse prior at the first event, then its update
        {
            Node &a = nd[s];
            double z[2]; zOf(ev[s], z);
            for (int ax = 0; ax < 2; ++ax) { a.xp[ax][0] = z[ax]; a.xp[ax][1] = 0; a.xp[ax][2] = 0; }
            a.Pp = M3{{{opt.initPosSd * opt.initPosSd, 0, 0}, {0, sigmaV2(motionOf(ev[s].dyn, opt)), 0}, {0, 0, opt.sigmaB * opt.sigmaB}}};
            a.tr = Tr();
        }
        int e = s, gatedRun = 0, firstGated = -1, fixCount = 0, refCount = 0;
        double lastInfoT = ev[s].t, lastFixT = -1e300;
        bool restart = false;
        for (int k = s; k < N; ++k) {
            Node &c = nd[k];
            const Ev &E = ev[k];
            if (k > s) {
                const double dt = E.t - ev[k - 1].t;
                if (E.t - lastInfoT > opt.gapResetS) break;            // gap: a new segment from here
                const int d = std::max(E.dyn, ev[k - 1].dyn);
                c.tr = transition(dt, motionOf(d, opt), opt);
                const Node &p = nd[k - 1];
                for (int ax = 0; ax < 2; ++ax) {
                    c.xp[ax][0] = p.xf[ax][0] + c.tr.a * p.xf[ax][1];
                    c.xp[ax][1] = c.tr.e * p.xf[ax][1];
                    c.xp[ax][2] = c.tr.phi * p.xf[ax][2];
                }
                c.Pp = predictCov(p.Pf, c.tr);
            }
            e = k + 1;
            // update: fix H = [1 0 1] (z = p + b + w), ref H = [1 0 0] (z = p + w)
            const double hb = E.fix >= 0 ? 1.0 : 0.0;
            double R = E.var;
            if (E.fix >= 0) {
                double w = std::max(E.var - opt.sigmaB * opt.sigmaB, 0.25);      // white part (≥ 0.5 m)
                if (opt.whiteTauS > 0 && lastFixT > -1e299) {
                    const double rho = std::exp(-(E.t - lastFixT) / opt.whiteTauS);
                    w *= std::min(MAX_INFLATE, (1 + rho) / std::max(1e-12, 1 - rho));
                }
                R = w;
            }
            const M3 &P = c.Pp;
            const double PH[3] = {P.a[0][0] + hb * P.a[0][2], P.a[1][0] + hb * P.a[1][2], P.a[2][0] + hb * P.a[2][2]};
            const double S = PH[0] + hb * PH[2] + R;
            double z[2]; zOf(E, z);
            const double nu[2] = {z[0] - (c.xp[0][0] + hb * c.xp[0][2]), z[1] - (c.xp[1][0] + hb * c.xp[1][2])};
            const double nis = (nu[0] * nu[0] + nu[1] * nu[1]) / S;
            c.gated = k > s && nis > opt.gateChi2;
            if (c.gated) {
                for (int ax = 0; ax < 2; ++ax) for (int j = 0; j < 3; ++j) c.xf[ax][j] = c.xp[ax][j];
                c.Pf = c.Pp;
                if (E.fix >= 0) {
                    // a spike disagrees with the next fix too; a fix the next one confirms is a manoeuvre the
                    // motion model did not expect: restart here rather than flag a real move
                    int k2 = k + 1;
                    while (k2 < N && ev[k2].fix < 0) ++k2;
                    if (k2 < N && ev[k2].t - E.t <= opt.gapResetS) {
                        const double dt = ev[k2].t - E.t;
                        const Motion &mo = motionOf(std::max(E.dyn, ev[k2].dyn), opt);
                        const Tr t2 = transition(dt, mo, opt);
                        const double allow = t2.qpp + t2.a * t2.a * sigmaV2(mo);
                        const double d2 = std::pow(fr.x(ev[k2].lon) - z[0], 2) + std::pow(fr.y(ev[k2].lat) - z[1], 2);
                        if (d2 <= opt.gateChi2 * (E.var + ev[k2].var + allow)) { e = k; restart = true; break; }
                    }
                    if (++gatedRun == 1) firstGated = k;
                    if (gatedRun >= opt.maxGated) { e = firstGated; restart = true; break; }
                }
                continue;
            }
            if (E.fix >= 0) { gatedRun = 0; lastFixT = E.t; ++fixCount; } else ++refCount;
            lastInfoT = E.t;
            const double K[3] = {PH[0] / S, PH[1] / S, PH[2] / S};
            for (int ax = 0; ax < 2; ++ax) for (int j = 0; j < 3; ++j) c.xf[ax][j] = c.xp[ax][j] + K[j] * nu[ax];
            // Joseph form: (I − K·H)·P·(I − K·H)ᵀ + R·K·Kᵀ
            const double H[3] = {1, 0, hb};
            double A[3][3], AP[3][3];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) A[i][j] = (i == j) - K[i] * H[j];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) AP[i][j] = A[i][0] * P.a[0][j] + A[i][1] * P.a[1][j] + A[i][2] * P.a[2][j];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
                c.Pf.a[i][j] = AP[i][0] * A[j][0] + AP[i][1] * A[j][1] + AP[i][2] * A[j][2] + R * K[i] * K[j];
            guard(c.Pf);
        }
        if (restart) {
            ++st.restarts;
            // trailing gated fixes before firstGated stay outliers; a lone fix the next ones all reject is one too
            if (fixCount == 1 && refCount == 0) for (int k = s; k < e; ++k) if (ev[k].fix >= 0 && !nd[k].gated) nd[k].gated = true;
        }
        // RTS backward pass over [s, e)
        Node &last = nd[e - 1];
        for (int ax = 0; ax < 2; ++ax) for (int j = 0; j < 3; ++j) last.xs[ax][j] = last.xf[ax][j];
        last.Ps = last.Pf;
        for (int k = e - 2; k >= s; --k) {
            Node &c = nd[k];
            const Node &nx = nd[k + 1];
            const Tr &t = nx.tr;
            // C = Pf·Fᵀ·Pp⁻¹ ⇒ Cᵀ = Pp⁻¹·(F·Pf)
            M3 FP, Ct;
            const double F[3][3] = {{1, t.a, 0}, {0, t.e, 0}, {0, 0, t.phi}};
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) FP.a[i][j] = F[i][0] * c.Pf.a[0][j] + F[i][1] * c.Pf.a[1][j] + F[i][2] * c.Pf.a[2][j];
            spdSolve(nx.Pp, FP, Ct);
            for (int ax = 0; ax < 2; ++ax) {
                double d[3];
                for (int j = 0; j < 3; ++j) d[j] = nx.xs[ax][j] - nx.xp[ax][j];
                for (int i = 0; i < 3; ++i) c.xs[ax][i] = c.xf[ax][i] + Ct.a[0][i] * d[0] + Ct.a[1][i] * d[1] + Ct.a[2][i] * d[2];
            }
            double D[3][3], CD[3][3];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) D[i][j] = nx.Ps.a[i][j] - nx.Pp.a[i][j];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) CD[i][j] = Ct.a[0][i] * D[0][j] + Ct.a[1][i] * D[1][j] + Ct.a[2][i] * D[2][j];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
                c.Ps.a[i][j] = c.Pf.a[i][j] + CD[i][0] * Ct.a[0][j] + CD[i][1] * Ct.a[1][j] + CD[i][2] * Ct.a[2][j];
            guard(c.Ps);
        }
        // outputs
        bool first = true;
        for (int k = s; k < e; ++k) {
            const Ev &E = ev[k];
            const Node &c = nd[k];
            if (E.ref >= 0) { if (c.gated) ++st.refsGated; else ++st.refsUsed; continue; }
            Out &o = out[E.fix];
            o.lon = fr.lon(c.xs[0][0]); o.lat = fr.lat(c.xs[1][0]);
            o.cxx = o.cyy = c.Ps.a[0][0]; o.cxy = 0;
            o.acc = ACC68 * std::sqrt(std::max(0.0, c.Ps.a[0][0]));
            o.ve = c.xs[0][1]; o.vn = c.xs[1][1];
            o.biasE = c.xs[0][2]; o.biasN = c.xs[1][2];
            o.mode = modeOfDyn(E.dyn);
            o.outlier = c.gated;
            o.segmentStart = first; first = false;
            if (c.gated) ++st.outliers;
        }
        ++st.segments;
        s = e;
    }
    for (int i = 0; i < n; ++i) if (dupOf[i] >= 0) { out[i] = out[dupOf[i]]; out[i].segmentStart = false; }
    if (stats) *stats = st;
    return out;
}

double calibrateScale(const QList<Fix> &fixes, double refLat, double refLon, double accFloor)
{
    std::vector<double> err, acc;
    for (const Fix &f : fixes) {
        if (!(f.acc > 0) || !std::isfinite(f.lat) || !std::isfinite(f.lon)) continue;
        err.push_back(Estimator::distanceM(f.lat, f.lon, refLat, refLon)); acc.push_back(f.acc);
    }
    if (err.empty()) return 0;
    const double target = RAYLEIGH_MED / ACC68;          // median(err / honest 68 % radius)
    auto medRatio = [&](double k) {
        std::vector<double> r(err.size());
        for (size_t i = 0; i < err.size(); ++i) r[i] = err[i] / std::sqrt(k * k * acc[i] * acc[i] + accFloor * accFloor);
        return median(r);
    };
    if (accFloor <= 0) return medRatio(1) / target;
    if (medRatio(0) <= target) return 0;                  // the floor alone explains the errors
    double lo = 1e-3, hi = 1e3;                           // medRatio decreases in k: bisect in log k
    for (int it = 0; it < 80; ++it) { const double mid = std::sqrt(lo * hi); (medRatio(mid) > target ? lo : hi) = mid; }
    return std::sqrt(lo * hi);
}

QList<AllanPoint> allanDeviation(const QList<Fix> &fixes, double baseS, int maxOctaves)
{
    QList<AllanPoint> res;
    if (fixes.size() < 4 || !(baseS > 0)) return res;
    qint64 t0 = LLONG_MAX, t1 = LLONG_MIN;
    double la = 0, lo = 0;
    for (const Fix &f : fixes) { t0 = std::min(t0, f.tMs); t1 = std::max(t1, f.tMs); la += f.lat; lo += f.lon; }
    const Estimator::Frame fr(la / fixes.size(), lo / fixes.size());
    const qint64 nb64 = qint64((t1 - t0) / 1000.0 / baseS) + 1;
    if (nb64 > 50000000) return res;
    const int nb = int(nb64);
    std::vector<double> se(nb, 0), sn(nb, 0);
    std::vector<int> c(nb, 0);
    for (const Fix &f : fixes) {
        const int b = std::min(nb - 1, int((f.tMs - t0) / 1000.0 / baseS));
        se[b] += fr.x(f.lon); sn[b] += fr.y(f.lat); ++c[b];
    }
    // prefix sums of the filled bins' means
    std::vector<double> PE(nb + 1, 0), PN(nb + 1, 0);
    std::vector<int> PC(nb + 1, 0);
    for (int b = 0; b < nb; ++b) {
        PE[b + 1] = PE[b] + (c[b] ? se[b] / c[b] : 0); PN[b + 1] = PN[b] + (c[b] ? sn[b] / c[b] : 0); PC[b + 1] = PC[b] + (c[b] > 0);
    }
    for (int j = 0, mm = 1; j <= maxOctaves && 2 * mm <= nb; ++j, mm *= 2) {
        double ae = 0, an = 0; int pairs = 0;
        for (int i = 0; i + 2 * mm <= nb; ++i) {
            const int ca = PC[i + mm] - PC[i], cb = PC[i + 2 * mm] - PC[i + mm];
            if (2 * ca < mm || 2 * cb < mm || ca == 0 || cb == 0) continue;
            const double de = (PE[i + 2 * mm] - PE[i + mm]) / cb - (PE[i + mm] - PE[i]) / ca;
            const double dn = (PN[i + 2 * mm] - PN[i + mm]) / cb - (PN[i + mm] - PN[i]) / ca;
            ae += de * de; an += dn * dn; ++pairs;
        }
        if (pairs == 0) continue;
        AllanPoint p; p.tauS = mm * baseS; p.adevE = std::sqrt(ae / (2 * pairs)); p.adevN = std::sqrt(an / (2 * pairs)); p.pairs = pairs;
        res << p;
    }
    return res;
}

} // namespace TrackSmoother
