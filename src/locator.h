#pragma once
#include "wifiscanner.h"
#include "estimator.h"
#include "ranging/anchors.h"
#include "ranging/rangemath.h"
#include <QColor>
#include <algorithm>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QRegularExpression>

struct Fix {
    bool      valid = false;
    double    lat = 0, lon = 0;
    double    accuracy = -1;      // metres (-1 unknown)
    QString   source;             // starlink | wifi | ip
    QString   provider;           // wifi fixes: beacondb | apple
    QString   place;
    QDateTime time;
    int       apCount = 0;        // APs seen
    int       apUsed  = 0;        // APs sent to BeaconDB
    QDateTime departed;           // history only: last time we were still confirmed here
    double    elevation = -9999;  // metres above sea level (-9999 unknown)
    QString   city, region, country;   // from the reverse geocode (may be empty on old entries)
    bool      precise() const { return valid && source != QLatin1String("ip"); }
    bool      hasElevation() const { return elevation > -9000; }
    QJsonObject toJson() const;
    static Fix fromJson(const QJsonObject &o);
};

struct ApObservation {
    double lat = 0, lon = 0; double acc = 0; int dbm = -100; QDateTime time;
    QString device;                   // who heard it: "" = this host, else the LAN-API device id / peer name
    qint64  id = 0;                   // database row (0 = not stored yet)
    bool    dirty = false;            // changed in memory since it was stored
};

using BfAnchor = Anchors::Anchor;     // (Locator has a D-Bus method called Anchors(): the alias keeps the type reachable)
class RangingService;

// A place an AP was heard, at whatever precision the fix had (IP fixes included):
// two sightings further apart than their combined error mean the AP travels with us.
struct ApSighting { double lat = 0, lon = 0; double acc = 0; QDateTime time; };

// Everything we've learned about one BSSID across stops
struct ApRecord {
    QString ssid;
    QSet<QString> cells;              // ~5 km cells it was seen in (travelling detection)
    QList<ApObservation> obs;         // where we were + how loud it was (precise fixes only)
    QList<ApSighting> seen;           // coarse places it was heard (any fix)
    int freq = 0;
    bool wigle = false;               // WiGLE knows where it is
    double wLat = 0, wLon = 0;
    QDateTime wigleChecked;
    QString security;                 // last seen: open owe wep wpa1 wpa2-tkip wpa2 wpa2-eap wpa3 wpa2/3 wpa3-eap192
    int secFlags = 0, wpaFlags = 0, rsnFlags = 0, maxKbps = 0;
    bool adhoc = false;
    Estimator::Fit fit;               // our own multilateration from obs (valid when the geometry allows it)
    bool   fitDirty = false;          // new observations since the last full fit
    bool   fitCurrent = false;        // the fit came from this estimator version (else it is recomputed at start-up)
    double peerLat = 0, peerLon = 0, peerAcc = 0;   // a position another BeaconFix / the phone synced to us
    QString peerFrom;                 // which device ("" = none)
    bool   hasPeer() const { return !peerFrom.isEmpty() && peerAcc > 0; }
};

// Where we think an AP is, for the map
struct ApEstimate {
    enum Kind { None, Ring, Centroid, Wigle, Observed, Trilat, Peer, Anchor, Region, Mobile } kind = None;   // Observed: one place we heard it · Trilat: our fit · Peer: synced from another device · Anchor: surveyed (docs/RANGING.md §4) · Region: our fit, region only (grade R) · Mobile: travels (grade M)
    double lat = 0, lon = 0;          // Centroid / Wigle / Trilat / Peer: the estimate. Ring: our own position.
    double radiusM = 0;               // Ring: RSSI distance. Others: uncertainty.
    double bearingDeg = 0;            // Ring only: stable pseudo-bearing (bearing is unknown)
    int    vantage = 0;               // Centroid / Trilat: distinct places it was heard from
    Estimator::Fit fit;               // Trilat: the fit statistics
    // When a placement (WiGLE / Apple) and our own fit disagree by more than 3× their accuracy, both are reported
    bool   hasAlt = false; Kind altKind = None; double altLat = 0, altLon = 0, altAcc = 0;
    static const char *kindName(Kind k) { switch (k) { case Ring: return "ring"; case Centroid: return "centroid"; case Wigle: return "wigle"; case Observed: return "observed"; case Trilat: return "trilat"; case Peer: return "peer"; case Anchor: return "anchor"; case Region: return "region"; case Mobile: return "mobile"; default: return "none"; } }
};

// Point-of-interest category (OpenStreetMap tags → icon, colour, label)
struct PoiCategory {
    QString key, label, icon;         // icon: emoji glyph
    QColor  color;
    QString group;                    // services | civic | kids  (menus group by this)
    bool    wide = false;             // civic: fetched out to the wide radius (police/fire are sparse in the country)
    int     reachKm = 0;              // searched out to this far by a separate query (pediatric ERs); 0 = the wide rule above
};

