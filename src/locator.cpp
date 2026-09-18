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
#include <algorithm>

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
    m_wigleToken     = s.value("wigleToken").toString();
    m_wigleTimer.setSingleShot(true);
    connect(&m_wigleTimer, &QTimer::timeout, this, &Locator::pumpWigle);

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
    saveApRecords();
    emit scanUpdated();
}

void Locator::setWigleToken(const QString &t)
{
    m_wigleToken = t.trimmed();
    QSettings().setValue("wigleToken", m_wigleToken);
    if (!m_wigleToken.isEmpty()) queueWigle();
}

bool Locator::isTravelling(const QString &bssid) const
{
    if (m_notTravelling.contains(bssid)) return false;
    if (m_travelling.contains(bssid)) return true;
    const auto it = m_apRecords.constFind(bssid);
    return it != m_apRecords.constEnd() && it->cells.size() >= 2;   // seen at two stops ≥ ~5 km apart
}

const ApRecord *Locator::record(const QString &bssid) const
{
    const auto it = m_apRecords.constFind(bssid);
    return it == m_apRecords.constEnd() ? nullptr : &*it;
}

// Log-distance path loss, outdoor-ish: -25 dBm at 1 m, exponent 2.2
// (-55 dBm ≈ 23 m, -75 dBm ≈ 190 m, -90 dBm ≈ 900 m → clamped)
double Locator::rssiDistanceM(int dbm)
{
    return qBound(5.0, std::pow(10.0, (-25.0 - dbm) / 22.0), 800.0);
}

ApEstimate Locator::estimateFor(const AccessPoint &ap) const
{
    ApEstimate e;
    const ApRecord *r = record(ap.bssid);
    if (r && r->wigle) {
        e.kind = ApEstimate::Wigle; e.lat = r->wLat; e.lon = r->wLon; e.radiusM = 25;
        return e;
    }
    // Signal-weighted centroid once we've heard it from two places ≥ 25 m apart
    if (r && r->obs.size() >= 2) {
        double sw = 0, sl = 0, so = 0, spread = 0, worstAcc = 0;
        for (const ApObservation &o : r->obs) {
            const double w = std::pow(double(o.dbm + 100), 2.0);
            sw += w; sl += w * o.lat; so += w * o.lon;
            worstAcc = qMax(worstAcc, o.acc);
        }
        for (const ApObservation &o : r->obs)
            spread = qMax(spread, distanceM(o.lat, o.lon, r->obs.first().lat, r->obs.first().lon));
        // Real movement, not fix jitter: the spread has to beat the fixes' own uncertainty
        if (sw > 0 && spread >= qMax(60.0, worstAcc * 1.5)) {
            e.kind = ApEstimate::Centroid; e.lat = sl / sw; e.lon = so / sw;
            e.radiusM = qMax(rssiDistanceM(ap.dbm) * 0.6, spread * 0.5);
            return e;
        }
    }
    if (!m_fix.valid) return e;
    e.kind = ApEstimate::Ring; e.lat = m_fix.lat; e.lon = m_fix.lon;
    e.radiusM = rssiDistanceM(ap.dbm);
    e.bearingDeg = double(qHash(ap.bssid) % 3600) / 10.0;   // stable, but NOT a real bearing
    return e;
}

