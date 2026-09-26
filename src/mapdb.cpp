#include "mapdb.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QtMath>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <cmath>

static const char MAGIC[5] = {'B', 'F', 'D', 'B', 1};
static const int NONCE_LEN = 12, TAG_LEN = 16, KEY_LEN = 32;

// ── Crypto ────────────────────────────────────────────────────────────────────
QByteArray MapDb::encrypt(const QByteArray &key, const QByteArray &plain, QString *error)
{
    QByteArray nonce(NONCE_LEN, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), NONCE_LEN) != 1) { *error = QStringLiteral("RAND_bytes failed"); return {}; }
    QByteArray out(plain.size() + TAG_LEN, 0);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0, total = 0; bool ok = ctx
        && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_LEN, nullptr) == 1
        && EVP_EncryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
        && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len, reinterpret_cast<const unsigned char *>(plain.constData()), plain.size()) == 1;
    total = len;
    ok = ok && EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total, &len) == 1;
    total += len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, out.data() + total) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) { *error = QStringLiteral("AES-GCM encryption failed"); return {}; }
    out.resize(total + TAG_LEN);
    return QByteArray(MAGIC, 5) + nonce + out;
}

QByteArray MapDb::decrypt(const QByteArray &key, const QByteArray &blob, QString *error)
{
    if (blob.size() < 5 + NONCE_LEN + TAG_LEN || blob.left(5) != QByteArray(MAGIC, 5)) { *error = QStringLiteral("not a BeaconFix database file"); return {}; }
    const QByteArray nonce = blob.mid(5, NONCE_LEN);
    const QByteArray body = blob.mid(5 + NONCE_LEN, blob.size() - 5 - NONCE_LEN - TAG_LEN);
    QByteArray tag = blob.right(TAG_LEN);
    QByteArray out(body.size(), 0);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0, total = 0; bool ok = ctx
        && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_LEN, nullptr) == 1
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
        && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len, reinterpret_cast<const unsigned char *>(body.constData()), body.size()) == 1;
    total = len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, tag.data()) == 1
            && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total, &len) > 0;
    total += len;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) { *error = QStringLiteral("wrong key or corrupted database (authentication failed)"); return {}; }
    out.resize(total);
    return out;
}

// ── Key storage ───────────────────────────────────────────────────────────────
static QString keyFilePath() { return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/sworrl/beaconfix.key"); }

QByteArray MapDb::fileKey(bool create, QString *source)
{
    QFile f(keyFilePath());
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray k = QByteArray::fromHex(f.readAll().trimmed());
        if (k.size() == KEY_LEN) { *source = QStringLiteral("keyfile"); return k; }
    }
    if (!create) return {};
    QByteArray k(KEY_LEN, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(k.data()), KEY_LEN) != 1) return {};
    QDir().mkpath(QFileInfo(keyFilePath()).path());
    QSaveFile s(keyFilePath());
    if (!s.open(QIODevice::WriteOnly)) return {};
    s.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    s.write(k.toHex() + "\n");
    if (!s.commit()) return {};
    *source = QStringLiteral("keyfile");
    return k;
}

QByteArray MapDb::walletKey(bool create, QString *source)
{
    // Only talk to a wallet daemon that is already running: D-Bus activation would spawn one
    // (and stall for its timeout) on a session without a desktop, e.g. a test bus.
    QDBusConnectionInterface *bi = QDBusConnection::sessionBus().interface();
    if (!bi || !bi->isServiceRegistered(QStringLiteral("org.kde.kwalletd6"))) return {};
    QDBusInterface w(QStringLiteral("org.kde.kwalletd6"), QStringLiteral("/modules/kwalletd6"), QStringLiteral("org.kde.KWallet"), QDBusConnection::sessionBus());
    if (!w.isValid()) return {};
    w.setTimeout(20000);                                       // the wallet may ask the user to unlock it
    QDBusReply<QString> name = w.call(QStringLiteral("networkWallet"));
    if (!name.isValid() || name.value().isEmpty()) return {};
    QDBusReply<int> h = w.call(QStringLiteral("open"), name.value(), qlonglong(0), QStringLiteral("BeaconFix"));
    if (!h.isValid() || h.value() < 0) return {};
    const int handle = h.value();
    const QString folder = QStringLiteral("BeaconFix"), entry = QStringLiteral("mapdb-key"), app = QStringLiteral("BeaconFix");
    QByteArray key;
    QDBusReply<QString> pw = w.call(QStringLiteral("readPassword"), handle, folder, entry, app);
    if (pw.isValid() && !pw.value().isEmpty()) key = QByteArray::fromBase64(pw.value().toLatin1());
    if (key.size() != KEY_LEN && create) {
        key.resize(KEY_LEN);
        if (RAND_bytes(reinterpret_cast<unsigned char *>(key.data()), KEY_LEN) != 1) key.clear();
        else {
            QDBusReply<bool> hasF = w.call(QStringLiteral("hasFolder"), handle, folder, app);
            if (!(hasF.isValid() && hasF.value())) w.call(QStringLiteral("createFolder"), handle, folder, app);
            QDBusReply<int> wr = w.call(QStringLiteral("writePassword"), handle, folder, entry, QString::fromLatin1(key.toBase64()), app);
            if (!wr.isValid() || wr.value() != 0) key.clear();
        }
    }
    w.call(QStringLiteral("close"), handle, false, app);
    if (key.size() == KEY_LEN) { *source = QStringLiteral("kwallet"); return key; }
    return {};
}

