// BeaconFix device ranging service (docs/RANGING.md §5, §7, §8): fuses everything a peer tells us
// (POST /api/v1/ranging: its Wi-Fi RTT bursts to our responder, what it hears of our BLE advert, its
// Wi-Fi scan, its fix) with what we hear of ITS BLE advert (BleLink) into one range filter per peer,
// then the relative-position posterior. One instance lives in the tray process.
#pragma once
#include "rangemath.h"
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <vector>

class Locator;
class BleLink;

class RangingService : public QObject {
    Q_OBJECT
public:
    explicit RangingService(Locator *loc, QObject *parent = nullptr);
    ~RangingService() override;

    void start();                                            // BLE advert + scan (tray only)
    BleLink *ble() const { return m_ble; }

    // GET /ranging/info
    QJsonObject info() const;
    static QJsonObject responderInfo();                      // /etc/beaconfix/rtt-responder.json ({} when not set up)
    // POST /ranging from an authenticated device → that device's estimate
    QJsonObject report(const QString &device, const QString &kind, const QJsonObject &body);
    // GET /ranging
    QJsonObject list() const;
    QJsonObject estimateJson(const QString &device) const;   // {} when we have nothing for it
    QString kindOf(const QString &device) const;             // from its BLE advert's kind bits ("" = never heard)
    // POST /ranging/calibrate: "these two are distanceM apart" for durationS seconds
    QJsonObject calibrate(const QString &device, double distanceM, int durationS);
    // Pairing (docs/RANGING.md §5.5 + pairing.h): the proximity object of a pairing request, the
    // requester's identity (its BLE tag) → {class, distanceM, lowM, highM, sigmaM, method[], evidence}
    QJsonObject pairingProximity(const QJsonObject &theirs, const QString &identityId) const;

