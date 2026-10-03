#include "mapdb.h"
#include "fitjson.h"
#include "hibf.h"
#include "cameraimport.h"
#include <QCryptographicHash>
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
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QSqlDriver>
#include <QStandardPaths>
#include <QThreadPool>
#include <QtMath>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <cmath>

// Every stored time is local ISO-8601 without an offset ("2026-10-01T19:40:00"): rows are ordered by the
// text, so a peer's "…Z" (UTC) or "+02:00" time is converted on the way in (the RV GNSS sent UTC: its
// fixes sorted 4 h away from the phone's, inside the route)
static QString localIso(const QString &t)
{
    if (!(t.endsWith(QLatin1Char('Z')) || t.contains(QLatin1Char('+')) || (t.size() > 19 && t.lastIndexOf(QLatin1Char('-')) > 16))) return t;
    const QDateTime d = QDateTime::fromString(t, Qt::ISODate);
    return d.isValid() ? d.toLocalTime().toString(Qt::ISODate) : t;
}
#include <limits>

static QJsonObject peRow(const QSqlQuery &q, bool withRaw);   // a plate_events row as JSON (below)

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
// BEACONFIX_KEY_FILE: the hub keeps its key where the unit says (e.g. a systemd credential), not next to the data
static QString keyFilePath()
{
    const QString env = qEnvironmentVariable("BEACONFIX_KEY_FILE");
    if (!env.isEmpty()) return env;
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/sworrl/beaconfix.key");
}

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
    if (qEnvironmentVariableIsSet("BEACONFIX_NO_KWALLET")) return {};   // the hub: no session bus at all, the key file
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

QByteArray MapDb::bootstrapKey(QString *source)
{
    QString src;
    QByteArray k = fileKey(false, &src);
    if (k.isEmpty()) k = walletKey(false, &src);
    if (k.isEmpty()) k = fileKey(true, &src);
    if (source) *source = src;
    return k;
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
    connect(&m_flushTimer, &QTimer::timeout, this, &MapDb::flushAsync);
    m_flushPool.setMaxThreadCount(1);
}

MapDb::~MapDb()
{
    if (m_db.isOpen()) { if (!m_readOnly) flush(); m_db.close(); }
    m_flushPool.waitForDone();
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
    routeInvalidate();
    const bool blobExists = QFile::exists(m_blobPath);
    if (readOnly && !blobExists) { m_error = QStringLiteral("no database yet"); return false; }
    if (!ensureKey(!readOnly)) return false;
    m_livePath = runtimeDir() + (readOnly ? QStringLiteral("/ro-%1.db").arg(QCoreApplication::applicationPid()) : QStringLiteral("/live.db"));
    // Read-only copies left by killed CLI runs (the widget times them out): each is a full plaintext copy in tmpfs (RAM)
    for (const QFileInfo &fi : QDir(runtimeDir()).entryInfoList({QStringLiteral("ro-*.db")}, QDir::Files)) {
        const QString pid = fi.completeBaseName().mid(3);
        if (pid.toLongLong() > 0 && !QFileInfo::exists(QStringLiteral("/proc/") + pid)) QFile::remove(fi.absoluteFilePath());
    }
    if (!decryptToLive(readOnly)) return false;
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    m_db.setDatabaseName(m_livePath);
    if (readOnly) m_db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!m_db.open()) { m_error = m_db.lastError().text(); return false; }
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("PRAGMA journal_mode=TRUNCATE"));
    q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    if (!readOnly && !schema()) return false;
    loadSeq();
    if (!readOnly && !blobExists) markDirty();                 // write the (empty) encrypted file right away
    return true;
}

// ── change sequence (for sync) ────────────────────────────────────────────────
void MapDb::loadSeq()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT value FROM kv WHERE key='seq'"));
    m_seq = q.next() ? q.value(0).toLongLong() : 0;
    for (const char *t : {"aps", "observations", "fixes", "estimates", "anchors", "flock_cameras", "plate_events"}) {
        QSqlQuery m(m_db); m.exec(QStringLiteral("SELECT COALESCE(MAX(seq),0) FROM %1").arg(QLatin1String(t)));
        if (m.next()) m_seq = qMax(m_seq, m.value(0).toLongLong());
    }
    if (m_readOnly) return;
    // Rows from before the sequence column existed (schema ≤ 2) have seq 0 and would never reach a
    // peer: number them once, in storage order, so the first sync carries the whole history.
    for (const char *t : {"aps", "observations", "fixes"}) {
        QSqlQuery c(m_db); c.exec(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE seq IS NULL OR seq=0").arg(QLatin1String(t)));
        if (!c.next() || c.value(0).toInt() == 0) continue;
        const QString order = QLatin1String(t) == QLatin1String("aps") ? QStringLiteral("rowid") : QStringLiteral("id");
        QSqlQuery sel(m_db); sel.exec(QStringLiteral("SELECT rowid FROM %1 WHERE seq IS NULL OR seq=0 ORDER BY %2").arg(QLatin1String(t), order));
        QList<qint64> rows; while (sel.next()) rows << sel.value(0).toLongLong();
        m_db.transaction();
        QSqlQuery up(m_db); up.prepare(QStringLiteral("UPDATE %1 SET seq=? WHERE rowid=?").arg(QLatin1String(t)));
        for (qint64 r : rows) { up.addBindValue(double(++m_seq)); up.addBindValue(double(r)); up.exec(); }
        QSqlQuery kv(m_db); kv.prepare(QStringLiteral("INSERT OR REPLACE INTO kv(key, value) VALUES('seq', ?)")); kv.addBindValue(QString::number(m_seq)); kv.exec();
        m_db.commit();
        markDirty();
    }
}
qint64 MapDb::nextSeq()
{
    ++m_seq;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO kv(key, value) VALUES('seq', ?)")); q.addBindValue(QString::number(m_seq)); q.exec();
    return m_seq;
}
QString MapDb::kv(const QString &key) const
{
    if (!m_db.isOpen()) return {};
    QSqlQuery q(m_db); q.prepare(QStringLiteral("SELECT value FROM kv WHERE key=?")); q.addBindValue(key); q.exec();
    return q.next() ? q.value(0).toString() : QString();
}
void MapDb::setKv(const QString &key, const QString &value)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db); q.prepare(QStringLiteral("INSERT OR REPLACE INTO kv(key, value) VALUES(?,?)")); q.addBindValue(key); q.addBindValue(value); q.exec();
    markDirty();
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
        // The pediatric ER search's answer (scope far), kept apart from the near list: sharing pois' (osm_type, osm_id) key,
        // a near save replaced every far row inside the near radius, so a restart lost the ERs closest to us.
        "CREATE TABLE IF NOT EXISTS pois_far (osm_type TEXT NOT NULL, osm_id INTEGER NOT NULL, cat TEXT, name TEXT, detail TEXT, lat REAL, lon REAL, wifi INTEGER,"
        " hours TEXT, phone TEXT, website TEXT, address TEXT DEFAULT '', wheelchair TEXT DEFAULT '', emergency INTEGER DEFAULT 0, scope TEXT DEFAULT 'far',"
        " peds INTEGER DEFAULT 0, er TEXT DEFAULT '', campus TEXT DEFAULT '', drive_s INTEGER DEFAULT 0, drive_m INTEGER DEFAULT 0, PRIMARY KEY (osm_type, osm_id))",
        "CREATE TABLE IF NOT EXISTS elevation (cell TEXT PRIMARY KEY, elev REAL, time TEXT)",
        "CREATE TABLE IF NOT EXISTS achievements (key TEXT PRIMARY KEY, unlocked TEXT)",
        "CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT)",
        // Our own fit per beacon (see estimator.h): position, error ellipse, model, statistics
        "CREATE TABLE IF NOT EXISTS estimates (bssid TEXT PRIMARY KEY, lat REAL, lon REAL, acc REAL, semi_major REAL, semi_minor REAL, orient REAL, rms REAL,"
        " p0 REAL, pathloss REAL, fitted_n INTEGER, n INTEGER, vantage INTEGER, rejected INTEGER, quality TEXT, updated TEXT, seq INTEGER DEFAULT 0)",
        // Flock / ALPR cameras and community vetting records
        "CREATE TABLE IF NOT EXISTS flock_cameras (id TEXT PRIMARY KEY, lat REAL NOT NULL, lon REAL NOT NULL, source TEXT, model TEXT, operator TEXT, direction TEXT, bssid TEXT, ble_mac TEXT, confidence INTEGER DEFAULT 100, detection_method TEXT, first_seen TEXT, last_seen TEXT, sighting_count INTEGER DEFAULT 1, vetted INTEGER DEFAULT 0, vetted_at TEXT, notes TEXT, seq INTEGER DEFAULT 0)",
        "CREATE INDEX IF NOT EXISTS flock_pos ON flock_cameras(lat, lon)",
    };
    QSqlQuery q(m_db);
    for (const char *s : ddl) if (!q.exec(QString::fromLatin1(s))) { m_error = q.lastError().text(); return false; }
    for (const QString &stmt : Anchors::schemaSql().split(QLatin1Char(';'), Qt::SkipEmptyParts))   // docs/RANGING.md §4.2
        if (!stmt.trimmed().isEmpty() && !q.exec(stmt.trimmed())) { m_error = q.lastError().text(); return false; }
    // Columns added after schema 1 (safe to run every time)
    QSqlQuery cols(m_db); cols.exec(QStringLiteral("PRAGMA table_info(aps)"));
    QSet<QString> have; while (cols.next()) have.insert(cols.value(1).toString());
    const QList<QPair<QString, QString>> extra{{QStringLiteral("security"), QStringLiteral("TEXT")}, {QStringLiteral("sec_flags"), QStringLiteral("INTEGER DEFAULT 0")},
                                               {QStringLiteral("wpa_flags"), QStringLiteral("INTEGER DEFAULT 0")}, {QStringLiteral("rsn_flags"), QStringLiteral("INTEGER DEFAULT 0")},
                                               {QStringLiteral("max_kbps"), QStringLiteral("INTEGER DEFAULT 0")}, {QStringLiteral("adhoc"), QStringLiteral("INTEGER DEFAULT 0")}};
    for (const auto &c : extra) if (!have.contains(c.first)) q.exec(QStringLiteral("ALTER TABLE aps ADD COLUMN %1 %2").arg(c.first, c.second));
    // Schema 3: change sequence numbers, per-device observations, positions synced from peers
    const QList<QPair<QString, QString>> extra3{{QStringLiteral("seq"), QStringLiteral("INTEGER DEFAULT 0")}, {QStringLiteral("peer_lat"), QStringLiteral("REAL")},
                                                {QStringLiteral("peer_lon"), QStringLiteral("REAL")}, {QStringLiteral("peer_acc"), QStringLiteral("REAL")}, {QStringLiteral("peer_from"), QStringLiteral("TEXT")}};
    for (const auto &c : extra3) if (!have.contains(c.first)) q.exec(QStringLiteral("ALTER TABLE aps ADD COLUMN %1 %2").arg(c.first, c.second));
    auto addCol = [&](const char *table, const char *col, const char *type) {
        QSqlQuery ti(m_db); ti.exec(QStringLiteral("PRAGMA table_info(%1)").arg(QLatin1String(table)));
        bool has = false; while (ti.next()) if (ti.value(1).toString() == QLatin1String(col)) has = true;
        if (!has) q.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3").arg(QLatin1String(table), QLatin1String(col), QLatin1String(type)));
    };
    addCol("observations", "seq", "INTEGER DEFAULT 0"); addCol("observations", "device", "TEXT DEFAULT ''");
    addCol("observations", "range_m", "REAL"); addCol("observations", "range_sd", "REAL");   // Wi-Fi RTT to the AP (the phone, when it answered)
    {   // Once: UTC / offset times stored by older peers → local (a clash with an existing local row = a duplicate: dropped)
        QSqlQuery m(m_db);
        m.exec(QStringLiteral("SELECT value FROM kv WHERE key='tz_local_v1'"));
        if (!m.next()) {
            const QString isOffset = QStringLiteral("(time LIKE '%Z' OR time LIKE '%+__:__' OR (length(time) > 19 AND substr(time, -6, 1) = '-'))");
            for (const char *t : {"fixes", "observations"}) {
                m.exec(QStringLiteral("UPDATE OR IGNORE %1 SET time = strftime('%Y-%m-%dT%H:%M:%S', time, 'localtime') WHERE %2").arg(QLatin1String(t), isOffset));
                m.exec(QStringLiteral("DELETE FROM %1 WHERE %2").arg(QLatin1String(t), isOffset));
            }
            m.exec(QStringLiteral("INSERT OR REPLACE INTO kv(key, value) VALUES('tz_local_v1', '1')"));
        }
    }
    addCol("fixes", "seq", "INTEGER DEFAULT 0"); addCol("fixes", "device", "TEXT DEFAULT ''");
    addCol("pois", "address", "TEXT DEFAULT ''"); addCol("pois", "wheelchair", "TEXT DEFAULT ''"); addCol("pois", "emergency", "INTEGER DEFAULT 0");
    // Pediatric ERs (3.8): which search a place came from, its tier / ER status / campus ER, a stored drive time (0 = estimate)
    addCol("pois", "scope", "TEXT DEFAULT 'near'"); addCol("pois", "peds", "INTEGER DEFAULT 0"); addCol("pois", "er", "TEXT DEFAULT ''");
    addCol("pois", "campus", "TEXT DEFAULT ''"); addCol("pois", "drive_s", "INTEGER DEFAULT 0"); addCol("pois", "drive_m", "INTEGER DEFAULT 0");
    // far rows written by an earlier 3.8 build move from pois to pois_far (the ones a near save had not yet replaced)
    q.exec(QStringLiteral("INSERT OR IGNORE INTO pois_far(osm_type, osm_id, cat, name, detail, lat, lon, wifi, hours, phone, website, address, wheelchair, emergency, scope, peds, er,"
                          " campus, drive_s, drive_m) SELECT osm_type, osm_id, cat, name, detail, lat, lon, wifi, hours, phone, website, address, wheelchair, emergency, 'far', peds,"
                          " er, campus, drive_s, drive_m FROM pois WHERE scope='far'"));
    q.exec(QStringLiteral("DELETE FROM pois WHERE scope='far'"));
    // Estimator 2 (3.9, docs/GRADING.md): graded estimates, their history, the cells this host scanned from
    addCol("estimates", "kind", "TEXT DEFAULT ''"); addCol("estimates", "grade", "TEXT DEFAULT ''"); addCol("estimates", "score", "REAL");
    addCol("estimates", "r95", "REAL"); addCol("estimates", "cep50", "REAL"); addCol("estimates", "p_within25", "REAL");
    addCol("estimates", "cxx", "REAL"); addCol("estimates", "cxy", "REAL"); addCol("estimates", "cyy", "REAL");
    addCol("estimates", "metrics", "TEXT DEFAULT ''"); addCol("estimates", "version", "INTEGER DEFAULT 0");
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS estimate_history (id INTEGER PRIMARY KEY, bssid TEXT NOT NULL, time TEXT, lat REAL, lon REAL,"
                          " cxx REAL, cxy REAL, cyy REAL, r95 REAL, score REAL, grade TEXT, kind TEXT)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS esthist_bssid ON estimate_history(bssid)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS scan_cells (cell TEXT PRIMARY KEY, lat REAL, lon REAL, count INTEGER DEFAULT 0, first INTEGER, last INTEGER)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS obs_seq ON observations(seq)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS aps_seq ON aps(seq)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS fix_seq ON fixes(seq)"));
    q.exec(QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS obs_dedup ON observations(bssid, time, device)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS fix_device_time ON fixes(device, time)"));   // appendPeerFixes dedup + per-device history: not a table scan per row
    addCol("flock_cameras", "pass_count", "INTEGER DEFAULT 0");
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS license_plates (plate TEXT PRIMARY KEY, display_plate TEXT NOT NULL, state TEXT NOT NULL, vehicle_desc TEXT, make TEXT, model TEXT, color TEXT, active INTEGER DEFAULT 1, added_at TEXT, notes TEXT)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS camera_encounters (id INTEGER PRIMARY KEY AUTOINCREMENT, camera_id TEXT NOT NULL, time TEXT NOT NULL, lat REAL NOT NULL, lon REAL NOT NULL, distance_m REAL NOT NULL, speed_kmh REAL DEFAULT 0, plate TEXT, vehicle_desc TEXT, device TEXT, encounter_num INTEGER DEFAULT 1, notes TEXT)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS encounter_cam ON camera_encounters(camera_id)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS encounter_time ON camera_encounters(time)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS plate_audits (id INTEGER PRIMARY KEY AUTOINCREMENT, plate TEXT NOT NULL, camera_id TEXT, lat REAL, lon REAL, operator TEXT, timestamp TEXT, source TEXT, confidence INTEGER DEFAULT 100, details TEXT)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS audit_plate ON plate_audits(plate)"));   // 3.9 and older; no longer written (docs/SIGHTINGS.md)
    // Plate events (docs/SIGHTINGS.md §1): camera passes and plate searches, their images
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS plate_events (id INTEGER PRIMARY KEY, uid TEXT NOT NULL UNIQUE, kind TEXT NOT NULL, plate TEXT, time TEXT NOT NULL,"
                          " lat REAL, lon REAL, acc REAL, camera_id TEXT, camera_lat REAL, camera_lon REAL, distance_m REAL, speed_kmh REAL, heading_deg REAL,"
                          " approach_bearing_deg REAL, camera_dir_deg REAL, facing INTEGER, operator TEXT, agency TEXT, model TEXT, camera_type TEXT,"
                          " source TEXT NOT NULL, source_url TEXT, source_name TEXT, confidence INTEGER, leaky INTEGER DEFAULT 0, details TEXT, metrics TEXT, raw TEXT,"
                          " device TEXT, created_at TEXT, updated_at TEXT, seq INTEGER DEFAULT 0)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS plate_events_time ON plate_events(time)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS plate_events_camera ON plate_events(camera_id, time)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS plate_events_seq ON plate_events(seq)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS plate_event_media (id INTEGER PRIMARY KEY, uid TEXT NOT NULL UNIQUE, event_uid TEXT, camera_id TEXT, kind TEXT NOT NULL,"
                          " mime TEXT NOT NULL, data BLOB NOT NULL, width INTEGER, height INTEGER, bytes INTEGER, original_url TEXT, original_mime TEXT, original_bytes INTEGER,"
                          " original_sha256 TEXT, jpeg_reconstructible INTEGER DEFAULT 0, attribution TEXT, license TEXT, captured_at TEXT, created_at TEXT)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS plate_event_media_event ON plate_event_media(event_uid)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS plate_event_media_camera ON plate_event_media(camera_id)"));
    // a uid merged into another pass (§1.1): the phone may still name it (media uploads)
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS plate_event_alias (uid TEXT PRIMARY KEY, target TEXT NOT NULL)"));
    // §2.0: what kind of camera (only an ALPR reads plates), and its OSM tags when known
    addCol("flock_cameras", "camera_type", "TEXT DEFAULT ''");
    addCol("flock_cameras", "tags", "TEXT DEFAULT ''");
    // 2026-10 camera source (docs/DATABASE.md): DeFlock's brand as manufacturer, the real OSM version / timestamp, and
    // rows their source no longer confirms (kept, stale, for the passes they have)
    addCol("flock_cameras", "manufacturer", "TEXT DEFAULT ''");
    addCol("flock_cameras", "osm_version", "INTEGER DEFAULT 0");
    addCol("flock_cameras", "osm_timestamp", "TEXT DEFAULT ''");
    addCol("flock_cameras", "stale", "INTEGER DEFAULT 0");
    // Phase D (docs/SIGHTINGS.md §2.6, §2.7, §4.6): the camera's trust (log-odds → 0–1, NULL = not computed yet) and its
    // terms, the user's verdict, the road it watches (an OSM way id) and when its roads were fetched, its agency portal
    addCol("flock_cameras", "trust", "REAL");
    addCol("flock_cameras", "trust_detail", "TEXT DEFAULT ''");
    addCol("flock_cameras", "verdict", "TEXT DEFAULT ''");
    addCol("flock_cameras", "verdict_at", "TEXT DEFAULT ''");
    addCol("flock_cameras", "watched_way", "INTEGER DEFAULT 0");
    addCol("flock_cameras", "watched_detail", "TEXT DEFAULT ''");
    addCol("flock_cameras", "ways_fetched", "TEXT DEFAULT ''");
    addCol("flock_cameras", "agency_portal", "TEXT DEFAULT ''");
    // §2.6: the drivable OSM ways around cameras (Overpass), cached 90 days; bbox columns for the area lookup
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS osm_ways (id INTEGER PRIMARY KEY, highway TEXT, name TEXT, ref TEXT, layer INTEGER DEFAULT 0,"
                          " bridge INTEGER DEFAULT 0, tunnel INTEGER DEFAULT 0, oneway INTEGER DEFAULT 0, nodes TEXT, geom TEXT,"
                          " min_lat REAL, max_lat REAL, min_lon REAL, max_lon REAL, fetched TEXT)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS osm_ways_box ON osm_ways(min_lat, max_lat)"));
    // §4.6: Eyes on Flock's transparency-portal facts (CC BY-SA 4.0), replaced by every weekly fetch
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS eof_portals (slug TEXT PRIMARY KEY, url TEXT, city TEXT, county TEXT, state TEXT, type TEXT,"
                          " population REAL, cameras REAL, searches REAL, retention_days REAL, vehicles REAL, hotlist_hits REAL, hotlist_rate REAL,"
                          " shared_with INTEGER, received_from INTEGER, prohibited_uses TEXT, public_audit INTEGER DEFAULT 0, updated TEXT, tokens TEXT, fetched TEXT)"));
    migrateCameras();
    {   // classify the rows that have no type yet (every row once per classifier version; later only new ones)
        QSqlQuery c(m_db);
        c.exec(QStringLiteral("SELECT value FROM kv WHERE key='camera_type_v3'"));
        const bool all = !c.next();
        c.exec(QStringLiteral("SELECT COUNT(*) FROM flock_cameras WHERE camera_type IS NULL OR camera_type=''"));
        if (all || (c.next() && c.value(0).toInt() > 0)) {
            const QString where = all ? QString() : QStringLiteral(" WHERE camera_type IS NULL OR camera_type=''");
            m_db.transaction();
            QSqlQuery rows(m_db);
            rows.setForwardOnly(true);
            rows.exec(QStringLiteral("SELECT id, model, source, detection_method, tags FROM flock_cameras%1").arg(where));
            QList<QPair<QString, QString>> set;
            while (rows.next()) {
                const QJsonObject tags = QJsonDocument::fromJson(rows.value(4).toString().toUtf8()).object();
                set.append({rows.value(0).toString(), PlateEvents::classifyCamera(rows.value(1).toString(), rows.value(2).toString(), rows.value(0).toString(),
                                                                                 rows.value(3).toString(), tags)});
            }
            QSqlQuery up(m_db);
            up.prepare(QStringLiteral("UPDATE flock_cameras SET camera_type=? WHERE id=?"));
            for (const auto &r : std::as_const(set)) { up.addBindValue(r.second); up.addBindValue(r.first); up.exec(); }
            QSqlQuery mark(m_db); mark.exec(QStringLiteral("INSERT OR REPLACE INTO kv(key, value) VALUES('camera_type_v3', '1')"));
            m_db.commit();
            qInfo("beaconfix: classified %d surveillance cameras (alpr / webcam / ptz / cctv / enforcement / not_camera, docs/SIGHTINGS.md §2.0)", int(set.size()));
        }
    }
    q.exec(QStringLiteral("INSERT OR IGNORE INTO kv(key, value) VALUES ('schema', '3')"));
    q.exec(QStringLiteral("UPDATE kv SET value='3' WHERE key='schema'"));
    q.exec(QStringLiteral("INSERT OR IGNORE INTO kv(key, value) VALUES ('created', '%1')").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    return true;
}

