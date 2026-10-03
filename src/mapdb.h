#pragma once
#include "locator.h"
#include "flockdetector.h"
#include "ranging/anchors.h"
#include "tracksmoother.h"
#include "plateevents.h"
#include "roadsnap.h"
#include "eyesonflock.h"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

// The internal mapping database: everything BeaconFix has learned, in one SQLite
// file — access points and where they are, every observation (where we stood, how
// loud), the trip log, places, elevation cells, milestones.
//
// Encryption at rest. This box has no SQLCipher, so the database is encrypted by
// BeaconFix itself: the file at ~/.local/state/beaconfix/beaconfix.db is an
// AES-256-GCM blob (OpenSSL EVP; header "BFDB\1", 12-byte nonce, ciphertext, 16-byte
// tag). While the tray runs, the plaintext copy lives in the private, tmpfs-backed
// runtime directory ($XDG_RUNTIME_DIR/beaconfix/live.db, mode 0600) and is
// re-encrypted back to the blob a few seconds after every change and on exit.
// The 256-bit key comes from KWallet (folder "BeaconFix", entry "mapdb-key") when
// the wallet can be opened, otherwise from ~/.config/sworrl/beaconfix.key (0600).
// A standalone process (--json, --once) opens a read-only decrypted copy of its own
// and never writes.
class MapDb : public QObject {
    Q_OBJECT
public:
    struct ApPos { QString bssid, ssid; double lat = 0, lon = 0, acc = 0; QString source; int home = 0, travelling = 0, ignored = 0; };

    explicit MapDb(const QString &stateDir, QObject *parent = nullptr);
    ~MapDb() override;

    bool    open(bool readOnly = false);
    bool    isOpen() const { return m_db.isOpen(); }
    bool    readOnly() const { return m_readOnly; }
    QString error() const { return m_error; }
    QString path() const { return m_blobPath; }              // the encrypted file
    QString keySource() const { return m_keySource; }          // "kwallet" | "keyfile"
    QByteArray key() const { return m_key; }                   // the 256-bit key (also seals the identity file)
    static QByteArray bootstrapKey(QString *source);           // the key, creating the key file if there is none yet (CLI before the first tray start)
    bool    isEmpty() const;                                   // no aps and no fixes
    QJsonObject stats() const;

    // Access points + everything learned about them
    QHash<QString, ApRecord> loadApRecords(QSet<QString> *travelling, QSet<QString> *notTravelling) const;   // incl. fits + peer positions
    void saveApRecords(QHash<QString, ApRecord> &recs, const QSet<QString> &travelling, const QSet<QString> &notTravelling,
                       const QHash<QString, int> &flags);      // flags: bit0 home, bit1 travelling, bit2 ignored; assigns observation ids
    void saveRecord(const QString &bssid, ApRecord &r, int flags);   // one record (new / changed observations only)
    void saveEstimate(const QString &bssid, const Estimator::Fit &fit);
    void appendEstimateHistory(const QString &bssid, const Estimator::Fit &fit);   // the last 20 per beacon
    QJsonArray estimateHistory(const QString &bssid) const;
    // Cells (~15 m) this host scanned from: where a beacon was NOT heard (docs/GRADING.md §1.7)
    struct ScanCellRow { QString key; double lat = 0, lon = 0; int count = 0; qint64 first = 0, last = 0; };
    QList<ScanCellRow> loadScanCells() const;
    void saveScanCell(const ScanCellRow &c);
    // Change sequence for sync: every stored/changed AP position, observation and fix gets the next number
    qint64 currentSeq() const { return m_seq; }
    QJsonObject changesSince(qint64 since, int limit, bool *more, qint64 *cursor) const;
    QString kv(const QString &key) const;
    void    setKv(const QString &key, const QString &value);
    // Trip log
    QList<Fix> loadFixes() const;
    void saveFixes(const QList<Fix> &fixes);                   // rewrite (departures change)
    void appendFix(const Fix &f);
    // Places. scope "near": the main query around the fix (kv poi_*); "far": the pediatric ER search (kv peds_*).
    // Both share the table: a place in both lists is stored once, as near (the merged view prefers near anyway).
    bool loadPois(QList<Poi> *pois, double *lat, double *lon, int *radiusM, QDateTime *time, const QString &scope = QStringLiteral("near")) const;
    void savePois(const QList<Poi> &pois, double lat, double lon, int radiusM, const QDateTime &time, const QString &scope = QStringLiteral("near"));
    // Elevation cells
    QHash<QString, double> loadElevation() const;
    void saveElevation(const QHash<QString, double> &cells);
    // Milestones
    QHash<QString, QDateTime> loadAchievements() const;
    void saveAchievements(const QList<Achievement> &list);

