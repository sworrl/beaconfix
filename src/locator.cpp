#include "locator.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrlQuery>
#include <QtMath>

static const char *USER_AGENT = "BeaconFix/" BEACONFIX_VERSION " (KDE desktop locator; +https://github.com/sworrl/beaconfix)";

// ── Fix ───────────────────────────────────────────────────────────────────────
QJsonObject Fix::toJson() const
{
    QJsonObject o;
    o["valid"] = valid; o["lat"] = lat; o["lon"] = lon; o["accuracy"] = accuracy;
    o["source"] = source; o["place"] = place; o["time"] = time.toString(Qt::ISODate);
    o["apCount"] = apCount; o["apUsed"] = apUsed;
    return o;
}
Fix Fix::fromJson(const QJsonObject &o)
{
    Fix f;
    f.valid = o["valid"].toBool(); f.lat = o["lat"].toDouble(); f.lon = o["lon"].toDouble();
    f.accuracy = o["accuracy"].toDouble(-1); f.source = o["source"].toString();
    f.place = o["place"].toString(); f.time = QDateTime::fromString(o["time"].toString(), Qt::ISODate);
    f.apCount = o["apCount"].toInt(); f.apUsed = o["apUsed"].toInt();
    return f;
}

// ── Locator ───────────────────────────────────────────────────────────────────
Locator::Locator(bool standalone, QObject *parent) : QObject(parent), m_standalone(standalone)
{
    QSettings s;
    m_intervalMin    = s.value("intervalMinutes", 15).toInt();
    m_moveThresholdM = s.value("moveThresholdM", 250).toInt();
    m_useStarlink    = s.value("useStarlink", true).toBool();
    m_starlinkHost   = s.value("starlinkHost", "192.168.100.1").toString();
    m_useIp          = s.value("useIp", true).toBool();
    m_ignoreActive   = s.value("ignoreActiveAp", true).toBool();
    m_ignore         = s.value("ignorePatterns").toStringList();

    connect(&m_scanner, &WifiScanner::scanFinished, this, &Locator::onScan);
    connect(&m_scanner, &WifiScanner::scanFailed, this, [this](const QString &m) {
        m_aps.clear(); emit scanUpdated();
        tryIp(QStringLiteral("Wi-Fi scan failed: ") + m);
    });
    m_timer.setInterval(m_intervalMin * 60 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &Locator::Refresh);
    loadState();
}

QString Locator::stateDir()
{
    QString d = qEnvironmentVariable("XDG_STATE_HOME");
    if (d.isEmpty())
        d = QDir::homePath() + "/.local/state";
    d += "/beaconfix";
    QDir().mkpath(d);
    return d;
}

void Locator::start()
{
    m_timer.start();
    Refresh();
}

void Locator::setIntervalMinutes(int m)
{
    m_intervalMin = qBound(1, m, 24 * 60);
    QSettings().setValue("intervalMinutes", m_intervalMin);
    m_timer.setInterval(m_intervalMin * 60 * 1000);
    if (m_timer.isActive()) m_timer.start();
}
void Locator::setMoveThresholdM(int m)      { m_moveThresholdM = qMax(0, m); QSettings().setValue("moveThresholdM", m_moveThresholdM); }
void Locator::setUseStarlink(bool b)        { m_useStarlink = b; QSettings().setValue("useStarlink", b); }
void Locator::setStarlinkHost(const QString &h) { m_starlinkHost = h.trimmed().isEmpty() ? QStringLiteral("192.168.100.1") : h.trimmed(); QSettings().setValue("starlinkHost", m_starlinkHost); }
void Locator::setUseIp(bool b)              { m_useIp = b; QSettings().setValue("useIp", b); }
void Locator::setIgnoreActiveAp(bool b)     { m_ignoreActive = b; QSettings().setValue("ignoreActiveAp", b); }
void Locator::setIgnorePatterns(const QStringList &l)
{
    m_ignore.clear();
    for (const QString &p : l) { const QString t = p.trimmed(); if (!t.isEmpty()) m_ignore << t; }
    QSettings().setValue("ignorePatterns", m_ignore);
    emit scanUpdated();
}
void Locator::addIgnorePattern(const QString &p) { QStringList l = m_ignore; l << p; setIgnorePatterns(l); }

