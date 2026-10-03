#pragma once
// Smoothing a device's own GPS track: the smoothed fixes are the vantage points the AP
// estimator (estimator.h) fits from, so their error and its covariance matter as much as the
// position itself.
//
// Model (per device; a local metric frame per segment, x = east, y = north). Per axis the state is
//   s = [p, v, b]          position, velocity, measurement bias
//   v  integrated Ornstein–Uhlenbeck: dv = −v/τ_v·dt + σ_a·dW, discretised exactly (τ_v → ∞ is the
//      continuous white-noise-acceleration model). σ_a, τ_v by motion mode (from the fix's source):
//        stationary  0.02 m/s², 5 s  : v ≈ 0, the position diffuses σ_a²τ_v² = 0.01 m²/s (±1 m in
//                                      100 s, ±6 m in an hour: a phone carried about inside an RV)
//        foot        0.7 m/s², 10 s  : σ_v = σ_a·√(τ_v/2) = 1.6 m/s
//        vehicle     3 m/s², 120 s   : σ_v = 23 m/s
//        unknown     foot, or vehicle where the fixes themselves move faster than 3 m/s (centre-free
//                    displacement between the fixes ≥ 30 s before and after, significant at χ²(2) 99 %,
//                    majority of 5 neighbours, so one spike cannot flip it). 91 % of a Pixel's fixes are
//                    plain "phone-gps", drives at a fix a minute included.
//      An interval takes the more dynamic mode of its two ends.
//   b  first-order Gauss–Markov, σ_b = 2.5 m, τ_b = 1200 s. A parked Pixel's Allan deviation (per axis)
//      is ~2.4 m at 1 min and flat at ~1.4 m from 8 to 64 min: the error wanders slowly, it is not white,
//      so averaging beyond a few minutes buys little. White + Gauss–Markov fits that curve best at
//      σ_w ≈ 2.3, σ_b ≈ 2.2 m, τ_b 1200–1800 s (τ_b = 300 s misses the flat tail: 5× the log error);
//      σ_b is rounded up for the constant part Allan cannot see (the parked phone's 3.5-h mean sat
//      5 m from the surveyed point). The bias is only observable through refs (below).
//   fix  z = p + b + w,  per-axis σ_tot = k_src·√((accScale·acc)² + accFloor²) / ACC68 (acc: the
//        reported 68 % radius). Reported accuracy is over-confident: a parked phone's error was inside
//        it only 11–37 % of the time (68 % if honest), and the deficit is concentrated at the small
//        values (against the surveyed point: acc ≤ 3.5 m ×2.3, 3.5–6 m ×1.9, 6–15 m ×1.2), i.e. a floor
//        added in quadrature (accFloor 5 m, accScale 1), not a factor (calibrateScale() measures either).
//        The bias carries σ_b² of σ_tot², w the rest (≥ 0.5 m); w is itself correlated over ~15 s (the
//        fused provider filters), so at spacing dt its variance is inflated by (1 + ρ)/(1 − ρ),
//        ρ = e^(−dt/15 s): sixty 1-s fixes are worth about four independent ones. Wi-Fi fixes k_src = 2,
//        IP fixes and anything coarser than maxAcc dropped.
//   ref  z = p + w, w ~ N(0, sd²) per axis: "the device was at this surveyed point" — no bias term, so
//        it observes the bias like a differential base station: at the surveyed RV the phone's current
//        error is learnt and carried, decaying with τ_b, into the walk that follows.
// Both axes share F, Q, H and R, so they share one 3×3 covariance (cxx = cyy, cxy = 0) and only the
// means differ: two 3-state filters for the price of one.
//
// Pipeline: sort; exact duplicates (same time and place: sync echoes) count once; Kalman forward pass
// (Joseph-form update, symmetrised, eigenvalues floored); a fix whose normalised innovation² exceeds
// χ²(2) 99.9 % = 13.8 is not used and flagged as an outlier — unless the next fix confirms it (within
// the same gate, allowing the motion model's displacement), which is a manoeuvre the model did not
// expect: the segment restarts there. maxGated rejections in a row restart it at the first of them; a
// lone fix that the following ones reject is an outlier too. No informative event for gapResetS: a new
// segment (no smoothing across a gap). Then a Rauch–Tung–Striebel backward pass over each segment.
//
// Pure (no Qt I/O, no state) and deterministic; ~1 µs per fix (a 9k-fix phone track: 5 ms).
#include <QList>
#include <QString>
#include <QtGlobal>

