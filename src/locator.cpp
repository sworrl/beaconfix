#include "locator.h"
#include "ranging/rangingservice.h"
#include "identity.h"
#include "osintegration.h"
#include "apiserver.h"
#include "mdns.h"
#include "notify.h"
#include "mapdb.h"
#include "fitjson.h"
#include "importers.h"
#include "poiclassify.h"
#include <QClipboard>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDir>
#include <QGuiApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSharedPointer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QHostInfo>
#include <QSaveFile>
#include <QSysInfo>
#include <QPointF>
#include <QHash>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrlQuery>
#include <QtMath>
#include <algorithm>
#include <functional>
#include <cmath>

static QString ourDeviceName();
static bool importerDevice(const QString &d);
static const char *USER_AGENT = "BeaconFix/" BEACONFIX_VERSION " (KDE desktop locator; +https://github.com/sworrl/beaconfix)";

// ── Fix ───────────────────────────────────────────────────────────────────────
QJsonObject Fix::toJson() const
{
    QJsonObject o;
    o["valid"] = valid; o["lat"] = lat; o["lon"] = lon; o["accuracy"] = accuracy;
    o["source"] = source; o["place"] = place; o["time"] = time.toString(Qt::ISODate);
    o["apCount"] = apCount; o["apUsed"] = apUsed;
    if (departed.isValid()) o["departed"] = departed.toString(Qt::ISODate);
    if (hasElevation()) o["elev"] = elevation;
    if (!city.isEmpty()) o["city"] = city;
    if (!region.isEmpty()) o["region"] = region;
    if (!country.isEmpty()) o["country"] = country;
    if (!provider.isEmpty()) o["provider"] = provider;
    return o;
}
Fix Fix::fromJson(const QJsonObject &o)
{
    Fix f;
    f.valid = o["valid"].toBool(); f.lat = o["lat"].toDouble(); f.lon = o["lon"].toDouble();
    f.accuracy = o["accuracy"].toDouble(-1); f.source = o["source"].toString();
    f.place = o["place"].toString(); f.time = QDateTime::fromString(o["time"].toString(), Qt::ISODate);
    f.apCount = o["apCount"].toInt(); f.apUsed = o["apUsed"].toInt();
    f.departed = QDateTime::fromString(o["departed"].toString(), Qt::ISODate);
    f.elevation = o["elev"].toDouble(-9999);
    f.city = o["city"].toString(); f.region = o["region"].toString(); f.country = o["country"].toString();
    f.provider = o["provider"].toString();
    return f;
}

QJsonObject BeaconEvent::toJson() const
{
    QJsonObject o;
    o["id"] = id; o["type"] = type; o["time"] = time.toString(Qt::ISODate); o["text"] = text;
    if (!bssid.isEmpty()) { o["bssid"] = bssid; o["ssid"] = ssid; o["dbm"] = dbm; o["kind"] = kind; o["status"] = status; if (!security.isEmpty()) o["security"] = security; }
    if (delta) o["delta"] = delta;
    if (hasPos) { o["lat"] = lat; o["lon"] = lon; }
    if (hasFrom) { o["fromLat"] = fromLat; o["fromLon"] = fromLon; }
    if (r > 0) { o["r"] = r; o["bearing"] = bearing; }
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) o[it.key()] = it.value();
    return o;
}

QJsonObject DevicePos::toJson() const
{
    const qint64 age = time.isValid() ? time.secsTo(QDateTime::currentDateTime()) : -1;
    return {{"device", device}, {"kind", kind}, {"identityId", identityId}, {"identityName", identityName}, {"lat", lat}, {"lon", lon}, {"acc", acc},
            {"time", time.toString(Qt::ISODate)}, {"ageS", double(age)}, {"source", source}, {"place", place}, {"online", online}, {"beacons", beacons},
            {"lastSeen", lastSeen.toString(Qt::ISODate)}};
}

// ── Locator ───────────────────────────────────────────────────────────────────
// ~15 m cells this host scanned from, with the time span (misses: docs/GRADING.md §1.7)
static QString scanCellKey(double lat, double lon, int *ky = nullptr)
{
    const int y = int(std::floor(lat / 0.000135));
    const double c = std::max(0.05, std::cos((y * 0.000135) * M_PI / 180.0));
    const int x = int(std::floor(lon * c / 0.000135));
    if (ky) *ky = y;
    return QStringLiteral("%1:%2").arg(y).arg(x);
}
static QString scanBucket(double lat, double lon) { return QStringLiteral("%1:%2").arg(qFloor(lat * 100)).arg(qFloor(lon * 100)); }


Locator::Locator(bool standalone, QObject *parent) : QObject(parent), m_standalone(standalone)
{
    QSettings s;
    m_intervalMin    = s.value("intervalMinutes", 15).toInt();
    m_moveThresholdM = s.value("moveThresholdM", 250).toInt();
    m_useStarlink    = s.value("useStarlink", true).toBool();
    m_starlinkHost   = s.value("starlinkHost", "192.168.100.1").toString();
    m_useIp          = s.value("useIp", true).toBool();
    m_useApple       = s.value("useApple", true).toBool();
    m_ignoreActive   = s.value("ignoreActiveAp", true).toBool();
    m_ignore         = s.value("ignorePatterns").toStringList();
    m_home           = s.value("homeNetworks").toStringList();
    {                                                          // our UniFi radios: BSSID → SSID, for home:true / homeSsid in the AP JSON
        QFile hf(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/sworrl/home-networks.json"));
        if (hf.open(QIODevice::ReadOnly))
            for (const QJsonValue &v : QJsonDocument::fromJson(hf.readAll()).object()["bssids"].toArray())
                if (v.isObject()) m_homeSsids.insert(v.toObject()["bssid"].toString().toUpper(), v.toObject()["ssid"].toString());
    }
    if (m_home.isEmpty()) {                                    // first run: seed from the UniFi export if it is there
        const QString seed = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/sworrl/home-networks.json");
        if (QFile::exists(seed)) {
            QString err;
            const QStringList l = parseHomeNetworksFile(seed, &err);
            if (!l.isEmpty()) { m_home = l; s.setValue("homeNetworks", m_home); qInfo("beaconfix: home networks seeded from %s (%d patterns)", qPrintable(seed), int(l.size())); }
        }
    }
    m_homeFix        = Fix::fromJson(QJsonDocument::fromJson(s.value("homeFix").toByteArray()).object());
    rebuildPatternCaches();
    m_wigleToken     = s.value("wigleToken").toString();
    m_poiRadiusKm    = qBound(1, s.value("poiRadiusKm", 6).toInt(), 30);
    m_pedsRadiusKm   = qBound(50, s.value("pedsRadiusKm", 150).toInt(), 300);
    m_pedsClassifier = s.value("pedsClassifier", 1).toInt();
    m_overpassTimer.setSingleShot(true);
    connect(&m_overpassTimer, &QTimer::timeout, this, &Locator::pumpOverpass);
    m_pedsRetryTimer.setSingleShot(true);
    connect(&m_pedsRetryTimer, &QTimer::timeout, this, [this] { refreshPediatric(false); });
    m_notifyStops    = s.value("notifyStops", true).toBool();
    m_notifyRegions  = s.value("notifyRegions", true).toBool();
    m_notifyAchievements = s.value("notifyAchievements", true).toBool();
    m_prefetch       = s.value("prefetchTiles", true).toBool();
    m_useElevation   = s.value("useElevation", true).toBool();
    m_tripStart      = s.value("tripStart").toDateTime();
    m_prefetchedTiles = s.value("prefetchedTiles", 0).toInt();
    m_liveScanSecs   = qBound(0, s.value("liveScanSeconds", 45).toInt(), 3600);
    m_liveTimer.setInterval(qMax(15, m_liveScanSecs) * 1000);
    connect(&m_liveTimer, &QTimer::timeout, this, &Locator::liveScan);
    m_wigleTimer.setSingleShot(true);
    connect(&m_wigleTimer, &QTimer::timeout, this, &Locator::pumpWigle);

    connect(&m_scanner, &WifiScanner::scanFinished, this, &Locator::onScanFinished);
    connect(&m_scanner, &WifiScanner::scanFailed, this, [this](const QString &m) {
        if (m_liveScan && !m_probeWantsScan) { m_liveScan = false; return; }   // quiet: the next live scan retries
        m_liveScan = false; m_probeWantsScan = false;
        m_aps.clear(); emit scanUpdated();
        tryIp(QStringLiteral("Wi-Fi scan failed: ") + m);
    });
    m_timer.setInterval(m_intervalMin * 60 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &Locator::Refresh);
    // The internal mapping database (encrypted SQLite). A standalone process only reads it.
    m_db = new MapDb(stateDir(), this);
    if (!m_db->open(standalone)) {
        if (!standalone || m_db->error() != QLatin1String("no database yet")) qWarning("beaconfix: map database unavailable: %s", qPrintable(m_db->error()));
    }
    m_dbUsable = m_db->isOpen() && !m_db->readOnly();
    // Identity (docs/IDENTITY.md): sealed with the map-database key; the OS integration follows the fix
    m_identity = new Identity(this);
    if (!m_standalone) { m_notifier = new Notifier(this); connect(m_notifier, &Notifier::fallback, this, &Locator::notificationFallback); }
    m_deviceTimer.setInterval(60000);
    connect(&m_deviceTimer, &QTimer::timeout, this, [this] {           // presence: seen through the API / mDNS in the last 10 min
        const QDateTime now = QDateTime::currentDateTime();
        if (m_api && m_api->mdns()) for (const Mdns::Peer &p : m_api->mdns()->peers(false)) if (!p.host.isEmpty()) { DevicePos &d = m_devicePos[p.host]; if (d.device.isEmpty()) d.device = p.host; if (d.kind.isEmpty()) d.kind = p.kind; if (p.lastSeen > d.lastSeen) d.lastSeen = p.lastSeen; if (!d.identityId.isEmpty() == false && !p.identityId.isEmpty()) { d.identityId = p.identityId; d.identityName = p.identityName; } }
        for (DevicePos &d : m_devicePos) {
            const bool on = d.lastSeen.isValid() && d.lastSeen.secsTo(now) < 600;
            if (on != d.online) { d.online = on; if (d.time.isValid()) { BeaconEvent ev; ev.type = on ? QStringLiteral("device_online") : QStringLiteral("device_offline"); ev.text = QStringLiteral("%1 is %2").arg(d.device, on ? QStringLiteral("online") : QStringLiteral("offline")); ev.extra = QJsonObject{{"device", d.device}}; logEvent(ev); } }
        }
    });
    if (!m_standalone) m_deviceTimer.start();
    m_identity->load(m_db->key());
    connect(m_identity, &Identity::changed, this, [this] { if (m_api) emit scanUpdated(); });
    m_os = new OsIntegration(this, this);
    if (!standalone) {
        connect(this, &Locator::FixChanged, m_os, &OsIntegration::onFixChanged);
        connect(m_os, &OsIntegration::applied, this, [this](const QString &what) {
            BeaconEvent ev; ev.type = QStringLiteral("tz"); ev.text = what; logEvent(ev);
            notify(QStringLiteral("BeaconFix"), what, QStringLiteral("preferences-system-time"));
        });
    }
    const bool anyJson = QFile::exists(stateDir() + "/aps.json") || QFile::exists(stateDir() + "/history.jsonl") || QFile::exists(stateDir() + "/pois.json")
                      || QFile::exists(stateDir() + "/elev.json") || QFile::exists(stateDir() + "/achievements.json");
    loadState();
    loadPois();
    loadElevationCache();
    loadAchievements();
    if (m_dbUsable && m_db->isEmpty() && anyJson) migrateJsonToDb();
    else if (m_db->isOpen()) loadFromDb();
    loadAnchors();
    // The estimator's state (docs/GRADING.md): device offsets, calibration, the cells we scanned from
    if (m_db->isOpen()) {
        const QJsonObject offs = QJsonDocument::fromJson(m_db->kv(QStringLiteral("device_offsets")).toUtf8()).object();
        for (auto it = offs.begin(); it != offs.end(); ++it) m_devOffsets.insert(it.key(), it.value().toDouble());
        const double k = m_db->kv(QStringLiteral("estimator_kappa")).toDouble();
        if (k > 0) m_kappa = k;
        m_calibration = QJsonDocument::fromJson(m_db->kv(QStringLiteral("estimator_calibration")).toUtf8()).object();
        for (const MapDb::ScanCellRow &c : m_db->loadScanCells()) {
            m_scanCells.insert(c.key, ScanCell{c.lat, c.lon, c.count, c.first, c.last});
            m_scanIndex[scanBucket(c.lat, c.lon)] << c.key;
        }
    }
    if (!standalone) {
        rebuildGroups();
        calibrateAnchors();
        queueUpgradeRefits();
        m_calibrateTimer.setInterval(6 * 3600 * 1000);
        connect(&m_calibrateTimer, &QTimer::timeout, this, &Locator::calibrateEstimator);
        m_calibrateTimer.start();
    }
    rebuildMergedPois();
    if (m_db->isOpen()) m_countryCode = m_db->kv(QStringLiteral("countryCode"));
    for (const Fix &f : m_history) noteVisited(f, false);
    connect(this, &Locator::FixChanged, this, [this] { rebuildMergedPois(); refreshPois(); refreshPediatric(false); fetchElevation(); checkAchievements(); });
    connect(this, &Locator::scanUpdated, this, [this] { checkAchievements(); });
    // Position refinement: beacons with new samples are refit in a batch, not on every scan
    m_refitTimer.setSingleShot(true); m_refitTimer.setInterval(10000);
    connect(&m_refitTimer, &QTimer::timeout, this, &Locator::refitQueued);
    // Sync with other BeaconFix instances (a laptop, the phone's desktop) on a timer
    loadSyncPeers();
    m_syncTimer.setInterval(60000);
    connect(&m_syncTimer, &QTimer::timeout, this, [this] {
        if (m_standalone || m_syncBusy) return;
        const QDateTime now = QDateTime::currentDateTime();
        for (int i = 0; i < m_syncPeers.size(); ++i) {
            const SyncPeer &pr = m_syncPeers[i];
            if (pr.minutes <= 0 || pr.url.isEmpty() || pr.token.isEmpty()) continue;
            if (!pr.last.isValid() || pr.last.secsTo(now) >= pr.minutes * 60) { syncStep(i); break; }
        }
    });
    if (!standalone) m_syncTimer.start();
}

void Locator::setNotifyStops(bool b)        { m_notifyStops = b; QSettings().setValue("notifyStops", b); }
void Locator::setNotifyRegions(bool b)      { m_notifyRegions = b; QSettings().setValue("notifyRegions", b); }
void Locator::setNotifyAchievements(bool b) { m_notifyAchievements = b; QSettings().setValue("notifyAchievements", b); }
void Locator::setPrefetchTiles(bool b)      { m_prefetch = b; QSettings().setValue("prefetchTiles", b); }
void Locator::setUseElevation(bool b)       { m_useElevation = b; QSettings().setValue("useElevation", b); if (b) fetchElevation(); }

void Locator::notePrefetchDone(int tiles)
{
    m_prefetchedTiles += tiles;
    QSettings().setValue("prefetchedTiles", m_prefetchedTiles);
    if (tiles > 0) emit statusMessage(QStringLiteral("Map around here saved for offline use (%1 tiles)").arg(tiles));
    if (tiles > 0) { BeaconEvent ev; ev.type = QStringLiteral("prefetch"); ev.text = QStringLiteral("Saved %1 map tiles around here for offline use").arg(tiles); logEvent(ev); }
    checkAchievements();
}

// Desktop notification through the freedesktop daemon (Plasma's), else the tray balloon
void Locator::notify(const QString &summary, const QString &body, const QString &icon)
{
    if (m_standalone || !m_notifier) return;
    m_notifier->send(summary, body, icon);
}

void Locator::notifyWithActions(const QString &summary, const QString &body, const QString &icon, const QStringList &actions, std::function<void(const QString &)> onAction, int timeoutMs)
{
    if (m_standalone || !m_notifier) return;
    m_notifier->send(summary, body, icon, actions, std::move(onAction), timeoutMs);
}

void Locator::startTrip(const QDateTime &at)
{
    m_tripStart = at;
    QSettings().setValue("tripStart", m_tripStart);
    emit statusMessage(QStringLiteral("New trip started %1").arg(at.toString(QStringLiteral("ddd d MMM HH:mm"))));
    emit FixChanged();
}
void Locator::StartTrip() { startTrip(); }

// ── LAN API forwarding (D-Bus / CLI) ─────────────────────────────────────────
void Locator::setApiServer(ApiServer *api)
{
    m_api = api;
    if (!api) return;
    connect(api, &ApiServer::pairingRequested, this, &Locator::pairingRequested);
    connect(api, &ApiServer::deviceApproved, this, &Locator::deviceApproved);
    connect(api, &ApiServer::peersChanged, this, &Locator::peersChanged);
    connect(api, &ApiServer::openPairRequested, this, &Locator::pairingOpenRequested);
}

QString Locator::Peers(bool scan)
{
    if (!m_api) return QStringLiteral("{\"peers\":[],\"count\":0,\"mdns\":false}");
    if (scan) {
        QEventLoop loop; bool finished = false;
        m_api->scanPeers([&] { finished = true; loop.quit(); });
        if (!finished) { QTimer::singleShot(8000, &loop, &QEventLoop::quit); loop.exec(); }
    }
    const QJsonArray peers = m_api->peersJson(false);
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"peers", peers}, {"count", peers.size()}, {"scanned", scan},
                                                       {"mdns", m_api->mdns() && m_api->mdns()->published()},
                                                       {"self", QJsonObject{{"host", QHostInfo::localHostName()}, {"addresses", QJsonArray::fromStringList(Mdns::lanAddresses())}, {"port", m_api->boundPort()}}}}).toJson(QJsonDocument::Compact));
}
bool    Locator::apiListening() const { return m_api && m_api->listening(); }
QString Locator::ApiStatus() const { return m_api ? QString::fromUtf8(QJsonDocument(m_api->statusJson()).toJson(QJsonDocument::Compact)) : QStringLiteral("{\"enabled\":false,\"listening\":false}"); }
bool    Locator::ApproveDevice(const QString &id) { return m_api && m_api->approve(id); }
bool    Locator::DenyDevice(const QString &id) { return m_api && m_api->deny(id); }
bool    Locator::RevokeDevice(const QString &nameOrId) { return m_api && m_api->revoke(nameOrId); }
QString Locator::CreateToken(const QString &name, const QString &scopes) { return m_api ? m_api->createToken(name, scopes.split(QLatin1Char(','), Qt::SkipEmptyParts)) : QString(); }
bool    Locator::OpenPairing(int minutes) { if (!m_api) return false; if (minutes <= 0) m_api->closePairing(); else m_api->openPairing(minutes); return true; }
QString Locator::KnownDevices() const { return m_api ? QString::fromUtf8(QJsonDocument(m_api->knownJson()).toJson(QJsonDocument::Compact)) : QStringLiteral("{}"); }
bool    Locator::KnownAdd(const QString &mac, const QString &name) { return m_api && m_api->knownAdd(mac, name); }
bool    Locator::KnownRemove(const QString &mac) { return m_api && m_api->knownRemove(mac); }
int     Locator::KnownImport(const QString &path) { return m_api ? m_api->knownImport(path) : -1; }
void Locator::PrefetchTiles() { emit prefetchRequested(); }

bool Locator::ExportGpx(const QString &path)
{
    QString err;
    const bool ok = exportGpx(path, &err);
    emit statusMessage(ok ? QStringLiteral("Exported %1 fixes to %2").arg(m_history.size()).arg(path) : QStringLiteral("GPX export failed: ") + err);
    return ok;
}

bool Locator::CopyToClipboard(const QString &what)
{
    if (!m_fix.valid) return false;
    const QString w = what.toLower();
    const QString text = w == QLatin1String("geo") ? geoUri() : w == QLatin1String("osm") ? osmUrl()
                       : w == QLatin1String("google") ? googleMapsUrl() : w == QLatin1String("apple") ? appleMapsUrl()
                       : w == QLatin1String("text") ? shareText() : coordsText();
    if (QClipboard *cb = QGuiApplication::clipboard()) { cb->setText(text); emit statusMessage(QStringLiteral("Copied: ") + text.section('\n', 0, 0)); return true; }
    return false;
}

// ── Sharing ───────────────────────────────────────────────────────────────────
QString Locator::coordsText() const { return QStringLiteral("%1, %2").arg(m_fix.lat, 0, 'f', 6).arg(m_fix.lon, 0, 'f', 6); }
QString Locator::geoUri() const
{
    if (!m_fix.valid) return {};
    QString u = QStringLiteral("geo:%1,%2").arg(m_fix.lat, 0, 'f', 6).arg(m_fix.lon, 0, 'f', 6);
    if (m_fix.accuracy > 0) u += QStringLiteral(";u=%1").arg(qRound(m_fix.accuracy));
    return u;
}
QString Locator::osmUrl() const { return QStringLiteral("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=15/%1/%2").arg(m_fix.lat, 0, 'f', 6).arg(m_fix.lon, 0, 'f', 6); }
QString Locator::googleMapsUrl() const { return QStringLiteral("https://www.google.com/maps/search/?api=1&query=%1,%2").arg(m_fix.lat, 0, 'f', 6).arg(m_fix.lon, 0, 'f', 6); }
QString Locator::appleMapsUrl() const { return QStringLiteral("https://maps.apple.com/?ll=%1,%2&q=%3").arg(m_fix.lat, 0, 'f', 6).arg(m_fix.lon, 0, 'f', 6).arg(QString::fromLatin1(QUrl::toPercentEncoding(m_fix.place.isEmpty() ? QStringLiteral("BeaconFix") : m_fix.place))); }
QString Locator::shareText() const
{
    if (!m_fix.valid) return {};
    QString t = m_fix.place + QStringLiteral("\n") + coordsText();
    if (m_fix.accuracy > 0) t += QStringLiteral(" (±%1 m, %2)").arg(qRound(m_fix.accuracy)).arg(m_fix.source == QLatin1String("wifi") ? QStringLiteral("Wi-Fi") : m_fix.source == QLatin1String("starlink") ? QStringLiteral("GPS") : QStringLiteral("IP, approximate"));
    if (m_fix.hasElevation()) t += QStringLiteral("\n") + elevationText();
    t += QStringLiteral("\n") + osmUrl();
    return t;
}