void Locator::setTravelling(const QString &bssid, bool travelling)
{
    if (travelling) { m_travelling.insert(bssid); m_notTravelling.remove(bssid); }
    else            { m_travelling.remove(bssid); m_notTravelling.insert(bssid); }
    saveApCells();
    emit scanUpdated();
}

bool Locator::isTravelling(const QString &bssid) const
{
    if (m_notTravelling.contains(bssid)) return false;
    if (m_travelling.contains(bssid)) return true;
    return m_apCells.value(bssid).size() >= 2;   // seen at two stops ≥ ~5 km apart
}

bool Locator::matchesIgnore(const AccessPoint &ap) const
{
    for (const QString &pat : m_ignore) {
        const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pat), QRegularExpression::CaseInsensitiveOption);
        if (re.match(ap.bssid).hasMatch() || re.match(ap.ssid).hasMatch())
            return true;
    }
    return false;
}

QString Locator::apStatus(const AccessPoint &ap) const
{
    if (ap.ssid.endsWith(QLatin1String("_nomap")) || ap.ssid.contains(QLatin1String("_optout")))
        return QStringLiteral("nomap");
    if (matchesIgnore(ap))               return QStringLiteral("ignored");
    if (isTravelling(ap.bssid))          return QStringLiteral("travelling");
    if (ap.active && m_ignoreActive)     return QStringLiteral("active");
    return QStringLiteral("used");
}

// ── Probe chain ───────────────────────────────────────────────────────────────
void Locator::Refresh()
{
    if (m_busy) return;
    m_busy = true;
    m_lastError.clear();
    m_starlinkError.clear();
    emit probeStarted();
    if (m_useStarlink) tryStarlink();
    else { emit statusMessage(QStringLiteral("Scanning Wi-Fi…")); m_scanner.scan(); }
}

void Locator::ShowWindow() { emit showWindowRequested(); }

