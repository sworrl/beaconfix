#include "osintegration.h"
#include "locator.h"
#include "mapdb.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimeZone>
#include <QUrl>
#include <QtMath>
#include <cmath>

static const char *USER_AGENT_OS = "BeaconFix/" BEACONFIX_VERSION " (KDE desktop locator; +https://github.com/sworrl/beaconfix)";

QJsonObject OsIntegration::LocaleHints::toJson() const
{
    return QJsonObject{{"country", country}, {"countryCode", countryCode}, {"region", region}, {"units", units},
                       {"emergencyNumber", emergency}, {"dialingCode", dialing}, {"timezone", timezone}};
}

OsIntegration::OsIntegration(Locator *loc, QObject *parent) : QObject(parent), m_loc(loc)
{
    QSettings s;
    m_tz         = s.value("osTimeZone", true).toBool();
    m_geoclue    = s.value("osGeoclue", true).toBool();
    m_nightLight = s.value("osNightLight", true).toBool();
    m_locale     = s.value("osLocale", false).toBool();
    m_zone       = s.value("osLastZone").toString();
    m_zoneSource = s.value("osLastZoneSource").toString();
    m_lastTzChange = s.value("osLastTzChange").toDateTime();
    m_gcLat = s.value("osGeoclueLat", 0.0).toDouble(); m_gcLon = s.value("osGeoclueLon", 0.0).toDouble(); m_gcWritten = s.contains("osGeoclueLat");
    m_nlLat = s.value("osNightLat", 0.0).toDouble(); m_nlLon = s.value("osNightLon", 0.0).toDouble(); m_nlWritten = s.contains("osNightLat");
}

void OsIntegration::setTimeZoneEnabled(bool on)   { m_tz = on; QSettings().setValue("osTimeZone", on); emit changed(); if (on) onFixChanged(); }
void OsIntegration::setGeoclueEnabled(bool on)    { m_geoclue = on; QSettings().setValue("osGeoclue", on); emit changed(); if (on) onFixChanged(); }
void OsIntegration::setNightLightEnabled(bool on) { m_nightLight = on; QSettings().setValue("osNightLight", on); emit changed(); if (on) onFixChanged(); }
void OsIntegration::setLocaleEnabled(bool on)     { m_locale = on; QSettings().setValue("osLocale", on); emit changed(); }

// ── locale hints ──────────────────────────────────────────────────────────────
QString OsIntegration::emergencyFor(const QString &ccIn)
{
    const QString cc = ccIn.toLower();
    static const struct { const char *cc; const char *n; } t[] = {
        {"us", "911"}, {"ca", "911"}, {"mx", "911"}, {"gb", "999 / 112"}, {"ie", "112 / 999"}, {"au", "000"}, {"nz", "111"},
        {"in", "112"}, {"jp", "110 police / 119 fire+ambulance"}, {"kr", "112 police / 119 fire+ambulance"}, {"cn", "110 police / 120 ambulance / 119 fire"},
        {"br", "190 police / 192 ambulance / 193 fire"}, {"ar", "911"}, {"cl", "131 ambulance / 132 fire / 133 police"}, {"co", "123"}, {"pe", "105"},
        {"za", "10111 police / 10177 ambulance"}, {"ru", "112"}, {"ua", "112"}, {"tr", "112"}, {"il", "100 police / 101 ambulance / 102 fire"},
        {"ae", "999"}, {"sa", "911"}, {"eg", "122 police / 123 ambulance"}, {"ng", "112"}, {"ke", "999 / 112"}, {"th", "191 police / 1669 ambulance"},
        {"vn", "113 police / 115 ambulance / 114 fire"}, {"ph", "911"}, {"id", "112"}, {"my", "999"}, {"sg", "999 police / 995 fire+ambulance"},
        {"hk", "999"}, {"tw", "110 police / 119 fire+ambulance"}, {"cr", "911"}, {"pa", "911"}, {"do", "911"}, {"jm", "119 police / 110 fire+ambulance"},
        {"is", "112"}, {"no", "112 police / 113 ambulance / 110 fire"}, {"ch", "112 / 117 police / 144 ambulance / 118 fire"},
    };
    for (const auto &e : t) if (cc == QLatin1String(e.cc)) return QString::fromLatin1(e.n);
    // The EU, EEA, most of Europe and much of the world route 112
    return QStringLiteral("112");
}