QString Locator::compass(double deg)
{
    static const char *pts[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    if (deg < 0) return QStringLiteral("—");
    return QString::fromLatin1(pts[int(std::fmod(deg + 11.25 + 360.0, 360.0) / 22.5) % 16]);
}

QString Locator::durationText(qint64 secs)
{
    if (secs < 0) return QStringLiteral("—");
    if (secs < 60) return QStringLiteral("%1 s").arg(secs);
    if (secs < 3600) return QStringLiteral("%1 min").arg(secs / 60);
    if (secs < 86400) return QStringLiteral("%1 h %2 min").arg(secs / 3600).arg((secs % 3600) / 60);
    return QStringLiteral("%1 d %2 h").arg(secs / 86400).arg((secs % 86400) / 3600);
}

QString Locator::elevationText() const
{
    if (!m_fix.hasElevation()) return QStringLiteral("elevation unknown");
    return QStringLiteral("%1 m (%2 ft) above sea level").arg(qRound(m_fix.elevation)).arg(qRound(m_fix.elevation * 3.28084));
}

// ── Sun: NOAA / Wikipedia sunrise equation, good to a minute or two ─────────────
static QDateTime jdToDateTime(double jd) { return QDateTime::fromSecsSinceEpoch(qint64((jd - 2440587.5) * 86400.0), QTimeZone::utc()).toLocalTime(); }

SunTimes Locator::sunTimes(double lat, double lon, const QDateTime &when)
{
    SunTimes st;
    if (!when.isValid()) return st;
    const double d2r = M_PI / 180.0;
    // Julian day of local noon so "today" is the observer's calendar day
    const QDateTime noonLocal(when.toLocalTime().date(), QTime(12, 0), when.toLocalTime().timeZone());
    const double jd = noonLocal.toSecsSinceEpoch() / 86400.0 + 2440587.5;
    const double n = std::round(jd - 2451545.0 + 0.0008 + lon / 360.0);
    const double js = n - lon / 360.0;
    const double M = std::fmod(357.5291 + 0.98560028 * js, 360.0);
    const double C = 1.9148 * std::sin(M * d2r) + 0.02 * std::sin(2 * M * d2r) + 0.0003 * std::sin(3 * M * d2r);
    const double L = std::fmod(M + C + 180.0 + 102.9372, 360.0);
    const double jt = 2451545.0 + js + 0.0053 * std::sin(M * d2r) - 0.0069 * std::sin(2 * L * d2r);
    const double sinDec = std::sin(L * d2r) * std::sin(23.4397 * d2r), dec = std::asin(sinDec);
    auto hourAngle = [&](double altDeg, bool *ok) {
        const double c = (std::sin(altDeg * d2r) - std::sin(lat * d2r) * sinDec) / (std::cos(lat * d2r) * std::cos(dec));
        *ok = c >= -1.0 && c <= 1.0;
        return *ok ? std::acos(c) / d2r : (c < -1.0 ? 180.0 : 0.0);
    };
    bool ok = false;
    const double w0 = hourAngle(-0.833, &ok);
    st.valid = true;
    st.solarNoon = jdToDateTime(jt);
    if (!ok) { st.polarDay = w0 >= 180.0; st.polarNight = !st.polarDay; st.dayLengthSecs = st.polarDay ? 86400 : 0; return st; }
    st.sunrise = jdToDateTime(jt - w0 / 360.0); st.sunset = jdToDateTime(jt + w0 / 360.0);
    st.dayLengthSecs = int(st.sunrise.secsTo(st.sunset));
    bool okG = false, okC = false;
    const double wg = hourAngle(6.0, &okG), wc = hourAngle(-6.0, &okC);
    if (okG) { st.goldenMorningEnd = jdToDateTime(jt - wg / 360.0); st.goldenEveningStart = jdToDateTime(jt + wg / 360.0); }
    if (okC) { st.civilDawn = jdToDateTime(jt - wc / 360.0); st.civilDusk = jdToDateTime(jt + wc / 360.0); }
    return st;
}

SunTimes Locator::sun() const
{
    if (!m_fix.valid) return {};
    return sunTimes(m_fix.lat, m_fix.lon, QDateTime::currentDateTime());
}

// ── Elevation (Open Topo Data SRTM 30 m), cached per ~100 m cell ────────────────
static QString elevKey(double lat, double lon) { return QStringLiteral("%1,%2").arg(lat, 0, 'f', 3).arg(lon, 0, 'f', 3); }

void Locator::fetchElevation()
{
    if (!m_useElevation || !m_fix.precise() || m_elevBusy) return;
    const QString k = elevKey(m_fix.lat, m_fix.lon);
    const auto it = m_elevCache.constFind(k);
    if (it != m_elevCache.constEnd()) {
        if (!m_fix.hasElevation() || std::abs(m_fix.elevation - *it) > 0.5) {
            m_fix.elevation = *it;
            if (!m_history.isEmpty() && distanceM(m_history.last().lat, m_history.last().lon, m_fix.lat, m_fix.lon) < 200) { m_history.last().elevation = *it; rewriteHistory(); }
            saveState(); emit elevationUpdated();
        }
        return;
    }
    if (m_elevTried.isValid() && m_elevTried.secsTo(QDateTime::currentDateTime()) < 60) return;
    m_elevTried = QDateTime::currentDateTime();
    m_elevBusy = true;
    QNetworkRequest req(QUrl(QStringLiteral("https://api.opentopodata.org/v1/srtm30m?locations=%1,%2").arg(m_fix.lat, 0, 'f', 5).arg(m_fix.lon, 0, 'f', 5)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(USER_AGENT));
    req.setTransferTimeout(12000);
    QNetworkReply *rep = m_nam.get(req);
    const double lat = m_fix.lat, lon = m_fix.lon;
    connect(rep, &QNetworkReply::finished, this, [this, rep, k, lat, lon] {
        rep->deleteLater();
        m_elevBusy = false;
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        const QJsonArray res = o["results"].toArray();
        if (rep->error() != QNetworkReply::NoError || res.isEmpty() || res.first().toObject()["elevation"].isNull()) {
            m_elevNote = QStringLiteral("elevation lookup failed (%1)").arg(rep->error() == QNetworkReply::NoError ? o["error"].toString().left(60) : rep->errorString());
            return;
        }
        const double e = res.first().toObject()["elevation"].toDouble();
        m_elevNote.clear();
        m_elevCache.insert(k, e);
        while (m_elevCache.size() > 600) m_elevCache.erase(m_elevCache.begin());
        saveElevationCache();
        if (m_fix.valid && distanceM(m_fix.lat, m_fix.lon, lat, lon) < 150) {
            m_fix.elevation = e;
            if (!m_history.isEmpty() && distanceM(m_history.last().lat, m_history.last().lon, lat, lon) < 200) { m_history.last().elevation = e; rewriteHistory(); }
            saveState();
            emit elevationUpdated();
            checkAchievements();
        }
    });
}

void Locator::loadElevationCache()
{
    QFile f(stateDir() + "/elev.json");
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = o.begin(); it != o.end(); ++it) m_elevCache.insert(it.key(), it.value().toDouble());
}
void Locator::saveElevationCache() const
{
    if (m_dbUsable) { m_db->saveElevation(m_elevCache); return; }
    QJsonObject o;
    for (auto it = m_elevCache.begin(); it != m_elevCache.end(); ++it) o[it.key()] = it.value();
    QFile f(stateDir() + "/elev.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// ── Places visited ────────────────────────────────────────────────────────────
void Locator::parsePlace(const Fix &f, QString *city, QString *region, QString *country)
{
    *city = f.city; *region = f.region; *country = f.country;
    if (!city->isEmpty() || !region->isEmpty()) return;
    // Old entries only carry "Neighbourhood, City, State"; a bare "lat, lon" placeholder
    // (geocode never arrived) is not a place at all
    static const QRegularExpression numeric(QStringLiteral("^\\s*-?\\d+(\\.\\d+)?\\s*$"));
    const QStringList parts = f.place.split(QStringLiteral(", "), Qt::SkipEmptyParts);
    for (const QString &p : parts) if (numeric.match(p).hasMatch()) return;
    if (parts.size() >= 2) { *region = parts.last(); *city = parts[parts.size() - 2]; }
    else if (parts.size() == 1) *city = parts[0];
}

void Locator::noteVisited(const Fix &f, bool announce)
{
    if (!f.precise()) return;
    QString city, region, country;
    parsePlace(f, &city, &region, &country);
    const bool newRegion = !region.isEmpty() && !m_seenRegions.contains(region);
    const bool newCountry = !country.isEmpty() && !m_seenCountries.contains(country);
    if (!region.isEmpty()) m_seenRegions.insert(region);
    if (!country.isEmpty()) m_seenCountries.insert(country);
    if (announce && (newRegion || newCountry)) {
        BeaconEvent ev; ev.type = QStringLiteral("region"); ev.hasPos = true; ev.lat = f.lat; ev.lon = f.lon;
        ev.text = QStringLiteral("Welcome to %1 · first time here").arg(newCountry ? country : region);
        logEvent(ev);
    }
    if (announce && m_notifyRegions && (newRegion || newCountry))
        notify(newCountry ? QStringLiteral("Welcome to %1").arg(country) : QStringLiteral("Welcome to %1").arg(region),
               QStringLiteral("%1 · first time here").arg(f.place), QStringLiteral("flag"));
}

// ── Trip log with dwell and legs ───────────────────────────────────────────────
QList<Stop> Locator::stops() const
{
    QList<Stop> out;
    int lastPrecise = -1;
    for (int i = 0; i < m_history.size(); ++i) {
        Stop s; s.fix = m_history[i];
        const QDateTime left = s.fix.departed.isValid() ? s.fix.departed
                             : i + 1 < m_history.size() ? QDateTime() : (m_fix.valid && m_fix.time.isValid() && m_fix.time >= s.fix.time ? m_fix.time : QDateTime());
        if (left.isValid()) s.dwellSecs = int(s.fix.time.secsTo(left));
        if (s.fix.precise() && lastPrecise >= 0) {
            const Fix &p = m_history[lastPrecise];
            s.legKm = distanceM(p.lat, p.lon, s.fix.lat, s.fix.lon) / 1000.0;
            if (p.departed.isValid() && p.departed < s.fix.time) s.legSecs = int(p.departed.secsTo(s.fix.time));
        }
        if (s.fix.precise()) lastPrecise = i;
        out << s;
    }
    return out;
}

// ── Ranks & achievements ──────────────────────────────────────────────────────
const QList<RankTier> &Locator::rankLadder()
{
    static const QList<RankTier> ladder = {{0, "Newcomer"}, {10, "Wanderer"}, {50, "Scout"}, {150, "Pathfinder"}, {400, "Navigator"},
                                           {1000, "Cartographer"}, {2500, "Beaconmaster"}, {5000, "Surveyor"}, {10000, "Wayfinder"}, {25000, "Lighthouse"}};
    return ladder;
}

static const QList<Achievement> &achievementDefs()
{
    static const QList<Achievement> defs = {
        {QStringLiteral("first_fix"),     QStringLiteral("On the map"),        QStringLiteral("First precise fix (Wi-Fi or GPS)"),            QStringLiteral("📍"), {}},
        {QStringLiteral("first_wifi"),    QStringLiteral("Heard the beacons"), QStringLiteral("First BeaconDB Wi-Fi fix"),                    QStringLiteral("📶"), {}},
        {QStringLiteral("first_gps"),     QStringLiteral("Dish talks"),        QStringLiteral("First Starlink dish GPS fix"),                 QStringLiteral("🛰️"), {}},
        {QStringLiteral("sharp_fix"),     QStringLiteral("Pin-sharp"),         QStringLiteral("A fix better than ±30 m"),                     QStringLiteral("🎯"), {}},
        {QStringLiteral("first_wigle"),   QStringLiteral("Ground truth"),      QStringLiteral("First beacon placed by WiGLE"),                QStringLiteral("💎"), {}},
        {QStringLiteral("first_located"), QStringLiteral("Triangulated"),      QStringLiteral("First beacon multilaterated from your own stops"), QStringLiteral("📐"), {}},
        {QStringLiteral("first_trilat"),  QStringLiteral("Surveyor"),          QStringLiteral("First beacon positioned by a least-squares fit of your samples"), QStringLiteral("📡"), {}},
        {QStringLiteral("trilat_100"),    QStringLiteral("Cartographer's hundred"), QStringLiteral("100 beacons positioned from your own samples"),  QStringLiteral("🗺️"), {}},
        {QStringLiteral("beacons_100"),   QStringLiteral("Century"),           QStringLiteral("100 beacons logged"),                          QStringLiteral("💯"), {}},
        {QStringLiteral("beacons_1000"),  QStringLiteral("Grand"),             QStringLiteral("1,000 beacons logged"),                        QStringLiteral("🏆"), {}},
        {QStringLiteral("beacons_5000"),  QStringLiteral("Beacon hoard"),      QStringLiteral("5,000 beacons logged"),                        QStringLiteral("👑"), {}},
        {QStringLiteral("stops_10"),      QStringLiteral("Ten stops"),         QStringLiteral("10 entries in the trip log"),                  QStringLiteral("🛑"), {}},
        {QStringLiteral("stops_50"),      QStringLiteral("Fifty stops"),       QStringLiteral("50 entries in the trip log"),                  QStringLiteral("🗺️"), {}},
        {QStringLiteral("regions_5"),     QStringLiteral("Five states"),       QStringLiteral("Precise fixes in 5 states or provinces"),       QStringLiteral("🧭"), {}},
        {QStringLiteral("regions_10"),    QStringLiteral("Ten states"),        QStringLiteral("Precise fixes in 10 states or provinces"),      QStringLiteral("🌎"), {}},
        {QStringLiteral("countries_2"),   QStringLiteral("Border crossing"),   QStringLiteral("Precise fixes in 2 countries"),                QStringLiteral("🛂"), {}},
        {QStringLiteral("km_1000"),       QStringLiteral("Thousand-kilometre club"), QStringLiteral("1,000 km between precise stops"),         QStringLiteral("🛣️"), {}},
        {QStringLiteral("km_10000"),      QStringLiteral("Ten thousand"),      QStringLiteral("10,000 km between precise stops"),             QStringLiteral("🌐"), {}},
        {QStringLiteral("long_haul"),     QStringLiteral("Long haul"),         QStringLiteral("A single leg over 500 km inside a day"),       QStringLiteral("🚛"), {}},
        {QStringLiteral("homebody"),      QStringLiteral("Settled in"),        QStringLiteral("A week at one stop"),                          QStringLiteral("🏕️"), {}},
        {QStringLiteral("night_owl"),     QStringLiteral("Night owl"),         QStringLiteral("A precise fix between midnight and 4 am"),     QStringLiteral("🦉"), {}},
        {QStringLiteral("early_bird"),    QStringLiteral("Early bird"),        QStringLiteral("A precise fix before sunrise"),                QStringLiteral("🐦"), {}},
        {QStringLiteral("high_ground"),   QStringLiteral("High ground"),       QStringLiteral("A stop above 2,000 m"),                        QStringLiteral("⛰️"), {}},
        {QStringLiteral("sea_level"),     QStringLiteral("Sea level"),         QStringLiteral("A stop within 5 m of sea level"),              QStringLiteral("🌊"), {}},
        {QStringLiteral("offline_ready"), QStringLiteral("Off-grid ready"),    QStringLiteral("Map tiles saved for offline use"),             QStringLiteral("💾"), {}},
        {QStringLiteral("lighthouse"),    QStringLiteral("Lighthouse"),        QStringLiteral("Reached the top of the rank ladder"),          QStringLiteral("🗼"), {}},
    };
    return defs;
}

void Locator::loadAchievements()
{
    m_achievements = achievementDefs();
    QFile f(stateDir() + "/achievements.json");
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (Achievement &a : m_achievements) a.unlocked = QDateTime::fromString(o[a.key].toString(), Qt::ISODate);
}
void Locator::saveAchievements() const
{
    if (m_dbUsable) { m_db->saveAchievements(m_achievements); return; }
    QJsonObject o;
    for (const Achievement &a : m_achievements) if (a.unlocked.isValid()) o[a.key] = a.unlocked.toString(Qt::ISODate);
    QFile f(stateDir() + "/achievements.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
void Locator::unlock(const QString &key)
{
    for (Achievement &a : m_achievements) {
        if (a.key != key || a.unlocked.isValid()) continue;
        a.unlocked = QDateTime::currentDateTime();
        saveAchievements();
        emit achievementUnlocked(a.key, a.title);
        { BeaconEvent ev; ev.type = QStringLiteral("achievement"); ev.text = QStringLiteral("%1 %2 — %3").arg(a.icon, a.title, a.desc); logEvent(ev); }
        if (m_notifyAchievements) notify(QStringLiteral("%1 %2").arg(a.icon, a.title), a.desc, QStringLiteral("games-highscores"));
        emit statusMessage(QStringLiteral("Milestone: %1 — %2").arg(a.title, a.desc));
    }
}

void Locator::checkAchievements()
{
    if (m_standalone) return;
    const Stats st = stats();
    const QDateTime now = QDateTime::currentDateTime();
    bool anyPrecise = false, anyWifi = false, anyGps = false, sharp = false, nightOwl = false, earlyBird = false, high = false, sea = false, longHaul = false, homebody = false;
    for (const Stop &s : stops()) {
        const Fix &f = s.fix;
        if (!f.precise()) continue;
        anyPrecise = true;
        if (f.source == QLatin1String("wifi")) anyWifi = true;
        if (f.source == QLatin1String("starlink")) anyGps = true;
        if (f.accuracy >= 0 && f.accuracy <= 30) sharp = true;
        const int h = f.time.toLocalTime().time().hour();
        if (h < 4) nightOwl = true;
        const SunTimes sun = sunTimes(f.lat, f.lon, f.time);
        if (sun.sunrise.isValid() && f.time < sun.sunrise && f.time > sun.sunrise.addSecs(-2 * 3600)) earlyBird = true;
        if (f.hasElevation() && f.elevation >= 2000) high = true;
        if (f.hasElevation() && std::abs(f.elevation) <= 5) sea = true;
        if (s.legKm >= 500 && s.legSecs > 0 && s.legSecs <= 86400) longHaul = true;
        if (s.dwellSecs >= 7 * 86400) homebody = true;
    }
    if (anyPrecise) unlock(QStringLiteral("first_fix"));
    if (anyWifi) unlock(QStringLiteral("first_wifi"));
    if (anyGps) unlock(QStringLiteral("first_gps"));
    if (sharp) unlock(QStringLiteral("sharp_fix"));
    if (nightOwl) unlock(QStringLiteral("night_owl"));
    if (earlyBird) unlock(QStringLiteral("early_bird"));
    if (high) unlock(QStringLiteral("high_ground"));
    if (sea) unlock(QStringLiteral("sea_level"));
    if (longHaul) unlock(QStringLiteral("long_haul"));
    if (homebody) unlock(QStringLiteral("homebody"));
    bool wigle = false, located = false;
    for (const ApRecord &r : m_apRecords) if (r.wigle) { wigle = true; break; }
    for (const AccessPoint &ap : m_aps) { const ApEstimate::Kind k = estimateFor(ap).kind; if (k == ApEstimate::Centroid || k == ApEstimate::Trilat) { located = true; break; } }
    if (wigle) unlock(QStringLiteral("first_wigle"));
    if (located) unlock(QStringLiteral("first_located"));
    const int fitted = refitCount();
    if (fitted >= 1) unlock(QStringLiteral("first_trilat"));
    if (fitted >= 100) unlock(QStringLiteral("trilat_100"));
    if (st.beaconsTotal >= 100) unlock(QStringLiteral("beacons_100"));
    if (st.beaconsTotal >= 1000) unlock(QStringLiteral("beacons_1000"));
    if (st.beaconsTotal >= 5000) unlock(QStringLiteral("beacons_5000"));
    if (st.stops >= 10) unlock(QStringLiteral("stops_10"));
    if (st.stops >= 50) unlock(QStringLiteral("stops_50"));
    if (st.regions.size() >= 5) unlock(QStringLiteral("regions_5"));
    if (st.regions.size() >= 10) unlock(QStringLiteral("regions_10"));
    if (st.countries.size() >= 2) unlock(QStringLiteral("countries_2"));
    if (st.distanceAllKm >= 1000) unlock(QStringLiteral("km_1000"));
    if (st.distanceAllKm >= 10000) unlock(QStringLiteral("km_10000"));
    if (m_prefetchedTiles > 0) unlock(QStringLiteral("offline_ready"));
    if (st.rankLevel >= rankLadder().size()) unlock(QStringLiteral("lighthouse"));
    Q_UNUSED(now);
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
    if (!m_standalone && m_identity && !m_identity->exists() && !QSettings().value("identityNudged", false).toBool()) {
        QSettings().setValue("identityNudged", true);
        notify(QStringLiteral("Create or import your BeaconFix identity"),
               QStringLiteral("An identity lets your phone and laptop share one map and sign in without pairing codes. Open BeaconFix → Identity."),
               QStringLiteral("user-identity"));
    }
    m_timer.start();
    if (m_liveScanSecs > 0 && !m_standalone) m_liveTimer.start();
    Refresh();
    refreshPois();
}

void Locator::setLiveScanSeconds(int s)
{
    m_liveScanSecs = qBound(0, s, 3600);
    QSettings().setValue("liveScanSeconds", m_liveScanSecs);
    m_liveTimer.setInterval(qMax(15, m_liveScanSecs) * 1000);
    if (m_liveScanSecs > 0 && !m_standalone && m_timer.isActive()) m_liveTimer.start(); else m_liveTimer.stop();
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
void Locator::setUseApple(bool b)           { m_useApple = b; QSettings().setValue("useApple", b); }
void Locator::setIgnoreActiveAp(bool b)     { m_ignoreActive = b; QSettings().setValue("ignoreActiveAp", b); }
void Locator::setIgnorePatterns(const QStringList &l)
{
    m_ignore.clear();
    for (const QString &p : l) { const QString t = p.trimmed(); if (!t.isEmpty()) m_ignore << t; }
    rebuildPatternCaches();
    QSettings().setValue("ignorePatterns", m_ignore);
    emit scanUpdated();
}
void Locator::addIgnorePattern(const QString &p) { QStringList l = m_ignore; l << p; setIgnorePatterns(l); }

// ── Home networks ─────────────────────────────────────────────────────────────
void Locator::setHomeNetworks(const QStringList &l)
{
    QStringList clean;
    for (const QString &p : l) { const QString t = p.trimmed(); if (!t.isEmpty() && !clean.contains(t, Qt::CaseInsensitive)) clean << t; }
    if (clean == m_home) return;
    m_home = clean;
    rebuildPatternCaches();
    QSettings().setValue("homeNetworks", m_home);
    emit scanUpdated();
    emit FixChanged();
}
void Locator::addHomeNetwork(const QString &p) { QStringList l = m_home; l << p; setHomeNetworks(l); }
void Locator::removeHomeNetwork(const QString &p)
{
    QStringList l;
    for (const QString &x : m_home) if (x.compare(p.trimmed(), Qt::CaseInsensitive) != 0) l << x;
    setHomeNetworks(l);
}
// The UniFi export: {"ssids":[…], "bssids":[{"bssid","ssid",…}], "patterns":[…]}. Patterns are
// merged as-is; every listed BSSID becomes an exact home BSSID too.
QStringList Locator::parseHomeNetworksFile(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = f.errorString(); return {}; }
    QJsonParseError pe;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &pe).object();
    if (pe.error != QJsonParseError::NoError) { if (error) *error = pe.errorString(); return {}; }
    QStringList out;
    auto add = [&out](const QString &v) { const QString t = v.trimmed(); if (!t.isEmpty() && !out.contains(t, Qt::CaseInsensitive)) out << t; };
    for (const QJsonValue &v : o["patterns"].toArray()) add(v.toString());
    for (const QJsonValue &v : o["ssids"].toArray()) add(v.toString());
    for (const QJsonValue &v : o["bssids"].toArray()) add(v.isObject() ? v.toObject()["bssid"].toString().toUpper() : v.toString().toUpper());
    if (out.isEmpty() && error) *error = QStringLiteral("no patterns, ssids or bssids in the file");
    return out;
}
int Locator::importHomeNetworks(const QString &path, QString *error)
{
    const QStringList l = parseHomeNetworksFile(path, error);
    if (l.isEmpty()) return 0;
    QStringList merged = m_home; int added = 0;
    for (const QString &p : l) if (!merged.contains(p, Qt::CaseInsensitive)) { merged << p; ++added; }
    setHomeNetworks(merged);
    return added;
}
bool Locator::isHome(const AccessPoint &ap) const
{
    for (const QRegularExpression &re : m_homeRe)
        if (re.match(ap.bssid).hasMatch() || (!ap.ssid.isEmpty() && re.match(ap.ssid).hasMatch())) return true;
    return false;
}
bool Locator::atHome() const
{
    for (const AccessPoint &ap : m_aps) if (isHome(ap)) return true;
    return false;
}
// Candidates for "this is my own network": the connected AP's SSID, plus the loudest APs
// whose BSSID shares the first five octets with it (one router, several radios/SSIDs).
QStringList Locator::suggestHomeNetworks() const
{
    QStringList out;
    QString family;
    for (const AccessPoint &ap : m_aps) if (ap.active) { family = ap.bssid.left(14); break; }
    if (family.isEmpty()) {                       // not connected: take the loudest AP as the anchor
        int best = -200;
        for (const AccessPoint &ap : m_aps) if (ap.dbm > best) { best = ap.dbm; family = ap.bssid.left(14); }
    }
    if (family.isEmpty()) return out;
    for (const AccessPoint &ap : m_aps) {
        if (ap.bssid.left(14).compare(family, Qt::CaseInsensitive) != 0 || ap.dbm < -75) continue;
        if (!ap.ssid.isEmpty() && !out.contains(ap.ssid)) out << ap.ssid;
    }
    // Same router family, any last octet / any locally-administered variant of the second octet
    QString pat = family + QStringLiteral(":??");
    pat[1] = QLatin1Char('?'); pat[10] = QLatin1Char('?');
    out << pat;
    return out;
}

void Locator::setTravelling(const QString &bssid, bool travelling)
{
    if (travelling) { m_travelling.insert(bssid); m_notTravelling.remove(bssid); }
    else            { m_travelling.remove(bssid); m_notTravelling.insert(bssid); }
    saveApRecords();
    emit scanUpdated();
}

void Locator::setPoiRadiusKm(int km)
{
    m_poiRadiusKm = qBound(1, km, 30);
    QSettings().setValue("poiRadiusKm", m_poiRadiusKm);
    refreshPois(true);
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
    if (m_pins.contains(bssid.toUpper())) return false;                 // anchored: where it is is known
    if (m_travelling.contains(bssid)) return true;
    const auto it = m_apRecords.constFind(bssid);
    if (it == m_apRecords.constEnd()) return false;
    if (it->cells.size() >= 2) return true;                          // seen at two precise stops ≥ ~5 km apart
    // Heard at two places further apart than both fixes' error allows (works with IP fixes too)
    QList<ApSighting> places = it->seen;
    if (!it->obs.isEmpty()) places.append({it->obs.last().lat, it->obs.last().lon, it->obs.last().acc, it->obs.last().time});
    for (int i = 0; i < places.size(); ++i)
        for (int j = i + 1; j < places.size(); ++j) {
            const ApSighting &a = places[i], &b = places[j];
            if (distanceM(a.lat, a.lon, b.lat, b.lon) > a.acc + b.acc + 3000) return true;
        }
    return false;
}

// Phone hotspots, car Wi-Fi, dashcams… by name: these follow someone around
bool Locator::looksMobile(const QString &ssid)
{
    static const QRegularExpression re(QStringLiteral(
        "iphone|ipad|galaxy|pixel|android|hotspot|mifi|jetpack|oneplus|redmi|xiaomi|huawei|motorola|moto ?[egz]|"
        "tesla|uconnect|onstar|fordpass|sync ?\\d|\\bmy ?car\\b|gopro|dashcam|viofo|nextbase|\\bcar ?wifi\\b|"
        "starlink ?mini|\\bmobile ?wifi\\b|verizon_mifi|coolpad|franklin|inseego|netgear ?nighthawk ?m"),
        QRegularExpression::CaseInsensitiveOption);
    return !ssid.isEmpty() && re.match(ssid).hasMatch();
}

const ApRecord *Locator::record(const QString &bssid) const
{
    const auto it = m_apRecords.constFind(bssid);
    return it == m_apRecords.constEnd() ? nullptr : &*it;
}

// Log-distance path loss, band-aware. Reference RSSI at 1 m ≈ 20 dBm EIRP minus
// free-space loss at the band (2.4 GHz ≈ 40 dB, 5 GHz ≈ 46, 6 GHz ≈ 48) and a
// few dB for walls/vehicles; exponent 2.4 for campground / suburban clutter.
// (2.4 GHz: -55 dBm ≈ 18 m, -70 ≈ 75 m, -85 ≈ 320 m. 5 GHz: ~0.55× that.)
double Locator::rssiDistanceM(int dbm, int freqMHz)
{
    const double p1m = freqMHz >= 5925 ? -32.0 : freqMHz >= 4900 ? -30.0 : -24.0;
    return qBound(3.0, std::pow(10.0, (p1m - dbm) / 24.0), 800.0);
}

double Locator::bearingDeg(double lat1, double lon1, double lat2, double lon2)
{
    const double d2r = M_PI / 180.0;
    const double y = std::sin((lon2 - lon1) * d2r) * std::cos(lat2 * d2r);
    const double x = std::cos(lat1 * d2r) * std::sin(lat2 * d2r) - std::sin(lat1 * d2r) * std::cos(lat2 * d2r) * std::cos((lon2 - lon1) * d2r);
    return std::fmod(qRadiansToDegrees(std::atan2(y, x)) + 360.0, 360.0);
}

ApEstimate Locator::estimateFor(const AccessPoint &ap) const
{
    ApEstimate e;
    if (const BfAnchor *a = pinnedAnchor(ap.bssid)) {  // surveyed: never estimated (docs/RANGING.md §4.3.2)
        e.kind = ApEstimate::Anchor; e.lat = a->lat; e.lon = a->lon; e.radiusM = a->accM;
        return e;
    }
    const QString st = apStatus(ap);
    // Our own gear rides along: wherever it was "located" before is meaningless now
    const ApRecord *r = st == QLatin1String("travelling") || st == QLatin1String("active") ? nullptr : record(ap.bssid);
    // Our fit: a fix (graded A–F; "none" = a fit from before 3.9 still waiting for its recompute), a region (R) or mobile (M)
    const bool fit = r && r->fit.valid && r->fit.quality != QLatin1String("none")
                     && (r->fit.kind == QLatin1String("fix") || r->fit.kind == QLatin1String("none"));
    const bool region = r && r->fit.valid && r->fit.kind == QLatin1String("region");
    if (r && r->fit.kind == QLatin1String("mobile") && !r->wigle) {
        e.kind = ApEstimate::Mobile; e.fit = r->fit; e.lat = r->fit.lat; e.lon = r->fit.lon; e.radiusM = 0;
        if (!r->obs.isEmpty()) { e.lat = r->obs.last().lat; e.lon = r->obs.last().lon; }
        return e;
    }
    // A placement (WiGLE / Apple, ±25 m) and our own multilateration: the tighter one is the answer,
    // and when they disagree by more than 3× their accuracy the card shows both.
    if (r && (fit || r->wigle)) {
        const bool useFit = fit && (!r->wigle || r->fit.acc <= 25.0);
        if (useFit) { e.kind = ApEstimate::Trilat; e.lat = r->fit.lat; e.lon = r->fit.lon; e.radiusM = r->fit.acc; e.vantage = r->fit.vantage; e.fit = r->fit; }
        else        { e.kind = ApEstimate::Wigle; e.lat = r->wLat; e.lon = r->wLon; e.radiusM = 25; if (fit || region) e.fit = r->fit; }
        if (fit && r->wigle) {
            const double d = distanceM(r->fit.lat, r->fit.lon, r->wLat, r->wLon);
            if (d > 3.0 * qMax(25.0, r->fit.acc)) {
                e.hasAlt = true;
                if (useFit) { e.altKind = ApEstimate::Wigle; e.altLat = r->wLat; e.altLon = r->wLon; e.altAcc = 25; }
                else        { e.altKind = ApEstimate::Trilat; e.altLat = r->fit.lat; e.altLon = r->fit.lon; e.altAcc = r->fit.acc; }
            }
        }
        return e;
    }
    if (r && r->hasPeer() && (!region || r->peerAcc * 2.45 < r->fit.r95)) {   // another device worked it out and synced it to us
        e.kind = ApEstimate::Peer; e.lat = r->peerLat; e.lon = r->peerLon; e.radiusM = r->peerAcc;
        if (region) e.fit = r->fit;
        return e;
    }
    if (region) {                                       // our own answer, but only an area (grade R): a disc of radius R95
        e.kind = ApEstimate::Region; e.lat = r->fit.lat; e.lon = r->fit.lon; e.radiusM = r->fit.r95; e.vantage = r->fit.vantage; e.fit = r->fit;
        return e;
    }
    // Two vantage points only (the engine wants three): the old signal-weighted centroid, honestly wide.
    // Observations whose fixes overlap (a parked rig, BeaconDB jitter) are merged into one vantage point.
    if (r && r->obs.size() >= 2) {
        struct V { double lat, lon, acc, dbmSum; int n; };
        QList<V> vs;
        for (const ApObservation &o : r->obs) {
            bool merged = false;
            for (V &v : vs) {
                if (distanceM(v.lat, v.lon, o.lat, o.lon) < qMax(80.0, v.acc + o.acc)) {
                    v.lat = (v.lat * v.n + o.lat) / (v.n + 1); v.lon = (v.lon * v.n + o.lon) / (v.n + 1);
                    v.acc = qMin(v.acc, o.acc); v.dbmSum += o.dbm; ++v.n; merged = true; break;
                }
            }
            if (!merged) vs.append({o.lat, o.lon, o.acc, double(o.dbm), 1});
        }
        if (vs.size() >= 2) {
            double sw = 0, sl = 0, so = 0, meanAcc = 0;
            for (const V &v : vs) { const double w = std::pow(qMax(1.0, v.dbmSum / v.n + 100.0), 2.0) / qMax(10.0, v.acc); sw += w; sl += w * v.lat; so += w * v.lon; meanAcc += v.acc / vs.size(); }
            e.kind = ApEstimate::Centroid; e.vantage = vs.size(); e.lat = sl / sw; e.lon = so / sw;
            double spread = 0;
            for (const V &v : vs) { const double w = std::pow(qMax(1.0, v.dbmSum / v.n + 100.0), 2.0) / qMax(10.0, v.acc), d = distanceM(e.lat, e.lon, v.lat, v.lon); spread += w * d * d; }
            e.radiusM = qBound(30.0, std::sqrt(spread / sw) + meanAcc, 600.0);
            return e;
        }
    }
    if (r && !r->obs.isEmpty()) {                       // one place we heard it: the internal map's "observed" position
        const ApObservation &o = r->obs.last();
        e.kind = ApEstimate::Observed; e.lat = o.lat; e.lon = o.lon; e.vantage = 1;
        e.radiusM = qBound(20.0, o.acc + rssiDistanceM(o.dbm, ap.frequency ? ap.frequency : r->freq), 900.0);
        return e;
    }
    if (!m_fix.valid) return e;
    e.kind = ApEstimate::Ring; e.lat = m_fix.lat; e.lon = m_fix.lon;
    e.radiusM = rssiDistanceM(ap.dbm, ap.frequency);
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
        if (k == ApEstimate::Centroid || k == ApEstimate::Wigle || k == ApEstimate::Observed || k == ApEstimate::Trilat || k == ApEstimate::Peer || k == ApEstimate::Region) ++st.locatedNow;
    }
    for (const Fix &f : m_history)
        if (f.accuracy >= 0 && (st.bestAccuracy < 0 || f.accuracy < st.bestAccuracy)) st.bestAccuracy = f.accuracy;
    const QList<RankTier> &ranks = rankLadder();
    st.rankCount = ranks.size();
    for (int i = 0; i < ranks.size(); ++i) {
        if (st.beaconsTotal >= ranks[i].at) { st.rank = QString::fromLatin1(ranks[i].name); st.rankLevel = i + 1; st.rankAt = ranks[i].at; st.nextRankAt = i + 1 < ranks.size() ? ranks[i+1].at : 0; }
    }

    // ── Trip intelligence ──
    const QDateTime now = QDateTime::currentDateTime();
    const QDate today = now.toLocalTime().date();
    // Trip start: explicit, else the last gap of a week or more between stops, else the first stop
    QDateTime tripStart = m_tripStart;
    if (!tripStart.isValid() && !m_history.isEmpty()) {
        tripStart = m_history.first().time;
        for (int i = 1; i < m_history.size(); ++i)
            if (m_history[i-1].time.daysTo(m_history[i].time) >= 7) tripStart = m_history[i].time;
    }
    st.tripStart = tripStart;
    if (tripStart.isValid()) st.tripDays = int(tripStart.daysTo(now)) + 1;
    const QList<Stop> sl = stops();
    int lastPrecise = -1;
    for (int i = 0; i < sl.size(); ++i) {
        const Stop &s = sl[i];
        const bool inTrip = tripStart.isValid() && s.fix.time >= tripStart;
        if (inTrip) ++st.stopsTrip;
        if (s.fix.time.toLocalTime().date() == today) ++st.stopsToday;
        if (s.dwellSecs > 0) { st.stoppedSecs += s.dwellSecs; if (inTrip) st.stoppedTripSecs += s.dwellSecs; }
        if (s.legSecs > 0) { st.movingSecs += s.legSecs; if (inTrip) st.movingTripSecs += s.legSecs; }
        if (s.legKm > 0) {
            st.distanceAllKm += s.legKm;
            if (inTrip) st.distanceTripKm += s.legKm;
            // A leg counts for "today" only if we also left the previous stop today —
            // a nine-day drive that ends this afternoon is trip distance, not today's.
            if (s.fix.time.toLocalTime().date() == today && lastPrecise >= 0) {
                const Fix &p = sl[lastPrecise].fix;
                const QDateTime dep = p.departed.isValid() ? p.departed : p.time;
                if (dep.toLocalTime().date() == today) st.distanceTodayKm += s.legKm;
            }
            st.longestLegKm = qMax(st.longestLegKm, s.legKm);
        }
        if (s.dwellSecs > st.longestStaySecs && s.fix.precise()) { st.longestStaySecs = s.dwellSecs; st.longestStayPlace = s.fix.place; }
        if (s.fix.precise()) {
            QString city, region, country;
            parsePlace(s.fix, &city, &region, &country);
            if (!city.isEmpty() && !st.cities.contains(city)) st.cities << city;
            if (!region.isEmpty() && !st.regions.contains(region)) st.regions << region;
            if (!country.isEmpty() && !st.countries.contains(country)) st.countries << country;
            if (lastPrecise >= 0) {
                const Fix &p = sl[lastPrecise].fix;
                st.headingDeg = bearingDeg(p.lat, p.lon, s.fix.lat, s.fix.lon);
                const qint64 secs = s.legSecs > 0 ? s.legSecs : p.time.secsTo(s.fix.time);
                // Average speed over a leg only means something for a leg of a few hours;
                // a multi-day gap between confirmations is not "10 km/h".
                st.speedKmh = (secs > 0 && secs <= 6 * 3600) ? s.legKm / (secs / 3600.0) : -1;
            }
            lastPrecise = i;
        }
    }
    if (!sl.isEmpty()) {
        st.dwellSecs = sl.last().dwellSecs;
        // "Moving" = the latest stop is precise, was reached within the last 15 min, and the
        // leg into it was short enough that the speed is real (not fix jitter, not a 9-day gap).
        st.moving = lastPrecise == sl.size() - 1 && st.speedKmh >= 0 && sl.last().legSecs > 0
                    && sl.last().legSecs <= 6 * 3600 && sl.last().fix.time.secsTo(now) < 900;
    }
    if (!st.moving) st.speedKmh = st.speedKmh >= 0 ? st.speedKmh : -1;
    st.achievementsTotal = m_achievements.size();
    for (const Achievement &a : m_achievements) if (a.unlocked.isValid()) ++st.achievementsUnlocked;
    // Home
    st.atHome = atHome();
    st.homeKnown = m_homeFix.valid;
    if (m_homeFix.valid) { st.homeLat = m_homeFix.lat; st.homeLon = m_homeFix.lon; st.homeTime = m_homeFix.time; }
    if (m_home.isEmpty()) st.awayText.clear();
    else if (st.atHome) { st.awayKm = 0; st.awayText = QStringLiteral("at the RV"); }
    else if (m_homeFix.valid && m_fix.valid) {
        st.awayKm = distanceM(m_homeFix.lat, m_homeFix.lon, m_fix.lat, m_fix.lon) / 1000.0;
        st.awayText = st.awayKm < 0.3 ? QStringLiteral("next to the RV")
                    : QStringLiteral("%1 km %2 of the RV").arg(st.awayKm, 0, 'f', st.awayKm < 10 ? 1 : 0).arg(compass(bearingDeg(m_homeFix.lat, m_homeFix.lon, m_fix.lat, m_fix.lon)));
    } else st.awayText = QStringLiteral("away from the RV (its position is not known yet)");
    return st;
}

void Locator::rebuildPatternCaches()
{
    m_homeRe.clear(); m_ignoreRe.clear();
    for (const QString &pat : m_home) m_homeRe << QRegularExpression(QRegularExpression::wildcardToRegularExpression(pat), QRegularExpression::CaseInsensitiveOption);
    for (const QString &pat : m_ignore) m_ignoreRe << QRegularExpression(QRegularExpression::wildcardToRegularExpression(pat), QRegularExpression::CaseInsensitiveOption);
}
bool Locator::matchesIgnore(const AccessPoint &ap) const
{
    for (const QRegularExpression &re : m_ignoreRe)
        if (re.match(ap.bssid).hasMatch() || re.match(ap.ssid).hasMatch())
            return true;
    return false;
}

QString Locator::apStatus(const AccessPoint &ap) const
{
    if (ap.ssid.endsWith(QLatin1String("_nomap")) || ap.ssid.contains(QLatin1String("_optout")))
        return QStringLiteral("nomap");
    if (isHome(ap))                      return QStringLiteral("home");
    if (matchesIgnore(ap))               return QStringLiteral("ignored");
    if (isTravelling(ap.bssid))          return QStringLiteral("travelling");
    if (!m_notTravelling.contains(ap.bssid) && looksMobile(ap.ssid)) return QStringLiteral("travelling");
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
    else { emit statusMessage(QStringLiteral("Scanning Wi-Fi…")); startProbeScan(); }
}

void Locator::ShowWindow() { emit showWindowRequested(); }

QJsonArray Locator::apsJson() const
{
    QJsonArray aps;
    for (const AccessPoint &ap : m_aps) {
        const ApEstimate e = estimateFor(ap);
        QJsonObject a;
        a["bssid"] = ap.bssid; a["ssid"] = ap.ssid; a["dbm"] = ap.dbm; a["freq"] = ap.frequency;
        a["status"] = apStatus(ap);
        a["kind"] = QLatin1String(ApEstimate::kindName(e.kind));
        a["lat"] = e.lat; a["lon"] = e.lon; a["r"] = e.radiusM; a["bearing"] = e.bearingDeg;
        if (e.vantage) a["vantage"] = e.vantage;
        if (e.fit.valid || e.fit.kind == QLatin1String("mobile")) {
            a["fit"] = fitJson(e.fit);
            if (!e.fit.grade.isEmpty()) { a["grade"] = e.fit.grade; a["score"] = std::round(e.fit.score * 10) / 10; }
            if (e.fit.r95 > 0) a["r95"] = e.fit.r95;
        }
        if (e.hasAlt) a["alt"] = QJsonObject{{"kind", QLatin1String(ApEstimate::kindName(e.altKind))}, {"lat", e.altLat}, {"lon", e.altLon}, {"acc", e.altAcc}};
        if (const ApRecord *rr = record(ap.bssid)) { a["samples"] = rr->obs.size(); if (rr->hasPeer()) a["peer"] = QJsonObject{{"from", rr->peerFrom}, {"lat", rr->peerLat}, {"lon", rr->peerLon}, {"acc", rr->peerAcc}}; }
        const int f = ap.frequency;
        a["band"] = f >= 5925 ? QStringLiteral("6") : f >= 4900 ? QStringLiteral("5") : QStringLiteral("2.4");
        a["ch"] = f >= 5925 ? (f - 5950) / 5 : f >= 4900 ? (f - 5000) / 5 : f == 2484 ? 14 : f > 2400 ? (f - 2407) / 5 : 0;
        a["security"] = ap.security; a["secFlags"] = ap.secFlags; a["wpaFlags"] = ap.wpaFlags; a["rsnFlags"] = ap.rsnFlags; a["maxKbps"] = ap.maxKbps; a["adhoc"] = ap.adhoc;
        a["insecure"] = AccessPoint::insecure(ap.security);
        if (isHome(ap)) { a["home"] = true; const QString hs = m_homeSsids.value(ap.bssid.toUpper()); if (!hs.isEmpty()) a["homeSsid"] = hs; }
        aps.append(a);
    }
    return aps;
}

QString Locator::StateJson() const
{
    QJsonObject o = m_fix.toJson();
    o["error"] = m_lastError;
    o["note"] = m_coarseNote;
    o["busy"] = m_busy;
    o["intervalMinutes"] = m_intervalMin;
    o["wifiInterface"] = m_scanner.interfaceName();
    const QJsonArray aps = apsJson();
    o["aps"] = aps;
    {
        int open = 0, wep = 0, wpa1 = 0, tkip = 0, wpa3 = 0;
        for (const AccessPoint &ap : m_aps) {
            if (ap.security == QLatin1String("open")) ++open; else if (ap.security == QLatin1String("wep")) ++wep; else if (ap.security == QLatin1String("wpa1")) ++wpa1;
            else if (ap.security == QLatin1String("wpa2-tkip")) ++tkip; else if (ap.security.startsWith(QLatin1String("wpa3")) || ap.security == QLatin1String("wpa2/3")) ++wpa3;
        }
        o["securitySummary"] = QJsonObject{{"open", open}, {"wep", wep}, {"wpa1", wpa1}, {"tkip", tkip}, {"wpa3", wpa3}, {"total", m_aps.size()}};
    }
    if (m_api) {
        QJsonArray kd;
        for (const QJsonValue &v : m_api->knownJson()["devices"].toArray()) {
            const QJsonObject d = v.toObject();
            kd.append(QJsonObject{{"mac", d["mac"]}, {"name", d["name"]}, {"ip", d["fixed_ip"].isString() ? d["fixed_ip"] : d["ip"]}, {"network", d["network"]},
                                  {"online", d["online"]}, {"wired", d["wired"]}, {"ours", d["ours"]}, {"last_seen", d["last_seen"]}});
        }
        o["knownDevices"] = kd;
    }
    QJsonArray events;
    for (const BeaconEvent &e : m_events) events.append(e.toJson());
    o["events"] = events;
    o["linkedDevices"] = linkedDevices();
    o["anchors"] = anchorsJson();
    o["features"] = QJsonArray::fromStringList(features());
    o["environment"] = environmentJson();
    o["lastEventId"] = m_eventId;
    o["liveScanSeconds"] = m_liveScanSecs;
    QJsonArray pois;
    for (const Poi &pt : m_allPois) {
        const PoiCategory *c = poiCategory(pt.cat);
        QJsonObject a;
        a["cat"] = pt.cat; a["name"] = pt.name; a["lat"] = pt.lat; a["lon"] = pt.lon;
        a["icon"] = c ? c->icon : QString(); a["color"] = c ? c->color.name() : QStringLiteral("#9fb0c8");
        a["label"] = c ? c->label : pt.cat; a["detail"] = pt.detail;
        if (pt.wifi) a["wifi"] = true;
        if (!pt.hours.isEmpty()) a["hours"] = pt.hours;
        if (!pt.phone.isEmpty()) a["phone"] = pt.phone;
        if (!pt.website.isEmpty()) a["website"] = pt.website;
        if (!pt.address.isEmpty()) a["address"] = pt.address;
        if (!pt.wheelchair.isEmpty()) a["wheelchair"] = pt.wheelchair;
        if (pt.emergency) a["emergency"] = true;
        a["group"] = c ? c->group : QStringLiteral("services");
        a["osm"] = QStringLiteral("https://www.openstreetmap.org/%1/%2").arg(pt.osmType).arg(pt.osmId);
        if (!pt.osmType.isEmpty()) a["osmType"] = pt.osmType;
        if (pt.osmId) a["osmId"] = double(pt.osmId);
        if (pt.peds) a["peds"] = pt.peds;
        if (!pt.er.isEmpty()) a["er"] = pt.er;
        if (!pt.campus.isEmpty()) a["campusEr"] = pt.campus;
        if (pt.scope != QLatin1String("near")) a["scope"] = pt.scope;
        if (pt.driveS > 0) { a["driveS"] = pt.driveS; a["driveM"] = pt.driveM; a["driveEst"] = pt.driveEst; }
        if (m_fix.valid) {
            a["d"] = qRound(distanceM(m_fix.lat, m_fix.lon, pt.lat, pt.lon));
            a["brg"] = qRound(bearingDeg(m_fix.lat, m_fix.lon, pt.lat, pt.lon));
        }
        pois.append(a);
    }
    o["pois"] = pois;
    o["poiNote"] = m_poiNote;
    o["tileBase"] = m_tileBase;
    QJsonArray cats;
    for (const PoiCategory &c : poiCategories())
        cats.append(QJsonObject{{"key", c.key}, {"label", c.label}, {"icon", c.icon}, {"color", c.color.name()}, {"group", c.group}, {"groupLabel", poiGroupLabel(c.group)}, {"wide", c.wide}, {"reachKm", c.reachKm}});
    o["poiCategories"] = cats;
    o["emergency"] = emergencyJson();
    o["pedsNote"] = o["emergency"].toObject()["pediatricNote"];
    o["pedsTime"] = m_pedsTime.isValid() ? m_pedsTime.toString(Qt::ISODate) : QString();
    o["pedsRadiusKm"] = m_pedsRadiusKm;
    QJsonArray track;
    for (const Stop &s : stops()) {
        const Fix &f = s.fix;
        QJsonObject t{{"lat", f.lat}, {"lon", f.lon}, {"acc", f.accuracy}, {"source", f.source},
                      {"place", f.place}, {"time", f.time.toString(Qt::ISODate)}};
        if (f.departed.isValid()) t["departed"] = f.departed.toString(Qt::ISODate);
        if (s.dwellSecs >= 0) t["dwellSecs"] = s.dwellSecs;
        if (s.legKm >= 0) t["legKm"] = s.legKm;
        if (s.legSecs >= 0) t["legSecs"] = s.legSecs;
        if (f.hasElevation()) t["elev"] = f.elevation;
        if (!f.city.isEmpty()) t["city"] = f.city;
        if (!f.region.isEmpty()) t["region"] = f.region;
        if (!f.country.isEmpty()) t["country"] = f.country;
        track.append(t);
    }
    o["track"] = track;
    if (m_fix.hasElevation()) o["elevation"] = m_fix.elevation; else o["elevation"] = QJsonValue();
    o["elevationNote"] = m_elevNote;
    const SunTimes su = sun();
    if (su.valid) {
        auto iso = [](const QDateTime &d) { return d.isValid() ? QJsonValue(d.toString(Qt::ISODate)) : QJsonValue(); };
        o["sun"] = QJsonObject{{"sunrise", iso(su.sunrise)}, {"sunset", iso(su.sunset)}, {"solarNoon", iso(su.solarNoon)},
                               {"goldenMorningEnd", iso(su.goldenMorningEnd)}, {"goldenEveningStart", iso(su.goldenEveningStart)},
                               {"civilDawn", iso(su.civilDawn)}, {"civilDusk", iso(su.civilDusk)}, {"dayLengthSecs", su.dayLengthSecs},
                               {"isDay", su.isDay(QDateTime::currentDateTime())}, {"polarDay", su.polarDay}, {"polarNight", su.polarNight}};
    }
    if (m_fix.valid)
        o["share"] = QJsonObject{{"coords", coordsText()}, {"geo", geoUri()}, {"osm", osmUrl()}, {"google", googleMapsUrl()}, {"apple", appleMapsUrl()}, {"text", shareText()}};
    const Stats st = stats();
    QJsonObject sj;
    sj["stops"] = st.stops; sj["distanceKm"] = st.distanceKm; sj["beaconsTotal"] = st.beaconsTotal;
    sj["beaconsNow"] = st.beaconsNow; sj["usedNow"] = st.usedNow; sj["travellingNow"] = st.travellingNow;
    sj["locatedNow"] = st.locatedNow; sj["bestAccuracy"] = st.bestAccuracy;
    sj["fitted"] = refitCount();
    sj["rank"] = st.rank; sj["rankLevel"] = st.rankLevel; sj["nextRankAt"] = st.nextRankAt; sj["rankAt"] = st.rankAt; sj["rankCount"] = st.rankCount;
    QJsonArray ladder;
    for (const RankTier &r : rankLadder()) ladder.append(QJsonObject{{"at", r.at}, {"name", QString::fromLatin1(r.name)}});
    sj["rankLadder"] = ladder;
    sj["distanceTodayKm"] = st.distanceTodayKm; sj["distanceTripKm"] = st.distanceTripKm; sj["distanceAllKm"] = st.distanceAllKm;
    sj["movingSecs"] = double(st.movingSecs); sj["stoppedSecs"] = double(st.stoppedSecs);
    sj["movingTripSecs"] = double(st.movingTripSecs); sj["stoppedTripSecs"] = double(st.stoppedTripSecs);
    sj["speedKmh"] = st.speedKmh; sj["headingDeg"] = st.headingDeg; sj["heading"] = compass(st.headingDeg); sj["moving"] = st.moving;
    sj["atHome"] = st.atHome; sj["awayKm"] = st.awayKm; sj["awayText"] = st.awayText; sj["homeNetworks"] = QJsonArray::fromStringList(m_home);
    if (m_os) { sj["locale"] = m_os->locale().toJson(); sj["timezone"] = m_os->lastZone().isEmpty() ? m_os->systemTimeZone() : m_os->lastZone(); sj["os"] = m_os->status(); }
    if (m_identity && m_identity->exists()) sj["identity"] = QJsonObject{{"id", m_identity->id()}, {"grouped", m_identity->groupedId()}, {"name", m_identity->name()}, {"linked", m_identity->linkedIds().size()}, {"pendingLinks", m_identity->pending().size()}};
    sj["homeFix"] = st.homeKnown ? QJsonValue(QJsonObject{{"lat", st.homeLat}, {"lon", st.homeLon}, {"time", st.homeTime.toString(Qt::ISODate)}}) : QJsonValue();
    sj["dwellSecs"] = double(st.dwellSecs);
    sj["tripStart"] = st.tripStart.isValid() ? QJsonValue(st.tripStart.toString(Qt::ISODate)) : QJsonValue();
    sj["tripDays"] = st.tripDays; sj["stopsTrip"] = st.stopsTrip; sj["stopsToday"] = st.stopsToday;
    sj["cities"] = QJsonArray::fromStringList(st.cities); sj["regions"] = QJsonArray::fromStringList(st.regions); sj["countries"] = QJsonArray::fromStringList(st.countries);
    sj["longestLegKm"] = st.longestLegKm; sj["longestStaySecs"] = double(st.longestStaySecs); sj["longestStayPlace"] = st.longestStayPlace;
    sj["achievementsUnlocked"] = st.achievementsUnlocked; sj["achievementsTotal"] = st.achievementsTotal;
    QJsonArray ach;
    for (const Achievement &a : m_achievements)
        ach.append(QJsonObject{{"key", a.key}, {"title", a.title}, {"desc", a.desc}, {"icon", a.icon},
                               {"unlocked", a.unlocked.isValid()}, {"at", a.unlocked.isValid() ? QJsonValue(a.unlocked.toString(Qt::ISODate)) : QJsonValue()}});
    sj["achievements"] = ach;
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
        startProbeScan();
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
        startProbeScan();
    });
    proc->start(grpcurl, {QStringLiteral("-plaintext"), QStringLiteral("-max-time"), QStringLiteral("5"),
                          QStringLiteral("-d"), QStringLiteral("{\"get_location\":{}}"),
                          m_starlinkHost + ":9200", QStringLiteral("SpaceX.API.Device.Device/Handle")});
}

// ── Scans: one scanner, two callers ───────────────────────────────────────────
// The probe chain asks for a scan to geolocate; the live timer asks for one just
// to keep the beacons fresh. NetworkManager only runs one at a time, so a probe
// that arrives while a live scan is in flight simply takes that scan's result.
void Locator::startProbeScan()
{
    if (m_liveScan) { m_probeWantsScan = true; return; }
    m_scanner.scan();
}

void Locator::liveScan()
{
    if (m_busy || m_liveScan || m_standalone || m_scanner.busy()) return;
    m_liveScan = true;
    m_scanner.scan();
}

void Locator::onScanFinished(const QList<AccessPoint> &aps)
{
    diffScan(aps);
    noteAnchorCalibration(aps);
    if (m_liveScan) {
        m_liveScan = false;
        if (m_probeWantsScan) { m_probeWantsScan = false; onScan(aps); return; }
        // Live path: refresh the beacons without re-geolocating…
        QSet<QString> before, after;
        for (const AccessPoint &ap : m_aps) if (apStatus(ap) == QLatin1String("used")) before.insert(ap.bssid);
        for (const AccessPoint &ap : aps)   if (apStatus(ap) == QLatin1String("used")) after.insert(ap.bssid);
        m_aps = aps;
        if (m_fix.valid) noteSightings(m_aps, m_fix);
        // A fresh, tight fix (GPS, or a Wi-Fi fix from the last two minutes) makes every live scan a sample:
        // this is how a laptop carried around collects vantage points without re-geolocating each time
        if (m_fix.precise() && m_fix.accuracy > 0 && m_fix.accuracy <= 60 && m_fix.time.isValid() && m_fix.time.secsTo(QDateTime::currentDateTime()) <= 120) {
            QList<AccessPoint> fixed;
            for (const AccessPoint &ap : m_aps) if (!isHome(ap)) fixed << ap;
            noteObservations(fixed, m_fix);
        }
        emit scanUpdated();
        queueWigle();
        // …unless the neighbourhood changed by more than half: then we've arrived somewhere
        const int common = (before & after).size(), all = (before | after).size();
        const bool changed = all >= 2 && common * 2 < all;
        const QDateTime now = QDateTime::currentDateTime();
        if (changed && !before.isEmpty() && (!m_lastLiveProbe.isValid() || m_lastLiveProbe.secsTo(now) > 120)) {
            m_lastLiveProbe = now;
            emit statusMessage(QStringLiteral("The beacons around you changed — re-checking the position"));
            Refresh();
        }
        return;
    }
    onScan(aps);
}

// Turn two consecutive scans into events: new beacons, ones that faded (after two
// misses, so a single dropped frame doesn't flap), and level changes of ≥ 8 dB.
void Locator::diffScan(const QList<AccessPoint> &aps)
{
    QSet<QString> seen;
    bool homeHeard = false; QString homeName;
    for (const AccessPoint &ap : aps) {
        if (isHome(ap)) {                          // our own networks never "appear" or "fade"
            homeHeard = true; if (homeName.isEmpty() && !ap.ssid.isEmpty()) homeName = ap.ssid;
            continue;
        }
        seen.insert(ap.bssid);
        const auto last = m_lastDbm.constFind(ap.bssid);
        if (last == m_lastDbm.constEnd()) {
            // "Appeared" only if it is genuinely new here: not heard in the last 10 minutes
            // (weak beacons drop in and out of scans; that is noise, not an event).
            const QDateTime lastSeen = m_lastSeenAt.value(ap.bssid);
            if (m_firstScanDone && (!lastSeen.isValid() || lastSeen.secsTo(QDateTime::currentDateTime()) > 600)) {
                BeaconEvent e = apEvent(QStringLiteral("ap_new"), ap);
                e.text = QStringLiteral("%1 appeared · %2 dBm").arg(ap.ssid.isEmpty() ? QStringLiteral("(hidden)") : ap.ssid).arg(ap.dbm);
                logEvent(e);
            }
        } else if (std::abs(ap.dbm - last.value()) >= 8) {
            const int d = ap.dbm - last.value();
            BeaconEvent e = apEvent(d > 0 ? QStringLiteral("ap_up") : QStringLiteral("ap_down"), ap);
            e.delta = d;
            e.text = QStringLiteral("%1 %2%3 dB → %4 dBm").arg(ap.ssid.isEmpty() ? QStringLiteral("(hidden)") : ap.ssid, d > 0 ? QStringLiteral("+") : QString()).arg(d).arg(ap.dbm);
            logEvent(e);
        }
        m_lastDbm.insert(ap.bssid, ap.dbm);
        m_lastSeenAt.insert(ap.bssid, QDateTime::currentDateTime());
        m_missing.remove(ap.bssid);
        if (AccessPoint::insecure(ap.security)) {             // once per BSSID per day: "X is open / WEP / WPA1"
            const QDateTime noted = m_insecureNoted.value(ap.bssid);
            if (!noted.isValid() || noted.secsTo(QDateTime::currentDateTime()) > 86400) {
                m_insecureNoted.insert(ap.bssid, QDateTime::currentDateTime());
                BeaconEvent e = apEvent(QStringLiteral("ap_insecure"), ap);
                const QString how = ap.security == QLatin1String("open") ? QStringLiteral("open (no encryption)") : ap.security == QLatin1String("wep") ? QStringLiteral("WEP (broken since 2001)")
                                  : ap.security == QLatin1String("wpa1") ? QStringLiteral("WPA1 (TKIP, deprecated)") : QStringLiteral("WPA2 with TKIP (weak cipher)");
                e.text = QStringLiteral("%1 is %2").arg(ap.ssid.isEmpty() ? QStringLiteral("(hidden)") : ap.ssid, how);
                logEvent(e);
            }
        }
    }
    for (auto it = m_lastDbm.begin(); it != m_lastDbm.end();) {
        if (seen.contains(it.key())) { ++it; continue; }
        const int misses = ++m_missing[it.key()];
        if (misses < 2) { ++it; continue; }
        AccessPoint ap; ap.bssid = it.key(); ap.dbm = it.value();
        if (ap.dbm < -85) {                       // a beacon at the edge of hearing: drop it quietly
            m_missing.remove(it.key()); it = m_lastDbm.erase(it); continue;
        }
        if (const ApRecord *r = record(ap.bssid)) { ap.ssid = r->ssid; ap.frequency = r->freq; }
        BeaconEvent e = apEvent(QStringLiteral("ap_lost"), ap);
        e.text = QStringLiteral("%1 faded out").arg(ap.ssid.isEmpty() ? QStringLiteral("(hidden)") : ap.ssid);
        logEvent(e);
        m_missing.remove(it.key());
        it = m_lastDbm.erase(it);
    }
    if (m_firstScanDone && homeHeard != m_homeInRange) {
        BeaconEvent e; e.type = QStringLiteral("home");
        e.text = homeHeard ? QStringLiteral("Home network in range%1").arg(homeName.isEmpty() ? QString() : QStringLiteral(": ") + homeName)
                           : QStringLiteral("Home network out of range");
        e.ssid = homeName;
        logEvent(e);
    }
    m_homeInRange = homeHeard;
    m_firstScanDone = true;
}

BeaconEvent Locator::apEvent(const QString &type, const AccessPoint &ap) const
{
    BeaconEvent e;
    e.type = type; e.bssid = ap.bssid; e.ssid = ap.ssid; e.dbm = ap.dbm;
    e.status = apStatus(ap);
    const ApEstimate est = estimateFor(ap);
    e.kind = est.kind == ApEstimate::Wigle ? QStringLiteral("wigle") : est.kind == ApEstimate::Centroid ? QStringLiteral("centroid")
           : est.kind == ApEstimate::Observed ? QStringLiteral("observed") : est.kind == ApEstimate::Ring ? QStringLiteral("ring") : QStringLiteral("none");
    if (est.kind != ApEstimate::None) { e.hasPos = true; e.lat = est.lat; e.lon = est.lon; }
    e.security = ap.security;
    if (est.kind == ApEstimate::Ring) { e.r = est.radiusM; e.bearing = est.bearingDeg; }
    return e;
}

void Locator::logEvent(BeaconEvent e)
{
    e.id = ++m_eventId;
    if (!e.time.isValid()) e.time = QDateTime::currentDateTime();
    m_events.append(e);
    while (m_events.size() > 60) m_events.removeFirst();
    emit eventLogged(QString::fromUtf8(QJsonDocument(e.toJson()).toJson(QJsonDocument::Compact)));
}

void Locator::onScan(const QList<AccessPoint> &aps)
{
    m_aps = aps;
    emit scanUpdated();
    if (tryRvGnss()) return;                            // the Pi's averaged GNSS, ahead of BeaconDB / Apple / IP (§4.3.7)
    QList<AccessPoint> usable;
    for (const AccessPoint &ap : aps)
        if (apStatus(ap) == QLatin1String("used")) usable << ap;
    if (usable.size() < 2) {
        // BeaconDB wants two; Apple answers per BSSID, so one is enough to try
        tryApple(usable, QStringLiteral("Only %1 usable access point(s); BeaconDB needs 2").arg(usable.size()));
        return;
    }
    if (tryInternal(usable)) return;                    // somewhere we have been: no network needed
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
    connect(rep, &QNetworkReply::finished, this, [this, rep, used, seen, usable] {
        rep->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        const QJsonObject loc = o["location"].toObject();
        const double acc = o["accuracy"].toDouble(-1);
        if (rep->error() == QNetworkReply::NoError && loc.contains("lat") && acc > 5000) {
            // That's BeaconDB's GeoIP fallback (city-sized radius), not a Wi-Fi match
            tryApple(usable, QStringLiteral("BeaconDB had no match for these access points (it offered a %1 km GeoIP guess)").arg(qRound(acc / 1000)));
            return;
        }
        if (rep->error() == QNetworkReply::NoError && loc.contains("lat")) {
            Fix f; f.valid = true; f.lat = loc["lat"].toDouble(); f.lon = loc["lng"].toDouble();
            f.accuracy = acc; f.source = QStringLiteral("wifi"); f.provider = QStringLiteral("beacondb");
            f.time = QDateTime::currentDateTime(); f.apCount = seen; f.apUsed = used;
            accept(f);
            finish(true, QStringLiteral("BeaconDB fix from %1 access points (±%2 m)").arg(used).arg(qRound(f.accuracy)));
            return;
        }
        QString why = o["error"].toObject()["message"].toString();
        if (why.isEmpty()) why = rep->error() == QNetworkReply::NoError ? QStringLiteral("no location in reply") : rep->errorString();
        tryApple(usable, QStringLiteral("BeaconDB: ") + why);
    });
}

// ── Apple Wi-Fi positioning (keyless; used when BeaconDB has no match) ─────────
// gs-loc.apple.com returns the mapped position of every BSSID it knows (plus
// ~100 neighbours it volunteers). No account, no token. Request and reply are
// two tiny protobuf messages, hand-encoded here rather than pulling in a library.
static QByteArray pbVarint(quint64 v)
{
    QByteArray b;
    do { quint8 c = v & 0x7f; v >>= 7; if (v) c |= 0x80; b.append(char(c)); } while (v);
    return b;
}
static QByteArray pbBytes(int tag, const QByteArray &d) { return pbVarint(quint64(tag << 3) | 2) + pbVarint(quint64(d.size())) + d; }
static bool pbReadVarint(const QByteArray &b, int &i, quint64 &v)
{
    v = 0; int s = 0;
    while (i < b.size() && s < 64) {
        const quint8 c = quint8(b[i++]);
        v |= quint64(c & 0x7f) << s; s += 7;
        if (!(c & 0x80)) return true;
    }
    return false;
}
struct PbField { int tag = 0; int wt = 0; quint64 v = 0; QByteArray d; };
static QList<PbField> pbParse(const QByteArray &b)
{
    QList<PbField> out; int i = 0;
    while (i < b.size()) {
        quint64 k; if (!pbReadVarint(b, i, k)) break;
        PbField f; f.tag = int(k >> 3); f.wt = int(k & 7);
        if (f.wt == 0)      { if (!pbReadVarint(b, i, f.v)) break; }
        else if (f.wt == 2) { quint64 l; if (!pbReadVarint(b, i, l) || i + int(l) > b.size()) break; f.d = b.mid(i, int(l)); i += int(l); }
        else if (f.wt == 1) { if (i + 8 > b.size()) break; i += 8; }
        else if (f.wt == 5) { if (i + 4 > b.size()) break; i += 4; }
        else break;
        out << f;
    }
    return out;
}
// Apple writes octets without leading zeros ("2:11:22:3:44:5"); we keep "02:11:22:03:44:05"
static QString appleBssid(const QString &mac)
{
    QStringList parts;
    for (const QString &o : mac.split(QLatin1Char(':'))) { bool ok; parts << QString::number(o.toInt(&ok, 16), 16); }
    return parts.join(QLatin1Char(':'));
}
static QString canonMac(const QString &mac)
{
    QStringList parts;
    for (const QString &o : mac.split(QLatin1Char(':'))) { bool ok; parts << QStringLiteral("%1").arg(o.toInt(&ok, 16), 2, 16, QLatin1Char('0')); }
    return parts.join(QLatin1Char(':')).toLower();
}

void Locator::tryApple(const QList<AccessPoint> &usable, const QString &why)
{
    if (!m_useApple || usable.isEmpty()) { tryIp(why); return; }
    QList<AccessPoint> aps = usable;                                   // strongest first, at most 30
    std::sort(aps.begin(), aps.end(), [](const AccessPoint &a, const AccessPoint &b) { return a.dbm > b.dbm; });
    if (aps.size() > 30) aps = aps.mid(0, 30);
    emit statusMessage(QStringLiteral("BeaconDB had nothing — asking Apple about %1 access points…").arg(aps.size()));

    struct Shared { int pending = 0; QHash<QString, QPointF> pos; QHash<QString, double> acc; QString err; };
    auto sh = QSharedPointer<Shared>::create();
    const int seen = m_aps.size();
    for (int start = 0; start < aps.size(); start += 10) {              // the framing length is one byte → batches of 10
        QByteArray body;
        for (int i = start; i < qMin(start + 10, aps.size()); ++i)
            body += pbBytes(2, pbBytes(1, appleBssid(aps[i].bssid).toLatin1()));
        body += pbVarint(3 << 3) + pbVarint(0) + pbVarint(4 << 3) + pbVarint(100);   // noise 0, signal 100
        QByteArray req = QByteArray("\x00\x01\x00\x05", 4) + "en_US" + QByteArray("\x00\x13", 2) + "com.apple.locationd"
                       + QByteArray("\x00\x0a", 2) + "8.1.12B411" + QByteArray("\x00\x00\x00\x01\x00\x00\x00", 7);
        req.append(char(body.size())); req += body;
        QNetworkRequest nr(QUrl(QStringLiteral("https://gs-loc.apple.com/clls/wloc")));
        nr.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
        nr.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("locationd/1753.17 CFNetwork/711.1.12 Darwin/14.0.0"));
        nr.setTransferTimeout(15000);
        ++sh->pending;
        QNetworkReply *rep = m_nam.post(nr, req);
        connect(rep, &QNetworkReply::finished, this, [this, rep, sh, aps, seen, why] {
            rep->deleteLater();
            if (rep->error() != QNetworkReply::NoError) sh->err = rep->errorString();
            else {
                const QByteArray all = rep->readAll();
                for (const PbField &dev : pbParse(all.mid(10))) {
                    if (dev.tag != 2 || dev.wt != 2) continue;
                    QString mac; double lat = -180, lon = -180, hacc = -1;
                    for (const PbField &f : pbParse(dev.d)) {
                        if (f.tag == 1 && f.wt == 2) mac = QString::fromLatin1(f.d);
                        else if (f.tag == 2 && f.wt == 2)
                            for (const PbField &l : pbParse(f.d)) {
                                if (l.tag == 1)      lat = double(qint64(l.v)) / 1e8;
                                else if (l.tag == 2) lon = double(qint64(l.v)) / 1e8;
                                else if (l.tag == 3) hacc = double(qint64(l.v));
                            }
                    }
                    if (mac.isEmpty() || lat < -90 || lat > 90 || lon < -180 || lon > 180 || (lat == 0 && lon == 0)) continue;   // -180,-180 = unknown
                    sh->pos.insert(canonMac(mac), QPointF(lat, lon));
                    sh->acc.insert(canonMac(mac), hacc);
                }
            }
            if (--sh->pending > 0) return;

            // Every heard AP Apple placed goes on the map (same slot the WiGLE lookup fills).
            // Runs after accept(): that is what creates the records for newly heard APs.
            auto place = [this, sh] {
                int placed = 0;
                for (auto it = sh->pos.constBegin(); it != sh->pos.constEnd(); ++it) {
                    auto r = m_apRecords.find(it.key());
                    if (r == m_apRecords.end()) r = m_apRecords.find(it.key().toUpper());
                    if (r == m_apRecords.end()) continue;
                    if (!r->wigle) {
                        ++placed;
                        for (const AccessPoint &ap : m_aps) {
                            if (ap.bssid != r.key()) continue;
                            BeaconEvent ev = apEvent(QStringLiteral("ap_placed"), ap);      // before: the ring guess
                            if (ev.kind == QLatin1String("ring")) { ev.hasFrom = true; ev.fromLat = ev.lat; ev.fromLon = ev.lon; }
                            ev.kind = QStringLiteral("wigle"); ev.hasPos = true; ev.lat = it->x(); ev.lon = it->y();   // r/bearing kept: the orbit point it animates from
                            ev.text = QStringLiteral("%1 placed on the map by Apple").arg(ap.ssid.isEmpty() ? QStringLiteral("(hidden)") : ap.ssid);
                            logEvent(ev);
                            break;
                        }
                    }
                    r->wigle = true; r->wLat = it->x(); r->wLon = it->y(); r->wigleChecked = QDateTime::currentDateTime();
                }
                if (placed) { saveApRecords(); emit scanUpdated(); }
            };

            // Our position: signal-weighted centroid of the APs we asked about and Apple knew
            double sw = 0, slat = 0, slon = 0; int matched = 0; QList<double> accs;
            for (const AccessPoint &ap : aps) {
                const auto it = sh->pos.constFind(canonMac(ap.bssid));
                if (it == sh->pos.constEnd()) continue;
                const double w = std::pow(10.0, ap.dbm / 20.0);
                sw += w; slat += w * it->x(); slon += w * it->y(); ++matched;
                const double a = sh->acc.value(canonMac(ap.bssid), -1); if (a > 0) accs << a;
            }
            if (matched == 0 || sw <= 0) {
                place();
                tryIp(why + (sh->err.isEmpty() ? QStringLiteral("; Apple knows none of them either") : QStringLiteral("; Apple: ") + sh->err));
                return;
            }
            Fix f; f.valid = true; f.lat = slat / sw; f.lon = slon / sw;
            double spread = 0;
            for (const AccessPoint &ap : aps) {
                const auto it = sh->pos.constFind(canonMac(ap.bssid)); if (it == sh->pos.constEnd()) continue;
                const double w = std::pow(10.0, ap.dbm / 20.0), d = distanceM(f.lat, f.lon, it->x(), it->y());
                spread += w * d * d;
            }
            spread = std::sqrt(spread / sw);
            std::sort(accs.begin(), accs.end());
            const double medAcc = accs.isEmpty() ? 100.0 : accs[accs.size() / 2];
            f.accuracy = qBound(30.0, qMax(spread, medAcc) + (matched < 3 ? 50.0 : 0.0), 1500.0);
            f.source = QStringLiteral("wifi"); f.provider = QStringLiteral("apple");
            f.time = QDateTime::currentDateTime(); f.apCount = seen; f.apUsed = matched;
            accept(f);
            place();
            finish(true, QStringLiteral("Apple Wi-Fi fix from %1 of %2 access points (±%3 m)").arg(matched).arg(aps.size()).arg(qRound(f.accuracy)));
        });
    }
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
    maybeReproject(cand);                               // the RV moved: its anchors follow (docs/RANGING.md §4.3.5)
    anchorFix(cand);                                    // within 100 m of the this-computer anchor: the anchor IS the fix
    noteSightings(m_aps, cand);
    if (cand.source != QLatin1String("ip") && cand.accuracy >= 0 && cand.accuracy < 2000) {
        QList<AccessPoint> fixed;                       // home networks move with the RV: no vantage points from them
        for (const AccessPoint &ap : m_aps) if (!isHome(ap)) fixed << ap;
        noteObservations(fixed, cand);
    }
    if (cand.precise() && atHome()) {                   // a precise fix while a home AP is heard = where the RV is
        m_homeFix = cand;
        QSettings().setValue("homeFix", QJsonDocument(cand.toJson()).toJson(QJsonDocument::Compact));
    }

    const bool moved = !m_fix.valid || m_fix.source != cand.source
                       || distanceM(m_fix.lat, m_fix.lon, cand.lat, cand.lon) > m_moveThresholdM;
    if (!moved) {
        m_fix.time = cand.time; m_fix.accuracy = cand.accuracy;
        m_fix.apCount = cand.apCount; m_fix.apUsed = cand.apUsed;
        saveState();
        emit FixChanged();
        BeaconEvent ev; ev.type = QStringLiteral("fix"); ev.hasPos = true; ev.lat = m_fix.lat; ev.lon = m_fix.lon;
        ev.text = QStringLiteral("Fix confirmed · ±%1 m via %2").arg(qRound(cand.accuracy)).arg(cand.source == QLatin1String("wifi") ? (cand.provider == QLatin1String("apple") ? QStringLiteral("Apple Wi-Fi") : QStringLiteral("BeaconDB")) : cand.source == QLatin1String("starlink") ? QStringLiteral("Starlink GPS") : QStringLiteral("IP"));
        logEvent(ev);
        return;
    }
    const bool nearby = m_fix.valid && distanceM(m_fix.lat, m_fix.lon, cand.lat, cand.lon) < 500;
    cand.place = !ipCity.isEmpty() ? ipCity : (nearby ? m_fix.place : QString());
    if (nearby) { cand.city = m_fix.city; cand.region = m_fix.region; cand.country = m_fix.country; if (m_fix.hasElevation()) cand.elevation = m_fix.elevation; }
    if (cand.place.isEmpty())
        cand.place = QStringLiteral("%1, %2").arg(cand.lat, 0, 'f', 4).arg(cand.lon, 0, 'f', 4);
    // We're leaving the previous stop: its last confirmation is when we were last seen there
    if (!m_history.isEmpty() && m_fix.valid && m_fix.time.isValid() && !m_history.last().departed.isValid()
        && distanceM(m_history.last().lat, m_history.last().lon, m_fix.lat, m_fix.lon) < 1) {
        m_history.last().departed = m_fix.time >= m_history.last().time ? m_fix.time : m_history.last().time;
        rewriteHistory();
    }
    const Fix prev = m_fix;
    m_fix = cand;
    appendHistory(m_fix);
    saveState();
    emit FixChanged();
    {
        const double movedM = prev.valid ? distanceM(prev.lat, prev.lon, cand.lat, cand.lon) : 0;
        BeaconEvent ev; ev.type = QStringLiteral("fix"); ev.hasPos = true; ev.lat = cand.lat; ev.lon = cand.lon;
        if (prev.valid && movedM > qMax(0.0, cand.accuracy)) { ev.hasFrom = true; ev.fromLat = prev.lat; ev.fromLon = prev.lon; }
        ev.text = QStringLiteral("New fix: %1 · ±%2 m").arg(cand.place).arg(qRound(cand.accuracy));
        logEvent(ev);
        BeaconEvent st; st.type = QStringLiteral("stop"); st.hasPos = true; st.lat = cand.lat; st.lon = cand.lon; st.hasFrom = ev.hasFrom; st.fromLat = ev.fromLat; st.fromLon = ev.fromLon;
        st.text = movedM > 1000 && prev.valid ? QStringLiteral("Stop logged: %1 · %2 km from %3").arg(cand.place).arg(movedM / 1000.0, 0, 'f', 1).arg(prev.place)
                                              : QStringLiteral("Stop logged: %1").arg(cand.place);
        logEvent(st);
    }
    emit stopAdded(cand.lat, cand.lon);
    if (m_notifyStops && cand.precise() && !nearby) {
        const double km = prev.precise() ? distanceM(prev.lat, prev.lon, cand.lat, cand.lon) / 1000.0 : -1;
        notify(QStringLiteral("New stop: %1").arg(cand.place),
               km >= 0 ? QStringLiteral("%1 km %2 of %3 · ±%4 m").arg(km, 0, 'f', km < 10 ? 1 : 0).arg(compass(bearingDeg(prev.lat, prev.lon, cand.lat, cand.lon)), prev.place).arg(qRound(cand.accuracy))
                       : QStringLiteral("±%1 m via %2").arg(qRound(cand.accuracy)).arg(cand.source == QLatin1String("wifi") ? (cand.provider == QLatin1String("apple") ? QStringLiteral("Apple Wi-Fi") : QStringLiteral("BeaconDB Wi-Fi")) : QStringLiteral("Starlink GPS")),
               QStringLiteral("mark-location"));
    }
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
            const bool first = !r.wigle;
            r.wigle = true; r.wLat = n["trilat"].toDouble(); r.wLon = n["trilong"].toDouble();
            if (r.ssid.isEmpty()) r.ssid = n["ssid"].toString();
            if (first)
                for (const AccessPoint &ap : m_aps) {
                    if (ap.bssid != bssid) continue;
                    BeaconEvent ev; ev.type = QStringLiteral("ap_placed"); ev.bssid = bssid; ev.ssid = ap.ssid; ev.dbm = ap.dbm; ev.status = apStatus(ap);
                    ev.kind = QStringLiteral("wigle"); ev.hasPos = true; ev.lat = r.wLat; ev.lon = r.wLon;
                    if (m_fix.valid) { ev.hasFrom = true; ev.fromLat = m_fix.lat; ev.fromLon = m_fix.lon; }
                    ev.text = QStringLiteral("%1 placed on the map by WiGLE").arg(ap.ssid.isEmpty() ? QStringLiteral("(hidden)") : ap.ssid);
                    logEvent(ev);
                    break;
                }
        }
        saveApRecords();
        emit scanUpdated();
        m_wigleTimer.start(1500);
    });
}

