#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <functional>

class Locator;

// Keeps the desktop in step with where the machine is (each part opt-in, Settings → System):
//  · time zone   — the fix → IANA zone (timeapi.io, cached per ~5 km cell in the map database;
//                  offline fallback: nearest zone of the same country from tzdata's zone1970.tab)
//                  → org.freedesktop.timedate1.SetTimezone over the system bus (polkit; a shipped
//                  rules file lets active local users in sudo/wheel do it without a prompt)
//  · GeoClue     — /etc/geolocation (GeoClue ≥ 2.6 static source) through the root helper
//                  beaconfix-osd (pkexec, its own polkit action), so Night Light, weather apps and
//                  browsers with location permission all get the fix
//  · Night Light — KWin's [NightColor] Mode=Location with the fix's coordinates
//  · locale hints — country, region, units, emergency number, dialling code (suggest only)
class OsIntegration : public QObject {
    Q_OBJECT
public:
    struct LocaleHints {
        QString country, countryCode, region, units, emergency, dialing, timezone;
        QJsonObject toJson() const;
    };

    explicit OsIntegration(Locator *loc, QObject *parent = nullptr);

    bool timeZoneEnabled()   const { return m_tz; }
    bool geoclueEnabled()    const { return m_geoclue; }
    bool nightLightEnabled() const { return m_nightLight; }
    bool localeEnabled()     const { return m_locale; }
    void setTimeZoneEnabled(bool on);
    void setGeoclueEnabled(bool on);
    void setNightLightEnabled(bool on);
    void setLocaleEnabled(bool on);

    LocaleHints locale() const;                                  // for the current fix
    QString     systemTimeZone() const;                          // what the OS is on now
    QString     lastZone() const { return m_zone; }              // resolved for the fix ("" if not yet)
    QString     lastZoneSource() const { return m_zoneSource; }
    QString     lastReport() const { return m_lastReport; }
    QJsonObject status() const;

    // Resolve the zone for a position: cache → timeapi.io → tzdata fallback. `done` gets (zone, source).
    void resolveTimeZone(double lat, double lon, const QString &countryCode, std::function<void(QString, QString)> done);
    static QString zoneFromTab(double lat, double lon, const QString &countryCode, double *distKm = nullptr);
    static QString helperPath();                                 // beaconfix-osd, "" if not installed

    QJsonObject apply(bool dryRun, bool force = false);          // run every enabled step now; JSON report
    static QString emergencyFor(const QString &cc);
    static QString dialingFor(const QString &cc);
    static QString unitsFor(const QString &cc);

public slots:
    void onFixChanged();                                         // throttled automatic application

signals:
    void applied(const QString &what);
    void changed();

private:
    QJsonObject applyTimeZone(const QString &zone, bool dryRun);
    QJsonObject applyGeoclue(bool dryRun, bool force);
    QJsonObject applyNightLight(bool dryRun, bool force);
    bool setSystemTimeZone(const QString &zone, QString *error);

    Locator *m_loc;
    QNetworkAccessManager m_nam;
    bool m_tz = true, m_geoclue = true, m_nightLight = true, m_locale = false;
    QString m_zone, m_zoneSource, m_lastReport;
    QDateTime m_lastTzChange, m_lastResolve;
    double m_resolvedLat = 0, m_resolvedLon = 0;
    double m_gcLat = 0, m_gcLon = 0; bool m_gcWritten = false;
    double m_nlLat = 0, m_nlLon = 0; bool m_nlWritten = false;
    bool m_resolving = false;
};
