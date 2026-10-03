#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

// BFS3 (docs/SECURE-API.md, normative; reference tools/bfs3_ref.py, vectors tests/fixtures/bfs3_vectors.json):
// X25519 device keys, HKDF-SHA512, one ChaCha20-Poly1305 key per message (derived from its counter), HMAC-SHA512
// for enrolment, ±300 s time window and a 128-wide replay window. Everything here is pure (no Qt event loop, no
// I/O) so the unit test reproduces every vector byte for byte; the hub (hub.h) and the node (hubclient.h) use it.
namespace Bfs3 {

constexpr int kWindowSecs = 300;                       // |now − ts| allowed
constexpr int kInviteSecs = 900;                       // an invite is valid 15 minutes, once

QByteArray b64u(const QByteArray &b);                  // base64url, no padding
QByteArray unb64u(const QByteArray &s, bool *ok = nullptr);
QByteArray u64be(quint64 x);
QByteArray random(int n);                              // RAND_bytes; empty on failure
bool       constantTimeEqual(const QByteArray &a, const QByteArray &b);

// X25519 (RFC 7748): 32-byte raw keys
QByteArray x25519Generate(QByteArray *pub);            // a fresh private key (+ its public key)
QByteArray x25519Public(const QByteArray &sk);
QByteArray x25519(const QByteArray &sk, const QByteArray &peerPub);   // shared secret, empty on failure (incl. all-zero)

QByteArray hkdf(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int len = 32);   // HKDF-SHA512 (RFC 5869)
QByteArray hmacSha512(const QByteArray &key, const QByteArray &msg);
QString    hmacHex(const QByteArray &key, const QByteArray &msg);   // lowercase hex

QString    fingerprint(const QByteArray &serverPub);  // hex(SHA-256(S_pk))[0:32]
QString    deviceId(const QByteArray &devicePub);     // "d" + hex(SHA-256(D_pk))[0:24]
QByteArray rootKey(const QByteArray &shared, const QByteArray &serverPub, const QByteArray &devicePub);
QString    enrollMac(const QByteArray &inviteSecret, const QString &inviteId, const QString &name, const QString &kind, const QString &pubB64u, qint64 ts);
QString    proof(const QByteArray &rk, const QString &deviceId);

QByteArray requestKey(const QByteArray &rk, quint64 c);
QByteArray responseKey(const QByteArray &rk, quint64 c);
QByteArray eventKey(const QByteArray &rk, quint64 c, quint64 seq);
QByteArray requestAad(const QByteArray &method, const QByteArray &target, const QString &deviceId, quint64 c, qint64 ts);
QByteArray responseAad(int status, const QString &deviceId, quint64 c);
QByteArray eventAad(const QString &deviceId, quint64 c, quint64 seq);

// ChaCha20-Poly1305 IETF: out = ciphertext ‖ 16-byte tag
bool aeadSeal(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad, const QByteArray &plain, QByteArray *out);
bool aeadOpen(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad, const QByteArray &sealed, QByteArray *plain);

QByteArray sealRequest(const QByteArray &rk, const QString &did, const QByteArray &method, const QByteArray &target, quint64 c, qint64 ts,
                       const QByteArray &nonce, const QByteArray &plain);
bool       openRequest(const QByteArray &rk, const QString &did, const QByteArray &method, const QByteArray &target, quint64 c, qint64 ts,
                       const QByteArray &nonce, const QByteArray &sealed, QByteArray *plain);
QByteArray sealResponse(const QByteArray &rk, const QString &did, int status, quint64 c, const QByteArray &nonce, const QByteArray &plain);
bool       openResponse(const QByteArray &rk, const QString &did, int status, quint64 c, const QByteArray &nonce, const QByteArray &sealed, QByteArray *plain);
QByteArray sealEvent(const QByteArray &rk, const QString &did, quint64 c, quint64 seq, const QByteArray &nonce, const QByteArray &plain);   // the "data:" value
bool       openEvent(const QByteArray &rk, const QString &did, quint64 c, quint64 seq, const QByteArray &line, QByteArray *plain);

// "bfs3:" + b64u(JSON {"u","s","i","k","e"})
struct Invite {
    QString url, id;
    QByteArray serverPub, secret;
    qint64 expires = 0;
    QString encode() const;
    static bool decode(const QString &text, Invite *out);
};

// The hub's replay state per device: the highest counter seen H and a 128-bit bitmap below it (bit i = H − i seen).
// c ≤ H − 128, c = 0 or an already-seen c is a replay; out of order inside the window is fine.
class ReplayWindow {
public:
    bool    fresh(quint64 c) const;
    void    accept(quint64 c);                         // call only after fresh(c) and a good tag
    quint64 highest() const { return m_high; }
    QString save() const;                              // "H:lo:hi" (hex)
    static ReplayWindow load(const QString &s);
private:
    quint64 m_high = 0, m_lo = 0, m_hi = 0;            // m_lo: bits 0..63, m_hi: bits 64..127
};

// Secrets at rest (S_sk on the hub, D_sk on a node): AES-256-GCM under HKDF(dbKey, label), "BFK1" ‖ nonce ‖ ct ‖ tag
QByteArray sealAtRest(const QByteArray &dbKey, const QByteArray &label, const QByteArray &plain);
QByteArray openAtRest(const QByteArray &dbKey, const QByteArray &label, const QByteArray &blob);

} // namespace Bfs3