void Locator::finish(bool ok, const QString &message)
{
    m_busy = false;
    if (!ok) { m_lastError = message; saveState(); BeaconEvent ev; ev.type = QStringLiteral("error"); ev.text = message.section(QLatin1Char('\n'), 0, 0); logEvent(ev); }
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
        QString city;
        for (const char *k : {"city", "town", "village", "hamlet", "municipality", "county"}) { city = a[k].toString(); if (!city.isEmpty()) break; }
        const QString region = !state.isEmpty() ? state : a["province"].toString();
        const QString country = a["country"].toString();
        // Only apply if we're still at that spot
        if (m_fix.valid && distanceM(m_fix.lat, m_fix.lon, lat, lon) < 50) {
            m_countryCode = a["country_code"].toString().toLower();
            if (!m_countryCode.isEmpty() && m_dbUsable) m_db->setKv(QStringLiteral("countryCode"), m_countryCode);
            m_fix.place = place; m_fix.city = city; m_fix.region = region; m_fix.country = country;
            if (!m_history.isEmpty() && distanceM(m_history.last().lat, m_history.last().lon, lat, lon) < 50) {
                m_history.last().place = place; m_history.last().city = city; m_history.last().region = region; m_history.last().country = country;
                rewriteHistory();
            }
            saveState();
            noteVisited(m_fix, true);
            emit FixChanged();
        }
    });
}