// Once (kv camera_clean_v1): the camera table after the 2026-10 signature and source corrections (docs/DATABASE.md).
// 1. a row with OSM tags takes operator / manufacturer / model from them (an older import folded brand / manufacturer
//    into the operator and wrote surveillance:type as the model);
// 2. "Flock Safety" as the operator of a tagless row was never data (the old bulk import and the RF detector wrote it);
// 3. RF detections made with the retracted signatures (generic Espressif / consumer OUIs, random BLE addresses, …) are
//    re-checked against the current rules: the ones that no longer hold are deleted, or only marked stale when they
//    have camera passes. The old bulk import's own rows are reconciled against DeFlock by the next camera sync.
void MapDb::migrateCameras()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT value FROM kv WHERE key='camera_clean_v1'"));
    if (q.next()) return;
    m_db.transaction();
    int fromTags = 0, operators = 0, dropped = 0, staled = 0;
    {
        QSqlQuery rows(m_db);
        rows.setForwardOnly(true);
        rows.exec(QStringLiteral("SELECT id, tags FROM flock_cameras WHERE tags IS NOT NULL AND tags<>''"));
        QList<QStringList> set;
        while (rows.next()) {
            const QJsonObject t = QJsonDocument::fromJson(rows.value(1).toString().toUtf8()).object();
            if (t.isEmpty()) continue;
            auto tag = [&t](const char *k) { return t.value(QLatin1String(k)).toString().trimmed(); };
            set << QStringList{rows.value(0).toString(), tag("operator"), !tag("manufacturer").isEmpty() ? tag("manufacturer") : tag("brand"),
                               !tag("model").isEmpty() ? tag("model") : tag("camera:model")};
        }
        QSqlQuery up(m_db);
        up.prepare(QStringLiteral("UPDATE flock_cameras SET operator=?, manufacturer=?, model=? WHERE id=?"));
        for (const QStringList &r : std::as_const(set)) { up.addBindValue(r[1]); up.addBindValue(r[2]); up.addBindValue(r[3]); up.addBindValue(r[0]); if (up.exec()) ++fromTags; }
    }
    q.exec(QStringLiteral("UPDATE flock_cameras SET manufacturer = CASE WHEN COALESCE(manufacturer,'')='' AND (model LIKE '%flock%' OR id LIKE 'det:%') THEN 'Flock Safety'"
                          " ELSE COALESCE(manufacturer,'') END, operator='' WHERE operator='Flock Safety' AND COALESCE(tags,'')=''"));
    operators = q.numRowsAffected();
    {
        QSet<QString> passed;
        q.exec(QStringLiteral("SELECT DISTINCT camera_id FROM plate_events WHERE camera_id LIKE 'det:%'"));
        while (q.next()) passed.insert(q.value(0).toString());
        QSqlQuery rows(m_db);
        rows.exec(QStringLiteral("SELECT id, COALESCE(bssid,''), COALESCE(detection_method,'') FROM flock_cameras WHERE id LIKE 'det:%'"));
        QStringList drop, stale;
        QSqlQuery ssid(m_db);
        ssid.prepare(QStringLiteral("SELECT ssid FROM aps WHERE bssid=? COLLATE NOCASE"));
        static const QRegularExpression stillValidBle(QStringLiteral("ble_name_exact|ble_name_penguin|ble_raven_services"));
        while (rows.next()) {
            const QString id = rows.value(0).toString(), bssid = rows.value(1).toString(), method = rows.value(2).toString();
            bool keep;
            if (!bssid.isEmpty()) {
                ssid.addBindValue(bssid);
                const QString name = ssid.exec() && ssid.next() ? ssid.value(0).toString() : QString();
                keep = FlockDetector::evaluateWifi(bssid, name).isFlock;
            } else {
                keep = stillValidBle.match(method).hasMatch();   // an exact Flock name or Raven services; OUI-only / XUNTONG-only were not
            }
            if (!keep) (passed.contains(id) ? stale : drop) << id;
        }
        QSqlQuery del(m_db), st(m_db);
        del.prepare(QStringLiteral("DELETE FROM flock_cameras WHERE id=?"));
        st.prepare(QStringLiteral("UPDATE flock_cameras SET stale=1 WHERE id=?"));
        for (const QString &id : std::as_const(drop)) { del.addBindValue(id); if (del.exec()) ++dropped; }
        for (const QString &id : std::as_const(stale)) { st.addBindValue(id); if (st.exec()) ++staled; }
    }
    const QJsonObject summary{{"time", QDateTime::currentDateTime().toString(Qt::ISODate)}, {"fromTags", fromTags}, {"operatorCleared", operators},
                              {"detectionsDeleted", dropped}, {"detectionsStale", staled}};
    QSqlQuery mark(m_db);
    mark.prepare(QStringLiteral("INSERT OR REPLACE INTO kv(key, value) VALUES('camera_clean_v1', ?)"));
    mark.addBindValue(QString::fromUtf8(QJsonDocument(summary).toJson(QJsonDocument::Compact)));
    mark.exec();
    m_db.commit();
    qInfo("beaconfix: camera cleanup: %d from their OSM tags, %d invented operators cleared, %d retracted RF detections deleted, %d marked stale",
          fromTags, operators, dropped, staled);
}

void MapDb::markDirty()
{
    if (m_readOnly) return;
    m_dirty = true;
    if (!m_dirtyAge.isValid()) m_dirtyAge.start();
    if (m_batch) return;                                   // saveApRecords: one changed() for the whole batch, not one per record
    scheduleFlush();
    emit changed();
}

// Debounced 4 s after the last change, but a change stream (a scan every few seconds) can't put the write off
// more than ~30 s: past that the running timer is left to fire.
void MapDb::scheduleFlush()
{
    if (!m_dirty || m_readOnly) return;
    if (!m_flushTimer.isActive() || !m_dirtyAge.isValid() || m_dirtyAge.elapsed() < 30000) m_flushTimer.start();
}

// The live file is read here, on the owning thread between statements (no transaction is open in the event
// loop, so the bytes are consistent); the AES pass and the ~100 MB write run on a worker so the GUI doesn't stall.
void MapDb::flushAsync()
{
    if (!m_dirty || m_readOnly || !m_db.isOpen()) return;
    if (m_flushing) { m_flushTimer.start(); return; }         // one at a time: try again after this one lands
    QFile live(m_livePath);
    if (!live.open(QIODevice::ReadOnly)) { m_error = live.errorString(); return; }
    QByteArray plain = live.readAll();
    m_dirty = false; m_dirtyAge.invalidate(); m_flushing = true;
    m_flushPool.start([this, plain = std::move(plain), key = m_key, path = m_blobPath] {
        QString err;
        const bool ok = writeBlob(key, plain, path, &err);
        QMetaObject::invokeMethod(this, [this, ok, err] {
            m_flushing = false;
            if (!ok) { m_error = err; m_dirty = true; if (!m_dirtyAge.isValid()) m_dirtyAge.start(); m_flushTimer.start(); }
        }, Qt::QueuedConnection);
    });
}

void MapDb::flush()
{
    m_flushPool.waitForDone();                                // a background write in flight lands first
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    m_flushing = false;
    if (!m_dirty || m_readOnly || !m_db.isOpen()) return;
    QFile live(m_livePath);
    if (!live.open(QIODevice::ReadOnly)) { m_error = live.errorString(); return; }
    QString err;
    if (writeBlob(m_key, live.readAll(), m_blobPath, &err)) { m_dirty = false; m_dirtyAge.invalidate(); } else m_error = err;
}

bool MapDb::writeBlob(const QByteArray &key, const QByteArray &plain, const QString &path, QString *error)
{
    const QByteArray blob = encrypt(key, plain, error);
    if (blob.isEmpty()) return false;
    QDir().mkpath(QFileInfo(path).path());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { *error = f.errorString(); return false; }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(blob);
    if (!f.commit()) { *error = f.errorString(); return false; }
    return true;
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
    q.exec(QStringLiteral("SELECT COUNT(*) FROM estimates WHERE quality<>'none'")); o["estimates"] = q.next() ? q.value(0).toInt() : 0;
    q.exec(QStringLiteral("SELECT COUNT(*) FROM estimates WHERE quality='good'")); o["estimatesGood"] = q.next() ? q.value(0).toInt() : 0;
    {   QJsonObject g; q.exec(QStringLiteral("SELECT grade, COUNT(*) FROM estimates WHERE grade<>'' GROUP BY grade"));
        while (q.next()) g[q.value(0).toString()] = q.value(1).toInt();
        o["grades"] = g; }
    q.exec(QStringLiteral("SELECT COUNT(*) FROM scan_cells")); o["scanCells"] = q.next() ? q.value(0).toInt() : 0;
    q.exec(QStringLiteral("SELECT COUNT(*) FROM aps WHERE source='trilat'")); o["apsTrilat"] = q.next() ? q.value(0).toInt() : 0;
    q.exec(QStringLiteral("SELECT COUNT(*) FROM aps WHERE peer_from IS NOT NULL AND peer_from<>''")); o["apsFromPeers"] = q.next() ? q.value(0).toInt() : 0;
    q.exec(QStringLiteral("SELECT COUNT(DISTINCT device) FROM observations WHERE device<>''")); o["observingDevices"] = q.next() ? q.value(0).toInt() : 0;
    o["seq"] = double(m_seq);
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
    // Our own fit beats a placement when it is at least as tight; a placement (WiGLE / Apple, ±25 m)
    // beats a loose fit; a peer's position fills in when we have neither.
    const bool fit = r.fit.valid && r.fit.quality != QLatin1String("none")
                  && (r.fit.kind == QLatin1String("fix") || r.fit.kind == QLatin1String("none"));   // a region (grade R) is not a position; "none" = a fit from before 3.9
    if (fit && (!r.wigle || r.fit.acc <= 25.0)) { *lat = r.fit.lat; *lon = r.fit.lon; *acc = r.fit.acc; *source = QStringLiteral("trilat"); return true; }
    if (r.wigle) { *lat = r.wLat; *lon = r.wLon; *acc = 25; *source = QStringLiteral("placed"); return true; }
    if (r.hasPeer()) { *lat = r.peerLat; *lon = r.peerLon; *acc = r.peerAcc; *source = QStringLiteral("peer"); return true; }
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
    const bool have = positionFrom(r, &lat, &lon, &acc, &source);
    // Only a real change gets a new sequence number, so peers do not re-download every AP on every save
    QSqlQuery cur(m_db);
    cur.prepare(QStringLiteral("SELECT lat, lon, acc, source, home, travelling, ignored FROM aps WHERE bssid=?")); cur.addBindValue(bssid); cur.exec();
    bool changed = true;
    if (cur.next()) {
        const bool had = !cur.value(0).isNull();
        changed = had != have || (have && (std::fabs(cur.value(0).toDouble() - lat) > 1e-7 || std::fabs(cur.value(1).toDouble() - lon) > 1e-7
                                           || std::fabs(cur.value(2).toDouble() - acc) > 0.5 || cur.value(3).toString() != source))
               || cur.value(4).toInt() != ((flags & 1) ? 1 : 0) || cur.value(5).toInt() != ((flags & 2) ? 1 : 0) || cur.value(6).toInt() != ((flags & 4) ? 1 : 0);
    }
    if (!changed) return;
    QSqlQuery q(m_db);
    if (have) {
        q.prepare(QStringLiteral("UPDATE aps SET lat=?, lon=?, acc=?, source=?, home=?, travelling=?, ignored=?, seq=? WHERE bssid=?"));
        q.addBindValue(lat); q.addBindValue(lon); q.addBindValue(acc); q.addBindValue(source);
    } else {
        q.prepare(QStringLiteral("UPDATE aps SET lat=NULL, lon=NULL, acc=NULL, source=NULL, home=?, travelling=?, ignored=?, seq=? WHERE bssid=?"));
    }
    q.addBindValue(flags & 1 ? 1 : 0); q.addBindValue(flags & 2 ? 1 : 0); q.addBindValue(flags & 4 ? 1 : 0); q.addBindValue(double(nextSeq())); q.addBindValue(bssid);
    q.exec();
}

// ── AP records ────────────────────────────────────────────────────────────────
QHash<QString, ApRecord> MapDb::loadApRecords(QSet<QString> *travelling, QSet<QString> *notTravelling) const
{
    QHash<QString, ApRecord> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    QHash<QString, int> storedFlags;
    q.exec(QStringLiteral("SELECT bssid, ssid, freq, wigle, wlat, wlon, wigle_checked, security, sec_flags, wpa_flags, rsn_flags, max_kbps, adhoc, peer_lat, peer_lon, peer_acc, peer_from, home, travelling, ignored FROM aps"));
    while (q.next()) {
        ApRecord &r = out[q.value(0).toString()];
        storedFlags.insert(q.value(0).toString(), (q.value(17).toInt() ? 1 : 0) | (q.value(18).toInt() ? 2 : 0) | (q.value(19).toInt() ? 4 : 0));
        r.ssid = q.value(1).toString(); r.freq = q.value(2).toInt();
        r.wigle = q.value(3).toInt() != 0; r.wLat = q.value(4).toDouble(); r.wLon = q.value(5).toDouble();
        r.wigleChecked = QDateTime::fromString(q.value(6).toString(), Qt::ISODate);
        r.security = q.value(7).toString(); r.secFlags = q.value(8).toInt(); r.wpaFlags = q.value(9).toInt(); r.rsnFlags = q.value(10).toInt(); r.maxKbps = q.value(11).toInt(); r.adhoc = q.value(12).toInt() != 0;
        if (!q.value(13).isNull() && !q.value(16).toString().isEmpty()) { r.peerLat = q.value(13).toDouble(); r.peerLon = q.value(14).toDouble(); r.peerAcc = q.value(15).toDouble(); r.peerFrom = q.value(16).toString(); }
    }
    q.exec(QStringLiteral("SELECT bssid, time, lat, lon, acc, dbm, id, device, range_m, range_sd FROM observations ORDER BY id"));
    while (q.next()) {
        auto it = out.find(q.value(0).toString()); if (it == out.end()) continue;
        ApObservation o; o.time = QDateTime::fromString(q.value(1).toString(), Qt::ISODate);
        o.lat = q.value(2).toDouble(); o.lon = q.value(3).toDouble(); o.acc = q.value(4).toDouble(); o.dbm = q.value(5).toInt();
        o.id = q.value(6).toLongLong(); o.device = q.value(7).toString();
        if (!q.value(8).isNull() && q.value(8).toDouble() > 0) { o.rangeM = q.value(8).toDouble(); o.rangeSd = q.value(9).toDouble(); }
        it->obs.append(o);
    }
    q.exec(QStringLiteral("SELECT bssid, lat, lon, acc, semi_major, semi_minor, orient, rms, p0, pathloss, fitted_n, n, vantage, rejected, quality, updated, metrics FROM estimates"));
    while (q.next()) {
        auto it = out.find(q.value(0).toString()); if (it == out.end()) continue;
        Estimator::Fit &f = it->fit;
        const QString metrics = q.value(16).toString();
        if (!metrics.isEmpty()) {                       // estimator 2: every field
            const QJsonObject j = QJsonDocument::fromJson(metrics.toUtf8()).object();
            if (!j.isEmpty()) { f = Estimator::fromStorage(j); it->fitCurrent = j["v"].toInt() >= Estimator::kVersion; continue; }
        }
        f.lat = q.value(1).toDouble(); f.lon = q.value(2).toDouble(); f.acc = q.value(3).toDouble(); f.semiMajor = q.value(4).toDouble(); f.semiMinor = q.value(5).toDouble();
        f.orientDeg = q.value(6).toDouble(); f.rms = q.value(7).toDouble(); f.p0 = q.value(8).toDouble(); f.pathloss = q.value(9).toDouble(); f.fittedN = q.value(10).toInt() != 0;
        f.n = q.value(11).toInt(); f.vantage = q.value(12).toInt(); f.rejected = q.value(13).toInt(); f.quality = q.value(14).toString();
        f.updated = QDateTime::fromString(q.value(15).toString(), Qt::ISODate).toSecsSinceEpoch();
        f.valid = f.quality != QLatin1String("none") && !f.quality.isEmpty();
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
    // What is stored now: saveApRecords() writes a record again only when its signature changes
    m_recSig.clear();
    for (auto it = out.constBegin(); it != out.constEnd(); ++it) m_recSig.insert(it.key(), recordSignature(it.value(), storedFlags.value(it.key(), 0)));
    return out;
}

// FNV-1a over everything saveRecord() writes. Observations that are new (id 0) or edited (dirty) make the
// record dirty by themselves; the rest is summarised by count and the last one.
quint64 MapDb::recordSignature(const ApRecord &r, int flags)
{
    quint64 h = 1469598103934665603ULL;
    auto mix = [&h](const void *p, size_t n) { const unsigned char *c = static_cast<const unsigned char *>(p); for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ULL; } };
    auto mixD = [&](double d) { mix(&d, sizeof d); };
    auto mixI = [&](qint64 i) { mix(&i, sizeof i); };
    auto mixS = [&](const QString &s) { mix(s.constData(), size_t(s.size()) * sizeof(QChar)); mixI(s.size()); };
    for (const ApObservation &o : r.obs) if (o.id == 0 || o.dirty) return 0;          // 0 never matches a stored signature
    mixS(r.ssid); mixI(r.freq); mixI(flags);
    mixI(r.wigle); mixD(r.wLat); mixD(r.wLon); mixI(r.wigleChecked.isValid() ? r.wigleChecked.toSecsSinceEpoch() : 0);
    mixS(r.security); mixI(r.secFlags); mixI(r.wpaFlags); mixI(r.rsnFlags); mixI(r.maxKbps); mixI(r.adhoc);
    mixD(r.peerLat); mixD(r.peerLon); mixD(r.peerAcc); mixS(r.peerFrom);
    mixI(r.obs.size()); if (!r.obs.isEmpty()) { mixI(r.obs.last().id); mixD(r.obs.last().lat); mixD(r.obs.last().lon); mixI(r.obs.last().dbm); }
    mixI(r.fit.valid); if (r.fit.valid) { mixD(r.fit.lat); mixD(r.fit.lon); mixD(r.fit.acc); }
    mixI(r.seen.size()); for (const ApSighting &sg : r.seen) { mixD(sg.lat); mixD(sg.lon); mixD(sg.acc); mixI(sg.time.isValid() ? sg.time.toSecsSinceEpoch() : 0); }
    QStringList cells(r.cells.begin(), r.cells.end()); cells.sort(); for (const QString &c : cells) mixS(c);
    return h ? h : 1;
}

void MapDb::saveApRecords(QHash<QString, ApRecord> &recs, const QSet<QString> &travelling, const QSet<QString> &notTravelling, const QHash<QString, int> &flags)
{
    if (!m_db.isOpen() || m_readOnly) return;
    m_batch = true;                                        // thousands of records: listeners (stats, views) hear about it once
    m_db.transaction();
    int written = 0;
    // Only records whose content changed since they were stored (per-record signature): a Wi-Fi scan touches a few
    // dozen of them, not the ~100k a well-travelled database holds.
    for (auto it = recs.begin(); it != recs.end(); ++it) {
        const int f = flags.value(it.key(), 0);
        const quint64 sig = recordSignature(it.value(), f);
        if (sig != 0 && m_recSig.value(it.key()) == sig) continue;
        saveRecord(it.key(), it.value(), f);
        ++written;
    }
    // The travelling flags table only when the sets changed
    quint64 fh = 1469598103934665603ULL;
    for (const QSet<QString> *set : {&travelling, &notTravelling}) {
        QStringList l(set->begin(), set->end()); l.sort();
        for (const QString &b : l) { for (QChar c : b) { fh ^= c.unicode(); fh *= 1099511628211ULL; } fh ^= 0xff; fh *= 1099511628211ULL; }
        fh ^= 0x1234; fh *= 1099511628211ULL;
    }
    if (fh != m_flagsSig) {
        QSqlQuery q(m_db), ins(m_db);
        q.exec(QStringLiteral("DELETE FROM flags"));
        ins.prepare(QStringLiteral("INSERT OR IGNORE INTO flags(kind, bssid) VALUES(?,?)"));
        for (const QString &b : travelling) { ins.addBindValue(QStringLiteral("travelling")); ins.addBindValue(b); ins.exec(); }
        for (const QString &b : notTravelling) { ins.addBindValue(QStringLiteral("notTravelling")); ins.addBindValue(b); ins.exec(); }
        m_flagsSig = fh; ++written;
    }
    m_db.commit();
    m_batch = false;
    if (written) markDirty();
}