struct Poi {
    QString cat, name, detail;        // detail: brand / hours / fuel types etc., one line
    double  lat = 0, lon = 0;
    QString osmType; qint64 osmId = 0;
    bool    wifi = false;             // advertises internet access
    QString hours, phone, website;
    QString address;                  // "123 Main St, Town, ST 12345" from addr:* tags
    QString wheelchair;               // yes | limited | no | ""
    bool    emergency = false;        // hospital with an emergency department / 24 h service
    // Pediatric ERs (src/poiclassify.h): tier 0 none · 1 pediatric ER · 2 children's hospital, ER not confirmed ·
    // 3 general ER with a pediatrics dept. · 4 pediatric urgent care (not an ER)
    int     peds = 0;
    QString er;                       // "yes" | "no" | "" (unknown)
    QString campus;                   // tier 2: the ER hospital on the same campus
    int     driveS = 0, driveM = 0;   // drive time / road distance from the fix (pois() only; 0 = not computed)
    bool    driveEst = true;          // an estimate (straight line × 1.4 at 70 km/h), not a routed time
    QString scope = QStringLiteral("near");   // near: the main places query · far: the pediatric ER search
};

// Local solar times for the fix (computed, no network)
struct SunTimes {
    bool valid = false;
    QDateTime sunrise, sunset, solarNoon, goldenMorningEnd, goldenEveningStart, civilDawn, civilDusk;
    int dayLengthSecs = 0;
    bool polarDay = false, polarNight = false;
    bool isDay(const QDateTime &now) const { return polarDay || (!polarNight && sunrise.isValid() && now >= sunrise && now < sunset); }
};

struct RankTier { int at; const char *name; };

// A small, persistent set of milestones. Display only: nothing to click, nothing to win.
struct Achievement {
    QString key, title, desc, icon;
    QDateTime unlocked;               // invalid = locked
};

// One entry of the trip log with what we know about the stay
struct Stop {
    Fix fix;
    int dwellSecs = -1;               // -1 unknown (no departure recorded yet)
    double legKm = -1;                // distance from the previous precise stop (-1 n/a)
    int legSecs = -1;                 // travel time of that leg (-1 unknown)
};

// Something that happened on the map: a beacon appeared / faded / got louder,
// was placed, a fix or stop, a milestone. Kept as a short ring buffer for the
// views to animate and the ticker to list. Newest last.
struct BeaconEvent {
    int       id = 0;
    QString   type;                   // ap_new ap_lost ap_up ap_down ap_placed fix stop achievement region prefetch error
    QDateTime time;
    QString   text;                   // one human line
    QString   bssid, ssid;            // ap_*
    int       dbm = 0, delta = 0;     // ap_*: level; ap_up/ap_down: change in dB
    bool      hasPos = false, hasFrom = false;
    double    lat = 0, lon = 0;       // the thing's position (ap_*: its estimate; fix/stop: the fix)
    double    fromLat = 0, fromLon = 0;   // fix/stop: where we were; ap_placed: where the ring guess was
    double    r = 0, bearing = 0;     // ap_* with kind "ring": RSSI distance + pseudo-bearing from the fix
    QString   kind, status;           // ap_*: ring|centroid|wigle · used|travelling|ignored|active|nomap
    QString   security;               // ap_*: open|wep|wpa1|wpa2-tkip|wpa2|…
    QJsonObject extra;                // type-specific fields merged into the JSON (ap_refit: acc, prevAcc, n, vantage, rms, vantagePoints[]; device: distanceM …)
    QJsonObject toJson() const;
};

// The newest known position of one of OUR other devices (phone, laptop, another desktop):
// from synced fixes, from POST /api/v1/devices/position, or from a peer BeaconFix.
struct DevicePos {
    QString device, kind, identityId, identityName, source, place;
    double lat = 0, lon = 0, acc = 0;
    QDateTime time, lastSeen;
    int beacons = 0;
    bool online = false;
    QJsonObject toJson() const;
};

struct Stats {
    int stops = 0;
    double distanceKm = 0;            // all history, every fix (v1 semantics)
    int beaconsTotal = 0;             // distinct BSSIDs ever seen
    int beaconsNow = 0, usedNow = 0, travellingNow = 0, locatedNow = 0;
    double bestAccuracy = -1;
    QString rank; int rankLevel = 0; int nextRankAt = 0; int rankAt = 0; int rankCount = 0;
    // Trip intelligence (precise fixes only — IP fixes are the ground-station city)
    double distanceTodayKm = 0, distanceTripKm = 0, distanceAllKm = 0;
    qint64 movingSecs = 0, stoppedSecs = 0;          // all-time, from recorded arrivals/departures
    qint64 movingTripSecs = 0, stoppedTripSecs = 0;
    double speedKmh = -1;             // last leg (-1 unknown)
    double headingDeg = -1;           // direction of the last leg (-1 unknown)
    bool   moving = false;            // arrived < 1 h ago after a leg
    qint64 dwellSecs = -1;            // at the current stop (confirmed)
    QDateTime tripStart; int tripDays = 0; int stopsTrip = 0; int stopsToday = 0;
    QStringList cities, regions, countries;          // visited (precise fixes), in first-seen order
    int achievementsUnlocked = 0, achievementsTotal = 0;
    double longestLegKm = 0; qint64 longestStaySecs = 0; QString longestStayPlace;
    // Home networks (the RV): heard now? where was the RV last precisely fixed? how far are we from it?
    bool   atHome = false;
    bool   homeKnown = false;         // a home fix exists
    double homeLat = 0, homeLon = 0; QDateTime homeTime;
    double awayKm = -1;               // -1 = at home / unknown
    QString awayText;                 // "12 km NE of the RV" / "at the RV"
};