bool MapDb::ensureKey(bool create)
{
    m_key = fileKey(false, &m_keySource);                     // a key file means the wallet was not there when the key was made
    if (m_key.isEmpty()) m_key = walletKey(create, &m_keySource);
    if (m_key.isEmpty() && create) m_key = fileKey(true, &m_keySource);
    if (m_key.isEmpty()) { m_error = QStringLiteral("no database key: unlock KWallet (or restore ~/.config/sworrl/beaconfix.key) and restart"); return false; }
    return true;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
QString MapDb::runtimeDir()
{
    QString d = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (d.isEmpty() || !QFileInfo(d).isWritable()) d = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/beaconfix/.live");
    else d += QStringLiteral("/beaconfix");
    QDir().mkpath(d);
    QFile::setPermissions(d, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return d;
}

MapDb::MapDb(const QString &stateDir, QObject *parent) : QObject(parent), m_stateDir(stateDir)
{
    m_blobPath = stateDir + QStringLiteral("/beaconfix.db");
    m_conn = QStringLiteral("mapdb-%1").arg(QCoreApplication::applicationPid());
    m_flushTimer.setSingleShot(true); m_flushTimer.setInterval(4000);
    connect(&m_flushTimer, &QTimer::timeout, this, &MapDb::flush);
}

MapDb::~MapDb()
{
    if (m_db.isOpen()) { if (!m_readOnly) flush(); m_db.close(); }
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_conn);
    if (m_readOnly && !m_livePath.isEmpty()) QFile::remove(m_livePath);
}

bool MapDb::decryptToLive(bool readOnly)
{
    QFile blob(m_blobPath);
    if (!blob.exists()) {
        if (readOnly) { m_error = QStringLiteral("no database yet"); return false; }
        if (QFile::exists(m_livePath)) return true;          // a live copy left by a previous run: keep it
        return true;                                           // fresh database
    }
    if (!blob.open(QIODevice::ReadOnly)) { m_error = blob.errorString(); return false; }
    const QByteArray plain = decrypt(m_key, blob.readAll(), &m_error);
    if (plain.isEmpty()) return false;
    if (!readOnly && QFile::exists(m_livePath)) {
        // Both exist: the live copy is newer only if a previous run died before flushing; prefer it when larger/newer
        const QFileInfo li(m_livePath), bi(m_blobPath);
        if (li.lastModified() > bi.lastModified()) return true;
    }
    QSaveFile f(m_livePath);
    if (!f.open(QIODevice::WriteOnly)) { m_error = f.errorString(); return false; }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(plain);
    if (!f.commit()) { m_error = f.errorString(); return false; }
    return true;
}

bool MapDb::open(bool readOnly)
{
    m_readOnly = readOnly;
    const bool blobExists = QFile::exists(m_blobPath);
    if (readOnly && !blobExists) { m_error = QStringLiteral("no database yet"); return false; }
    if (!ensureKey(!readOnly)) return false;
    m_livePath = runtimeDir() + (readOnly ? QStringLiteral("/ro-%1.db").arg(QCoreApplication::applicationPid()) : QStringLiteral("/live.db"));
    if (!decryptToLive(readOnly)) return false;
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    m_db.setDatabaseName(m_livePath);
    if (readOnly) m_db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!m_db.open()) { m_error = m_db.lastError().text(); return false; }
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("PRAGMA journal_mode=TRUNCATE"));
    q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    if (!readOnly && !schema()) return false;
    if (!readOnly && !blobExists) markDirty();                 // write the (empty) encrypted file right away
    return true;
}