// One record. Observations are written incrementally (INSERT new ones and remember their row id,
// UPDATE the ones changed in memory) so their sequence numbers stay stable for sync; sightings
// and cells are small and rewritten.
void MapDb::saveRecord(const QString &bssid, ApRecord &r, int flags)
{
    if (!m_db.isOpen() || m_readOnly) return;
    const bool ownTx = !m_db.driver()->hasFeature(QSqlDriver::Transactions) ? false : m_db.transaction();
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    const int f = r.freq;
    QString last = now;
    for (const ApObservation &o : r.obs) if (o.time.isValid() && o.time.toString(Qt::ISODate) > last) last = o.time.toString(Qt::ISODate);
    QSqlQuery q(m_db);
    QSqlQuery cur(m_db); cur.prepare(QStringLiteral("SELECT ssid FROM aps WHERE bssid=?")); cur.addBindValue(bssid); cur.exec();
    const bool isNew = !cur.next();
    const bool ssidChanged = !isNew && !r.ssid.isEmpty() && cur.value(0).toString() != r.ssid;
    q.prepare(QStringLiteral("INSERT INTO aps(bssid, ssid, band, ch, freq, first_seen, last_seen, times_seen, wigle, wlat, wlon, wigle_checked, seq) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"
                             " ON CONFLICT(bssid) DO UPDATE SET ssid=CASE WHEN excluded.ssid<>'' THEN excluded.ssid ELSE aps.ssid END, band=excluded.band, ch=excluded.ch,"
                             " freq=CASE WHEN excluded.freq>0 THEN excluded.freq ELSE aps.freq END, last_seen=excluded.last_seen, times_seen=excluded.times_seen,"
                             " wigle=excluded.wigle, wlat=excluded.wlat, wlon=excluded.wlon, wigle_checked=excluded.wigle_checked, seq=CASE WHEN excluded.seq>0 THEN excluded.seq ELSE aps.seq END"));
    q.addBindValue(bssid); q.addBindValue(r.ssid);
    q.addBindValue(f >= 5925 ? QStringLiteral("6") : f >= 4900 ? QStringLiteral("5") : f > 0 ? QStringLiteral("2.4") : QString());
    q.addBindValue(f >= 5925 ? (f - 5950) / 5 : f >= 4900 ? (f - 5000) / 5 : f == 2484 ? 14 : f > 2400 ? (f - 2407) / 5 : 0);
    q.addBindValue(f); q.addBindValue(now); q.addBindValue(now); q.addBindValue(r.obs.size() + r.seen.size());
    q.addBindValue(r.wigle ? 1 : 0); q.addBindValue(r.wigle ? QVariant(r.wLat) : QVariant()); q.addBindValue(r.wigle ? QVariant(r.wLon) : QVariant());
    q.addBindValue(r.wigleChecked.isValid() ? QVariant(r.wigleChecked.toString(Qt::ISODate)) : QVariant());
    q.addBindValue(isNew || ssidChanged ? double(nextSeq()) : 0.0);
    q.exec();
    // Observations: new rows get an id + seq; rows changed in memory get a fresh seq
    QSqlQuery ins(m_db), upd(m_db);
    ins.prepare(QStringLiteral("INSERT OR IGNORE INTO observations(bssid, time, lat, lon, acc, dbm, fix_source, device, seq) VALUES(?,?,?,?,?,?,?,?,?)"));
    upd.prepare(QStringLiteral("UPDATE observations SET time=?, lat=?, lon=?, acc=?, dbm=?, seq=? WHERE id=?"));
    for (ApObservation &o : r.obs) {
        if (o.id > 0 && !o.dirty) continue;
        if (o.id > 0) {
            upd.addBindValue(o.time.toString(Qt::ISODate)); upd.addBindValue(o.lat); upd.addBindValue(o.lon); upd.addBindValue(o.acc); upd.addBindValue(o.dbm); upd.addBindValue(double(nextSeq())); upd.addBindValue(double(o.id));
            upd.exec(); o.dirty = false; continue;
        }
        ins.addBindValue(bssid); ins.addBindValue(o.time.toString(Qt::ISODate)); ins.addBindValue(o.lat); ins.addBindValue(o.lon); ins.addBindValue(o.acc); ins.addBindValue(o.dbm);
        ins.addBindValue(o.device.isEmpty() ? QStringLiteral("wifi") : QStringLiteral("remote")); ins.addBindValue(o.device); ins.addBindValue(double(nextSeq()));
        if (ins.exec() && ins.numRowsAffected() > 0) o.id = ins.lastInsertId().toLongLong();
        else {                                                     // duplicate (bssid, time, device): adopt the existing row
            QSqlQuery d(m_db); d.prepare(QStringLiteral("SELECT id FROM observations WHERE bssid=? AND time=? AND device=?"));
            d.addBindValue(bssid); d.addBindValue(o.time.toString(Qt::ISODate)); d.addBindValue(o.device); d.exec();
            if (d.next()) o.id = d.value(0).toLongLong();
        }
        o.dirty = false;
    }
    QSqlQuery del(m_db);
    for (const char *t : {"sightings", "cells"}) { del.prepare(QStringLiteral("DELETE FROM %1 WHERE bssid=?").arg(QLatin1String(t))); del.addBindValue(bssid); del.exec(); }
    ins.prepare(QStringLiteral("INSERT INTO sightings(bssid, lat, lon, acc, time) VALUES(?,?,?,?,?)"));
    for (const ApSighting &sg : r.seen) { ins.addBindValue(bssid); ins.addBindValue(sg.lat); ins.addBindValue(sg.lon); ins.addBindValue(sg.acc); ins.addBindValue(sg.time.toString(Qt::ISODate)); ins.exec(); }
    ins.prepare(QStringLiteral("INSERT OR IGNORE INTO cells(bssid, cell) VALUES(?,?)"));
    for (const QString &c : r.cells) { ins.addBindValue(bssid); ins.addBindValue(c); ins.exec(); }
    if (r.hasPeer()) {
        QSqlQuery pq(m_db); pq.prepare(QStringLiteral("UPDATE aps SET peer_lat=?, peer_lon=?, peer_acc=?, peer_from=? WHERE bssid=?"));
        pq.addBindValue(r.peerLat); pq.addBindValue(r.peerLon); pq.addBindValue(r.peerAcc); pq.addBindValue(r.peerFrom); pq.addBindValue(bssid); pq.exec();
    }
    updatePosition(bssid, r, flags);
    if (!r.security.isEmpty()) {
        QSqlQuery sec(m_db);
        sec.prepare(QStringLiteral("UPDATE aps SET security=?, sec_flags=?, wpa_flags=?, rsn_flags=?, max_kbps=?, adhoc=? WHERE bssid=?"));
        sec.addBindValue(r.security); sec.addBindValue(r.secFlags); sec.addBindValue(r.wpaFlags); sec.addBindValue(r.rsnFlags); sec.addBindValue(r.maxKbps); sec.addBindValue(r.adhoc ? 1 : 0); sec.addBindValue(bssid);
        sec.exec();
    }
    if (ownTx) m_db.commit();
    m_recSig.insert(bssid, recordSignature(r, flags));
    markDirty();
}

void MapDb::saveEstimate(const QString &bssid, const Estimator::Fit &fit)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO estimates(bssid, lat, lon, acc, semi_major, semi_minor, orient, rms, p0, pathloss, fitted_n, n, vantage, rejected, quality, updated, seq,"
                             " kind, grade, score, r95, cep50, p_within25, cxx, cxy, cyy, metrics, version) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(bssid); q.addBindValue(fit.lat); q.addBindValue(fit.lon); q.addBindValue(fit.acc); q.addBindValue(fit.semiMajor); q.addBindValue(fit.semiMinor); q.addBindValue(fit.orientDeg);
    q.addBindValue(fit.rms); q.addBindValue(fit.p0); q.addBindValue(fit.pathloss); q.addBindValue(fit.fittedN ? 1 : 0); q.addBindValue(fit.n); q.addBindValue(fit.vantage); q.addBindValue(fit.rejected);
    q.addBindValue(fit.valid ? fit.quality : QStringLiteral("none"));
    q.addBindValue(QDateTime::fromSecsSinceEpoch(fit.updated > 0 ? fit.updated : QDateTime::currentSecsSinceEpoch()).toString(Qt::ISODate));
    q.addBindValue(double(nextSeq()));
    q.addBindValue(fit.kind); q.addBindValue(fit.grade); q.addBindValue(fit.score); q.addBindValue(fit.r95); q.addBindValue(fit.cep50); q.addBindValue(fit.pWithin25);
    q.addBindValue(fit.cxx); q.addBindValue(fit.cxy); q.addBindValue(fit.cyy);
    q.addBindValue(QString::fromUtf8(QJsonDocument(Estimator::toStorage(fit)).toJson(QJsonDocument::Compact)));
    q.addBindValue(Estimator::kVersion);
    q.exec();
    markDirty();
}

void MapDb::appendEstimateHistory(const QString &bssid, const Estimator::Fit &fit)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO estimate_history(bssid, time, lat, lon, cxx, cxy, cyy, r95, score, grade, kind) VALUES(?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(bssid); q.addBindValue(QDateTime::fromSecsSinceEpoch(fit.updated > 0 ? fit.updated : QDateTime::currentSecsSinceEpoch()).toString(Qt::ISODate));
    q.addBindValue(fit.lat); q.addBindValue(fit.lon); q.addBindValue(fit.cxx); q.addBindValue(fit.cxy); q.addBindValue(fit.cyy);
    q.addBindValue(fit.r95); q.addBindValue(fit.score); q.addBindValue(fit.grade); q.addBindValue(fit.kind);
    q.exec();
    // the last 20 per beacon are plenty for drift and trends
    q.prepare(QStringLiteral("DELETE FROM estimate_history WHERE bssid=? AND id NOT IN (SELECT id FROM estimate_history WHERE bssid=? ORDER BY id DESC LIMIT 20)"));
    q.addBindValue(bssid); q.addBindValue(bssid); q.exec();
    markDirty();
}

QJsonArray MapDb::estimateHistory(const QString &bssid) const
{
    QJsonArray out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT time, lat, lon, cxx, cxy, cyy, r95, score, grade, kind FROM estimate_history WHERE bssid=? ORDER BY id"));
    q.addBindValue(bssid); q.exec();
    while (q.next()) out.append(QJsonObject{{"time", q.value(0).toString()}, {"lat", q.value(1).toDouble()}, {"lon", q.value(2).toDouble()}, {"cxx", q.value(3).toDouble()},
                                            {"cxy", q.value(4).toDouble()}, {"cyy", q.value(5).toDouble()}, {"r95", q.value(6).toDouble()}, {"score", q.value(7).toDouble()},
                                            {"grade", q.value(8).toString()}, {"kind", q.value(9).toString()}});
    return out;
}

QList<MapDb::ScanCellRow> MapDb::loadScanCells() const
{
    QList<ScanCellRow> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT cell, lat, lon, count, first, last FROM scan_cells"));
    while (q.next()) out << ScanCellRow{q.value(0).toString(), q.value(1).toDouble(), q.value(2).toDouble(), q.value(3).toInt(), q.value(4).toLongLong(), q.value(5).toLongLong()};
    return out;
}

void MapDb::saveScanCell(const ScanCellRow &c)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO scan_cells(cell, lat, lon, count, first, last) VALUES(?,?,?,?,?,?)"));
    q.addBindValue(c.key); q.addBindValue(c.lat); q.addBindValue(c.lon); q.addBindValue(c.count); q.addBindValue(double(c.first)); q.addBindValue(double(c.last));
    q.exec();
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
static const char *FIX_INSERT = "INSERT INTO fixes(time, lat, lon, acc, source, provider, place, city, region, country, elev, ap_count, ap_used, departed, seq, device) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

QList<Fix> MapDb::loadFixes() const
{
    QList<Fix> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT time, lat, lon, acc, source, provider, place, city, region, country, elev, ap_count, ap_used, departed FROM fixes WHERE device='' OR device IS NULL ORDER BY id"));
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
    q.exec(QStringLiteral("DELETE FROM fixes WHERE device='' OR device IS NULL"));   // peers' fixes are kept
    q.prepare(QString::fromLatin1(FIX_INSERT));
    for (const Fix &f : fixes) { bindFix(q, f); q.addBindValue(double(nextSeq())); q.addBindValue(QString()); q.exec(); }
    m_db.commit();
    routeOwnReplaced(fixes);
    markDirty();
}

void MapDb::appendFix(const Fix &f)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db);
    q.prepare(QString::fromLatin1(FIX_INSERT));
    bindFix(q, f); q.addBindValue(double(nextSeq())); q.addBindValue(QString());
    if (q.exec()) { const QString t = f.time.toString(Qt::ISODate); routeAdd(t, routeFixRow(f, t), true); }
    markDirty();
}

QJsonArray MapDb::latestFixesByDevice() const
{
    QJsonArray out;
    if (!isOpen()) return out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT f.device, f.time, f.lat, f.lon, f.acc, f.source, f.provider, f.place FROM fixes f "
                          "JOIN (SELECT device, MAX(time) AS t FROM fixes WHERE device <> '' GROUP BY device) m ON f.device = m.device AND f.time = m.t"));
    while (q.next())
        out.append(QJsonObject{{"device", q.value(0).toString()}, {"time", q.value(1).toString()}, {"lat", q.value(2).toDouble()}, {"lon", q.value(3).toDouble()},
                               {"acc", q.value(4).toDouble()}, {"source", q.value(5).toString()}, {"provider", q.value(6).toString()}, {"place", q.value(7).toString()}});
    return out;
}

int MapDb::appendPeerFixes(const QJsonArray &fixes, const QString &device)
{
    if (!m_db.isOpen() || m_readOnly || device.isEmpty()) return 0;
    int n = 0;
    m_db.transaction();
    QSqlQuery q(m_db), dup(m_db);
    dup.prepare(QStringLiteral("SELECT 1 FROM fixes WHERE device=? AND time=?"));
    q.prepare(QString::fromLatin1(FIX_INSERT));
    for (const QJsonValue &v : fixes) {
        const QJsonObject o = v.toObject();
        if (!o["lat"].isDouble() || !o["lon"].isDouble() || o["time"].toString().isEmpty()) continue;
        const QString dev = o["device"].toString().isEmpty() ? device : o["device"].toString();   // a hub's feed: each row keeps its own device
        dup.addBindValue(dev); dup.addBindValue(localIso(o["time"].toString())); dup.exec();
        if (dup.next()) continue;
        Fix f = Fix::fromJson(o); f.valid = true;
        f.time = f.time.toLocalTime();
        if (f.accuracy < 0) f.accuracy = o["acc"].toDouble(-1);
        bindFix(q, f); q.addBindValue(double(nextSeq())); q.addBindValue(dev);
        if (q.exec()) { ++n; if (m_routeLoaded) { const QString t = f.time.toString(Qt::ISODate); routeAdd(t, routeFixRow(f, t), false); } }
    }
    m_db.commit();
    if (n && !m_routeLoaded) { ++m_routeGen; ++m_routeReset; }
    if (n) markDirty();
    return n;
}

QList<Fix> MapDb::peerFixes(const QString &device) const
{
    QList<Fix> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT time, lat, lon, acc, source, provider, place, city, region, country, elev, ap_count, ap_used, departed, device FROM fixes WHERE device<>'' %1 ORDER BY id").arg(device.isEmpty() ? QString() : QStringLiteral("AND device=?")));
    if (!device.isEmpty()) q.addBindValue(device);
    q.exec();
    while (q.next()) {
        Fix f; f.valid = true;
        f.time = QDateTime::fromString(q.value(0).toString(), Qt::ISODate); f.lat = q.value(1).toDouble(); f.lon = q.value(2).toDouble(); f.accuracy = q.value(3).toDouble();
        f.source = q.value(4).toString(); f.provider = q.value(5).toString(); f.place = q.value(6).toString(); f.city = q.value(7).toString(); f.region = q.value(8).toString(); f.country = q.value(9).toString();
        f.elevation = q.value(10).isNull() ? -9999 : q.value(10).toDouble(); f.apCount = q.value(11).toInt(); f.apUsed = q.value(12).toInt();
        out << f;
    }
    return out;
}

// ── Places, elevation, milestones ─────────────────────────────────────────────
bool MapDb::loadPois(QList<Poi> *pois, double *lat, double *lon, int *radiusM, QDateTime *time, const QString &scope) const
{
    if (!m_db.isOpen()) return false;
    const bool far = scope == QLatin1String("far");
    const QString pre = far ? QStringLiteral("peds_") : QStringLiteral("poi_");
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT key, value FROM kv WHERE key IN (?,?,?,?)"));
    for (const char *k : {"lat", "lon", "radius", "time"}) q.addBindValue(pre + QLatin1String(k));
    q.exec();
    bool any = false;
    while (q.next()) {
        const QString k = q.value(0).toString().mid(pre.size()); any = true;
        if (k == QLatin1String("lat")) *lat = q.value(1).toDouble(); else if (k == QLatin1String("lon")) *lon = q.value(1).toDouble();
        else if (k == QLatin1String("radius")) *radiusM = q.value(1).toInt(); else *time = QDateTime::fromString(q.value(1).toString(), Qt::ISODate);
    }
    if (!any) return false;
    q.prepare(QStringLiteral("SELECT osm_type, osm_id, cat, name, detail, lat, lon, wifi, hours, phone, website, address, wheelchair, emergency, peds, er, campus, drive_s, drive_m"
                             " FROM %1 WHERE scope=?").arg(far ? QStringLiteral("pois_far") : QStringLiteral("pois")));
    q.addBindValue(far ? QStringLiteral("far") : QStringLiteral("near"));
    q.exec();
    while (q.next()) {
        Poi p; p.osmType = q.value(0).toString(); p.osmId = q.value(1).toLongLong(); p.cat = q.value(2).toString(); p.name = q.value(3).toString(); p.detail = q.value(4).toString();
        p.lat = q.value(5).toDouble(); p.lon = q.value(6).toDouble(); p.wifi = q.value(7).toInt() != 0; p.hours = q.value(8).toString(); p.phone = q.value(9).toString(); p.website = q.value(10).toString();
        p.address = q.value(11).toString(); p.wheelchair = q.value(12).toString(); p.emergency = q.value(13).toInt() != 0;
        p.peds = q.value(14).toInt(); p.er = q.value(15).toString(); p.campus = q.value(16).toString(); p.driveS = q.value(17).toInt(); p.driveM = q.value(18).toInt();
        p.driveEst = p.driveS <= 0;
        p.scope = far ? QStringLiteral("far") : QStringLiteral("near");
        pois->append(p);
    }
    return true;
}

void MapDb::savePois(const QList<Poi> &pois, double lat, double lon, int radiusM, const QDateTime &time, const QString &scope)
{
    if (!m_db.isOpen() || m_readOnly) return;
    const bool far = scope == QLatin1String("far");
    const QString sc = far ? QStringLiteral("far") : QStringLiteral("near"), pre = far ? QStringLiteral("peds_") : QStringLiteral("poi_");
    m_db.transaction();
    QSqlQuery q(m_db);
    // Each search has its own table (pois = near, pois_far = the pediatric search), so saving one never touches the
    // other: a place both found is stored twice and merged in memory (Locator::rebuildMergedPois, near wins)
    const QString table = far ? QStringLiteral("pois_far") : QStringLiteral("pois");
    q.prepare(QStringLiteral("DELETE FROM %1 WHERE scope=?").arg(table)); q.addBindValue(sc); q.exec();
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO %1(osm_type, osm_id, cat, name, detail, lat, lon, wifi, hours, phone, website, address, wheelchair, emergency,"
                             " scope, peds, er, campus, drive_s, drive_m) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)").arg(table));
    for (const Poi &p : pois) {
        q.addBindValue(p.osmType); q.addBindValue(p.osmId); q.addBindValue(p.cat); q.addBindValue(p.name); q.addBindValue(p.detail); q.addBindValue(p.lat); q.addBindValue(p.lon);
        q.addBindValue(p.wifi ? 1 : 0); q.addBindValue(p.hours); q.addBindValue(p.phone); q.addBindValue(p.website); q.addBindValue(p.address); q.addBindValue(p.wheelchair); q.addBindValue(p.emergency ? 1 : 0);
        q.addBindValue(sc); q.addBindValue(p.peds); q.addBindValue(p.er); q.addBindValue(p.campus); q.addBindValue(p.driveEst ? 0 : p.driveS); q.addBindValue(p.driveEst ? 0 : p.driveM); q.exec();
    }
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO kv(key, value) VALUES(?,?)"));
    const QList<QPair<QString, QString>> kv{{pre + QStringLiteral("lat"), QString::number(lat, 'f', 7)}, {pre + QStringLiteral("lon"), QString::number(lon, 'f', 7)},
                                            {pre + QStringLiteral("radius"), QString::number(radiusM)}, {pre + QStringLiteral("time"), time.toString(Qt::ISODate)}};
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
    QSet<QString> pinnedSeen;
    while (q.next()) {
        ApPos p; p.bssid = q.value(0).toString(); p.ssid = q.value(1).toString(); p.lat = q.value(2).toDouble(); p.lon = q.value(3).toDouble(); p.acc = q.value(4).toDouble();
        p.source = q.value(5).toString(); p.home = q.value(6).toInt(); p.travelling = q.value(7).toInt(); p.ignored = q.value(8).toInt();
        const auto pin = m_pins.constFind(p.bssid.toUpper());
        if (pin != m_pins.constEnd()) { p.lat = pin->lat; p.lon = pin->lon; p.acc = pin->acc; p.source = pin->source; p.travelling = 0; pinnedSeen.insert(p.bssid.toUpper()); }
        out << p;
    }
    // Anchored transmitters are known even before we ever stored a position for them (docs/RANGING.md §4.3.2)
    for (auto it = m_pins.constBegin(); it != m_pins.constEnd(); ++it) {
        if (pinnedSeen.contains(it.key())) continue;
        if (!bssids.isEmpty() && !bssids.contains(it.key(), Qt::CaseInsensitive)) continue;
        out << it.value();
    }
    return out;
}

QJsonArray MapDb::positionsPage(const QString &after, int limit, QString *next) const
{
    QJsonArray out;
    if (next) next->clear();
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT bssid, ssid, freq, lat, lon, acc, source, home, travelling, ignored, security, seq FROM aps WHERE bssid > ? ORDER BY bssid LIMIT ?"));
    q.addBindValue(after.isNull() ? QStringLiteral("") : after); q.addBindValue(limit + 1);   // NULL would match nothing
    if (!q.exec()) return out;
    while (q.next()) {
        if (out.size() >= limit) { if (next) *next = out.last().toObject()["bssid"].toString(); break; }
        QJsonObject a{{"bssid", q.value(0).toString()}, {"ssid", q.value(1).toString()}, {"freq", q.value(2).toInt()}, {"source", q.value(6).toString()},
                      {"home", q.value(7).toInt() != 0}, {"travelling", q.value(8).toInt() != 0}, {"ignored", q.value(9).toInt() != 0}, {"security", q.value(10).toString()}, {"seq", q.value(11).toDouble()}};
        const auto pin = m_pins.constFind(q.value(0).toString().toUpper());
        if (pin != m_pins.constEnd()) { a["lat"] = pin->lat; a["lon"] = pin->lon; a["acc"] = pin->acc; a["source"] = QStringLiteral("anchor"); }
        else if (!q.value(3).isNull()) { a["lat"] = q.value(3).toDouble(); a["lon"] = q.value(4).toDouble(); a["acc"] = q.value(5).toDouble(); }
        out.append(a);
    }
    return out;
}

bool MapDb::estimate(const QList<QPair<QString, int>> &heard, double *lat, double *lon, double *acc, int *used, QStringList *usedBssids, int minAps, double maxAcc) const
{
    // Weighted least squares on ranges (see estimator.h): each known beacon says "you are d metres
    // from me" with d from ITS fitted P0/n when we have a fit, the default model otherwise.
    QStringList ids; QHash<QString, int> dbm;
    for (const auto &h : heard) { ids << h.first.toUpper(); dbm.insert(h.first.toUpper(), h.second); }
    QList<ApPos> ps = positions(ids, maxAcc);
    QList<Estimator::Known> known;
    QSqlQuery q(m_db);
    for (const ApPos &p : ps) {
        if (p.home || p.travelling || p.ignored) continue;
        Estimator::Known k; k.bssid = p.bssid; k.lat = p.lat; k.lon = p.lon; k.acc = p.acc; k.dbm = dbm.value(p.bssid.toUpper(), -80);
        q.prepare(QStringLiteral("SELECT p0, pathloss, quality, kind, grade, cxx, cxy, cyy FROM estimates WHERE bssid=?")); q.addBindValue(p.bssid); q.exec();
        if (q.next() && q.value(2).toString() != QLatin1String("none") && q.value(3).toString() != QLatin1String("region") && q.value(3).toString() != QLatin1String("mobile")) {
            k.p0 = q.value(0).toDouble(); k.pathloss = q.value(1).toDouble(); k.haveModel = true;
            if (p.source == QLatin1String("trilat")) { k.cxx = q.value(5).toDouble(); k.cxy = q.value(6).toDouble(); k.cyy = q.value(7).toDouble(); }
            // a beacon we placed badly counts less (docs/GRADING.md §4.6)
            const QString g = q.value(4).toString();
            k.weight = g == QLatin1String("A") || g == QLatin1String("B") ? 1.0 : g == QLatin1String("C") ? 0.8 : g == QLatin1String("D") ? 0.6 : g == QLatin1String("E") ? 0.4 : g == QLatin1String("F") ? 0.3 : 1.0;
        }
        known << k;
    }
    *used = known.size();
    if (known.size() < minAps) return false;
    const Estimator::SelfFix sf = Estimator::selfLocate(known);
    if (!sf.valid) return false;
    *lat = sf.lat; *lon = sf.lon; *acc = sf.acc; *used = sf.used;
    if (usedBssids) for (const Estimator::Known &k : known) usedBssids->append(k.bssid);
    return true;
}