Stats Locator::stats() const
{
    Stats st;
    st.stops = m_history.size();
    for (int i = 1; i < m_history.size(); ++i)
        st.distanceKm += distanceM(m_history[i-1].lat, m_history[i-1].lon, m_history[i].lat, m_history[i].lon) / 1000.0;
    st.beaconsTotal = m_apRecords.size();
    st.beaconsNow = m_aps.size();
    for (const AccessPoint &ap : m_aps) {
        const QString s = apStatus(ap);
        if (s == QLatin1String("used")) ++st.usedNow;
        if (s == QLatin1String("travelling") || s == QLatin1String("active")) ++st.travellingNow;
        const ApEstimate::Kind k = estimateFor(ap).kind;
        if (k == ApEstimate::Centroid || k == ApEstimate::Wigle) ++st.locatedNow;
    }
    for (const Fix &f : m_history)
        if (f.accuracy >= 0 && (st.bestAccuracy < 0 || f.accuracy < st.bestAccuracy)) st.bestAccuracy = f.accuracy;
    struct R { int at; const char *name; };
    static const R ranks[] = {{0,"Newcomer"},{10,"Wanderer"},{50,"Scout"},{150,"Pathfinder"},{400,"Navigator"},{1000,"Cartographer"},{2500,"Beaconmaster"}};
    for (int i = 0; i < 7; ++i) {
        if (st.beaconsTotal >= ranks[i].at) { st.rank = QString::fromLatin1(ranks[i].name); st.rankLevel = i + 1; st.nextRankAt = i < 6 ? ranks[i+1].at : 0; }
    }
    return st;
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
    o["note"] = m_coarseNote;
    o["busy"] = m_busy;
    o["intervalMinutes"] = m_intervalMin;
    o["wifiInterface"] = m_scanner.interfaceName();
    QJsonArray aps;
    for (const AccessPoint &ap : m_aps) {
        const ApEstimate e = estimateFor(ap);
        QJsonObject a;
        a["bssid"] = ap.bssid; a["ssid"] = ap.ssid; a["dbm"] = ap.dbm; a["freq"] = ap.frequency;
        a["status"] = apStatus(ap);
        a["kind"] = e.kind == ApEstimate::Wigle ? "wigle" : e.kind == ApEstimate::Centroid ? "centroid" : e.kind == ApEstimate::Ring ? "ring" : "none";
        a["lat"] = e.lat; a["lon"] = e.lon; a["r"] = e.radiusM; a["bearing"] = e.bearingDeg;
        aps.append(a);
    }
    o["aps"] = aps;
    const Stats st = stats();
    QJsonObject sj;
    sj["stops"] = st.stops; sj["distanceKm"] = st.distanceKm; sj["beaconsTotal"] = st.beaconsTotal;
    sj["beaconsNow"] = st.beaconsNow; sj["usedNow"] = st.usedNow; sj["travellingNow"] = st.travellingNow;
    sj["locatedNow"] = st.locatedNow; sj["bestAccuracy"] = st.bestAccuracy;
    sj["rank"] = st.rank; sj["rankLevel"] = st.rankLevel; sj["nextRankAt"] = st.nextRankAt;
    o["stats"] = sj;
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
    QJsonObject fb; fb["ipf"] = false; fb["lacf"] = false;   // no GeoIP / cell fallback — we do that ourselves, honestly labelled
    body["fallbacks"] = fb;
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
        const double acc = o["accuracy"].toDouble(-1);
        if (rep->error() == QNetworkReply::NoError && loc.contains("lat") && acc > 5000) {
            // That's BeaconDB's GeoIP fallback (city-sized radius), not a Wi-Fi match
            tryIp(QStringLiteral("BeaconDB had no match for these access points (it offered a %1 km GeoIP guess)").arg(qRound(acc / 1000)));
            return;
        }
        if (rep->error() == QNetworkReply::NoError && loc.contains("lat")) {
            Fix f; f.valid = true; f.lat = loc["lat"].toDouble(); f.lon = loc["lng"].toDouble();
            f.accuracy = acc; f.source = QStringLiteral("wifi");
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
    // A coarse (IP) answer must not overwrite a precise fix we got recently: on
    // Starlink the IP answer is the ground station, hundreds of km off.
    if (cand.source == QLatin1String("ip") && m_fix.valid && m_fix.source != QLatin1String("ip")
        && m_fix.time.isValid() && m_fix.time.secsTo(cand.time) < 12 * 3600) {
        m_lastError.clear();
        m_coarseNote = QStringLiteral("IP says %1 — keeping the %2 fix from %3")
                           .arg(ipCity, m_fix.source == QLatin1String("wifi") ? QStringLiteral("Wi-Fi") : QStringLiteral("GPS"),
                                m_fix.time.toString(QStringLiteral("HH:mm")));
        saveState();
        emit FixChanged();
        return;
    }
    m_coarseNote.clear();
    if (cand.source != QLatin1String("ip") && cand.accuracy >= 0 && cand.accuracy < 2000)
        noteObservations(m_aps, cand);

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

// ── WiGLE (optional): real AP positions, one lookup per 1.5 s, cached per BSSID ──
void Locator::queueWigle()
{
    if (m_wigleToken.isEmpty()) return;
    const QDateTime stale = QDateTime::currentDateTime().addDays(-30);
    QList<AccessPoint> cand;
    for (const AccessPoint &ap : m_aps) {
        if (apStatus(ap) != QLatin1String("used")) continue;
        const ApRecord *r = record(ap.bssid);
        if (r && (r->wigle || (r->wigleChecked.isValid() && r->wigleChecked > stale))) continue;
        cand << ap;
    }
    std::sort(cand.begin(), cand.end(), [](const AccessPoint &a, const AccessPoint &b) { return a.dbm > b.dbm; });
    m_wigleQueue.clear();
    for (int i = 0; i < qMin(25, cand.size()); ++i) m_wigleQueue << cand[i].bssid;
    if (!m_wigleBusy && !m_wigleQueue.isEmpty()) m_wigleTimer.start(500);
}

void Locator::pumpWigle()
{
    if (m_wigleQueue.isEmpty() || m_wigleToken.isEmpty()) { m_wigleBusy = false; return; }
    m_wigleBusy = true;
    const QString bssid = m_wigleQueue.takeFirst();
    QUrl url(QStringLiteral("https://api.wigle.net/api/v2/network/search"));
    QUrlQuery q; q.addQueryItem("netid", bssid); url.setQuery(q);
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", "Basic " + m_wigleToken.toLatin1());
    req.setRawHeader("Accept", "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(USER_AGENT));
    req.setTransferTimeout(15000);
    QNetworkReply *rep = m_nam.get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep, bssid] {
        rep->deleteLater();
        const int http = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http == 401 || http == 429) {
            m_wigleQueue.clear(); m_wigleBusy = false;
            emit statusMessage(http == 401 ? QStringLiteral("WiGLE rejected the API token") : QStringLiteral("WiGLE daily query limit reached"));
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        ApRecord &r = m_apRecords[bssid];
        r.wigleChecked = QDateTime::currentDateTime();
        const QJsonArray res = o["results"].toArray();
        if (o["success"].toBool() && !res.isEmpty()) {
            const QJsonObject n = res.first().toObject();
            r.wigle = true; r.wLat = n["trilat"].toDouble(); r.wLon = n["trilong"].toDouble();
            if (r.ssid.isEmpty()) r.ssid = n["ssid"].toString();
        }
        saveApRecords();
        emit scanUpdated();
        m_wigleTimer.start(1500);
    });
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
        const QJsonObject cells = o["cells"].toObject();          // v1 format
        for (auto it = cells.begin(); it != cells.end(); ++it)
            for (const QJsonValue &v : it.value().toArray()) m_apRecords[it.key()].cells.insert(v.toString());
        const QJsonObject recs = o["records"].toObject();         // v2+ format
        const bool keepObs = o["version"].toInt() >= 3;           // v2 observations lacked accuracy → jitter-polluted
        for (auto it = recs.begin(); it != recs.end(); ++it) {
            const QJsonObject ro = it.value().toObject();
            ApRecord &r = m_apRecords[it.key()];
            r.ssid = ro["ssid"].toString();
            for (const QJsonValue &v : ro["cells"].toArray()) r.cells.insert(v.toString());
            for (const QJsonValue &v : ro["obs"].toArray()) {
                if (!keepObs) break;
                const QJsonObject oo = v.toObject();
                ApObservation ob; ob.lat = oo["lat"].toDouble(); ob.lon = oo["lon"].toDouble(); ob.acc = oo["acc"].toDouble();
                ob.dbm = oo["dbm"].toInt(); ob.time = QDateTime::fromString(oo["t"].toString(), Qt::ISODate);
                r.obs.append(ob);
            }
            r.wigle = ro["wigle"].toBool(); r.wLat = ro["wLat"].toDouble(); r.wLon = ro["wLon"].toDouble();
            r.wigleChecked = QDateTime::fromString(ro["wigleChecked"].toString(), Qt::ISODate);
        }
        for (const QJsonValue &v : o["travelling"].toArray()) m_travelling.insert(v.toString());
        for (const QJsonValue &v : o["notTravelling"].toArray()) m_notTravelling.insert(v.toString());
    }
}

