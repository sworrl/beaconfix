#pragma once
#include "locator.h"
#include "ranging/anchors.h"
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QTimer>

// The internal mapping database: everything BeaconFix has learned, in one SQLite
// file — access points and where they are, every observation (where we stood, how
// loud), the trip log, places, elevation cells, milestones.
//
// Encryption at rest. This box has no SQLCipher, so the database is encrypted by
// BeaconFix itself: the file at ~/.local/state/beaconfix/beaconfix.db is an
// AES-256-GCM blob (OpenSSL EVP; header "BFDB\1", 12-byte nonce, ciphertext, 16-byte
// tag). While the tray runs, the plaintext copy lives in the private, tmpfs-backed
// runtime directory ($XDG_RUNTIME_DIR/beaconfix/live.db, mode 0600) and is
// re-encrypted back to the blob a few seconds after every change and on exit.
// The 256-bit key comes from KWallet (folder "BeaconFix", entry "mapdb-key") when
// the wallet can be opened, otherwise from ~/.config/sworrl/beaconfix.key (0600).
// A standalone process (--json, --once) opens a read-only decrypted copy of its own
// and never writes.
class MapDb : public QObject {
    Q_OBJECT
public:
    struct ApPos { QString bssid, ssid; double lat = 0, lon = 0, acc = 0; QString source; int home = 0, travelling = 0, ignored = 0; };

    explicit MapDb(const QString &stateDir, QObject *parent = nullptr);
    ~MapDb() override;

    bool    open(bool readOnly = false);
    bool    isOpen() const { return m_db.isOpen(); }
    bool    readOnly() const { return m_readOnly; }
    QString error() const { return m_error; }
    QString path() const { return m_blobPath; }              // the encrypted file
    QString keySource() const { return m_keySource; }          // "kwallet" | "keyfile"
    QByteArray key() const { return m_key; }                   // the 256-bit key (also seals the identity file)
    static QByteArray bootstrapKey(QString *source);           // the key, creating the key file if there is none yet (CLI before the first tray start)
    bool    isEmpty() const;                                   // no aps and no fixes
    QJsonObject stats() const;

    // Access points + everything learned about them
    QHash<QString, ApRecord> loadApRecords(QSet<QString> *travelling, QSet<QString> *notTravelling) const;   // incl. fits + peer positions
    void saveApRecords(QHash<QString, ApRecord> &recs, const QSet<QString> &travelling, const QSet<QString> &notTravelling,
                       const QHash<QString, int> &flags);      // flags: bit0 home, bit1 travelling, bit2 ignored; assigns observation ids
    void saveRecord(const QString &bssid, ApRecord &r, int flags);   // one record (new / changed observations only)
    void saveEstimate(const QString &bssid, const Estimator::Fit &fit);
    void appendEstimateHistory(const QString &bssid, const Estimator::Fit &fit);   // the last 20 per beacon
    QJsonArray estimateHistory(const QString &bssid) const;
    // Cells (~15 m) this host scanned from: where a beacon was NOT heard (docs/GRADING.md §1.7)
    struct ScanCellRow { QString key; double lat = 0, lon = 0; int count = 0; qint64 first = 0, last = 0; };
    QList<ScanCellRow> loadScanCells() const;
    void saveScanCell(const ScanCellRow &c);
    // Change sequence for sync: every stored/changed AP position, observation and fix gets the next number
    qint64 currentSeq() const { return m_seq; }
    QJsonObject changesSince(qint64 since, int limit, bool *more, qint64 *cursor) const;
    QString kv(const QString &key) const;
    void    setKv(const QString &key, const QString &value);
    // Trip log
    QList<Fix> loadFixes() const;
    void saveFixes(const QList<Fix> &fixes);                   // rewrite (departures change)
    void appendFix(const Fix &f);
    // Places. scope "near": the main query around the fix (kv poi_*); "far": the pediatric ER search (kv peds_*).
    // Both share the table: a place in both lists is stored once, as near (the merged view prefers near anyway).
    bool loadPois(QList<Poi> *pois, double *lat, double *lon, int *radiusM, QDateTime *time, const QString &scope = QStringLiteral("near")) const;
    void savePois(const QList<Poi> &pois, double lat, double lon, int radiusM, const QDateTime &time, const QString &scope = QStringLiteral("near"));
    // Elevation cells
    QHash<QString, double> loadElevation() const;
    void saveElevation(const QHash<QString, double> &cells);
    // Milestones
    QHash<QString, QDateTime> loadAchievements() const;
    void saveAchievements(const QList<Achievement> &list);