int MapDb::addObservations(const QJsonArray &observations, const QString &device, QString *error, QHash<QString, QList<ApObservation>> *added)
{
    if (!m_db.isOpen() || m_readOnly) { if (error) *error = QStringLiteral("database not writable"); return -1; }
    int n = 0;
    m_db.transaction();
    QSqlQuery ap(m_db), q(m_db), fixQ(m_db);                    // prepared once: an import pushes hundreds of thousands of rows through here
    ap.prepare(QStringLiteral("INSERT INTO aps(bssid, ssid, freq, first_seen, last_seen, times_seen, seq) VALUES(?,?,?,?,?,1,?) ON CONFLICT(bssid) DO UPDATE SET last_seen=excluded.last_seen, times_seen=aps.times_seen+1,"
                              " ssid=CASE WHEN excluded.ssid<>'' THEN excluded.ssid ELSE aps.ssid END, freq=CASE WHEN excluded.freq>0 THEN excluded.freq ELSE aps.freq END"));
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO observations(bssid, time, lat, lon, acc, dbm, fix_source, device, seq, range_m, range_sd) VALUES(?,?,?,?,?,?,?,?,?,?,?)"));
    fixQ.prepare(QStringLiteral("INSERT OR IGNORE INTO fixes(time, lat, lon, acc, source, provider, device, seq) VALUES(?,?,?,?,?,?,?,?)"));
    QSet<QPair<QString, QString>> fixSeen;
    for (const QJsonValue &v : observations) {
        const QJsonObject o = v.toObject();
        const QString bssid = o["bssid"].toString().toUpper().trimmed();
        if (bssid.size() != 17 || !o["lat"].isDouble() || !o["lon"].isDouble()) continue;
        const double acc = o["acc"].toDouble(100);
        if (acc <= 0 || acc > 2000) continue;
        const QString dev = o["device"].toString().isEmpty() ? device : o["device"].toString();
        const QString t = o["time"].toString().isEmpty() ? QDateTime::currentDateTime().toString(Qt::ISODate) : localIso(o["time"].toString());
        ap.addBindValue(bssid); ap.addBindValue(o["ssid"].toString()); ap.addBindValue(o["freq"].toInt(0)); ap.addBindValue(t); ap.addBindValue(t); ap.addBindValue(double(nextSeq())); ap.exec();
        q.addBindValue(bssid); q.addBindValue(t); q.addBindValue(o["lat"].toDouble()); q.addBindValue(o["lon"].toDouble()); q.addBindValue(acc);
        q.addBindValue(o["dbm"].toInt(-80)); q.addBindValue(o["source"].toString().isEmpty() ? QStringLiteral("remote") : o["source"].toString()); q.addBindValue(dev); q.addBindValue(double(nextSeq()));
        // Wi-Fi RTT: the phone ranged the AP (802.11mc / az) when it answered; a plausible distance only
        const bool ranged = o["rangeM"].isDouble() && o["rangeM"].toDouble() >= 0 && o["rangeM"].toDouble() < 500;
        q.addBindValue(ranged ? QVariant(o["rangeM"].toDouble()) : QVariant());
        q.addBindValue(ranged ? QVariant(qBound(0.1, o["rangeSd"].toDouble(1.5), 50.0)) : QVariant());
        if (!q.exec() || q.numRowsAffected() <= 0) continue;   // duplicate (bssid, time, device)

        const auto fixKey = qMakePair(t, dev);
        if (!fixSeen.contains(fixKey)) {
            fixSeen.insert(fixKey);
            fixQ.addBindValue(t);
            fixQ.addBindValue(o["lat"].toDouble());
            fixQ.addBindValue(o["lon"].toDouble());
            fixQ.addBindValue(acc);
            fixQ.addBindValue(o["source"].toString().isEmpty() ? QStringLiteral("phone-gps") : o["source"].toString());
            fixQ.addBindValue(QStringLiteral("fused"));
            fixQ.addBindValue(dev);
            fixQ.addBindValue(double(nextSeq()));
            if (fixQ.exec() && m_routeLoaded) {
                Fix rf; rf.valid = true; rf.time = QDateTime::fromString(t, Qt::ISODate); rf.lat = o["lat"].toDouble(); rf.lon = o["lon"].toDouble(); rf.accuracy = acc;
                rf.source = o["source"].toString().isEmpty() ? QStringLiteral("phone-gps") : o["source"].toString(); rf.provider = QStringLiteral("fused");
                routeAdd(t, rf, false);
            }
        }

        ApObservation ob; ob.id = q.lastInsertId().toLongLong(); ob.time = QDateTime::fromString(t, Qt::ISODate); ob.lat = o["lat"].toDouble(); ob.lon = o["lon"].toDouble(); ob.acc = acc; ob.dbm = o["dbm"].toInt(-80); ob.device = dev;
        if (ranged) { ob.rangeM = o["rangeM"].toDouble(); ob.rangeSd = qBound(0.1, o["rangeSd"].toDouble(1.5), 50.0); }
        if (added) (*added)[bssid].append(ob);
        ++n;
    }
    m_db.commit();
    if (n && !m_routeLoaded) { ++m_routeGen; ++m_routeReset; }
    if (n) markDirty();
    return n;
}

// Positions another device worked out (its own fits). They fill the "peer" slot; our own fit,
// once we have one, takes precedence in positionFrom(). A better (tighter) peer estimate replaces an older one.
int MapDb::mergePeerAps(const QJsonArray &aps, const QString &device, QStringList *touched)
{
    if (!m_db.isOpen() || m_readOnly || device.isEmpty()) return 0;
    int n = 0;
    m_db.transaction();
    QSqlQuery q(m_db), cur(m_db);
    for (const QJsonValue &v : aps) {
        const QJsonObject o = v.toObject();
        const QString bssid = o["bssid"].toString().toUpper().trimmed();
        if (bssid.size() != 17 || !o["lat"].isDouble() || !o["lon"].isDouble()) continue;
        const double acc = o["acc"].toDouble(100);
        if (acc <= 0 || acc > 2000) continue;
        const QString t = QDateTime::currentDateTime().toString(Qt::ISODate);
        q.prepare(QStringLiteral("INSERT INTO aps(bssid, ssid, freq, first_seen, last_seen, times_seen, seq) VALUES(?,?,?,?,?,0,?) ON CONFLICT(bssid) DO UPDATE SET"
                                 " ssid=CASE WHEN excluded.ssid<>'' AND (aps.ssid IS NULL OR aps.ssid='') THEN excluded.ssid ELSE aps.ssid END, freq=CASE WHEN excluded.freq>0 AND (aps.freq IS NULL OR aps.freq=0) THEN excluded.freq ELSE aps.freq END"));
        q.addBindValue(bssid); q.addBindValue(o["ssid"].toString()); q.addBindValue(o["freq"].toInt(0)); q.addBindValue(t); q.addBindValue(t); q.addBindValue(double(nextSeq())); q.exec();
        cur.prepare(QStringLiteral("SELECT peer_acc, peer_from, lat, acc FROM aps WHERE bssid=?")); cur.addBindValue(bssid); cur.exec();
        if (!cur.next()) continue;
        const bool hadPeer = !cur.value(1).toString().isEmpty() && cur.value(0).toDouble() > 0;
        if (hadPeer && cur.value(1).toString() != device && cur.value(0).toDouble() <= acc) continue;   // someone else's tighter estimate stays
        q.prepare(QStringLiteral("UPDATE aps SET peer_lat=?, peer_lon=?, peer_acc=?, peer_from=? WHERE bssid=?"));
        q.addBindValue(o["lat"].toDouble()); q.addBindValue(o["lon"].toDouble()); q.addBindValue(acc); q.addBindValue(device); q.addBindValue(bssid); q.exec();
        if (cur.value(2).isNull() || cur.value(3).toDouble() > acc) {                 // no position of our own (or a worse one): use it
            q.prepare(QStringLiteral("UPDATE aps SET lat=?, lon=?, acc=?, source='peer', seq=? WHERE bssid=? AND (lat IS NULL OR source='peer' OR source='observed' OR acc>?)"));
            q.addBindValue(o["lat"].toDouble()); q.addBindValue(o["lon"].toDouble()); q.addBindValue(acc); q.addBindValue(double(nextSeq())); q.addBindValue(bssid); q.addBindValue(acc); q.exec();
        }
        if (touched) touched->append(bssid);
        ++n;
    }
    m_db.commit();
    if (n) markDirty();
    return n;
}

// Everything with a change sequence after `since`, oldest first, capped — the sync feed
QJsonObject MapDb::changesSince(qint64 since, int limit, bool *more, qint64 *cursor) const
{
    QJsonObject out; QJsonArray aps, obs, fixes;
    if (more) *more = false;
    if (cursor) *cursor = m_seq;
    if (!m_db.isOpen()) return out;
    limit = qBound(1, limit, 5000);
    qint64 maxSeq = since; int total = 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT bssid, ssid, freq, lat, lon, acc, source, home, travelling, ignored, security, seq FROM aps WHERE seq>? ORDER BY seq LIMIT ?")); q.addBindValue(double(since)); q.addBindValue(limit + 1); q.exec();
    while (q.next()) {
        if (aps.size() >= limit) { if (more) *more = true; break; }
        QJsonObject a{{"bssid", q.value(0).toString()}, {"ssid", q.value(1).toString()}, {"freq", q.value(2).toInt()}, {"source", q.value(6).toString()},
                      {"home", q.value(7).toInt() != 0}, {"travelling", q.value(8).toInt() != 0}, {"ignored", q.value(9).toInt() != 0}, {"security", q.value(10).toString()}, {"seq", q.value(11).toDouble()}};
        if (!q.value(3).isNull()) { a["lat"] = q.value(3).toDouble(); a["lon"] = q.value(4).toDouble(); a["acc"] = q.value(5).toDouble(); }
        QSqlQuery e(m_db); e.prepare(QStringLiteral("SELECT n, vantage, rms, acc, p0, pathloss, quality, updated, metrics FROM estimates WHERE bssid=? AND quality<>'none'")); e.addBindValue(q.value(0).toString()); e.exec();
        if (e.next()) {
            QJsonObject fit{{"n", e.value(0).toInt()}, {"vantage", e.value(1).toInt()}, {"rms", e.value(2).toDouble()}, {"acc", e.value(3).toDouble()}, {"p0", e.value(4).toDouble()}, {"pathloss", e.value(5).toDouble()}, {"quality", e.value(6).toString()}, {"updated", e.value(7).toString()}};
            const QJsonObject m = QJsonDocument::fromJson(e.value(8).toString().toUtf8()).object();
            if (!m.isEmpty()) {         // 3.9: the graded estimate (older peers ignore the extra keys)
                const QJsonObject api = Estimator::toApi(Estimator::fromStorage(m));
                for (const char *k : {"kind", "grade", "score", "r95", "cep50", "pWithin25", "cxx", "cxy", "cyy", "semiMajor", "semiMinor", "orient", "lat", "lon",
                                      "devices", "sessions", "inHull", "ambiguous", "modes", "moved", "flags"})
                    fit[QLatin1String(k)] = api.value(QLatin1String(k));
            }
            a["fit"] = fit;
        }
        aps.append(a); maxSeq = qMax(maxSeq, q.value(11).toLongLong()); ++total;
    }
    q.prepare(QStringLiteral("SELECT bssid, time, lat, lon, acc, dbm, device, seq, range_m, range_sd FROM observations WHERE seq>? ORDER BY seq LIMIT ?")); q.addBindValue(double(since)); q.addBindValue(limit + 1); q.exec();
    while (q.next()) {
        if (obs.size() >= limit) { if (more) *more = true; break; }
        QJsonObject ob{{"bssid", q.value(0).toString()}, {"time", q.value(1).toString()}, {"lat", q.value(2).toDouble()}, {"lon", q.value(3).toDouble()}, {"acc", q.value(4).toDouble()},
                       {"dbm", q.value(5).toInt()}, {"device", q.value(6).toString()}, {"seq", q.value(7).toDouble()}};
        if (!q.value(8).isNull()) { ob["rangeM"] = q.value(8).toDouble(); ob["rangeSd"] = q.value(9).toDouble(); }
        obs.append(ob);
        maxSeq = qMax(maxSeq, q.value(7).toLongLong()); ++total;
    }
    q.prepare(QStringLiteral("SELECT time, lat, lon, acc, source, provider, place, city, region, country, elev, device, seq FROM fixes WHERE seq>? ORDER BY seq LIMIT ?")); q.addBindValue(double(since)); q.addBindValue(limit + 1); q.exec();
    while (q.next()) {
        if (fixes.size() >= limit) { if (more) *more = true; break; }
        QJsonObject f{{"time", q.value(0).toString()}, {"lat", q.value(1).toDouble()}, {"lon", q.value(2).toDouble()}, {"acc", q.value(3).toDouble()}, {"source", q.value(4).toString()}, {"provider", q.value(5).toString()},
                      {"place", q.value(6).toString()}, {"city", q.value(7).toString()}, {"region", q.value(8).toString()}, {"country", q.value(9).toString()}, {"device", q.value(11).toString()}, {"seq", q.value(12).toDouble()}};
        if (!q.value(10).isNull()) f["elev"] = q.value(10).toDouble();
        fixes.append(f); maxSeq = qMax(maxSeq, q.value(12).toLongLong()); ++total;
    }
    QJsonArray anchors;
    q.prepare(QStringLiteral("SELECT json, deleted, seq FROM anchors WHERE seq>? ORDER BY seq LIMIT ?")); q.addBindValue(double(since)); q.addBindValue(limit + 1);
    if (q.exec()) while (q.next()) {
        if (anchors.size() >= limit) { if (more) *more = true; break; }
        Anchors::Anchor a = Anchors::Anchor::fromJson(QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object());
        a.deleted = a.deleted || q.value(1).toInt() != 0; a.seq = q.value(2).toLongLong();
        anchors.append(a.toJson(true)); maxSeq = qMax(maxSeq, a.seq); ++total;
    }
    // Plate events (docs/SIGHTINGS.md §5): records only, raw included; the media stay on the node that has them
    QJsonArray plateEvents;
    q.prepare(QStringLiteral("SELECT * FROM plate_events WHERE seq>? ORDER BY seq LIMIT ?")); q.addBindValue(double(since)); q.addBindValue(limit + 1);
    if (q.exec()) while (q.next()) {
        if (plateEvents.size() >= limit) { if (more) *more = true; break; }
        const QJsonObject pe = peRow(q, true);
        plateEvents.append(pe); maxSeq = qMax(maxSeq, qint64(pe["seq"].toDouble())); ++total;
    }
    // The cursor a client should store: when a table hit the cap, the smallest "next" seq across tables keeps ordering safe
    qint64 next = maxSeq;
    if (more && *more) {
        next = m_seq;
        for (const QJsonArray *arr : {&aps, &obs, &fixes, &anchors, &plateEvents}) if (arr->size() >= limit) next = qMin(next, qint64(arr->last().toObject()["seq"].toDouble()));
        // every table is complete up to `next` only if the others have no rows in (since, next] beyond what we returned — they were read fully or capped at ≥ next
    }
    if (cursor) *cursor = (more && *more) ? next : m_seq;
    out["since"] = double(since); out["aps"] = aps; out["observations"] = obs; out["fixes"] = fixes; out["anchors"] = anchors; out["plateEvents"] = plateEvents; out["count"] = total;
    out["cursor"] = double(cursor ? *cursor : m_seq); out["more"] = more ? *more : false;
    return out;
}

// ── Anchors (docs/RANGING.md §4.2) ────────────────────────────────────────────
QList<Anchors::Anchor> MapDb::loadAnchors(bool includeDeleted) const
{
    QList<Anchors::Anchor> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT json, deleted, seq FROM anchors ORDER BY seq"))) return out;
    while (q.next()) {
        if (!includeDeleted && q.value(1).toInt() != 0) continue;
        Anchors::Anchor a = Anchors::Anchor::fromJson(QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object());
        a.seq = q.value(2).toLongLong();
        out << a;
    }
    return out;
}

static QString anchorStamp(const Anchors::Anchor &a) { return a.deleted ? a.deletedAt : a.placedAt; }

bool MapDb::putAnchor(Anchors::Anchor a, bool force, Anchors::Anchor *stored)
{
    if (!m_db.isOpen() || m_readOnly || a.id.isEmpty()) return false;
    QSqlQuery cur(m_db); cur.prepare(QStringLiteral("SELECT json, deleted FROM anchors WHERE id=?")); cur.addBindValue(a.id); cur.exec();
    if (cur.next()) {
        Anchors::Anchor old = Anchors::Anchor::fromJson(QJsonDocument::fromJson(cur.value(0).toString().toUtf8()).object());
        old.deleted = cur.value(1).toInt() != 0;
        if (!force) {
            // Sync: newest placedAt / deletedAt wins; equal stamps keep ours (idempotent re-delivery)
            const QDateTime tNew = QDateTime::fromString(anchorStamp(a), Qt::ISODateWithMs), tOld = QDateTime::fromString(anchorStamp(old), Qt::ISODateWithMs);
            if (tOld.isValid() && (!tNew.isValid() || tNew <= tOld)) { if (stored) *stored = old; return false; }
        }
        if (!a.deleted && old.toJson(false) == a.toJson(false) && !old.deleted) { if (stored) *stored = old; return false; }
    } else if (a.deleted && !force) {
        // a tombstone for something we never had: keep it so it propagates, but only once
    }
    if (a.deleted && a.deletedAt.isEmpty()) a.deletedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    a.seq = nextSeq();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO anchors(id, json, kind, lat, lon, rv, ref, deleted, updated, seq) VALUES(?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(a.id);
    q.addBindValue(QString::fromUtf8(QJsonDocument(a.toJson(false)).toJson(QJsonDocument::Compact)));
    q.addBindValue(a.deleted ? QVariant() : QVariant(a.kind));
    q.addBindValue(a.deleted ? QVariant() : QVariant(a.lat)); q.addBindValue(a.deleted ? QVariant() : QVariant(a.lon));
    q.addBindValue(a.rv ? 1 : 0); q.addBindValue(a.ref ? 1 : 0); q.addBindValue(a.deleted ? 1 : 0);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)); q.addBindValue(double(a.seq));
    if (!q.exec()) { m_error = q.lastError().text(); return false; }
    markDirty();
    if (stored) *stored = a;
    return true;
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
    o["observations"] = dumpTable(m_db, QStringLiteral("SELECT bssid, time, lat, lon, acc, dbm, fix_source, device FROM observations ORDER BY id"));
    o["estimates"] = dumpTable(m_db, QStringLiteral("SELECT * FROM estimates"));
    o["estimateHistory"] = dumpTable(m_db, QStringLiteral("SELECT bssid, time, lat, lon, cxx, cxy, cyy, r95, score, grade, kind FROM estimate_history ORDER BY id"));
    o["scanCells"] = dumpTable(m_db, QStringLiteral("SELECT * FROM scan_cells"));
    o["sightings"] = dumpTable(m_db, QStringLiteral("SELECT bssid, lat, lon, acc, time FROM sightings ORDER BY id"));
    o["cells"] = dumpTable(m_db, QStringLiteral("SELECT bssid, cell FROM cells"));
    o["flags"] = dumpTable(m_db, QStringLiteral("SELECT kind, bssid FROM flags"));
    o["fixes"] = dumpTable(m_db, QStringLiteral("SELECT time, lat, lon, acc, source, provider, place, city, region, country, elev, ap_count, ap_used, departed, device FROM fixes ORDER BY id"));
    o["pois"] = dumpTable(m_db, QStringLiteral("SELECT * FROM pois"));
    o["poisFar"] = dumpTable(m_db, QStringLiteral("SELECT * FROM pois_far"));
    o["elevation"] = dumpTable(m_db, QStringLiteral("SELECT * FROM elevation"));
    o["achievements"] = dumpTable(m_db, QStringLiteral("SELECT * FROM achievements"));
    o["kv"] = dumpTable(m_db, QStringLiteral("SELECT * FROM kv"));
    o["flockCameras"] = dumpTable(m_db, QStringLiteral("SELECT * FROM flock_cameras"));
    {   // plate events (records; the media are binary and stay in the database)
        QJsonArray pe; QSqlQuery q(m_db); q.exec(QStringLiteral("SELECT * FROM plate_events ORDER BY id"));
        while (q.next()) { QJsonObject r = peRow(q, true); r.remove(QStringLiteral("seq")); pe.append(r); }
        o["plateEvents"] = pe;
    }
    QJsonArray an; for (const Anchors::Anchor &a : loadAnchors(true)) an.append(a.toJson(false));
    o["anchors"] = an;
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
    routeInvalidate();
    insertRows(QStringLiteral("pois"), dump["pois"].toArray(), true);
    insertRows(QStringLiteral("pois_far"), dump["poisFar"].toArray(), true);
    insertRows(QStringLiteral("elevation"), dump["elevation"].toArray(), true);
    insertRows(QStringLiteral("achievements"), dump["achievements"].toArray(), true);
    insertRows(QStringLiteral("estimates"), dump["estimates"].toArray(), true);
    insertRows(QStringLiteral("estimate_history"), dump["estimateHistory"].toArray(), true);
    insertRows(QStringLiteral("scan_cells"), dump["scanCells"].toArray(), true);
    insertRows(QStringLiteral("flock_cameras"), dump["flockCameras"].toArray(), true);
    m_db.commit();
    for (const QJsonValue &v : dump["anchors"].toArray()) if (putAnchor(Anchors::Anchor::fromJson(v.toObject()), false)) ++n;
    for (const QJsonValue &v : dump["plateEvents"].toArray()) { bool created = false, changed = false; mergePlateEvent(v.toObject(), false, &created, &changed); if (changed) ++n; }
    if (n) markDirty();
    return n;
}

// Wi-Fi estimate jumps: > 38 m/s within 5 min of the previous kept fix
static bool routeTeleport(const Fix &prev, const Fix &f)
{
    if (!f.time.isValid() || !prev.time.isValid()) return false;
    const qint64 dt = std::abs(prev.time.secsTo(f.time));
    return dt > 0 && dt <= 300 && Locator::distanceM(prev.lat, prev.lon, f.lat, f.lon) / dt > 38.0;
}

// The same test on epoch milliseconds (QDateTime::secsTo converts both local times on every call)
static const qint64 kNoTime = std::numeric_limits<qint64>::min();
static qint64 routeMs(const QDateTime &t) { return t.isValid() ? t.toMSecsSinceEpoch() : kNoTime; }
static bool routeJump(qint64 pMs, double pLat, double pLon, qint64 ms, double lat, double lon)
{
    if (pMs == kNoTime || ms == kNoTime) return false;
    const qint64 dt = std::abs((ms - pMs) / 1000);
    return dt > 0 && dt <= 300 && Locator::distanceM(pLat, pLon, lat, lon) / dt > 38.0;
}

