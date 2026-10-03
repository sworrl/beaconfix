// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "plateevents.h"
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

// Road snapping for camera passes (docs/SIGHTINGS.md §2.6). Pure: no database, no network. The drivable OSM ways
// around a camera come from Overpass (src/platewatch.cpp fetches and caches them in osm_ways); here they are parsed,
// the camera's watched way is chosen, and a pass's track is matched to the ways (a small HMM / Viterbi over the
// ways near each sample: distance, heading, oneway, layer continuity). A pass that was on another road than the
// one the camera watches — a parallel road, the other carriageway, an over- or underpass — gets its P(read) cut.
// A heavier option for later is a real map matcher (Valhalla trace_attributes, OSRM match) on the whole trip.
namespace RoadSnap {

constexpr double kFetchRadiusM = 80.0;     // Overpass: the motor roads within 80 m of the camera
constexpr double kWatchRadiusM = 40.0;     // the watched way lies within 40 m of the camera …
constexpr double kWatchDirTolDeg = 35.0;   // … and runs along its direction ±35° (ALPRs point along the road)
constexpr double kHeadingTolDeg = 40.0;    // a trip heading more than 40° off a way's axis is not on it
constexpr double kAxisOffsetM = 12.0;      // another way more than 12 m off the watched way's axis is another road
constexpr double kOffFactor = 0.1;         // P(read) × 0.1 when the trip was on another road
constexpr int    kTtlDays = 90;            // the cached ways are refetched after 90 days
constexpr double kSampleWindowS = 90.0;    // the fixes within ±90 s of the closest approach …
constexpr double kSampleRadiusM = 100.0;   // … and 100 m of the camera are matched
constexpr double kStretchM = 30.0;         // "on the watched road" must hold along 30 m around the closest approach

struct Pt { double lat = 0, lon = 0; };

struct Way {
    qint64 id = 0;
    QString highway, name, ref;
    int  layer = 0;             // OSM layer; a bridge without one is 1, a tunnel -1
    bool bridge = false, tunnel = false;
    int  oneway = 0;            // 0 both ways, 1 along the node order, -1 against it
    QList<qint64> nodes;        // node ids: two ways that share one are connected
    QList<Pt> pts;
};

// The Overpass QL for the motor roads within radiusM of a point (out body geom: tags, node ids and geometry)
QString overpassQuery(double lat, double lon, double radiusM = kFetchRadiusM);
bool isMotorRoad(const QString &highway);
// An Overpass JSON answer → the motor-road ways (other elements and footways are dropped); *error when unreadable
QList<Way> parseOverpass(const QByteArray &json, QString *error = nullptr);
QJsonObject toJson(const Way &w);             // the osm_ways cache row's content
Way fromJson(const QJsonObject &o);

// Where a point lies on a way: the nearest point of its polyline
struct OnWay {
    double distM = 1e18;        // point → way
    double bearingDeg = PlateEvents::nan();   // the way's direction there, along its node order
    Pt     at;                  // the nearest point
    int    seg = -1;
};
OnWay project(const Way &w, double lat, double lon);

// The camera's watched way: within 40 m, its axis along one of the camera's directions ±35°, on the camera's layer
// (its OSM layer tag; a camera without one is on the ground, but may watch a bridge when nothing else qualifies).
// Without a direction: the nearest way. basis: direction | nearest | nearest_no_direction_match | none.
struct Watched {
    qint64 wayId = 0;
    int    index = -1;          // into the ways list it was chosen from
    double distM = PlateEvents::nan();
    double bearingDeg = PlateEvents::nan();
    int    layer = 0;
    int    oneway = 0;
    QString basis = QStringLiteral("none");
    QJsonObject toJson() const;
};
Watched watchedWay(const QList<Way> &ways, double camLat, double camLon, const QList<double> &dirs, const QJsonObject &camTags = QJsonObject());

// One point of the track to match
struct Sample {
    qint64 ms = 0;
    double lat = 0, lon = 0;
    double acc = -1;                        // metres (68 %), -1 unknown
    double headingDeg = PlateEvents::nan(); // direction of travel, NaN unknown
};
// The track around a pass: the fixes of ONE device (time-ordered) within ±90 s of the closest approach and 100 m of the
// camera, each with the heading of its neighbouring segments, plus the closest-approach point itself (*closest = its index)
QList<Sample> samplesFor(const QList<PlateEvents::TrackFix> &fixes, const PlateEvents::Pass &p, double camLat, double camLon, int *closest);

struct Result {
    QString status = QStringLiteral("no_roads");   // ok | no_roads | no_watched_way | no_samples
    QString verdict;            // on_watched_way | near_axis | parallel_road | different_layer | opposite_direction | ambiguous
    double factor = 1.0;        // multiplies P(read)
    double pWatched = 1.0;      // P(the trip was on the watched way, or a way that counts as it)
    qint64 watchedWay = 0, matchedWay = 0;
    double offsetM = PlateEvents::nan();     // the matched way's distance from the watched way's axis at the closest approach
    double trackOffsetM = PlateEvents::nan();// the closest-approach point's distance from the watched way
    int    watchedLayer = 0, matchedLayer = 0;
    QString layerVerdict;       // same | different | unknown
    bool   oppositeOneway = false;
    int    samples = 0, candidates = 0;
    QJsonObject toJson() const;               // metrics.snap
};
// The matched way at the closest approach and the factor for P(read). closest: the closest-approach sample's index.
Result snap(const QList<Sample> &samples, int closest, const QList<Way> &ways, const Watched &watched);

} // namespace RoadSnap
