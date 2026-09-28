#include "mapdb.h"
#include "fitjson.h"
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
#include <QSqlDriver>
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
    for (const char *t : {"aps", "observations", "fixes", "estimates", "anchors"}) {
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
    q.exec(QStringLiteral("INSERT OR IGNORE INTO kv(key, value) VALUES ('schema', '3')"));
    q.exec(QStringLiteral("UPDATE kv SET value='3' WHERE key='schema'"));
    q.exec(QStringLiteral("INSERT OR IGNORE INTO kv(key, value) VALUES ('created', '%1')").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    return true;
}

void MapDb::markDirty()
{
    if (m_readOnly) return;
    m_dirty = true;
    if (m_batch) return;                                   // saveApRecords: one changed() for the whole batch, not one per record
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
    q.exec(QStringLiteral("SELECT bssid, time, lat, lon, acc, dbm, id, device FROM observations ORDER BY id"));
    while (q.next()) {
        auto it = out.find(q.value(0).toString()); if (it == out.end()) continue;
        ApObservation o; o.time = QDateTime::fromString(q.value(1).toString(), Qt::ISODate);
        o.lat = q.value(2).toDouble(); o.lon = q.value(3).toDouble(); o.acc = q.value(4).toDouble(); o.dbm = q.value(5).toInt();
        o.id = q.value(6).toLongLong(); o.device = q.value(7).toString();
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
    markDirty();
}

void MapDb::appendFix(const Fix &f)
{
    if (!m_db.isOpen() || m_readOnly) return;
    QSqlQuery q(m_db);
    q.prepare(QString::fromLatin1(FIX_INSERT));
    bindFix(q, f); q.addBindValue(double(nextSeq())); q.addBindValue(QString()); q.exec();
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
        dup.addBindValue(device); dup.addBindValue(o["time"].toString()); dup.exec();
        if (dup.next()) continue;
        Fix f = Fix::fromJson(o); f.valid = true;
        if (f.accuracy < 0) f.accuracy = o["acc"].toDouble(-1);
        bindFix(q, f); q.addBindValue(double(nextSeq())); q.addBindValue(device);
        if (q.exec()) ++n;
    }
    m_db.commit();
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
    QSqlQuery ap(m_db), q(m_db);                    // prepared once: an import pushes hundreds of thousands of rows through here
    ap.prepare(QStringLiteral("INSERT INTO aps(bssid, ssid, freq, first_seen, last_seen, times_seen, seq) VALUES(?,?,?,?,?,1,?) ON CONFLICT(bssid) DO UPDATE SET last_seen=excluded.last_seen, times_seen=aps.times_seen+1,"
                              " ssid=CASE WHEN excluded.ssid<>'' THEN excluded.ssid ELSE aps.ssid END, freq=CASE WHEN excluded.freq>0 THEN excluded.freq ELSE aps.freq END"));
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO observations(bssid, time, lat, lon, acc, dbm, fix_source, device, seq) VALUES(?,?,?,?,?,?,?,?,?)"));
    for (const QJsonValue &v : observations) {
        const QJsonObject o = v.toObject();
        const QString bssid = o["bssid"].toString().toUpper().trimmed();
        if (bssid.size() != 17 || !o["lat"].isDouble() || !o["lon"].isDouble()) continue;
        const double acc = o["acc"].toDouble(100);
        if (acc <= 0 || acc > 2000) continue;
        const QString dev = o["device"].toString().isEmpty() ? device : o["device"].toString();
        const QString t = o["time"].toString().isEmpty() ? QDateTime::currentDateTime().toString(Qt::ISODate) : o["time"].toString();
        ap.addBindValue(bssid); ap.addBindValue(o["ssid"].toString()); ap.addBindValue(o["freq"].toInt(0)); ap.addBindValue(t); ap.addBindValue(t); ap.addBindValue(double(nextSeq())); ap.exec();
        q.addBindValue(bssid); q.addBindValue(t); q.addBindValue(o["lat"].toDouble()); q.addBindValue(o["lon"].toDouble()); q.addBindValue(acc);
        q.addBindValue(o["dbm"].toInt(-80)); q.addBindValue(o["source"].toString().isEmpty() ? QStringLiteral("remote") : o["source"].toString()); q.addBindValue(dev); q.addBindValue(double(nextSeq()));
        if (!q.exec() || q.numRowsAffected() <= 0) continue;   // duplicate (bssid, time, device)
        ApObservation ob; ob.id = q.lastInsertId().toLongLong(); ob.time = QDateTime::fromString(t, Qt::ISODate); ob.lat = o["lat"].toDouble(); ob.lon = o["lon"].toDouble(); ob.acc = acc; ob.dbm = o["dbm"].toInt(-80); ob.device = dev;
        if (added) (*added)[bssid].append(ob);
        ++n;
    }
    m_db.commit();
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
    q.prepare(QStringLiteral("SELECT bssid, time, lat, lon, acc, dbm, device, seq FROM observations WHERE seq>? ORDER BY seq LIMIT ?")); q.addBindValue(double(since)); q.addBindValue(limit + 1); q.exec();
    while (q.next()) {
        if (obs.size() >= limit) { if (more) *more = true; break; }
        obs.append(QJsonObject{{"bssid", q.value(0).toString()}, {"time", q.value(1).toString()}, {"lat", q.value(2).toDouble()}, {"lon", q.value(3).toDouble()}, {"acc", q.value(4).toDouble()},
                               {"dbm", q.value(5).toInt()}, {"device", q.value(6).toString()}, {"seq", q.value(7).toDouble()}});
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
    // The cursor a client should store: when a table hit the cap, the smallest "next" seq across tables keeps ordering safe
    qint64 next = maxSeq;
    if (more && *more) {
        next = m_seq;
        for (const QJsonArray *arr : {&aps, &obs, &fixes, &anchors}) if (arr->size() >= limit) next = qMin(next, qint64(arr->last().toObject()["seq"].toDouble()));
        // every table is complete up to `next` only if the others have no rows in (since, next] beyond what we returned — they were read fully or capped at ≥ next
    }
    if (cursor) *cursor = (more && *more) ? next : m_seq;
    out["since"] = double(since); out["aps"] = aps; out["observations"] = obs; out["fixes"] = fixes; out["anchors"] = anchors; out["count"] = total;
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
    insertRows(QStringLiteral("pois"), dump["pois"].toArray(), true);
    insertRows(QStringLiteral("pois_far"), dump["poisFar"].toArray(), true);
    insertRows(QStringLiteral("elevation"), dump["elevation"].toArray(), true);
    insertRows(QStringLiteral("achievements"), dump["achievements"].toArray(), true);
    insertRows(QStringLiteral("estimates"), dump["estimates"].toArray(), true);
    insertRows(QStringLiteral("estimate_history"), dump["estimateHistory"].toArray(), true);
    insertRows(QStringLiteral("scan_cells"), dump["scanCells"].toArray(), true);
    m_db.commit();
    for (const QJsonValue &v : dump["anchors"].toArray()) if (putAnchor(Anchors::Anchor::fromJson(v.toObject()), false)) ++n;
    if (n) markDirty();
    return n;
}