    // The TX power we ask BlueZ for (dBm): the AX210 reports [−34, +7]; BleLink clamps it to the adapter's range.
    static constexpr int kOurBleTxDbm = 7;
    // An uncalibrated RTT pair: phone/responder offsets of a metre or two are normal and the Pixel ↔ AX210 pair
    // read ~10 m at 0.6 m (docs/RANGING.md §8), so the consider-state prior is σ 2 m until a calibration.
    static constexpr double kUncalRttOffsetVar = 4.0;

signals:
    void estimateChanged(const QString &device);

private:
    struct Peer {
        QString device, kind;
        RangeMath::RangeFilter f{2, 1.0, 1.0, RangeMath::kOffsetVar0, kUncalRttOffsetVar};   // link 0: the peer hears our advert · link 1: we hear the peer's
        RangeMath::Rls2 rlsDown{-59, 2.0}, rlsUp{-59, 2.0};
        // Both links' models as the last successful manual calibration left them (ranging.json rlsDownCal / rlsUpCal):
        // an RTT offset found stale puts them back (forgetSuspectLearning)
        RangeMath::Rls2 rlsDownCal{-59, 2.0}, rlsUpCal{-59, 2.0};
        bool haveCalModels = false;
        bool haveUpPrior = false;
        int upTx = 127;                                      // the TX power the peer's advert carries
        int downTx = 127;                                    // our TX power as the peer reports it (its fallback when our byte 8 says 127)
        double rttOffset = 0, rttOffsetVar = kUncalRttOffsetVar;
        bool calibrated = false; QString calibratedAt; double calDistM = 0;
        bool rttCal = false;                                 // a calibration measured the RTT pair offset (gates the automatic BLE learning)
        QJsonObject lastCal;                                 // the last calibration's outcome: {at, ok, text}
        qint64 lastPredictMs = 0, lastRttMs = 0, lastBleUpMs = 0, lastBleDownMs = 0, updatedMs = 0;
        bool moving = false, started = false;
        std::vector<double> winDown, winUp; qint64 winDownStartMs = 0, winUpStartMs = 0; bool firstDown = true, firstUp = true;
        int winUpFresh = 0;                                  // samples in winUp that BlueZ reported (the rest are BleLink's held repeats)
        int nRtt = 0, nBleDown = 0, nBleUp = 0, nWifiDiff = 0;   // since this process started (see m_startMs)
        qint64 prevRtt = 0, prevBleDown = 0, prevBleUp = 0, prevLastRttMs = 0;   // totals from earlier runs (ranging.json)
        QString rttState; qint64 rttStateMs = 0;             // the peer's own word on its RTT (RangingPost.rttState)
        RangeMath::Fingerprint fp; qint64 fpMs = 0;
        RangeMath::Gauss2 fix; qint64 fixMs = 0;
        RangeMath::GeoSolve geo; qint64 geoMs = 0;
        // calibration window
        qint64 calUntilMs = 0, calStartMs = 0; double calTarget = 0;
        std::vector<double> calRtt, calRttSigma, calDown, calUp; int calUpFresh = 0;
        RangeMath::RelOutput out; QStringList method;
        // Is the calibrated RTT offset still right? The last bursts' ranges minus the offset (m) while it is calibrated;
        // a median well below zero means the offset moved (rttOffsetStale): RTT then stays out until a recalibration.
        std::vector<double> rttImplied;
        bool rttStale = false; double rttStaleByM = 0; QString rttStaleAt;
        qint64 lastRttOutlierMs = 0;                         // the last RTT burst with |z| > 3 (learning waits 30 s after one)
        double holdU = 0; qint64 holdSinceMs = 0;            // the range estimate has stayed within ±12 % since then
    };
    void onBleSample(const QByteArray &tag, int rssi, int txPower, int kind, int flags, qint64 timeMs, const QString &address, bool held);
    QString resolveTag(const QByteArray &tag, int kind, qint64 nowMs, bool *own);
    Peer &peer(const QString &device, const QString &kind);
    void advance(Peer &p, qint64 tMs);
    void flushDown(Peer &p, qint64 nowMs, bool force);
    void flushUp(Peer &p, qint64 nowMs, bool force);
    void finishCalibration(Peer &p);
    void recompute(Peer &p);
    QJsonObject toJson(const Peer &p) const;
    void wifiDiff(Peer &p, const QJsonArray &theirWifi, qint64 nowMs);
    void updateAdvertFlags();
    void load();
    void save() const;
    // The RTT pair offset was measured by a calibration (a BLE-only calibration leaves it at the wide prior)
    static bool rttCalibrated(const Peer &p) { return p.calibrated && p.rttCal && !p.rttStale && p.rttOffsetVar < 1.0; }
    // RTT-supervised BLE learning (§8 "Automatic"): a calibrated, current RTT offset, a tight range from RTT seen in the
    // last 30 s that has HELD (within ±12 %) for 30 s, and no RTT outlier in the last 30 s. A range sliding towards 0 m
    // under biased bursts is tight too, and it rewrote a manual calibration's BLE models.
    bool learnOk(Peer &p, qint64 nowMs) const;
    void noteRtt(Peer &p, double impliedM, qint64 nowMs);
    int downTxOf(const Peer &p) const { return m_ourTx != 127 ? m_ourTx : p.downTx; }   // the TX power behind the peer's down link
public:
    // Calibration RTT reduction: size of the densest [x, x + width] cluster, *center = its median, [*lo, *hi] = the
    // cluster's values (its inliers; see the .cpp).
    static int rttCluster(std::vector<double> v, double width, double *center, double *lo = nullptr, double *hi = nullptr);
    // The 1-σ of one calibration burst, from the cluster's inliers only: the larger of their RMS reported σ and their scatter.
    static double rttInlierSigma(const std::vector<double> &dist, const std::vector<double> &sigma, double lo, double hi);
    // A BLE level from samples of which only `fresh` are new: BleLink re-sends an unchanged RSSI once a second (holdTick);
    // those keep the level time-weighted but must not count as new measurements. Invalid when fresh == 0.
    static RangeMath::Level freshLevel(const std::vector<double> &rssi, int fresh, double spanS, bool moving);
    // At least 3 of the window's bursts, and at least 30 % of them, agree within kCalClusterM.
    static bool rttClusterOk(int bursts, int agreed) { return agreed >= 3 && agreed * 10 >= bursts * 3; }
    // Why a calibration window is rejected as a whole ("" = usable): 3+ RTT bursts that do not agree make it suspect
    // (nothing changes then); with fewer bursts the BLE links alone may calibrate.
    static QString calibrationFailure(int bursts, int agreed, bool bleUsable);
    static constexpr double kCalClusterM = 2.0;
    // The calibrated offset no longer matches: at least kStaleMinBursts of the last kStaleWindow bursts, and their
    // median range (burst − offset) is below −max(1 m, 3·σ_offset) — a distance cannot be negative. *median = that median.
    static constexpr int kStaleWindow = 20, kStaleMinBursts = 10;
    static bool rttOffsetStale(const std::vector<double> &implied, double offsetSigmaM, double *median = nullptr);
    // The RTT offset was found stale: everything the BLE links learnt since the last calibration was supervised by it.
    // Put both models back to that calibration's (downCal / upCal; null = not known, e.g. a ranging.json from before
    // 3.8's snapshot) and widen both BLE offsets to the prior (kOffsetVar0) and P_uu to at least 0.25, so the interval
    // shows the doubt until a recalibration instead of BLE updates from the suspect models narrowing it again.
    static void forgetSuspectLearning(RangeMath::RangeFilter &f, RangeMath::Rls2 &down, RangeMath::Rls2 &up,
                                      const RangeMath::Rls2 *downCal, const RangeMath::Rls2 *upCal);
private:
    void applyOurTx(int tx);                                 // our advert's TX power changed: re-prior uncalibrated down links
    static QString agentBeaconId(const QString &desktopId, const QString &device);
    static QString statePath();

    Locator *m_loc;
    BleLink *m_ble = nullptr;
    QHash<QString, Peer> m_peers;
    struct TagOwner { QString device; bool self = false, desktop = false; };   // desktop: only a desktop/laptop advert is it
    QHash<QByteArray, TagOwner> m_tags;                      // identity tag → owner, for windows w-1..w+1
    QHash<QByteArray, QString> m_bound;                      // session-bound unknown adverts: tag + kind byte → device
    qint64 m_tagWindow = -1;
    struct RawBle { QByteArray tag; int rssi; qint64 t; int tx; };
    std::vector<RawBle> m_raw;                               // last 2 min of every BeaconFix advert heard (pairing, unknown tags)
    QTimer m_tick;
    qint64 m_startMs = 0, m_savedMs = 0;
    bool m_dirty = false;
    int m_ourTx = 127;
};
