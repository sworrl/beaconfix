// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "flockdetector.h"
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

// The camera map's bulk sources (docs/SIGHTINGS.md §2.0, docs/DATABASE.md). Pure: no database, no network, so the
// sync worker and the unit tests run the same code.
//   DeFlock      https://data.dontgetflocked.com/cameras.geojson.gz — daily, ALPRs only (OSM man_made=surveillance +
//                surveillance:type=ALPR), ~143k features, plain JSON despite the name. Data ODbL 1.0 (OpenStreetMap
//                contributors), pipeline MIT (github.com/FoggedLens/deflock-data).
//   flocklocations.com export — only its community submissions (rows WITHOUT an osm_id), CC BY 4.0.
namespace CameraImport {

constexpr const char *kDeflockUrl = "https://data.dontgetflocked.com/cameras.geojson.gz";
constexpr const char *kFlockLocationsUrl = "https://flocklocations.com/api/cameras/export?format=geojson";
constexpr int kDeflockMinFeatures = 10000;   // fewer means a truncated / broken download: nothing is purged then

// Splits a GeoJSON FeatureCollection into its features as the bytes arrive: the document is never held whole
// (DeFlock is ~38 MB, flocklocations ~60 MB). Only the "features" array is read; one feature at a time is parsed.
class FeatureStream {
public:
    void feed(const QByteArray &chunk);
    bool next(QJsonObject *feature);       // false: none complete yet
    bool done() const { return m_state == Done; }
    bool started() const { return m_state != Seeking; }
    qint64 count() const { return m_count; }
    int  bad() const { return m_bad; }     // features that were not JSON objects
private:
    enum State { Seeking, InArray, InFeature, Done };
    State m_state = Seeking;
    QByteArray m_head;                     // before "features": [ (bounded)
    QByteArray m_cur;                      // the feature being read
    int  m_depth = 0;
    bool m_inStr = false, m_esc = false;
    QList<QByteArray> m_ready;
    qint64 m_count = 0;
    int m_bad = 0;
};

// One DeFlock feature → a camera row (id osm:<type>/<id>, source "deflock", camera_type alpr, the real OSM version /
// timestamp, the brand as manufacturer, every direction). false when it has no position or no OSM id.
bool fromDeflock(const QJsonObject &feature, const QDateTime &now, FlockCamera *out);
// One flocklocations feature → a camera row, only for community submissions (no osm_id); false otherwise
bool fromFlockLocations(const QJsonObject &feature, const QDateTime &now, FlockCamera *out);
// DeFlock's directions[] (or direction) as the direction text ("90;270"); "" none. A lone 0 stays "0" (= unknown,
// docs/SIGHTINGS.md §2.3: DeFlock writes 0 for N as well as for "not given")
QString deflockDirections(const QJsonObject &props);

} // namespace CameraImport
