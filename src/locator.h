#pragma once
#include "wifiscanner.h"
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

struct Fix {
    bool      valid = false;
    double    lat = 0, lon = 0;
    double    accuracy = -1;      // metres (-1 unknown)
    QString   source;             // starlink | wifi | ip
    QString   place;
    QDateTime time;
    int       apCount = 0;        // APs seen
    int       apUsed  = 0;        // APs sent to BeaconDB
    QJsonObject toJson() const;
    static Fix fromJson(const QJsonObject &o);
};

struct ApObservation { double lat = 0, lon = 0; double acc = 0; int dbm = -100; QDateTime time; };

// Everything we've learned about one BSSID across stops
struct ApRecord {
    QString ssid;
    QSet<QString> cells;              // ~5 km cells it was seen in (travelling detection)
    QList<ApObservation> obs;         // where we were + how loud it was
    bool wigle = false;               // WiGLE knows where it is
    double wLat = 0, wLon = 0;
    QDateTime wigleChecked;
};

// Where we think an AP is, for the map
struct ApEstimate {
    enum Kind { None, Ring, Centroid, Wigle } kind = None;
    double lat = 0, lon = 0;          // Centroid / Wigle: the estimate. Ring: our own position.
    double radiusM = 0;               // Ring: RSSI distance. Others: uncertainty.
    double bearingDeg = 0;            // Ring only: stable pseudo-bearing (bearing is unknown)
};

struct Stats {
    int stops = 0;
    double distanceKm = 0;
    int beaconsTotal = 0;             // distinct BSSIDs ever seen
    int beaconsNow = 0, usedNow = 0, travellingNow = 0, locatedNow = 0;
    double bestAccuracy = -1;
    QString rank; int rankLevel = 0; int nextRankAt = 0;
};

// Runs the probe chain (Starlink dish GPS → BeaconDB Wi-Fi → IP) on a timer,
// keeps state + history on disk, and is exported on the session bus as
// org.sworrl.BeaconFix so the tray, the plasmoid and other apps share one fix.
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
    static double rssiDistanceM(int dbm);
    static double distanceM(double lat1, double lon1, double lat2, double lon2);

    // Settings
    int         moveThresholdM() const { return m_moveThresholdM; }
    bool        useStarlink()    const { return m_useStarlink; }
    QString     starlinkHost()   const { return m_starlinkHost; }
    bool        useIp()          const { return m_useIp; }
    bool        ignoreActiveAp() const { return m_ignoreActive; }
    QStringList ignorePatterns() const { return m_ignore; }
    QString     wigleToken()     const { return m_wigleToken; }
    void setMoveThresholdM(int m);
    void setUseStarlink(bool b);
    void setStarlinkHost(const QString &h);
    void setUseIp(bool b);
    void setIgnoreActiveAp(bool b);
    void setIgnorePatterns(const QStringList &l);
    void addIgnorePattern(const QString &p);
    void setTravelling(const QString &bssid, bool travelling);
    void setWigleToken(const QString &t);

    void    start();
    bool    exportGpx(const QString &path, QString *error) const;
    static QString stateDir();

public slots:
    void    Refresh();
    void    ShowWindow();
    QString StateJson() const;

signals:
    void FixChanged();
    void showWindowRequested();
    void probeStarted();
    void probeFinished(bool ok, const QString &message);
    void scanUpdated();
    void statusMessage(const QString &message);

private:
    void tryStarlink();
    void onScan(const QList<AccessPoint> &aps);
    void queryBeaconDb(const QList<AccessPoint> &usable);
    void tryIp(const QString &why);
    void accept(Fix cand, const QString &ipCity = QString());
    void finish(bool ok, const QString &message);
    void reverseGeocode(double lat, double lon);
    void loadState();
    void saveState() const;
    void appendHistory(const Fix &f);
    void noteObservations(const QList<AccessPoint> &aps, const Fix &at);
    void saveApRecords() const;
    bool matchesIgnore(const AccessPoint &ap) const;
    void queueWigle();
    void pumpWigle();

    bool m_standalone;
    WifiScanner m_scanner;
    QNetworkAccessManager m_nam;
    QTimer m_timer;
    QTimer m_wigleTimer;
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

    int m_intervalMin = 15;
    int m_moveThresholdM = 250;
    bool m_useStarlink = true;
    QString m_starlinkHost = QStringLiteral("192.168.100.1");
    bool m_useIp = true;
    bool m_ignoreActive = true;
    QStringList m_ignore;
    QString m_wigleToken;
};