static Fix routeRow(const QSqlQuery &q)
{
    Fix f; f.valid = true;
    f.time = QDateTime::fromString(q.value(0).toString(), Qt::ISODate);
    f.lat = q.value(1).toDouble();
    f.lon = q.value(2).toDouble();
    f.accuracy = q.value(3).toDouble();
    f.source = q.value(4).toString();
    f.provider = q.value(5).toString();
    f.place = q.value(6).toString();
    f.city = q.value(7).toString();
    f.region = q.value(8).toString();
    f.country = q.value(9).toString();
    f.elevation = q.value(10).isNull() ? -9999 : q.value(10).toDouble();
    f.apCount = q.value(11).toInt();
    f.apUsed = q.value(12).toInt();
    f.departed = QDateTime::fromString(q.value(13).toString(), Qt::ISODate);
    return f;
}

static const char *ROUTE_SELECT = "SELECT time, lat, lon, acc, source, provider, place, city, region, country, elev, ap_count, ap_used, departed FROM fixes ORDER BY time ASC";

QList<Fix> MapDb::loadAllRouteFixes() const
{
    QList<Fix> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.exec(QLatin1String(ROUTE_SELECT));
    while (q.next()) {
        const Fix f = routeRow(q);
        if (f.accuracy > 500.0) continue;
        if (!out.isEmpty() && routeTeleport(out.last(), f)) continue;
        out << f;
    }
    return out;
}

// What bindFix stores, read back (whole seconds, NULL elevation = -9999): the cached row equals the scanned one
Fix MapDb::routeFixRow(const Fix &in, const QString &t)
{
    Fix f; f.valid = true;
    f.time = QDateTime::fromString(t, Qt::ISODate);
    f.lat = in.lat; f.lon = in.lon; f.accuracy = in.accuracy;
    f.source = in.source; f.provider = in.provider; f.place = in.place; f.city = in.city; f.region = in.region; f.country = in.country;
    f.elevation = in.hasElevation() ? in.elevation : -9999;
    f.apCount = in.apCount; f.apUsed = in.apUsed;
    if (in.departed.isValid()) f.departed = QDateTime::fromString(in.departed.toString(Qt::ISODate), Qt::ISODate);
    return f;
}

static bool sameRouteFix(const Fix &a, const Fix &b)
{
    return a.time == b.time && a.lat == b.lat && a.lon == b.lon && a.accuracy == b.accuracy && a.source == b.source && a.provider == b.provider
        && a.place == b.place && a.city == b.city && a.region == b.region && a.country == b.country && a.elevation == b.elevation
        && a.apCount == b.apCount && a.apUsed == b.apUsed && a.departed == b.departed;
}

// The table is scanned once; after that a write keeps the rows current: a row at or after the newest one is
// appended (filtered on the next read), anything else is merged in and the list refiltered on the next read.
// Equal times keep insertion order (the scan's ORDER BY leaves them unspecified).
QList<Fix> MapDb::routeFixes() const
{
    if (!m_routeLoaded) {
        m_routeRaw.clear(); m_route = QList<Fix>(); m_routePatch.clear();
        if (m_db.isOpen()) {
            QSqlQuery q(m_db);                                 // ROUTE_SELECT + whose row it is (saveFixes replaces this host's)
            q.setForwardOnly(true);
            q.exec(QStringLiteral("SELECT time, lat, lon, acc, source, provider, place, city, region, country, elev, ap_count, ap_used, departed,"
                                  " (device='' OR device IS NULL) FROM fixes ORDER BY time ASC"));
            while (q.next()) {
                const Fix f = routeRow(q);
                if (f.accuracy <= 500.0) m_routeRaw.append(RouteRow{q.value(0).toString(), f, q.value(14).toInt() != 0, -1, routeMs(f.time)});
            }
        }
        m_routeSorted = m_routeRaw.size(); m_routeOutAt = 0; m_routeRefilter = false; m_routeLoaded = true;
    }
    if (m_routeRefilter) {
        const auto byT = [](const RouteRow &a, const RouteRow &b) { return a.t < b.t; };
        std::stable_sort(m_routeRaw.begin() + m_routeSorted, m_routeRaw.end(), byT);
        std::inplace_merge(m_routeRaw.begin(), m_routeRaw.begin() + m_routeSorted, m_routeRaw.end(), byT);
        m_routeSorted = m_routeRaw.size(); m_routeOutAt = 0; m_routeRefilter = false;
        m_route = QList<Fix>(); m_routePatch.clear();
    }
    if (!m_routePatch.isEmpty()) {
        for (qsizetype i : std::as_const(m_routePatch))
            if (i < m_routeOutAt && m_routeRaw[i].out >= 0) m_route[m_routeRaw[i].out] = m_routeRaw[i].f;
        m_routePatch.clear();
    }
    if (m_routeOutAt < m_routeRaw.size()) {
        if (m_routeOutAt == 0) m_route.reserve(m_routeRaw.size());
        bool have = !m_route.isEmpty();
        qint64 pMs = have ? routeMs(m_route.last().time) : kNoTime;
        double pLat = have ? m_route.last().lat : 0, pLon = have ? m_route.last().lon : 0;
        for (qsizetype i = m_routeOutAt; i < m_routeRaw.size(); ++i) {
            RouteRow &r = m_routeRaw[i];
            if (have && routeJump(pMs, pLat, pLon, r.ms, r.f.lat, r.f.lon)) { r.out = -1; continue; }
            r.out = int(m_route.size()); m_route.append(r.f);
            have = true; pMs = r.ms; pLat = r.f.lat; pLon = r.f.lon;
        }
        m_routeOutAt = m_routeRaw.size();
    }
    return m_route;
}

void MapDb::routeAdd(const QString &t, const Fix &f, bool own)
{
    ++m_routeGen;
    if (!m_routeLoaded) { ++m_routeReset; return; }
    if (f.accuracy > 500.0) return;
    const bool inOrder = !m_routeRefilter && m_routeSorted == m_routeRaw.size() && (m_routeRaw.isEmpty() || !(t < m_routeRaw.last().t));
    m_routeRaw.append(RouteRow{t, f, own, -1, routeMs(f.time)});
    if (inOrder) ++m_routeSorted;
    else { m_routeRefilter = true; ++m_routeReset; }
}

void MapDb::routeInvalidate()
{
    m_routeLoaded = false; m_routeRefilter = false;
    m_routeRaw.clear(); m_route = QList<Fix>(); m_routePatch.clear();
    m_routeSorted = m_routeOutAt = 0;
    ++m_routeGen; ++m_routeReset;
}

// saveFixes rewrote this host's rows. Usually only the last stop's departure / place / elevation changed: same
// times and positions, so the rows are patched in place; otherwise they are swapped and the list merged again.
void MapDb::routeOwnReplaced(const QList<Fix> &fixes)
{
    ++m_routeGen;
    if (!m_routeLoaded) { ++m_routeReset; return; }
    QList<RouteRow> mine; mine.reserve(fixes.size());
    for (const Fix &in : fixes) {
        const QString t = in.time.toString(Qt::ISODate);
        const Fix f = routeFixRow(in, t);
        if (f.accuracy <= 500.0) mine.append(RouteRow{t, f, true, -1, routeMs(f.time)});
    }
    std::stable_sort(mine.begin(), mine.end(), [](const RouteRow &a, const RouteRow &b) { return a.t < b.t; });
    if (!m_routeRefilter && m_routeSorted == m_routeRaw.size()) {
        QList<qsizetype> at;
        for (qsizetype i = 0; i < m_routeRaw.size(); ++i) if (m_routeRaw[i].own) at.append(i);
        bool same = at.size() == mine.size();
        for (qsizetype k = 0; same && k < at.size(); ++k) {
            const RouteRow &a = m_routeRaw[at[k]], &b = mine[k];
            same = a.t == b.t && a.f.lat == b.f.lat && a.f.lon == b.f.lon && a.f.accuracy == b.f.accuracy;
        }
        if (same) {
            for (qsizetype k = 0; k < at.size(); ++k) {
                RouteRow &a = m_routeRaw[at[k]];
                if (sameRouteFix(a.f, mine[k].f)) continue;
                a.f = mine[k].f; a.ms = mine[k].ms; m_routePatch.append(at[k]);
            }
            return;
        }
    }
    qsizetype w = 0, sorted = 0;
    for (qsizetype i = 0; i < m_routeRaw.size(); ++i) {
        if (m_routeRaw[i].own) continue;
        if (i < m_routeSorted) ++sorted;
        if (w != i) m_routeRaw[w] = std::move(m_routeRaw[i]);
        ++w;
    }
    m_routeRaw.resize(w);
    m_routeRaw.append(mine);
    m_routeSorted = sorted; m_routeRefilter = true; ++m_routeReset;
}

static const char *FLOCK_COLS = "id, lat, lon, source, model, operator, direction, bssid, ble_mac, confidence, detection_method, first_seen, last_seen, sighting_count, vetted, vetted_at, notes, seq, pass_count, camera_type, tags,"
                                " manufacturer, osm_version, osm_timestamp, stale";
static const char *FLOCK_PLACEHOLDERS = "?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?";

static FlockCamera flockRow(const QSqlQuery &q)
{
    FlockCamera c;
    c.id = q.value(0).toString();
    c.lat = q.value(1).toDouble();
    c.lon = q.value(2).toDouble();
    c.source = q.value(3).toString();
    c.model = q.value(4).toString();
    c.operatorName = q.value(5).toString();
    c.direction = q.value(6).toString();
    c.bssid = q.value(7).toString();
    c.bleMac = q.value(8).toString();
    c.confidence = q.value(9).toInt();
    c.detectionMethod = q.value(10).toString();
    c.firstSeen = QDateTime::fromString(q.value(11).toString(), Qt::ISODate);
    c.lastSeen = QDateTime::fromString(q.value(12).toString(), Qt::ISODate);
    c.sightingCount = q.value(13).toInt();
    c.vetted = q.value(14).toInt() != 0;
    c.vettedAt = QDateTime::fromString(q.value(15).toString(), Qt::ISODate);
    c.notes = q.value(16).toString();
    c.seq = q.value(17).toLongLong();
    c.passCount = q.value(18).toInt();
    c.cameraType = q.value(19).toString();
    c.tags = q.value(20).toString();
    c.manufacturer = q.value(21).toString();
    c.osmVersion = q.value(22).toInt();
    c.osmTimestamp = q.value(23).toString();
    c.stale = q.value(24).toInt() != 0;
    if (c.cameraType.isEmpty())
        c.cameraType = PlateEvents::classifyCamera(c.model, c.source, c.id, c.detectionMethod, QJsonDocument::fromJson(c.tags.toUtf8()).object());
    return c;
}

static QString cameraTypeOf(const FlockCamera &c)
{
    static const QSet<QString> types{QStringLiteral("alpr"), QStringLiteral("webcam"), QStringLiteral("ptz"), QStringLiteral("cctv"), QStringLiteral("enforcement"), QStringLiteral("not_camera")};
    if (types.contains(c.cameraType) && c.tags.isEmpty()) return c.cameraType;   // the OSM tags, when we have them, decide
    return PlateEvents::classifyCamera(c.model, c.source, c.id, c.detectionMethod, QJsonDocument::fromJson(c.tags.toUtf8()).object());
}

QHash<QString, QList<TrackSmoother::Fix>> MapDb::deviceFixes() const
{
    QHash<QString, QList<TrackSmoother::Fix>> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.exec(QStringLiteral("SELECT COALESCE(device, ''), time, lat, lon, acc, COALESCE(source, '') FROM fixes WHERE lat IS NOT NULL ORDER BY device, time"));
    while (q.next()) {
        const QDateTime t = QDateTime::fromString(q.value(1).toString(), Qt::ISODate);
        if (!t.isValid()) continue;
        TrackSmoother::Fix f; f.tMs = t.toMSecsSinceEpoch(); f.lat = q.value(2).toDouble(); f.lon = q.value(3).toDouble();
        f.acc = q.value(4).toDouble() > 0 ? q.value(4).toDouble() : 30; f.source = q.value(5).toString();
        out[q.value(0).toString()].append(f);
    }
    for (auto &l : out) std::sort(l.begin(), l.end(), [](const TrackSmoother::Fix &a, const TrackSmoother::Fix &b) { return a.tMs < b.tMs; });   // mixed time-zone text: sort by instant
    return out;
}

QList<QPair<QString, double>> MapDb::providerErrorSamples() const
{
    QList<QPair<QString, double>> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    // The nearest-in-time precise phone fix for each of this host's Wi-Fi fixes (the phone is with us: a few metres
    // apart at most, against errors of hundreds); time text compared via strftime (local and Z forms both parse)
    q.exec(QStringLiteral("SELECT w.id, w.provider, w.lat, w.lon, w.acc, p.lat, p.lon, ABS(strftime('%s', p.time) - strftime('%s', w.time)) AS dt "
                          "FROM fixes w JOIN fixes p ON p.device <> '' AND p.source LIKE 'phone%' AND p.acc > 0 AND p.acc <= 15 "
                          "AND ABS(strftime('%s', p.time) - strftime('%s', w.time)) <= 120 "
                          "WHERE w.source = 'wifi' AND (w.device = '' OR w.device IS NULL) AND w.provider IN ('apple', 'beacondb', 'internal', 'fingerprint') AND w.acc > 0 "
                          "ORDER BY w.id, dt"));
    qint64 last = -1;
    while (q.next()) {
        if (q.value(0).toLongLong() == last) continue;      // nearest only
        last = q.value(0).toLongLong();
        const double d = Locator::distanceM(q.value(2).toDouble(), q.value(3).toDouble(), q.value(5).toDouble(), q.value(6).toDouble());
        out.append({q.value(1).toString(), d / q.value(4).toDouble()});
    }
    return out;
}

QList<FlockCamera> MapDb::loadFlockCameras() const
{
    QList<FlockCamera> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.exec(QStringLiteral("SELECT %1 FROM flock_cameras ORDER BY last_seen DESC").arg(QLatin1String(FLOCK_COLS)));
    while (q.next()) out << flockRow(q);
    return out;
}

// A box through the flock_pos index: after a nationwide sync the table holds ~140k rows, and
// loading them all costs seconds on the GUI thread. limit > 0 keeps the nearest to the box centre.
QList<FlockCamera> MapDb::loadFlockCamerasIn(double latMin, double latMax, double lonMin, double lonMax, int limit) const
{
    QList<FlockCamera> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    const double cLat = (latMin + latMax) / 2, cLon = (lonMin + lonMax) / 2, k = std::cos(qDegreesToRadians(cLat));
    q.prepare(QStringLiteral("SELECT %1 FROM flock_cameras WHERE lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?%2")
              .arg(QLatin1String(FLOCK_COLS), limit > 0 ? QStringLiteral(" ORDER BY (lat-?)*(lat-?) + (lon-?)*(lon-?)*? LIMIT ?") : QString()));
    q.addBindValue(latMin); q.addBindValue(latMax); q.addBindValue(lonMin); q.addBindValue(lonMax);
    if (limit > 0) { q.addBindValue(cLat); q.addBindValue(cLat); q.addBindValue(cLon); q.addBindValue(cLon); q.addBindValue(k * k); q.addBindValue(limit); }
    if (!q.exec()) return out;
    while (q.next()) out << flockRow(q);
    return out;
}

bool MapDb::saveFlockCamera(const FlockCamera &c)
{
    return saveFlockCameras(QList<FlockCamera>{c}) > 0;
}

int MapDb::saveFlockCameras(const QList<FlockCamera> &cams)
{
    if (!m_db.isOpen() || m_readOnly || cams.isEmpty()) return 0;
    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO flock_cameras(%1) VALUES (%2)").arg(QLatin1String(FLOCK_COLS), QLatin1String(FLOCK_PLACEHOLDERS)));
    // INSERT OR REPLACE rewrites the row: keep what an Overpass refresh does not know (pass count, OSM tags fetched
    // later, DeFlock's manufacturer / OSM version)
    QSqlQuery keep(m_db);
    keep.prepare(QStringLiteral("SELECT pass_count, tags, manufacturer, osm_version, osm_timestamp, trust, COALESCE(trust_detail,''), COALESCE(verdict,''),"
                                " COALESCE(verdict_at,''), COALESCE(watched_way,0), COALESCE(watched_detail,''), COALESCE(ways_fetched,''), COALESCE(agency_portal,'')"
                                " FROM flock_cameras WHERE id=?"));
    // … and what BeaconFix learned about the camera itself (docs/SIGHTINGS.md §2.6, §2.7, §4.6): put back after the REPLACE
    QSqlQuery restore(m_db);
    restore.prepare(QStringLiteral("UPDATE flock_cameras SET trust=?, trust_detail=?, verdict=?, verdict_at=?, watched_way=?, watched_detail=?, ways_fetched=?,"
                                   " agency_portal=? WHERE id=?"));
    int saved = 0;
    for (const FlockCamera &c : cams) {
        q.addBindValue(c.id);
        q.addBindValue(c.lat);
        q.addBindValue(c.lon);
        q.addBindValue(c.source);
        q.addBindValue(c.model);
        q.addBindValue(c.operatorName);
        q.addBindValue(c.direction);
        q.addBindValue(c.bssid);
        q.addBindValue(c.bleMac);
        q.addBindValue(c.confidence);
        q.addBindValue(c.detectionMethod);
        q.addBindValue(c.firstSeen.isValid() ? c.firstSeen.toString(Qt::ISODate) : QDateTime::currentDateTime().toString(Qt::ISODate));
        q.addBindValue(c.lastSeen.isValid() ? c.lastSeen.toString(Qt::ISODate) : QDateTime::currentDateTime().toString(Qt::ISODate));
        q.addBindValue(c.sightingCount);
        q.addBindValue(c.vetted ? 1 : 0);
        q.addBindValue(c.vettedAt.isValid() ? c.vettedAt.toString(Qt::ISODate) : QString());
        q.addBindValue(c.notes);
        q.addBindValue(double(c.seq > 0 ? c.seq : nextSeq()));
        FlockCamera k = c;
        keep.addBindValue(c.id);
        QVariantList extras;
        if (keep.exec() && keep.next()) {
            k.passCount = qMax(c.passCount, keep.value(0).toInt());
            if (k.tags.isEmpty()) k.tags = keep.value(1).toString();
            if (k.manufacturer.isEmpty()) k.manufacturer = keep.value(2).toString();
            if (k.osmVersion <= 0) { k.osmVersion = keep.value(3).toInt(); k.osmTimestamp = keep.value(4).toString(); }
            for (int i = 5; i <= 12; ++i) extras << keep.value(i);
        }
        q.addBindValue(k.passCount);
        q.addBindValue(cameraTypeOf(k));
        q.addBindValue(k.tags);
        q.addBindValue(k.manufacturer);
        q.addBindValue(k.osmVersion);
        q.addBindValue(k.osmTimestamp);
        q.addBindValue(k.stale ? 1 : 0);
        if (q.exec()) {
            saved++;
            if (!extras.isEmpty()) { for (const QVariant &v : std::as_const(extras)) restore.addBindValue(v); restore.addBindValue(c.id); restore.exec(); }
        }
    }
    m_db.commit();
    if (saved > 0) {
        markDirty();
        emit changed();
    }
    return saved;
}

bool MapDb::recordFlockSighting(const QString &bssidOrMac, double lat, double lon, const FlockDetector::Detection &det, const QDateTime &time)
{
    if (!m_db.isOpen() || m_readOnly || lat == 0.0 || lon == 0.0) return false;
    const QString nowIso = (time.isValid() ? time : QDateTime::currentDateTime()).toString(Qt::ISODate);

    // Look for existing camera within 100 meters
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, lat, lon, source, model, operator, direction, bssid, ble_mac, confidence, detection_method, first_seen, last_seen, sighting_count, vetted, vetted_at, notes, seq, pass_count, COALESCE(camera_type,'') FROM flock_cameras WHERE lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?"));
    q.addBindValue(lat - 0.002);
    q.addBindValue(lat + 0.002);
    q.addBindValue(lon - 0.002);
    q.addBindValue(lon + 0.002);
    if (!q.exec()) return false;

    QString matchedId;
    FlockCamera cam;
    double bestDist = 100.0;

    const bool gunshot = det.cameraType == QLatin1String("not_camera");   // a Raven confirms no camera, and no camera confirms it
    while (q.next()) {
        if ((q.value(19).toString() == QLatin1String("not_camera")) != gunshot) continue;
        const double cLat = q.value(1).toDouble();
        const double cLon = q.value(2).toDouble();
        const double d = Locator::distanceM(lat, lon, cLat, cLon);
        if (d < bestDist) {
            bestDist = d;
            matchedId = q.value(0).toString();
            cam.id = matchedId;
            cam.lat = cLat;
            cam.lon = cLon;
            cam.source = q.value(3).toString();
            cam.model = q.value(4).toString();
            cam.operatorName = q.value(5).toString();
            cam.direction = q.value(6).toString();
            cam.bssid = q.value(7).toString();
            cam.bleMac = q.value(8).toString();
            cam.confidence = q.value(9).toInt();
            cam.detectionMethod = q.value(10).toString();
            cam.firstSeen = QDateTime::fromString(q.value(11).toString(), Qt::ISODate);
            cam.lastSeen = QDateTime::fromString(q.value(12).toString(), Qt::ISODate);
            cam.sightingCount = q.value(13).toInt();
            cam.vetted = q.value(14).toInt() != 0;
            cam.vettedAt = QDateTime::fromString(q.value(15).toString(), Qt::ISODate);
            cam.notes = q.value(16).toString();
            cam.passCount = q.value(18).toInt();
        }
    }

    const qint64 seq = nextSeq();
    {   // an RF detection is trust evidence for the cameras around it (docs/SIGHTINGS.md §2.7): recompute theirs
        QSqlQuery t(m_db);
        t.prepare(QStringLiteral("UPDATE flock_cameras SET trust=NULL WHERE lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?"));
        t.addBindValue(lat - 0.001); t.addBindValue(lat + 0.001); t.addBindValue(lon - 0.0013); t.addBindValue(lon + 0.0013);
        t.exec();
    }
    if (!matchedId.isEmpty()) {
        // Correlated with existing camera: mark vetted and record field confirmation
        cam.vetted = true;
        cam.vettedAt = time.isValid() ? time : QDateTime::currentDateTime();
        cam.lastSeen = cam.vettedAt;
        cam.sightingCount++;
        if (det.method.startsWith(QLatin1String("wifi")) && cam.bssid.isEmpty())
            cam.bssid = bssidOrMac;
        else if (det.method.startsWith(QLatin1String("ble")) && cam.bleMac.isEmpty())
            cam.bleMac = bssidOrMac;

        QString newNote = QStringLiteral("Field confirmed (%1) at %2").arg(det.method, nowIso);
        if (cam.notes.isEmpty()) cam.notes = newNote;
        else if (!cam.notes.contains(det.method)) cam.notes += QStringLiteral(" · ") + newNote;

        QSqlQuery u(m_db);
        u.prepare(QStringLiteral("UPDATE flock_cameras SET last_seen=?, sighting_count=?, vetted=1, vetted_at=?, bssid=?, ble_mac=?, notes=?, seq=?, stale=0 WHERE id=?"));
        u.addBindValue(nowIso);
        u.addBindValue(cam.sightingCount);
        u.addBindValue(nowIso);
        u.addBindValue(cam.bssid);
        u.addBindValue(cam.bleMac);
        u.addBindValue(cam.notes);
        u.addBindValue(double(seq));
        u.addBindValue(cam.id);
        u.exec();
    } else {
        // New camera detection candidate
        FlockCamera c;
        c.id = QStringLiteral("det:%1").arg(bssidOrMac.toUpper());
        c.lat = lat;
        c.lon = lon;
        c.source = det.method.startsWith(QLatin1String("ble")) ? QStringLiteral("ble_scan") : QStringLiteral("wifi_scan");
        c.model = det.model.isEmpty() ? det.label : det.model;
        c.manufacturer = QStringLiteral("Flock Safety");          // the hardware's maker; the operator (an agency) is unknown
        if (det.method.startsWith(QLatin1String("wifi"))) c.bssid = bssidOrMac;
        else c.bleMac = bssidOrMac;
        c.confidence = det.confidence;
        c.detectionMethod = det.method;
        c.firstSeen = time.isValid() ? time : QDateTime::currentDateTime();
        c.lastSeen = c.firstSeen;
        c.sightingCount = 1;
        c.passCount = 0;
        c.vetted = false;
        c.notes = det.details;
        c.seq = seq;

        QSqlQuery ins(m_db);
        ins.prepare(QStringLiteral("INSERT OR REPLACE INTO flock_cameras(id, lat, lon, source, model, operator, direction, bssid, ble_mac, confidence, detection_method, first_seen, last_seen, sighting_count, vetted, vetted_at, notes, seq, pass_count, camera_type, tags, manufacturer)"
                                   " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'',?)"));
        ins.addBindValue(c.id);
        ins.addBindValue(c.lat);
        ins.addBindValue(c.lon);
        ins.addBindValue(c.source);
        ins.addBindValue(c.model);
        ins.addBindValue(c.operatorName);
        ins.addBindValue(c.direction);
        ins.addBindValue(c.bssid);
        ins.addBindValue(c.bleMac);
        ins.addBindValue(c.confidence);
        ins.addBindValue(c.detectionMethod);
        ins.addBindValue(nowIso);
        ins.addBindValue(nowIso);
        ins.addBindValue(c.sightingCount);
        ins.addBindValue(0);
        ins.addBindValue(QString());
        ins.addBindValue(c.notes);
        ins.addBindValue(double(seq));
        ins.addBindValue(0);
        ins.addBindValue(det.cameraType.isEmpty() ? QStringLiteral("alpr") : det.cameraType);   // a Raven is a gunshot detector, not a camera
        ins.addBindValue(c.manufacturer);
        ins.exec();
    }

    markDirty();
    emit changed();
    return true;
}