// ── Points of interest (OpenStreetMap via Overpass) ──────────────────────────
// Picked for life on the road: fuel, food, water, dump stations, camping, laundry…
const QList<PoiCategory> &Locator::poiCategories()
{
    // key, label, icon, colour, group, wide-radius[, reachKm]
    static const QList<PoiCategory> cats = {
        // Services (life on the road)
        {QStringLiteral("fuel"),     QStringLiteral("Fuel"),              QStringLiteral("⛽"), QColor(0xff, 0x9f, 0x43), QStringLiteral("services"), false},
        {QStringLiteral("propane"),  QStringLiteral("Propane"),           QStringLiteral("🔥"), QColor(0xff, 0x6b, 0x3d), QStringLiteral("services"), false},
        {QStringLiteral("charging"), QStringLiteral("EV charging"),       QStringLiteral("🔌"), QColor(0x2e, 0xd5, 0x73), QStringLiteral("services"), false},
        {QStringLiteral("grocery"),  QStringLiteral("Groceries"),         QStringLiteral("🛒"), QColor(0x6c, 0xff, 0x8a), QStringLiteral("services"), false},
        {QStringLiteral("food"),     QStringLiteral("Food"),              QStringLiteral("🍴"), QColor(0xff, 0x6b, 0x81), QStringLiteral("services"), false},
        {QStringLiteral("cafe"),     QStringLiteral("Café"),              QStringLiteral("☕"), QColor(0xd9, 0xa4, 0x7a), QStringLiteral("services"), false},
        {QStringLiteral("camp"),     QStringLiteral("Camping / RV"),      QStringLiteral("⛺"), QColor(0x7b, 0xed, 0x9f), QStringLiteral("services"), false},
        {QStringLiteral("water"),    QStringLiteral("Drinking water"),    QStringLiteral("🚰"), QColor(0x4f, 0xc3, 0xf7), QStringLiteral("services"), false},
        {QStringLiteral("dump"),     QStringLiteral("Dump station"),      QStringLiteral("🚐"), QColor(0xa2, 0x9b, 0xfe), QStringLiteral("services"), false},
        {QStringLiteral("shower"),   QStringLiteral("Showers"),           QStringLiteral("🚿"), QColor(0x74, 0xb9, 0xff), QStringLiteral("services"), false},
        {QStringLiteral("toilets"),  QStringLiteral("Toilets"),           QStringLiteral("🚻"), QColor(0x8a, 0x93, 0xa6), QStringLiteral("services"), false},
        {QStringLiteral("laundry"),  QStringLiteral("Laundry"),           QStringLiteral("🧺"), QColor(0x81, 0xec, 0xec), QStringLiteral("services"), false},
        {QStringLiteral("repair"),   QStringLiteral("Auto / tyres"),      QStringLiteral("🔧"), QColor(0xb2, 0xbe, 0xc3), QStringLiteral("services"), false},
        {QStringLiteral("hardware"), QStringLiteral("Hardware / outdoor"),QStringLiteral("🧰"), QColor(0xc8, 0xa0, 0x6a), QStringLiteral("services"), false},
        {QStringLiteral("wifi"),     QStringLiteral("Public Wi-Fi"),      QStringLiteral("📶"), QColor(0x35, 0xd6, 0xff), QStringLiteral("services"), false},
        {QStringLiteral("post"),     QStringLiteral("Post / parcels"),    QStringLiteral("📮"), QColor(0xe1, 0x70, 0x55), QStringLiteral("services"), false},
        {QStringLiteral("rest"),     QStringLiteral("Rest area"),         QStringLiteral("🅿️"), QColor(0x60, 0xa3, 0xff), QStringLiteral("services"), false},
        {QStringLiteral("carwash"),  QStringLiteral("Car wash"),          QStringLiteral("🫧"), QColor(0x81, 0xec, 0xec), QStringLiteral("services"), false},
        // Emergency & civic (fetched out to the wide radius)
        {QStringLiteral("police"),   QStringLiteral("Police"),            QStringLiteral("🚔"), QColor(0x4d, 0x8b, 0xff), QStringLiteral("civic"), true},
        {QStringLiteral("fire"),     QStringLiteral("Fire station"),      QStringLiteral("🚒"), QColor(0xff, 0x4d, 0x4d), QStringLiteral("civic"), true},
        {QStringLiteral("health"),   QStringLiteral("Hospital / ER"),     QStringLiteral("🏥"), QColor(0xff, 0x4f, 0x4f), QStringLiteral("civic"), true},
        {QStringLiteral("urgent"),   QStringLiteral("Urgent care / clinic"), QStringLiteral("🩺"), QColor(0xff, 0x8a, 0x8a), QStringLiteral("civic"), true},
        // Pediatric ERs come from their own search (reachKm), see refreshPediatric()
        {QStringLiteral("peds_er"),  QStringLiteral("Pediatric ER"),      QStringLiteral("🧸"), QColor(0xff, 0x5f, 0xa2), QStringLiteral("civic"), true, 150},
        {QStringLiteral("peds_urgent"), QStringLiteral("Pediatric urgent care"), QStringLiteral("🩹"), QColor(0xff, 0xa3, 0xcf), QStringLiteral("civic"), true, 50},
        {QStringLiteral("pharmacy"), QStringLiteral("Pharmacy"),          QStringLiteral("💊"), QColor(0xff, 0x7a, 0xa8), QStringLiteral("civic"), false},
        {QStringLiteral("dentist"),  QStringLiteral("Dentist"),           QStringLiteral("🦷"), QColor(0xe6, 0xed, 0xf7), QStringLiteral("civic"), false},
        {QStringLiteral("vet"),      QStringLiteral("Veterinary"),        QStringLiteral("🐾"), QColor(0xd0, 0xa0, 0x6a), QStringLiteral("civic"), true},
        {QStringLiteral("library"),  QStringLiteral("Library"),           QStringLiteral("📚"), QColor(0xfd, 0xcb, 0x6e), QStringLiteral("civic"), false},
        {QStringLiteral("townhall"), QStringLiteral("Town hall"),         QStringLiteral("🏛️"), QColor(0xc9, 0xd4, 0xe5), QStringLiteral("civic"), true},
        {QStringLiteral("court"),    QStringLiteral("Courthouse"),        QStringLiteral("⚖️"), QColor(0xc9, 0xd4, 0xe5), QStringLiteral("civic"), true},
        {QStringLiteral("dmv"),      QStringLiteral("DMV / licensing"),   QStringLiteral("🪪"), QColor(0xb2, 0xbe, 0xc3), QStringLiteral("civic"), true},
        {QStringLiteral("school"),   QStringLiteral("School"),            QStringLiteral("🏫"), QColor(0xff, 0xd1, 0x66), QStringLiteral("civic"), false},
        {QStringLiteral("community"),QStringLiteral("Community centre"),  QStringLiteral("🏢"), QColor(0x9f, 0xb0, 0xc8), QStringLiteral("civic"), false},
        // Kids & fun
        {QStringLiteral("playground"),QStringLiteral("Playground"),       QStringLiteral("🛝"), QColor(0xff, 0xb3, 0x47), QStringLiteral("kids"), false},
        {QStringLiteral("park"),     QStringLiteral("Park"),              QStringLiteral("🌳"), QColor(0x6c, 0xff, 0x8a), QStringLiteral("kids"), false},
        {QStringLiteral("dogpark"),  QStringLiteral("Dog park"),          QStringLiteral("🐕"), QColor(0xd0, 0xa0, 0x6a), QStringLiteral("kids"), false},
        {QStringLiteral("pool"),     QStringLiteral("Swimming pool"),     QStringLiteral("🏊"), QColor(0x4f, 0xc3, 0xf7), QStringLiteral("kids"), false},
        {QStringLiteral("splash"),   QStringLiteral("Splash pad / water park"), QStringLiteral("💦"), QColor(0x74, 0xb9, 0xff), QStringLiteral("kids"), true},
        {QStringLiteral("zoo"),      QStringLiteral("Zoo / aquarium"),    QStringLiteral("🦁"), QColor(0xff, 0x9f, 0x43), QStringLiteral("kids"), true},
        {QStringLiteral("museum"),   QStringLiteral("Museum"),            QStringLiteral("🏛"), QColor(0xa2, 0x9b, 0xfe), QStringLiteral("kids"), true},
        {QStringLiteral("themepark"),QStringLiteral("Theme park"),        QStringLiteral("🎢"), QColor(0xff, 0x4f, 0xd8), QStringLiteral("kids"), true},
        {QStringLiteral("icecream"), QStringLiteral("Ice cream"),         QStringLiteral("🍦"), QColor(0xff, 0xc6, 0xe0), QStringLiteral("kids"), false},
        {QStringLiteral("cinema"),   QStringLiteral("Cinema"),            QStringLiteral("🎬"), QColor(0xe6, 0xed, 0xf7), QStringLiteral("kids"), true},
        {QStringLiteral("bowling"),  QStringLiteral("Bowling"),           QStringLiteral("🎳"), QColor(0xe6, 0xed, 0xf7), QStringLiteral("kids"), true},
        {QStringLiteral("arcade"),   QStringLiteral("Arcade"),            QStringLiteral("🕹️"), QColor(0xff, 0x4f, 0xd8), QStringLiteral("kids"), true},
        {QStringLiteral("trampoline"),QStringLiteral("Trampoline park"),  QStringLiteral("🤸"), QColor(0xff, 0xb3, 0x47), QStringLiteral("kids"), true},
        {QStringLiteral("skate"),    QStringLiteral("Skate park"),        QStringLiteral("🛹"), QColor(0xb2, 0xbe, 0xc3), QStringLiteral("kids"), false},
        {QStringLiteral("beach"),    QStringLiteral("Beach"),             QStringLiteral("🏖️"), QColor(0xff, 0xd1, 0x66), QStringLiteral("kids"), true},
        {QStringLiteral("picnic"),   QStringLiteral("Picnic site"),       QStringLiteral("🧺"), QColor(0x7b, 0xed, 0x9f), QStringLiteral("kids"), false},
        {QStringLiteral("trail"),    QStringLiteral("Trailhead / nature reserve"), QStringLiteral("🥾"), QColor(0x2e, 0xd5, 0x73), QStringLiteral("kids"), true},
    };
    return cats;
}

QString Locator::poiGroupLabel(const QString &group)
{
    if (group == QLatin1String("civic")) return QStringLiteral("Emergency & civic");
    if (group == QLatin1String("kids"))  return QStringLiteral("Kids & fun");
    return QStringLiteral("Services");
}

const PoiCategory *Locator::poiCategory(const QString &key)
{
    for (const PoiCategory &c : poiCategories()) if (c.key == key) return &c;
    return nullptr;
}

static const char *const kOverpassMirrors[] = {"https://overpass-api.de/api/interpreter",
                                               "https://overpass.kumi.systems/api/interpreter"};
static const int kOverpassLastMirror = int(sizeof(kOverpassMirrors) / sizeof(*kOverpassMirrors)) - 1;

// A bbox query is ~20× faster than around: on ways; the radius is applied to the answer.
static QString bboxFor(double lat, double lon, int rM)
{
    const double dLat = rM / 111320.0, dLon = rM / (111320.0 * std::cos(qDegreesToRadians(lat)));
    return QStringLiteral("%1,%2,%3,%4").arg(lat - dLat, 0, 'f', 5).arg(lon - dLon, 0, 'f', 5).arg(lat + dLat, 0, 'f', 5).arg(lon + dLon, 0, 'f', 5);
}

// The help categories get a drive time (docs/API.md "Pediatric ER")
static bool driveCat(const QString &cat)
{
    return cat == QLatin1String("peds_er") || cat == QLatin1String("peds_urgent") || cat == QLatin1String("health") || cat == QLatin1String("urgent")
        || cat == QLatin1String("police") || cat == QLatin1String("fire");
}

static QString poiKey(const Poi &p) { return p.osmType + QLatin1Char('/') + QString::number(p.osmId); }

static QString ageText(qint64 secs)
{
    if (secs < 3600) return QStringLiteral("%1 min").arg(qMax<qint64>(1, secs / 60));
    if (secs < 48 * 3600) return QStringLiteral("%1 h").arg(secs / 3600);
    return QStringLiteral("%1 d").arg(secs / 86400);
}

void Locator::refreshPois(bool force)
{
    if (m_standalone) return;                             // only the tray queries Overpass (see refreshPediatric)
    if (!m_fix.valid || m_poiBusy) return;
    const int radius = m_poiRadiusKm * 1000;
    const bool coarse = m_fix.source == QLatin1String("ip");
    const double moved = m_poiTime.isValid() ? distanceM(m_poiLat, m_poiLon, m_fix.lat, m_fix.lon) : 1e9;
    const bool stale = !m_poiTime.isValid() || m_poiTime.daysTo(QDateTime::currentDateTime()) >= 7;
    if (!force && !stale && moved < qMax(500.0, radius / 3.0) && m_poiRadiusM == radius) {
        m_poiNote = coarse ? QStringLiteral("around the approximate (IP) position — may be far off") : QString();
        return;
    }
    // Don't hammer Overpass after a failure
    if (!force && m_poiTried.isValid() && m_poiTried.secsTo(QDateTime::currentDateTime()) < 120) return;
    if (!overpassSlot(false, force)) return;
    m_poiTried = QDateTime::currentDateTime();
    queryOverpass(m_fix.lat, m_fix.lon, radius, 0);
}

// Overpass allows two query slots per IP. We use one: a single query in flight, 5 s between
// queries, and a minute's pause after 429 / 504. A request that has to wait is queued (the
// newest one per kind) and re-checked by pumpOverpass().
bool Locator::overpassSlot(bool peds, bool force)
{
    const QDateTime now = QDateTime::currentDateTime();
    qint64 wait = 0;
    if (m_overpassIdle.isValid()) wait = qMax<qint64>(wait, 5000 - m_overpassIdle.msecsTo(now));
    if (m_overpassCoolUntil.isValid()) wait = qMax<qint64>(wait, now.msecsTo(m_overpassCoolUntil));
    if (!m_poiBusy && !m_pedsBusy && wait <= 0) return true;
    if (peds) { m_pedsPending = true; m_pedsPendingForce = m_pedsPendingForce || force; }
    else      { m_poiPending = true;  m_poiPendingForce = m_poiPendingForce || force; }
    if (!m_poiBusy && !m_pedsBusy) m_overpassTimer.start(int(qBound(qint64(0), wait, qint64(3600000))) + 50);
    return false;
}

void Locator::overpassDone()
{
    m_overpassIdle = QDateTime::currentDateTime();
    if (m_poiPending || m_pedsPending) m_overpassTimer.start(5050);
}

void Locator::pumpOverpass()
{
    if (m_poiBusy || m_pedsBusy) return;
    if (m_poiPending) {                                   // the places around us first
        const bool f = m_poiPendingForce; m_poiPending = m_poiPendingForce = false;
        refreshPois(f);
        if (m_poiBusy) return;                            // the pediatric search waits for it (overpassDone re-arms the timer)
    }
    if (m_pedsPending) {
        const bool f = m_pedsPendingForce; m_pedsPending = m_pedsPendingForce = false;
        refreshPediatric(f);
    }
}

static QString hcLabel(const QJsonObject &t)
{
    const QString hc = t["healthcare"].toString(), am = t["amenity"].toString(), sp = t["healthcare:speciality"].toString();
    if (hc == "urgent_care" || am == "urgent_care") return QStringLiteral("urgent care");
    if (am == "doctors" || hc == "doctor") return sp.isEmpty() ? QStringLiteral("doctor's office") : QString(sp).replace('_', ' ');
    if (am == "clinic" || hc == "clinic") return QStringLiteral("clinic");
    return {};
}

// One classified Overpass element → a place with its one-line detail; false = not worth showing
static bool makePoi(const PoiClassify::Element &e, const PoiClassify::Result &r, Poi *out)
{
    const QJsonObject &t = e.tags;
    const QString &cat = r.cat;
    Poi pt;
    pt.cat = cat;
    pt.lat = e.lat; pt.lon = e.lon;
    pt.osmType = e.type; pt.osmId = e.id;
    pt.name = t["name"].toString();
    if (pt.name.isEmpty()) pt.name = t["brand"].toString();
    if (pt.name.isEmpty()) pt.name = t["operator"].toString();
    const QString ia = t["internet_access"].toString();
    pt.wifi = ia == "wlan" || ia == "yes";
    pt.hours = t["opening_hours"].toString();
    pt.phone = r.phone;
    pt.website = t["website"].toString(); if (pt.website.isEmpty()) pt.website = t["contact:website"].toString();
    pt.address = r.address;
    pt.wheelchair = t["wheelchair"].toString();
    pt.emergency = r.emergency;
    pt.peds = r.peds; pt.er = r.er; pt.campus = r.campus;
    QStringList d;
    if (!r.detail.isEmpty()) d << r.detail;                // the ER confidence first: "ER not confirmed — call ahead", "not an ER", …
    if (!t["brand"].toString().isEmpty() && t["brand"].toString() != pt.name) d << t["brand"].toString();
    if (cat == "fuel") {
        QStringList f;
        if (t["fuel:diesel"].toString() == "yes") f << "diesel";
        if (t["fuel:lpg"].toString() == "yes") f << "propane";
        if (t["fuel:HGV_diesel"].toString() == "yes" || t["hgv"].toString() == "yes") f << "truck lanes";
        if (!f.isEmpty()) d << f.join(" · ");
    }
    if (cat == "camp") {
        if (t["fee"].toString() == "no") d << "free";
        if (t["power_supply"].toString() == "yes") d << "hookups";
        if (t["sanitary_dump_station"].toString() == "yes") d << "dump station";
        if (t["shower"].toString() == "yes" || t["showers"].toString() == "yes") d << "showers";
        if (!t["capacity"].toString().isEmpty()) d << t["capacity"].toString() + " sites";
    }
    if (cat == "dump" || cat == "water" || cat == "toilets" || cat == "shower") {
        if (t["fee"].toString() == "no") d << "free";
        else if (t["fee"].toString() == "yes") d << "fee";
        if (t["access"].toString() == "customers") d << "customers only";
    }
    if (cat == "food" || cat == "cafe") {
        const QString c = t["cuisine"].toString();
        if (!c.isEmpty()) d << QString(c).replace('_', ' ').replace(';', ", ");
    }
    if (cat == "charging") {
        QStringList s;
        for (const char *k : {"socket:tesla_supercharger", "socket:type2_combo", "socket:chademo", "socket:nacs"})
            if (!t[k].toString().isEmpty() && t[k].toString() != "no") s << QString::fromLatin1(k).section(':', 1);
        if (!s.isEmpty()) d << s.join(", ");
    }
    if ((cat == "urgent" || cat == "peds_urgent") && !hcLabel(t).isEmpty()) d << hcLabel(t);
    if (cat == "park" || cat == "playground" || cat == "dogpark") { if (t["dog"].toString() == "yes" || t["dog"].toString() == "leashed") d << "dogs ok"; if (t["fee"].toString() == "yes") d << "fee"; }
    if (cat == "pool" && t["indoor"].toString() == "yes") d << "indoor";
    if (cat == "trail" && !t["sac_scale"].toString().isEmpty()) d << t["sac_scale"].toString().replace('_', ' ');
    if (!pt.wheelchair.isEmpty() && pt.wheelchair != "no") d << (pt.wheelchair == "yes" ? QStringLiteral("♿") : QStringLiteral("♿ limited"));
    if (pt.wifi && cat != "wifi") d << "Wi-Fi";
    pt.detail = d.join(" · ");
    if (pt.name.isEmpty() && (cat == "food" || cat == "cafe" || cat == "grocery" || cat == "wifi" || cat == "hardware" || cat == "museum" || cat == "cinema" || cat == "icecream" || cat == "school" || cat == "community")) return false;
    *out = pt;
    return true;
}

