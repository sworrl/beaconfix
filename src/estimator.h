#pragma once
// Position estimation from signal-strength samples, with a graded confidence (docs/GRADING.md).
//
// Model (per cluster k of samples taken within a few metres of each other):
//   y_k = P0 + g_dev − 10·n·log10(d_k) + ε_k,   d_k = √(|p − q_k|² + h²)
//   P0  level at 1 m (Gaussian prior per band), n the path-loss exponent (Gaussian prior),
//   g_dev the hearing device's offset (calibrated across APs, this host = 0), h the AP height,
//   ε  shadowing: σ² = σ0²·(ρ_in + (1 − ρ_in)/m_k) + (∂model/∂p)²·a_k²  (the observer's own
//      fix error a_k enters as an errors-in-variables term, so near samples with poor fixes
//      carry little information).
//
// Pipeline (estimator.cpp):
//   1. samples → clusters (radius max(15 m, median fix accuracy)); fixes worse than 100 m only
//      when nothing better exists ("heard near");
//   2. a grid posterior over the position with (P0, n) integrated out in closed form, plus the
//      likelihood of NOT hearing the AP where we scanned (misses) and Wi-Fi RTT ranges → global
//      optimum, number of modes, the 95 % highest-posterior region;
//   3. Levenberg–Marquardt polish (robust IRLS: a Gaussian + uniform outlier mixture, EM weights, at the
//      nominal and then a MAD-pooled scale) from the
//      grid modes and the mirror image across the samples' principal axis;
//   4. covariance: Laplace × max(1, τ²) × design effect of correlated shadowing (Gudmundson) +
//      correlated fix error / sessions, floored by the Cramér–Rao bound, the leave-one-cluster-out
//      jackknife and a cluster bootstrap, then scaled by the anchor calibration κ²;
//   5. metrics (geometry, evidence, fit, stability, freshness, external agreement) → a 0–100
//      score and a letter grade (A–F, R = region only, M = mobile) with hysteresis.
//
// Everything here is pure (no Qt I/O, no state) and deterministic, so the Kotlin twin
// (android/…/estimate/Estimator.kt) reproduces it exactly: tests/fixtures/estimator_golden.json.
#include <QHash>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace Estimator {

inline constexpr int kVersion = 2;       // stored in kv 'estimator_version': estimates older than this are recomputed

struct Obs {
    double lat = 0, lon = 0;          // where the observer was
    double acc = 30;                  // its fix accuracy (m, 68 % radius as Android/GeoClue report it)
    int    dbm = -100;
    qint64 t = 0;                     // seconds since the epoch (0 = unknown)
    double weight = 1;                // extra factor: 0.3 for "travels with you" statuses etc.
    QString device;                   // who heard it ("" = this host)
    double rangeM = -1, rangeSd = 0;  // optional Wi-Fi RTT (FTM) range to the AP and its 1-σ (m)
};

// Where we scanned without hearing the AP (docs/GRADING.md §1.7)
struct Miss { double lat = 0, lon = 0; int count = 1; };

// A position somebody else claims (WiGLE / Apple / BeaconDB / the AP's own LCI): only compared, never fitted
struct External { bool has = false; double lat = 0, lon = 0, acc = 50; QString source; };

struct Fit {
    bool   valid = false;             // a position is claimed (kind fix or region)
    QString kind = QStringLiteral("none");   // fix | region | mobile | none
    double lat = 0, lon = 0;
    double acc = 0;                   // circular 1-σ (m): semi-major axis of the 1-σ ellipse (compat)
    double semiMajor = 0, semiMinor = 0, orientDeg = 0;   // 1-σ error ellipse (orient: bearing of the major axis)
    double cxx = 0, cxy = 0, cyy = 0; // position covariance (m², x = east, y = north)
    double r95 = 0, cep50 = 0;        // radius holding 95 % / 50 % of the position probability
    double pWithin25 = 0;             // P(|error| < 25 m)
    double rms = 0;                   // weighted residual RMS in dB
    double p0 = -40;                  // fitted reference level at 1 m
    double pathloss = 2.4;            // path-loss exponent (fitted with a prior)
    bool   fittedN = false;
    int    n = 0;                     // samples used
    int    vantage = 0;               // clusters (distinct places)
    int    rejected = 0;              // samples in clusters the robust fit treats as outliers
    QString quality = QStringLiteral("none");   // compat: good | fair | poor | none
    qint64 updated = 0;               // seconds since the epoch
    // ── grading metrics (docs/GRADING.md §2) ──
    double score = 0;                 // 0–100
    QString grade;                    // A–F · R (region only) · M (mobile) · "" (none)
    QString pendingGrade;             // hysteresis: a letter change waiting for confirmation
    double rssDop = 0;                // √tr(M⁻¹) (m), geometry-only dilution of precision
    double crlbR95 = 0;               // R95 the Cramér–Rao bound allows with σ0
    double rbar = 1;                  // mean resultant length of the bearings AP → places (0 surrounded, 1 one side)
    double maxGapDeg = 360;           // largest gap between those bearings
    bool   inHull = false;            // estimate inside the convex hull of the places
    double linRatio = 0;              // μ2/μ1 of the places' scatter (0 = a straight line)
    double chi2nu = 0;                // reduced χ² of the inliers
    double sigmaDb = 0;               // robust residual scale (dB)
    double outlierFrac = 0;
    double ess = 0;                   // Kish effective number of places
    int    sessions = 0, devices = 0;
    double spearman = 0;              // rank correlation of level and log-distance (should be < −0.5)
    double p0RangeCorr = 0;           // |corr(P0, range)|: ~1 = no close sample, range scale unidentified
    double dminRatio = 0;             // closest place / median place distance
    bool   ambiguous = false;         // a mirror / second mode explains the data about as well
    double altLat = 0, altLon = 0;    // that alternative (ghost marker)
    int    modes = 0;                 // significant posterior modes
    double driftD2 = 0;               // smoothed Mahalanobis² of the move between refits
    double extD2 = -1;                // Mahalanobis² against the external position (−1 none)
    double jackMax = 0;               // largest leave-one-place-out shift (m)
    double nisEwma = 0;               // smoothed normalised innovation² of the incremental updates (0 = none yet)
    double fadingDb = 0;              // mean within-place spread of the level (fast fading)
    bool   moved = false;             // the AP moved: only the recent epoch is fitted
    qint64 newest = 0;                // newest sample time
    double suggestLat = 0, suggestLon = 0, suggestGain = 0;   // "sample here next" (gain in nats; 0 none)
    double cP = 0, cG = 0, cE = 0, cF = 0, cFfit = 0, cS = 0, cT = 0, cX = -1;   // score components (cX −1: no external)
    QString groupRef;                 // multi-BSSID group this fit was pooled over ("" none)
    int    groupSize = 0;
};