bool MapDb::schema()
{
    static const char *ddl[] = {
        "CREATE TABLE IF NOT EXISTS aps (bssid TEXT PRIMARY KEY, ssid TEXT, band TEXT, ch INTEGER, freq INTEGER, first_seen TEXT, last_seen TEXT, times_seen INTEGER DEFAULT 0,"
        " lat REAL, lon REAL, acc REAL, source TEXT, home INTEGER DEFAULT 0, travelling INTEGER DEFAULT 0, ignored INTEGER DEFAULT 0,"
        " wigle INTEGER DEFAULT 0, wlat REAL, wlon REAL, wigle_checked TEXT)",
        "CREATE TABLE IF NOT EXISTS observations (id INTEGER PRIMARY KEY, bssid TEXT NOT NULL, time TEXT, lat REAL, lon REAL, acc REAL, dbm INTEGER, fix_source TEXT)",
        "CREATE INDEX IF NOT EXISTS obs_bssid ON observations(bssid)",
        "CREATE TABLE IF NOT EXISTS sightings (id INTEGER PRIMARY KEY, bssid TEXT NOT NULL, lat REAL, lon REAL, acc REAL, time TEXT)",
        "CREATE INDEX IF NOT EXISTS sight_bssid ON sightings(bssid)",
        "CREATE TABLE IF NOT EXISTS cells (bssid TEXT NOT NULL, cell TEXT NOT NULL, PRIMARY KEY (bssid, cell))",
        "CREATE TABLE IF NOT EXISTS flags (kind TEXT NOT NULL, bssid TEXT NOT NULL, PRIMARY KEY (kind, bssid))",
        "CREATE TABLE IF NOT EXISTS fixes (id INTEGER PRIMARY KEY, time TEXT, lat REAL, lon REAL, acc REAL, source TEXT, provider TEXT, place TEXT, city TEXT, region TEXT, country TEXT,"
        " elev REAL, ap_count INTEGER, ap_used INTEGER, departed TEXT)",
        "CREATE TABLE IF NOT EXISTS pois (osm_type TEXT NOT NULL, osm_id INTEGER NOT NULL, cat TEXT, name TEXT, detail TEXT, lat REAL, lon REAL, wifi INTEGER, hours TEXT, phone TEXT, website TEXT,"
        " PRIMARY KEY (osm_type, osm_id))",
        "CREATE TABLE IF NOT EXISTS elevation (cell TEXT PRIMARY KEY, elev REAL, time TEXT)",
        "CREATE TABLE IF NOT EXISTS achievements (key TEXT PRIMARY KEY, unlocked TEXT)",
        "CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT)",
    };
    QSqlQuery q(m_db);
    for (const char *s : ddl) if (!q.exec(QString::fromLatin1(s))) { m_error = q.lastError().text(); return false; }
    // Columns added after schema 1 (safe to run every time)
    QSqlQuery cols(m_db); cols.exec(QStringLiteral("PRAGMA table_info(aps)"));
    QSet<QString> have; while (cols.next()) have.insert(cols.value(1).toString());
    const QList<QPair<QString, QString>> extra{{QStringLiteral("security"), QStringLiteral("TEXT")}, {QStringLiteral("sec_flags"), QStringLiteral("INTEGER DEFAULT 0")},
                                               {QStringLiteral("wpa_flags"), QStringLiteral("INTEGER DEFAULT 0")}, {QStringLiteral("rsn_flags"), QStringLiteral("INTEGER DEFAULT 0")},
                                               {QStringLiteral("max_kbps"), QStringLiteral("INTEGER DEFAULT 0")}, {QStringLiteral("adhoc"), QStringLiteral("INTEGER DEFAULT 0")}};
    for (const auto &c : extra) if (!have.contains(c.first)) q.exec(QStringLiteral("ALTER TABLE aps ADD COLUMN %1 %2").arg(c.first, c.second));
    q.exec(QStringLiteral("INSERT OR IGNORE INTO kv(key, value) VALUES ('schema', '2')"));
    q.exec(QStringLiteral("UPDATE kv SET value='2' WHERE key='schema'"));
    q.exec(QStringLiteral("INSERT OR IGNORE INTO kv(key, value) VALUES ('created', '%1')").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    return true;
}

void MapDb::markDirty()
{
    if (m_readOnly) return;
    m_dirty = true;
    m_flushTimer.start();
    emit changed();
}

void MapDb::flush()
{
    if (!m_dirty || m_readOnly || !m_db.isOpen()) return;
    QFile live(m_livePath);
    if (!live.open(QIODevice::ReadOnly)) { m_error = live.errorString(); return; }
    QString err;
    const QByteArray blob = encrypt(m_key, live.readAll(), &err);
    if (blob.isEmpty()) { m_error = err; return; }
    QDir().mkpath(QFileInfo(m_blobPath).path());
    QSaveFile f(m_blobPath);
    if (!f.open(QIODevice::WriteOnly)) { m_error = f.errorString(); return; }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(blob);
    if (f.commit()) m_dirty = false; else m_error = f.errorString();
}

bool MapDb::isEmpty() const
{
    if (!m_db.isOpen()) return true;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT (SELECT COUNT(*) FROM aps) + (SELECT COUNT(*) FROM fixes)"));
    return !q.next() || q.value(0).toInt() == 0;
}

QJsonObject MapDb::stats() const
{
    QJsonObject o{{"open", m_db.isOpen()}, {"path", m_blobPath}, {"encrypted", true}, {"cipher", "AES-256-GCM (OpenSSL)"}, {"keySource", m_keySource},
                  {"readOnly", m_readOnly}, {"error", m_error}, {"sizeBytes", double(QFileInfo(m_blobPath).size())}};
    if (!m_db.isOpen()) return o;
    QSqlQuery q(m_db);
    for (const char *t : {"aps", "observations", "sightings", "fixes", "pois", "elevation", "achievements"}) {
        q.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(QLatin1String(t)));
        o[QLatin1String(t)] = q.next() ? q.value(0).toInt() : 0;
    }
    q.exec(QStringLiteral("SELECT COUNT(*) FROM aps WHERE lat IS NOT NULL")); o["apsPositioned"] = q.next() ? q.value(0).toInt() : 0;
    q.exec(QStringLiteral("SELECT COUNT(*) FROM aps WHERE home=1")); o["apsHome"] = q.next() ? q.value(0).toInt() : 0;
    q.exec(QStringLiteral("SELECT value FROM kv WHERE key='created'")); if (q.next()) o["created"] = q.value(0).toString();
    q.exec(QStringLiteral("SELECT value FROM kv WHERE key='migrated'")); if (q.next()) o["migrated"] = q.value(0).toString();
    return o;
}