void Locator::queryOverpass(double lat, double lon, int radiusM, int mirror)
{
    m_poiBusy = true;
    m_poiNote = QStringLiteral("Loading places within %1 km…").arg(radiusM / 1000);
    emit poisUpdated();
    // Two boxes: everything within the radius, and the sparse civic / attraction categories out to the wide radius.
    const int wideM = qMax(radiusM, qMin(25000, radiusM * 4));
    const QString bbox = bboxFor(lat, lon, radiusM), wide = bboxFor(lat, lon, wideM);
    const QString q = QStringLiteral(
        "[out:json][timeout:25];("
        "nwr[amenity~\"^(fuel|charging_station|restaurant|fast_food|food_court|pub|cafe|sanitary_dump_station|drinking_water|water_point|"
        "shower|toilets|laundry|pharmacy|dentist|library|school|kindergarten|community_centre|post_office|parcel_locker|vehicle_inspection|car_wash|ice_cream)$\"](%1);"
        "nwr[shop~\"^(supermarket|convenience|greengrocer|wholesale|gas|bottled_gas|laundry|chemist|car_repair|tyres|car_parts|hardware|doityourself|outdoor|trade|ice_cream)$\"](%1);"
        "nwr[tourism~\"^(camp_site|caravan_site|picnic_site)$\"](%1);"
        "nwr[leisure~\"^(playground|park|garden|dog_park|swimming_pool|swimming_area|skatepark|picnic_table)$\"](%1);"
        "nwr[highway~\"^(rest_area|services)$\"](%1);"
        "nwr[internet_access~\"^(wlan|yes)$\"][name](%1);"
        // wide: emergency / civic / attractions (sparse in the country)
        "nwr[amenity~\"^(police|fire_station|hospital|clinic|doctors|urgent_care|veterinary|townhall|courthouse|cinema)$\"](%2);"
        "nwr[healthcare~\"^(urgent_care|clinic|doctor|hospital)$\"](%2);"
        "nwr[office=government](%2);"
        "nwr[tourism~\"^(zoo|aquarium|museum|gallery|theme_park)$\"](%2);"
        "nwr[leisure~\"^(water_park|bowling_alley|amusement_arcade|trampoline_park|nature_reserve|beach_resort)$\"](%2);"
        "nwr[natural=beach](%2);"
        "nwr[highway=trailhead](%2);"
        ");out center tags qt 2500;").arg(bbox, wide);
    QNetworkRequest req{QUrl(QString::fromLatin1(kOverpassMirrors[mirror]))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(USER_AGENT));
    req.setTransferTimeout(30000);
    QUrlQuery body; body.addQueryItem(QStringLiteral("data"), q);
    QNetworkReply *rep = m_nam.post(req, body.toString(QUrl::FullyEncoded).toUtf8());
    connect(rep, &QNetworkReply::finished, this, [this, rep, lat, lon, radiusM, wideM, mirror] {
        rep->deleteLater();
        const int http = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
        // Overpass reports timeouts / overload as HTTP 200 with a "remark" and no elements
        const QString remark = doc.object()["remark"].toString();
        if (rep->error() != QNetworkReply::NoError || !doc.isObject() || remark.contains(QLatin1String("error"), Qt::CaseInsensitive)) {
            if (mirror < kOverpassLastMirror) { queryOverpass(lat, lon, radiusM, mirror + 1); return; }
            m_poiBusy = false;
            if (http == 429 || http == 504) m_overpassCoolUntil = QDateTime::currentDateTime().addSecs(60);
            m_poiNote = QStringLiteral("Couldn't load places (%1)").arg(!remark.isEmpty() ? remark.left(80) : http ? QStringLiteral("HTTP %1").arg(http) : rep->errorString());
            emit poisUpdated();
            overpassDone();
            return;
        }
        QList<PoiClassify::Element> els;
        for (const QJsonValue &v : doc.object()["elements"].toArray()) els << PoiClassify::Element::fromOverpass(v.toObject());
        const QList<PoiClassify::Result> res = PoiClassify::classifyAll(els);   // category + pediatric tier, campus ERs, duplicates
        QHash<QString, QList<Poi>> byCat;
        for (int i = 0; i < els.size(); ++i) {
            const PoiClassify::Result &r = res[i];
            const PoiClassify::Element &e = els[i];
            if (r.dropped || r.cat.isEmpty() || (e.lat == 0 && e.lon == 0)) continue;
            const PoiCategory *pc = poiCategory(r.cat);
            if (distanceM(lat, lon, e.lat, e.lon) > (pc && pc->wide ? wideM : radiusM)) continue;
            Poi pt;
            if (makePoi(e, r, &pt)) byCat[r.cat] << pt;
        }
        // Keep the nearest few dozen per category so a city doesn't bury the map
        QList<Poi> out;
        for (auto it = byCat.begin(); it != byCat.end(); ++it) {
            QList<Poi> &l = it.value();
            std::sort(l.begin(), l.end(), [lat, lon](const Poi &a, const Poi &b) {
                return distanceM(lat, lon, a.lat, a.lon) < distanceM(lat, lon, b.lat, b.lon); });
            const PoiCategory *pc = poiCategory(it.key());
            out += l.mid(0, it.key() == QLatin1String("toilets") || it.key() == QLatin1String("water") || (pc && pc->wide) ? 15 : 30);
        }
        m_pois = out;
        m_poiLat = lat; m_poiLon = lon; m_poiRadiusM = radiusM;
        m_poiTime = QDateTime::currentDateTime();
        m_poiBusy = false;
        m_poiNote = m_fix.source == QLatin1String("ip") ? QStringLiteral("around the approximate (IP) position — may be far off") : QString();
        if (m_pois.isEmpty()) m_poiNote = QStringLiteral("No mapped places within %1 km").arg(radiusM / 1000);
        savePois();
        rebuildMergedPois();
        emit poisUpdated();
        overpassDone();
    });
}

// ── Pediatric ERs ─────────────────────────────────────────────────────────────
// Children's hospitals are sparse (one per region), so they get their own query out to
// pedsRadiusKm instead of a bigger main query. It re-runs only when we moved a quarter of the
// radius, the answer is 30 days old, the radius changed, or the user asked; a failure backs off
// 10 minutes and never throws away what we had.
void Locator::setPedsRadiusKm(int km)
{
    km = qBound(50, km, 300);
    if (km == m_pedsRadiusKm) return;
    m_pedsRadiusKm = km;
    QSettings().setValue("pedsRadiusKm", km);
    emit poisUpdated();
    refreshPediatric(false);                              // a different radius re-queries (when the fix allows it)
}

void Locator::refreshPediatric(bool force)
{
    // Only the tray talks to Overpass: a short-lived `beaconfix --once` / `--snapshot` would post the heaviest query and
    // quit seconds later, leaving the server working on it in one of our two slots (and it ignores the tray's back-off)
    if (m_standalone) return;
    if (!m_fix.valid || m_pedsBusy) return;
    if (m_fix.source == QLatin1String("ip") || m_fix.accuracy > 5000) {   // the ground station's city is no place to search from
        if (m_pedsSkipLogged != m_fix.time) {
            m_pedsSkipLogged = m_fix.time;
            if (m_fix.source == QLatin1String("ip")) qInfo("beaconfix: pediatric search skipped (IP fix)");
            else qInfo("beaconfix: pediatric search skipped (fix ±%d km)", qRound(m_fix.accuracy / 1000));
        }
        return;
    }
    const int radiusM = m_pedsRadiusKm * 1000;
    const QDateTime now = QDateTime::currentDateTime();
    const bool have = m_pedsTime.isValid();
    const double moved = have ? distanceM(m_pedsLat, m_pedsLon, m_fix.lat, m_fix.lon) : 1e12;
    // a list classified by older rules is searched again (once: the search stores the current version)
    const bool stale = !have || m_pedsTime.secsTo(now) > 30LL * 86400 || m_pedsClassifier != kPedsClassifierVersion;
    if (!force && !stale && moved <= radiusM / 4.0 && m_pedsRadiusM == radiusM) return;
    if (!force && m_pedsBusyUntil.isValid() && now < m_pedsBusyUntil) return;
    if (!overpassSlot(true, force)) return;
    queryPediatric(m_fix.lat, m_fix.lon, radiusM, 0);
}

void Locator::queryPediatric(double lat, double lon, int radiusM, int mirror)
{
    m_pedsBusy = true;
    if (mirror == 0) { qInfo("beaconfix: pediatric ER search within %d km", radiusM / 1000); emit poisUpdated(); }
    // Only exact key=value lookups over a bbox. Value regexes and around: over a 300 km box ran into the timeout, and so
    // did the six [name~…,i] filters (a regex on "name" scans every name in the box): the live query gave up after 79 s,
    // while the same query without them answered in 11 s. Every hospital, hospital building and clinic in the boxes comes
    // back and PoiClassify does the name matching here, which also yields the general ERs (kept within 80 km) and a
    // children's hospital's campus ER without an around: pass. Dense areas: ~1800 elements, ~0.7 MB, ~30 s.
    const QString far = bboxFor(lat, lon, radiusM), urg = bboxFor(lat, lon, 50000);
    const QString q = QStringLiteral(
        "[out:json][timeout:%3];("
        "nwr[amenity=hospital](%1);nwr[healthcare=hospital](%1);"
        "nwr[building=hospital](%1);nwr[\"emergency:paediatric\"=yes](%1);"
        "nwr[amenity=clinic](%2);nwr[amenity=doctors](%2);nwr[amenity=urgent_care](%2);"
        "nwr[healthcare=clinic](%2);nwr[healthcare=urgent_care](%2);nwr[healthcare=doctor](%2);"
        ");out center tags qt;").arg(far, urg).arg(kPedsServerTimeoutS);
    QNetworkRequest req{QUrl(QString::fromLatin1(kOverpassMirrors[mirror]))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(USER_AGENT));
    // Longer than the server's own limit plus queueing: hanging up first lost the server's error remark and left it
    // running the query in our slot
    req.setTransferTimeout((kPedsServerTimeoutS + 30) * 1000);
    QUrlQuery body; body.addQueryItem(QStringLiteral("data"), q);
    QNetworkReply *rep = m_nam.post(req, body.toString(QUrl::FullyEncoded).toUtf8());
    connect(rep, &QNetworkReply::finished, this, [this, rep, lat, lon, radiusM, mirror] {
        rep->deleteLater();
        const int http = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
        const QString remark = doc.object()["remark"].toString();
        if (rep->error() != QNetworkReply::NoError || !doc.isObject() || remark.contains(QLatin1String("error"), Qt::CaseInsensitive)) {
            const QString why = !remark.isEmpty() ? remark.left(80) : http ? QStringLiteral("HTTP %1").arg(http) : rep->errorString();
            qWarning("beaconfix: pediatric ER search failed (%s): %s", kOverpassMirrors[mirror], qPrintable(why));
            if (mirror < kOverpassLastMirror) {              // the next mirror, after a breath
                QTimer::singleShot(5000, this, [this, lat, lon, radiusM, mirror] { queryPediatric(lat, lon, radiusM, mirror + 1); });
                return;
            }
            const QDateTime now = QDateTime::currentDateTime();
            m_pedsBusy = false; m_pedsFailed = true;
            // Back off 10, 20, 40 … minutes (at most 4 h) after failures in a row: every attempt costs two heavy
            // requests (both mirrors), and a server that is busy now stays busy for a while
            const int backoffS = pedsBackoffS(++m_pedsFailCount);
            m_pedsBusyUntil = now.addSecs(backoffS);
            if (http == 429 || http == 504) m_overpassCoolUntil = now.addSecs(60);
            m_pedsNote = QStringLiteral("Overpass busy — will retry");
            qInfo("beaconfix: pediatric ER search: retry in %d min (failure %d in a row)", backoffS / 60, m_pedsFailCount);
            m_pedsRetryTimer.start(backoffS * 1000 + 5000);   // what we had stays: m_pedsPois and the database rows are untouched
            emit poisUpdated();
            overpassDone();
            return;
        }
        QList<PoiClassify::Element> els;
        for (const QJsonValue &v : doc.object()["elements"].toArray()) els << PoiClassify::Element::fromOverpass(v.toObject());
        const QList<PoiClassify::Result> res = PoiClassify::classifyAll(els);
        QList<QPair<double, Poi>> peds, urgent, ers;
        for (int i = 0; i < els.size(); ++i) {
            const PoiClassify::Result &r = res[i];
            const PoiClassify::Element &e = els[i];
            if (r.dropped || r.cat.isEmpty() || (e.lat == 0 && e.lon == 0)) continue;
            const double d = distanceM(lat, lon, e.lat, e.lon);
            QList<QPair<double, Poi>> *to = nullptr;
            if (r.cat == QLatin1String("peds_er") && d <= radiusM) to = &peds;
            else if (r.cat == QLatin1String("peds_urgent") && d <= 50000) to = &urgent;
            else if (r.cat == QLatin1String("health") && r.emergency && d <= 80000) to = &ers;
            if (!to) continue;
            Poi pt;
            if (!makePoi(e, r, &pt)) continue;
            pt.scope = QStringLiteral("far");
            to->append(qMakePair(d, pt));
        }
        auto nearest = [](QList<QPair<double, Poi>> &l, int n) {
            std::sort(l.begin(), l.end(), [](const QPair<double, Poi> &a, const QPair<double, Poi> &b) { return a.first < b.first; });
            QList<Poi> out; for (int i = 0; i < l.size() && i < n; ++i) out << l[i].second;
            return out;
        };
        // Pediatric ERs: the nearest 5 confirmed ones (tier 1) first, then the nearest 5 others. By distance alone, three
        // nearer false positives (a psychiatric centre, an office building, a department) pushed a real pediatric ER
        // inside the radius off the list.
        QList<int> tiers; QList<double> pedsDist;
        for (const auto &x : peds) { tiers << x.second.peds; pedsDist << x.first; }
        QList<Poi> keptPeds;
        for (int i : PoiClassify::keepPediatricEr(tiers, pedsDist, 5)) keptPeds << peds[i].second;
        m_pedsPois = keptPeds + nearest(urgent, 5) + nearest(ers, 8);
        m_pedsLat = lat; m_pedsLon = lon; m_pedsRadiusM = radiusM;
        m_pedsTime = QDateTime::currentDateTime();
        m_pedsBusy = false; m_pedsFailed = false; m_pedsBusyUntil = QDateTime(); m_pedsFailCount = 0;
        if (m_pedsClassifier != kPedsClassifierVersion) { m_pedsClassifier = kPedsClassifierVersion; QSettings().setValue("pedsClassifier", m_pedsClassifier); }
        m_pedsNote.clear();
        m_pedsRetryTimer.stop();
        qInfo("beaconfix: pediatric ER search: %d pediatric ER, %d pediatric urgent care, %d ER within reach (%d elements)",
              int(keptPeds.size()), int(qMin<qsizetype>(5, urgent.size())), int(qMin<qsizetype>(8, ers.size())), int(els.size()));
        savePedsPois();
        rebuildMergedPois();
        emit poisUpdated();
        overpassDone();
    });
}

void Locator::rebuildMergedPois()
{
    QList<Poi> all = m_pois;
    QSet<QString> seen;
    for (const Poi &p : m_pois) seen.insert(poiKey(p));
    for (const Poi &p : m_pedsPois) if (!seen.contains(poiKey(p))) all << p;
    for (Poi &p : all) {
        p.driveS = p.driveM = 0; p.driveEst = true;
        if (m_fix.valid && driveCat(p.cat)) PoiClassify::driveEstimate(distanceM(m_fix.lat, m_fix.lon, p.lat, p.lon), &p.driveS, &p.driveM);
    }
    m_allPois = all;
}

bool Locator::helpOrigin(double *lat, double *lon) const
{
    if (m_fix.valid) { *lat = m_fix.lat; *lon = m_fix.lon; return true; }
    if (m_poiTime.isValid()) { *lat = m_poiLat; *lon = m_poiLon; return true; }
    if (m_pedsTime.isValid()) { *lat = m_pedsLat; *lon = m_pedsLon; return true; }
    return false;
}

Locator::HelpPicks Locator::helpPicks() const
{
    double lat = 0, lon = 0;
    const bool have = helpOrigin(&lat, &lon);
    QList<PoiClassify::HelpCandidate> c;
    QList<const Poi *> ref;
    for (const Poi &p : m_allPois) {
        if (p.cat != QLatin1String("peds_er") && p.cat != QLatin1String("peds_urgent") && p.cat != QLatin1String("health")) continue;
        PoiClassify::HelpCandidate h;
        h.cat = p.cat; h.peds = p.peds; h.campus = p.campus; h.emergency = p.emergency;
        h.distM = have ? distanceM(lat, lon, p.lat, p.lon) : 0;
        if (p.driveS > 0) h.driveS = p.driveS; else PoiClassify::driveEstimate(h.distM, &h.driveS, nullptr);
        c << h; ref << &p;
    }
    const PoiClassify::HelpPicks k = PoiClassify::pickHelp(c);
    auto at = [&](int i) -> const Poi * { return i >= 0 ? ref[i] : nullptr; };
    HelpPicks out;
    out.pediatric = at(k.pediatric); out.closer = at(k.pediatricCloser); out.urgent = at(k.pediatricUrgent); out.hospital = at(k.hospital);
    return out;
}

// What to tell the family next to the pediatric pick (docs/API.md "pediatricNote"), first match wins
QString Locator::pedsNoteFor(const Poi *pick) const
{
    const bool cached = m_pedsTime.isValid();
    if (!cached && m_pedsFailed) return QStringLiteral("Overpass busy — will retry");
    if (!cached && m_pedsBusy) return QStringLiteral("Looking for pediatric ERs within %1 km…").arg(m_pedsRadiusKm);
    if (!pick) {
        if (cached) return QStringLiteral("No pediatric ER mapped within %1 km — go to the nearest ER").arg(m_pedsRadiusM > 0 ? m_pedsRadiusM / 1000 : m_pedsRadiusKm);
        if (m_fix.valid && (m_fix.source == QLatin1String("ip") || m_fix.accuracy > 5000))
            return QStringLiteral("The pediatric ER search needs a precise fix — go to the nearest ER");
        return {};
    }
    if (cached) {
        double lat = 0, lon = 0;
        const double away = helpOrigin(&lat, &lon) ? distanceM(m_pedsLat, m_pedsLon, lat, lon) : 0;
        const qint64 age = m_pedsTime.secsTo(QDateTime::currentDateTime());
        if (m_pedsFailed || age > 30LL * 86400 || away > m_pedsRadiusM / 2.0)
            return QStringLiteral("Saved %1 ago, %2 km from here — may be incomplete").arg(ageText(age)).arg(qRound(away / 1000.0));
    }
    if (pick->peds == 2) return QStringLiteral("ER not confirmed — call ahead");
    return {};
}

QString Locator::pedsNote() const { return pedsNoteFor(helpPicks().pediatric); }

QJsonObject Locator::poiOriginJson() const
{
    return QJsonObject{{"lat", m_poiLat}, {"lon", m_poiLon}, {"time", m_poiTime.isValid() ? m_poiTime.toString(Qt::ISODate) : QString()}, {"radiusKm", m_poiRadiusM / 1000.0}};
}

QJsonValue Locator::pedsOriginJson() const
{
    if (!m_pedsTime.isValid()) return QJsonValue();
    return QJsonObject{{"lat", m_pedsLat}, {"lon", m_pedsLon}, {"time", m_pedsTime.toString(Qt::ISODate)}, {"radiusKm", m_pedsRadiusM / 1000.0}};
}

// JSON fallback (no map database): pois.json holds the near list and, under "peds", the far one
static QJsonObject poiToJson(const Poi &pt)
{
    QJsonObject o{{"cat", pt.cat}, {"name", pt.name}, {"detail", pt.detail}, {"lat", pt.lat}, {"lon", pt.lon},
                  {"type", pt.osmType}, {"id", double(pt.osmId)}, {"wifi", pt.wifi}, {"hours", pt.hours},
                  {"phone", pt.phone}, {"website", pt.website}, {"address", pt.address}, {"wheelchair", pt.wheelchair}, {"emergency", pt.emergency}};
    if (pt.peds) o["peds"] = pt.peds;
    if (!pt.er.isEmpty()) o["er"] = pt.er;
    if (!pt.campus.isEmpty()) o["campus"] = pt.campus;
    return o;
}

static Poi poiFromJson(const QJsonObject &a)
{
    Poi pt;
    pt.cat = a["cat"].toString(); pt.name = a["name"].toString(); pt.detail = a["detail"].toString();
    pt.lat = a["lat"].toDouble(); pt.lon = a["lon"].toDouble();
    pt.osmType = a["type"].toString(); pt.osmId = qint64(a["id"].toDouble());
    pt.wifi = a["wifi"].toBool(); pt.hours = a["hours"].toString(); pt.phone = a["phone"].toString(); pt.website = a["website"].toString();
    pt.address = a["address"].toString(); pt.wheelchair = a["wheelchair"].toString(); pt.emergency = a["emergency"].toBool();
    pt.peds = a["peds"].toInt(); pt.er = a["er"].toString(); pt.campus = a["campus"].toString();
    return pt;
}

void Locator::loadPois()
{
    QFile f(stateDir() + "/pois.json");
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    m_poiLat = o["lat"].toDouble(); m_poiLon = o["lon"].toDouble(); m_poiRadiusM = o["radius"].toInt();
    m_poiTime = QDateTime::fromString(o["time"].toString(), Qt::ISODate);
    for (const QJsonValue &v : o["pois"].toArray()) {
        const Poi pt = poiFromJson(v.toObject());
        if (poiCategory(pt.cat)) m_pois << pt;
    }
    const QJsonObject far = o["peds"].toObject();
    if (!far.isEmpty()) {
        m_pedsLat = far["lat"].toDouble(); m_pedsLon = far["lon"].toDouble(); m_pedsRadiusM = far["radius"].toInt();
        m_pedsTime = QDateTime::fromString(far["time"].toString(), Qt::ISODate);
        for (const QJsonValue &v : far["pois"].toArray()) {
            Poi pt = poiFromJson(v.toObject()); pt.scope = QStringLiteral("far");
            if (poiCategory(pt.cat)) m_pedsPois << pt;
        }
    }
    if (m_fix.valid && m_fix.source == QLatin1String("ip")) m_poiNote = QStringLiteral("around the approximate (IP) position — may be far off");
}