// Runs the probe chain (Starlink dish GPS → BeaconDB Wi-Fi → IP) on a timer,
// keeps state + history on disk, and is exported on the session bus as
// org.sworrl.BeaconFix so the tray, the plasmoid and other apps share one fix.
class ApiServer;
class MapDb;
class Identity;
class Notifier;
class OsIntegration;

class Locator : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.sworrl.BeaconFix")
    Q_PROPERTY(bool    valid     READ valid)
    Q_PROPERTY(double  latitude  READ latitude)
    Q_PROPERTY(double  longitude READ longitude)
    Q_PROPERTY(double  accuracy  READ accuracy)
    Q_PROPERTY(QString source    READ source)
    Q_PROPERTY(QString place     READ place)
    Q_PROPERTY(QString timestamp READ timestamp)
    Q_PROPERTY(int     apCount   READ apCount)
    Q_PROPERTY(int     intervalMinutes READ intervalMinutes WRITE setIntervalMinutes)
    Q_PROPERTY(double  elevation READ elevation)
    Q_PROPERTY(double  speedKmh  READ speedKmh)
    Q_PROPERTY(double  headingDeg READ headingDeg)
    Q_PROPERTY(QString rank      READ rank)
    Q_PROPERTY(QString geoUri    READ geoUri)
    Q_PROPERTY(int     lastEventId READ lastEventId)
    Q_PROPERTY(bool    apiListening READ apiListening)
    Q_PROPERTY(bool    atHome    READ atHome)
    Q_PROPERTY(double  awayKm    READ awayKm)
