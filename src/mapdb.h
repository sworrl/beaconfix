#pragma once
#include "locator.h"
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
    bool    isEmpty() const;                                   // no aps and no fixes
    QJsonObject stats() const;

    // Access points + everything learned about them
    QHash<QString, ApRecord> loadApRecords(QSet<QString> *travelling, QSet<QString> *notTravelling) const;
    void saveApRecords(const QHash<QString, ApRecord> &recs, const QSet<QString> &travelling, const QSet<QString> &notTravelling,
                       const QHash<QString, int> &flags);      // flags: bit0 home, bit1 travelling, bit2 ignored
    // Trip log
    QList<Fix> loadFixes() const;
    void saveFixes(const QList<Fix> &fixes);                   // rewrite (departures change)
    void appendFix(const Fix &f);
    // Places
    bool loadPois(QList<Poi> *pois, double *lat, double *lon, int *radiusM, QDateTime *time) const;
    void savePois(const QList<Poi> &pois, double lat, double lon, int radiusM, const QDateTime &time);
    // Elevation cells
    QHash<QString, double> loadElevation() const;
    void saveElevation(const QHash<QString, double> &cells);
    // Milestones
    QHash<QString, QDateTime> loadAchievements() const;
    void saveAchievements(const QList<Achievement> &list);

    // Positions and self-location (offline: places we have been before)
    QList<ApPos> positions(const QStringList &bssids, double maxAcc = 0) const;   // all when bssids is empty
    bool estimate(const QList<QPair<QString, int>> &heard, double *lat, double *lon, double *acc, int *used, QStringList *usedBssids = nullptr,
                  int minAps = 2, double maxAcc = 150) const;
    int  addObservations(const QJsonArray &observations, QString *error = nullptr);   // from another BeaconFix / device
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
    static QByteArray encrypt(const QByteArray &key, const QByteArray &plain, QString *error);
    static QByteArray decrypt(const QByteArray &key, const QByteArray &blob, QString *error);
    static QByteArray walletKey(bool create, QString *source);
    static QByteArray fileKey(bool create, QString *source);

    QString m_stateDir, m_blobPath, m_livePath, m_error, m_keySource, m_conn;
    QByteArray m_key;
    QSqlDatabase m_db;
    bool m_readOnly = false, m_dirty = false;
    QTimer m_flushTimer;
};