// ── Positions ─────────────────────────────────────────────────────────────────
// Where an AP is, from what we have: a placement (WiGLE / Apple) wins; else two or more
// distinct vantage points give a signal-weighted centroid ("trilat"); else the one place we
// heard it, widened by the RSSI distance ("observed").
static bool positionFrom(const ApRecord &r, double *lat, double *lon, double *acc, QString *source)
{
    if (r.wigle) { *lat = r.wLat; *lon = r.wLon; *acc = 25; *source = QStringLiteral("placed"); return true; }
    if (r.obs.isEmpty()) return false;
    struct V { double lat, lon, acc, dbm; int n; };
    QList<V> vs;
    for (const ApObservation &o : r.obs) {
        bool merged = false;
        for (V &v : vs)
            if (Locator::distanceM(v.lat, v.lon, o.lat, o.lon) < qMax(80.0, v.acc + o.acc)) {
                v.lat = (v.lat * v.n + o.lat) / (v.n + 1); v.lon = (v.lon * v.n + o.lon) / (v.n + 1);
                v.acc = qMin(v.acc, o.acc); v.dbm = (v.dbm * v.n + o.dbm) / (v.n + 1); ++v.n; merged = true; break;
            }
        if (!merged) vs.append({o.lat, o.lon, o.acc, double(o.dbm), 1});
    }
    if (vs.size() >= 2) {
        double sw = 0, sl = 0, so = 0;
        for (const V &v : vs) { const double w = std::pow(10.0, v.dbm / 20.0) / qMax(10.0, v.acc); sw += w; sl += w * v.lat; so += w * v.lon; }
        *lat = sl / sw; *lon = so / sw;
        double spread = 0, accs = 0;
        for (const V &v : vs) { const double w = std::pow(10.0, v.dbm / 20.0) / qMax(10.0, v.acc), d = Locator::distanceM(*lat, *lon, v.lat, v.lon); spread += w * d * d; accs += v.acc; }
        *acc = qMax(30.0, std::sqrt(spread / sw) + accs / vs.size() / 2);
        *source = QStringLiteral("trilat");
        return true;
    }
    const V &v = vs.first();
    *lat = v.lat; *lon = v.lon; *acc = v.acc + Locator::rssiDistanceM(qRound(v.dbm), r.freq ? r.freq : 2437); *source = QStringLiteral("observed");
    return true;
}

void MapDb::updatePosition(const QString &bssid, const ApRecord &r, int flags)
{
    double lat = 0, lon = 0, acc = 0; QString source;
    QSqlQuery q(m_db);
    if (positionFrom(r, &lat, &lon, &acc, &source)) {
        q.prepare(QStringLiteral("UPDATE aps SET lat=?, lon=?, acc=?, source=?, home=?, travelling=?, ignored=? WHERE bssid=?"));
        q.addBindValue(lat); q.addBindValue(lon); q.addBindValue(acc); q.addBindValue(source);
    } else {
        q.prepare(QStringLiteral("UPDATE aps SET lat=NULL, lon=NULL, acc=NULL, source=NULL, home=?, travelling=?, ignored=? WHERE bssid=?"));
    }
    q.addBindValue(flags & 1 ? 1 : 0); q.addBindValue(flags & 2 ? 1 : 0); q.addBindValue(flags & 4 ? 1 : 0); q.addBindValue(bssid);
    q.exec();
}

// ── AP records ────────────────────────────────────────────────────────────────
QHash<QString, ApRecord> MapDb::loadApRecords(QSet<QString> *travelling, QSet<QString> *notTravelling) const
{
    QHash<QString, ApRecord> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT bssid, ssid, freq, wigle, wlat, wlon, wigle_checked, security, sec_flags, wpa_flags, rsn_flags, max_kbps, adhoc FROM aps"));
    while (q.next()) {
        ApRecord &r = out[q.value(0).toString()];
        r.ssid = q.value(1).toString(); r.freq = q.value(2).toInt();
        r.wigle = q.value(3).toInt() != 0; r.wLat = q.value(4).toDouble(); r.wLon = q.value(5).toDouble();
        r.wigleChecked = QDateTime::fromString(q.value(6).toString(), Qt::ISODate);
        r.security = q.value(7).toString(); r.secFlags = q.value(8).toInt(); r.wpaFlags = q.value(9).toInt(); r.rsnFlags = q.value(10).toInt(); r.maxKbps = q.value(11).toInt(); r.adhoc = q.value(12).toInt() != 0;
    }
    q.exec(QStringLiteral("SELECT bssid, time, lat, lon, acc, dbm FROM observations ORDER BY id"));
    while (q.next()) {
        auto it = out.find(q.value(0).toString()); if (it == out.end()) continue;
        ApObservation o; o.time = QDateTime::fromString(q.value(1).toString(), Qt::ISODate);
        o.lat = q.value(2).toDouble(); o.lon = q.value(3).toDouble(); o.acc = q.value(4).toDouble(); o.dbm = q.value(5).toInt();
        it->obs.append(o);
    }
    q.exec(QStringLiteral("SELECT bssid, lat, lon, acc, time FROM sightings ORDER BY id"));
    while (q.next()) {
        auto it = out.find(q.value(0).toString()); if (it == out.end()) continue;
        it->seen.append({q.value(1).toDouble(), q.value(2).toDouble(), q.value(3).toDouble(), QDateTime::fromString(q.value(4).toString(), Qt::ISODate)});
    }
    q.exec(QStringLiteral("SELECT bssid, cell FROM cells"));
    while (q.next()) { auto it = out.find(q.value(0).toString()); if (it != out.end()) it->cells.insert(q.value(1).toString()); }
    q.exec(QStringLiteral("SELECT kind, bssid FROM flags"));
    while (q.next()) {
        if (q.value(0).toString() == QLatin1String("travelling")) { if (travelling) travelling->insert(q.value(1).toString()); }
        else if (notTravelling) notTravelling->insert(q.value(1).toString());
    }
    return out;
}