public:
    explicit Locator(bool standalone, QObject *parent = nullptr);

    bool    valid()     const { return m_fix.valid; }
    double  latitude()  const { return m_fix.lat; }
    double  longitude() const { return m_fix.lon; }
    double  accuracy()  const { return m_fix.accuracy; }
    QString source()    const { return m_fix.source; }
    QString place()     const { return m_fix.place; }
    QString timestamp() const { return m_fix.time.toString(Qt::ISODate); }
    int     apCount()   const { return m_fix.apCount; }
    int     intervalMinutes() const { return m_intervalMin; }
    int     lastEventId() const { return m_eventId; }
    const QList<BeaconEvent> &events() const { return m_events; }
    int     liveScanSeconds() const { return m_liveScanSecs; }
    void    setLiveScanSeconds(int s);
    double  speedKmh()  const { return stats().speedKmh; }
    double  headingDeg() const { return stats().headingDeg; }
    QString rank()      const { return stats().rank; }
    void    setIntervalMinutes(int m);

    const Fix &fix() const { return m_fix; }
    const Fix &lastProbe() const { return m_last; }
    QString lastError() const { return m_lastError; }
    QString coarseNote() const { return m_coarseNote; }
    bool    busy() const { return m_busy; }
    bool    geocodePending() const { return m_geocodePending; }
    const QList<AccessPoint> &accessPoints() const { return m_aps; }
    const QList<Fix> &history() const { return m_history; }
    const QList<Fix> &importedHistory() const { return m_imported; }   // positions/visits from imported exports (device timeline/wigle/gpx/kml), never the live fix
    bool importing() const { return m_importing; }
    QString wifiInterface() const { return m_scanner.interfaceName(); }

    QString    apStatus(const AccessPoint &ap) const;   // used | active | ignored | travelling | nomap
    bool       isTravelling(const QString &bssid) const;
    ApEstimate estimateFor(const AccessPoint &ap) const;
    const ApRecord *record(const QString &bssid) const;
    Stats      stats() const;
    QList<Stop> stops() const;                          // trip log enriched with dwell / leg data
    static const QList<RankTier> &rankLadder();
    const QList<Achievement> &achievements() const { return m_achievements; }
    static SunTimes sunTimes(double lat, double lon, const QDateTime &when);
    SunTimes sun() const;                               // at the fix, for today
    double  elevation() const { return m_fix.elevation; }
    QString elevationText() const;
    QString elevationNote() const { return m_elevNote; }
    static QString compass(double deg);
    static QString durationText(qint64 secs);
    // Sharing
    QString coordsText() const;
    QString geoUri() const;
    QString osmUrl() const;
    QString googleMapsUrl() const;
    QString appleMapsUrl() const;
    QString shareText() const;
    // Trip
    void    startTrip(const QDateTime &at = QDateTime::currentDateTime());
    QDateTime tripStart() const { return m_tripStart; }
    static bool looksMobile(const QString &ssid);
    static double rssiDistanceM(int dbm, int freqMHz = 2437);
    static double bearingDeg(double lat1, double lon1, double lat2, double lon2);

    // Points of interest around the fix (OpenStreetMap via Overpass)
    static const QList<PoiCategory> &poiCategories();
    static const PoiCategory *poiCategory(const QString &key);
    static QString poiGroupLabel(const QString &group);           // "Emergency & civic" …
    QJsonObject emergencyJson() const;                            // nearest police / fire / ER / pediatric ER / urgent care + the local number (docs/API.md)
    QList<Poi> poisMatching(const QStringList &catsOrGroups, double radiusKm) const;   // nearest first
    const QList<Poi> &pois() const { return m_allPois; }         // near + far merged (deduped by OSM object, near wins); drive times from the fix
    QString poiNote() const { return m_poiNote; }
    bool    poisLoading() const { return m_poiBusy; }
    int     poiRadiusKm() const { return m_poiRadiusKm; }
    void    setPoiRadiusKm(int km);
    void    setTileBase(const QString &url) { m_tileBase = url; }
    void    refreshPois(bool force = false);
    // Pediatric ERs: a separate, rarer query out to pedsRadiusKm (QSettings "pedsRadiusKm", 50–300, default 150)
    int     pedsRadiusKm() const { return m_pedsRadiusKm; }
    void    setPedsRadiusKm(int km);
    void    refreshPediatric(bool force = false);
    bool    pedsLoading() const { return m_pedsBusy; }
    QString pedsNote() const;                                     // the pediatricNote of emergencyJson()
    QDateTime pedsTime() const { return m_pedsTime; }
    QJsonObject poiOriginJson() const;                            // {lat, lon, time, radiusKm} of the places query
    QJsonValue  pedsOriginJson() const;                           // the same for the pediatric search, or null
    static double distanceM(double lat1, double lon1, double lat2, double lon2);

    // Settings
    int         moveThresholdM() const { return m_moveThresholdM; }
    bool        useStarlink()    const { return m_useStarlink; }
    QString     starlinkHost()   const { return m_starlinkHost; }
    bool        useIp()          const { return m_useIp; }
    bool        useApple()       const { return m_useApple; }
    bool        ignoreActiveAp() const { return m_ignoreActive; }
    QStringList ignorePatterns() const { return m_ignore; }
    QString     wigleToken()     const { return m_wigleToken; }
    bool        notifyStops()    const { return m_notifyStops; }
    bool        notifyRegions()  const { return m_notifyRegions; }
    bool        notifyAchievements() const { return m_notifyAchievements; }
    bool        prefetchTiles()  const { return m_prefetch; }
    bool        useElevation()   const { return m_useElevation; }
    void setNotifyStops(bool b);
    void setNotifyRegions(bool b);
    void setNotifyAchievements(bool b);
    void setPrefetchTiles(bool b);
    void setUseElevation(bool b);
    void notePrefetchDone(int tiles);                   // TileSource tells us (achievement + note)
    void notify(const QString &summary, const QString &body, const QString &icon = QString());
    // With buttons: actions = key, label pairs; onAction gets the key ("default" = the body was clicked)
    void notifyWithActions(const QString &summary, const QString &body, const QString &icon, const QStringList &actions, std::function<void(const QString &)> onAction, int timeoutMs = 15000);
    void logEvent(BeaconEvent e);                    // append to the event log (also used by the API server)
    void setMoveThresholdM(int m);
    void setUseStarlink(bool b);
    void setStarlinkHost(const QString &h);
    void setUseIp(bool b);
    void setUseApple(bool b);
    void setIgnoreActiveAp(bool b);
    void setIgnorePatterns(const QStringList &l);
    // Home networks: SSID / BSSID globs for the networks that travel with the RV (never used for positioning,
    // never "appear"/"fade"; they tell us whether we are at the RV and where the RV is).
    QStringList homeNetworks() const { return m_home; }
    void setHomeNetworks(const QStringList &l);
    void addHomeNetwork(const QString &p);
    void removeHomeNetwork(const QString &p);
    bool isHome(const AccessPoint &ap) const;
    bool atHome() const;
    double awayKm() const { return stats().awayKm; }
    const Fix &homeFix() const { return m_homeFix; }
    QStringList suggestHomeNetworks() const;      // strongest APs that look like our own router family
    static QStringList parseHomeNetworksFile(const QString &path, QString *error = nullptr);   // UniFi export: patterns[] + bssids[].bssid
    int  importHomeNetworks(const QString &path, QString *error = nullptr);                    // merge, idempotent; returns how many were new
    void addIgnorePattern(const QString &p);
    void setTravelling(const QString &bssid, bool travelling);
    void setWigleToken(const QString &t);

    void    start();
    void    setApiServer(ApiServer *api);              // the LAN API, forwarded over D-Bus below
    ApiServer *apiServer() const { return m_api; }
    MapDb  *mapDb() const { return m_db; }
    Identity *identity() const { return m_identity; }
    OsIntegration *os() const { return m_os; }
    QString countryCode() const { return m_countryCode; }   // ISO 3166-1 alpha-2 of the fix, lower-case (from the reverse geocode)
    int     rebuildDbFromJson();                    // re-import the *.migrated JSON files into the database
    bool    apiListening() const;
    bool    exportGpx(const QString &path, QString *error) const;
    static QString stateDir();
    // Observations from other devices (the LAN API / sync): stored, merged into the records, refit queued
    int     ingestObservations(const QJsonArray &observations, const QString &device, QString *error = nullptr);
    int     mergePeerAps(const QJsonArray &aps, const QString &device);
    int     appendPeerFixes(const QJsonArray &fixes, const QString &device);
    void    queueRefit(const QString &bssid);
    // Anchors (docs/RANGING.md §4): surveyed transmitters / places
    QList<BfAnchor> anchors() const { return m_anchors; }
    QJsonArray anchorsJson() const;
    // Create / update by id (validated + normalised); placedByDefault fills an empty placedBy. ok=false → *error says why.
    BfAnchor setAnchor(const QJsonObject &o, const QString &placedByDefault, bool *ok, QString *error);
    bool    removeAnchor(const QString &id);
    int     mergeAnchors(const QJsonArray &rows);          // from a peer's /db/sync (newest placedAt / deletedAt wins)
    const BfAnchor *pinnedAnchor(const QString &bssid) const;   // wifi-ap / rtt-responder / this-computer anchor carrying this BSSID
    // Environment calibration from anchors (§4.3.3): per band P0 / n of the path-loss model, and the n the AP fits start from
    double  environmentN(int freqMHz) const;
    QJsonObject environmentJson() const;
    // The estimator (docs/GRADING.md): priors per band, calibration, device offsets, BSSID groups, grade counts
    Estimator::Options estimatorOptions(int freqMHz) const;
    QJsonObject estimatorJson() const;
    static QJsonObject fitJson(const Estimator::Fit &f);  // the "fit" object of the AP JSON (API / D-Bus / sync)
    void    calibrateEstimator();                        // anchor leave-one-out κ + per-device offsets + BSSID groups
    // Device ranging (docs/RANGING.md §5–§8); set by the tray
    void    setRanging(RangingService *r) { m_ranging = r; }
    RangingService *ranging() const { return m_ranging; }
    QString kindForDevice(const QString &device, const QString &hint = QString()) const;   // android | laptop | desktop | pi | gnss | device
    static QStringList features();                       // what this build can do (hello / StateJson)
    // The pediatric ER search: the server-side [timeout:] (the client waits 30 s longer), and the back-off after
    // `failures` failed searches in a row: 10, 20, 40 … minutes, at most 4 h
    static constexpr int kPedsServerTimeoutS = 90;
    // The pediatric classifier's rules version: a saved far list classified by older rules is searched again once
    // (2: not-an-ER specialities, offices and departments; confirmed pediatric ERs kept first)
    static constexpr int kPedsClassifierVersion = 2;
    static int pedsBackoffS(int failures) { return failures <= 1 ? 600 : std::min(4 * 3600, 600 << std::min(failures - 1, 5)); }
    // Our other devices on the map (docs/API.md "Devices")
    QJsonArray linkedDevices() const;
    QJsonArray apsJson() const;                          // the beacons heard now, as in StateJson "aps" (GET /api/v1/aps pages it)
    QList<DevicePos> devicePositions() const { return m_devicePos.values(); }
    void    noteDevicePosition(const QString &device, const QString &kind, double lat, double lon, double acc, const QDateTime &time, const QString &source, int beacons,
                               const QString &identityId = QString(), const QString &identityName = QString(), const QString &place = QString());
    void    noteDeviceSeen(const QString &device, const QString &kind = QString());
    int     refitCount() const;                          // records with a valid fit of our own
    // Sync with another BeaconFix (a laptop feeding the RV desktop, or the other way round)
    struct SyncPeer { QString url, token, name; int minutes = 15; QDateTime last; QString lastResult; bool ok = false; };
    QList<SyncPeer> syncPeers() const { return m_syncPeers; }
    void    setSyncPeers(const QList<SyncPeer> &peers);
    bool    syncBusy() const { return m_syncBusy; }