QString OsIntegration::dialingFor(const QString &ccIn)
{
    const QString cc = ccIn.toLower();
    static const struct { const char *cc; const char *n; } t[] = {
        {"us", "+1"}, {"ca", "+1"}, {"mx", "+52"}, {"gb", "+44"}, {"ie", "+353"}, {"fr", "+33"}, {"de", "+49"}, {"es", "+34"}, {"it", "+39"}, {"pt", "+351"},
        {"nl", "+31"}, {"be", "+32"}, {"ch", "+41"}, {"at", "+43"}, {"dk", "+45"}, {"se", "+46"}, {"no", "+47"}, {"fi", "+358"}, {"is", "+354"}, {"pl", "+48"},
        {"cz", "+420"}, {"sk", "+421"}, {"hu", "+36"}, {"ro", "+40"}, {"gr", "+30"}, {"tr", "+90"}, {"ru", "+7"}, {"ua", "+380"}, {"il", "+972"}, {"ae", "+971"},
        {"sa", "+966"}, {"eg", "+20"}, {"za", "+27"}, {"ng", "+234"}, {"ke", "+254"}, {"in", "+91"}, {"pk", "+92"}, {"cn", "+86"}, {"jp", "+81"}, {"kr", "+82"},
        {"tw", "+886"}, {"hk", "+852"}, {"sg", "+65"}, {"my", "+60"}, {"th", "+66"}, {"vn", "+84"}, {"ph", "+63"}, {"id", "+62"}, {"au", "+61"}, {"nz", "+64"},
        {"br", "+55"}, {"ar", "+54"}, {"cl", "+56"}, {"co", "+57"}, {"pe", "+51"}, {"cr", "+506"}, {"pa", "+507"}, {"do", "+1"}, {"jm", "+1"},
    };
    for (const auto &e : t) if (cc == QLatin1String(e.cc)) return QString::fromLatin1(e.n);
    return {};
}

QString OsIntegration::unitsFor(const QString &ccIn)
{
    const QString cc = ccIn.toLower();
    return (cc == QLatin1String("us") || cc == QLatin1String("lr") || cc == QLatin1String("mm")) ? QStringLiteral("imperial") : QStringLiteral("metric");
}

OsIntegration::LocaleHints OsIntegration::locale() const
{
    LocaleHints h;
    const Fix &f = m_loc->fix();
    h.country = f.country; h.region = f.region; h.countryCode = m_loc->countryCode();
    if (h.countryCode.isEmpty() && !f.country.isEmpty()) {
        // a few names Nominatim returns, for when the code is not there yet
        static const struct { const char *name; const char *cc; } t[] = {{"United States", "us"}, {"Canada", "ca"}, {"Mexico", "mx"}, {"United Kingdom", "gb"},
                                                                          {"Australia", "au"}, {"Germany", "de"}, {"France", "fr"}, {"Spain", "es"}, {"Italy", "it"}};
        for (const auto &e : t) if (f.country.startsWith(QLatin1String(e.name))) h.countryCode = QString::fromLatin1(e.cc);
    }
    h.units = unitsFor(h.countryCode); h.emergency = emergencyFor(h.countryCode); h.dialing = dialingFor(h.countryCode);
    h.timezone = m_zone.isEmpty() ? systemTimeZone() : m_zone;
    return h;
}

// ── time zone ─────────────────────────────────────────────────────────────────
QString OsIntegration::systemTimeZone() const
{
    QDBusInterface td(QStringLiteral("org.freedesktop.timedate1"), QStringLiteral("/org/freedesktop/timedate1"),
                      QStringLiteral("org.freedesktop.timedate1"), QDBusConnection::systemBus());
    if (td.isValid()) { const QVariant v = td.property("Timezone"); if (v.isValid() && !v.toString().isEmpty()) return v.toString(); }
    const QString link = QFile::symLinkTarget(QStringLiteral("/etc/localtime"));
    const int i = link.indexOf(QLatin1String("zoneinfo/"));
    if (i >= 0) return link.mid(i + 9);
    return QString::fromUtf8(QTimeZone::systemTimeZoneId());
}

