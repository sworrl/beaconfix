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

    // D-Bus property readers
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
    bool    busy() const { return m_busy; }
    bool    geocodePending() const { return m_geocodePending; }
    const QList<AccessPoint> &accessPoints() const { return m_aps; }
    const QList<Fix> &history() const { return m_history; }
    QString wifiInterface() const { return m_scanner.interfaceName(); }

    // Per-AP classification for the UI: used | active | ignored | travelling | nomap
    QString apStatus(const AccessPoint &ap) const;
    bool    isTravelling(const QString &bssid) const;

    // Settings
    int         moveThresholdM() const { return m_moveThresholdM; }
    bool        useStarlink()    const { return m_useStarlink; }
    QString     starlinkHost()   const { return m_starlinkHost; }
    bool        useIp()          const { return m_useIp; }
    bool        ignoreActiveAp() const { return m_ignoreActive; }
    QStringList ignorePatterns() const { return m_ignore; }
    void setMoveThresholdM(int m);
    void setUseStarlink(bool b);
    void setStarlinkHost(const QString &h);
    void setUseIp(bool b);
    void setIgnoreActiveAp(bool b);
    void setIgnorePatterns(const QStringList &l);
    void addIgnorePattern(const QString &p);
    void setTravelling(const QString &bssid, bool travelling);

    void    start();
    bool    exportGpx(const QString &path, QString *error) const;
    static QString stateDir();

public slots:
    // Exported on D-Bus
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
    void noteApCells(const QList<AccessPoint> &aps, double lat, double lon);
    void saveApCells() const;
    bool matchesIgnore(const AccessPoint &ap) const;
    static double distanceM(double lat1, double lon1, double lat2, double lon2);

    bool m_standalone;
    WifiScanner m_scanner;
    QNetworkAccessManager m_nam;
    QTimer m_timer;
    bool   m_busy = false;
    bool   m_geocodePending = false;
    Fix    m_fix;          // accepted fix (what everyone reads)
    Fix    m_last;         // result of the most recent successful probe, movement or not
    QString m_lastError;
    QString m_starlinkError;
    QList<AccessPoint> m_aps;
    QList<Fix> m_history;
    QHash<QString, QSet<QString>> m_apCells;   // bssid → ~5 km cells it was seen in
    QSet<QString> m_travelling;                // manually flagged
    QSet<QString> m_notTravelling;             // manually cleared

    int m_intervalMin = 15;
    int m_moveThresholdM = 250;
    bool m_useStarlink = true;
    QString m_starlinkHost = QStringLiteral("192.168.100.1");
    bool m_useIp = true;
    bool m_ignoreActive = true;
    QStringList m_ignore;
};