public slots:
    void    Refresh();
    void    ShowWindow();
    QString StateJson() const;
    bool    ExportGpx(const QString &path);
    bool    CopyToClipboard(const QString &what);       // coords | geo | osm | google | apple | text
    void    StartTrip();
    void    PrefetchTiles();
    void    RefreshPlaces() { refreshPois(true); refreshPediatric(true); }   // re-query OpenStreetMap for places around the fix (and the pediatric ERs)
    // LAN API management (see apiserver.h)
    QString ApiStatus() const;
    bool    ApproveDevice(const QString &id);
    bool    DenyDevice(const QString &id);
    bool    RevokeDevice(const QString &nameOrId);
    QString CreateToken(const QString &name, const QString &scopes);   // scopes: "read" or "read,control"; returns the token once
    bool    OpenPairing(int minutes);
    QStringList HomeNetworks() const { return m_home; }
    int     ImportHomeNetworks(const QString &path) { return importHomeNetworks(path); }
    QString KnownDevices() const;
    QString DbStats() const;
    bool    DbExport(const QString &path);
    int     DbImport(const QString &path);
    bool    KnownAdd(const QString &mac, const QString &name);
    bool    KnownRemove(const QString &mac);
    int     KnownImport(const QString &path);
    void    SetHomeNetworks(const QStringList &patterns) { setHomeNetworks(patterns); }
    int     Refit();                                     // full refit of every beacon with samples; returns valid fits
    QString EstimatorJson() const;                       // JSON: GET /api/v1/estimator (calibration, device offsets, groups, grades)
    QString Sync(const QString &url, const QString &token);   // one sync round with another BeaconFix; JSON result.
                                                              // url may be a peer's name / hostname / address (resolved through mDNS);
                                                              // token may be empty when our identity is the peer's or linked to it
    QString Peers(bool scan);                             // JSON: BeaconFix devices on this network (scan=true probes the /24s too)
    QString LinkedDevices() const;                       // JSON array: newest position of each of our other devices
    QString Import(const QString &path, const QString &optsJson);   // history importers (docs/DATABASE.md): JSON summary; progress via importProgress()
    // Identity (docs/IDENTITY.md)
    QString IdentityJson() const;                        // public record + linkedIds + pending link requests
    bool    IdentityCreate(const QString &name);
    QString IdentityExport(const QString &passphrase);   // "BFID1:…" or "" on error
    bool    IdentityImport(const QString &textOrPath, const QString &passphrase);
    QString IdentityLinkPayload() const;                 // BFLNK1:… (what our link QR carries)
    QString IdentityAcceptLink(const QString &statementJson);   // completed statement JSON, or {"error":…}
    bool    IdentityForget();
    void    IdentityReload();                            // re-read the file (after a CLI change)
    // Anchors + ranging (docs/RANGING.md)
    QString Anchors() const;                             // JSON array of the anchors
    QString SetAnchor(const QString &json);              // create / update → the anchor id, or "error: <why>"
    bool    RemoveAnchor(const QString &id);
    QString Ranging() const;                             // JSON: GET /api/v1/ranging
    QString RangingInfo() const;                         // JSON: GET /api/v1/ranging/info
    QString RangingCalibrate(const QString &device, double distanceM, int durationS);
    bool    GrantControl(const QString &nameOrId);       // add the control scope to a paired device's existing token
    // OS integration
    QString ApplyOs(bool dryRun);                        // JSON: what was (or would be) applied
    QString TimeZoneForFix() const;