QJsonObject MapDb::flockStats() const
{
    QJsonObject o;
    if (!m_db.isOpen()) return o;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT COUNT(*), SUM(CASE WHEN vetted=1 THEN 1 ELSE 0 END), SUM(CASE WHEN vetted=0 THEN 1 ELSE 0 END), SUM(sighting_count) FROM flock_cameras"));
    if (q.next()) {
        o[QStringLiteral("total")] = q.value(0).toInt();
        o[QStringLiteral("vetted")] = q.value(1).toInt();
        o[QStringLiteral("candidates")] = q.value(2).toInt();
        o[QStringLiteral("sightings")] = q.value(3).toInt();
    }
    return o;
}

QByteArray MapDb::exportFlockGeoJson() const
{
    QJsonObject root;
    root[QStringLiteral("type")] = QStringLiteral("FeatureCollection");
    QJsonArray feats;
    for (const FlockCamera &c : loadFlockCameras()) {
        QJsonObject feat;
        feat[QStringLiteral("type")] = QStringLiteral("Feature");
        QJsonObject geom;
        geom[QStringLiteral("type")] = QStringLiteral("Point");
        geom[QStringLiteral("coordinates")] = QJsonArray{c.lon, c.lat};
        feat[QStringLiteral("geometry")] = geom;
        QJsonObject props = c.toJson();
        props.remove(QStringLiteral("lat"));
        props.remove(QStringLiteral("lon"));
        feat[QStringLiteral("properties")] = props;
        feats.append(feat);
    }
    root[QStringLiteral("features")] = feats;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// New ones only (INSERT OR IGNORE): a camera we already have keeps its sightings, vetting and pass count
int MapDb::insertNewFlockCameras(const QList<FlockCamera> &cams)
{
    if (!m_db.isOpen() || m_readOnly || cams.isEmpty()) return 0;
    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO flock_cameras(%1) VALUES (%2)").arg(QLatin1String(FLOCK_COLS), QLatin1String(FLOCK_PLACEHOLDERS)));
    int saved = 0;
    for (const FlockCamera &c : cams) {
        q.addBindValue(c.id); q.addBindValue(c.lat); q.addBindValue(c.lon); q.addBindValue(c.source); q.addBindValue(c.model);
        q.addBindValue(c.operatorName); q.addBindValue(c.direction); q.addBindValue(c.bssid); q.addBindValue(c.bleMac);
        q.addBindValue(c.confidence); q.addBindValue(c.detectionMethod);
        q.addBindValue(c.firstSeen.toString(Qt::ISODate)); q.addBindValue(c.lastSeen.toString(Qt::ISODate));
        q.addBindValue(c.sightingCount); q.addBindValue(c.vetted ? 1 : 0);
        q.addBindValue(c.vettedAt.isValid() ? c.vettedAt.toString(Qt::ISODate) : QString());
        q.addBindValue(c.notes); q.addBindValue(double(c.seq > 0 ? c.seq : nextSeq())); q.addBindValue(0);
        q.addBindValue(cameraTypeOf(c)); q.addBindValue(c.tags);
        q.addBindValue(c.manufacturer); q.addBindValue(c.osmVersion); q.addBindValue(c.osmTimestamp); q.addBindValue(c.stale ? 1 : 0);
        if (q.exec() && q.numRowsAffected() > 0) ++saved;
    }
    m_db.commit();
    if (saved > 0) markDirty();
    return saved;
}

// The old flocklocations bulk import's OSM-derived rows (replaced by DeFlock, docs/DATABASE.md)
static const char *kLegacyBulkSources = "'3rd Party / Suspected', 'openstreetmap', 'flocklocations'";

bool MapDb::hasLegacyBulkCameras() const
{
    if (!m_db.isOpen()) return false;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT 1 FROM flock_cameras WHERE id LIKE 'osm:%' AND source IN (%1) LIMIT 1").arg(QLatin1String(kLegacyBulkSources)));
    return q.next();
}

QJsonObject MapDb::upsertDeflockCameras(const QList<FlockCamera> &cams)
{
    int added = 0, updated = 0, same = 0;
    if (!m_db.isOpen() || m_readOnly || cams.isEmpty()) return QJsonObject{{"added", 0}, {"updated", 0}, {"unchanged", 0}};
    m_db.transaction();
    QSqlQuery get(m_db), ins(m_db), upTagged(m_db), up(m_db);
    get.prepare(QStringLiteral("SELECT lat, lon, source, COALESCE(model,''), COALESCE(operator,''), COALESCE(manufacturer,''), COALESCE(direction,''), COALESCE(osm_version,0),"
                               " COALESCE(osm_timestamp,''), COALESCE(stale,0), COALESCE(tags,''), COALESCE(camera_type,'') FROM flock_cameras WHERE id=?"));
    ins.prepare(QStringLiteral("INSERT INTO flock_cameras(%1) VALUES (%2)").arg(QLatin1String(FLOCK_COLS), QLatin1String(FLOCK_PLACEHOLDERS)));
    // a row with OSM tags (Overpass) knows more than DeFlock: it only gains the version / timestamp / manufacturer
    upTagged.prepare(QStringLiteral("UPDATE flock_cameras SET manufacturer=CASE WHEN COALESCE(manufacturer,'')='' THEN ? ELSE manufacturer END, osm_version=?, osm_timestamp=?,"
                                    " stale=0, seq=? WHERE id=?"));
    up.prepare(QStringLiteral("UPDATE flock_cameras SET lat=?, lon=?, source='deflock', model='', operator=?, manufacturer=?, direction=?, osm_version=?, osm_timestamp=?, confidence=?,"
                              " detection_method=?, camera_type='alpr', stale=0, notes=CASE WHEN COALESCE(notes,'')='' THEN ? ELSE notes END, seq=? WHERE id=?"));
    for (const FlockCamera &c : cams) {
        get.addBindValue(c.id);
        QVariantList row;
        if (get.exec() && get.next()) for (int i = 0; i < 12; ++i) row << get.value(i);
        get.finish();
        if (row.isEmpty()) {
            const QList<QVariant> v{c.id, c.lat, c.lon, c.source, c.model, c.operatorName, c.direction, c.bssid, c.bleMac, c.confidence, c.detectionMethod,
                                    c.firstSeen.toString(Qt::ISODate), c.lastSeen.toString(Qt::ISODate), c.sightingCount, c.vetted ? 1 : 0, QString(), c.notes,
                                    double(nextSeq()), 0, c.cameraType.isEmpty() ? QStringLiteral("alpr") : c.cameraType, QString(), c.manufacturer, c.osmVersion,
                                    c.osmTimestamp, 0};
            for (const QVariant &x : v) ins.addBindValue(x);
            if (ins.exec()) ++added;
            continue;
        }
        const bool stale = row.at(9).toInt() != 0;
        if (!row.at(10).toString().isEmpty()) {
            const bool sameMeta = row.at(7).toInt() == c.osmVersion && row.at(8).toString() == c.osmTimestamp && !stale
                               && (!row.at(5).toString().isEmpty() || c.manufacturer.isEmpty());
            if (sameMeta) { ++same; continue; }
            upTagged.addBindValue(c.manufacturer); upTagged.addBindValue(c.osmVersion); upTagged.addBindValue(c.osmTimestamp);
            upTagged.addBindValue(double(nextSeq())); upTagged.addBindValue(c.id);
            if (upTagged.exec()) ++updated;
            continue;
        }
        const bool unchanged = std::fabs(row.at(0).toDouble() - c.lat) < 1e-7 && std::fabs(row.at(1).toDouble() - c.lon) < 1e-7
                            && row.at(2).toString() == QLatin1String("deflock") && row.at(3).toString().isEmpty() && row.at(4).toString() == c.operatorName
                            && row.at(5).toString() == c.manufacturer && row.at(6).toString() == c.direction && row.at(7).toInt() == c.osmVersion
                            && row.at(8).toString() == c.osmTimestamp && !stale && row.at(11).toString() == QLatin1String("alpr");
        if (unchanged) { ++same; continue; }
        const QList<QVariant> v{c.lat, c.lon, c.operatorName, c.manufacturer, c.direction, c.osmVersion, c.osmTimestamp, c.confidence, c.detectionMethod, c.notes,
                                double(nextSeq()), c.id};
        for (const QVariant &x : v) up.addBindValue(x);
        if (up.exec()) ++updated;
    }
    m_db.commit();
    if (added || updated) { markDirty(); emit changed(); }
    return QJsonObject{{"added", added}, {"updated", updated}, {"unchanged", same}};
}

QJsonObject MapDb::reconcileBulkCameras(const QSet<QString> &deflockIds)
{
    int checked = 0, deleted = 0, staled = 0;
    if (!m_db.isOpen() || m_readOnly) return QJsonObject{{"error", "database not open"}};
    if (deflockIds.size() < CameraImport::kDeflockMinFeatures)
        return QJsonObject{{"error", QStringLiteral("only %1 DeFlock cameras: not purging on an incomplete list").arg(deflockIds.size())}};
    QSet<QString> passed;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT DISTINCT camera_id FROM plate_events WHERE camera_id LIKE 'osm:%'"));
    while (q.next()) passed.insert(q.value(0).toString());
    QStringList drop, stale;
    q.setForwardOnly(true);
    q.exec(QStringLiteral("SELECT id, COALESCE(stale,0) FROM flock_cameras WHERE id LIKE 'osm:%' AND source IN (%1, 'deflock')").arg(QLatin1String(kLegacyBulkSources)));
    while (q.next()) {
        ++checked;
        const QString id = q.value(0).toString();
        if (deflockIds.contains(id)) continue;
        if (passed.contains(id)) { if (q.value(1).toInt() == 0) stale << id; }
        else drop << id;
    }
    m_db.transaction();
    QSqlQuery del(m_db), st(m_db);
    del.prepare(QStringLiteral("DELETE FROM flock_cameras WHERE id=?"));
    st.prepare(QStringLiteral("UPDATE flock_cameras SET stale=1, seq=? WHERE id=?"));
    for (const QString &id : std::as_const(drop)) { del.addBindValue(id); if (del.exec()) ++deleted; }
    for (const QString &id : std::as_const(stale)) { st.addBindValue(double(nextSeq())); st.addBindValue(id); if (st.exec()) ++staled; }
    m_db.commit();
    if (deleted || staled) { markDirty(); emit changed(); }
    return QJsonObject{{"checked", checked}, {"deleted", deleted}, {"stale", staled}};
}

QJsonObject MapDb::cameraCounts() const
{
    QJsonObject o, bySource, byType;
    if (!m_db.isOpen()) return o;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT COUNT(*), SUM(COALESCE(stale,0)) FROM flock_cameras"));
    if (q.next()) { o[QStringLiteral("total")] = q.value(0).toInt(); o[QStringLiteral("stale")] = q.value(1).toInt(); }
    q.exec(QStringLiteral("SELECT COALESCE(source,''), COUNT(*) FROM flock_cameras GROUP BY 1"));
    while (q.next()) bySource[q.value(0).toString()] = q.value(1).toInt();
    q.exec(QStringLiteral("SELECT COALESCE(camera_type,''), COUNT(*) FROM flock_cameras GROUP BY 1"));
    while (q.next()) byType[q.value(0).toString()] = q.value(1).toInt();
    o[QStringLiteral("bySource")] = bySource;
    o[QStringLiteral("byType")] = byType;
    return o;
}

QList<LicensePlate> MapDb::loadLicensePlates() const
{
    QList<LicensePlate> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT plate, display_plate, state, vehicle_desc, make, model, color, active, added_at, notes FROM license_plates ORDER BY active DESC, added_at DESC"));
    while (q.next()) {
        LicensePlate p;
        p.plate = q.value(0).toString();
        p.displayPlate = q.value(1).toString();
        p.state = q.value(2).toString();
        p.vehicleDesc = q.value(3).toString();
        p.make = q.value(4).toString();
        p.model = q.value(5).toString();
        p.color = q.value(6).toString();
        p.active = q.value(7).toInt() != 0;
        p.addedAt = QDateTime::fromString(q.value(8).toString(), Qt::ISODate);
        p.notes = q.value(9).toString();
        out << p;
    }
    return out;
}

bool MapDb::saveLicensePlate(const LicensePlate &p)
{
    if (!m_db.isOpen() || m_readOnly) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO license_plates(plate, display_plate, state, vehicle_desc, make, model, color, active, added_at, notes) VALUES (?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(p.plate);
    q.addBindValue(p.displayPlate.isEmpty() ? p.plate : p.displayPlate);
    q.addBindValue(p.state);
    q.addBindValue(p.vehicleDesc);
    q.addBindValue(p.make);
    q.addBindValue(p.model);
    q.addBindValue(p.color);
    q.addBindValue(p.active ? 1 : 0);
    q.addBindValue(p.addedAt.isValid() ? p.addedAt.toString(Qt::ISODate) : QDateTime::currentDateTime().toString(Qt::ISODate));
    q.addBindValue(p.notes);
    const bool ok = q.exec();
    if (ok) {
        markDirty();
        emit changed();
    }
    return ok;
}

bool MapDb::deleteLicensePlate(const QString &plate)
{
    if (!m_db.isOpen() || m_readOnly) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM license_plates WHERE plate = ?"));
    q.addBindValue(plate.toUpper().remove(QLatin1Char(' ')).remove(QLatin1Char('-')));
    const bool ok = q.exec();
    if (ok) {
        markDirty();
        emit changed();
    }
    return ok;
}

LicensePlate MapDb::activeLicensePlate() const
{
    const auto list = loadLicensePlates();
    for (const auto &p : list) if (p.active) return p;
    if (!list.isEmpty()) return list.first();
    return LicensePlate();                                     // none registered: no plate is invented
}

// /api/v1/flock/encounters: the camera passes (plate_events), numbered per camera in time order
QList<CameraEncounter> MapDb::loadCameraEncounters(const QString &cameraId, int limit) const
{
    QList<CameraEncounter> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT e.id, e.camera_id, e.time, e.lat, e.lon, e.distance_m, e.speed_kmh, e.plate, e.device, e.details, e.source,"
                             " (SELECT COUNT(*) FROM plate_events p WHERE p.kind='camera_pass' AND p.camera_id=e.camera_id AND p.time<=e.time)"
                             " FROM plate_events e WHERE e.kind='camera_pass'%1 ORDER BY e.time DESC LIMIT ?").arg(cameraId.isEmpty() ? QString() : QStringLiteral(" AND e.camera_id=?")));
    if (!cameraId.isEmpty()) q.addBindValue(cameraId);
    q.addBindValue(limit > 0 ? limit : 200);
    if (!q.exec()) return out;
    const LicensePlate active = activeLicensePlate();
    while (q.next()) {
        CameraEncounter e;
        e.id = q.value(0).toLongLong();
        e.cameraId = q.value(1).toString();
        e.time = QDateTime::fromString(q.value(2).toString(), Qt::ISODate);
        e.lat = q.value(3).toDouble();
        e.lon = q.value(4).toDouble();
        e.distanceM = q.value(5).toDouble();
        e.speedKmh = q.value(6).isNull() ? 0 : q.value(6).toDouble();
        e.plate = q.value(7).toString();
        e.vehicleDesc = e.plate == active.displayPlate ? active.vehicleDesc : QString();
        e.device = q.value(8).toString().isEmpty() ? q.value(10).toString() : q.value(8).toString();
        e.notes = q.value(9).toString();
        e.encounterNum = q.value(11).toInt();
        out << e;
    }
    return out;
}