namespace TrackSmoother {

enum class Mode { Stationary, Foot, Vehicle, Unknown };

struct Fix {
    qint64 tMs = 0;                   // ms since the epoch
    double lat = 0, lon = 0;
    double acc = 30;                  // reported 68 % radius (m), as Android / GeoClue report it
    QString source;                   // phone-stationary | phone-foot | phone-vehicle | phone-gps | gps | wifi | ip | …
};

// "The device was at this surveyed point (±sd, per-axis 1-σ in m) at tMs"
struct Ref { qint64 tMs = 0; double lat = 0, lon = 0, sd = 1; };

struct Out {
    double lat = 0, lon = 0;          // smoothed position (the device, without the bias)
    double cxx = 0, cxy = 0, cyy = 0; // its covariance (m², x = east, y = north)
    double acc = 0;                   // the same as a 68 % radius (m), Fix.acc's convention
    double biasE = 0, biasN = 0;      // smoothed measurement bias (m): raw fix ≈ position + bias
    double ve = 0, vn = 0;            // smoothed velocity (m/s)
    Mode mode = Mode::Unknown;        // the motion mode used (Unknown resolved to Foot / Vehicle)
    bool outlier = false;             // failed the innovation gate: not used
    bool dropped = false;             // not used at all (IP, coarser than maxAcc, invalid): raw position and σ_tot
    bool segmentStart = false;        // first fix of a segment (after a gap, a restart, or the very first)
};

struct Motion { double sigmaA; double tauV; };   // σ_a (m/s²), velocity correlation time (s; ≤ 0 = ∞)

struct Options {
    double accScale = 1.0;            // multiplies the reported 68 % radius …
    double accFloor = 5.0;            // … and this 68 % radius (m) is added in quadrature
    double wifiScale = 2.0;           // k_src for Wi-Fi positioning fixes (Apple / BeaconDB / internal)
    bool   useIp = false;             // IP geolocation is kilometres off: dropped
    double maxAcc = 200;              // coarser fixes (reported 68 % radius, m) are dropped
    double sigmaB = 2.5, tauB = 1200; // bias: stationary σ per axis (m), correlation time (s)
    double whiteTauS = 15;            // correlation time of the "white" part (0 = truly white)
    Motion stationary{0.02, 5}, foot{0.7, 10}, vehicle{3.0, 120};
    double vehicleSpeed = 3.0;        // unknown-mode fixes moving faster than this (m/s) are vehicle
    double speedWindowS = 30;         // … measured between fixes this far before and after
    double gateChi2 = 13.815510557964274;   // χ²(2) 99.9 %
    int    maxGated = 3;              // consecutive rejections that mean "it moved": restart there
    double gapResetS = 600;           // no informative event for this long: new segment
    double initPosSd = 1000;          // diffuse prior of a segment's first position (m)
};

struct Stats { int segments = 0, outliers = 0, dropped = 0, duplicates = 0, restarts = 0, refsUsed = 0, refsGated = 0; };

// The motion mode a source name implies
Mode modeFor(const QString &source);
// Smooth one device's track: one Out per Fix, in the input order (sorted by time or not)
QList<Out> smooth(const QList<Fix> &fixes, const QList<Ref> &refs = {}, const Options &opt = Options(), Stats *stats = nullptr);
// How over-confident the reported accuracy is, from fixes known to have been at (refLat, refLon):
// the k for which √((k·acc)² + accFloor²) is an honest 68 % radius (median of err / radius at the
// Rayleigh median, 1.1774σ). accFloor 0: k = median(err/acc)/0.777. 0 when there is nothing to go on.
double calibrateScale(const QList<Fix> &fixes, double refLat, double refLon, double accFloor = 0);

// Overlapping Allan deviation of the east / north coordinates (m) at τ = baseS·2^j. Irregular samples
// are first averaged into baseS bins; a group needs half of its bins filled.
struct AllanPoint { double tauS = 0, adevE = 0, adevN = 0; int pairs = 0; };
QList<AllanPoint> allanDeviation(const QList<Fix> &fixes, double baseS = 60, int maxOctaves = 12);

} // namespace TrackSmoother