struct Options {
    double defaultN = 2.4;            // prior mean of n
    double nSd = 0.5;                 // prior σ of n
    double p0Mean = -40, p0Sd = 8;    // prior of P0 (dBm at 1 m)
    double sigmaDb = 6.0;             // σ0: shadowing
    double outlierPrior = 0.1;        // robust fit: prior share of gross outliers (Gaussian + uniform mixture)
    double outlierSpanDb = 80;        // … spread uniformly over this many dB
    double heightM = 3.0;             // AP height above the observer
    double clusterMinM = 15;          // minimum cluster radius
    int    maxClusters = 120;
    double ageTauDays = 180;          // sample age weight exp(−age/τ), floored at 0.15
    double maxAcc = 300;              // coarser fixes are ignored
    double geomAcc = 100;             // fixes coarser than this only when nothing better exists
    double rhoIn = 0.6;               // shadowing correlation inside one cluster
    double shadowCorrM = 30;          // Gudmundson decorrelation distance
    double rangePriorM = 150;         // position prior exp(−distance to the nearest place / this)
    double missFloor = 0.2;           // P(not heard) even when in range (scan misses)
    double sensitivity = -92;         // detection floor (dBm)
    int    gridN = 48;
    double minHalfWidth = 120, maxHalfWidth = 600;
    int    bootstrap = 24;            // cluster bootstrap replicates (needs ≥ 6 places; 0 = off)
    int    jackMaxK = 60;             // jackknife up to this many places (0 = off)
    int    maxIter = 40;
    double fixMaxR95 = 150;           // a "fix" needs R95 and the CRLB R95 below this
    double maxR95 = 1500;             // beyond this nothing is claimed
    double kappa = 1.0;               // covariance calibration (σ multiplier) from the anchors
    quint64 seed = 0x5EEDBEAC0F1ULL;
    // kept for callers of the previous engine
    int    minSamples = 1;
};

struct Context {
    QHash<QString, double> deviceOffset;   // dB the device hears louder than this host
    QList<Miss> misses;
    External external;
    bool   hasPrev = false;
    Fit    prev;                      // the previous fit (drift, hysteresis)
    bool   mobile = false;            // the locator already knows it travels with us
    bool   noEpochSplit = false;      // internal: no moved-AP check
};

// A known transmitter for self-location
struct Known {
    double lat = 0, lon = 0, acc = 25; int dbm = -100; double p0 = -40, pathloss = 2.4; bool haveModel = false; QString bssid;
    double cxx = 0, cxy = 0, cyy = 0; // its position covariance (0 = use acc)
    double weight = 1;                // by grade
};

struct SelfFix {
    bool valid = false; double lat = 0, lon = 0, acc = 0, rms = 0; int used = 0, rejected = 0;
    int excluded = 0;                 // APs removed by the integrity check
    QString integrity;                // ok | repaired | failed | unverified
    double r95 = 0;
};

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

// Solve A·x = b (k ≤ 4) by Gaussian elimination with partial pivoting
bool solve(double A[4][4], double b[4], int k, double out[4]);
// 2×2 symmetric inverse [[a b][b d]] → [o0 o1; o1 o2]
bool invert2(double a, double b, double d, double out[3]);
// 1-σ ellipse of a 2×2 covariance
void ellipse(double cxx, double cxy, double cyy, double *major, double *minor, double *orientDeg);
// P(|X| < r) for X ~ N(0, diag(s1², s2²)), and the radius for probability p
double probWithin(double s1, double s2, double r);
double radiusFor(double s1, double s2, double p);
double normCdf(double z);
// Distinct places, as the fitter clusters them (for callers that want the count without fitting)
int vantageCount(const QList<Obs> &obs, double clusterMinM = 15.0);

// The AP fitter
Fit fitAp(const QList<Obs> &samples, qint64 now = 0, const Options &opt = Options(), const Context &ctx = Context());
// Score and letter from the metrics already in f (with hysteresis against prev)
void grade(Fit &f, const Fit *prev);
// Incremental update between refits: a 2-D Kalman step along the radial direction
Fit update(const Fit &prev, const Obs &o, const Options &opt = Options());
// Self-location from beacons with known positions (with an integrity check)
SelfFix selfLocate(const QList<Known> &known, const Options &opt = Options());
// Letter for a score (no hysteresis)
QString letterFor(double score);
QString qualityFor(const QString &grade);

} // namespace Estimator