    // Positions and self-location (offline: places we have been before)
    QList<ApPos> positions(const QStringList &bssids, double maxAcc = 0) const;   // all when bssids is empty
    // GET /api/v1/aps?all=1: every positioned beacon, bssid order, keyset-paged (after = last bssid of the previous page)
    QJsonArray positionsPage(const QString &after, int limit, QString *next) const;
    void setPins(const QHash<QString, ApPos> &pins) { m_pins = pins; }             // anchored BSSIDs (upper-case): override stored positions
    bool estimate(const QList<QPair<QString, int>> &heard, double *lat, double *lon, double *acc, int *used, QStringList *usedBssids = nullptr,
                  int minAps = 2, double maxAcc = 150) const;
    int  addObservations(const QJsonArray &observations, const QString &device, QString *error = nullptr,
                         QHash<QString, QList<ApObservation>> *added = nullptr);   // from another BeaconFix / device; dedup (bssid, time, device)
    int  mergePeerAps(const QJsonArray &aps, const QString &device, QStringList *touched = nullptr);   // positions another device worked out
    int  appendPeerFixes(const QJsonArray &fixes, const QString &device);
    QJsonArray latestFixesByDevice() const;              // newest fix per peer device (device != "")
    QList<Fix> peerFixes(const QString &device = QString()) const;
    // Anchors (docs/RANGING.md §4): surveyed transmitters/places, synced through /db/changes like everything else
    QList<Anchors::Anchor> loadAnchors(bool includeDeleted = false) const;
    // Store one anchor (or tombstone). force = a local edit (always wins, new seq); otherwise a synced row that
    // only replaces what we have when it is newer (placedAt / deletedAt). Returns true when the table changed.
    bool putAnchor(Anchors::Anchor a, bool force, Anchors::Anchor *stored = nullptr);
    QJsonObject exportJson() const;
    int  importJson(const QJsonObject &dump, QString *error = nullptr);

    void flush();                                              // encrypt + write now (if dirty)
    static QString runtimeDir();

signals:
    void changed();

private:
    bool ensureKey(bool create);
    bool decryptToLive(bool readOnly);
    bool schema();
    void markDirty();
    void updatePosition(const QString &bssid, const ApRecord &r, int flags);
    qint64 nextSeq();
    void   loadSeq();
    static QByteArray encrypt(const QByteArray &key, const QByteArray &plain, QString *error);
    static QByteArray decrypt(const QByteArray &key, const QByteArray &blob, QString *error);
    static QByteArray walletKey(bool create, QString *source);
    static QByteArray fileKey(bool create, QString *source);

    QString m_stateDir, m_blobPath, m_livePath, m_error, m_keySource, m_conn;
    QByteArray m_key;
    QSqlDatabase m_db;
    bool m_readOnly = false, m_dirty = false;
    bool m_batch = false;                 // inside saveApRecords: markDirty() only sets the flag
    qint64 m_seq = 0;
    QTimer m_flushTimer;
    mutable QHash<QString, quint64> m_recSig;
    quint64 m_flagsSig = 0;
    QHash<QString, ApPos> m_pins;   // per-record signature of what is stored: saveApRecords skips unchanged records
    static quint64 recordSignature(const ApRecord &r, int flags);
};