void Locator::saveState() const
{
    QJsonObject o = m_fix.toJson();
    o["error"] = m_lastError;
    o["note"] = m_coarseNote;
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

void Locator::noteObservations(const QList<AccessPoint> &aps, const Fix &at)
{
    const QString cell = QStringLiteral("%1,%2").arg(qRound(at.lat * 20)).arg(qRound(at.lon * 20));
    for (const AccessPoint &ap : aps) {
        ApRecord &r = m_apRecords[ap.bssid];
        if (!ap.ssid.isEmpty()) r.ssid = ap.ssid;
        r.cells.insert(cell);
        // New observation if we've moved ≥ 25 m since the last one for this AP, or it's the first
        bool add = r.obs.isEmpty();
        if (!add) {
            const ApObservation &last = r.obs.last();
            const double moved = distanceM(last.lat, last.lon, at.lat, at.lon);
            // Only count it as a new vantage point if we've clearly moved beyond both fixes' error
            add = moved >= qMax(40.0, qMax(last.acc, at.accuracy) * 1.2);
            if (!add && qAbs(last.dbm - ap.dbm) >= 6 && at.accuracy < last.acc) {
                // same spot, better fix: replace the last observation
                r.obs.last().lat = at.lat; r.obs.last().lon = at.lon; r.obs.last().acc = at.accuracy;
                r.obs.last().dbm = ap.dbm; r.obs.last().time = at.time;
            }
        }
        if (add) {
            ApObservation ob; ob.lat = at.lat; ob.lon = at.lon; ob.acc = at.accuracy; ob.dbm = ap.dbm; ob.time = at.time;
            r.obs.append(ob);
            while (r.obs.size() > 60) r.obs.removeFirst();
        }
    }
    saveApRecords();
    queueWigle();
}

void Locator::saveApRecords() const
{
    QJsonObject recs;
    for (auto it = m_apRecords.begin(); it != m_apRecords.end(); ++it) {
        const ApRecord &r = it.value();
        QJsonObject ro; ro["ssid"] = r.ssid;
        QJsonArray cells; for (const QString &c : r.cells) cells.append(c); ro["cells"] = cells;
        QJsonArray obs;
        for (const ApObservation &o : r.obs) {
            QJsonObject oo; oo["lat"] = o.lat; oo["lon"] = o.lon; oo["acc"] = o.acc; oo["dbm"] = o.dbm; oo["t"] = o.time.toString(Qt::ISODate);
            obs.append(oo);
        }
        ro["obs"] = obs;
        if (r.wigle) { ro["wigle"] = true; ro["wLat"] = r.wLat; ro["wLon"] = r.wLon; }
        if (r.wigleChecked.isValid()) ro["wigleChecked"] = r.wigleChecked.toString(Qt::ISODate);
        recs[it.key()] = ro;
    }
    QJsonArray t, nt;
    for (const QString &b : m_travelling) t.append(b);
    for (const QString &b : m_notTravelling) nt.append(b);
    QJsonObject o; o["version"] = 3; o["records"] = recs; o["travelling"] = t; o["notTravelling"] = nt;
    QFile f(stateDir() + "/aps.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
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
