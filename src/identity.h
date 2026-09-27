#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

// BeaconFix identity (docs/IDENTITY.md, shared spec v1): an Ed25519 key pair plus a
// display name, minted in any app (desktop, Android, laptop) and moved to the others
// as an encrypted bundle (QR / text / file / LAN hand-off). Two identities created
// independently can be LINKED (merged) later; a link statement signed by both sides
// makes them one owner set. Data rows keep their original identity id and ownership
// is evaluated through the link set, so late links need no rewrite.
//
// Storage: ~/.config/sworrl/identity.json (0600). The public record is readable as
// is; the seed lives in a sealed part encrypted with the map-database key
// (AES-256-GCM, same key MapDb uses). Pending link requests are kept there too.

struct IdentityDevice {
    QString name, kind;                 // kind: desktop | android | laptop
    QDateTime added;
    QByteArray pub;                     // optional: that device's own key (for links / auth)
    QJsonObject toJson() const;
    static IdentityDevice fromJson(const QJsonObject &o);
};

// {"v":1,"a":idA,"b":idB,"ts":ISO,"sigA":b64,"sigB":b64} + optional pubA/pubB carried
// while the statement is being completed (so the other side can verify before signing).
struct LinkStatement {
    QString a, b, ts;
    QByteArray sigA, sigB, pubA, pubB;
    QByteArray canon() const;           // "beaconfix-link|v1|" + min(a,b) + "|" + max(a,b) + "|" + ts
    bool complete() const { return !sigA.isEmpty() && !sigB.isEmpty(); }
    QJsonObject toJson(bool withPubs = false) const;
    static LinkStatement fromJson(const QJsonObject &o);
};

// An identity that tried to authenticate and is not ours (yet): shown in the Devices
// tab so the user can link it.
struct PendingLink {
    QString id, name, ip, deviceName, deviceKind;
    QByteArray pub;
    QDateTime time;
    QJsonObject toJson() const;
    static PendingLink fromJson(const QJsonObject &o);
};

class Identity : public QObject {
    Q_OBJECT
public:
    explicit Identity(QObject *parent = nullptr);

    static QString filePath();
    bool    load(const QByteArray &dbKey);          // reads the file; unlocks the seed when the key fits
    bool    exists() const { return !m_id.isEmpty(); }
    bool    unlocked() const { return m_seed.size() == 32; }
    QString error() const { return m_error; }

    QString id() const { return m_id; }
    QString groupedId() const { return groupId(m_id); }
    static QString groupId(const QString &id);       // "7k3m-…" 4-char groups
    QString name() const { return m_name; }
    QByteArray pub() const { return m_pub; }
    QDateTime created() const { return m_created; }
    QList<IdentityDevice> devices() const { return m_devices; }
    QList<LinkStatement>  links() const { return m_links; }
    QStringList linkedIds() const;                   // every id reachable through valid links (not ours)
    bool    isOwner(const QString &id) const { return !id.isEmpty() && (id == m_id || linkedIds().contains(id)); }
    QJsonObject record() const;                      // the spec's record object
    QJsonObject publicJson() const;                  // GET /api/v1/identity
    QJsonObject linkPayload() const;                 // what our "link QR" carries (no secrets)

    bool    create(const QString &name, const QString &deviceName, const QString &deviceKind, QString *error);
    bool    importBundle(const QString &textOrJson, const QString &passphrase, const QString &deviceName, const QString &deviceKind, QString *error);
    QString exportBundle(const QString &passphrase, QString *error) const;     // "BFID1:" + base64url(json)
    static QString wordCode(int words = 6);          // EFF short wordlist, space separated
    bool    rename(const QString &name);
    bool    addDevice(const QString &name, const QString &kind, const QByteArray &pub = QByteArray());
    bool    forget();                                // delete the identity file ("Forget this device")

    // Signing
    QByteArray sign(const QByteArray &message) const;
    static bool verify(const QByteArray &pub, const QByteArray &message, const QByteArray &sig);
    static QString idFor(const QByteArray &pub);     // Crockford base32 of SHA-256(pub)[0..16]
    static QByteArray authCanon(const QString &host, const QByteArray &nonceB64, const QString &id, const QString &deviceName);

    // Links
    // Our half of a link statement. ts must be the ts of the link payload the OTHER device displayed (its offer):
    // that device co-signs only statements bound to an offer it showed. Empty ts (legacy) uses the current time.
    LinkStatement startLink(const QString &otherId, const QByteArray &otherPub, const QString &ts = QString()) const;
    bool    acceptLink(LinkStatement st, QString *error, LinkStatement *completed = nullptr);   // verify; co-sign if we are a party; store when complete
    bool    hasLinkWith(const QString &id) const;

    // Pending link requests (identities that failed auth)
    QList<PendingLink> pending() const { return m_pending; }
    void    addPending(const PendingLink &p);
    bool    removePending(const QString &id);

    static QJsonObject selftest();                   // fixed-seed vectors (seed = 32×0x01)
    static QByteArray base64url(const QByteArray &b);
    static QByteArray fromBase64url(const QByteArray &s);
    // Payload texts for QRs / paste boxes / the API. URIs, so a phone's camera offers to open them in the app:
    //   beaconfix://link/<b64url>  (link payload)   beaconfix://statement/<b64url>  (link statement)   beaconfix://identity/<b64url>  (export bundle)
    // decodePayload() also still reads the older BFLNK1: / BFLINK1: / BFID1: texts and raw JSON.
    static QString     encodeUri(const QString &kind, const QJsonObject &o);
    static QJsonObject decodePayload(const QString &text, QString *kind = nullptr);   // kind: link | statement | identity | json | "" (unparsable)

signals:
    void changed();

private:
    bool save();
    bool keypairFromSeed(const QByteArray &seed, QByteArray *pub) const;
    static bool aeadSeal(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad, const QByteArray &plain, QByteArray *out);
    static bool aeadOpen(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad, const QByteArray &ct, QByteArray *out);
    void    fromRecord(const QJsonObject &rec);

    QByteArray m_dbKey, m_seed, m_pub;
    QString m_id, m_name, m_error;
    QDateTime m_created;
    QList<IdentityDevice> m_devices;
    QList<LinkStatement> m_links;
    // Link QRs this process displayed: {ts nonce, expiry}. A statement that asks us to co-sign must carry one of
    // these timestamps (see acceptLink), which proves the other device saw our screen. Kept in memory only.
    mutable QList<QPair<QString, QDateTime>> m_offers;
    QList<PendingLink> m_pending;
};