// tzdata's zone1970.tab: "CC[,CC]<tab>±DDMM±DDDMM[SS]<tab>Zone<tab>comment"
QString OsIntegration::zoneFromTab(double lat, double lon, const QString &countryCode, double *distKm)
{
    QFile f(QStringLiteral("/usr/share/zoneinfo/zone1970.tab"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QString cc = countryCode.toUpper();
    QString bestAny, bestCc; double dAny = 1e12, dCc = 1e12;
    auto parseCoord = [](const QString &s, double *la, double *lo) -> bool {
        // ±DDMM±DDDMM or ±DDMMSS±DDDMMSS
        int split = -1;
        for (int i = 1; i < s.size(); ++i) if (s[i] == QLatin1Char('+') || s[i] == QLatin1Char('-')) { split = i; break; }
        if (split < 0) return false;
        const QString a = s.left(split), b = s.mid(split);
        auto conv = [](const QString &x, int degDigits) -> double {
            const double sign = x.startsWith(QLatin1Char('-')) ? -1 : 1;
            const QString d = x.mid(1);
            const double deg = d.left(degDigits).toDouble(), min = d.mid(degDigits, 2).toDouble(), sec = d.size() > degDigits + 2 ? d.mid(degDigits + 2, 2).toDouble() : 0;
            return sign * (deg + min / 60.0 + sec / 3600.0);
        };
        *la = conv(a, 2); *lo = conv(b, 3);
        return true;
    };
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() < 3) continue;
        double la = 0, lo = 0;
        if (!parseCoord(parts[1], &la, &lo)) continue;
        const double d = Locator::distanceM(lat, lon, la, lo) / 1000.0;
        if (d < dAny) { dAny = d; bestAny = parts[2]; }
        if (!cc.isEmpty() && parts[0].split(QLatin1Char(',')).contains(cc) && d < dCc) { dCc = d; bestCc = parts[2]; }
    }
    if (!bestCc.isEmpty()) { if (distKm) *distKm = dCc; return bestCc; }
    if (distKm) *distKm = dAny;
    return bestAny;
}

void OsIntegration::resolveTimeZone(double lat, double lon, const QString &countryCode, std::function<void(QString, QString)> done)
{
    const QString cell = QStringLiteral("tz:%1,%2").arg(qRound(lat * 20) / 20.0, 0, 'f', 2).arg(qRound(lon * 20) / 20.0, 0, 'f', 2);
    MapDb *db = m_loc->mapDb();
    if (db && db->isOpen()) {
        const QString cached = db->kv(cell);                    // "zone|epochSecs"
        if (!cached.isEmpty()) {
            const QString zone = cached.section(QLatin1Char('|'), 0, 0);
            const qint64 when = cached.section(QLatin1Char('|'), 1, 1).toLongLong();
            if (!zone.isEmpty() && QDateTime::currentSecsSinceEpoch() - when < 86400) { done(zone, QStringLiteral("cache")); return; }
        }
    }
    QNetworkRequest req(QUrl(QStringLiteral("https://timeapi.io/api/TimeZone/coordinate?latitude=%1&longitude=%2").arg(lat, 0, 'f', 5).arg(lon, 0, 'f', 5)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(USER_AGENT_OS));
    req.setTransferTimeout(8000);
    QNetworkReply *rep = m_nam.get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep, lat, lon, countryCode, cell, done] {
        rep->deleteLater();
        QString zone;
        if (rep->error() == QNetworkReply::NoError) zone = QJsonDocument::fromJson(rep->readAll()).object()["timeZone"].toString();
        if (!zone.isEmpty() && QTimeZone(zone.toUtf8()).isValid()) {
            if (MapDb *db = m_loc->mapDb(); db && db->isOpen() && !db->readOnly()) db->setKv(cell, zone + QLatin1Char('|') + QString::number(QDateTime::currentSecsSinceEpoch()));
            done(zone, QStringLiteral("timeapi.io"));
            return;
        }
        double km = 0;
        const QString fallback = zoneFromTab(lat, lon, countryCode, &km);
        done(fallback, fallback.isEmpty() ? QString() : QStringLiteral("zone1970.tab (nearest, %1 km)").arg(qRound(km)));
    });
}

bool OsIntegration::setSystemTimeZone(const QString &zone, QString *error)
{
    QDBusInterface td(QStringLiteral("org.freedesktop.timedate1"), QStringLiteral("/org/freedesktop/timedate1"),
                      QStringLiteral("org.freedesktop.timedate1"), QDBusConnection::systemBus());
    if (!td.isValid()) { if (error) *error = QStringLiteral("timedate1 not available on the system bus"); return false; }
    td.setTimeout(30000);                                       // polkit may put up a prompt
    QDBusReply<void> r = td.call(QStringLiteral("SetTimezone"), zone, true);
    if (!r.isValid()) { if (error) *error = r.error().message(); return false; }
    return true;
}