void MapDb::saveApRecords(const QHash<QString, ApRecord> &recs, const QSet<QString> &travelling, const QSet<QString> &notTravelling, const QHash<QString, int> &flags)
{
    if (!m_db.isOpen() || m_readOnly) return;
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    m_db.transaction();
    QSqlQuery q(m_db), del(m_db), ins(m_db);
    q.prepare(QStringLiteral("INSERT INTO aps(bssid, ssid, band, ch, freq, first_seen, last_seen, times_seen, wigle, wlat, wlon, wigle_checked) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"
                             " ON CONFLICT(bssid) DO UPDATE SET ssid=CASE WHEN excluded.ssid<>'' THEN excluded.ssid ELSE aps.ssid END, band=excluded.band, ch=excluded.ch,"
                             " freq=CASE WHEN excluded.freq>0 THEN excluded.freq ELSE aps.freq END, last_seen=excluded.last_seen, times_seen=excluded.times_seen,"
                             " wigle=excluded.wigle, wlat=excluded.wlat, wlon=excluded.wlon, wigle_checked=excluded.wigle_checked"));
    for (auto it = recs.constBegin(); it != recs.constEnd(); ++it) {
        const ApRecord &r = it.value();
        const int f = r.freq;
        QString last = now;
        for (const ApObservation &o : r.obs) if (o.time.isValid() && o.time.toString(Qt::ISODate) > last) last = o.time.toString(Qt::ISODate);
        q.addBindValue(it.key()); q.addBindValue(r.ssid);
        q.addBindValue(f >= 5925 ? QStringLiteral("6") : f >= 4900 ? QStringLiteral("5") : f > 0 ? QStringLiteral("2.4") : QString());
        q.addBindValue(f >= 5925 ? (f - 5950) / 5 : f >= 4900 ? (f - 5000) / 5 : f == 2484 ? 14 : f > 2400 ? (f - 2407) / 5 : 0);
        q.addBindValue(f); q.addBindValue(now); q.addBindValue(now); q.addBindValue(r.obs.size() + r.seen.size());
        q.addBindValue(r.wigle ? 1 : 0); q.addBindValue(r.wigle ? QVariant(r.wLat) : QVariant()); q.addBindValue(r.wigle ? QVariant(r.wLon) : QVariant());
        q.addBindValue(r.wigleChecked.isValid() ? QVariant(r.wigleChecked.toString(Qt::ISODate)) : QVariant());
        q.exec();
        for (const char *t : {"observations", "sightings", "cells"}) { del.prepare(QStringLiteral("DELETE FROM %1 WHERE bssid=?").arg(QLatin1String(t))); del.addBindValue(it.key()); del.exec(); }
        ins.prepare(QStringLiteral("INSERT INTO observations(bssid, time, lat, lon, acc, dbm, fix_source) VALUES(?,?,?,?,?,?,?)"));
        for (const ApObservation &o : r.obs) {
            ins.addBindValue(it.key()); ins.addBindValue(o.time.toString(Qt::ISODate)); ins.addBindValue(o.lat); ins.addBindValue(o.lon); ins.addBindValue(o.acc); ins.addBindValue(o.dbm); ins.addBindValue(QStringLiteral("wifi"));
            ins.exec();
        }
        ins.prepare(QStringLiteral("INSERT INTO sightings(bssid, lat, lon, acc, time) VALUES(?,?,?,?,?)"));
        for (const ApSighting &s : r.seen) { ins.addBindValue(it.key()); ins.addBindValue(s.lat); ins.addBindValue(s.lon); ins.addBindValue(s.acc); ins.addBindValue(s.time.toString(Qt::ISODate)); ins.exec(); }
        ins.prepare(QStringLiteral("INSERT OR IGNORE INTO cells(bssid, cell) VALUES(?,?)"));
        for (const QString &c : r.cells) { ins.addBindValue(it.key()); ins.addBindValue(c); ins.exec(); }
        updatePosition(it.key(), r, flags.value(it.key(), 0));
        if (!r.security.isEmpty()) {
            QSqlQuery sec(m_db);
            sec.prepare(QStringLiteral("UPDATE aps SET security=?, sec_flags=?, wpa_flags=?, rsn_flags=?, max_kbps=?, adhoc=? WHERE bssid=?"));
            sec.addBindValue(r.security); sec.addBindValue(r.secFlags); sec.addBindValue(r.wpaFlags); sec.addBindValue(r.rsnFlags); sec.addBindValue(r.maxKbps); sec.addBindValue(r.adhoc ? 1 : 0); sec.addBindValue(it.key());
            sec.exec();
        }
    }
    q.exec(QStringLiteral("DELETE FROM flags"));
    ins.prepare(QStringLiteral("INSERT OR IGNORE INTO flags(kind, bssid) VALUES(?,?)"));
    for (const QString &b : travelling) { ins.addBindValue(QStringLiteral("travelling")); ins.addBindValue(b); ins.exec(); }
    for (const QString &b : notTravelling) { ins.addBindValue(QStringLiteral("notTravelling")); ins.addBindValue(b); ins.exec(); }
    m_db.commit();
    markDirty();
}

// ── Fixes ─────────────────────────────────────────────────────────────────────
static void bindFix(QSqlQuery &q, const Fix &f)
{
    q.addBindValue(f.time.toString(Qt::ISODate)); q.addBindValue(f.lat); q.addBindValue(f.lon); q.addBindValue(f.accuracy);
    q.addBindValue(f.source); q.addBindValue(f.provider); q.addBindValue(f.place); q.addBindValue(f.city); q.addBindValue(f.region); q.addBindValue(f.country);
    q.addBindValue(f.hasElevation() ? QVariant(f.elevation) : QVariant()); q.addBindValue(f.apCount); q.addBindValue(f.apUsed);
    q.addBindValue(f.departed.isValid() ? QVariant(f.departed.toString(Qt::ISODate)) : QVariant());
}
static const char *FIX_INSERT = "INSERT INTO fixes(time, lat, lon, acc, source, provider, place, city, region, country, elev, ap_count, ap_used, departed) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