void Locator::savePois() const
{
    if (m_dbUsable) {
        m_db->savePois(m_pois, m_poiLat, m_poiLon, m_poiRadiusM, m_poiTime);
        savePedsPois();                                   // far rows a replaced near row was shadowing come back
        return;
    }
    QJsonArray arr, far;
    for (const Poi &pt : m_pois) arr.append(poiToJson(pt));
    for (const Poi &pt : m_pedsPois) far.append(poiToJson(pt));
    QJsonObject o{{"lat", m_poiLat}, {"lon", m_poiLon}, {"radius", m_poiRadiusM}, {"time", m_poiTime.toString(Qt::ISODate)}, {"pois", arr}};
    if (m_pedsTime.isValid())
        o["peds"] = QJsonObject{{"lat", m_pedsLat}, {"lon", m_pedsLon}, {"radius", m_pedsRadiusM}, {"time", m_pedsTime.toString(Qt::ISODate)}, {"pois", far}};
    QFile f(stateDir() + "/pois.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void Locator::savePedsPois() const
{
    if (!m_pedsTime.isValid()) return;
    if (m_dbUsable) { m_db->savePois(m_pedsPois, m_pedsLat, m_pedsLon, m_pedsRadiusM, m_pedsTime, QStringLiteral("far")); return; }
    savePois();
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
            for (const QJsonValue &v : ro["seen"].toArray()) {
                const QJsonObject so = v.toObject();
                r.seen.append({so["lat"].toDouble(), so["lon"].toDouble(), so["acc"].toDouble(50000),
                               QDateTime::fromString(so["t"].toString(), Qt::ISODate)});
            }
            r.freq = ro["freq"].toInt();
            r.security = ro["security"].toString(); r.secFlags = ro["secFlags"].toInt(); r.wpaFlags = ro["wpaFlags"].toInt(); r.rsnFlags = ro["rsnFlags"].toInt(); r.maxKbps = ro["maxKbps"].toInt(); r.adhoc = ro["adhoc"].toBool();
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

void Locator::rewriteHistory() const
{
    if (m_dbUsable) { m_db->saveFixes(m_history); return; }
    QFile h(stateDir() + "/history.jsonl");
    if (!h.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    for (const Fix &fx : m_history) h.write(QJsonDocument(fx.toJson()).toJson(QJsonDocument::Compact) + "\n");
}

void Locator::appendHistory(const Fix &fx)
{
    m_history.append(fx);
    if (m_dbUsable) { m_db->appendFix(fx); return; }
    QFile h(stateDir() + "/history.jsonl");
    if (h.open(QIODevice::WriteOnly | QIODevice::Append))
        h.write(QJsonDocument(fx.toJson()).toJson(QJsonDocument::Compact) + "\n");
}

void Locator::noteObservations(const QList<AccessPoint> &aps, const Fix &at)
{
    const QString cell = QStringLiteral("%1,%2").arg(qRound(at.lat * 20)).arg(qRound(at.lon * 20));
    noteScanCell(at);
    for (const AccessPoint &ap : aps) {
        ApRecord &r = m_apRecords[ap.bssid];
        if (!ap.ssid.isEmpty()) r.ssid = ap.ssid;
        if (ap.frequency) r.freq = ap.frequency;
        r.cells.insert(cell);
        // New observation if we've moved ≥ 25 m since the last one for this AP, or it's the first
        bool add = r.obs.isEmpty();
        if (!add) {
            const ApObservation &last = r.obs.last();
            const double moved = distanceM(last.lat, last.lon, at.lat, at.lon);
            // A new sample once we moved about half the fixes' error (at least 12 m), or ten minutes later at the
            // same spot: the estimator clusters places and takes each place's median itself, so denser samples help
            // and a long stay cannot outvote the rest (docs/GRADING.md §5). Memory is thinned past 600 samples.
            add = moved >= qMax(12.0, 0.5 * qMax(last.acc, at.accuracy)) || !last.device.isEmpty()
                  || (last.time.isValid() && at.time.isValid() && last.time.secsTo(at.time) >= 600);
            if (!add && at.accuracy < last.acc) {
                // same spot, better fix: replace the last observation
                r.obs.last().lat = at.lat; r.obs.last().lon = at.lon; r.obs.last().acc = at.accuracy;
                r.obs.last().dbm = ap.dbm; r.obs.last().time = at.time; r.obs.last().dirty = true;
                r.fitDirty = true; queueRefit(ap.bssid);
            }
        }
        if (add) {
            ApObservation ob; ob.lat = at.lat; ob.lon = at.lon; ob.acc = at.accuracy; ob.dbm = ap.dbm; ob.time = at.time; ob.dirty = true;
            r.obs.append(ob);
            thinObservations(r);
            // Nudge the existing fit right away; the full refit follows in the 10 s batch
            if (r.fit.valid) { Estimator::Obs eo; eo.lat = at.lat; eo.lon = at.lon; eo.acc = at.accuracy; eo.dbm = ap.dbm; eo.t = at.time.toSecsSinceEpoch(); r.fit = Estimator::update(r.fit, eo); }
            r.fitDirty = true; queueRefit(ap.bssid);
        }
    }
    saveApRecords();
    queueWigle();
}

// ── position refinement ──────────────────────────────────────────────────────
QList<Estimator::Obs> Locator::obsFor(const ApRecord &r, const QString &bssid) const
{
    Q_UNUSED(bssid)
    QList<Estimator::Obs> out; out.reserve(r.obs.size());
    for (const ApObservation &o : r.obs) {
        if (o.acc <= 0 || o.acc > 300) continue;                         // a coarse fix says nothing about the beacon
        Estimator::Obs e; e.lat = o.lat; e.lon = o.lon; e.acc = o.acc; e.dbm = o.dbm; e.t = o.time.isValid() ? o.time.toSecsSinceEpoch() : 0; e.device = o.device;
        out << e;
    }
    return out;
}

// Priors per band (docs/GRADING.md §3): P0 from the free-space loss at 1 m, n from the anchors' environment
// calibration when there is one, and the calibration κ from the anchors' leave-one-out test.
Estimator::Options Locator::estimatorOptions(int freqMHz) const
{
    Estimator::Options o;
    const QString band = freqMHz >= 5925 ? QStringLiteral("6") : freqMHz >= 4900 ? QStringLiteral("5") : QStringLiteral("2.4");
    if (freqMHz <= 0) { o.p0Mean = -42; o.p0Sd = 10; o.defaultN = 2.5; }            // unknown band: wide enough for 2.4 and 5 GHz
    else if (band == QLatin1String("2.4")) { o.p0Mean = -40; o.defaultN = 2.4; }
    else if (band == QLatin1String("5")) { o.p0Mean = -47; o.defaultN = 2.7; }
    else { o.p0Mean = -48; o.defaultN = 2.7; }
    if (freqMHz > 0 && m_envSamples.value(band) >= 3 && m_envRls.contains(band)) {
        const RangeMath::Rls2 &e = m_envRls[band];
        o.defaultN = std::clamp(e.n, 1.6, 4.5);
        o.nSd = std::clamp(std::sqrt(std::max(0.0, e.S[1][1])) + 0.3, 0.3, 0.6);
    }
    o.kappa = m_kappa;
    return o;
}

void Locator::noteScanCell(const Fix &at)
{
    if (!at.valid || at.accuracy <= 0 || at.accuracy > 60 || at.source == QLatin1String("ip")) return;
    const QString key = scanCellKey(at.lat, at.lon);
    const qint64 t = at.time.isValid() ? at.time.toSecsSinceEpoch() : QDateTime::currentSecsSinceEpoch();
    ScanCell &c = m_scanCells[key];
    if (c.count == 0) { c.lat = at.lat; c.lon = at.lon; c.first = t; m_scanIndex[scanBucket(at.lat, at.lon)] << key; }
    else { c.lat += (at.lat - c.lat) / (c.count + 1); c.lon += (at.lon - c.lon) / (c.count + 1); }
    ++c.count; c.last = std::max(c.last, t);
    if (m_dbUsable) m_db->saveScanCell({key, c.lat, c.lon, c.count, c.first, c.last});
}

QList<Estimator::Miss> Locator::missesFor(const QList<Estimator::Obs> &obs) const
{
    QList<Estimator::Miss> out;
    if (obs.isEmpty() || m_scanCells.isEmpty()) return out;
    double lat = 0, lon = 0; qint64 first = 0, last = 0; int own = 0;
    QSet<QString> heard;
    for (const Estimator::Obs &o : obs) {
        lat += o.lat / obs.size(); lon += o.lon / obs.size();
        if (!o.device.isEmpty()) continue;
        ++own; heard.insert(scanCellKey(o.lat, o.lon));
        if (o.t > 0) { first = first ? std::min(first, o.t) : o.t; last = std::max(last, o.t); }
    }
    if (!own) return out;                               // only our own scans say where it was NOT heard
    if (first == 0) { first = 0; last = std::numeric_limits<qint64>::max() / 2; }
    struct C { double d; Estimator::Miss m; };
    QList<C> cand;
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        const QString b = QStringLiteral("%1:%2").arg(qFloor(lat * 100) + dy).arg(qFloor(lon * 100) + dx);
        const auto keys = m_scanIndex.constFind(b);
        if (keys == m_scanIndex.constEnd()) continue;
        for (const QString &k : *keys) {
            if (heard.contains(k)) continue;
            const ScanCell &c = m_scanCells[k];
            if (c.last < first - 7 * 86400 || c.first > last + 30 * 86400) continue;   // not while the AP was around
            const double d = distanceM(lat, lon, c.lat, c.lon);
            if (d > 600) continue;
            // a heard sample nearby means this cell is not a miss
            bool near = false; for (const Estimator::Obs &o : obs) if (distanceM(o.lat, o.lon, c.lat, c.lon) < 15) { near = true; break; }
            if (near) continue;
            Estimator::Miss m; m.lat = c.lat; m.lon = c.lon; m.count = c.count;
            cand.append({d, m});
        }
    }
    std::sort(cand.begin(), cand.end(), [](const C &a, const C &b) { return a.d < b.d; });
    for (int i = 0; i < cand.size() && i < 150; ++i) out << cand[i].m;
    return out;
}

Estimator::Context Locator::contextFor(const ApRecord &r, const QString &bssid, const QList<Estimator::Obs> &obs) const
{
    Estimator::Context c;
    c.deviceOffset = m_devOffsets;
    c.misses = missesFor(obs);
    if (r.wigle) { c.external.has = true; c.external.lat = r.wLat; c.external.lon = r.wLon; c.external.acc = 50; c.external.source = QStringLiteral("placed"); }
    if (r.fit.valid || r.fit.kind == QLatin1String("mobile")) { c.hasPrev = true; c.prev = r.fit; }
    AccessPoint probe; probe.bssid = bssid; probe.ssid = r.ssid;
    c.mobile = isTravelling(bssid) || isHome(probe);                     // moves with us: graded M, never placed
    return c;
}

// Keep the in-memory sample list bounded: drop the oldest sample of the most crowded ~15 m cell (the database keeps them all)
void Locator::thinObservations(ApRecord &r)
{
    if (r.obs.size() <= 600) return;
    while (r.obs.size() > 500) {
        QHash<QString, QList<int>> cells;
        for (int i = 0; i < r.obs.size(); ++i)
            if (r.obs[i].id != 0 && !r.obs[i].dirty) cells[scanCellKey(r.obs[i].lat, r.obs[i].lon) + r.obs[i].device].append(i);   // only rows already stored
        int drop = 0, most = 0;
        for (auto it = cells.constBegin(); it != cells.constEnd(); ++it) if (it->size() > most) { most = it->size(); drop = it->first(); }
        if (most <= 1) { int i = 0; while (i < r.obs.size() && (r.obs[i].id == 0 || r.obs[i].dirty)) ++i; if (i >= r.obs.size()) break; r.obs.removeAt(i); continue; }
        // several at once: the oldest half of the crowded cells above 4 samples
        QList<int> kill;
        for (auto it = cells.constBegin(); it != cells.constEnd() && r.obs.size() - kill.size() > 500; ++it)
            if (it->size() > 4) for (int j = 0; j < it->size() / 2 && r.obs.size() - kill.size() > 500; ++j) kill << it->at(j);
        if (kill.isEmpty()) kill << drop;
        std::sort(kill.begin(), kill.end(), std::greater<int>());
        for (int i : kill) r.obs.removeAt(i);
    }
}

void Locator::queueRefit(const QString &bssid)
{
    if (m_standalone || bssid.isEmpty()) return;
    m_refitQueue.insert(bssid);
    if (!m_refitTimer.isActive() && !m_bulkImport) m_refitTimer.start();   // an import drains the queue after the last batch
}

bool Locator::refitOne(const QString &bssid, qint64 now)
{
    auto it = m_apRecords.find(bssid);
    if (it == m_apRecords.end()) return false;
    if (m_pins.contains(bssid.toUpper())) { it->fitDirty = false; m_upgradePending.remove(bssid); return false; }   // anchored: never refitted
    // A multi-BSSID group (one radio, several networks) is fitted once, from all its members' samples
    QStringList members{bssid}; QHash<QString, double> offs{{bssid, 0.0}}; QString ref = bssid;
    const auto g = m_groups.constFind(bssid);
    if (g != m_groups.constEnd() && g->size > 1) {
        ref = g->ref; members.clear(); offs.clear();
        for (auto m = m_groups.constBegin(); m != m_groups.constEnd(); ++m)
            if (m->ref == ref && m_apRecords.contains(m.key()) && !m_pins.contains(m.key().toUpper())) { members << m.key(); offs.insert(m.key(), m->offsetDb); }
        std::sort(members.begin(), members.end());
        if (members.isEmpty()) { members << bssid; offs.insert(bssid, 0.0); ref = bssid; }
    }
    QList<Estimator::Obs> obs; int freq = 0;
    for (const QString &m : members) {
        const ApRecord &mr = m_apRecords[m];
        if (!freq && mr.freq) freq = mr.freq;
        for (Estimator::Obs o : obsFor(mr, m)) { o.dbm = int(std::lround(o.dbm - offs.value(m))); obs << o; }
    }
    const ApRecord &r0 = m_apRecords.contains(ref) ? m_apRecords[ref] : it.value();
    Estimator::Context ctx = contextFor(r0, ref, obs);
    for (const QString &m : members) { AccessPoint pr; pr.bssid = m; pr.ssid = m_apRecords[m].ssid; if (isTravelling(m) || isHome(pr)) ctx.mobile = true; }
    const Estimator::Fit fit = obs.isEmpty() && !ctx.mobile ? Estimator::Fit() : Estimator::fitAp(obs, now, estimatorOptions(freq), ctx);
    const bool upgrading = m_upgradePending.contains(bssid);
    for (const QString &m : members) {
        Estimator::Fit fm = fit;
        fm.p0 += offs.value(m);
        if (members.size() > 1) { fm.groupRef = ref; fm.groupSize = members.size(); }
        ApRecord &r = m_apRecords[m];
        const Estimator::Fit before = r.fit;
        r.fit = fm; r.fitDirty = false; r.fitCurrent = true;
        m_upgradePending.remove(m);
        const bool changed = before.kind != fm.kind || before.grade != fm.grade || distanceM(before.lat, before.lon, fm.lat, fm.lon) > 1.0
                             || std::fabs(before.r95 - fm.r95) > 1.0;
        if (m_dbUsable) { m_db->saveEstimate(m, fm); if (changed && (fm.valid || fm.kind == QLatin1String("mobile"))) m_db->appendEstimateHistory(m, fm); saveRecord(m); }
        if (upgrading || m != (members.contains(bssid) ? bssid : members.first())) continue;   // one event per group; none while recomputing on upgrade
        const bool fixNow = fm.valid && fm.kind == QLatin1String("fix"), fixBefore = before.valid && before.kind != QLatin1String("region");
        // "Refined": the fit moved or tightened noticeably → an ap_refit event with the vantage points that made it
        if (fixNow && fixBefore) {
            const double moved = distanceM(before.lat, before.lon, fm.lat, fm.lon);
            const bool movedEnough = moved > qMax(5.0, 0.10 * before.acc), tighter = fm.acc < before.acc * 0.85;
            if (movedEnough || tighter || before.grade != fm.grade) {
                BeaconEvent ev; ev.type = QStringLiteral("ap_refit"); ev.bssid = m; ev.ssid = r.ssid; ev.kind = QStringLiteral("trilat");
                ev.hasPos = true; ev.lat = fm.lat; ev.lon = fm.lon; ev.hasFrom = true; ev.fromLat = before.lat; ev.fromLon = before.lon;
                // Vantage points: strongest sample per 25 m cell, at least one per contributing device, ≤ 6
                struct Cell { double lat, lon; int dbm; QString device; };
                QList<Cell> cells;
                for (const Estimator::Obs &o : obs) {
                    bool merged = false;
                    for (Cell &c : cells) if (distanceM(c.lat, c.lon, o.lat, o.lon) < 25) { if (o.dbm > c.dbm) { c.dbm = o.dbm; c.lat = o.lat; c.lon = o.lon; c.device = o.device; } merged = true; break; }
                    if (!merged) cells.append({o.lat, o.lon, o.dbm, o.device});
                }
                std::sort(cells.begin(), cells.end(), [](const Cell &a, const Cell &b) { return a.dbm > b.dbm; });
                QList<Cell> pick; QSet<QString> devs;
                for (const Cell &c : cells) if (!devs.contains(c.device)) { devs.insert(c.device); pick << c; }   // one per device first
                for (const Cell &c : cells) { if (pick.size() >= 6) break; bool dup = false; for (const Cell &p : pick) if (p.lat == c.lat && p.lon == c.lon) dup = true; if (!dup) pick << c; }
                if (pick.size() > 6) pick = pick.mid(0, 6);
                QJsonArray vps; for (const Cell &c : pick) vps.append(QJsonObject{{"lat", c.lat}, {"lon", c.lon}, {"dbm", c.dbm}, {"device", c.device.isEmpty() ? ourDeviceName() : c.device}});
                ev.extra = QJsonObject{{"acc", fm.acc}, {"prevAcc", before.acc}, {"n", fm.n}, {"vantage", fm.vantage}, {"rms", fm.rms}, {"movedM", moved}, {"vantagePoints", vps},
                                       {"grade", fm.grade}, {"prevGrade", before.grade}, {"score", fm.score}, {"r95", fm.r95}};
                ev.text = QStringLiteral("Refined %1: ±%2 m → ±%3 m, grade %4 (%5 samples, %6 places)").arg(r.ssid.isEmpty() ? QStringLiteral("(hidden)") : r.ssid)
                              .arg(qRound(before.r95 > 0 ? before.r95 : before.acc * 2.45)).arg(qRound(fm.r95)).arg(fm.grade).arg(fm.n).arg(fm.vantage);
                logEvent(ev);
            }
        }
        if (fixNow && !fixBefore) {
            BeaconEvent ev; ev.type = QStringLiteral("ap_placed"); ev.bssid = m; ev.ssid = r.ssid; ev.kind = QStringLiteral("trilat");
            ev.hasPos = true; ev.lat = fm.lat; ev.lon = fm.lon;
            ev.extra = QJsonObject{{"grade", fm.grade}, {"score", fm.score}, {"r95", fm.r95}};
            ev.text = QStringLiteral("%1 positioned from %2 of your samples (±%3 m, grade %4)").arg(r.ssid.isEmpty() ? QStringLiteral("(hidden)") : r.ssid).arg(fm.n).arg(qRound(fm.r95)).arg(fm.grade);
            logEvent(ev);
        }
    }
    return fit.valid;
}

void Locator::refitQueued()
{
    // Time-boxed batches: a few hundred fits at start-up (or after an import) must not freeze the tray
    if (m_refitQueue.isEmpty()) { finishUpgradeRefit(); return; }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QElapsedTimer clock; clock.start();
    QSet<QString> done;
    QStringList order = m_refitQueue.values(); std::sort(order.begin(), order.end());
    int n = 0;
    for (const QString &b : order) {
        m_refitQueue.remove(b);
        const auto g = m_groups.constFind(b);
        const QString key = g != m_groups.constEnd() && g->size > 1 ? g->ref : b;
        if (done.contains(key)) continue;                // the group was fitted in this batch already
        done.insert(key);
        refitOne(b, now); ++n;
        if (clock.elapsed() > 150) break;
    }
    if (m_dbUsable) m_db->flush();
    emit refitDone(n);
    emit scanUpdated();
    if (!m_refitQueue.isEmpty()) { m_refitTimer.start(50); return; }
    m_refitTimer.setInterval(10000);
    finishUpgradeRefit();
    checkAchievements();
}

// Estimates from an older engine (or imported ones) are recomputed in time-boxed batches — none deleted
void Locator::queueUpgradeRefits()
{
    if (m_standalone || !m_dbUsable) return;
    for (auto it = m_apRecords.constBegin(); it != m_apRecords.constEnd(); ++it)
        if (!it->fitCurrent && (!it->obs.isEmpty() || it->fit.valid)) { m_upgradePending.insert(it.key()); m_refitQueue.insert(it.key()); }
    if (m_upgradePending.isEmpty()) { finishUpgradeRefit(); return; }
    m_upgradeActive = true;
    QTimer::singleShot(3000, this, [this] { if (!m_refitTimer.isActive()) m_refitTimer.start(50); });
}

void Locator::finishUpgradeRefit()
{
    if (!m_upgradePending.isEmpty() || !m_dbUsable) return;
    if (m_db->kv(QStringLiteral("estimator_version")).toInt() < Estimator::kVersion) {
        m_db->setKv(QStringLiteral("estimator_version"), QString::number(Estimator::kVersion));
        m_db->flush();
    }
    if (!m_upgradeActive) return;
    m_upgradeActive = false;
    emit statusMessage(QStringLiteral("Every beacon's estimate was recomputed and graded"));
    QTimer::singleShot(2000, this, [this] { calibrateEstimator(); });     // groups and device offsets from the new fits
}

int Locator::Refit()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    int fitted = 0, tried = 0;
    m_refitQueue.clear(); m_refitTimer.stop(); m_refitTimer.setInterval(10000);
    QSet<QString> done;
    QStringList keys = m_apRecords.keys(); std::sort(keys.begin(), keys.end());
    for (const QString &b : keys) {
        const ApRecord &r = m_apRecords[b];
        if (r.obs.isEmpty()) continue;
        const auto g = m_groups.constFind(b);
        const QString key = g != m_groups.constEnd() && g->size > 1 ? g->ref : b;
        if (done.contains(key)) continue;
        done.insert(key);
        ++tried;
        if (refitOne(b, now)) ++fitted;
    }
    if (m_dbUsable) m_db->flush();
    finishUpgradeRefit();
    emit statusMessage(QStringLiteral("Refit %1 beacons: %2 positioned").arg(tried).arg(fitted));
    emit refitDone(tried);
    emit scanUpdated();
    checkAchievements();
    return fitted;
}

int Locator::refitCount() const
{
    int n = 0;
    for (const ApRecord &r : m_apRecords) if (r.fit.valid && r.fit.quality != QLatin1String("none") && r.fit.kind != QLatin1String("region")) ++n;
    return n;
}

// ── data from other devices (LAN API, sync) ──────────────────────────────────
int Locator::ingestObservations(const QJsonArray &observations, const QString &device, QString *error)
{
    if (!m_dbUsable) { if (error) *error = QStringLiteral("map database not writable"); return -1; }
    QHash<QString, QList<ApObservation>> added;
    const int n = m_db->addObservations(observations, device, error, &added);
    if (n <= 0) return n;
    QHash<QString, QPair<QString, int>> names;      // bssid → (ssid, freq) from the rows, for records that have none yet
    for (const QJsonValue &v : observations) { const QJsonObject o = v.toObject(); const QString b = o["bssid"].toString().toUpper().trimmed(); if (!names.contains(b) && (!o["ssid"].toString().isEmpty() || o["freq"].toInt() > 0)) names.insert(b, qMakePair(o["ssid"].toString(), o["freq"].toInt())); }
    for (auto it = added.constBegin(); it != added.constEnd(); ++it) {
        ApRecord &r = m_apRecords[it.key()];
        if (const auto nm = names.constFind(it.key()); nm != names.constEnd()) { if (r.ssid.isEmpty()) r.ssid = nm->first; if (!r.freq && nm->second > 0) r.freq = nm->second; }
        for (const ApObservation &o : it.value()) { r.obs.append(o); if (r.fit.valid) { Estimator::Obs eo; eo.lat = o.lat; eo.lon = o.lon; eo.acc = o.acc; eo.dbm = o.dbm; eo.t = o.time.toSecsSinceEpoch(); r.fit = Estimator::update(r.fit, eo); } }
        thinObservations(r);
        r.fitDirty = true;
        queueRefit(it.key());                           // even one sample gives a region (docs/GRADING.md §5)
    }
    if (!m_bulkImport) emit scanUpdated();          // an import repaints once at the end, not per batch
    return n;
}

int Locator::mergePeerAps(const QJsonArray &aps, const QString &device)
{
    if (!m_dbUsable) return -1;
    QStringList touched;
    const int n = m_db->mergePeerAps(aps, device, &touched);
    for (const QJsonValue &v : aps) {
        const QJsonObject o = v.toObject();
        const QString b = o["bssid"].toString().toUpper().trimmed();
        if (!touched.contains(b)) continue;
        ApRecord &r = m_apRecords[b];
        if (r.ssid.isEmpty()) r.ssid = o["ssid"].toString();
        if (!r.freq && o["freq"].toInt() > 0) r.freq = o["freq"].toInt();
        const double acc = o["acc"].toDouble(100);
        if (!r.hasPeer() || r.peerFrom == device || r.peerAcc > acc) { r.peerLat = o["lat"].toDouble(); r.peerLon = o["lon"].toDouble(); r.peerAcc = acc; r.peerFrom = device; }
    }
    if (n > 0) emit scanUpdated();
    return n;
}

int Locator::appendPeerFixes(const QJsonArray &fixes, const QString &device)
{
    if (!m_dbUsable) return -1;
    const int n = m_db->appendPeerFixes(fixes, device);
    // The newest of them is where that device is now
    QJsonObject newest; QDateTime newestT;
    for (const QJsonValue &v : fixes) {
        const QJsonObject f = v.toObject();
        const QDateTime t = QDateTime::fromString(f["time"].toString(), Qt::ISODate);
        if (t.isValid() && (!newestT.isValid() || t > newestT) && f["lat"].isDouble()) { newest = f; newestT = t; }
    }
    if (!newest.isEmpty()) noteDevicePosition(newest["device"].toString().isEmpty() ? device : newest["device"].toString(), QString(), newest["lat"].toDouble(), newest["lon"].toDouble(),
                                              newest["acc"].toDouble(), newestT, newest["source"].toString(), 0, newest["identity"].toString(), QString(), newest["place"].toString());
    else noteDeviceSeen(device);
    return n;
}

static bool importerDevice(const QString &d) { const QString l = d.toLower(); return l == QLatin1String("timeline") || l == QLatin1String("wigle") || l == QLatin1String("gpx") || l == QLatin1String("kml") || l.startsWith(QLatin1String("import")); }

void Locator::noteDeviceSeen(const QString &device, const QString &kind)
{
    if (device.isEmpty() || device == ourDeviceName() || importerDevice(device)) return;
    DevicePos &d = m_devicePos[device];
    d.device = device; if (!kind.isEmpty()) d.kind = kind;
    d.lastSeen = QDateTime::currentDateTime();
    if (!d.online) { d.online = true; if (d.time.isValid()) { BeaconEvent ev; ev.type = QStringLiteral("device_online"); ev.text = QStringLiteral("%1 is online").arg(device); ev.extra = QJsonObject{{"device", device}}; logEvent(ev); } }
}

void Locator::noteDevicePosition(const QString &device, const QString &kind, double lat, double lon, double acc, const QDateTime &time, const QString &source, int beacons,
                                 const QString &identityId, const QString &identityName, const QString &place)
{
    if (device.isEmpty() || device == ourDeviceName() || importerDevice(device)) return;
    DevicePos &d = m_devicePos[device];
    const bool had = d.time.isValid();
    const double moved = had ? distanceM(d.lat, d.lon, lat, lon) : 0;
    if (had && time.isValid() && time < d.time) { noteDeviceSeen(device, kind); return; }   // older than what we have
    d.device = device; if (!kind.isEmpty()) d.kind = kind; if (!identityId.isEmpty()) d.identityId = identityId; if (!identityName.isEmpty()) d.identityName = identityName;
    if (d.kind.isEmpty() || d.kind == QLatin1String("laptop") || d.kind == QLatin1String("device")) d.kind = kindForDevice(device, d.kind);
    d.lat = lat; d.lon = lon; d.acc = acc; d.time = time.isValid() ? time : QDateTime::currentDateTime(); d.source = source; if (beacons) d.beacons = beacons; if (!place.isEmpty()) d.place = place;
    noteDeviceSeen(device, d.kind);
    if (had && moved > 100) {
        BeaconEvent ev; ev.type = QStringLiteral("device"); ev.hasPos = true; ev.lat = lat; ev.lon = lon;
        const double dist = m_fix.valid ? distanceM(m_fix.lat, m_fix.lon, lat, lon) : -1;
        ev.extra = QJsonObject{{"device", device}, {"kind", d.kind}, {"movedM", moved}, {"distanceM", dist}, {"acc", acc}};
        ev.text = QStringLiteral("%1 moved%2%3").arg(device, d.place.isEmpty() ? QString() : QStringLiteral(" to ") + d.place, dist >= 0 ? QStringLiteral(", %1 away").arg(dist < 1000 ? QStringLiteral("%1 m").arg(qRound(dist)) : QStringLiteral("%1 km").arg(dist / 1000, 0, 'f', 1)) : QString());
        logEvent(ev);
    }
    emit scanUpdated();
}

QJsonArray Locator::linkedDevices() const
{
    QJsonArray arr;
    QList<DevicePos> l = m_devicePos.values();
    std::sort(l.begin(), l.end(), [](const DevicePos &a, const DevicePos &b) { return a.time > b.time; });
    for (const DevicePos &d : l) {
        if (!d.time.isValid()) continue;
        QJsonObject o = d.toJson();
        o["kind"] = kindForDevice(d.device, d.kind);
        if (m_fix.valid) o["distanceM"] = distanceM(m_fix.lat, m_fix.lon, d.lat, d.lon);
        if (m_ranging) { const QJsonObject r = m_ranging->estimateJson(d.device); if (!r.isEmpty()) o["range"] = r; }   // measured, not fix-to-fix
        arr.append(o);
    }
    return arr;
}

QString Locator::LinkedDevices() const { return QString::fromUtf8(QJsonDocument(linkedDevices()).toJson(QJsonDocument::Compact)); }

void Locator::noteSightings(const QList<AccessPoint> &aps, const Fix &at)
{
    if (!at.valid || aps.isEmpty()) return;
    const double acc = at.accuracy > 0 ? at.accuracy : 50000;
    bool changed = false;
    for (const AccessPoint &ap : aps) {
        ApRecord &r = m_apRecords[ap.bssid];
        if (!ap.ssid.isEmpty()) r.ssid = ap.ssid;
        if (ap.frequency) r.freq = ap.frequency;
        if (!ap.security.isEmpty()) { r.security = ap.security; r.secFlags = ap.secFlags; r.wpaFlags = ap.wpaFlags; r.rsnFlags = ap.rsnFlags; r.maxKbps = ap.maxKbps; r.adhoc = ap.adhoc; }
        bool found = false;
        for (ApSighting &s : r.seen) {
            if (distanceM(s.lat, s.lon, at.lat, at.lon) <= qMax(s.acc, acc)) {
                if (acc < s.acc) { s.lat = at.lat; s.lon = at.lon; s.acc = acc; }
                s.time = at.time; found = true; break;
            }
        }
        if (!found) {
            r.seen.append({at.lat, at.lon, acc, at.time});
            while (r.seen.size() > 24) r.seen.removeFirst();
            changed = true;
        }
    }
    if (changed) { saveApRecords(); emit scanUpdated(); }
}

QHash<QString, int> Locator::apFlags() const
{
    QHash<QString, int> flags;
    flags.reserve(m_apRecords.size());
    for (auto it = m_apRecords.constBegin(); it != m_apRecords.constEnd(); ++it) flags.insert(it.key(), apFlag(it.key()));
    return flags;
}

int Locator::apFlag(const QString &bssid) const
{
    const auto it = m_apRecords.constFind(bssid);
    if (it == m_apRecords.constEnd()) return 0;
    AccessPoint ap; ap.bssid = it.key(); ap.ssid = it->ssid;
    int f = 0;
    if (isHome(ap)) f |= 1;
    if (!m_pins.contains(bssid.toUpper()) && (isTravelling(it.key()) || (!m_notTravelling.contains(it.key()) && looksMobile(it->ssid)))) f |= 2;
    if (matchesIgnore(ap) || it->ssid.endsWith(QLatin1String("_nomap")) || it->ssid.contains(QLatin1String("_optout"))) f |= 4;
    return f;
}

// ── Internal mapping database ────────────────────────────────────────────────
void Locator::loadFromDb()
{
    QSet<QString> trav, notTrav;
    const QHash<QString, ApRecord> recs = m_db->loadApRecords(&trav, &notTrav);
    if (!recs.isEmpty()) { m_apRecords = recs; m_travelling = trav; m_notTravelling = notTrav; }
    const QList<Fix> fixes = m_db->loadFixes();
    if (!fixes.isEmpty()) m_history = fixes;
    loadImported();
    QList<Poi> pois; double plat = 0, plon = 0; int prad = 0; QDateTime ptime;
    if (m_db->loadPois(&pois, &plat, &plon, &prad, &ptime)) {
        m_pois.clear(); for (const Poi &p : pois) if (poiCategory(p.cat)) m_pois << p;
        m_poiLat = plat; m_poiLon = plon; m_poiRadiusM = prad; m_poiTime = ptime;
    }
    QList<Poi> far; double flat = 0, flon = 0; int frad = 0; QDateTime ftime;
    if (m_db->loadPois(&far, &flat, &flon, &frad, &ftime, QStringLiteral("far"))) {       // the pediatric ER search
        m_pedsPois.clear(); for (const Poi &p : far) if (poiCategory(p.cat)) m_pedsPois << p;
        m_pedsLat = flat; m_pedsLon = flon; m_pedsRadiusM = frad; m_pedsTime = ftime;
    }
    const QHash<QString, double> elev = m_db->loadElevation();
    for (auto it = elev.constBegin(); it != elev.constEnd(); ++it) m_elevCache.insert(it.key(), it.value());
    const QHash<QString, QDateTime> ach = m_db->loadAchievements();
    for (Achievement &a : m_achievements) if (ach.contains(a.key) && ach.value(a.key).isValid()) a.unlocked = ach.value(a.key);
    for (const QJsonValue &v : m_db->latestFixesByDevice()) {          // where our other devices were last
        const QJsonObject f = v.toObject();
        const QString dev = f["device"].toString();
        if (dev.isEmpty() || dev == ourDeviceName() || importerDevice(dev)) continue;
        DevicePos &d = m_devicePos[dev]; d.device = dev; d.lat = f["lat"].toDouble(); d.lon = f["lon"].toDouble(); d.acc = f["acc"].toDouble();
        d.time = QDateTime::fromString(f["time"].toString(), Qt::ISODate); d.source = f["source"].toString(); d.place = f["place"].toString();
        d.kind = kindForDevice(dev);
    }
}

// First run with the database: everything the JSON files held goes in, and the files are kept as *.migrated
void Locator::migrateJsonToDb()
{
    m_db->saveApRecords(m_apRecords, m_travelling, m_notTravelling, apFlags());
    m_db->saveFixes(m_history);
    if (!m_pois.isEmpty() || m_poiTime.isValid()) m_db->savePois(m_pois, m_poiLat, m_poiLon, m_poiRadiusM, m_poiTime);
    if (m_pedsTime.isValid()) m_db->savePois(m_pedsPois, m_pedsLat, m_pedsLon, m_pedsRadiusM, m_pedsTime, QStringLiteral("far"));
    if (!m_elevCache.isEmpty()) m_db->saveElevation(m_elevCache);
    m_db->saveAchievements(m_achievements);
    int moved = 0;
    for (const char *f : {"aps.json", "history.jsonl", "pois.json", "elev.json", "achievements.json"}) {
        const QString p = stateDir() + QLatin1Char('/') + QLatin1String(f);
        if (!QFile::exists(p)) continue;
        QFile::remove(p + QStringLiteral(".migrated"));
        if (QFile::rename(p, p + QStringLiteral(".migrated"))) ++moved;
    }
    m_db->flush();
    qInfo("beaconfix: migrated %d JSON state file(s) into the map database (%d APs, %d fixes)", moved, int(m_apRecords.size()), int(m_history.size()));
    emit statusMessage(QStringLiteral("Map database created from %1 APs and %2 stops").arg(m_apRecords.size()).arg(m_history.size()));
}

int Locator::rebuildDbFromJson()
{
    if (!m_dbUsable) return -1;
    int n = 0;
    for (const char *f : {"aps.json", "history.jsonl", "pois.json", "elev.json", "achievements.json"}) {
        const QString p = stateDir() + QLatin1Char('/') + QLatin1String(f), m = p + QStringLiteral(".migrated");
        if (QFile::exists(m) && !QFile::exists(p) && QFile::copy(m, p)) ++n;
    }
    if (!n) return 0;
    loadState(); loadPois(); loadElevationCache(); loadAchievements();     // merges the files over what we have
    m_db->saveApRecords(m_apRecords, m_travelling, m_notTravelling, apFlags());
    m_db->saveFixes(m_history);
    m_db->savePois(m_pois, m_poiLat, m_poiLon, m_poiRadiusM, m_poiTime);
    if (m_pedsTime.isValid()) m_db->savePois(m_pedsPois, m_pedsLat, m_pedsLon, m_pedsRadiusM, m_pedsTime, QStringLiteral("far"));
    m_db->saveElevation(m_elevCache);
    m_db->saveAchievements(m_achievements);
    for (const char *f : {"aps.json", "history.jsonl", "pois.json", "elev.json", "achievements.json"}) QFile::remove(stateDir() + QLatin1Char('/') + QLatin1String(f));
    m_db->flush();
    emit FixChanged(); emit scanUpdated();
    return n;
}

QString Locator::DbStats() const { return m_db ? QString::fromUtf8(QJsonDocument(m_db->stats()).toJson(QJsonDocument::Compact)) : QStringLiteral("{\"open\":false}"); }
bool Locator::DbExport(const QString &path)
{
    if (!m_db || !m_db->isOpen()) return false;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(m_db->exportJson()).toJson(QJsonDocument::Compact));
    emit statusMessage(QStringLiteral("Database exported to %1").arg(path));
    return true;
}
void Locator::loadImported()
{
    m_imported.clear();
    if (!m_db || !m_db->isOpen()) return;
    for (const char *dev : {"timeline", "wigle", "gpx", "kml"}) m_imported += m_db->peerFixes(QString::fromLatin1(dev));
    std::stable_sort(m_imported.begin(), m_imported.end(), [](const Fix &a, const Fix &b) { return a.time < b.time; });
}

QString Locator::Import(const QString &path, const QString &optsJson)
{
    auto json = [](const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); };
    if (m_importing) return json({{"ok", false}, {"error", "an import is already running"}});
    if (!m_dbUsable) return json({{"ok", false}, {"error", "map database not writable"}});
    const Importers::Options opt = Importers::Options::fromJson(QJsonDocument::fromJson(optsJson.toUtf8()).object());
    m_importing = true; m_bulkImport = true;
    emit statusMessage(QStringLiteral("Importing %1…").arg(QFileInfo(path).fileName()));
    Importers::Summary sum;
    int lastPct = -1;
    const bool ok = Importers::run(path, opt, this, [&](int pct, const QString &stage) {
        if (pct == lastPct && pct != 100) return;
        lastPct = pct;
        emit importProgress(json({{"file", path}, {"percent", pct}, {"stage", stage}}));
    }, &sum);
    m_importing = false; m_bulkImport = false;
    loadImported();
    if (!m_refitQueue.isEmpty() && !m_refitTimer.isActive()) m_refitTimer.start();   // the batched refit of everything the import touched
    if (ok) {
        BeaconEvent ev; ev.type = QStringLiteral("import");
        ev.text = QStringLiteral("Imported %1: %2 positions, %3 Wi-Fi observations, %4 visits").arg(QFileInfo(path).fileName()).arg(sum.positions + sum.tracks).arg(sum.observations).arg(sum.visits);
        ev.extra = sum.toJson(); logEvent(ev);
        emit statusMessage(ev.text);
        emit scanUpdated(); emit FixChanged();
    }
    QJsonObject res{{"ok", ok}, {"summary", sum.toJson()}};
    if (!ok) res["error"] = sum.error;
    emit importProgress(json({{"file", path}, {"percent", 100}, {"stage", "done"}, {"done", true}, {"ok", ok}, {"summary", sum.toJson()}}));
    return json(res);
}