QString Locator::StateJson() const
{
    QJsonObject o = m_fix.toJson();
    o["error"] = m_lastError;
    o["busy"] = m_busy;
    o["intervalMinutes"] = m_intervalMin;
    o["wifiInterface"] = m_scanner.interfaceName();
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void Locator::tryStarlink()
{
    QString grpcurl;
    for (const QString &dir : {QDir::homePath() + "/go/bin", QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/bin"), QDir::homePath() + "/.local/bin"}) {
        if (QFile::exists(dir + "/grpcurl")) { grpcurl = dir + "/grpcurl"; break; }
    }
    if (grpcurl.isEmpty()) grpcurl = QStandardPaths::findExecutable(QStringLiteral("grpcurl"));
    if (grpcurl.isEmpty()) {
        m_starlinkError = QStringLiteral("grpcurl not installed");
        emit statusMessage(QStringLiteral("Scanning Wi-Fi…"));
        m_scanner.scan();
        return;
    }
    emit statusMessage(QStringLiteral("Asking Starlink dish for GPS…"));
    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::finished, this, [this, proc](int, QProcess::ExitStatus) {
        const QByteArray out = proc->readAll();
        proc->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(out).object();
        QJsonObject lla = o["getLocation"].toObject()["lla"].toObject();
        if (lla.contains("lat") && lla.contains("lon")) {
            Fix f; f.valid = true; f.lat = lla["lat"].toDouble(); f.lon = lla["lon"].toDouble();
            f.accuracy = 10; f.source = QStringLiteral("starlink"); f.time = QDateTime::currentDateTime();
            accept(f);
            finish(true, QStringLiteral("Starlink dish GPS fix"));
            return;
        }
        static const QRegularExpression re(QStringLiteral("Message: (.*)|Failed to dial[^\\n]*|context deadline exceeded"));
        const auto m = re.match(QString::fromUtf8(out));
        m_starlinkError = m.hasMatch() ? m.captured(0).left(160).trimmed() : QString::fromUtf8(out.left(160)).simplified();
        emit statusMessage(QStringLiteral("Scanning Wi-Fi…"));
        m_scanner.scan();
    });
    proc->start(grpcurl, {QStringLiteral("-plaintext"), QStringLiteral("-max-time"), QStringLiteral("5"),
                          QStringLiteral("-d"), QStringLiteral("{\"get_location\":{}}"),
                          m_starlinkHost + ":9200", QStringLiteral("SpaceX.API.Device.Device/Handle")});
}

void Locator::onScan(const QList<AccessPoint> &aps)
{
    m_aps = aps;
    emit scanUpdated();
    QList<AccessPoint> usable;
    for (const AccessPoint &ap : aps)
        if (apStatus(ap) == QLatin1String("used")) usable << ap;
    if (usable.size() < 2) {
        tryIp(QStringLiteral("Only %1 usable access point(s); BeaconDB needs 2").arg(usable.size()));
        return;
    }
    queryBeaconDb(usable);
}

void Locator::queryBeaconDb(const QList<AccessPoint> &usable)
{
    emit statusMessage(QStringLiteral("Asking BeaconDB about %1 access points…").arg(usable.size()));
    QJsonArray arr;
    for (const AccessPoint &ap : usable) {
        QJsonObject o;
        o["macAddress"] = ap.bssid; o["signalStrength"] = ap.dbm; o["frequency"] = ap.frequency;
        arr.append(o);
    }
    QJsonObject body; body["wifiAccessPoints"] = arr;
    QNetworkRequest req(QUrl(QStringLiteral("https://api.beacondb.net/v1/geolocate")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(USER_AGENT));
    req.setTransferTimeout(15000);
    QNetworkReply *rep = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    const int used = usable.size(), seen = m_aps.size();
    connect(rep, &QNetworkReply::finished, this, [this, rep, used, seen] {
        rep->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        const QJsonObject loc = o["location"].toObject();
        if (rep->error() == QNetworkReply::NoError && loc.contains("lat")) {
            Fix f; f.valid = true; f.lat = loc["lat"].toDouble(); f.lon = loc["lng"].toDouble();
            f.accuracy = o["accuracy"].toDouble(-1); f.source = QStringLiteral("wifi");
            f.time = QDateTime::currentDateTime(); f.apCount = seen; f.apUsed = used;
            accept(f);
            finish(true, QStringLiteral("BeaconDB fix from %1 access points (±%2 m)").arg(used).arg(qRound(f.accuracy)));
            return;
        }
        QString why = o["error"].toObject()["message"].toString();
        if (why.isEmpty()) why = rep->error() == QNetworkReply::NoError ? QStringLiteral("no location in reply") : rep->errorString();
        tryIp(QStringLiteral("BeaconDB: ") + why);
    });
}

void Locator::tryIp(const QString &why)
{
    if (!m_useIp) { finish(false, why); return; }
    emit statusMessage(QStringLiteral("Falling back to IP geolocation…"));
    QNetworkRequest req(QUrl(QStringLiteral("http://ip-api.com/json/?fields=status,lat,lon,city,regionName,isp")));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(USER_AGENT));
    req.setTransferTimeout(10000);
    QNetworkReply *rep = m_nam.get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep, why] {
        rep->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        if (rep->error() == QNetworkReply::NoError && o["status"].toString() == QLatin1String("success")) {
            Fix f; f.valid = true; f.lat = o["lat"].toDouble(); f.lon = o["lon"].toDouble();
            f.accuracy = 50000; f.source = QStringLiteral("ip"); f.time = QDateTime::currentDateTime();
            f.apCount = m_aps.size();
            const QString city = o["city"].toString() + QStringLiteral(", ") + o["regionName"].toString();
            accept(f, city);
            finish(true, why + QStringLiteral(" — using IP geolocation (%1 via %2)").arg(city, o["isp"].toString()));
            return;
        }
        finish(false, why + QStringLiteral("; IP lookup failed: ") + (rep->error() == QNetworkReply::NoError ? QStringLiteral("no result") : rep->errorString()));
    });
}