QList<Fix> MapDb::loadFixes() const
{
    QList<Fix> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT time, lat, lon, acc, source, provider, place, city, region, country, elev, ap_count, ap_used, departed FROM fixes ORDER BY id"));
    while (q.next()) {
        Fix f; f.valid = true;
        f.time = QDateTime::fromString(q.value(0).toString(), Qt::ISODate); f.lat = q.value(1).toDouble(); f.lon = q.value(2).toDouble(); f.accuracy = q.value(3).toDouble();
        f.source = q.value(4).toString(); f.provider = q.value(5).toString(); f.place = q.value(6).toString(); f.city = q.value(7).toString(); f.region = q.value(8).toString(); f.country = q.value(9).toString();
        f.elevation = q.value(10).isNull() ? -9999 : q.value(10).toDouble(); f.apCount = q.value(11).toInt(); f.apUsed = q.value(12).toInt();
        f.departed = QDateTime::fromString(q.value(13).toString(), Qt::ISODate);
        out << f;
    }
    return out;
}

void MapDb::saveFixes(const QList<Fix> &fixes)
{
    if (!m_db.isOpen() || m_readOnly) return;
    m_db.transaction();
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("DELETE FROM fixes"));
    q.prepare(QString::fromLatin1(FIX_INSERT));
    for (const Fix &f : fixes) { bindFix(q, f); q.exec(); }
    m_db.commit();
    markDirty();
}

void MapDb::appendFix(const Fix &f)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db);
    q.prepare(QString::fromLatin1(FIX_INSERT));
    bindFix(q, f); q.exec();
    markDirty();
}

// ── Places, elevation, milestones ─────────────────────────────────────────────
bool MapDb::loadPois(QList<Poi> *pois, double *lat, double *lon, int *radiusM, QDateTime *time) const
{
    if (!m_db.isOpen()) return false;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT key, value FROM kv WHERE key IN ('poi_lat','poi_lon','poi_radius','poi_time')"));
    bool any = false;
    while (q.next()) {
        const QString k = q.value(0).toString(); any = true;
        if (k == QLatin1String("poi_lat")) *lat = q.value(1).toDouble(); else if (k == QLatin1String("poi_lon")) *lon = q.value(1).toDouble();
        else if (k == QLatin1String("poi_radius")) *radiusM = q.value(1).toInt(); else *time = QDateTime::fromString(q.value(1).toString(), Qt::ISODate);
    }
    if (!any) return false;
    q.exec(QStringLiteral("SELECT osm_type, osm_id, cat, name, detail, lat, lon, wifi, hours, phone, website FROM pois"));
    while (q.next()) {
        Poi p; p.osmType = q.value(0).toString(); p.osmId = q.value(1).toLongLong(); p.cat = q.value(2).toString(); p.name = q.value(3).toString(); p.detail = q.value(4).toString();
        p.lat = q.value(5).toDouble(); p.lon = q.value(6).toDouble(); p.wifi = q.value(7).toInt() != 0; p.hours = q.value(8).toString(); p.phone = q.value(9).toString(); p.website = q.value(10).toString();
        pois->append(p);
    }
    return true;
}

void MapDb::savePois(const QList<Poi> &pois, double lat, double lon, int radiusM, const QDateTime &time)
{
    if (!m_db.isOpen() || m_readOnly) return;
    m_db.transaction();
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("DELETE FROM pois"));
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO pois(osm_type, osm_id, cat, name, detail, lat, lon, wifi, hours, phone, website) VALUES(?,?,?,?,?,?,?,?,?,?,?)"));
    for (const Poi &p : pois) {
        q.addBindValue(p.osmType); q.addBindValue(p.osmId); q.addBindValue(p.cat); q.addBindValue(p.name); q.addBindValue(p.detail); q.addBindValue(p.lat); q.addBindValue(p.lon);
        q.addBindValue(p.wifi ? 1 : 0); q.addBindValue(p.hours); q.addBindValue(p.phone); q.addBindValue(p.website); q.exec();
    }
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO kv(key, value) VALUES(?,?)"));
    const QList<QPair<QString, QString>> kv{{QStringLiteral("poi_lat"), QString::number(lat, 'f', 7)}, {QStringLiteral("poi_lon"), QString::number(lon, 'f', 7)},
                                            {QStringLiteral("poi_radius"), QString::number(radiusM)}, {QStringLiteral("poi_time"), time.toString(Qt::ISODate)}};
    for (const auto &p : kv) { q.addBindValue(p.first); q.addBindValue(p.second); q.exec(); }
    m_db.commit();
    markDirty();
}

QHash<QString, double> MapDb::loadElevation() const
{
    QHash<QString, double> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT cell, elev FROM elevation"));
    while (q.next()) out.insert(q.value(0).toString(), q.value(1).toDouble());
    return out;
}

void MapDb::saveElevation(const QHash<QString, double> &cells)
{
    if (!m_db.isOpen() || m_readOnly) return;
    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO elevation(cell, elev, time) VALUES(?,?,?)"));
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    for (auto it = cells.constBegin(); it != cells.constEnd(); ++it) { q.addBindValue(it.key()); q.addBindValue(it.value()); q.addBindValue(now); q.exec(); }
    m_db.commit();
    markDirty();
}

QHash<QString, QDateTime> MapDb::loadAchievements() const
{
    QHash<QString, QDateTime> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT key, unlocked FROM achievements"));
    while (q.next()) out.insert(q.value(0).toString(), QDateTime::fromString(q.value(1).toString(), Qt::ISODate));
    return out;
}