bool MapDb::logCameraEncounter(CameraEncounter &enc)
{
    if (!m_db.isOpen() || m_readOnly) return false;
    const int newCount = cameraPassCount(enc.cameraId) + 1;
    enc.encounterNum = newCount;
    if (!enc.time.isValid()) enc.time = QDateTime::currentDateTime();

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO camera_encounters(camera_id, time, lat, lon, distance_m, speed_kmh, plate, vehicle_desc, device, encounter_num, notes) VALUES (?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(enc.cameraId);
    q.addBindValue(enc.time.toString(Qt::ISODate));
    q.addBindValue(enc.lat);
    q.addBindValue(enc.lon);
    q.addBindValue(enc.distanceM);
    q.addBindValue(enc.speedKmh);
    q.addBindValue(enc.plate);
    q.addBindValue(enc.vehicleDesc);
    q.addBindValue(enc.device);
    q.addBindValue(enc.encounterNum);
    q.addBindValue(enc.notes);
    if (!q.exec()) return false;
    enc.id = q.lastInsertId().toLongLong();

    // Update camera pass count and last seen
    QSqlQuery u(m_db);
    u.prepare(QStringLiteral("UPDATE flock_cameras SET pass_count = ?, last_seen = ? WHERE id = ?"));
    u.addBindValue(newCount);
    u.addBindValue(enc.time.toString(Qt::ISODate));
    u.addBindValue(enc.cameraId);
    u.exec();

    markDirty();
    emit changed();
    return true;
}

int MapDb::cameraPassCount(const QString &cameraId) const
{
    if (!m_db.isOpen() || cameraId.isEmpty()) return 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT pass_count FROM flock_cameras WHERE id = ?"));
    q.addBindValue(cameraId);
    if (q.exec() && q.next()) {
        const int c = q.value(0).toInt();
        if (c > 0) return c;
    }
    QSqlQuery qCount(m_db);
    qCount.prepare(QStringLiteral("SELECT COUNT(*) FROM camera_encounters WHERE camera_id = ?"));
    qCount.addBindValue(cameraId);
    if (qCount.exec() && qCount.next()) return qCount.value(0).toInt();
    return 0;
}

// /api/v1/flock/audits and the D-Bus PlateAuditsJson keep their old shape, fed from plate_events (docs/SIGHTINGS.md §1):
// a camera pass → the camera and its operator; a plate search → the searching agency. plate_audits is no longer written.
QList<PlateAudit> MapDb::loadPlateAudits(const QString &plate, int limit) const
{
    QList<PlateAudit> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    const QString norm = QString(plate).remove(QLatin1Char('-')).remove(QLatin1Char(' ')).toUpper();
    q.prepare(QStringLiteral("SELECT id, plate, camera_id, camera_lat, camera_lon, lat, lon, operator, agency, time, source, confidence, details, kind FROM plate_events"
                             "%1 ORDER BY time DESC LIMIT ?").arg(plate.isEmpty() ? QString() : QStringLiteral(" WHERE REPLACE(REPLACE(UPPER(plate),'-',''),' ','')=?")));
    if (!plate.isEmpty()) q.addBindValue(norm);
    q.addBindValue(limit > 0 ? limit : 200);
    if (!q.exec()) return out;
    while (q.next()) {
        PlateAudit a;
        a.id = q.value(0).toLongLong();
        a.plate = q.value(1).toString();
        const bool pass = q.value(13).toString() == QLatin1String("camera_pass");
        a.cameraId = pass ? q.value(2).toString() : QString();
        a.lat = pass ? q.value(3).toDouble() : q.value(5).toDouble();
        a.lon = pass ? q.value(4).toDouble() : q.value(6).toDouble();
        a.operatorName = pass ? q.value(7).toString() : q.value(8).toString();
        a.timestamp = QDateTime::fromString(q.value(9).toString(), Qt::ISODate);
        a.source = q.value(10).toString();
        a.confidence = q.value(11).toInt();
        a.details = q.value(12).toString();
        out << a;
    }
    return out;
}

bool MapDb::logPlateAudit(const PlateAudit &audit)
{
    Q_UNUSED(audit);                                   // plate_audits is no longer written: plate events replace it (docs/SIGHTINGS.md)
    return false;
}

QList<MapDb::RouteCamHit> MapDb::camerasAlongRoute(const QList<Fix> &fixes, double maxM) const
{
    QList<RouteCamHit> out;
    if (!m_db.isOpen() || fixes.isEmpty()) return out;
    const auto cell = [](double v) { return qint32(std::floor(v * 1000.0)); };       // 0.001° ≈ 100 m
    const auto key = [](qint32 la, qint32 lo) { return (quint64(quint32(la)) << 32) | quint64(quint32(lo)); };
    QHash<quint64, QList<int>> grid;
    double laMin = 1e9, laMax = -1e9, loMin = 1e9, loMax = -1e9;
    for (int i = 0; i < int(fixes.size()); ++i) {
        const Fix &f = fixes[i];
        if (!f.valid) continue;
        grid[key(cell(f.lat), cell(f.lon))].append(i);
        laMin = qMin(laMin, f.lat); laMax = qMax(laMax, f.lat); loMin = qMin(loMin, f.lon); loMax = qMax(loMax, f.lon);
    }
    if (grid.isEmpty()) return out;
    // Only positions, and only inside the route's box (flock_pos): no 19-column parse of ~140k rows
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT id, lat, lon FROM flock_cameras WHERE lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?"));
    q.addBindValue(laMin - 0.002); q.addBindValue(laMax + 0.002); q.addBindValue(loMin - 0.002); q.addBindValue(loMax + 0.002);
    if (!q.exec()) return out;
    QStringList ids; QList<QList<QPair<int, double>>> near;
    QList<int> cand;
    while (q.next()) {
        const double lat = q.value(1).toDouble(), lon = q.value(2).toDouble();
        const qint32 cla = cell(lat), clo = cell(lon);
        cand.clear();
        for (qint32 dy = -1; dy <= 1; ++dy)
            for (qint32 dx = -1; dx <= 1; ++dx) {
                const auto it = grid.constFind(key(cla + dy, clo + dx));
                if (it != grid.constEnd()) cand += *it;
            }
        if (cand.isEmpty()) continue;
        std::sort(cand.begin(), cand.end());
        QList<QPair<int, double>> hits;
        for (int i : std::as_const(cand)) {
            const Fix &f = fixes[i];
            if (std::abs(f.lat - lat) > 0.001 || std::abs(f.lon - lon) > 0.001) continue;
            const double d = Locator::distanceM(f.lat, f.lon, lat, lon);
            if (d <= maxM) hits.append({i, d});
        }
        if (hits.isEmpty()) continue;
        ids << q.value(0).toString(); near << hits;
    }
    QSqlQuery one(m_db);
    one.prepare(QStringLiteral("SELECT %1 FROM flock_cameras WHERE id=?").arg(QLatin1String(FLOCK_COLS)));
    for (int k = 0; k < int(ids.size()); ++k) {
        one.addBindValue(ids[k]);
        if (one.exec() && one.next()) out.append(RouteCamHit{flockRow(one), near[k]});
    }
    return out;
}


QJsonObject MapDb::alprSummary() const
{
    QJsonObject o;
    if (!m_db.isOpen()) return o;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT COUNT(*), SUM(CASE WHEN vetted=1 THEN 1 ELSE 0 END), SUM(CASE WHEN pass_count > 0 THEN 1 ELSE 0 END), SUM(pass_count) FROM flock_cameras"));
    if (q.next()) {
        o[QStringLiteral("totalCameras")] = q.value(0).toInt();
        o[QStringLiteral("vettedCameras")] = q.value(1).toInt();
        o[QStringLiteral("passedCameras")] = q.value(2).toInt();
        o[QStringLiteral("totalPasses")] = q.value(3).toInt();
    }
    QJsonArray pArr;
    for (const auto &p : loadLicensePlates()) pArr.append(p.toJson());
    o[QStringLiteral("plates")] = pArr;

    QJsonArray eArr;
    for (const auto &e : loadCameraEncounters(QString(), 50)) eArr.append(e.toJson());
    o[QStringLiteral("recentEncounters")] = eArr;

    QJsonArray aArr;
    for (const auto &a : loadPlateAudits(QString(), 50)) aArr.append(a.toJson());
    o[QStringLiteral("recentAudits")] = aArr;

    return o;
}

// ── Plate events (docs/SIGHTINGS.md) ──────────────────────────────────────────
static const char *PE_COLS[] = {"kind", "plate", "time", "lat", "lon", "acc", "camera_id", "camera_lat", "camera_lon", "distance_m", "speed_kmh", "heading_deg",
                                "approach_bearing_deg", "camera_dir_deg", "facing", "operator", "agency", "model", "camera_type", "source", "source_url",
                                "source_name", "confidence", "leaky", "details", "metrics", "raw", "device"};
static bool peJsonCol(const QString &c) { return c == QLatin1String("metrics") || c == QLatin1String("raw"); }
static bool peIntCol(const QString &c) { return c == QLatin1String("facing") || c == QLatin1String("confidence") || c == QLatin1String("leaky"); }
static bool peRealCol(const QString &c)
{
    static const QSet<QString> r{QStringLiteral("lat"), QStringLiteral("lon"), QStringLiteral("acc"), QStringLiteral("camera_lat"), QStringLiteral("camera_lon"),
                                 QStringLiteral("distance_m"), QStringLiteral("speed_kmh"), QStringLiteral("heading_deg"), QStringLiteral("approach_bearing_deg"),
                                 QStringLiteral("camera_dir_deg")};
    return r.contains(c);
}

// camelCase (what a phone may send) → column names; metrics / raw as objects; booleans → 0/1; time → local ISO
static QJsonObject peNormalise(const QJsonObject &in)
{
    static const QHash<QString, QString> alias{
        {"cameraId", "camera_id"}, {"cameraLat", "camera_lat"}, {"cameraLon", "camera_lon"}, {"distanceM", "distance_m"}, {"speedKmh", "speed_kmh"},
        {"headingDeg", "heading_deg"}, {"approachBearingDeg", "approach_bearing_deg"}, {"cameraDirDeg", "camera_dir_deg"}, {"cameraType", "camera_type"},
        {"sourceUrl", "source_url"}, {"sourceName", "source_name"}, {"createdAt", "created_at"}, {"updatedAt", "updated_at"}, {"operatorName", "operator"}};
    QJsonObject o;
    for (auto it = in.begin(); it != in.end(); ++it) o[alias.value(it.key(), it.key())] = it.value();
    for (const char *k : {"metrics", "raw"}) {
        const QJsonValue v = o.value(QLatin1String(k));
        if (v.isString()) { const QJsonDocument d = QJsonDocument::fromJson(v.toString().toUtf8()); o[QLatin1String(k)] = d.isObject() ? QJsonValue(d.object()) : QJsonValue(); }
        else if (!v.isObject() && !v.isUndefined()) o[QLatin1String(k)] = QJsonValue();
    }
    for (const char *k : {"facing", "leaky"}) if (o.value(QLatin1String(k)).isBool()) o[QLatin1String(k)] = o.value(QLatin1String(k)).toBool() ? 1 : 0;
    if (o.value(QLatin1String("time")).isString()) o["time"] = localIso(o.value(QLatin1String("time")).toString());
    return o;
}

static QVariant peBind(const QString &col, const QJsonValue &v)
{
    if (v.isNull() || v.isUndefined()) return QVariant();
    if (peJsonCol(col)) return v.isObject() ? QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact)) : QVariant();
    if (peIntCol(col)) return v.isBool() ? QVariant(v.toBool() ? 1 : 0) : QVariant(qint64(std::llround(v.toDouble())));
    if (peRealCol(col)) return v.isDouble() ? QVariant(v.toDouble()) : (v.toString().isEmpty() ? QVariant() : QVariant(v.toString().toDouble()));
    return v.isDouble() ? QVariant(QString::number(v.toDouble(), 'g', 15)) : QVariant(v.toString());
}

static QJsonObject peRow(const QSqlQuery &q, bool withRaw)
{
    QJsonObject o;
    const QSqlRecord rec = q.record();
    for (int i = 0; i < rec.count(); ++i) {
        const QString k = rec.fieldName(i);
        if (k == QLatin1String("id")) continue;
        if (k == QLatin1String("raw") && !withRaw) continue;
        const QVariant v = q.value(i);
        if (v.isNull()) { o[k] = QJsonValue(); continue; }
        if (peJsonCol(k)) { o[k] = QJsonDocument::fromJson(v.toString().toUtf8()).object(); continue; }
        if (k == QLatin1String("seq")) { o[k] = v.toDouble(); continue; }
        if (peIntCol(k)) { o[k] = v.toInt(); continue; }
        if (peRealCol(k)) { o[k] = v.toDouble(); continue; }
        o[k] = v.toString();
    }
    return o;
}

static int peRank(const QString &source)
{
    if (source == QLatin1String("dashcam") || source == QLatin1String("phone_live")) return 3;
    if (source == QLatin1String("live_route")) return 2;
    if (source == QLatin1String("route_backfill") || source == QLatin1String("haveibeenflocked")) return 1;
    return 0;
}

QString MapDb::resolvePlateUid(const QString &uid) const
{
    if (!m_db.isOpen() || uid.isEmpty()) return uid;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT target FROM plate_event_alias WHERE uid=?")); q.addBindValue(uid);
    return q.exec() && q.next() ? q.value(0).toString() : uid;
}

QString MapDb::mergePlateEvent(const QJsonObject &evIn, bool local, bool *created, bool *changed, QString *error)
{
    if (created) *created = false;
    if (changed) *changed = false;
    auto fail = [&](const QString &e) { if (error) *error = e; return QString(); };
    if (!m_db.isOpen() || m_readOnly) return fail(QStringLiteral("database not writable"));
    QJsonObject ev = peNormalise(evIn);
    const QString kind = ev.value(QLatin1String("kind")).toString();
    if (kind != QLatin1String("camera_pass") && kind != QLatin1String("plate_search")) return fail(QStringLiteral("kind must be camera_pass or plate_search"));
    const QString time = ev.value(QLatin1String("time")).toString();
    const QDateTime t = QDateTime::fromString(time, Qt::ISODate);
    if (!t.isValid()) return fail(QStringLiteral("time required (ISO 8601)"));
    if (ev.value(QLatin1String("source")).toString().isEmpty()) return fail(QStringLiteral("source required"));
    QString uid = ev.value(QLatin1String("uid")).toString();
    const QString cam = ev.value(QLatin1String("camera_id")).toString();
    if (kind == QLatin1String("camera_pass")) {
        if (cam.isEmpty()) return fail(QStringLiteral("camera_id required for a camera_pass"));
        if (uid.isEmpty()) uid = PlateEvents::passUid(cam, t.toMSecsSinceEpoch());
    } else if (uid.isEmpty()) {
        if (!ev.value(QLatin1String("raw")).isObject()) return fail(QStringLiteral("uid or raw required for a plate_search"));
        uid = PlateEvents::searchUid(ev.value(QLatin1String("raw")).toObject());
    }
    ev["uid"] = uid;
    // the row it merges into: the same uid (or one merged away before), else the same camera within ±10 min
    QString target = resolvePlateUid(uid);
    QJsonObject ex;
    {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral("SELECT * FROM plate_events WHERE uid=?")); q.addBindValue(target);
        if (q.exec() && q.next()) ex = peRow(q, true);
    }
    if (ex.isEmpty() && kind == QLatin1String("camera_pass")) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral("SELECT * FROM plate_events WHERE kind='camera_pass' AND camera_id=? AND time BETWEEN ? AND ?"));
        q.addBindValue(cam); q.addBindValue(t.addSecs(-600).toString(Qt::ISODate)); q.addBindValue(t.addSecs(600).toString(Qt::ISODate));
        qint64 best = std::numeric_limits<qint64>::max();
        if (q.exec()) while (q.next()) {
            const QJsonObject r = peRow(q, true);
            const qint64 d = std::llabs(QDateTime::fromString(r["time"].toString(), Qt::ISODate).secsTo(t));
            if (d <= 600 && d < best) { best = d; ex = r; }
        }
    }
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    if (ex.isEmpty()) {
        QStringList cols{QStringLiteral("uid")}, marks{QStringLiteral("?")};
        QVariantList vals{uid};
        for (const char *c : PE_COLS) {
            const QString col = QLatin1String(c);
            QJsonValue v = ev.value(col);
            if (col == QLatin1String("device") && v.isUndefined()) v = QString();
            if (col == QLatin1String("leaky") && v.isUndefined()) v = 0;
            cols << col; marks << QStringLiteral("?"); vals << peBind(col, v);
        }
        cols << QStringLiteral("created_at") << QStringLiteral("updated_at") << QStringLiteral("seq");
        marks << QStringLiteral("?") << QStringLiteral("?") << QStringLiteral("?");
        vals << (ev.value(QLatin1String("created_at")).toString().isEmpty() ? now : ev.value(QLatin1String("created_at")).toString()) << now << double(nextSeq());
        QSqlQuery ins(m_db);
        ins.prepare(QStringLiteral("INSERT INTO plate_events(%1) VALUES(%2)").arg(cols.join(QLatin1Char(',')), marks.join(QLatin1Char(','))));
        for (const QVariant &v : std::as_const(vals)) ins.addBindValue(v);
        if (!ins.exec()) return fail(ins.lastError().text());
        if (created) *created = true;
        if (changed) *changed = true;
        if (kind == QLatin1String("camera_pass")) recountPasses({cam});
        markDirty();
        return uid;
    }
    // merge into ex
    const QString exUid = ex["uid"].toString();
    const QString inSrc = ev.value(QLatin1String("source")).toString(), exSrc = ex["source"].toString();
    // our own re-run, or the device that recorded it updating its record: replaced as a whole
    const QString inDev = ev.value(QLatin1String("device")).toString(), exDev = ex["device"].toString();
    const bool sameLocal = (local && exDev.isEmpty() && inSrc == exSrc) || (!local && !inDev.isEmpty() && inDev == exDev && inSrc == exSrc);
    const int inRank = peRank(inSrc), exRank = peRank(exSrc);
    const int inConf = ev.value(QLatin1String("confidence")).toInt(-1), exConf = ex["confidence"].toInt(-1);
    const bool better = sameLocal || inRank > exRank || (inRank == exRank && inConf > exConf);
    static const QSet<QString> measured{QStringLiteral("time"), QStringLiteral("lat"), QStringLiteral("lon"), QStringLiteral("acc"), QStringLiteral("distance_m"),
                                        QStringLiteral("speed_kmh"), QStringLiteral("heading_deg"), QStringLiteral("approach_bearing_deg"), QStringLiteral("camera_dir_deg"),
                                        QStringLiteral("facing"), QStringLiteral("confidence"), QStringLiteral("details"), QStringLiteral("metrics"), QStringLiteral("source"),
                                        QStringLiteral("device"), QStringLiteral("camera_type"), QStringLiteral("leaky")};
    QJsonObject next = ex;
    for (const char *c : PE_COLS) {
        const QString col = QLatin1String(c);
        if (!ev.contains(col)) continue;
        const QJsonValue v = ev.value(col);
        const QJsonValue cur = ex.value(col);
        const bool curEmpty = cur.isNull() || cur.isUndefined() || (cur.isString() && cur.toString().isEmpty()) || (cur.isObject() && cur.toObject().isEmpty());
        const bool vEmpty = v.isNull() || v.isUndefined() || (v.isString() && v.toString().isEmpty()) || (v.isObject() && v.toObject().isEmpty());
        if (col == QLatin1String("kind")) continue;
        if (sameLocal || (better && !vEmpty && measured.contains(col))) next[col] = v;   // our own re-run: as computed now; a better source: its numbers
        else if (curEmpty && !vEmpty) next[col] = v;                                     // otherwise only what we did not know
    }
    bool diff = false;
    for (const char *c : PE_COLS) {
        const QString col = QLatin1String(c);
        if (peBind(col, next.value(col)) != peBind(col, ex.value(col))) { diff = true; break; }
    }
    if (uid != exUid) {                                   // remember the merged-away uid (media posted with it land here)
        QSqlQuery a(m_db);
        a.prepare(QStringLiteral("INSERT OR REPLACE INTO plate_event_alias(uid, target) VALUES(?,?)")); a.addBindValue(uid); a.addBindValue(exUid); a.exec();
        QSqlQuery m(m_db);
        m.prepare(QStringLiteral("UPDATE plate_event_media SET event_uid=? WHERE event_uid=?")); m.addBindValue(exUid); m.addBindValue(uid); m.exec();
    }
    if (diff) {
        QStringList sets; QVariantList vals;
        for (const char *c : PE_COLS) { sets << QStringLiteral("%1=?").arg(QLatin1String(c)); vals << peBind(QLatin1String(c), next.value(QLatin1String(c))); }
        sets << QStringLiteral("updated_at=?") << QStringLiteral("seq=?");
        vals << now << double(nextSeq());
        QSqlQuery up(m_db);
        up.prepare(QStringLiteral("UPDATE plate_events SET %1 WHERE uid=?").arg(sets.join(QLatin1Char(','))));
        for (const QVariant &v : std::as_const(vals)) up.addBindValue(v);
        up.addBindValue(exUid);
        if (!up.exec()) return fail(up.lastError().text());
        if (changed) *changed = true;
        markDirty();
    }
    return exUid;
}

QJsonArray MapDb::plateEventMedia(const QString &eventUid, const QString &cameraId, bool includeLocal) const
{
    QJsonArray out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT uid, kind, mime, width, height, bytes, attribution, license, original_url, captured_at, jpeg_reconstructible, original_mime, original_bytes"
                             " FROM plate_event_media WHERE ((event_uid=? AND event_uid<>'') OR (camera_id=? AND camera_id<>'' AND (event_uid IS NULL OR event_uid='')))%1"
                             " ORDER BY CASE kind WHEN 'dashcam' THEN 0 WHEN 'webcam' THEN 1 ELSE 2 END, id").arg(includeLocal ? QString() : QStringLiteral(" AND kind<>'webcam'")));
    q.addBindValue(eventUid); q.addBindValue(cameraId.isEmpty() ? QStringLiteral("\x01") : cameraId);
    if (q.exec()) while (q.next()) {
        QJsonObject m{{"uid", q.value(0).toString()}, {"kind", q.value(1).toString()}, {"mime", q.value(2).toString()}, {"width", q.value(3).toInt()},
                      {"height", q.value(4).toInt()}, {"bytes", q.value(5).toDouble()}, {"attribution", q.value(6).toString()}, {"license", q.value(7).toString()}};
        if (!q.value(8).toString().isEmpty()) m["originalUrl"] = q.value(8).toString();
        if (!q.value(9).toString().isEmpty()) m["capturedAt"] = q.value(9).toString();
        m["jpegReconstructible"] = q.value(10).toInt() != 0;
        if (!q.value(11).toString().isEmpty()) { m["originalMime"] = q.value(11).toString(); m["originalBytes"] = q.value(12).toDouble(); }
        out.append(m);
    }
    return out;
}

QJsonObject MapDb::plateEvent(const QString &uidIn, bool withRaw, bool withMedia) const
{
    if (!m_db.isOpen()) return {};
    const QString uid = resolvePlateUid(uidIn);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT * FROM plate_events WHERE uid=?")); q.addBindValue(uid);
    if (!q.exec() || !q.next()) return {};
    QJsonObject o = peRow(q, withRaw);
    if (withMedia) o["media"] = plateEventMedia(uid, o["kind"].toString() == QLatin1String("camera_pass") ? o["camera_id"].toString() : QString());
    return o;
}

QJsonArray MapDb::plateEventsSince(qint64 since, int limit, const QString &kind, bool *more, qint64 *cursor) const
{
    QJsonArray out;
    if (more) *more = false;
    if (cursor) *cursor = since;
    if (!m_db.isOpen()) return out;
    limit = qBound(1, limit, 2000);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT * FROM plate_events WHERE seq>?%1 ORDER BY seq LIMIT ?").arg(kind.isEmpty() ? QString() : QStringLiteral(" AND kind=?")));
    q.addBindValue(double(since));
    if (!kind.isEmpty()) q.addBindValue(kind);
    q.addBindValue(limit + 1);
    qint64 last = since;
    if (q.exec()) while (q.next()) {
        if (out.size() >= limit) { if (more) *more = true; break; }
        QJsonObject o = peRow(q, false);
        o["media"] = plateEventMedia(o["uid"].toString(), o["kind"].toString() == QLatin1String("camera_pass") ? o["camera_id"].toString() : QString(), false);   // webcam stills stay here (§2.0)
        last = qint64(o["seq"].toDouble());
        out.append(o);
    }
    if (cursor) *cursor = (more && *more) ? last : qMax(last, since);
    return out;
}

QJsonArray MapDb::plateEventsLatest(int limit, const QString &kind, bool withMedia) const
{
    QJsonArray out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT * FROM plate_events%1 ORDER BY time DESC, id DESC LIMIT ?").arg(kind.isEmpty() ? QString() : QStringLiteral(" WHERE kind=?")));
    if (!kind.isEmpty()) q.addBindValue(kind);
    q.addBindValue(qBound(1, limit, 100000));
    if (q.exec()) while (q.next()) {
        QJsonObject o = peRow(q, false);
        if (withMedia) o["media"] = plateEventMedia(o["uid"].toString(), o["kind"].toString() == QLatin1String("camera_pass") ? o["camera_id"].toString() : QString());
        out.append(o);
    }
    return out;
}

QJsonObject MapDb::plateEventCounts() const
{
    QJsonObject o{{"total", 0}, {"cameraPass", 0}, {"alprPass", 0}, {"cameraOnlyPass", 0}, {"plateSearch", 0}, {"media", 0}, {"leaky", 0}};
    if (!m_db.isOpen()) return o;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT COUNT(*), SUM(kind='camera_pass'), SUM(kind='camera_pass' AND camera_type='alpr'), SUM(kind='camera_pass' AND camera_type<>'alpr'),"
                          " SUM(kind='plate_search'), SUM(kind='camera_pass' AND leaky=1), MAX(time) FROM plate_events"));
    if (q.next()) {
        o["total"] = q.value(0).toInt(); o["cameraPass"] = q.value(1).toInt(); o["alprPass"] = q.value(2).toInt(); o["cameraOnlyPass"] = q.value(3).toInt();
        o["plateSearch"] = q.value(4).toInt(); o["leaky"] = q.value(5).toInt(); o["latest"] = q.value(6).isNull() ? QJsonValue() : QJsonValue(q.value(6).toString());
    }
    QJsonObject byType;
    q.exec(QStringLiteral("SELECT COALESCE(camera_type,''), COUNT(*) FROM plate_events WHERE kind='camera_pass' GROUP BY 1"));
    while (q.next()) byType[q.value(0).toString()] = q.value(1).toInt();
    o["passesByType"] = byType;
    q.exec(QStringLiteral("SELECT COUNT(*), COALESCE(SUM(bytes),0), COALESCE(SUM(original_bytes),0) FROM plate_event_media"));
    if (q.next()) { o["media"] = q.value(0).toInt(); o["mediaBytes"] = q.value(1).toDouble(); o["mediaOriginalBytes"] = q.value(2).toDouble(); }
    return o;
}

bool MapDb::storeMedia(MediaRow m, const QByteArray &data)
{
    if (!m_db.isOpen() || m_readOnly || data.isEmpty()) return false;
    m.uid = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(32));
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO plate_event_media(uid, event_uid, camera_id, kind, mime, data, width, height, bytes, original_url, original_mime,"
                             " original_bytes, original_sha256, jpeg_reconstructible, attribution, license, captured_at, created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    for (const QVariant &v : QVariantList{m.uid, m.eventUid, m.cameraId, m.kind, m.mime, data, m.width, m.height, double(data.size()), m.originalUrl, m.originalMime,
                                           double(m.originalBytes), m.originalSha256, m.jpegReconstructible ? 1 : 0, m.attribution, m.license, m.capturedAt,
                                           QDateTime::currentDateTime().toString(Qt::ISODate)})
        q.addBindValue(v);
    if (!q.exec()) return false;
    if (q.numRowsAffected() > 0) {
        // an event's media changed: the feed carries the event again (its media list)
        QSqlQuery s(m_db);
        if (!m.eventUid.isEmpty()) { s.prepare(QStringLiteral("UPDATE plate_events SET seq=? WHERE uid=?")); s.addBindValue(double(nextSeq())); s.addBindValue(m.eventUid); s.exec(); }
        markDirty();
    }
    return true;
}