void Locator::accept(Fix cand, const QString &ipCity)
{
    m_last = cand;
    if (cand.source == QLatin1String("wifi") && cand.accuracy >= 0 && cand.accuracy < 2000)
        noteApCells(m_aps, cand.lat, cand.lon);

    const bool moved = !m_fix.valid || m_fix.source != cand.source
                       || distanceM(m_fix.lat, m_fix.lon, cand.lat, cand.lon) > m_moveThresholdM;
    if (!moved) {
        m_fix.time = cand.time; m_fix.accuracy = cand.accuracy;
        m_fix.apCount = cand.apCount; m_fix.apUsed = cand.apUsed;
        saveState();
        emit FixChanged();
        return;
    }
    cand.place = !ipCity.isEmpty() ? ipCity : (m_fix.valid && distanceM(m_fix.lat, m_fix.lon, cand.lat, cand.lon) < 500 ? m_fix.place : QString());
    if (cand.place.isEmpty())
        cand.place = QStringLiteral("%1, %2").arg(cand.lat, 0, 'f', 4).arg(cand.lon, 0, 'f', 4);
    m_fix = cand;
    appendHistory(m_fix);
    saveState();
    emit FixChanged();
    if (ipCity.isEmpty())
        reverseGeocode(cand.lat, cand.lon);
}

void Locator::finish(bool ok, const QString &message)
{
    m_busy = false;
    if (!ok) { m_lastError = message; saveState(); }
    QString msg = message;
    if (!m_starlinkError.isEmpty() && m_fix.source != QLatin1String("starlink"))
        msg += QStringLiteral("\nStarlink: ") + m_starlinkError;
    emit statusMessage(msg);
    emit probeFinished(ok, msg);
}

void Locator::reverseGeocode(double lat, double lon)
{
    QUrl url(QStringLiteral("https://nominatim.openstreetmap.org/reverse"));
    QUrlQuery q;
    q.addQueryItem("format", "jsonv2"); q.addQueryItem("zoom", "14");
    q.addQueryItem("lat", QString::number(lat, 'f', 6)); q.addQueryItem("lon", QString::number(lon, 'f', 6));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(USER_AGENT));
    req.setTransferTimeout(10000);
    m_geocodePending = true;
    QNetworkReply *rep = m_nam.get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep, lat, lon] {
        rep->deleteLater();
        m_geocodePending = false;
        if (rep->error() != QNetworkReply::NoError) return;
        const QJsonObject d = QJsonDocument::fromJson(rep->readAll()).object();
        const QJsonObject a = d["address"].toObject();
        QString place;
        for (const char *k : {"neighbourhood", "suburb", "hamlet", "village", "town", "city"}) {
            const QString v = a[k].toString();
            if (!v.isEmpty()) { place = place.isEmpty() ? v : place + ", " + v; }
        }
        if (place.isEmpty()) place = a["county"].toString();
        if (place.isEmpty()) place = d["name"].toString();
        const QString state = a["state"].toString();
        if (!state.isEmpty()) place += (place.isEmpty() ? "" : ", ") + state;
        if (place.isEmpty()) return;
        // Only apply if we're still at that spot
        if (m_fix.valid && distanceM(m_fix.lat, m_fix.lon, lat, lon) < 50) {
            m_fix.place = place;
            if (!m_history.isEmpty()) m_history.last().place = place;
            saveState();
            emit FixChanged();
        }
    });
}