void MapDb::saveAchievements(const QList<Achievement> &list)
{
    if (!m_db.isOpen() || m_readOnly) return;
    m_db.transaction();
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("DELETE FROM achievements"));
    q.prepare(QStringLiteral("INSERT INTO achievements(key, unlocked) VALUES(?,?)"));
    for (const Achievement &a : list) if (a.unlocked.isValid()) { q.addBindValue(a.key); q.addBindValue(a.unlocked.toString(Qt::ISODate)); q.exec(); }
    m_db.commit();
    markDirty();
}

// ── Self-location ─────────────────────────────────────────────────────────────
QList<MapDb::ApPos> MapDb::positions(const QStringList &bssids, double maxAcc) const
{
    QList<ApPos> out;
    if (!m_db.isOpen()) return out;
    QString sql = QStringLiteral("SELECT bssid, ssid, lat, lon, acc, source, home, travelling, ignored FROM aps WHERE lat IS NOT NULL");
    if (maxAcc > 0) sql += QStringLiteral(" AND acc <= %1").arg(maxAcc);
    if (!bssids.isEmpty()) {
        QStringList quoted; for (const QString &b : bssids) quoted << QStringLiteral("'%1'").arg(QString(b).replace(QLatin1Char('\''), QString()).toUpper());
        sql += QStringLiteral(" AND UPPER(bssid) IN (%1)").arg(quoted.join(QLatin1Char(',')));
    }
    QSqlQuery q(m_db);
    q.exec(sql);
    while (q.next()) {
        ApPos p; p.bssid = q.value(0).toString(); p.ssid = q.value(1).toString(); p.lat = q.value(2).toDouble(); p.lon = q.value(3).toDouble(); p.acc = q.value(4).toDouble();
        p.source = q.value(5).toString(); p.home = q.value(6).toInt(); p.travelling = q.value(7).toInt(); p.ignored = q.value(8).toInt();
        out << p;
    }
    return out;
}

bool MapDb::estimate(const QList<QPair<QString, int>> &heard, double *lat, double *lon, double *acc, int *used, QStringList *usedBssids, int minAps, double maxAcc) const
{
    QStringList ids; QHash<QString, int> dbm;
    for (const auto &h : heard) { ids << h.first.toUpper(); dbm.insert(h.first.toUpper(), h.second); }
    QList<ApPos> ps = positions(ids, maxAcc);
    double sw = 0, sl = 0, so = 0; QList<double> accs; int n = 0;
    QList<ApPos> usable;
    for (const ApPos &p : ps) {
        if (p.home || p.travelling || p.ignored) continue;
        usable << p;
    }
    if (usable.size() < minAps) { *used = usable.size(); return false; }
    for (const ApPos &p : usable) {
        const double w = std::pow(10.0, dbm.value(p.bssid.toUpper(), -80) / 20.0) / qMax(10.0, p.acc);
        sw += w; sl += w * p.lat; so += w * p.lon; accs << p.acc; ++n;
        if (usedBssids) usedBssids->append(p.bssid);
    }
    *lat = sl / sw; *lon = so / sw;
    double spread = 0;
    for (const ApPos &p : usable) { const double w = std::pow(10.0, dbm.value(p.bssid.toUpper(), -80) / 20.0) / qMax(10.0, p.acc), d = Locator::distanceM(*lat, *lon, p.lat, p.lon); spread += w * d * d; }
    std::sort(accs.begin(), accs.end());
    *acc = qMax(40.0, qMax(std::sqrt(spread / sw), accs[accs.size() / 2]));
    *used = n;
    return true;
}

int MapDb::addObservations(const QJsonArray &observations, QString *error)
{
    if (!m_db.isOpen() || m_readOnly) { if (error) *error = QStringLiteral("database not writable"); return -1; }
    int added = 0;
    m_db.transaction();
    QSqlQuery q(m_db);
    QSet<QString> touched;
    for (const QJsonValue &v : observations) {
        const QJsonObject o = v.toObject();
        const QString bssid = o["bssid"].toString().toUpper().trimmed();
        if (bssid.size() != 17 || !o["lat"].isDouble() || !o["lon"].isDouble()) continue;
        const double acc = o["acc"].toDouble(100);
        if (acc <= 0 || acc > 2000) continue;
        q.prepare(QStringLiteral("INSERT INTO aps(bssid, ssid, first_seen, last_seen, times_seen) VALUES(?,?,?,?,1) ON CONFLICT(bssid) DO UPDATE SET last_seen=excluded.last_seen, times_seen=aps.times_seen+1,"
                                 " ssid=CASE WHEN excluded.ssid<>'' THEN excluded.ssid ELSE aps.ssid END"));
        const QString t = o["time"].toString().isEmpty() ? QDateTime::currentDateTime().toString(Qt::ISODate) : o["time"].toString();
        q.addBindValue(bssid); q.addBindValue(o["ssid"].toString()); q.addBindValue(t); q.addBindValue(t); q.exec();
        q.prepare(QStringLiteral("INSERT INTO observations(bssid, time, lat, lon, acc, dbm, fix_source) VALUES(?,?,?,?,?,?,?)"));
        q.addBindValue(bssid); q.addBindValue(t); q.addBindValue(o["lat"].toDouble()); q.addBindValue(o["lon"].toDouble()); q.addBindValue(acc);
        q.addBindValue(o["dbm"].toInt(-80)); q.addBindValue(o["source"].toString().isEmpty() ? QStringLiteral("remote") : o["source"].toString()); q.exec();
        touched.insert(bssid); ++added;
    }
    // Recompute the position of every AP that received data, from all its observations
    for (const QString &b : touched) {
        ApRecord r;
        q.prepare(QStringLiteral("SELECT freq, wigle, wlat, wlon, home, travelling, ignored FROM aps WHERE bssid=?")); q.addBindValue(b); q.exec();
        int flags = 0;
        if (q.next()) { r.freq = q.value(0).toInt(); r.wigle = q.value(1).toInt() != 0; r.wLat = q.value(2).toDouble(); r.wLon = q.value(3).toDouble(); flags = (q.value(4).toInt() ? 1 : 0) | (q.value(5).toInt() ? 2 : 0) | (q.value(6).toInt() ? 4 : 0); }
        q.prepare(QStringLiteral("SELECT time, lat, lon, acc, dbm FROM observations WHERE bssid=? ORDER BY id")); q.addBindValue(b); q.exec();
        while (q.next()) { ApObservation ob; ob.time = QDateTime::fromString(q.value(0).toString(), Qt::ISODate); ob.lat = q.value(1).toDouble(); ob.lon = q.value(2).toDouble(); ob.acc = q.value(3).toDouble(); ob.dbm = q.value(4).toInt(); r.obs << ob; }
        updatePosition(b, r, flags);
    }
    m_db.commit();
    if (added) markDirty();
    return added;
}