int Locator::DbImport(const QString &path)
{
    if (!m_dbUsable) return -1;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return -1;
    QString err;
    const int n = m_db->importJson(QJsonDocument::fromJson(f.readAll()).object(), &err);
    if (n >= 0) { loadFromDb(); rebuildGroups(); queueUpgradeRefits(); emit FixChanged(); emit scanUpdated(); emit statusMessage(QStringLiteral("Imported %1 rows from %2").arg(n).arg(path)); }
    return n;
}

// Offline self-location: places we have been before. Two or more heard beacons with a
// position of their own in the database put us at their signal-weighted centroid.
bool Locator::tryInternal(const QList<AccessPoint> &usable)
{
    if (!m_db || !m_db->isOpen() || usable.size() < 2) return false;
    QList<QPair<QString, int>> heard;
    for (const AccessPoint &ap : usable) heard << qMakePair(ap.bssid, ap.dbm);
    double lat = 0, lon = 0, acc = 0; int used = 0;
    if (!m_db->estimate(heard, &lat, &lon, &acc, &used, nullptr, 2, 150) || acc > 150) return false;
    Fix f; f.valid = true; f.lat = lat; f.lon = lon; f.accuracy = acc;
    f.source = QStringLiteral("wifi"); f.provider = QStringLiteral("internal");
    f.time = QDateTime::currentDateTime(); f.apCount = m_aps.size(); f.apUsed = used;
    accept(f);
    finish(true, QStringLiteral("Internal map fix from %1 access points (±%2 m) — a place we have been before").arg(used).arg(qRound(acc)));
    return true;
}

void Locator::saveRecord(const QString &bssid)
{
    auto it = m_apRecords.find(bssid);
    if (it == m_apRecords.end()) return;
    if (m_dbUsable) { m_db->saveRecord(bssid, it.value(), apFlag(bssid)); return; }
    saveApRecords();
}

void Locator::saveApRecords()
{
    if (m_dbUsable) { m_db->saveApRecords(m_apRecords, m_travelling, m_notTravelling, apFlags()); return; }
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
        if (!r.seen.isEmpty()) {
            QJsonArray seen;
            for (const ApSighting &sg : r.seen)
                seen.append(QJsonObject{{"lat", sg.lat}, {"lon", sg.lon}, {"acc", sg.acc}, {"t", sg.time.toString(Qt::ISODate)}});
            ro["seen"] = seen;
        }
        if (r.freq) ro["freq"] = r.freq;
        if (!r.security.isEmpty()) { ro["security"] = r.security; ro["secFlags"] = r.secFlags; ro["wpaFlags"] = r.wpaFlags; ro["rsnFlags"] = r.rsnFlags; ro["maxKbps"] = r.maxKbps; if (r.adhoc) ro["adhoc"] = true; }
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
    const Stats st = stats();
    ts << "  <metadata><name>BeaconFix trip</name><desc>" << QStringLiteral("%1 stops · %2 km between precise stops · %3").arg(st.stops).arg(st.distanceAllKm, 0, 'f', 1).arg(st.rank).toHtmlEscaped()
       << "</desc><time>" << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << "</time></metadata>\n";
    for (const Stop &s : stops()) {
        const Fix &fx = s.fix;
        ts << "  <wpt lat=\"" << QString::number(fx.lat, 'f', 6) << "\" lon=\"" << QString::number(fx.lon, 'f', 6) << "\">\n";
        if (fx.hasElevation()) ts << "    <ele>" << QString::number(fx.elevation, 'f', 1) << "</ele>\n";
        ts << "    <time>" << fx.time.toUTC().toString(Qt::ISODate) << "</time>\n"
           << "    <name>" << fx.place.toHtmlEscaped() << "</name>\n"
           << "    <desc>" << fx.source << (fx.accuracy >= 0 ? QStringLiteral(" ±%1 m").arg(qRound(fx.accuracy)) : QString())
           << (s.dwellSecs >= 0 ? QStringLiteral(" · stayed %1").arg(durationText(s.dwellSecs)) : QString()) << "</desc>\n"
           << "  </wpt>\n";
    }
    ts << "  <trk><name>BeaconFix track</name><trkseg>\n";
    for (const Fix &fx : m_history) {
        ts << "    <trkpt lat=\"" << QString::number(fx.lat, 'f', 6) << "\" lon=\"" << QString::number(fx.lon, 'f', 6) << "\">";
        if (fx.hasElevation()) ts << "<ele>" << QString::number(fx.elevation, 'f', 1) << "</ele>";
        ts << "<time>" << fx.time.toUTC().toString(Qt::ISODate) << "</time></trkpt>\n";
    }
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


// ── Sync with another BeaconFix ──────────────────────────────────────────────
// A laptop carried around feeds the desktop in the RV (and gets its map back), the phone's
// desktop feeds the laptop, and so on. Each side keeps two cursors per peer in the database:
// "pushed" (the last change sequence of ours the peer has) and "pulled" (theirs we have).
static QString syncFile() { return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/sworrl/beaconfix-sync.json"); }
static QString peerKey(const QString &url) { const QUrl u(url); return QStringLiteral("%1:%2").arg(u.host()).arg(u.port(47822)); }
static QString ourDeviceName() { QString h = QSysInfo::machineHostName(); if (h.isEmpty()) h = QHostInfo::localHostName(); return h.isEmpty() ? QStringLiteral("beaconfix") : h; }

void Locator::loadSyncPeers()
{
    m_syncPeers.clear();
    QFile f(syncFile());
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object()["peers"].toArray()) {
        const QJsonObject o = v.toObject();
        SyncPeer p; p.url = o["url"].toString().trimmed(); p.token = o["token"].toString(); p.name = o["name"].toString(); p.minutes = o["minutes"].toInt(15);
        p.last = QDateTime::fromString(o["last"].toString(), Qt::ISODate); p.lastResult = o["lastResult"].toString(); p.ok = o["ok"].toBool();
        if (!p.url.isEmpty()) m_syncPeers << p;
    }
}

void Locator::saveSyncPeers() const
{
    QJsonArray arr;
    for (const SyncPeer &p : m_syncPeers)
        arr.append(QJsonObject{{"url", p.url}, {"token", p.token}, {"name", p.name}, {"minutes", p.minutes}, {"last", p.last.isValid() ? p.last.toString(Qt::ISODate) : QString()}, {"lastResult", p.lastResult}, {"ok", p.ok}});
    QDir().mkpath(QFileInfo(syncFile()).path());
    QSaveFile f(syncFile());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(QJsonObject{{"peers", arr}}).toJson(QJsonDocument::Indented));
    f.commit();
}

void Locator::setSyncPeers(const QList<SyncPeer> &peers)
{
    QList<SyncPeer> keep;
    for (const SyncPeer &p : peers) {
        if (p.url.trimmed().isEmpty()) continue;
        SyncPeer q = p; q.url = p.url.trimmed(); if (q.url.endsWith(QLatin1Char('/'))) q.url.chop(1);
        for (const SyncPeer &old : m_syncPeers) if (peerKey(old.url) == peerKey(q.url)) { q.last = old.last; q.lastResult = old.lastResult; q.ok = old.ok; }
        keep << q;
    }
    m_syncPeers = keep;
    saveSyncPeers();
}

// One round with peer i: hello → push our changes → pull theirs. Runs asynchronously; the
// result lands in m_syncPeers[i] and syncFinished().
void Locator::syncStep(int i)
{
    if (m_syncBusy || i < 0 || i >= m_syncPeers.size() || !m_dbUsable) return;
    m_syncBusy = true;
    const SyncPeer peer = m_syncPeers[i];
    const QString base = peer.url + QStringLiteral("/api/v1/"), key = peerKey(peer.url);
    auto auth = QSharedPointer<QByteArray>::create("Bearer " + peer.token.toUtf8());   // may be replaced by an identity sign-in below
    struct Ctx { int pushedObs = 0, pushedFixes = 0, pulledObs = 0, pulledAps = 0, pulledFixes = 0, rounds = 0; bool reauth = false; QString peerName; };
    auto identityAuth = QSharedPointer<std::function<void()>>::create();   // set below; used by push when a token is rejected
    auto ctx = QSharedPointer<Ctx>::create();
    auto done = [this, i, key](bool ok, const QString &msg) {
        m_syncBusy = false;
        if (i < m_syncPeers.size()) { m_syncPeers[i].last = QDateTime::currentDateTime(); m_syncPeers[i].lastResult = msg; m_syncPeers[i].ok = ok; saveSyncPeers(); }
        if (m_dbUsable) m_db->flush();
        emit syncFinished(key, ok, msg);
        emit statusMessage((ok ? QStringLiteral("Sync with %1: ") : QStringLiteral("Sync with %1 failed: ")).arg(key) + msg);
        if (ok) { emit scanUpdated(); emit FixChanged(); }
    };
    auto request = [base, auth](const QString &ep) { QNetworkRequest r(QUrl(base + ep)); r.setRawHeader("Authorization", *auth); r.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json")); r.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(USER_AGENT));
                                                     r.setTransferTimeout(ep.startsWith(QLatin1String("db/")) ? 180000 : 30000); return r; };   // a first merge of hundreds of samples can take the peer a while

    // Pull loop (after the push): GET db/changes?since=<pulled>
    auto pull = QSharedPointer<std::function<void()>>::create();
    *pull = [this, ctx, key, request, done, pull] {
        const qint64 since = m_db->kv(QStringLiteral("sync:%1:pulled").arg(key)).toLongLong();
        QNetworkReply *rep = m_nam.get(request(QStringLiteral("db/changes?since=%1&limit=2000").arg(since)));
        connect(rep, &QNetworkReply::finished, this, [this, rep, ctx, key, done, pull] {
            rep->deleteLater();
            const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
            if (rep->error() != QNetworkReply::NoError) { done(false, QStringLiteral("pull: %1").arg(o["error"].toString().isEmpty() ? rep->errorString() : o["error"].toString())); return; }
            const QString me = ourDeviceName();
            QJsonArray obs; for (const QJsonValue &v : o["observations"].toArray()) if (v.toObject()["device"].toString() != me) obs.append(v);   // our own samples coming back
            QJsonArray fixes; for (const QJsonValue &v : o["fixes"].toArray()) if (v.toObject()["device"].toString() != me) fixes.append(v);
            QJsonArray aps; for (const QJsonValue &v : o["aps"].toArray()) if (v.toObject()["lat"].isDouble() && v.toObject()["source"].toString() != QLatin1String("peer")) aps.append(v);
            const QString from = ctx->peerName.isEmpty() ? key : ctx->peerName;
            ctx->pulledObs += qMax(0, ingestObservations(obs, from));
            ctx->pulledAps += qMax(0, mergePeerAps(aps, from));
            ctx->pulledFixes += qMax(0, appendPeerFixes(fixes, from));
            mergeAnchors(o["anchors"].toArray());                                  // newest placedAt / deletedAt wins
            m_db->setKv(QStringLiteral("sync:%1:pulled").arg(key), QString::number(qint64(o["cursor"].toDouble())));
            if (o["more"].toBool() && ++ctx->rounds < 50) { (*pull)(); return; }
            done(true, QStringLiteral("pushed %1 samples + %2 stops, pulled %3 samples, %4 positions, %5 stops").arg(ctx->pushedObs).arg(ctx->pushedFixes).arg(ctx->pulledObs).arg(ctx->pulledAps).arg(ctx->pulledFixes));
        });
    };
    // Push loop: POST db/sync with our changes since <pushed>
    auto push = QSharedPointer<std::function<void()>>::create();
    *push = [this, ctx, key, request, done, pull, push, identityAuth] {
        const qint64 since = m_db->kv(QStringLiteral("sync:%1:pushed").arg(key)).toLongLong();
        bool more = false; qint64 cursor = 0;
        QJsonObject ch = m_db->changesSince(since, 500, &more, &cursor);      // small pushes: the peer merges + queues refits per request
        QJsonArray obs; for (const QJsonValue &v : ch["observations"].toArray()) if (v.toObject()["device"].toString().isEmpty()) obs.append(v);   // ours only, not what peers gave us
        QJsonArray fixes; for (const QJsonValue &v : ch["fixes"].toArray()) if (v.toObject()["device"].toString().isEmpty()) fixes.append(v);
        QJsonArray aps; for (const QJsonValue &v : ch["aps"].toArray()) { const QJsonObject a = v.toObject(); if (a["source"].toString() == QLatin1String("trilat") || a["source"].toString() == QLatin1String("placed")) aps.append(a); }
        const QJsonArray anchorsOut = ch["anchors"].toArray();
        if (obs.isEmpty() && fixes.isEmpty() && aps.isEmpty() && anchorsOut.isEmpty()) { m_db->setKv(QStringLiteral("sync:%1:pushed").arg(key), QString::number(cursor)); (*pull)(); return; }
        QJsonObject body{{"device", ourDeviceName()}, {"observations", obs}, {"aps", aps}, {"fixes", fixes}, {"anchors", anchorsOut}, {"sinceCursor", double(m_db->kv(QStringLiteral("sync:%1:pulled").arg(key)).toLongLong())}};
        QNetworkReply *rep = m_nam.post(request(QStringLiteral("db/sync")), QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(rep, &QNetworkReply::finished, this, [this, rep, ctx, key, cursor, more, done, pull, push, identityAuth] {
            rep->deleteLater();
            const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
            if (rep->error() != QNetworkReply::NoError) {
                const int status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 401 && !ctx->reauth && m_identity && m_identity->unlocked() && *identityAuth) { ctx->reauth = true; (*identityAuth)(); return; }   // stale token: sign in again
                done(false, QStringLiteral("push: %1").arg(o["error"].toString().isEmpty() ? rep->errorString() : o["error"].toString())); return;
            }
            ctx->pushedObs += o["accepted"].toObject()["observations"].toInt(); ctx->pushedFixes += o["accepted"].toObject()["fixes"].toInt();
            m_db->setKv(QStringLiteral("sync:%1:pushed").arg(key), QString::number(cursor));
            if (more && ++ctx->rounds < 50) { (*push)(); return; }
            ctx->rounds = 0; (*pull)();
        });
    };
    // Identity sign-in (docs/IDENTITY.md): when we hold no token for this peer and our identity is the peer's
    // (or linked to it), a challenge signature gets us a control token — no pairing code, nothing to type.
    *identityAuth = [this, i, key, base, auth, request, done, push] {
        QNetworkReply *ch = m_nam.get(request(QStringLiteral("identity/challenge")));
        connect(ch, &QNetworkReply::finished, this, [this, ch, i, key, base, auth, request, done, push] {
            ch->deleteLater();
            const QJsonObject c = QJsonDocument::fromJson(ch->readAll()).object();
            if (ch->error() != QNetworkReply::NoError || c["nonce"].toString().isEmpty()) { done(false, QStringLiteral("no token for %1 and its identity challenge failed: %2").arg(key, c["error"].toString().isEmpty() ? ch->errorString() : c["error"].toString())); return; }
            const QString nonce = c["nonce"].toString(), host = c["host"].toString(), dev = ourDeviceName();
            const QByteArray sig = m_identity->sign(Identity::authCanon(host, nonce.toLatin1(), m_identity->id(), dev));
            QJsonObject body{{"id", m_identity->id()}, {"pub", QString::fromLatin1(m_identity->pub().toBase64())}, {"name", m_identity->name()},
                             {"device", QJsonObject{{"name", dev}, {"kind", "desktop"}}}, {"nonce", nonce}, {"sig", QString::fromLatin1(sig.toBase64())}};
            QNetworkReply *au = m_nam.post(request(QStringLiteral("identity/auth")), QJsonDocument(body).toJson(QJsonDocument::Compact));
            connect(au, &QNetworkReply::finished, this, [this, au, i, key, auth, done, push] {
                au->deleteLater();
                const QJsonObject o = QJsonDocument::fromJson(au->readAll()).object();
                if (au->error() != QNetworkReply::NoError || o["token"].toString().isEmpty()) {
                    const QString why = o["error"].toString();
                    done(false, why == QLatin1String("unknown identity") ? QStringLiteral("%1 does not know our identity — link it there (its Devices tab lists our sign-in), or pass --sync-token").arg(key)
                                                                       : QStringLiteral("identity sign-in at %1 failed: %2").arg(key, why.isEmpty() ? au->errorString() : why));
                    return;
                }
                *auth = "Bearer " + o["token"].toString().toUtf8();
                if (i < m_syncPeers.size()) { m_syncPeers[i].token = o["token"].toString(); if (m_syncPeers[i].name.isEmpty()) m_syncPeers[i].name = o["identity"].toObject()["name"].toString(); saveSyncPeers(); }
                (*push)();
            });
        });
    };
    QNetworkReply *hello = m_nam.get(request(QStringLiteral("hello")));
    connect(hello, &QNetworkReply::finished, this, [this, hello, ctx, peer, done, push, identityAuth] {
        hello->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(hello->readAll()).object();
        if (hello->error() != QNetworkReply::NoError) { done(false, QStringLiteral("hello: %1").arg(hello->errorString())); return; }
        bool sync = false, identity = false;
        for (const QJsonValue &v : o["features"].toArray()) { if (v.toString() == QLatin1String("sync")) sync = true; if (v.toString() == QLatin1String("identity")) identity = true; }
        if (!sync) { done(false, QStringLiteral("%1 runs BeaconFix %2 without the sync API (needs 3.4+)").arg(o["hostname"].toString(), o["version"].toString())); return; }
        ctx->peerName = o["hostname"].toString();
        if (peer.token.isEmpty()) {
            if (identity && m_identity && m_identity->unlocked()) { (*identityAuth)(); return; }
            done(false, QStringLiteral("no token for %1 (pass --sync-token, or give both sides the same / a linked identity)").arg(ctx->peerName)); return;
        }
        (*push)();
    });
}

QString Locator::Sync(const QString &url, const QString &token)
{
    if (m_standalone || !m_dbUsable) return QStringLiteral("{\"ok\":false,\"error\":\"needs the running instance with a writable database\"}");
    if (m_syncBusy) return QStringLiteral("{\"ok\":false,\"error\":\"a sync is already running\"}");
    QString u = url.trimmed(); if (u.endsWith(QLatin1Char('/'))) u.chop(1);
    QString peerName;
    if (!u.contains(QLatin1String("://"))) {                  // a peer's name / hostname / address: look it up on the network
        const Mdns::Peer *peer = m_api && m_api->mdns() ? m_api->mdns()->find(u) : nullptr;
        if (!peer) {
            const QHostAddress a(u.section(QLatin1Char(':'), 0, 0));
            if (a.isNull() && !u.contains(QLatin1Char('.'))) return QString::fromUtf8(QJsonDocument(QJsonObject{{"ok", false}, {"error", QStringLiteral("no BeaconFix called \"%1\" on this network (beaconfix --peers lists them)").arg(u)}}).toJson(QJsonDocument::Compact));
            u = QStringLiteral("http://") + u + (u.contains(QLatin1Char(':')) && !a.isNull() && a.protocol() == QAbstractSocket::IPv6Protocol ? QString() : u.contains(QLatin1Char(':')) ? QString() : QStringLiteral(":47822"));
        } else { u = peer->url(); peerName = peer->identityName.isEmpty() ? peer->host : peer->identityName; }
    }
    int idx = -1;
    for (int i = 0; i < m_syncPeers.size(); ++i) if (peerKey(m_syncPeers[i].url) == peerKey(u)) idx = i;
    if (idx < 0) { SyncPeer p; p.url = u; p.token = token; p.name = peerName; p.minutes = 0; m_syncPeers << p; idx = m_syncPeers.size() - 1; saveSyncPeers(); }
    else { if (!token.isEmpty()) m_syncPeers[idx].token = token; if (!peerName.isEmpty()) m_syncPeers[idx].name = peerName; }
    QEventLoop loop; QJsonObject result;
    QMetaObject::Connection c = connect(this, &Locator::syncFinished, &loop, [&](const QString &, bool ok, const QString &msg) { result = QJsonObject{{"ok", ok}, {"message", msg}}; loop.quit(); });
    QTimer::singleShot(180000, &loop, &QEventLoop::quit);
    syncStep(idx);
    loop.exec();
    disconnect(c);
    if (result.isEmpty()) result = QJsonObject{{"ok", false}, {"error", "timed out"}};
    result["peer"] = peerKey(u);
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

// ── Identity + OS integration (D-Bus / CLI entry points) ─────────────────────
QString Locator::IdentityJson() const
{
    if (!m_identity) return QStringLiteral("{}");
    QJsonObject o = m_identity->exists() ? m_identity->publicJson() : QJsonObject{{"exists", false}};
    o["exists"] = m_identity->exists();
    QJsonArray pend; for (const PendingLink &p : m_identity->pending()) pend.append(p.toJson()); o["pending"] = pend;
    o["file"] = Identity::filePath();
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
bool Locator::IdentityCreate(const QString &name)
{
    if (!m_identity) return false;
    QString err;
    const bool ok = m_identity->create(name, QHostInfo::localHostName(), QStringLiteral("desktop"), &err);
    if (!ok) m_lastError = err;
    if (ok) { BeaconEvent ev; ev.type = QStringLiteral("identity"); ev.text = QStringLiteral("Identity created: %1 (%2)").arg(m_identity->name(), m_identity->groupedId()); logEvent(ev); }
    return ok;
}
QString Locator::IdentityExport(const QString &passphrase)
{
    if (!m_identity) return {};
    QString err; const QString t = m_identity->exportBundle(passphrase, &err);
    if (t.isEmpty()) m_lastError = err;
    return t;
}
bool Locator::IdentityImport(const QString &textOrPath, const QString &passphrase)
{
    if (!m_identity) return false;
    QString text = textOrPath.trimmed();
    if (QFile::exists(text)) { QFile f(text); if (f.open(QIODevice::ReadOnly)) text = QString::fromUtf8(f.readAll()).trimmed(); }
    QString err;
    const bool ok = m_identity->importBundle(text, passphrase, QHostInfo::localHostName(), QStringLiteral("desktop"), &err);
    if (!ok) m_lastError = err;
    else { BeaconEvent ev; ev.type = QStringLiteral("identity"); ev.text = QStringLiteral("Identity imported: %1 (%2)").arg(m_identity->name(), m_identity->groupedId()); logEvent(ev); }
    return ok;
}
QString Locator::IdentityLinkPayload() const
{
    if (!m_identity || !m_identity->exists()) return {};
    return Identity::encodeUri(QStringLiteral("link"), m_identity->linkPayload());
}
QString Locator::IdentityAcceptLink(const QString &statementJson)
{
    if (!m_identity) return QStringLiteral("{\"error\":\"no identity\"}");
    QString err; LinkStatement done;
    const QJsonObject o = QJsonDocument::fromJson(statementJson.toUtf8()).object();
    if (!m_identity->acceptLink(LinkStatement::fromJson(o), &err, &done)) return QString::fromUtf8(QJsonDocument(QJsonObject{{"error", err}}).toJson(QJsonDocument::Compact));
    BeaconEvent ev; ev.type = QStringLiteral("identity"); ev.text = QStringLiteral("Linked with identity %1").arg(Identity::groupId(done.a == m_identity->id() ? done.b : done.a)); logEvent(ev);
    return QString::fromUtf8(QJsonDocument(done.toJson(true)).toJson(QJsonDocument::Compact));
}
bool Locator::IdentityForget() { return m_identity && m_identity->forget(); }
void Locator::IdentityReload() { if (m_identity && m_db) m_identity->load(m_db->key()); }

QString Locator::ApplyOs(bool dryRun)
{
    if (!m_os) return QStringLiteral("{}");
    return QString::fromUtf8(QJsonDocument(m_os->apply(dryRun, true)).toJson(QJsonDocument::Compact));
}
QString Locator::TimeZoneForFix() const
{
    if (!m_os || !m_fix.valid) return {};
    if (!m_os->lastZone().isEmpty()) return m_os->lastZone();
    return OsIntegration::zoneFromTab(m_fix.lat, m_fix.lon, m_countryCode);
}

// ── Nearest help + places by category / group ─────────────────────────────────
QList<Poi> Locator::poisMatching(const QStringList &catsOrGroups, double radiusKm) const
{
    QList<Poi> out;
    for (const Poi &p : m_allPois) {
        const PoiCategory *c = poiCategory(p.cat);
        bool match = catsOrGroups.isEmpty();
        for (const QString &k : catsOrGroups) if (k.compare(p.cat, Qt::CaseInsensitive) == 0 || (c && k.compare(c->group, Qt::CaseInsensitive) == 0) || k == QLatin1String("all")) match = true;
        if (!match) continue;
        if (radiusKm > 0 && m_fix.valid && distanceM(m_fix.lat, m_fix.lon, p.lat, p.lon) > radiusKm * 1000) continue;
        out << p;
    }
    if (m_fix.valid) std::sort(out.begin(), out.end(), [this](const Poi &a, const Poi &b) {
        return distanceM(m_fix.lat, m_fix.lon, a.lat, a.lon) < distanceM(m_fix.lat, m_fix.lon, b.lat, b.lon); });
    return out;
}

QJsonObject Locator::helpPlaceJson(const Poi &p) const
{
    const PoiCategory *c = poiCategory(p.cat);
    QJsonObject a{{"name", p.name.isEmpty() ? (c ? c->label : p.cat) : p.name}, {"lat", p.lat}, {"lon", p.lon}};
    if (m_fix.valid) { a["d"] = qRound(distanceM(m_fix.lat, m_fix.lon, p.lat, p.lon)); a["brg"] = qRound(bearingDeg(m_fix.lat, m_fix.lon, p.lat, p.lon)); }
    if (!p.phone.isEmpty()) a["phone"] = p.phone;
    if (!p.address.isEmpty()) a["address"] = p.address;
    if (!p.hours.isEmpty()) a["hours"] = p.hours;
    if (!p.website.isEmpty()) a["website"] = p.website;
    if (!p.osmType.isEmpty() && p.osmId) a["osm"] = QStringLiteral("https://www.openstreetmap.org/%1/%2").arg(p.osmType).arg(p.osmId);
    int s = p.driveS, m = p.driveM;
    double lat = 0, lon = 0;
    if (s <= 0 && helpOrigin(&lat, &lon)) PoiClassify::driveEstimate(distanceM(lat, lon, p.lat, p.lon), &s, &m);
    if (s > 0) { a["driveS"] = s; a["driveM"] = m; a["driveEst"] = p.driveS > 0 ? p.driveEst : true; }
    return a;
}

QJsonObject Locator::emergencyJson() const
{
    QJsonObject o;
    const QString cc = !m_countryCode.isEmpty() ? m_countryCode : (m_os ? m_os->locale().countryCode : QString());   // name-based guess until the geocode lands
    o["number"] = OsIntegration::emergencyFor(cc);
    o["countryCode"] = cc;
    double lat = 0, lon = 0;
    const bool have = helpOrigin(&lat, &lon);
    auto nearest = [&](const QString &cat) -> QJsonValue {
        const Poi *best = nullptr; double bd = 1e18;
        for (const Poi &p : m_allPois) {
            if (p.cat != cat) continue;
            const double d = have ? distanceM(lat, lon, p.lat, p.lon) : 0;
            if (!best || d < bd) { bd = d; best = &p; }
        }
        return best ? QJsonValue(helpPlaceJson(*best)) : QJsonValue();
    };
    auto pedsPlace = [&](const Poi *p, bool notEr) -> QJsonValue {
        if (!p) return QJsonValue();
        QJsonObject a = helpPlaceJson(*p);
        a["tier"] = p->peds; a["er"] = p->er; a["campusEr"] = p->campus;
        if (notEr) a["notEr"] = true;
        return a;
    };
    const HelpPicks hp = helpPicks();
    o["police"] = nearest(QStringLiteral("police"));
    o["fire"] = nearest(QStringLiteral("fire"));
    o["hospital"] = hp.hospital ? QJsonValue(helpPlaceJson(*hp.hospital)) : QJsonValue();   // the nearest general ER (children's hospitals are "pediatric")
    {   // "urgent" is every clinic and doctor's office: under "Urgent care" only an actual urgent care
        const Poi *best = nullptr; double bd = 1e18;
        for (const Poi &p : m_allPois) {
            if (p.cat != QLatin1String("urgent") || !PoiClassify::isUrgentCare(p.name, p.detail)) continue;
            const double d = have ? distanceM(lat, lon, p.lat, p.lon) : 0;
            if (!best || d < bd) { bd = d; best = &p; }
        }
        QJsonValue u;
        if (best) { QJsonObject a = helpPlaceJson(*best); a["urgentCare"] = true; u = a; }
        o["urgent"] = u;
    }
    o["pharmacy"] = nearest(QStringLiteral("pharmacy"));
    o["vet"] = nearest(QStringLiteral("vet"));
    o["pediatric"] = pedsPlace(hp.pediatric, false);
    o["pediatricCloser"] = pedsPlace(hp.closer, false);
    o["pediatricUrgent"] = pedsPlace(hp.urgent, true);
    o["pediatricNote"] = pedsNoteFor(hp.pediatric);
    o["pediatricSearchKm"] = m_pedsRadiusKm;
    o["pediatricTime"] = m_pedsTime.isValid() ? m_pedsTime.toString(Qt::ISODate) : QString();
    o["origin"] = m_fix.valid ? QJsonValue(QJsonObject{{"lat", m_fix.lat}, {"lon", m_fix.lon}, {"acc", m_fix.accuracy}, {"source", m_fix.source}, {"time", m_fix.time.toString(Qt::ISODate)}})
                              : QJsonValue();
    return o;
}

// ── Anchors (docs/RANGING.md §4) ──────────────────────────────────────────────
void Locator::loadAnchors()
{
    m_anchors = m_db && m_db->isOpen() ? m_db->loadAnchors(false) : QList<BfAnchor>();
    if (m_db && m_db->isOpen()) {                        // the environment fit survives restarts
        const QJsonObject env = QJsonDocument::fromJson(m_db->kv(QStringLiteral("environment")).toUtf8()).object();
        for (auto it = env.begin(); it != env.end(); ++it) {
            const QJsonObject b = it.value().toObject();
            RangeMath::Rls2 r(b["p0"].toDouble(-40), b["n"].toDouble(2.4));
            const QJsonArray S = b["S"].toArray();
            if (S.size() == 3) { r.S[0][0] = S[0].toDouble(); r.S[0][1] = r.S[1][0] = S[1].toDouble(); r.S[1][1] = S[2].toDouble(); }
            m_envRls.insert(it.key(), r); m_envSamples.insert(it.key(), b["samples"].toInt());
        }
    }
    anchorsChanged();
}

void Locator::anchorsChanged()
{
    m_pins.clear();
    const QHash<QString, BfAnchor> pins = ::Anchors::pinnedBssids(m_anchors);
    for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) m_pins.insert(it.key(), it.value());
    // Our own RTT responder sits on the this-computer anchor: it must never be "positioned" from samples
    const QString responder = ::Anchors::normalizeBssid(RangingService::responderInfo().value(QStringLiteral("bssid")).toString());
    if (!responder.isEmpty() && !m_pins.contains(responder)) {
        const BfAnchor *self = nullptr;
        for (const BfAnchor &a : m_anchors) if (a.kind == QLatin1String("this-computer") && (!self || a.placedAt > self->placedAt)) self = &a;
        if (self) m_pins.insert(responder, *self);
    }
    if (m_db) {
        QHash<QString, MapDb::ApPos> dbPins;
        for (auto it = m_pins.constBegin(); it != m_pins.constEnd(); ++it) {
            MapDb::ApPos p; p.bssid = it.key(); p.lat = it->lat; p.lon = it->lon; p.acc = it->accM; p.source = QStringLiteral("anchor");
            dbPins.insert(it.key(), p);
        }
        m_db->setPins(dbPins);
    }
    // A (new) this-computer anchor next to where we are: it is our position from now on
    if (!m_standalone && m_fix.valid) {
        Fix f = m_fix;
        if (anchorFix(f) && (std::fabs(f.lat - m_fix.lat) > 1e-9 || std::fabs(f.lon - m_fix.lon) > 1e-9 || f.accuracy != m_fix.accuracy)) {
            m_fix.lat = f.lat; m_fix.lon = f.lon; m_fix.accuracy = f.accuracy; m_fix.source = f.source; m_fix.provider = f.provider;
            saveState(); emit FixChanged();
        }
    }
    emit scanUpdated();
}

QJsonArray Locator::anchorsJson() const
{
    QJsonArray a;
    for (const BfAnchor &x : m_anchors) a.append(x.toJson(true));
    return a;
}

const BfAnchor *Locator::pinnedAnchor(const QString &bssid) const
{
    const auto it = m_pins.constFind(bssid.toUpper());
    return it == m_pins.constEnd() ? nullptr : &it.value();
}

BfAnchor Locator::setAnchor(const QJsonObject &o, const QString &placedByDefault, bool *ok, QString *error)
{
    if (ok) *ok = false;
    QString err;
    BfAnchor a = BfAnchor::fromJson(o, &err);
    if (!err.isEmpty()) { if (error) *error = err; return a; }
    if (a.deleted) { if (error) *error = QStringLiteral("use DELETE /api/v1/anchors/<id> to remove an anchor"); return a; }
    if (!m_dbUsable) { if (error) *error = QStringLiteral("map database not writable (is the tray running?)"); return a; }
    if (!o.contains(QStringLiteral("placedBy")) && !placedByDefault.isEmpty()) a.placedBy = placedByDefault;
    // RV anchors store where they are relative to the RV reference (and the heading), so they can follow the RV
    if (a.rv && !a.hasRvOffset) {
        const BfAnchor *ref = ::Anchors::reference(m_anchors);
        if (ref && ref->id != a.id) {
            const Stats st = stats();
            ::Anchors::placeRv(a, *ref, st.headingDeg >= 0, st.headingDeg);
        } else {
            a.hasRvOffset = true; a.rvOffset = ::Anchors::Enu{0, 0, 0};
        }
    }
    BfAnchor stored;
    const bool changed = m_db->putAnchor(a, false, &stored);
    if (changed) {
        // At most one ref:true per site: the newest wins, the others are demoted (and that syncs too)
        QList<BfAnchor> all = m_db->loadAnchors(false);
        const QList<BfAnchor> norm = ::Anchors::normalizeSet(all);
        for (const BfAnchor &n : norm)
            for (const BfAnchor &old : all) if (old.id == n.id && old.ref != n.ref) m_db->putAnchor(n, true);
        loadAnchors();
        BeaconEvent ev; ev.type = QStringLiteral("anchor"); ev.hasPos = true; ev.lat = stored.lat; ev.lon = stored.lon;
        ev.text = QStringLiteral("Anchor placed: %1 (%2, ±%3 m)").arg(stored.name.isEmpty() ? stored.kind : stored.name, stored.kind).arg(stored.accM, 0, 'g', 2);
        ev.extra = QJsonObject{{"anchor", stored.id}, {"kind", stored.kind}, {"placedBy", stored.placedBy}};
        logEvent(ev);
    }
    if (ok) *ok = true;
    for (const BfAnchor &x : m_anchors) if (x.id == stored.id) return x;
    return stored;
}

bool Locator::removeAnchor(const QString &id)
{
    if (!m_dbUsable) return false;
    for (const BfAnchor &a : m_anchors) {
        if (a.id != id) continue;
        BfAnchor t; t.id = id; t.deleted = true; t.deletedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        if (!m_db->putAnchor(t, true)) return false;
        loadAnchors();
        BeaconEvent ev; ev.type = QStringLiteral("anchor"); ev.text = QStringLiteral("Anchor removed: %1").arg(a.name.isEmpty() ? a.kind : a.name);
        ev.extra = QJsonObject{{"anchor", id}, {"deleted", true}};
        logEvent(ev);
        return true;
    }
    return false;
}

int Locator::mergeAnchors(const QJsonArray &rows)
{
    if (!m_dbUsable || rows.isEmpty()) return 0;
    int n = 0;
    for (const QJsonValue &v : rows) {
        QString err;
        const BfAnchor a = BfAnchor::fromJson(v.toObject(), &err);
        if (!err.isEmpty() || a.id.isEmpty()) continue;
        if (m_db->putAnchor(a, false)) ++n;
    }
    if (n) loadAnchors();
    return n;
}

bool Locator::anchorFix(Fix &cand) const
{
    if (!cand.valid || cand.source == QLatin1String("ip") || cand.source == QLatin1String("anchor")) return false;
    const ::Anchors::AnchorFix af = ::Anchors::thisComputerFix(m_anchors, cand.lat, cand.lon);
    if (!af.valid) return false;
    cand.provider = cand.provider.isEmpty() ? cand.source : cand.source + QLatin1Char('/') + cand.provider;   // what found us, for the record
    cand.lat = af.lat; cand.lon = af.lon; cand.accuracy = af.acc; cand.source = QStringLiteral("anchor");
    if (af.hasAlt) cand.elevation = af.alt;
    return true;
}

void Locator::maybeReproject(const Fix &cand)
{
    if (!m_dbUsable || !cand.valid || !cand.precise() || cand.source == QLatin1String("anchor")) return;
    const BfAnchor *ref = ::Anchors::reference(m_anchors);
    if (!ref || !ref->rv || !atHome()) return;                     // only an RV that moved (we hear its own networks)
    if (!::Anchors::shouldReproject(ref->lat, ref->lon, cand.lat, cand.lon)) return;
    // The RV's heading now: the bearing of the leg that brought us here, when it is a real leg
    const bool haveHeading = m_fix.valid && m_fix.precise() && distanceM(m_fix.lat, m_fix.lon, cand.lat, cand.lon) > 250;
    const double heading = haveHeading ? bearingDeg(m_fix.lat, m_fix.lon, cand.lat, cand.lon) : 0;
    const QList<BfAnchor> moved = ::Anchors::reprojectAll(m_anchors, cand.lat, cand.lon, false, 0, haveHeading, heading);
    int n = 0;
    for (BfAnchor a : moved) {
        if (a.deleted) continue;
        if (a.kind == QLatin1String("gnss") && a.id != ref->id) continue;   // a GNSS anchor is re-measured, not re-projected
        if (a.id == ref->id) { a.accM = std::max(a.accM, cand.accuracy > 0 ? cand.accuracy : a.accM); if (haveHeading) { a.hasHeading = true; a.headingDeg = heading; } }
        a.placedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);   // newest wins on every peer
        a.source = a.id == ref->id ? QStringLiteral("gps-average") : a.source;
        if (m_db->putAnchor(a, true)) ++n;
    }
    if (!n) return;
    const QString refName = ref->name;
    loadAnchors();
    BeaconEvent ev; ev.type = QStringLiteral("anchor"); ev.hasPos = true; ev.lat = cand.lat; ev.lon = cand.lon;
    ev.text = QStringLiteral("RV moved: %1 anchor(s) re-projected around %2%3").arg(n).arg(refName.isEmpty() ? QStringLiteral("the reference") : refName,
                                                                                       haveHeading ? QStringLiteral(" (heading %1°)").arg(qRound(heading)) : QStringLiteral(" (heading unknown — check them in the picker)"));
    ev.extra = QJsonObject{{"reprojected", n}, {"headingAssumed", !haveHeading}};
    logEvent(ev);
}