    // Positions and self-location (offline: places we have been before)
    QList<ApPos> positions(const QStringList &bssids, double maxAcc = 0) const;   // all when bssids is empty
    // GET /api/v1/aps?all=1: every positioned beacon, bssid order, keyset-paged (after = last bssid of the previous page)
    QJsonArray positionsPage(const QString &after, int limit, QString *next) const;
    void setPins(const QHash<QString, ApPos> &pins) { m_pins = pins; }             // anchored BSSIDs (upper-case): override stored positions
    bool estimate(const QList<QPair<QString, int>> &heard, double *lat, double *lon, double *acc, int *used, QStringList *usedBssids = nullptr,
                  int minAps = 2, double maxAcc = 150) const;
    int  addObservations(const QJsonArray &observations, const QString &device, QString *error = nullptr,
                         QHash<QString, QList<ApObservation>> *added = nullptr);   // from another BeaconFix / device; dedup (bssid, time, device)
    int  mergePeerAps(const QJsonArray &aps, const QString &device, QStringList *touched = nullptr);   // positions another device worked out
    int  appendPeerFixes(const QJsonArray &fixes, const QString &device);
    QJsonArray latestFixesByDevice() const;              // newest fix per peer device (device != "")
    QList<Fix> peerFixes(const QString &device = QString()) const;
    // Anchors (docs/RANGING.md §4): surveyed transmitters/places, synced through /db/changes like everything else
    QList<Anchors::Anchor> loadAnchors(bool includeDeleted = false) const;
    // Store one anchor (or tombstone). force = a local edit (always wins, new seq); otherwise a synced row that
    // only replaces what we have when it is newer (placedAt / deletedAt). Returns true when the table changed.
    bool putAnchor(Anchors::Anchor a, bool force, Anchors::Anchor *stored = nullptr);
    // Flock / ALPR Cameras and sighting vetting
    QList<QPair<QString, double>> providerErrorSamples() const;
    // Every device's own fixes (device "" = this host), time-ordered, for the track smoother
    QHash<QString, QList<TrackSmoother::Fix>> deviceFixes() const;   // (provider, |error| / claimed acc): this host's Wi-Fi fixes vs the phone's GPS within 2 min
    QList<FlockCamera> loadFlockCameras() const;
    QList<FlockCamera> loadFlockCamerasIn(double latMin, double latMax, double lonMin, double lonMax, int limit = 0) const;   // nearest-first when limit > 0
    bool saveFlockCamera(const FlockCamera &cam);
    int  saveFlockCameras(const QList<FlockCamera> &cams);
    bool recordFlockSighting(const QString &bssidOrMac, double lat, double lon, const FlockDetector::Detection &det, const QDateTime &time = QDateTime::currentDateTime());
    QJsonObject flockStats() const;
    QJsonObject cameraCounts() const;                                                // per source / type / stale (docs/DATABASE.md)
    QByteArray exportFlockGeoJson() const;
    int  insertNewFlockCameras(const QList<FlockCamera> &cams);                      // INSERT OR IGNORE, one transaction
    // The DeFlock bulk source (src/cameraimport.h): rows added / updated / unchanged. A row with OSM tags (Overpass) keeps
    // them and only gains the OSM version / timestamp / manufacturer; a tagless row takes DeFlock's values.
    QJsonObject upsertDeflockCameras(const QList<FlockCamera> &cams);
    // After a complete DeFlock import: bulk-imported OSM rows DeFlock no longer lists (deleted nodes, gunshot detectors,
    // anything not an ALPR) are deleted — or only marked stale when they have camera passes. {"checked","deleted","stale"}
    QJsonObject reconcileBulkCameras(const QSet<QString> &deflockIds);
    bool hasLegacyBulkCameras() const;                                               // rows of the old flocklocations bulk import

    // License Plates management
    QList<LicensePlate> loadLicensePlates() const;
    bool saveLicensePlate(const LicensePlate &p);
    bool deleteLicensePlate(const QString &plate);
    LicensePlate activeLicensePlate() const;

    // Camera Encounters / Passes
    QList<CameraEncounter> loadCameraEncounters(const QString &cameraId = QString(), int limit = 200) const;
    bool logCameraEncounter(CameraEncounter &enc);
    int  cameraPassCount(const QString &cameraId) const;

    // Plate Audits / Public database sightings
    QList<PlateAudit> loadPlateAudits(const QString &plate = QString(), int limit = 200) const;
    bool logPlateAudit(const PlateAudit &audit);

    QJsonObject alprSummary() const;