// ── Export / import ───────────────────────────────────────────────────────────
static QJsonArray dumpTable(const QSqlDatabase &db, const QString &sql)
{
    QJsonArray arr;
    QSqlQuery q(db); q.exec(sql);
    while (q.next()) {
        QJsonObject o;
        for (int i = 0; i < q.record().count(); ++i) {
            const QVariant v = q.value(i);
            const QString k = q.record().fieldName(i);
            if (v.isNull()) o[k] = QJsonValue(); else if (v.typeId() == QMetaType::Double) o[k] = v.toDouble(); else if (v.typeId() == QMetaType::LongLong || v.typeId() == QMetaType::Int) o[k] = v.toDouble(); else o[k] = v.toString();
        }
        arr.append(o);
    }
    return arr;
}

QJsonObject MapDb::exportJson() const
{
    if (!m_db.isOpen()) return {};
    QJsonObject o{{"beaconfix", "mapdb"}, {"version", 1}, {"exported", QDateTime::currentDateTime().toString(Qt::ISODate)}};
    o["aps"] = dumpTable(m_db, QStringLiteral("SELECT * FROM aps"));
    o["observations"] = dumpTable(m_db, QStringLiteral("SELECT bssid, time, lat, lon, acc, dbm, fix_source FROM observations ORDER BY id"));
    o["sightings"] = dumpTable(m_db, QStringLiteral("SELECT bssid, lat, lon, acc, time FROM sightings ORDER BY id"));
    o["cells"] = dumpTable(m_db, QStringLiteral("SELECT bssid, cell FROM cells"));
    o["flags"] = dumpTable(m_db, QStringLiteral("SELECT kind, bssid FROM flags"));
    o["fixes"] = dumpTable(m_db, QStringLiteral("SELECT time, lat, lon, acc, source, provider, place, city, region, country, elev, ap_count, ap_used, departed FROM fixes ORDER BY id"));
    o["pois"] = dumpTable(m_db, QStringLiteral("SELECT * FROM pois"));
    o["elevation"] = dumpTable(m_db, QStringLiteral("SELECT * FROM elevation"));
    o["achievements"] = dumpTable(m_db, QStringLiteral("SELECT * FROM achievements"));
    o["kv"] = dumpTable(m_db, QStringLiteral("SELECT * FROM kv"));
    return o;
}

int MapDb::importJson(const QJsonObject &dump, QString *error)
{
    if (!m_db.isOpen() || m_readOnly) { if (error) *error = QStringLiteral("database not writable"); return -1; }
    if (dump["beaconfix"].toString() != QLatin1String("mapdb")) { if (error) *error = QStringLiteral("not a BeaconFix database export"); return -1; }
    int n = 0;
    m_db.transaction();
    QSqlQuery q(m_db);
    auto insertRows = [&](const QString &table, const QJsonArray &rows, bool ignore) {
        for (const QJsonValue &v : rows) {
            const QJsonObject o = v.toObject();
            QStringList cols, marks; QVariantList vals;
            for (auto it = o.begin(); it != o.end(); ++it) { cols << it.key(); marks << QStringLiteral("?"); vals << (it.value().isNull() ? QVariant() : it.value().isDouble() ? QVariant(it.value().toDouble()) : QVariant(it.value().toString())); }
            if (cols.isEmpty()) continue;
            q.prepare(QStringLiteral("INSERT OR %1 INTO %2(%3) VALUES(%4)").arg(ignore ? QStringLiteral("IGNORE") : QStringLiteral("REPLACE"), table, cols.join(QLatin1Char(',')), marks.join(QLatin1Char(','))));
            for (const QVariant &val : vals) q.addBindValue(val);
            if (q.exec() && q.numRowsAffected() > 0) ++n;
        }
    };
    insertRows(QStringLiteral("aps"), dump["aps"].toArray(), true);           // keep what we know; add the rest
    insertRows(QStringLiteral("observations"), dump["observations"].toArray(), true);
    insertRows(QStringLiteral("sightings"), dump["sightings"].toArray(), true);
    insertRows(QStringLiteral("cells"), dump["cells"].toArray(), true);
    insertRows(QStringLiteral("flags"), dump["flags"].toArray(), true);
    insertRows(QStringLiteral("fixes"), dump["fixes"].toArray(), true);
    insertRows(QStringLiteral("pois"), dump["pois"].toArray(), true);
    insertRows(QStringLiteral("elevation"), dump["elevation"].toArray(), true);
    insertRows(QStringLiteral("achievements"), dump["achievements"].toArray(), true);
    m_db.commit();
    if (n) markDirty();
    return n;
}