// §4.3.3: the desktop, sitting on its this-computer anchor, hears a wifi-ap anchor at a known distance
void Locator::noteAnchorCalibration(const QList<AccessPoint> &aps)
{
    if (m_standalone || m_anchors.isEmpty() || !m_fix.valid || m_fix.source != QLatin1String("anchor")) return;
    const BfAnchor *self = nullptr;
    for (const BfAnchor &a : m_anchors) if (a.kind == QLatin1String("this-computer") && (!self || a.placedAt > self->placedAt)) self = &a;
    if (!self) return;
    static QHash<QString, qint64> last;                              // one sample per AP per 10 min: scans of a still AP are correlated
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    bool changed = false;
    for (const AccessPoint &ap : aps) {
        const BfAnchor *a = pinnedAnchor(ap.bssid);
        if (!a || a->kind != QLatin1String("wifi-ap") || a->id == self->id) continue;
        if (now - last.value(ap.bssid, 0) < 600) continue;
        last.insert(ap.bssid, now);
        const double h = distanceM(self->lat, self->lon, a->lat, a->lon);
        const double dz = (a->hasHeight && self->hasHeight) ? a->heightM - self->heightM : (a->hasAlt && self->hasAlt) ? a->alt - self->alt : 0;
        const double d = std::max(0.5, std::sqrt(h * h + dz * dz));
        const int f = ap.frequency;
        const QString band = f >= 5925 ? QStringLiteral("6") : f >= 4900 ? QStringLiteral("5") : QStringLiteral("2.4");
        if (!m_envRls.contains(band)) m_envRls.insert(band, RangeMath::Rls2(RangeMath::priorP0Wifi(f > 0 ? f : 2437), RangeMath::kNWifi));
        // one scan sample: Rayleigh fade + that AP's own shadowing
        m_envRls[band].update(std::log10(d), ap.dbm, RangeMath::kRayleighDbStd * RangeMath::kRayleighDbStd + RangeMath::kShadowWifi * RangeMath::kShadowWifi);
        m_envSamples[band] += 1;
        changed = true;
    }
    if (changed && m_dbUsable) m_db->setKv(QStringLiteral("environment"), QString::fromUtf8(QJsonDocument(environmentJson()).toJson(QJsonDocument::Compact)));
}

double Locator::environmentN(int freqMHz) const
{
    const QString band = freqMHz >= 5925 ? QStringLiteral("6") : freqMHz >= 4900 ? QStringLiteral("5") : QStringLiteral("2.4");
    if (m_envSamples.value(band) < 3 || !m_envRls.contains(band)) return Estimator::Options().defaultN;
    return std::clamp(m_envRls.value(band).n, 1.5, 4.5);
}

QJsonObject Locator::environmentJson() const
{
    QJsonObject o;
    for (auto it = m_envRls.constBegin(); it != m_envRls.constEnd(); ++it)
        o[it.key()] = QJsonObject{{"p0", it->p0}, {"n", it->n}, {"S", QJsonArray{it->S[0][0], it->S[0][1], it->S[1][1]}}, {"samples", m_envSamples.value(it.key())},
                                  {"sigmaP0", std::sqrt(std::max(0.0, it->S[0][0]))}, {"sigmaN", std::sqrt(std::max(0.0, it->S[1][1]))}};
    return o;
}

// ── the estimator's calibration (docs/GRADING.md §2.3, §3, §4.1) ─────────────
QJsonObject Locator::fitJson(const Estimator::Fit &f) { return Estimator::toApi(f); }

static double medianOf(QList<double> v)
{
    if (v.isEmpty()) return 0;
    std::sort(v.begin(), v.end());
    const int h = v.size() / 2;
    return v.size() % 2 ? v[h] : 0.5 * (v[h - 1] + v[h]);
}

// The same radio behind several BSSIDs (guest network, 2.4/5 GHz, mesh backhaul): same MAC apart from the
// locally-administered bit and the last nibble, heard together in ≥ 5 scans at a steady level difference.
static QString groupKeyOf(const QString &bssid)
{
    const QStringList o = bssid.toUpper().split(QLatin1Char(':'));
    if (o.size() != 6) return QString();
    bool ok = false; const int first = o[0].toInt(&ok, 16);
    if (!ok) return QString();
    return QStringLiteral("%1:%2:%3:%4:%5:%6").arg(first & ~0x02, 2, 16, QLatin1Char('0')).arg(o[1], o[2], o[3], o[4]).arg(o[5].left(1)).toUpper();
}

void Locator::rebuildGroups()
{
    m_groups.clear();
    QHash<QString, QStringList> buckets;
    for (auto it = m_apRecords.constBegin(); it != m_apRecords.constEnd(); ++it) {
        if (it->obs.size() < 5 || m_pins.contains(it.key().toUpper()) || isTravelling(it.key())) continue;
        AccessPoint probe; probe.bssid = it.key(); probe.ssid = it->ssid;
        if (isHome(probe)) continue;
        const QString k = groupKeyOf(it.key());
        if (!k.isEmpty()) buckets[k] << it.key();
    }
    for (auto b = buckets.begin(); b != buckets.end(); ++b) {
        QStringList mem = b.value();
        if (mem.size() < 2) continue;
        std::sort(mem.begin(), mem.end());
        // each member's samples by (device, second)
        QList<QHash<QString, int>> byScan;
        for (const QString &m : mem) {
            QHash<QString, int> h;
            for (const ApObservation &o : m_apRecords[m].obs) if (o.time.isValid()) h.insert(o.device + QLatin1Char('|') + QString::number(o.time.toSecsSinceEpoch()), o.dbm);
            byScan << h;
        }
        struct Edge { int a, b; double d; };
        QList<Edge> edges;
        for (int i = 0; i < mem.size(); ++i)
            for (int j = i + 1; j < mem.size(); ++j) {
                QList<double> diffs;
                for (auto it = byScan[i].constBegin(); it != byScan[i].constEnd(); ++it) {
                    const int bar = it.key().lastIndexOf(QLatin1Char('|'));
                    const QString dev = it.key().left(bar); const qint64 t = it.key().mid(bar + 1).toLongLong();
                    for (qint64 dt = -2; dt <= 2; ++dt) {
                        const auto hit = byScan[j].constFind(dev + QLatin1Char('|') + QString::number(t + dt));
                        if (hit != byScan[j].constEnd()) { diffs << double(hit.value() - it.value()); break; }
                    }
                }
                if (diffs.size() < 5) continue;
                const double med = medianOf(diffs);
                QList<double> dev; for (double d : diffs) dev << std::fabs(d - med);
                if (std::fabs(med) > 10 || 1.4826 * medianOf(dev) > 4) continue;
                // two good fixes far apart are two radios after all
                const Estimator::Fit &fa = m_apRecords[mem[i]].fit, &fb = m_apRecords[mem[j]].fit;
                if (fa.valid && fb.valid && fa.kind == QLatin1String("fix") && fb.kind == QLatin1String("fix") && fa.cxx > 0 && fb.cxx > 0) {
                    const Estimator::Frame fr(fa.lat, fa.lon);
                    const double dx = fr.x(fb.lon), dy = fr.y(fb.lat); double inv[3];
                    if (Estimator::invert2(fa.cxx + fb.cxx, fa.cxy + fb.cxy, fa.cyy + fb.cyy, inv) && dx * dx * inv[0] + 2 * dx * dy * inv[1] + dy * dy * inv[2] > 6) continue;
                }
                edges.append({i, j, med});
            }
        if (edges.isEmpty()) continue;
        // connected components; offsets relative to the member with the most samples (BFS over the verified pairs)
        QList<int> comp(mem.size(), -1);
        for (int s0 = 0; s0 < mem.size(); ++s0) {
            if (comp[s0] >= 0) continue;
            QList<int> stack{s0}; comp[s0] = s0; QList<int> members;
            while (!stack.isEmpty()) {
                const int c = stack.takeLast(); members << c;
                for (const Edge &e : edges) {
                    const int o = e.a == c ? e.b : e.b == c ? e.a : -1;
                    if (o >= 0 && comp[o] < 0) { comp[o] = s0; stack << o; }
                }
            }
            if (members.size() < 2) continue;
            int ref = members.first();
            for (int m : members) if (m_apRecords[mem[m]].obs.size() > m_apRecords[mem[ref]].obs.size()) ref = m;
            QHash<int, double> off{{ref, 0.0}}; QList<int> queue{ref};
            while (!queue.isEmpty()) {
                const int c = queue.takeFirst();
                for (const Edge &e : edges) {
                    if (e.a == c && !off.contains(e.b)) { off.insert(e.b, off[c] + e.d); queue << e.b; }
                    else if (e.b == c && !off.contains(e.a)) { off.insert(e.a, off[c] - e.d); queue << e.a; }
                }
            }
            for (int m : members) m_groups.insert(mem[m], GroupInfo{mem[ref], off.value(m), int(members.size())});
        }
    }
}

// Per-device level offsets: how much louder another device hears the same beacons than this host (§4 of the plan:
// shrunk towards 0, this host fixed at 0), from graded fixes heard by both
void Locator::calibrateDeviceOffsets()
{
    QHash<QString, QList<double>> perDev;
    for (auto it = m_apRecords.constBegin(); it != m_apRecords.constEnd(); ++it) {
        const Estimator::Fit &f = it->fit;
        if (!f.valid || f.kind != QLatin1String("fix") || f.grade.isEmpty() || f.grade > QLatin1String("D")) continue;
        QHash<QString, QList<double>> res;
        for (const ApObservation &o : it->obs) {
            if (o.acc <= 0 || o.acc > 100) continue;
            const double d = std::sqrt(std::pow(distanceM(o.lat, o.lon, f.lat, f.lon), 2) + 9.0);
            res[o.device] << double(o.dbm) - m_devOffsets.value(o.device, 0.0) - Estimator::modelDbm(f.p0, f.pathloss, d);
        }
        if (res.size() < 2 || !res.contains(QString())) continue;
        const double host = medianOf(res.value(QString()));
        for (auto d = res.constBegin(); d != res.constEnd(); ++d) if (!d.key().isEmpty() && d->size() >= 2) perDev[d.key()] << medianOf(*d) - host;
    }
    QSet<QString> changed;
    for (auto it = perDev.constBegin(); it != perDev.constEnd(); ++it) {
        if (it->size() < 3) continue;
        double sum = 0; for (double v : *it) sum += v;
        const double step = sum / (it->size() + 0.25);
        if (std::fabs(step) < 0.25) continue;
        m_devOffsets[it.key()] = std::clamp(m_devOffsets.value(it.key(), 0.0) + step, -25.0, 25.0);
        if (std::fabs(step) > 1.0) changed.insert(it.key());
    }
    if (m_dbUsable) {
        QJsonObject o; for (auto it = m_devOffsets.constBegin(); it != m_devOffsets.constEnd(); ++it) o[it.key()] = std::round(it.value() * 100) / 100;
        m_db->setKv(QStringLiteral("device_offsets"), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    }
    if (changed.isEmpty()) return;
    for (auto it = m_apRecords.constBegin(); it != m_apRecords.constEnd(); ++it)
        for (const ApObservation &o : it->obs) if (changed.contains(o.device)) { queueRefit(it.key()); break; }
}

// Anchors hide their pin: fit them from their samples and compare with the survey (leave-one-out). The mean
// NEES of the fixes gives κ (docs/GRADING.md §2.3), and the error per grade says what a letter means here.
void Locator::calibrateAnchors()
{
    QList<double> nees; int fixes = 0, regions = 0, inside = 0, regionInside = 0, withSamples = 0;
    QHash<QString, QList<double>> errByGrade;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (auto pin = m_pins.constBegin(); pin != m_pins.constEnd(); ++pin) {
        QString key = pin.key();
        if (!m_apRecords.contains(key)) { for (auto r = m_apRecords.constBegin(); r != m_apRecords.constEnd(); ++r) if (r.key().toUpper() == key) { key = r.key(); break; } }
        const auto rec = m_apRecords.constFind(key);
        if (rec == m_apRecords.constEnd() || rec->obs.isEmpty()) continue;
        ++withSamples;
        const QList<Estimator::Obs> obs = obsFor(*rec, key);
        Estimator::Context c; c.deviceOffset = m_devOffsets; c.misses = missesFor(obs);
        Estimator::Options o = estimatorOptions(rec->freq); o.kappa = 1.0;
        const Estimator::Fit f = Estimator::fitAp(obs, now, o, c);
        if (!f.valid) continue;
        const Estimator::Frame fr(f.lat, f.lon);
        const double ex = fr.x(pin->lon), ey = fr.y(pin->lat), err = std::hypot(ex, ey);
        if (f.kind == QLatin1String("region")) { ++regions; if (err <= f.r95) ++regionInside; errByGrade[QStringLiteral("R")] << err; continue; }
        ++fixes; if (err <= f.r95) ++inside;
        errByGrade[f.grade] << err;
        double inv[3];
        if (Estimator::invert2(f.cxx, f.cxy, f.cyy, inv)) nees << ex * ex * inv[0] + 2 * ex * ey * inv[1] + ey * ey * inv[2];
    }
    double meanNees = 0; for (double v : nees) meanNees += v; if (!nees.isEmpty()) meanNees /= nees.size();
    if (nees.size() >= 3) m_kappa = std::sqrt(std::clamp(meanNees / 2.0, 0.25, 25.0));
    QJsonObject byGrade;
    for (auto it = errByGrade.constBegin(); it != errByGrade.constEnd(); ++it) byGrade[it.key()] = QJsonObject{{"n", int(it->size())}, {"medianErrorM", std::round(medianOf(*it) * 10) / 10}};
    m_calibration = QJsonObject{{"anchors", int(m_pins.size())}, {"withSamples", withSamples}, {"fixes", fixes}, {"regions", regions},
                                {"meanNees", nees.isEmpty() ? QJsonValue() : QJsonValue(std::round(meanNees * 100) / 100)}, {"kappa", std::round(m_kappa * 1000) / 1000},
                                {"coverage95", fixes ? QJsonValue(double(inside) / fixes) : QJsonValue()},
                                {"regionCoverage95", regions ? QJsonValue(double(regionInside) / regions) : QJsonValue()},
                                {"byGrade", byGrade}, {"updated", QDateTime::currentDateTime().toString(Qt::ISODate)}};
    if (m_dbUsable) {
        m_db->setKv(QStringLiteral("estimator_calibration"), QString::fromUtf8(QJsonDocument(m_calibration).toJson(QJsonDocument::Compact)));
        m_db->setKv(QStringLiteral("estimator_kappa"), QString::number(m_kappa, 'g', 10));
    }
}

void Locator::calibrateEstimator()
{
    calibrateAnchors();
    rebuildGroups();
    calibrateDeviceOffsets();
}

QJsonObject Locator::estimatorJson() const
{
    QJsonObject grades, kinds;
    for (auto it = m_apRecords.constBegin(); it != m_apRecords.constEnd(); ++it) {
        const QString g = it->fit.grade.isEmpty() ? QStringLiteral("none") : it->fit.grade;
        grades[g] = grades[g].toInt() + 1;
        const QString k = it->fit.kind.isEmpty() ? QStringLiteral("none") : it->fit.kind;
        kinds[k] = kinds[k].toInt() + 1;
    }
    QJsonObject offs; for (auto it = m_devOffsets.constBegin(); it != m_devOffsets.constEnd(); ++it) offs[it.key()] = it.value();
    QHash<QString, QJsonArray> groups;
    for (auto it = m_groups.constBegin(); it != m_groups.constEnd(); ++it) groups[it->ref].append(QJsonObject{{"bssid", it.key()}, {"offsetDb", it->offsetDb}});
    QJsonArray ga; for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) ga.append(QJsonObject{{"ref", it.key()}, {"members", it.value()}});
    // where sampling next helps most: the largest expected information gains within 2 km of the fix
    struct Sug { double gain; QJsonObject o; };
    QList<Sug> sug;
    for (auto it = m_apRecords.constBegin(); it != m_apRecords.constEnd(); ++it) {
        const Estimator::Fit &f = it->fit;
        if (!f.valid || f.suggestGain <= 0) continue;
        const double d = m_fix.valid ? distanceM(m_fix.lat, m_fix.lon, f.suggestLat, f.suggestLon) : -1;
        if (m_fix.valid && d > 2000) continue;
        sug.append({f.suggestGain, QJsonObject{{"bssid", it.key()}, {"ssid", it->ssid}, {"grade", f.grade}, {"lat", f.suggestLat}, {"lon", f.suggestLon},
                                               {"gain", f.suggestGain}, {"distanceM", d < 0 ? QJsonValue() : QJsonValue(std::round(d))}}});
    }
    std::sort(sug.begin(), sug.end(), [](const Sug &a, const Sug &b) { return a.gain > b.gain; });
    QJsonArray sa; for (int i = 0; i < sug.size() && i < 10; ++i) sa.append(sug[i].o);
    return QJsonObject{{"version", Estimator::kVersion}, {"kappa", m_kappa}, {"calibration", m_calibration}, {"deviceOffsets", offs}, {"groups", ga},
                       {"grades", grades}, {"kinds", kinds}, {"upgradePending", int(m_upgradePending.size())}, {"scanCells", int(m_scanCells.size())},
                       {"suggestions", sa}, {"environment", environmentJson()}};
}

QString Locator::EstimatorJson() const { return QString::fromUtf8(QJsonDocument(estimatorJson()).toJson(QJsonDocument::Compact)); }

// §4.3.7: a linked Pi / GNSS receiver in the RV knows where the RV is to a few metres
bool Locator::tryRvGnss()
{
    if (!atHome()) return false;
    const QDateTime now = QDateTime::currentDateTime();
    const DevicePos *best = nullptr;
    for (const DevicePos &d : m_devicePos) {
        const QString k = kindForDevice(d.device, d.kind);
        if ((k != QLatin1String("pi") && k != QLatin1String("gnss")) || !d.time.isValid() || d.time.secsTo(now) > 600 || !(d.acc > 0) || d.acc > 5) continue;
        // Plausibility: a fresh precise fix of ours more than 5 km away says this receiver is not in the RV with us
        if (m_fix.valid && m_fix.precise() && m_fix.time.isValid() && m_fix.time.secsTo(now) < 1800 && distanceM(m_fix.lat, m_fix.lon, d.lat, d.lon) > 5000) continue;
        if (!best || d.acc < best->acc) best = &d;
    }
    if (!best) return false;
    const BfAnchor *gnss = nullptr, *self = nullptr;
    for (const BfAnchor &a : m_anchors) {
        if (a.kind == QLatin1String("gnss") && (!gnss || a.placedAt > gnss->placedAt)) gnss = &a;
        if (a.kind == QLatin1String("this-computer") && (!self || a.placedAt > self->placedAt)) self = &a;
    }
    const ::Anchors::RvGnss r = ::Anchors::rvGnssPosition(best->lat, best->lon, best->acc, gnss, self, false, 0);
    if (!r.valid) return false;
    Fix f; f.valid = true; f.lat = r.lat; f.lon = r.lon; f.accuracy = r.sigmaM;
    f.source = QStringLiteral("gnss"); f.provider = QStringLiteral("rv-gnss"); f.time = now; f.apCount = m_aps.size();
    const QString who = best->device;
    accept(f);
    finish(true, QStringLiteral("RV GNSS fix from %1 (±%2 m)").arg(who).arg(r.sigmaM, 0, 'f', 1));
    return true;
}

QString Locator::kindForDevice(const QString &device, const QString &hint) const
{
    auto good = [](const QString &k) { return !k.isEmpty() && k != QLatin1String("device"); };
    if (m_api) for (const ApiServer::Device &d : m_api->devices()) if (d.name == device && good(d.kind)) return d.kind;
    if (m_identity) {
        for (const IdentityDevice &idv : m_identity->devices()) if (idv.name == device && good(idv.kind)) return idv.kind;
        for (const PendingLink &pl : m_identity->pending()) if (pl.deviceName == device && good(pl.deviceKind)) return pl.deviceKind;
    }
    if (m_ranging) { const QString k = m_ranging->kindOf(device); if (good(k)) return k; }   // its BLE advert says what it is
    if (good(hint) && hint != QLatin1String("laptop")) return hint;      // "laptop" was the old catch-all
    static const QRegularExpression phone(QStringLiteral("pixel|galaxy|android|phone|oneplus|motorola|moto |xiaomi|redmi|\\bsm-"), QRegularExpression::CaseInsensitiveOption);
    if (phone.match(device).hasMatch()) return QStringLiteral("android");
    return good(hint) ? hint : QStringLiteral("device");
}

QStringList Locator::features()
{
    return {QStringLiteral("sync"), QStringLiteral("locate"), QStringLiteral("home"), QStringLiteral("events"), QStringLiteral("stream"), QStringLiteral("estimates"),
            QStringLiteral("identity"), QStringLiteral("peers"), QStringLiteral("anchors"), QStringLiteral("ranging"), QStringLiteral("aps-paging"), QStringLiteral("grant-control"),
            QStringLiteral("pediatric"), QStringLiteral("whoami")};
}

// ── D-Bus ────────────────────────────────────────────────────────────────────
QString Locator::Anchors() const { return QString::fromUtf8(QJsonDocument(anchorsJson()).toJson(QJsonDocument::Compact)); }

QString Locator::SetAnchor(const QString &json)
{
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(json.toUtf8(), &pe);
    if (!d.isObject()) return QStringLiteral("error: not a JSON object (%1)").arg(pe.errorString());
    bool ok = false; QString err;
    const BfAnchor a = setAnchor(d.object(), QStringLiteral("desktop"), &ok, &err);
    return ok ? a.id : QStringLiteral("error: ") + err;
}

bool Locator::RemoveAnchor(const QString &id) { return removeAnchor(id); }
QString Locator::Ranging() const { return QString::fromUtf8(QJsonDocument(m_ranging ? m_ranging->list() : QJsonObject{{"devices", QJsonArray()}}).toJson(QJsonDocument::Compact)); }
QString Locator::RangingInfo() const { return QString::fromUtf8(QJsonDocument(m_ranging ? m_ranging->info() : QJsonObject{{"rtt", RangingService::responderInfo()}}).toJson(QJsonDocument::Compact)); }
QString Locator::RangingCalibrate(const QString &device, double distanceM, int durationS)
{
    return QString::fromUtf8(QJsonDocument(m_ranging ? m_ranging->calibrate(device, distanceM, durationS) : QJsonObject{{"error", "ranging not running"}}).toJson(QJsonDocument::Compact));
}
bool Locator::GrantControl(const QString &nameOrId) { return m_api && m_api->grantControlDevice(nameOrId); }
