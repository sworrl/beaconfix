// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <cmath>
#include <limits>

// Plate events (docs/SIGHTINGS.md): camera passes from route fixes. Pure — no database, no network, no Qt event
// loop — so the desktop's live detection, the backfill worker and the unit tests run exactly the same code.
namespace PlateEvents {

constexpr double kPassRadiusM = 65.0;          // §2.1: a pass is a run of fixes within 65 m of the camera
constexpr qint64 kRunGapMs = 5 * 60 * 1000;    // split where fixes are > 5 min apart
constexpr qint64 kMergeMs = 10 * 60 * 1000;    // §1.1: the same camera within ±10 min is the same pass
constexpr double kMaxSegmentM = 1500.0;        // a segment longer than this is not interpolated (the road is not straight)
constexpr qint64 kFacingOffsetMs = 10000;      // §2.2: frontBearingDeg / rearBearingDeg, 10 s before / after the closest approach
constexpr double kConeMarginDeg = 3.0;         // §2.3: the vendor half-angle + 3°
constexpr double kCapture = 0.97, kRead = 0.93;   // §2.4: P(capture), P(read | captured)

inline double nan() { return std::numeric_limits<double>::quiet_NaN(); }

// One route fix of ONE device (the detector never mixes devices inside a polyline)
struct TrackFix {
    qint64  ms = 0;               // epoch milliseconds
    double  lat = 0, lon = 0;
    double  acc = -1;             // metres, -1 unknown
    double  speedMps = -1;        // the fix's own speed when it has one
    QString device;               // "" = this host
    QString source;               // the fix source (gps, wifi, starlink, …)
};

struct Camera {
    QString id;
    double  lat = 0, lon = 0;
    QString type;                 // alpr | webcam | ptz | cctv | enforcement | not_camera (§2.0)
    QString operatorName, model, direction, source;
    QString manufacturer;         // flock_cameras.manufacturer (DeFlock's brand), "" unknown
    QString webcam;               // OSM contact:webcam / webcam (a public live feed), "" none
    QJsonObject tags;             // the OSM tags when known
    int     sourceConfidence = 100; // flock_cameras.confidence (a list's own confidence)
    double  trust = -1;           // §2.7: P(the camera is there and working), a factor of P(read); -1 not known (then a
                                  // suspected-only camera keeps the old × 0.7)
    QJsonObject trustDetail;      // its log-odds terms (flock_cameras.trust_detail)
};

// §2.3: an ALPR reads only inside a narrow cone and range (vendor datasheets)
struct Cone { double halfDeg = 12.5, minM = 5, maxM = 35; QString name = QStringLiteral("unknown fixed ALPR"); };
Cone coneFor(const Camera &cam);

struct Pass {
    QString cameraId;
    qint64  ms = 0;               // closest approach
    double  lat = 0, lon = 0, acc = -1;
    double  distanceM = 0;
    double  speedKmh = nan();
    double  headingDeg = nan();
    double  approachBearingDeg = 0; // camera → us
    double  cameraDirDeg = nan();   // the direction that saw us (or the first one when none did), NaN unknown
    int     facing = -1;            // -1 unknown, 0 no, 1 yes
    double  dwellS = 0;
    int     fixes = 0;
    QStringList devices, sources;
    double  enterDistanceM = nan(), exitDistanceM = nan();
    double  frontBearingDeg = nan(), rearBearingDeg = nan();
    bool    frontVisible = false, rearVisible = false;
    QList<double> directions;
    qint64  startMs = 0, endMs = 0; // the run's extent
    Cone    cone;                   // §2.3
    double  inConeS = 0;            // time inside the cone and the range (no direction: inside the range)
    double  pInCone = 0;            // §2.4: P(in cone and range), the fix error integrated
    double  pRead = 0;              // × capture × read × snapFactor × trustFactor
    double  pReadBase = 0;          // P(in cone) × capture × read: before the factors below
    double  snapFactor = 1;         // §2.6 road snapping: 0.1 … 1 (1 = not snapped or on the watched road)
    QJsonObject snap;               // §2.6 metrics.snap
    double  trustFactor = 1;        // §2.7 the camera's trust
    int     confidence = 0;         // §2.4 for the camera's type
    int     confidenceAlpr = 0;     // §2.4 as if the camera were an ALPR (a reclassification recomputes from it)
};

// ── geometry ──
double distanceM(double lat1, double lon1, double lat2, double lon2);
double bearingDeg(double lat1, double lon1, double lat2, double lon2);
double angleDiff(double a, double b);   // 0…180

// ── §2.0 classification ──
// From the raw OSM tags, in order: enforcement, not_camera, alpr, webcam, ptz, cctv. Non-OSM rows (community /
// suspected lists, RF detections of Flock hardware) are alpr. An OSM row whose tags are not known yet falls back to
// its model (as imported) until the tags are fetched.
QString classifyCamera(const QString &model, const QString &source, const QString &id, const QString &detectionMethod, const QJsonObject &tags);
// A note about the tags: a misspelt ALPR type (reduced confidence), contact:webcam on an ALPR (a tagging conflict)
QString classifyNote(const QJsonObject &tags);
bool    isOsmId(const QString &id);
QString directionText(const Camera &cam);          // direction wins over camera:direction (§2.3)
bool    isPlateReaderModel(const QString &model);
bool    suspectedOnly(const QString &source, const QString &model);   // only a "Suspected" list knows it
QString webcamUrl(const QJsonObject &tags);

// ── §2.3 directions: degrees, cardinals (N … NNW), ranges "a-b" (the centre), ';'-separated lists; 0 = unknown ──
QList<double> parseDirections(const QString &s);

// ── §2.1 / §2.2 detection over one device's time-ordered fixes ──
QList<Pass> detectPasses(const QList<TrackFix> &fixes, const Camera &cam);
// Passes of the same camera from several devices within ±10 min → one (the closest approach wins, the
// devices / sources / fix counts are combined)
QList<Pass> mergePasses(QList<Pass> passes);
// Several devices at once: grouped per device, detected, merged
QList<Pass> detectAll(const QList<TrackFix> &fixes, const Camera &cam);

// ── §2.4 confidence = round(100 × P(read)); × 0.7 suspected-only; non-ALPR types ≤ 40 ──
int confidenceFor(const Pass &p, const Camera &cam, bool asAlpr);
// pRead = pReadBase × snapFactor × trustFactor, and both confidences again (after a factor changed)
void rescore(Pass &p, const Camera &cam);
// The same for a stored pass (its metrics): pReadBase (older rows: from pInCone), the factors, pRead, confidenceAlpr
// updated in place; returns the confidence for the camera's type
int rescoreMetrics(QJsonObject &metrics, const QString &cameraType, double snapFactor, const QJsonObject &snap, double trustFactor, const QJsonObject &trustDetail);
QString typeLabel(const QString &type);             // "ALPR", "public traffic webcam", …

// ── §1.1 uids ──
QString passUid(const QString &cameraId, qint64 closestMs);
QString searchUid(const QJsonObject &row);   // hibf:<24 hex>

// The plate_events row (column names as keys; metrics / raw as objects) for a pass
QString     isoLocal(qint64 ms);
QJsonObject passEvent(const Pass &p, const Camera &cam, const QString &plate, const QString &source, const QString &device,
                      bool leaky = false, const QString &leakyMatch = QString());
QString     passDetails(const QString &cameraType, const QString &operatorName, const QString &model, double distanceM, int facing);
bool        alertable(const QString &cameraType);   // only alpr passes say "your plate was likely read" and alert

// ── live detection (desktop live / phone live): fixes in, finished passes out ──
// A pass is finished when we have left the 65 m circle, or 2 min after its closest approach.
class LiveTracker {
public:
    // A new own fix and the cameras within a few hundred metres of it. Returns the passes that finished.
    QList<Pass> addFix(const TrackFix &f, const QList<Camera> &nearby);
    QList<Pass> tick(qint64 nowMs);                     // time-based finalisation (no new fix)
    QList<Camera> activeCameras() const;                 // cameras we are passing right now
    const Camera *camera(const QString &id) const;
    const QList<TrackFix> &buffer() const { return m_buf; }     // the last ~10 min of own fixes (road snapping, §2.6)
private:
    struct Active { Camera cam; qint64 enteredMs = 0; qint64 bestMs = 0; double bestD = 1e9; bool left = false; };
    QList<Pass> finish(const QString &id);
    void trim(qint64 nowMs);
    QList<TrackFix> m_buf;                               // the last ~10 min of own fixes
    QList<Active> m_active;
    struct Done { qint64 ms = 0; bool inside = true; };
    QHash<QString, Done> m_done;                         // camera → the last finished pass: no repeat while still inside / within 10 min
};

} // namespace PlateEvents