QJsonObject OsIntegration::applyTimeZone(const QString &zone, bool dryRun)
{
    QJsonObject o{{"step", "timezone"}, {"zone", zone}};
    const QString cur = systemTimeZone();
    o["system"] = cur;
    if (zone.isEmpty()) { o["result"] = "no zone resolved"; return o; }
    if (zone == cur) { o["result"] = "already set"; return o; }
    if (!dryRun && m_lastTzChange.isValid() && m_lastTzChange.secsTo(QDateTime::currentDateTime()) < 600) { o["result"] = "throttled (10 min)"; return o; }
    if (dryRun) { o["result"] = QStringLiteral("would set %1 → %2").arg(cur, zone); return o; }
    QString err;
    if (!setSystemTimeZone(zone, &err)) { o["result"] = QStringLiteral("failed: %1").arg(err); return o; }
    m_lastTzChange = QDateTime::currentDateTime(); QSettings().setValue("osLastTzChange", m_lastTzChange);
    o["result"] = QStringLiteral("set %1 → %2").arg(cur, zone);
    emit applied(QStringLiteral("Time zone → %1").arg(zone));
    return o;
}

// ── GeoClue static source ─────────────────────────────────────────────────────
QString OsIntegration::helperPath()
{
    const QStringList cands{QCoreApplication::applicationDirPath() + QStringLiteral("/../libexec/beaconfix-osd"),
                            QStringLiteral("/usr/local/libexec/beaconfix-osd"), QStringLiteral("/usr/libexec/beaconfix-osd"),
                            QStringLiteral("/usr/lib/beaconfix/beaconfix-osd")};
    for (const QString &c : cands) { QFileInfo fi(c); if (fi.isExecutable()) return fi.canonicalFilePath(); }
    return {};
}

QJsonObject OsIntegration::applyGeoclue(bool dryRun, bool force)
{
    QJsonObject o{{"step", "geoclue"}};
    const Fix &f = m_loc->fix();
    if (!f.valid || !f.precise()) { o["result"] = "no precise fix"; return o; }
    const double moved = m_gcWritten ? Locator::distanceM(m_gcLat, m_gcLon, f.lat, f.lon) : 1e9;
    if (!force && moved < 250) { o["result"] = "unchanged (< 250 m)"; return o; }
    const QString helper = helperPath();
    if (helper.isEmpty()) { o["result"] = "helper beaconfix-osd not installed (run install.sh with polkit, or the .deb)"; return o; }
    o["file"] = "/etc/geolocation";
    if (dryRun) { o["result"] = QStringLiteral("would write %1 %2 ±%3 m").arg(f.lat, 0, 'f', 5).arg(f.lon, 0, 'f', 5).arg(qRound(f.accuracy)); return o; }
    QProcess p;
    p.start(QStringLiteral("pkexec"), {helper, QStringLiteral("geolocation"), QString::number(f.lat, 'f', 6), QString::number(f.lon, 'f', 6),
                                       QString::number(f.hasElevation() ? f.elevation : 0.0, 'f', 1), QString::number(qMax(1.0, f.accuracy), 'f', 0)});
    if (!p.waitForFinished(30000) || p.exitCode() != 0) { o["result"] = QStringLiteral("failed: %1").arg(QString::fromUtf8(p.readAllStandardError()).trimmed()); return o; }
    m_gcLat = f.lat; m_gcLon = f.lon; m_gcWritten = true;
    QSettings s; s.setValue("osGeoclueLat", f.lat); s.setValue("osGeoclueLon", f.lon);
    o["result"] = "written";
    emit applied(QStringLiteral("GeoClue position updated"));
    return o;
}

// ── KWin Night Light ──────────────────────────────────────────────────────────
QJsonObject OsIntegration::applyNightLight(bool dryRun, bool force)
{
    QJsonObject o{{"step", "nightlight"}};
    const Fix &f = m_loc->fix();
    if (!f.valid || !f.precise()) { o["result"] = "no precise fix"; return o; }
    const double moved = m_nlWritten ? Locator::distanceM(m_nlLat, m_nlLon, f.lat, f.lon) : 1e9;
    if (!force && moved < 25000) { o["result"] = "unchanged (< 25 km)"; return o; }
    if (dryRun) { o["result"] = QStringLiteral("would set kwinrc [NightColor] Mode=Location %1, %2").arg(f.lat, 0, 'f', 4).arg(f.lon, 0, 'f', 4); return o; }
    const QString kw = QStandardPaths::findExecutable(QStringLiteral("kwriteconfig6"));
    if (kw.isEmpty()) { o["result"] = "kwriteconfig6 not found (not a Plasma session?)"; return o; }
    auto set = [&](const QString &key, const QString &val) {
        // "--" ends option parsing: a negative longitude would otherwise be read as a flag
        QProcess p; p.start(kw, {QStringLiteral("--file"), QStringLiteral("kwinrc"), QStringLiteral("--group"), QStringLiteral("NightColor"), QStringLiteral("--key"), key, QStringLiteral("--"), val});
        return p.waitForFinished(5000) && p.exitCode() == 0;
    };
    if (!set(QStringLiteral("Mode"), QStringLiteral("Location")) || !set(QStringLiteral("LatitudeFixed"), QString::number(f.lat, 'f', 4)) || !set(QStringLiteral("LongitudeFixed"), QString::number(f.lon, 'f', 4))) {
        o["result"] = "kwriteconfig6 failed"; return o;
    }
    QDBusInterface kwin(QStringLiteral("org.kde.KWin"), QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QDBusConnection::sessionBus());
    if (kwin.isValid()) kwin.call(QStringLiteral("reconfigure"));
    m_nlLat = f.lat; m_nlLon = f.lon; m_nlWritten = true;
    QSettings s; s.setValue("osNightLat", f.lat); s.setValue("osNightLon", f.lon);
    o["result"] = "set";
    emit applied(QStringLiteral("Night Light location updated"));
    return o;
}

