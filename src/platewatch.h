// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "plateevents.h"
#include "flockdetector.h"
#include "cameratrust.h"
#include "eyesonflock.h"
#include "roadsnap.h"
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <functional>

class MapDb;
struct Fix;
class QNetworkReply;
class RoutePlanner;

// Plate events (docs/SIGHTINGS.md), the moving parts: live pass detection on this host's own fixes, the backfill over
// every route fix (worker thread, chunked, idempotent, then incremental every 5 min), camera photos (OSM tags,
// Wikimedia Commons, Panoramax), public webcam stills on live passes, and the HaveIBeenFlocked plate-search watcher.
// The detection itself is src/plateevents.cpp (pure); storage is MapDb.
class PlateWatch : public QObject {
    Q_OBJECT
public:
    explicit PlateWatch(MapDb *db, QObject *parent = nullptr);
    // full = this host detects, backfills and watches (the tray / a node); the hub only stores what it is sent
    void start(bool full);
    bool active() const { return m_full; }

    void onOwnFix(const Fix &f);                                 // desktop live (§2.1)
    void startBackfill(bool restart);                            // §2.5; POST /plate-events/backfill
    void scheduleIncremental(int delayMs = 60000);               // new fixes arrived (a phone sync): look soon
    void checkHibfNow();                                         // a manual check (respects the floors)
    // POST /plate-events: {events:[…]} from a phone → {accepted, uids}; alerts for new live ALPR passes / searches
    QJsonObject ingest(const QJsonArray &events, const QString &device, bool alert);
    // POST /plate-events/<uid>/media (async: the encoding runs on a worker)
    void addMedia(const QString &uid, const QJsonObject &body, std::function<void(int, const QJsonObject &)> done);
    // GET /plate-events/media/<uid>?as=display|stored (async for the JXL → JPEG / PNG conversion)
    void mediaFor(const QString &mediaUid, bool display, std::function<void(int, const QByteArray &, const QByteArray &)> done);
    QJsonObject status() const;                                  // GET /plate-events/status
    QJsonObject backfillState() const;
    QJsonObject hibfState() const;
    void fetchCameraPhotos(const QStringList &cameraIds, bool force = false);

    // §2.6 road snapping: the drivable OSM ways around these cameras (Overpass, ≤ 1 request / 2 s, cached 90 days),
    // then their passes are matched again
    void ensureRoads(const QStringList &cameraIds);
    int  resnapCamera(const QString &cameraId);                  // rescore every pass of the camera (snap + trust); returns how many changed
    // §2.7 trust: the user's verdict (present | absent | clear) → {camera, trust, trustDetail, passesRescored} or {error}
    QJsonObject setCameraVerdict(const QString &cameraId, const QString &verdict);
    QJsonObject cameraInfo(const QString &cameraId);             // GET /cameras/<id>: the row, trust, watched way, agency facts
    // §4.6 Eyes on Flock: the agency facts of an event (camera operator / searching agency), {} when unknown
    QJsonObject agencyFacts(const QJsonObject &event);
    QJsonObject eyesOnFlockState() const;
    void refreshEyesOnFlock(bool force = false);
    RoutePlanner *routePlanner() const { return m_router; }      // docs/SIGHTINGS.md §8
    // A camera's cached roads (§2.6): fresh = fetched within 90 days; the ways around it and the one it watches
    struct Roads { bool fresh = false; QList<RoadSnap::Way> ways; RoadSnap::Watched watched; };

signals:
    void alert(const QJsonObject &event);                        // a new ALPR live pass or plate search: notify (§6)
    void backfillSummary(int alprPasses, int cameraPasses, const QString &since);
    void eventsChanged();                                        // the Sightings tab refreshes

private:
    struct Chunk { QString from, to; };
    struct Still { qint64 ms = 0; QByteArray bytes; QString url; };
    void nextChunk();
    void chunkDone(const QList<QJsonObject> &events, const Chunk &c);
    void finishBackfill();
    void setBackfillKv(const QJsonObject &o);
    QList<PlateEvents::Camera> toCameras(const QList<FlockCamera> &cams) const;
    QJsonArray agencies() const;
    QString activePlate() const;
    QStringList plates() const;
    void livePasses(const QList<PlateEvents::Pass> &passes);
    // webcam stills
    void fetchWebcam(const PlateEvents::Camera &cam);
    void wv511List(std::function<void(const QJsonArray &)> done);
    static bool webcamStillsAllowed(const QString &host);
    void grabStream(const QString &camId, const QString &streamUrl, const QString &referer, const QString &pageUrl);
    void flushStills(const QString &camId);
    // photos
    void nextPhoto();
    void enforcementCheck(const QString &id, const FlockCamera &cam, const QJsonObject &tags);
    void photoCandidates(const QString &camId, const QJsonObject &tags, double lat, double lon);
    void fetchPhoto(const QString &camId, QList<QJsonObject> cands, int found);
    void panoramaxNear(const QString &camId, double lat, double lon, int host, std::function<void(QList<QJsonObject>)> done);
    void photoDone(const QString &camId, int found);
    // HIBF
    void hibfTick();
    void hibfSearch(const QString &cursor, int page);
    void hibfSources();
    void hibfFinish(int status, const QString &error, int retryAfter);
    QString hibfMode() const;
    bool hibfMayRequest(QString *why) const;
    void hibfCount();
    // §2.6 / §2.7
    Roads roadsFor(const PlateEvents::Camera &cam);
    void nextRoads();
    void applyTrust(QList<PlateEvents::Camera> &cams);
    CameraTrust::Result computeTrust(const FlockCamera &c);
    bool rescoreStored(const QJsonObject &event, const PlateEvents::Camera &cam, const Roads &roads);
    void rescoreAll();
    // §4.6
    void eofTick();
    void eofDone(int http, const QByteArray &body, const QString &blockedBy, const QString &netError);
    void loadPortals();
    QString agencySlug(const QString &name, const QString &state, bool *stateVerified);
    QString stateHint();
    // network
    QNetworkReply *get(const QUrl &url, const QByteArray &ua = QByteArray(), const QList<QPair<QByteArray, QByteArray>> &headers = {});
    void storeEncoded(const QByteArray &bytes, const QJsonObject &meta, std::function<void(bool, const QString &)> done = nullptr);