// ── Persistence ───────────────────────────────────────────────────────────────
void Locator::loadState()
{
    QFile f(stateDir() + "/state.json");
    if (f.open(QIODevice::ReadOnly)) {
        m_fix = Fix::fromJson(QJsonDocument::fromJson(f.readAll()).object());
        m_last = m_fix;
    }
    QFile h(stateDir() + "/history.jsonl");
    if (h.open(QIODevice::ReadOnly)) {
        while (!h.atEnd()) {
            const QByteArray line = h.readLine().trimmed();
            if (line.isEmpty()) continue;
            Fix fx = Fix::fromJson(QJsonDocument::fromJson(line).object());
            if (fx.valid) m_history.append(fx);
        }
    }
    QFile c(stateDir() + "/aps.json");
    if (c.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(c.readAll()).object();
        const QJsonObject cells = o["cells"].toObject();
        for (auto it = cells.begin(); it != cells.end(); ++it) {
            QSet<QString> s;
            for (const QJsonValue &v : it.value().toArray()) s.insert(v.toString());
            m_apCells.insert(it.key(), s);
        }
        for (const QJsonValue &v : o["travelling"].toArray()) m_travelling.insert(v.toString());
        for (const QJsonValue &v : o["notTravelling"].toArray()) m_notTravelling.insert(v.toString());
    }
}

void Locator::saveState() const
{
    QJsonObject o = m_fix.toJson();
    o["error"] = m_lastError;
    o["checked"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    QFile f(stateDir() + "/state.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n");
}

void Locator::appendHistory(const Fix &fx)
{
    m_history.append(fx);
    QFile h(stateDir() + "/history.jsonl");
    if (h.open(QIODevice::WriteOnly | QIODevice::Append))
        h.write(QJsonDocument(fx.toJson()).toJson(QJsonDocument::Compact) + "\n");
}

void Locator::noteApCells(const QList<AccessPoint> &aps, double lat, double lon)
{
    const QString cell = QStringLiteral("%1,%2").arg(qRound(lat * 20)).arg(qRound(lon * 20));
    bool changed = false;
    for (const AccessPoint &ap : aps) {
        QSet<QString> &s = m_apCells[ap.bssid];
        if (!s.contains(cell)) { s.insert(cell); changed = true; }
    }
    if (changed) saveApCells();
}

void Locator::saveApCells() const
{
    QJsonObject cells;
    for (auto it = m_apCells.begin(); it != m_apCells.end(); ++it) {
        QJsonArray a; for (const QString &c : it.value()) a.append(c);
        cells[it.key()] = a;
    }
    QJsonArray t, nt;
    for (const QString &b : m_travelling) t.append(b);
    for (const QString &b : m_notTravelling) nt.append(b);
    QJsonObject o; o["cells"] = cells; o["travelling"] = t; o["notTravelling"] = nt;
    QFile f(stateDir() + "/aps.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

bool Locator::exportGpx(const QString &path, QString *error) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return false;
    }
    QTextStream ts(&f);
    ts << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<gpx version=\"1.1\" creator=\"BeaconFix\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n";
    for (const Fix &fx : m_history) {
        ts << "  <wpt lat=\"" << QString::number(fx.lat, 'f', 6) << "\" lon=\"" << QString::number(fx.lon, 'f', 6) << "\">\n"
           << "    <time>" << fx.time.toUTC().toString(Qt::ISODate) << "</time>\n"
           << "    <name>" << fx.place.toHtmlEscaped() << "</name>\n"
           << "    <desc>" << fx.source << (fx.accuracy >= 0 ? QStringLiteral(" ±%1 m").arg(qRound(fx.accuracy)) : QString()) << "</desc>\n"
           << "  </wpt>\n";
    }
    ts << "  <trk><name>BeaconFix track</name><trkseg>\n";
    for (const Fix &fx : m_history)
        ts << "    <trkpt lat=\"" << QString::number(fx.lat, 'f', 6) << "\" lon=\"" << QString::number(fx.lon, 'f', 6)
           << "\"><time>" << fx.time.toUTC().toString(Qt::ISODate) << "</time></trkpt>\n";
    ts << "  </trkseg></trk>\n</gpx>\n";
    return true;
}

double Locator::distanceM(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0, d2r = M_PI / 180.0;
    const double dLat = (lat2 - lat1) * d2r, dLon = (lon2 - lon1) * d2r;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2)
                   + std::cos(lat1 * d2r) * std::cos(lat2 * d2r) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * R * std::asin(std::sqrt(a));
}