    // ── Plate events (docs/SIGHTINGS.md): camera passes and plate searches, their media ──
    // Merge one event per §1.1 (column names as keys; camelCase aliases accepted; metrics / raw as objects or JSON text).
    // local = computed on this host (a local re-run replaces its own earlier result). Returns the surviving uid ("" = refused).
    QString mergePlateEvent(const QJsonObject &ev, bool local, bool *created = nullptr, bool *changed = nullptr, QString *error = nullptr);
    QString resolvePlateUid(const QString &uid) const;                    // an alias (merged away) → the surviving uid
    QJsonObject plateEvent(const QString &uid, bool withRaw = true, bool withMedia = true) const;
    // The feed: seq > since, oldest first; kind "" = both
    QJsonArray plateEventsSince(qint64 since, int limit, const QString &kind, bool *more, qint64 *cursor) const;
    QJsonArray plateEventsLatest(int limit, const QString &kind = QString(), bool withMedia = false) const;   // newest first (time)
    QJsonObject plateEventCounts() const;
    QJsonArray plateEventMedia(const QString &eventUid, const QString &cameraId, bool includeLocal = true) const;   // metadata; includeLocal = webcam stills too (never served onward)
    struct MediaRow {
        QString uid, eventUid, cameraId, kind, mime, originalUrl, originalMime, originalSha256, attribution, license, capturedAt, createdAt;
        int width = 0, height = 0; qint64 bytes = 0, originalBytes = 0; bool jpegReconstructible = false;
    };
    bool storeMedia(MediaRow m, const QByteArray &data);                  // uid = first 32 hex of SHA-256(data); idempotent
    QByteArray mediaData(const QString &uid, MediaRow *meta = nullptr) const;
    int  cameraMediaCount(const QString &cameraId, const QString &kind = QString()) const;
    // A camera's type / OSM tags learned later (the photo lookup): the camera row and every pass of it follow
    void setCameraClassification(const QString &cameraId, const QString &type, const QString &tagsJson);
    // The leaky flag of every pass, from kv hibf_sources (§4.4); returns how many changed
    int  updateLeakyPasses(const QJsonArray &agencies);
    void recountPasses(const QStringList &cameraIds = QStringList());     // flock_cameras.pass_count from the passes
    // Route fixes for the pass detector: one device ("" = this host) or all (device = "*"), time range [from, to] (local ISO, "" open)
    QList<PlateEvents::TrackFix> trackFixes(const QString &fromIso, const QString &toIso, const QString &device = QStringLiteral("*")) const;
    // Fixes stored after `sinceSeq`: per device the time range they cover (the incremental backfill), *maxSeq the newest
    QHash<QString, QPair<QString, QString>> newFixRanges(qint64 sinceSeq, qint64 *maxSeq) const;
    QString firstFixTime() const;
    // Every camera within padM of a track (small boxes along it, through flock_pos)
    QList<FlockCamera> camerasNearTrack(const QList<PlateEvents::TrackFix> &track, double padM = 150.0) const;
    FlockCamera flockCamera(const QString &id, bool *found = nullptr) const;

    // ── Camera trust, road snapping, agency portals (docs/SIGHTINGS.md §2.6, §2.7, §4.6) ──
    struct CameraExtra {
        bool hasTrust = false; double trust = 0.5; QJsonObject trustDetail;   // hasTrust false: not computed yet (NULL)
        QString verdict, verdictAt;                                            // present | absent | ""
        qint64 watchedWay = 0; QJsonObject watchedDetail; QString waysFetched; // the road it watches; when its roads were fetched
        QString agencyPortal;                                                  // the Eyes on Flock portal slug, "" none
    };
    CameraExtra cameraExtra(const QString &id) const;
    void setCameraTrust(const QString &id, double trust, const QJsonObject &detail);
    void setCameraVerdict(const QString &id, const QString &verdict);         // present | absent | "" (cleared); the trust is recomputed
    void setCameraRoads(const QString &id, const QString &fetchedIso, qint64 watchedWay, const QJsonObject &watchedDetail);
    void setCameraAgency(const QString &id, const QString &slug);
    // RF evidence near a point: our det: ALPR rows (not stale) and cameras field-confirmed by an RF detection
    QList<FlockCamera> rfDetectionsNear(double lat, double lon, double radiusM) const;
    void storeWays(const QList<RoadSnap::Way> &ways, const QString &fetchedIso);
    QList<RoadSnap::Way> waysNear(double lat, double lon, double radiusM) const;   // every cached way whose box comes within radiusM
    QJsonArray passesOfCamera(const QString &cameraId) const;                      // its camera_pass rows (metrics as objects, no raw)
    QStringList camerasWithPasses() const;
    QString latestRegion() const;                                                  // the region (state) of the newest fix that has one
    bool updatePassScore(const QString &uid, int confidence, const QJsonObject &metrics);   // a rescore: confidence + metrics, seq when changed
    int  storePortals(const QList<EyesOnFlock::Portal> &portals, const QString &fetchedIso);   // replaces the table
    QList<EyesOnFlock::Portal> loadPortals() const;