// ── driver ────────────────────────────────────────────────────────────────────
QJsonObject OsIntegration::apply(bool dryRun, bool force)
{
    QJsonObject rep{{"dryRun", dryRun}, {"ts", QDateTime::currentDateTime().toString(Qt::ISODate)}};
    const Fix &f = m_loc->fix();
    QJsonArray steps;
    if (!f.valid) { rep["error"] = "no fix"; rep["steps"] = steps; return rep; }
    if (m_tz) {
        if (!f.precise()) steps.append(QJsonObject{{"step", "timezone"}, {"result", "IP-only fix: never changes the zone"}});
        else if (!m_zone.isEmpty() && Locator::distanceM(m_resolvedLat, m_resolvedLon, f.lat, f.lon) < 5000) steps.append(applyTimeZone(m_zone, dryRun));
        else steps.append(QJsonObject{{"step", "timezone"}, {"result", "zone not resolved yet for this position"}});
    } else steps.append(QJsonObject{{"step", "timezone"}, {"result", "disabled"}});
    steps.append(m_geoclue ? applyGeoclue(dryRun, force) : QJsonObject{{"step", "geoclue"}, {"result", "disabled"}});
    steps.append(m_nightLight ? applyNightLight(dryRun, force) : QJsonObject{{"step", "nightlight"}, {"result", "disabled"}});
    steps.append(QJsonObject{{"step", "locale"}, {"result", m_locale ? "hints exposed" : "disabled (hints still available in stats.locale)"}, {"hints", locale().toJson()}});
    rep["steps"] = steps;
    m_lastReport = QString::fromUtf8(QJsonDocument(rep).toJson(QJsonDocument::Compact));
    emit changed();
    return rep;
}

void OsIntegration::onFixChanged()
{
    const Fix &f = m_loc->fix();
    if (!f.valid || !f.precise()) return;
    if (!m_tz && !m_geoclue && !m_nightLight) return;
    // Resolve the zone when we moved ≥ 5 km from the last resolution (or never resolved), then apply everything
    const bool need = m_zone.isEmpty() || Locator::distanceM(m_resolvedLat, m_resolvedLon, f.lat, f.lon) >= 5000
                      || (m_lastResolve.isValid() && m_lastResolve.secsTo(QDateTime::currentDateTime()) > 6 * 3600);
    if (m_tz && need && !m_resolving) {
        m_resolving = true;
        const double lat = f.lat, lon = f.lon;
        resolveTimeZone(lat, lon, m_loc->countryCode(), [this, lat, lon](const QString &zone, const QString &source) {
            m_resolving = false;
            if (!zone.isEmpty()) {
                m_zone = zone; m_zoneSource = source; m_resolvedLat = lat; m_resolvedLon = lon; m_lastResolve = QDateTime::currentDateTime();
                QSettings s; s.setValue("osLastZone", zone); s.setValue("osLastZoneSource", source);
            }
            apply(false, false);
        });
        return;
    }
    apply(false, false);
}

QJsonObject OsIntegration::status() const
{
    return QJsonObject{{"timeZone", m_tz}, {"geoclue", m_geoclue}, {"nightLight", m_nightLight}, {"locale", m_locale},
                       {"zone", m_zone}, {"zoneSource", m_zoneSource}, {"systemZone", systemTimeZone()},
                       {"helper", helperPath()}, {"lastTzChange", m_lastTzChange.isValid() ? m_lastTzChange.toString(Qt::ISODate) : QString()},
                       {"geoclueWritten", m_gcWritten}, {"nightLightWritten", m_nlWritten}};
}
