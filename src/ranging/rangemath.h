// BeaconFix ranging maths — device-to-device distance from Wi-Fi RTT, BLE RSSI (both ways) and
// differential Wi-Fi RSSI, fused into the relative position of a peer. Pure: no I/O, no time,
// no random numbers. Mirrors android/…/ranging/RangeMath.kt function by function; both must
// reproduce the vectors in docs/RANGING.md §11 to 1e-9 relative.
//
// Units: metres, dBm/dB, seconds, degrees (bearings clockwise from north).
#pragma once
#include <QByteArray>
#include <QString>
#include <cmath>
#include <vector>

namespace RangeMath {

// ── constants (docs/RANGING.md §3) ───────────────────────────────────────────
constexpr double kLn10 = 2.302585092994046;
constexpr double kDbPerNeper = 4.342944819032518;   // 10 / ln 10
constexpr double kRayleighDbStd = 5.570043140052503; // std of 10·log10(Exp(1))
constexpr double kShadowWifi = 5.0, kShadowBle = 6.0, kDevGain = 1.5, kDecorrM = 3.0;
constexpr double kNWifi = 2.4, kNBle = 2.0;
constexpr double kSigmaNlos = 1.0, kRttFloor = 0.3;
constexpr double kZ84 = 0.9944578832097535;          // Φ⁻¹(0.84)
// A still device's multipath fade is frozen per channel; averaged over BLE's 3 advertising channels
// it is a constant error of variance 5.57²/3 dB² that belongs in the link offset, not in the noise.
constexpr double kFrozenFadeVar = kRayleighDbStd * kRayleighDbStd / 3.0;
constexpr double kOffsetVar0 = 36.0 + kShadowBle * kShadowBle + kFrozenFadeVar;   // σ_P0² + σ_s² + frozen fade
// Drift of a still link's offset (dB²/s): σ 3 dB per √hour. People moving about the RV, doors, temperature and the
// phone's orientation change a static link by several dB over an hour. The first value (10⁻⁴, 0.6 dB per √hour) let a
// filter that had run for hours trust its offsets so much that it read 1.0 m [0.87, 1.15] for a true 0.6 m.
constexpr double kOffsetDrift = 2.5e-3;

double fsplDb(double dM, double fMHz);               // 20·log10(d) + 20·log10(f) − 27.55
double priorP0Ble(int txPower);                      // t − 41, or −59 when t == 127 (unknown)
double priorP0Wifi(double fMHz);                     // −40 at 2.437 GHz, Friis-scaled for other bands
double modelLevel(double p0, double n, double dM);   // p0 − 10·n·log10(d)
double modelDistance(double p0, double n, double level);
double quantile7(std::vector<double> v, double p);   // "type 7" quantile (sorts a copy)
double median(const std::vector<double> &v);
double chi2Quantile(double m, double z);             // Wilson–Hilferty: m·(1 − 2/(9m) + z·√(2/(9m)))³

// ── §3.2 level from faded samples ────────────────────────────────────────────
struct Level { bool valid = false; double dbm = 0, sigma = 0, nEff = 0; int n = 0; };
Level levelFromSamples(const std::vector<double> &rssi, int channels, double spanS, bool moving);

// ── §5.2 range filter: x = [u = log10 d, b_1 … b_M, e_c] ──────────────────────
// b_k: offset of RSSI link k (dB). e_c: error of the RTT pair offset passed to updateRtt (m),
// so a calibrated offset carries its own uncertainty instead of being treated as exact.
class RangeFilter {
public:
    static constexpr int kMax = 5;                   // u + up to 3 RSSI offsets + the RTT offset error
    explicit RangeFilter(int offsets = 2, double u0 = 1.0, double puu0 = 1.0, double offsetVar0 = kOffsetVar0, double rttOffsetVar0 = 0.25);
    void predict(double dtS, bool moving);
    // RSSI of link k (0-based offset index): level L ± sigmaL under model (p0, n). Returns the Huber-normalised innovation.
    double updateRssi(int k, double level, double sigmaL, double p0, double n);
    // Wi-Fi RTT range (m) with its 1-σ and the pair's constant offset; asymmetric (NLOS) robust IEKF.
    double updateRtt(double rangeM, double sigmaM, double offsetM);
    double updateLogRange(double u, double sigmaU);
    void resetOffset(int k, double value, double var);
    void resetRttOffset(double value, double var);   // after a calibration: the offset error and its variance
    double rttOffsetError() const { return x[nx - 1]; }
    double rttOffsetVar() const { return P[nx - 1][nx - 1]; }
    double u() const { return x[0]; }
    double puu() const { return P[0][0]; }
    double offset(int k) const { return x[1 + k]; }
    double offsetVar(int k) const { return P[1 + k][1 + k]; }
    double distanceM() const;
    double sigmaM() const;
    double lowM() const;
    double highM() const;
    int dim() const { return nx; }
    double offsetVarCap = kOffsetVar0;
    double x[kMax] = {0, 0, 0, 0, 0};
    double P[kMax][kMax] = {};
private:
    double scalarUpdate(const double *H, double y, double R, double huberK);
    int nx;
};

// ── §5.3 recursive least squares for [P0, n] ──────────────────────────────────
struct Rls2 {
    double p0 = -59, n = 2.0;
    double S[2][2] = {{64, 0}, {0, 0.25}};
    double lambda = 0.999;
    Rls2() = default;
    Rls2(double p0Prior, double nPrior, double varP0 = 64, double varN = 0.25, double lam = 0.999);
    void update(double log10d, double level, double R);
};
// Batch weighted least squares with the same Gaussian prior (λ = 1): anchors' calibration fit.
struct PathLossFit { double p0 = 0, n = 0, varP0 = 0, varN = 0, covP0N = 0, rmsDb = 0; int used = 0; };
struct CalPoint { double distM = 1, level = -60, weight = 1; };
PathLossFit fitPathLoss(const std::vector<CalPoint> &pts, double p0Prior, double nPrior, double varP0 = 64, double varN = 0.25, double noiseDb = 4.0);

// ── §5.4(a) fingerprint distance from shared-AP levels ────────────────────────
struct DiffPair { QString group; double levelA = 0, sigmaA = kDbPerNeper, levelB = 0, sigmaB = kDbPerNeper; };
struct Fingerprint {
    bool ok = false; int groups = 0;
    double gainDb = 0, d2 = 0, noise2 = 0, excess = 0;
    double deltaM = 0, lowM = 0, highM = 0;
};
Fingerprint fingerprintDistance(const std::vector<DiffPair> &pairs);
double fingerprintExcessModel(double deltaM);              // E(δ)
double fingerprintInvert(double target);                   // δ with E(δ) = target
double fingerprintLogLik(const Fingerprint &fp, double r); // −(m/2)·ln s − m·D²/(2s),  s = E(r) + noise²

// ── §5.4(b) geometric differential solve ─────────────────────────────────────
struct GeoPair { QString group; double apE = 0, apN = 0, pathloss = kNWifi, sigmaPos = 10, delta = 0, sigmaDelta = 6.1; };
struct GeoSolve {
    bool ok = false; int used = 0, iterations = 0;
    double dE = 0, dN = 0, gainDb = 0, chi2 = 0, spreadDeg = 0;
    double cov[3][3] = {};
};
GeoSolve solveDifferential(const std::vector<GeoPair> &pairs);

// ── §5.5 relative-position posterior on a polar grid ─────────────────────────
struct Gauss2 { bool valid = false; double muE = 0, muN = 0, sEE = 0, sEN = 0, sNN = 0; };
struct RelInput {
    bool haveRange = false; double u = 0, puu = 0;         // range filter marginal
    bool haveFp = false; Fingerprint fp;                    // §5.4(a)
    Gauss2 geo;                                             // §5.4(b)
    Gauss2 fix;                                             // fix_B − fix_A
};
struct RelOutput {
    bool valid = false;
    double distanceM = 0, lowM = 0, highM = 0, sigmaM = 0;
    bool haveBearing = false; double bearingDeg = 0, bearingSigmaDeg = 0, resultantLength = 0;
    QString cls = QStringLiteral("unknown");
};
RelOutput relativePosterior(const RelInput &in);
QString classify(bool evidence, double lowM, double highM);

// ── §9.1 BLE advertisement ───────────────────────────────────────────────────
constexpr const char *kBleServiceUuid = "28c9f0bf-a089-4a95-b632-5e8ede1b03b6";
enum BleKind { KindDesktop = 0, KindAndroid = 1, KindLaptop = 2, KindPi = 3, KindGnss = 4, KindOther = 7 };
struct BleAdvert { bool valid = false; QByteArray tag; int txPower = 127; bool rtt = false, api = false, calibrating = false; int kind = 7, version = 0; };
QByteArray bleTag(const QString &identityId, qint64 unixSeconds);   // first 8 bytes of SHA-256("beaconfix-ble-v1|id|⌊t/900⌋")
int bleFlags(bool rtt, bool api, int kind, bool calibrating);
QByteArray bleServiceData(const QByteArray &tag, int txPower, int flags);
BleAdvert parseServiceData(const QByteArray &data);

} // namespace RangeMath