signals:
    void FixChanged();
    void stopAdded(double lat, double lon);             // a new entry in the trip log
    void achievementUnlocked(const QString &key, const QString &title);
    void notificationFallback(const QString &summary, const QString &body);   // no notification daemon
    void prefetchRequested();
    void elevationUpdated();
    void showWindowRequested();
    void probeStarted();
    void probeFinished(bool ok, const QString &message);
    void scanUpdated();
    void poisUpdated();
    void statusMessage(const QString &message);
    void eventLogged(const QString &json);          // one BeaconEvent, as JSON
    void syncFinished(const QString &url, bool ok, const QString &message);
    void refitDone(int refitted);
    void pairingRequested(const QString &json);     // LAN API: a device asked for access (id, name, ip, code)
    void pairingOpenRequested(const QString &id);   // the pairing notification was clicked: show the dialog for that request
    void peersChanged();                            // mDNS: the list of BeaconFix devices on this network changed
    void importProgress(const QString &json);       // {"file","percent","stage"} while an import runs, then {"done":true,"summary":{…}}
    void deviceApproved(const QString &name);

private:
    void tryStarlink();
    void loadImported();
    void onScan(const QList<AccessPoint> &aps);
    void onScanFinished(const QList<AccessPoint> &aps);   // dispatcher: diff → probe path or live path
    void liveScan();
    void startProbeScan();
    void diffScan(const QList<AccessPoint> &aps);
    BeaconEvent apEvent(const QString &type, const AccessPoint &ap) const;
    void queryBeaconDb(const QList<AccessPoint> &usable);
    void tryApple(const QList<AccessPoint> &usable, const QString &why);
    void tryIp(const QString &why);
    void accept(Fix cand, const QString &ipCity = QString());
    void finish(bool ok, const QString &message);
    void reverseGeocode(double lat, double lon);
    void loadState();
    void saveState() const;
    void appendHistory(const Fix &f);
    void noteObservations(const QList<AccessPoint> &aps, const Fix &at);
    void saveApRecords();
    void saveRecord(const QString &bssid);
    void refitQueued();
    bool refitOne(const QString &bssid, qint64 now);
    QList<Estimator::Obs> obsFor(const ApRecord &r, const QString &bssid) const;
    Estimator::Context contextFor(const ApRecord &r, const QString &bssid, const QList<Estimator::Obs> &obs) const;
    QList<Estimator::Miss> missesFor(const QList<Estimator::Obs> &obs) const;
    void noteScanCell(const Fix &at);
    void thinObservations(ApRecord &r);
    void rebuildGroups();
    void calibrateDeviceOffsets();
    void calibrateAnchors();
    void finishUpgradeRefit();
    void queueUpgradeRefits();
    void loadSyncPeers();
    void saveSyncPeers() const;
    void syncStep(int peerIndex);
    bool matchesIgnore(const AccessPoint &ap) const;
    void queueWigle();
    void pumpWigle();
    void noteSightings(const QList<AccessPoint> &aps, const Fix &at);
    void queryOverpass(double lat, double lon, int radiusM, int mirror);
    void queryPediatric(double lat, double lon, int radiusM, int mirror);
    bool overpassSlot(bool peds, bool force);     // one Overpass query at a time, 5 s apart: false = queued
    void overpassDone();
    void pumpOverpass();
    void rebuildMergedPois();
    struct HelpPicks { const Poi *pediatric = nullptr, *closer = nullptr, *urgent = nullptr, *hospital = nullptr; };
    HelpPicks helpPicks() const;                  // PoiClassify::pickHelp over pois(), from the fix
    bool helpOrigin(double *lat, double *lon) const;   // the fix, else where the places were fetched
    QString pedsNoteFor(const Poi *pick) const;
    QJsonObject helpPlaceJson(const Poi &p) const;
    void loadPois();
    void savePois() const;
    void savePedsPois() const;
    void rewriteHistory() const;
    void fetchElevation();
    void loadElevationCache();
    void saveElevationCache() const;
    void loadAchievements();
    void saveAchievements() const;
    void checkAchievements();
    void unlock(const QString &key);
    void noteVisited(const Fix &f, bool announce);
    static void parsePlace(const Fix &f, QString *city, QString *region, QString *country);

    bool m_standalone;
    ApiServer *m_api = nullptr;
    MapDb *m_db = nullptr;
    bool m_dbUsable = false;              // open and writable: persistence goes through it
    Identity *m_identity = nullptr;
    Notifier *m_notifier = nullptr;
    QHash<QString, DevicePos> m_devicePos;
    QTimer m_deviceTimer;
    OsIntegration *m_os = nullptr;
    QString m_countryCode;
    void loadFromDb();
    void migrateJsonToDb();
    bool tryInternal(const QList<AccessPoint> &usable);
    QHash<QString, int> apFlags() const;    // bit0 home, bit1 travelling, bit2 ignored, per known BSSID
    int apFlag(const QString &bssid) const;  // the same for one BSSID (saveRecord: no pass over every record)
    // Anchors
    QList<BfAnchor> m_anchors;
    QHash<QString, BfAnchor> m_pins;         // BSSID → anchor
    void loadAnchors();
    void anchorsChanged();                   // rebuild pins, tell the map database, repaint
    bool anchorFix(Fix &cand) const;         // this-computer anchor within 100 m replaces the fix
    void maybeReproject(const Fix &cand);    // the RV moved: re-project the RV anchors (§4.3.5)
    void noteAnchorCalibration(const QList<AccessPoint> &aps);   // RSSI at known distance → environment P0 / n
    bool tryRvGnss();                        // §4.3.7
    QHash<QString, RangeMath::Rls2> m_envRls;   // band ("2.4" | "5" | "6") → path-loss fit
    QHash<QString, int> m_envSamples;
    RangingService *m_ranging = nullptr;
    WifiScanner m_scanner;
    QNetworkAccessManager m_nam;
    QTimer m_timer;
    QTimer m_wigleTimer;
    QTimer m_liveTimer;
    int    m_liveScanSecs = 45;
    bool   m_liveScan = false;          // a live (non-probe) scan is in flight
    bool   m_probeWantsScan = false;    // a probe asked while a live scan was in flight
    bool   m_firstScanDone = false;
    QDateTime m_lastLiveProbe;
    QHash<QString, int> m_lastDbm;      // last level per BSSID we consider "in range"
    QHash<QString, QDateTime> m_lastSeenAt;   // last scan each BSSID was heard in (event de-noising)
    QHash<QString, int> m_missing;      // consecutive scans a BSSID has been absent
    QList<BeaconEvent> m_events;
    int    m_eventId = 0;
    bool   m_busy = false;
    bool   m_geocodePending = false;
    Fix    m_fix, m_last;
    QString m_lastError, m_starlinkError, m_coarseNote;
    QList<AccessPoint> m_aps;
    QList<Fix> m_history;
    QList<Fix> m_imported;                          // docs/DATABASE.md "Importing your history"
    bool m_importing = false, m_bulkImport = false;  // bulk: no per-batch repaint, refits deferred to the end
    QHash<QString, ApRecord> m_apRecords;
    QSet<QString> m_travelling, m_notTravelling;
    QStringList m_wigleQueue;
    bool m_wigleBusy = false;
    QList<Poi> m_pois;                              // near: the main places query
    QList<Poi> m_allPois;                           // near + far, what pois() returns
    double m_poiLat = 0, m_poiLon = 0; int m_poiRadiusM = 0;
    QDateTime m_poiTime, m_poiTried;
    QString m_poiNote;
    bool m_poiBusy = false;
    int  m_poiRadiusKm = 6;
    // Pediatric ERs (far scope)
    QList<Poi> m_pedsPois;
    double m_pedsLat = 0, m_pedsLon = 0; int m_pedsRadiusM = 0;
    QDateTime m_pedsTime;                           // when the cached far list was fetched (invalid = never)
    QDateTime m_pedsBusyUntil;                      // back-off after a failed search
    QDateTime m_pedsSkipLogged;                     // the fix we last logged "skipped (IP fix)" for
    int  m_pedsRadiusKm = 150;
    bool m_pedsBusy = false, m_pedsFailed = false;
    int  m_pedsFailCount = 0;                        // failed pediatric searches in a row (back-off: pedsBackoffS)
    int  m_pedsClassifier = 1;                      // the rules version the saved far list was classified with (settings)
    QString m_pedsNote;                             // the last search's own message ("Overpass busy — will retry" …)
    QTimer m_pedsRetryTimer;
    // Overpass etiquette: one query in flight, 5 s between queries, a minute's pause after 429 / 504
    QTimer m_overpassTimer;
    QDateTime m_overpassIdle, m_overpassCoolUntil;
    bool m_poiPending = false, m_poiPendingForce = false, m_pedsPending = false, m_pedsPendingForce = false;
    QString m_tileBase;
    QHash<QString, double> m_elevCache;   // "lat,lon" rounded to ~100 m
    QString m_elevNote; bool m_elevBusy = false; QDateTime m_elevTried;
    QList<Achievement> m_achievements;
    QDateTime m_tripStart;
    QSet<QString> m_seenRegions, m_seenCountries;
    int m_prefetchedTiles = 0;

    int m_intervalMin = 15;
    int m_moveThresholdM = 250;
    bool m_useStarlink = true;
    QString m_starlinkHost = QStringLiteral("192.168.100.1");
    bool m_useIp = true;
    bool m_useApple = true;
    bool m_ignoreActive = true;
    QStringList m_ignore;
    QStringList m_home;
    QList<QRegularExpression> m_homeRe, m_ignoreRe;   // compiled once per list change: isHome/matchesIgnore run on every paint
    void rebuildPatternCaches();
    QHash<QString, QString> m_homeSsids;     // BSSID → SSID from the UniFi export (home-networks.json bssids[])
    QHash<QString, QDateTime> m_insecureNoted; // ap_insecure once per BSSID per 24 h
    QSet<QString> m_refitQueue;         // BSSIDs with new samples, refit in a batch
    QTimer m_refitTimer;
    // Estimator state (docs/GRADING.md)
    struct ScanCell { double lat = 0, lon = 0; int count = 0; qint64 first = 0, last = 0; };
    QHash<QString, ScanCell> m_scanCells;                // ~15 m cells this host scanned from (misses)
    QHash<QString, QStringList> m_scanIndex;             // 0.01° bucket → scan cell keys
    struct GroupInfo { QString ref; double offsetDb = 0; int size = 1; };
    QHash<QString, GroupInfo> m_groups;                  // member BSSID → its group (multi-BSSID pooling)
    QHash<QString, double> m_devOffsets;                 // device → dB it hears louder than this host
    double m_kappa = 1.0;                                // anchor calibration (σ multiplier)
    QJsonObject m_calibration;                           // last anchor leave-one-out result
    QSet<QString> m_upgradePending;                      // estimates still to recompute with the new engine
    bool   m_upgradeActive = false;
    QTimer m_calibrateTimer;
    QList<SyncPeer> m_syncPeers;
    QTimer m_syncTimer;
    bool m_syncBusy = false;
    int  m_syncCursorPeer = -1;
    Fix m_homeFix;                      // last precise fix taken while a home AP was heard
    bool m_homeInRange = false;         // for the single "home" event on transitions
    QString m_wigleToken;
    bool m_notifyStops = true, m_notifyRegions = true, m_notifyAchievements = true;
    bool m_prefetch = true, m_useElevation = true;
};