QByteArray MapDb::mediaData(const QString &uid, MediaRow *meta) const
{
    if (!m_db.isOpen()) return {};
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT data, event_uid, camera_id, kind, mime, width, height, bytes, original_url, original_mime, original_bytes, original_sha256,"
                             " jpeg_reconstructible, attribution, license, captured_at, created_at FROM plate_event_media WHERE uid=?"));
    q.addBindValue(uid);
    if (!q.exec() || !q.next()) return {};
    if (meta) {
        meta->uid = uid; meta->eventUid = q.value(1).toString(); meta->cameraId = q.value(2).toString(); meta->kind = q.value(3).toString(); meta->mime = q.value(4).toString();
        meta->width = q.value(5).toInt(); meta->height = q.value(6).toInt(); meta->bytes = q.value(7).toLongLong(); meta->originalUrl = q.value(8).toString();
        meta->originalMime = q.value(9).toString(); meta->originalBytes = q.value(10).toLongLong(); meta->originalSha256 = q.value(11).toString();
        meta->jpegReconstructible = q.value(12).toInt() != 0; meta->attribution = q.value(13).toString(); meta->license = q.value(14).toString();
        meta->capturedAt = q.value(15).toString(); meta->createdAt = q.value(16).toString();
    }
    return q.value(0).toByteArray();
}

int MapDb::cameraMediaCount(const QString &cameraId, const QString &kind) const
{
    if (!m_db.isOpen()) return 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM plate_event_media WHERE camera_id=?%1").arg(kind.isEmpty() ? QString() : QStringLiteral(" AND kind=?")));
    q.addBindValue(cameraId);
    if (!kind.isEmpty()) q.addBindValue(kind);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

void MapDb::setCameraClassification(const QString &cameraId, const QString &type, const QString &tagsJson)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery u(m_db);
    u.prepare(QStringLiteral("UPDATE flock_cameras SET camera_type=?, tags=CASE WHEN ?<>'' THEN ? ELSE tags END WHERE id=?"));
    u.addBindValue(type); u.addBindValue(tagsJson); u.addBindValue(tagsJson); u.addBindValue(cameraId); u.exec();
    // its passes: type, confidence (from the ALPR-formula value kept in metrics), details, the OSM tags in raw
    const QJsonObject tags = QJsonDocument::fromJson(tagsJson.toUtf8()).object();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT uid, camera_type, confidence, metrics, raw, operator, model, distance_m, facing FROM plate_events WHERE kind='camera_pass' AND camera_id=?"));
    q.addBindValue(cameraId);
    struct Up { QString uid, type, details, raw; int conf; };
    QList<Up> ups;
    if (q.exec()) while (q.next()) {
        const QJsonObject metrics = QJsonDocument::fromJson(q.value(3).toString().toUtf8()).object();
        QJsonObject raw = QJsonDocument::fromJson(q.value(4).toString().toUtf8()).object();
        const int alprConf = metrics.value(QLatin1String("confidenceAlpr")).toInt(q.value(2).toInt());
        const int conf = type == QLatin1String("alpr") ? alprConf : qMin(40, alprConf);
        if (!tags.isEmpty()) raw["osmTags"] = tags;
        const QString rawText = QString::fromUtf8(QJsonDocument(raw).toJson(QJsonDocument::Compact));
        const QString details = PlateEvents::passDetails(type, q.value(5).toString(), q.value(6).toString(), q.value(7).toDouble(), q.value(8).isNull() ? -1 : q.value(8).toInt());
        if (q.value(1).toString() == type && q.value(2).toInt() == conf && q.value(4).toString() == rawText) continue;
        ups.append({q.value(0).toString(), type, details, rawText, conf});
    }
    for (const Up &x : std::as_const(ups)) {
        QSqlQuery w(m_db);
        w.prepare(QStringLiteral("UPDATE plate_events SET camera_type=?, confidence=?, details=?, raw=?, updated_at=?, seq=? WHERE uid=?"));
        w.addBindValue(x.type); w.addBindValue(x.conf); w.addBindValue(x.details); w.addBindValue(x.raw);
        w.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate)); w.addBindValue(double(nextSeq())); w.addBindValue(x.uid);
        w.exec();
    }
    markDirty();
}

int MapDb::updateLeakyPasses(const QJsonArray &agencies)
{
    if (!m_db.isOpen() || m_readOnly) return 0;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT e.uid, e.operator, e.leaky, e.metrics, c.notes, c.tags FROM plate_events e LEFT JOIN flock_cameras c ON c.id=e.camera_id WHERE e.kind='camera_pass'"));
    struct Up { QString uid; int leaky; QString metrics; };
    QList<Up> ups;
    while (q.next()) {
        const QJsonObject tags = QJsonDocument::fromJson(q.value(5).toString().toUtf8()).object();
        QString state = tags.value(QLatin1String("addr:state")).toString();
        if (state.size() != 2) state = Hibf::stateFromText(q.value(4).toString());
        const QString match = Hibf::leakyMatch(q.value(1).toString(), state.toUpper(), agencies);
        const int leaky = match.isEmpty() ? 0 : 1;
        QJsonObject m = QJsonDocument::fromJson(q.value(3).toString().toUtf8()).object();
        if (leaky == q.value(2).toInt() && m.value(QLatin1String("leakyMatch")).toString() == match) continue;
        if (leaky) m["leakyMatch"] = match; else m.remove(QStringLiteral("leakyMatch"));
        ups.append({q.value(0).toString(), leaky, QString::fromUtf8(QJsonDocument(m).toJson(QJsonDocument::Compact))});
    }
    m_db.transaction();
    for (const Up &x : std::as_const(ups)) {
        QSqlQuery w(m_db);
        w.prepare(QStringLiteral("UPDATE plate_events SET leaky=?, metrics=?, seq=? WHERE uid=?"));
        w.addBindValue(x.leaky); w.addBindValue(x.metrics); w.addBindValue(double(nextSeq())); w.addBindValue(x.uid); w.exec();
    }
    m_db.commit();
    if (!ups.isEmpty()) markDirty();
    return int(ups.size());
}

void MapDb::recountPasses(const QStringList &cameraIds)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db);
    if (cameraIds.isEmpty()) {
        q.exec(QStringLiteral("UPDATE flock_cameras SET pass_count=(SELECT COUNT(*) FROM plate_events e WHERE e.kind='camera_pass' AND e.camera_id=flock_cameras.id)"
                              " WHERE pass_count>0 OR id IN (SELECT camera_id FROM plate_events WHERE kind='camera_pass')"));
    } else {
        q.prepare(QStringLiteral("UPDATE flock_cameras SET pass_count=(SELECT COUNT(*) FROM plate_events e WHERE e.kind='camera_pass' AND e.camera_id=?) WHERE id=?"));
        for (const QString &id : cameraIds) { q.addBindValue(id); q.addBindValue(id); q.exec(); }
    }
    markDirty();
}

// Teleport spikes dropped per device (the route filter's > 38 m/s within 5 min), accuracy ≤ 500 m
QList<PlateEvents::TrackFix> MapDb::trackFixes(const QString &fromIso, const QString &toIso, const QString &device) const
{
    QList<PlateEvents::TrackFix> out;
    if (!m_db.isOpen()) return out;
    QString where = QStringLiteral("acc IS NOT NULL AND acc>=0 AND acc<=500 AND lat IS NOT NULL AND NOT (lat=0 AND lon=0)");
    QVariantList binds;
    if (!fromIso.isEmpty()) { where += QStringLiteral(" AND time>=?"); binds << fromIso; }
    if (!toIso.isEmpty()) { where += QStringLiteral(" AND time<=?"); binds << toIso; }
    if (device != QLatin1String("*")) { where += QStringLiteral(" AND COALESCE(device,'')=?"); binds << device; }
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT time, lat, lon, acc, source, provider, COALESCE(device,'') FROM fixes WHERE %1 ORDER BY time ASC").arg(where));
    for (const QVariant &b : std::as_const(binds)) q.addBindValue(b);
    if (!q.exec()) return out;
    QHash<QString, PlateEvents::TrackFix> last;
    while (q.next()) {
        const QDateTime t = QDateTime::fromString(q.value(0).toString(), Qt::ISODate);
        if (!t.isValid()) continue;
        PlateEvents::TrackFix f;
        f.ms = t.toMSecsSinceEpoch(); f.lat = q.value(1).toDouble(); f.lon = q.value(2).toDouble(); f.acc = q.value(3).toDouble();
        f.source = q.value(4).toString();
        if (!q.value(5).toString().isEmpty()) f.source += QLatin1Char('/') + q.value(5).toString();
        f.device = q.value(6).toString();
        const auto it = last.constFind(f.device);
        if (it != last.constEnd() && routeJump(it->ms, it->lat, it->lon, f.ms, f.lat, f.lon)) continue;
        last[f.device] = f;
        out << f;
    }
    return out;
}

QHash<QString, QPair<QString, QString>> MapDb::newFixRanges(qint64 sinceSeq, qint64 *maxSeq) const
{
    QHash<QString, QPair<QString, QString>> out;
    if (maxSeq) *maxSeq = sinceSeq;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT COALESCE(device,''), MIN(time), MAX(time), MAX(seq) FROM fixes WHERE seq>? GROUP BY COALESCE(device,'')"));
    q.addBindValue(double(sinceSeq));
    if (q.exec()) while (q.next()) {
        out.insert(q.value(0).toString(), qMakePair(q.value(1).toString(), q.value(2).toString()));
        if (maxSeq) *maxSeq = qMax(*maxSeq, q.value(3).toLongLong());
    }
    return out;
}

QString MapDb::firstFixTime() const
{
    if (!m_db.isOpen()) return {};
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT MIN(time) FROM fixes WHERE acc<=500"));
    return q.next() ? q.value(0).toString() : QString();
}

QList<FlockCamera> MapDb::camerasNearTrack(const QList<PlateEvents::TrackFix> &track, double padM) const
{
    QList<FlockCamera> out;
    if (!m_db.isOpen() || track.isEmpty()) return out;
    QSet<QString> seen;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT %1 FROM flock_cameras WHERE lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?").arg(QLatin1String(FLOCK_COLS)));
    // per device, consecutive fixes grouped into boxes of ≤ ~2 km, each padded by padM
    QHash<QString, QList<const PlateEvents::TrackFix *>> byDev;
    for (const PlateEvents::TrackFix &f : track) byDev[f.device].append(&f);
    const double padLat = padM / 111194.93;
    auto query = [&](double la0, double la1, double lo0, double lo1) {
        const double padLon = padM / (111194.93 * std::max(0.2, std::cos(qDegreesToRadians((la0 + la1) / 2))));
        q.addBindValue(la0 - padLat); q.addBindValue(la1 + padLat); q.addBindValue(lo0 - padLon); q.addBindValue(lo1 + padLon);
        if (!q.exec()) return;
        while (q.next()) {
            const QString id = q.value(0).toString();
            if (seen.contains(id)) continue;
            seen.insert(id);
            out << flockRow(q);
        }
    };
    for (auto it = byDev.cbegin(); it != byDev.cend(); ++it) {
        const auto &pts = *it;
        double la0 = 0, la1 = 0, lo0 = 0, lo1 = 0; bool open = false;
        const PlateEvents::TrackFix *prev = nullptr;
        for (const PlateEvents::TrackFix *f : pts) {
            if (!open) { la0 = la1 = f->lat; lo0 = lo1 = f->lon; open = true; if (prev) { la0 = qMin(la0, prev->lat); la1 = qMax(la1, prev->lat); lo0 = qMin(lo0, prev->lon); lo1 = qMax(lo1, prev->lon); } }
            const double nla0 = qMin(la0, f->lat), nla1 = qMax(la1, f->lat), nlo0 = qMin(lo0, f->lon), nlo1 = qMax(lo1, f->lon);
            if (nla1 - nla0 > 0.02 || nlo1 - nlo0 > 0.025) {          // the box would grow past ~2 km: query it, start the next with the link
                query(la0, la1, lo0, lo1);
                la0 = qMin(prev ? prev->lat : f->lat, f->lat); la1 = qMax(prev ? prev->lat : f->lat, f->lat);
                lo0 = qMin(prev ? prev->lon : f->lon, f->lon); lo1 = qMax(prev ? prev->lon : f->lon, f->lon);
            } else { la0 = nla0; la1 = nla1; lo0 = nlo0; lo1 = nlo1; }
            prev = f;
        }
        if (open) query(la0, la1, lo0, lo1);
    }
    return out;
}

FlockCamera MapDb::flockCamera(const QString &id, bool *found) const
{
    if (found) *found = false;
    if (!m_db.isOpen()) return {};
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM flock_cameras WHERE id=?").arg(QLatin1String(FLOCK_COLS)));
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return {};
    if (found) *found = true;
    return flockRow(q);
}

// ── Camera trust, road snapping, agency portals (docs/SIGHTINGS.md §2.6, §2.7, §4.6) ──
static QJsonObject jsonObj(const QString &s) { return QJsonDocument::fromJson(s.toUtf8()).object(); }
static QString jsonText(const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

MapDb::CameraExtra MapDb::cameraExtra(const QString &id) const
{
    CameraExtra x;
    if (!m_db.isOpen()) return x;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT trust, COALESCE(trust_detail,''), COALESCE(verdict,''), COALESCE(verdict_at,''), COALESCE(watched_way,0),"
                             " COALESCE(watched_detail,''), COALESCE(ways_fetched,''), COALESCE(agency_portal,'') FROM flock_cameras WHERE id=?"));
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return x;
    x.hasTrust = !q.value(0).isNull();
    if (x.hasTrust) x.trust = q.value(0).toDouble();
    x.trustDetail = jsonObj(q.value(1).toString());
    x.verdict = q.value(2).toString(); x.verdictAt = q.value(3).toString();
    x.watchedWay = q.value(4).toLongLong(); x.watchedDetail = jsonObj(q.value(5).toString());
    x.waysFetched = q.value(6).toString(); x.agencyPortal = q.value(7).toString();
    return x;
}

void MapDb::setCameraTrust(const QString &id, double trust, const QJsonObject &detail)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE flock_cameras SET trust=?, trust_detail=? WHERE id=?"));
    q.addBindValue(trust); q.addBindValue(jsonText(detail)); q.addBindValue(id);
    if (q.exec() && q.numRowsAffected() > 0) markDirty();
}

void MapDb::setCameraVerdict(const QString &id, const QString &verdict)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE flock_cameras SET verdict=?, verdict_at=?, trust=NULL, seq=? WHERE id=?"));
    q.addBindValue(verdict); q.addBindValue(verdict.isEmpty() ? QString() : QDateTime::currentDateTime().toString(Qt::ISODate));
    q.addBindValue(double(nextSeq())); q.addBindValue(id);
    if (q.exec() && q.numRowsAffected() > 0) markDirty();
}

void MapDb::setCameraRoads(const QString &id, const QString &fetchedIso, qint64 watchedWay, const QJsonObject &watchedDetail)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE flock_cameras SET ways_fetched=?, watched_way=?, watched_detail=? WHERE id=?"));
    q.addBindValue(fetchedIso); q.addBindValue(double(watchedWay)); q.addBindValue(jsonText(watchedDetail)); q.addBindValue(id);
    if (q.exec() && q.numRowsAffected() > 0) markDirty();
}

void MapDb::setCameraAgency(const QString &id, const QString &slug)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE flock_cameras SET agency_portal=? WHERE id=? AND COALESCE(agency_portal,'')<>?"));
    q.addBindValue(slug); q.addBindValue(id); q.addBindValue(slug);
    if (q.exec() && q.numRowsAffected() > 0) markDirty();
}

QList<FlockCamera> MapDb::rfDetectionsNear(double lat, double lon, double radiusM) const
{
    QList<FlockCamera> out;
    const double dLat = radiusM / 111194.93, dLon = radiusM / (111194.93 * std::max(0.01, std::cos(qDegreesToRadians(lat))));
    for (const FlockCamera &c : loadFlockCamerasIn(lat - dLat, lat + dLat, lon - dLon, lon + dLon)) {
        if (Locator::distanceM(lat, lon, c.lat, c.lon) > radiusM) continue;
        const bool det = c.id.startsWith(QLatin1String("det:")) && c.cameraType == QLatin1String("alpr") && !c.stale;
        const bool confirmed = c.vetted && c.notes.contains(QLatin1String("Field confirmed")) && (!c.bssid.isEmpty() || !c.bleMac.isEmpty());
        if (det || confirmed) out << c;
    }
    return out;
}

void MapDb::storeWays(const QList<RoadSnap::Way> &ways, const QString &fetchedIso)
{
    if (!m_db.isOpen() || m_readOnly || ways.isEmpty()) return;
    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO osm_ways(id, highway, name, ref, layer, bridge, tunnel, oneway, nodes, geom, min_lat, max_lat, min_lon, max_lon, fetched)"
                             " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    for (const RoadSnap::Way &w : ways) {
        double a = 90, b = -90, c = 180, d = -180;
        for (const RoadSnap::Pt &p : w.pts) { a = std::min(a, p.lat); b = std::max(b, p.lat); c = std::min(c, p.lon); d = std::max(d, p.lon); }
        const QJsonObject j = RoadSnap::toJson(w);
        q.addBindValue(double(w.id)); q.addBindValue(w.highway); q.addBindValue(w.name); q.addBindValue(w.ref); q.addBindValue(w.layer);
        q.addBindValue(w.bridge ? 1 : 0); q.addBindValue(w.tunnel ? 1 : 0); q.addBindValue(w.oneway);
        q.addBindValue(QString::fromUtf8(QJsonDocument(j.value(QLatin1String("nodes")).toArray()).toJson(QJsonDocument::Compact)));
        q.addBindValue(QString::fromUtf8(QJsonDocument(j.value(QLatin1String("geom")).toArray()).toJson(QJsonDocument::Compact)));
        q.addBindValue(a); q.addBindValue(b); q.addBindValue(c); q.addBindValue(d); q.addBindValue(fetchedIso);
        q.exec();
    }
    m_db.commit();
    markDirty();
}

QList<RoadSnap::Way> MapDb::waysNear(double lat, double lon, double radiusM) const
{
    QList<RoadSnap::Way> out;
    if (!m_db.isOpen()) return out;
    const double dLat = radiusM / 111194.93, dLon = radiusM / (111194.93 * std::max(0.01, std::cos(qDegreesToRadians(lat))));
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, highway, name, ref, layer, bridge, tunnel, oneway, nodes, geom FROM osm_ways"
                             " WHERE min_lat<=? AND max_lat>=? AND min_lon<=? AND max_lon>=? ORDER BY id"));
    q.addBindValue(lat + dLat); q.addBindValue(lat - dLat); q.addBindValue(lon + dLon); q.addBindValue(lon - dLon);
    if (!q.exec()) return out;
    while (q.next()) {
        QJsonObject o{{"id", q.value(0).toDouble()}, {"highway", q.value(1).toString()}, {"name", q.value(2).toString()}, {"ref", q.value(3).toString()},
                      {"layer", q.value(4).toInt()}, {"bridge", q.value(5).toInt() != 0}, {"tunnel", q.value(6).toInt() != 0}, {"oneway", q.value(7).toInt()},
                      {"nodes", QJsonDocument::fromJson(q.value(8).toString().toUtf8()).array()}, {"geom", QJsonDocument::fromJson(q.value(9).toString().toUtf8()).array()}};
        out << RoadSnap::fromJson(o);
    }
    return out;
}

QJsonArray MapDb::passesOfCamera(const QString &cameraId) const
{
    QJsonArray out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT * FROM plate_events WHERE kind='camera_pass' AND camera_id=? ORDER BY time"));
    q.addBindValue(cameraId);
    if (q.exec()) while (q.next()) out.append(peRow(q, false));
    return out;
}

QStringList MapDb::camerasWithPasses() const
{
    QStringList out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT DISTINCT camera_id FROM plate_events WHERE kind='camera_pass' AND COALESCE(camera_id,'')<>''"));
    while (q.next()) out << q.value(0).toString();
    return out;
}

bool MapDb::updatePassScore(const QString &uid, int confidence, const QJsonObject &metrics)
{
    if (!m_db.isOpen() || m_readOnly) return false;
    const QString m = jsonText(metrics);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE plate_events SET confidence=?, metrics=?, updated_at=?, seq=? WHERE uid=? AND (COALESCE(confidence,-1)<>? OR COALESCE(metrics,'')<>?)"));
    q.addBindValue(confidence); q.addBindValue(m); q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate)); q.addBindValue(double(m_seq + 1));
    q.addBindValue(uid); q.addBindValue(confidence); q.addBindValue(m);
    if (!q.exec() || q.numRowsAffected() <= 0) return false;
    nextSeq();                                           // the row took m_seq + 1: claim it
    markDirty();
    return true;
}

int MapDb::storePortals(const QList<EyesOnFlock::Portal> &portals, const QString &fetchedIso)
{
    if (!m_db.isOpen() || m_readOnly || portals.isEmpty()) return 0;
    m_db.transaction();
    QSqlQuery d(m_db); d.exec(QStringLiteral("DELETE FROM eof_portals"));
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO eof_portals(slug, url, city, county, state, type, population, cameras, searches, retention_days, vehicles,"
                             " hotlist_hits, hotlist_rate, shared_with, received_from, prohibited_uses, public_audit, updated, tokens, fetched)"
                             " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    int n = 0;
    auto num = [](double v) { return v < 0 ? QVariant() : QVariant(v); };
    for (const EyesOnFlock::Portal &p : portals) {
        for (const QVariant &v : QVariantList{p.slug, p.url, p.city, p.county, p.state, p.type, num(p.population), num(p.cameras), num(p.searches),
                                              num(p.retentionDays), num(p.vehicles), num(p.hotlistHits), num(p.hotlistRate),
                                              p.sharedWith < 0 ? QVariant() : QVariant(p.sharedWith), p.receivedFrom < 0 ? QVariant() : QVariant(p.receivedFrom),
                                              p.prohibitedUses, p.publicAudit ? 1 : 0, p.updated, p.tokens.join(QLatin1Char(' ')), fetchedIso})
            q.addBindValue(v);
        if (q.exec()) ++n;
    }
    m_db.commit();
    markDirty();
    return n;
}

QList<EyesOnFlock::Portal> MapDb::loadPortals() const
{
    QList<EyesOnFlock::Portal> out;
    if (!m_db.isOpen()) return out;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.exec(QStringLiteral("SELECT slug, url, city, county, state, type, population, cameras, searches, retention_days, vehicles, hotlist_hits, hotlist_rate,"
                          " shared_with, received_from, prohibited_uses, public_audit, updated, tokens FROM eof_portals ORDER BY slug"));
    auto num = [&](int i) { return q.value(i).isNull() ? -1.0 : q.value(i).toDouble(); };
    while (q.next()) {
        EyesOnFlock::Portal p;
        p.slug = q.value(0).toString(); p.url = q.value(1).toString(); p.city = q.value(2).toString(); p.county = q.value(3).toString();
        p.state = q.value(4).toString(); p.type = q.value(5).toString();
        p.population = num(6); p.cameras = num(7); p.searches = num(8); p.retentionDays = num(9); p.vehicles = num(10); p.hotlistHits = num(11); p.hotlistRate = num(12);
        p.sharedWith = q.value(13).isNull() ? -1 : q.value(13).toInt(); p.receivedFrom = q.value(14).isNull() ? -1 : q.value(14).toInt();
        p.prohibitedUses = q.value(15).toString(); p.publicAudit = q.value(16).toInt() != 0; p.updated = q.value(17).toString();
        p.tokens = q.value(18).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        out << p;
    }
    return out;
}

QString MapDb::latestRegion() const
{
    if (!m_db.isOpen()) return {};
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT region FROM fixes WHERE COALESCE(region,'')<>'' ORDER BY time DESC LIMIT 1"));
    return q.next() ? q.value(0).toString() : QString();
}