    // Route history for Heatmap (all devices and visits): every fix in time order, accuracy ≤ 500 m, teleport spikes dropped.
    // loadAllRouteFixes scans the table; routeFixes is the same list from memory, scanned once and then kept current by
    // every fixes writer (appends extend it, out-of-order rows are merged on the next read; implicitly shared, copies O(1)).
    QList<Fix> loadAllRouteFixes() const;
    QList<Fix> routeFixes() const;
    quint64    routeGeneration() const { return m_routeGen; }        // bumps whenever routeFixes() may have changed
    quint64    routeResetGeneration() const { return m_routeReset; } // bumps when it changed other than by appending
    // Cameras within maxM of the route (one pass over the camera positions against a ~100 m grid of the fixes:
    // O(fixes + cameras), never fixes × cameras); fixes: (index into the list, metres), ascending index
    struct RouteCamHit { FlockCamera cam; QList<QPair<int, double>> fixes; };
    QList<RouteCamHit> camerasAlongRoute(const QList<Fix> &fixes, double maxM = 65.0) const;

    QJsonObject exportJson() const;
    int  importJson(const QJsonObject &dump, QString *error = nullptr);

    void flush();                                              // encrypt + write now (if dirty); waits for a background one
    void scheduleFlush();                                      // the debounced background write, soon (if dirty): never blocks
    static QString runtimeDir();

signals:
    void changed();

private:
    bool ensureKey(bool create);
    bool decryptToLive(bool readOnly);
    bool schema();
    void migrateCameras();                 // once: the 2026-10 camera cleanup (kv camera_clean_v1, docs/DATABASE.md)
    void markDirty();
    void flushAsync();                    // the debounced flush: encrypt + write on a worker
    static bool writeBlob(const QByteArray &key, const QByteArray &plain, const QString &path, QString *error);
    void updatePosition(const QString &bssid, const ApRecord &r, int flags);
    qint64 nextSeq();
    void   loadSeq();
    static QByteArray encrypt(const QByteArray &key, const QByteArray &plain, QString *error);
    static QByteArray decrypt(const QByteArray &key, const QByteArray &blob, QString *error);
    static QByteArray walletKey(bool create, QString *source);
    static QByteArray fileKey(bool create, QString *source);

    QString m_stateDir, m_blobPath, m_livePath, m_error, m_keySource, m_conn;
    QByteArray m_key;
    QSqlDatabase m_db;
    bool m_readOnly = false, m_dirty = false;
    bool m_batch = false;                 // inside saveApRecords: markDirty() only sets the flag
    qint64 m_seq = 0;
    QTimer m_flushTimer;
    QThreadPool m_flushPool;              // one worker: the blob writes, in order
    bool m_flushing = false;
    QElapsedTimer m_dirtyAge;             // since the first unwritten change: the debounce can't put a write off forever
    // routeFixes(): rows (acc ≤ 500) in time order with their stored time text (the SQL sorts on it), and the filtered list
    struct RouteRow { QString t; Fix f; bool own = false; int out = -1; qint64 ms = 0; };   // out: index in m_route, -1 = teleport spike; ms: f.time (no time-zone work in the filter)
    void routeAdd(const QString &t, const Fix &f, bool own);
    void routeInvalidate();
    void routeOwnReplaced(const QList<Fix> &fixes);
    static Fix routeFixRow(const Fix &f, const QString &t);    // f as it reads back from the fixes table (t: its stored time)
    mutable QList<RouteRow> m_routeRaw;
    mutable QList<Fix> m_route;
    mutable bool m_routeLoaded = false, m_routeRefilter = false;
    mutable qsizetype m_routeSorted = 0, m_routeOutAt = 0;     // raw[0, sorted) is in order; raw[0, outAt) is in m_route
    mutable QList<qsizetype> m_routePatch;                     // raw rows whose content changed in place (saveFixes)
    quint64 m_routeGen = 1, m_routeReset = 1;
    mutable QHash<QString, quint64> m_recSig;
    quint64 m_flagsSig = 0;
    QHash<QString, ApPos> m_pins;   // per-record signature of what is stored: saveApRecords skips unchanged records
    static quint64 recordSignature(const ApRecord &r, int flags);
};
