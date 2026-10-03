// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "plateevents.h"
#include "roadsnap.h"
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

// Camera-avoidance routing (docs/SIGHTINGS.md §8): avoid polygons from the ALPR cameras' cones along the corridor between
// A and B, the request bodies of the two providers (OpenRouteService avoid_polygons, GraphHopper custom-model areas),
// their answers, and the cameras a route still passes. Pure: src/routeplanner.cpp does the network.
namespace AvoidRoute {

struct LatLon { double lat = 0, lon = 0; };
using Ring = QList<LatLon>;                  // closed: the first point repeated at the end; counter-clockwise (RFC 7946)

constexpr double kDiscM = 60.0;              // a camera without a known direction: a 60 m disc
constexpr double kApexBackM = 8.0;           // the cone starts a little behind the mapped pole (its position is a few metres off)
constexpr double kMinCorridorM = 1500.0;     // cameras within max(1.5 km, 20 % of A–B, ≤ 10 km) of the straight line A–B
constexpr double kMaxCorridorM = 10000.0;

struct Cam {
    QString id, operatorName, model;
    double lat = 0, lon = 0;
    QList<double> dirs;                      // §2.3 directions; empty = unknown
    PlateEvents::Cone cone;                  // §2.3 half-angle and range
    double trust = 1.0;                      // §2.7
};
Cam fromCamera(const PlateEvents::Camera &c);

struct Area {
    QString cameraId;
    Ring ring;
    bool disc = false;                       // no direction: a disc
    double dirDeg = PlateEvents::nan();
    double areaM2 = 0;
};

Ring conePolygon(double lat, double lon, double dirDeg, double halfDeg, double rangeM, int arcPoints = 6);
Ring discPolygon(double lat, double lon, double radiusM, int points = 12);
double ringAreaM2(const Ring &r);
bool  pointInRing(const Ring &r, double lat, double lon);
// One cone per direction (the vendor half-angle + the 3° margin, out to the far range), else the 60 m disc
QList<Area> areasFor(const Cam &c);
double corridorM(LatLon a, LatLon b);
double distanceToSegmentM(LatLon p, LatLon a, LatLon b);

enum class Provider { Ors, GraphHopper };
QString providerId(Provider p);               // "ors" | "graphhopper"
QString providerName(Provider p);             // "OpenRouteService" | "GraphHopper"
bool    providerFromId(const QString &id, Provider *p);
struct Limits { int maxAreas = 0; int maxVertices = 0; double maxTotalKm2 = 0; };
Limits limits(Provider p);

struct Plan {
    QList<Area> areas;                       // what is sent, nearest the A–B line first
    QList<Cam> corridor;                     // every ALPR in the corridor
    int capped = 0;                          // cameras left out to stay inside the provider's limits
    double corridorM = 0;
};
Plan plan(const QList<Cam> &cams, LatLon a, LatLon b, Provider p);

QUrl requestUrl(Provider p, const QString &key, const QString &profile = QStringLiteral("car"));
QList<QPair<QByteArray, QByteArray>> requestHeaders(Provider p, const QString &key);
QByteArray requestBody(Provider p, LatLon a, LatLon b, const QList<Area> &areas, const QString &profile = QStringLiteral("car"));

struct Route {
    bool ok = false;
    QString error;
    QList<LatLon> points;
    double distanceM = 0, durationS = 0;
};
Route parseResponse(Provider p, int httpStatus, const QByteArray &body);

// The cameras a route still passes: its polyline enters the camera's cone (or, direction unknown, its 60 m disc)
struct Passed { Cam cam; double distanceM = 0; double alongM = 0; bool inCone = false; };
QList<Passed> camerasPassed(const QList<LatLon> &route, const QList<Cam> &cams);
QJsonObject toJson(const Route &r, const Plan &plan, const QList<Passed> &passed, Provider p);

// ── Inspect a camera unseen (docs/SIGHTINGS.md §9) ──
struct Vantage {
    LatLon pt;
    double distanceM = 0;
    double bearingToCamera = 0;              // deg (0…360): which way to look (candidate → camera)
    QString side;                            // "behind" | "beside" | "front" | "any"
    QString reason;                          // e.g. "Behind camera, outside field of view (32 m on sidewalk)"
    double score = 0;
};

struct Exposure {
    QString cameraId;
    QString operatorName;
    QString model;
    LatLon entry;
    LatLon exit;
    double meters = 0;
};

struct InspectPlan {
    Cam camera;
    QList<Vantage> vantages;
    Route toVantage;
    Route away;
    QList<Exposure> exposures;
    QList<Area> avoidRegions;
    Limits limits;
    Provider provider = Provider::Ors;
    bool safe = true;
    QString note;
};

// §9.1: Avoid regions for inspection: half-angle + 15°, range × 1.5 + 10 m, 15 m disc around pole;
// direction unknown or non-ALPR -> 60 m (or range × 1.5 + 10 m) disc.
QList<Area> inspectAvoidRegions(const QList<Cam> &cams);

// §9.2: Vantage points: 20-60 m from camera along OSM ways, outside every avoid region.
QList<Vantage> findVantages(const Cam &target, const QList<RoadSnap::Way> &ways,
                            const QList<Area> &avoidAreas,
                            const QString &profile = QStringLiteral("car"),
                            double minM = 20.0, double maxM = 60.0);

// §9.3: Exposure check: tests densified route (every 5 m) against nominal camera cones/ranges
QList<Exposure> checkExposures(const QList<LatLon> &route, const QList<Cam> &allCams);

// GPX export for inspection plan
QString toGpx(const QList<LatLon> &toVantage, const QList<LatLon> &away, const Vantage &vantage, const Cam &cam);

// JSON representation for API / UI
QJsonObject inspectToJson(const InspectPlan &ip, const QString &limitsText, const QJsonArray &photos = QJsonArray());

} // namespace AvoidRoute

