// BeaconFix anchors — surveyed transmitters and places (docs/RANGING.md §4).
// The JSON object is a frozen contract shared by the desktop, the Plasma widget, the Android app
// and the Pi agent; this module parses/normalises it and does the anchor maths (local frame,
// RV-frame re-projection, anchor fix, trilateration from ranges, the rv-gnss tier).
// Qt Core only. The maths mirrors AnchorMath in android/…/ranging/RangeMath.kt.
#pragma once
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <vector>

namespace Anchors {

struct Enu { double e = 0, n = 0, u = 0; };

struct Anchor {
    QString id, name, kind = QStringLiteral("custom");
    double lat = 0, lon = 0;
    bool hasAlt = false;     double alt = 0;
    bool hasHeight = false;  double heightM = 0;
    bool hasFloor = false;   int floor = 0;
    double accM = 1.0;
    QStringList bssids;                                // upper-case AA:BB:CC:DD:EE:FF
    QString ble;
    bool rv = false;
    bool hasRvOffset = false; Enu rvOffset;
    bool ref = false;
    bool hasHeading = false; double headingDeg = 0;
    QString placedBy, placedAt, source;
    // output-only / sync
    bool headingAssumed = false;
    bool deleted = false; QString deletedAt;
    qint64 seq = 0;
    QJsonObject extra;                                 // unknown keys, preserved on round-trip

    static Anchor fromJson(const QJsonObject &o, QString *error = nullptr);   // validates + normalises
    QJsonObject toJson(bool outputFields = true) const;
};

extern const QStringList kKinds;                       // this-computer, wifi-ap, ble, rtt-responder, gnss, custom
QString normalizeBssid(const QString &b);              // "aa-bb-cc-dd-ee-ff" → "AA:BB:CC:DD:EE:FF"; "" if invalid
QString schemaSql();                                   // CREATE TABLE anchors … (docs §4.2)

// ── local tangent plane (same constants as src/estimator.h Frame) ────────────
Enu enu(double refLat, double refLon, double refAlt, double lat, double lon, double alt);   // alt NaN → u = 0
void fromEnu(double refLat, double refLon, double refAlt, const Enu &o, double *lat, double *lon, double *alt);
void rotate(double e, double n, double deg, double *e2, double *n2);   // clockwise by deg (compass sense)
double distanceM(double lat1, double lon1, double lat2, double lon2);

// ── §4.3 uses ─────────────────────────────────────────────────────────────────
// The RV reference: the newest ref:true anchor, else the newest this-computer anchor, else null.
const Anchor *reference(const QList<Anchor> &all);
// Store an RV anchor's offset from the reference (and the RV heading, if known).
void placeRv(Anchor &a, const Anchor &ref, bool haveHeading, double headingDeg);
struct Reprojected { double lat = 0, lon = 0, alt = 0; bool hasAlt = false, headingAssumed = true; };
Reprojected reproject(const Enu &offset, bool hasHeadingPlaced, double headingPlacedDeg,
                      double newRefLat, double newRefLon, bool newRefHasAlt, double newRefAlt,
                      bool haveNewHeading, double newHeadingDeg);
// Re-project every RV anchor (except the reference) after the reference moved to (lat, lon, alt).
QList<Anchor> reprojectAll(const QList<Anchor> &all, double newRefLat, double newRefLon, bool newRefHasAlt, double newRefAlt,
                           bool haveNewHeading, double newHeadingDeg);
bool shouldReproject(double oldRefLat, double oldRefLon, double stopLat, double stopLon);   // > 250 m
// At most one ref:true per site (100 m): the newest placedAt wins.
QList<Anchor> normalizeSet(QList<Anchor> all);

struct AnchorFix { bool valid = false; double lat = 0, lon = 0, acc = 0; bool hasAlt = false; double alt = 0; QString anchorId; };
// The this-computer anchor that replaces our own fix while we are within 100 m of it.
AnchorFix thisComputerFix(const QList<Anchor> &all, double ownLat, double ownLon);
// BSSID → anchor, for every wifi-ap / rtt-responder / this-computer anchor's bssids.
QHash<QString, Anchor> pinnedBssids(const QList<Anchor> &all);

// ── absolute position of a device from ranges to anchors (MAP Gauss–Newton) ──
struct RangeToAnchor { double lat = 0, lon = 0, anchorAccM = 1, rangeM = 0, sigmaM = 1; };
struct Trilat { bool valid = false; double lat = 0, lon = 0, sigmaM = 0, rmsM = 0; int used = 0, iterations = 0; };
Trilat trilaterate(const std::vector<RangeToAnchor> &ranges, double priorLat, double priorLon, double priorSigmaM);

// ── §4.3.7 the rv-gnss tier ───────────────────────────────────────────────────
struct RvGnss { bool valid = false; double lat = 0, lon = 0, sigmaM = 0; };
RvGnss rvGnssPosition(double gnssLat, double gnssLon, double gnssSigmaM,
                      const Anchor *gnssAnchor, const Anchor *thisAnchor, bool haveHeading, double headingDeg);

} // namespace Anchors
