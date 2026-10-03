#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// Linking v3 (docs/LINKING.md, normative; reference tools/link_ref.py, vectors tests/fixtures/link_vectors.json):
// a phone links to this PC by scanning the QR of the "Link a device" dialog (HMAC with the QR's k → approved at once)
// or by picking the PC from its mDNS list (commitment, then key, then the user taps Link on the PC). Both screens show
// the same six-digit code; the API token and the hub invite travel sealed to the session key. HKDF here is SHA-256
// (BFS3 uses SHA-512). Everything in this file is pure (time is passed in, no I/O, no event loop) so the unit test
// reproduces the vectors and drives the session rules; ApiServer owns one Link::Book and does the HTTP, the tokens,
// the hub invite and the notifications.
namespace Link {

constexpr int kQrSecs = 600;          // a QR session lives 10 min (the dialog renews it while open)
constexpr int kRequestSecs = 120;     // an mDNS request: 2 min to be revealed, shown and answered
constexpr int kDeliverSecs = 120;     // an approved / denied session waits this long for the phone's poll
constexpr int kMaxPending = 3;        // mDNS requests waiting at once
constexpr int kPerSourcePerMin = 12;  // POST /link + POST /link/<sid> per source address and minute (a bad MAC costs 3)

QByteArray hkdfSha256(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int len);   // RFC 5869
QByteArray hmacSha256(const QByteArray &key, const QByteArray &msg);
QString    qrMac(const QByteArray &k, const QString &sid, const QString &name, const QString &kind, const QString &pubB64u);   // hex
QString    commit(const QString &name, const QString &kind, const QString &pubB64u);   // hex(SHA-256("bflink-commit\n" name \n kind \n pub))
QString    code(const QByteArray &shared, const QString &sid);       // "740948"
QString    codeText(const QString &code);                            // "740 948"
QByteArray payloadKey(const QByteArray &shared, const QString &sid);
QByteArray payloadAad(const QString &sid);
QString    seal(const QByteArray &shared, const QString &sid, const QByteArray &nonce, const QByteArray &payload);   // b64u(nonce ‖ ct ‖ tag)
bool       open(const QByteArray &shared, const QString &sid, const QString &sealedB64u, QByteArray *payload);

// JSON with the keys in the order given (the reference's order: the QR and the payload are reproduced byte for byte)
QByteArray orderedJson(const QList<QPair<QString, QJsonValue>> &fields);
QString    qrText(const QString &sid, const QString &name, const QStringList &hosts, int port, const QByteArray &pub, const QByteArray &k, qint64 expires, const QString &hub);
bool       decodeQr(const QString &text, QJsonObject *out);
QByteArray payloadJson(const QString &token, const QStringList &scopes, const QString &pcName, const QString &pcId, const QStringList &hosts, int port, const QString &hub);
QStringList mdnsTxt(const QString &name, const QString &id, int port);   // the link part of the _beaconfix._tcp TXT record

struct Session {
    enum Origin { Qr, Mdns };
    // Qr: Open → (MAC ok) Approving → Approved → Delivered.  Mdns: Committed → (key revealed) Pending → (Link) Approving → Approved → Delivered.
    // Denied from Pending (Reject) or Approving (token / payload failure). Cancelled: the QR's dialog closed or it was renewed.
    enum State { Open, Committed, Pending, Approving, Approved, Denied, Delivered, Cancelled };
    QString sid;
    Origin origin = Qr;
    State state = Open;
    QByteArray sk, pub, k;            // our X25519 pair for this session; k: the QR's MAC key
    QString name, kind, ip, commitHex;
    QByteArray theirPub, shared;
    QString code;                     // six digits once both keys are known
    qint64 created = 0, expires = 0;  // unix seconds
    QString hub;                      // the hub invite of the QR / the payload ("" = none)
    QString sealed;                   // approved: the sealed payload, handed out once
    QString reason;                   // denied
    QString deviceId;                 // the ApiServer device the token went to
    QJsonObject proximity;            // mDNS: the verdict shown as information
    bool live() const { return state != Delivered && state != Cancelled; }
    QString stateName() const;
};

class Book {
public:
    // A QR session (fresh sid, X25519 pair, k). nullptr only if the RNG fails.
    Session *offer(qint64 now, const QString &hub = QString());
    // POST /api/v1/link. Returns the HTTP status and fills out (the answer body). On 202 *sid names the session;
    // a QR request that verifies is left Approving: the caller issues the token and calls approve() before replying.
    int  request(const QJsonObject &body, const QString &ip, qint64 nowMs, QJsonObject *out, QString *sid);
    // POST /api/v1/link/<sid> (mDNS): the phone reveals its key; it must match the commitment → Pending (code known)
    int  reveal(const QString &sid, const QJsonObject &body, const QString &ip, qint64 nowMs, QJsonObject *out);
    // GET /api/v1/link/<sid>: pending | denied | approved (+ sealed, once — then Delivered)
    int  poll(const QString &sid, qint64 now, QJsonObject *out);
    bool startApproval(const QString &sid, qint64 now);                         // Pending → Approving (the user tapped Link)
    bool approve(const QString &sid, const QByteArray &payload, qint64 now, const QByteArray &nonce = QByteArray());   // Approving → Approved
    bool deny(const QString &sid, const QString &reason, qint64 now);          // Pending / Approving → Denied
    bool cancel(const QString &sid);                                            // an unused QR / a waiting request → Cancelled
    bool setHub(const QString &sid, const QString &hub);                        // an Open QR session's invite (arrived late)
    Session       *find(const QString &sid);
    const Session *find(const QString &sid) const;
    QList<Session> sessions() const { return m_s; }
    int  pendingCount(qint64 now) const;                                        // mDNS requests not yet answered
    bool sweep(qint64 now, QStringList *orphans = nullptr);                     // drop expired / closed ones (orphans: devices of approved, never fetched ones)
    bool allow(const QString &ip, qint64 nowMs, int cost = 1);                  // the per-source budget (also spends it)

private:
    QList<Session> m_s;
    QHash<QString, QList<qint64>> m_hits;
};

} // namespace Link
