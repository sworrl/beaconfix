#pragma once
#include "wifiscanner.h"
#include "estimator.h"
#include <QColor>
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
    double peerLat = 0, peerLon = 0, peerAcc = 0;   // a position another BeaconFix / the phone synced to us
    QString peerFrom;                 // which device ("" = none)
    bool   hasPeer() const { return !peerFrom.isEmpty() && peerAcc > 0; }
};

// Where we think an AP is, for the map
struct ApEstimate {
    enum Kind { None, Ring, Centroid, Wigle, Observed, Trilat, Peer } kind = None;   // Observed: one place we heard it · Trilat: our fit · Peer: synced from another device
    double lat = 0, lon = 0;          // Centroid / Wigle / Trilat / Peer: the estimate. Ring: our own position.
    double radiusM = 0;               // Ring: RSSI distance. Others: uncertainty.
    double bearingDeg = 0;            // Ring only: stable pseudo-bearing (bearing is unknown)
    int    vantage = 0;               // Centroid / Trilat: distinct places it was heard from
    Estimator::Fit fit;               // Trilat: the fit statistics
    // When a placement (WiGLE / Apple) and our own fit disagree by more than 3× their accuracy, both are reported
    bool   hasAlt = false; Kind altKind = None; double altLat = 0, altLon = 0, altAcc = 0;
    static const char *kindName(Kind k) { switch (k) { case Ring: return "ring"; case Centroid: return "centroid"; case Wigle: return "wigle"; case Observed: return "observed"; case Trilat: return "trilat"; case Peer: return "peer"; default: return "none"; } }
};

// Point-of-interest category (OpenStreetMap tags → icon, colour, label)
struct PoiCategory {
    QString key, label, icon;         // icon: emoji glyph
    QColor  color;
    QString group;                    // services | civic | kids  (menus group by this)
    bool    wide = false;             // civic: fetched out to the wide radius (police/fire are sparse in the country)
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
    QJsonObject emergencyJson() const;                            // nearest police / fire / ER / urgent care + the local number
    QList<Poi> poisMatching(const QStringList &catsOrGroups, double radiusKm) const;   // nearest first
    const QList<Poi> &pois() const { return m_pois; }
    QString poiNote() const { return m_poiNote; }
    bool    poisLoading() const { return m_poiBusy; }
    int     poiRadiusKm() const { return m_poiRadiusKm; }
    void    setPoiRadiusKm(int km);
    void    setTileBase(const QString &url) { m_tileBase = url; }
    void    refreshPois(bool force = false);
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
    void    RefreshPlaces() { refreshPois(true); }        // re-query OpenStreetMap for places around the fix
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
    int     Refit();                                     // full refit of every beacon with enough samples; returns valid fits
    QString Sync(const QString &url, const QString &token);   // one sync round with another BeaconFix; JSON result
    // Identity (docs/IDENTITY.md)
    QString IdentityJson() const;                        // public record + linkedIds + pending link requests
    bool    IdentityCreate(const QString &name);
    QString IdentityExport(const QString &passphrase);   // "BFID1:…" or "" on error
    bool    IdentityImport(const QString &textOrPath, const QString &passphrase);
    QString IdentityLinkPayload() const;                 // BFLNK1:… (what our link QR carries)
    QString IdentityAcceptLink(const QString &statementJson);   // completed statement JSON, or {"error":…}
    bool    IdentityForget();
    void    IdentityReload();                            // re-read the file (after a CLI change)
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
    void deviceApproved(const QString &name);

private:
    void tryStarlink();
    void onScan(const QList<AccessPoint> &aps);
    void onScanFinished(const QList<AccessPoint> &aps);   // dispatcher: diff → probe path or live path
    void liveScan();
    void startProbeScan();
    void diffScan(const QList<AccessPoint> &aps);
    void logEvent(BeaconEvent e);
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
    void loadSyncPeers();
    void saveSyncPeers() const;
    void syncStep(int peerIndex);
    bool matchesIgnore(const AccessPoint &ap) const;
    void queueWigle();
    void pumpWigle();
    void noteSightings(const QList<AccessPoint> &aps, const Fix &at);
    void queryOverpass(double lat, double lon, int radiusM, int mirror);
    void loadPois();
    void savePois() const;
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
    OsIntegration *m_os = nullptr;
    QString m_countryCode;
    void loadFromDb();
    void migrateJsonToDb();
    bool tryInternal(const QList<AccessPoint> &usable);
    QHash<QString, int> apFlags() const;    // bit0 home, bit1 travelling, bit2 ignored, per known BSSID
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
    QHash<QString, ApRecord> m_apRecords;
    QSet<QString> m_travelling, m_notTravelling;
    QStringList m_wigleQueue;
    bool m_wigleBusy = false;
    QList<Poi> m_pois;
    double m_poiLat = 0, m_poiLon = 0; int m_poiRadiusM = 0;
    QDateTime m_poiTime, m_poiTried;
    QString m_poiNote;
    bool m_poiBusy = false;
    int  m_poiRadiusKm = 6;
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