    MapDb *m_db;
    QNetworkAccessManager m_nam;
    bool m_full = false, m_started = false;
    // backfill
    QList<Chunk> m_chunks;
    bool m_bfRunning = false, m_bfFullRun = false, m_restartPending = false;
    qint64 m_bfTargetSeq = 0;
    QString m_bfThrough, m_bfFirstNew;
    int m_bfNewAlpr = 0, m_bfNewCamera = 0, m_bfPasses = 0;
    QSet<QString> m_bfCameras;
    QTimer m_incTimer, m_liveTimer, m_hibfTimer, m_incSoon;
    // live
    PlateEvents::LiveTracker m_live;
    QHash<QString, QList<Still>> m_stills;
    QHash<QString, int> m_stillBusy;                             // camera → fetches in flight
    QHash<QString, QPair<QString, qint64>> m_stillTarget;        // camera → (event uid, closest approach ms)
    QSet<QString> m_stillFetched;                                // cameras whose current pass already has a fetch
    QHash<QString, QString> m_streamUrl;                         // wv511 camera → its HLS playlist
    QJsonArray m_wv511; QDateTime m_wv511At;                     // WV511's camera list (a day)
    QHash<QString, PlateEvents::Camera> m_liveCams;              // cameras of the passes in progress
    // photos
    QStringList m_photoQueue;
    bool m_photoBusy = false;
    QDateTime m_lastOsm;
    // HIBF
    bool m_hibfBusy = false;
    QDateTime m_hibfLastRequest;
    int m_hibfNewHits = 0, m_hibfRows = 0;
    // roads (§2.6)
    QStringList m_roadQueue;
    QString m_roadInFlight;                                      // the camera whose roads are being fetched
    bool m_roadBusy = false;
    QDateTime m_lastOverpass, m_overpassCool;
    int m_overpassMirror = 0;
    QHash<QString, QDateTime> m_roadTried;                       // camera → last failed fetch (retried after 6 h)
    QHash<QString, int> m_roadRetries;                           // 429 / 504: put back once
    QSet<QString> m_roadsFresh;                                  // cameras whose roads are cached and fresh
    // trust (§2.7)
    struct TrustCache { double trust = -1; QJsonObject detail; QDateTime at; };
    QHash<QString, TrustCache> m_trustCache;
    // Eyes on Flock (§4.6)
    QTimer m_eofTimer;
    bool m_eofBusy = false, m_portalsLoaded = false;
    QList<EyesOnFlock::Portal> m_portals;
    QHash<QString, QPair<QString, bool>> m_agencyCache;          // "name|state" → (slug, stateVerified)
    QString m_stateHint; QDateTime m_stateHintAt;
    RoutePlanner *m_router = nullptr;
};
