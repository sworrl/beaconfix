#include "securechannel.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

namespace Bfs3 {

static const unsigned char *u(const QByteArray &b) { return reinterpret_cast<const unsigned char *>(b.constData()); }

QByteArray b64u(const QByteArray &b) { return b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals); }

QByteArray unb64u(const QByteArray &s, bool *ok)
{
    const auto r = QByteArray::fromBase64Encoding(s.trimmed(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (ok) *ok = bool(r);
    return r ? *r : QByteArray();
}

QByteArray u64be(quint64 x) { QByteArray b(8, 0); for (int i = 7; i >= 0; --i) { b[i] = char(x & 0xff); x >>= 8; } return b; }

QByteArray random(int n)
{
    QByteArray b(n, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(b.data()), n) != 1) return {};
    return b;
}

bool constantTimeEqual(const QByteArray &a, const QByteArray &b)
{
    return a.size() == b.size() && CRYPTO_memcmp(a.constData(), b.constData(), size_t(a.size())) == 0;
}

// ── X25519 ───────────────────────────────────────────────────────────────────
QByteArray x25519Public(const QByteArray &sk)
{
    if (sk.size() != 32) return {};
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, u(sk), 32);
    if (!k) return {};
    QByteArray pub(32, 0); size_t len = 32;
    const bool ok = EVP_PKEY_get_raw_public_key(k, reinterpret_cast<unsigned char *>(pub.data()), &len) == 1 && len == 32;
    EVP_PKEY_free(k);
    return ok ? pub : QByteArray();
}

QByteArray x25519Generate(QByteArray *pub)
{
    const QByteArray sk = random(32);
    if (sk.isEmpty()) return {};
    const QByteArray p = x25519Public(sk);
    if (p.isEmpty()) return {};
    if (pub) *pub = p;
    return sk;
}

QByteArray x25519(const QByteArray &sk, const QByteArray &peerPub)
{
    if (sk.size() != 32 || peerPub.size() != 32) return {};
    EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, u(sk), 32);
    EVP_PKEY *theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, u(peerPub), 32);
    EVP_PKEY_CTX *ctx = mine ? EVP_PKEY_CTX_new(mine, nullptr) : nullptr;
    QByteArray out(32, 0); size_t len = 32;
    const bool ok = ctx && theirs && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, theirs) == 1
                 && EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(out.data()), &len) == 1 && len == 32;   // fails on an all-zero result
    EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(theirs); EVP_PKEY_free(mine);
    return ok ? out : QByteArray();
}

// ── HKDF / HMAC ──────────────────────────────────────────────────────────────
QByteArray hmacSha512(const QByteArray &key, const QByteArray &msg)
{
    QByteArray out(64, 0); unsigned int len = 64;
    static const unsigned char zero = 0;
    if (!HMAC(EVP_sha512(), key.isEmpty() ? &zero : u(key), key.size(), u(msg), size_t(msg.size()), reinterpret_cast<unsigned char *>(out.data()), &len) || len != 64) return {};
    return out;
}

QString hmacHex(const QByteArray &key, const QByteArray &msg) { return QString::fromLatin1(hmacSha512(key, msg).toHex()); }

// RFC 5869 with SHA-512; an empty salt is HashLen zeros (what HMAC does with an empty key anyway)
QByteArray hkdf(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int len)
{
    if (len <= 0 || len > 255 * 64) return {};
    const QByteArray prk = hmacSha512(salt.isEmpty() ? QByteArray(64, 0) : salt, ikm);
    QByteArray okm, t;
    for (int i = 1; okm.size() < len; ++i) { t = hmacSha512(prk, t + info + char(i)); okm += t; }
    return okm.left(len);
}

static QByteArray sha256(const QByteArray &b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha256); }

QString fingerprint(const QByteArray &serverPub) { return QString::fromLatin1(sha256(serverPub).toHex().left(32)); }
QString deviceId(const QByteArray &devicePub)    { return QStringLiteral("d") + QString::fromLatin1(sha256(devicePub).toHex().left(24)); }

QByteArray rootKey(const QByteArray &shared, const QByteArray &serverPub, const QByteArray &devicePub)
{
    return hkdf(shared, sha256(QByteArrayLiteral("bfs3") + serverPub + devicePub), QByteArrayLiteral("beaconfix.bfs3.root"));
}

QString enrollMac(const QByteArray &inviteSecret, const QString &inviteId, const QString &name, const QString &kind, const QString &pubB64u, qint64 ts)
{
    return hmacHex(inviteSecret, QStringLiteral("bfs3-enroll\n%1\n%2\n%3\n%4\n%5").arg(inviteId, name, kind, pubB64u).arg(ts).toUtf8());
}

QString proof(const QByteArray &rk, const QString &did) { return hmacHex(rk, QStringLiteral("bfs3-enrolled\n").toUtf8() + did.toUtf8()); }

QByteArray requestKey(const QByteArray &rk, quint64 c)  { return hkdf(rk, {}, QByteArrayLiteral("bfs3 req") + u64be(c)); }
QByteArray responseKey(const QByteArray &rk, quint64 c) { return hkdf(rk, {}, QByteArrayLiteral("bfs3 resp") + u64be(c)); }
QByteArray eventKey(const QByteArray &rk, quint64 c, quint64 seq) { return hkdf(rk, {}, QByteArrayLiteral("bfs3 evt") + u64be(c) + u64be(seq)); }

QByteArray requestAad(const QByteArray &method, const QByteArray &target, const QString &did, quint64 c, qint64 ts)
{
    return "bfs3\n" + method + "\n" + target + "\n" + did.toUtf8() + "\n" + QByteArray::number(c) + "\n" + QByteArray::number(ts);
}
QByteArray responseAad(int status, const QString &did, quint64 c) { return "bfs3-resp\n" + QByteArray::number(status) + "\n" + did.toUtf8() + "\n" + QByteArray::number(c); }
QByteArray eventAad(const QString &did, quint64 c, quint64 seq)   { return "bfs3-evt\n" + did.toUtf8() + "\n" + QByteArray::number(c) + "\n" + QByteArray::number(seq); }

// ── ChaCha20-Poly1305 ────────────────────────────────────────────────────────
bool aeadSeal(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad, const QByteArray &plain, QByteArray *out)
{
    if (key.size() != 32 || nonce.size() != 12) return false;
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); if (!c) return false;
    QByteArray ct(plain.size() + 16, 0); int len = 0, total = 0; bool ok = false;
    do {
        if (EVP_EncryptInit_ex(c, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1) break;
        if (EVP_EncryptInit_ex(c, nullptr, nullptr, u(key), u(nonce)) != 1) break;
        if (!aad.isEmpty() && EVP_EncryptUpdate(c, nullptr, &len, u(aad), aad.size()) != 1) break;
        if (!plain.isEmpty() && EVP_EncryptUpdate(c, reinterpret_cast<unsigned char *>(ct.data()), &len, u(plain), plain.size()) != 1) break;
        total = plain.isEmpty() ? 0 : len;
        if (EVP_EncryptFinal_ex(c, reinterpret_cast<unsigned char *>(ct.data()) + total, &len) != 1) break;
        total += len;
        if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, ct.data() + total) != 1) break;
        ok = total == plain.size();
    } while (false);
    EVP_CIPHER_CTX_free(c);
    if (ok) *out = ct;
    return ok;
}

bool aeadOpen(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad, const QByteArray &sealed, QByteArray *plain)
{
    if (key.size() != 32 || nonce.size() != 12 || sealed.size() < 16) return false;
    const int n = sealed.size() - 16;
    QByteArray tag = sealed.right(16), pt(n, 0);
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); if (!c) return false;
    int len = 0, total = 0; bool ok = false;
    do {
        if (EVP_DecryptInit_ex(c, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1) break;
        if (EVP_DecryptInit_ex(c, nullptr, nullptr, u(key), u(nonce)) != 1) break;
        if (!aad.isEmpty() && EVP_DecryptUpdate(c, nullptr, &len, u(aad), aad.size()) != 1) break;
        if (n > 0 && EVP_DecryptUpdate(c, reinterpret_cast<unsigned char *>(pt.data()), &len, u(sealed), n) != 1) break;
        total = n > 0 ? len : 0;
        if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 16, tag.data()) != 1) break;
        if (EVP_DecryptFinal_ex(c, reinterpret_cast<unsigned char *>(pt.data()) + total, &len) != 1) break;   // the tag check
        ok = total + len == n;
    } while (false);
    EVP_CIPHER_CTX_free(c);
    if (ok) *plain = pt;
    return ok;
}

QByteArray sealRequest(const QByteArray &rk, const QString &did, const QByteArray &method, const QByteArray &target, quint64 c, qint64 ts, const QByteArray &nonce, const QByteArray &plain)
{
    QByteArray out; aeadSeal(requestKey(rk, c), nonce, requestAad(method, target, did, c, ts), plain, &out); return out;
}
bool openRequest(const QByteArray &rk, const QString &did, const QByteArray &method, const QByteArray &target, quint64 c, qint64 ts, const QByteArray &nonce, const QByteArray &sealed, QByteArray *plain)
{
    return aeadOpen(requestKey(rk, c), nonce, requestAad(method, target, did, c, ts), sealed, plain);
}
QByteArray sealResponse(const QByteArray &rk, const QString &did, int status, quint64 c, const QByteArray &nonce, const QByteArray &plain)
{
    QByteArray out; aeadSeal(responseKey(rk, c), nonce, responseAad(status, did, c), plain, &out); return out;
}
bool openResponse(const QByteArray &rk, const QString &did, int status, quint64 c, const QByteArray &nonce, const QByteArray &sealed, QByteArray *plain)
{
    return aeadOpen(responseKey(rk, c), nonce, responseAad(status, did, c), sealed, plain);
}
QByteArray sealEvent(const QByteArray &rk, const QString &did, quint64 c, quint64 seq, const QByteArray &nonce, const QByteArray &plain)
{
    QByteArray out; if (!aeadSeal(eventKey(rk, c, seq), nonce, eventAad(did, c, seq), plain, &out)) return {};
    return b64u(nonce + out);
}
bool openEvent(const QByteArray &rk, const QString &did, quint64 c, quint64 seq, const QByteArray &line, QByteArray *plain)
{
    bool ok = false; const QByteArray raw = unb64u(line, &ok);
    if (!ok || raw.size() < 12 + 16) return false;
    return aeadOpen(eventKey(rk, c, seq), raw.left(12), eventAad(did, c, seq), raw.mid(12), plain);
}

// ── Invite ───────────────────────────────────────────────────────────────────
QString Invite::encode() const
{
    // Key order u, s, i, k, e (as the reference prints it); readers must not depend on it
    QByteArray us = QJsonDocument(QJsonArray{url}).toJson(QJsonDocument::Compact);   // ["…"]: the JSON-quoted url
    us = us.mid(1, us.size() - 2);
    const QByteArray j = "{\"u\":" + us + ",\"s\":\"" + b64u(serverPub) + "\",\"i\":\"" + id.toUtf8() + "\",\"k\":\"" + b64u(secret) + "\",\"e\":" + QByteArray::number(expires) + "}";
    return QStringLiteral("bfs3:") + QString::fromLatin1(b64u(j));
}

bool Invite::decode(const QString &text, Invite *out)
{
    QString t = text.trimmed();
    if (!t.startsWith(QLatin1String("bfs3:"), Qt::CaseInsensitive)) return false;
    bool ok = false; const QByteArray j = unb64u(t.mid(5).toLatin1(), &ok);
    if (!ok) return false;
    const QJsonObject o = QJsonDocument::fromJson(j).object();
    Invite v; v.url = o["u"].toString(); v.id = o["i"].toString(); v.expires = qint64(o["e"].toDouble());
    v.serverPub = unb64u(o["s"].toString().toLatin1(), &ok); if (!ok) return false;
    v.secret = unb64u(o["k"].toString().toLatin1(), &ok); if (!ok) return false;
    if (v.url.isEmpty() || v.id.isEmpty() || v.serverPub.size() != 32 || v.secret.size() < 16) return false;
    *out = v;
    return true;
}

// ── Replay window ────────────────────────────────────────────────────────────
bool ReplayWindow::fresh(quint64 c) const
{
    if (c == 0) return false;
    if (c > m_high) return true;
    const quint64 i = m_high - c;
    if (i >= 128) return false;                          // c ≤ H − 128
    return !((i < 64 ? m_lo >> i : m_hi >> (i - 64)) & 1);
}

void ReplayWindow::accept(quint64 c)
{
    if (c == 0) return;
    if (c > m_high) {
        const quint64 s = c - m_high;
        if (s >= 128) { m_lo = m_hi = 0; }
        else if (s >= 64) { m_hi = m_lo << (s - 64); m_lo = 0; }
        else { m_hi = (m_hi << s) | (m_lo >> (64 - s)); m_lo <<= s; }
        m_high = c; m_lo |= 1;
        return;
    }
    const quint64 i = m_high - c;
    if (i < 64) m_lo |= quint64(1) << i; else if (i < 128) m_hi |= quint64(1) << (i - 64);
}

QString ReplayWindow::save() const { return QStringLiteral("%1:%2:%3").arg(m_high).arg(m_lo, 0, 16).arg(m_hi, 0, 16); }

ReplayWindow ReplayWindow::load(const QString &s)
{
    ReplayWindow w; const QStringList p = s.split(QLatin1Char(':'));
    if (p.size() == 3) { w.m_high = p[0].toULongLong(); w.m_lo = p[1].toULongLong(nullptr, 16); w.m_hi = p[2].toULongLong(nullptr, 16); }
    return w;
}

// ── At rest ──────────────────────────────────────────────────────────────────
QByteArray sealAtRest(const QByteArray &dbKey, const QByteArray &label, const QByteArray &plain)
{
    if (dbKey.size() != 32) return {};
    const QByteArray k = hkdf(dbKey, QByteArrayLiteral("beaconfix.bfs3.rest"), label), nonce = random(12);
    if (nonce.isEmpty()) return {};
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); if (!c) return {};
    QByteArray ct(plain.size() + 16, 0); int len = 0, total = 0; bool ok = false;
    do {
        if (EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_EncryptInit_ex(c, nullptr, nullptr, u(k), u(nonce)) != 1) break;
        if (EVP_EncryptUpdate(c, nullptr, &len, u(label), label.size()) != 1) break;
        if (EVP_EncryptUpdate(c, reinterpret_cast<unsigned char *>(ct.data()), &len, u(plain), plain.size()) != 1) break;
        total = len;
        if (EVP_EncryptFinal_ex(c, reinterpret_cast<unsigned char *>(ct.data()) + total, &len) != 1) break;
        total += len;
        ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, ct.data() + total) == 1;
    } while (false);
    EVP_CIPHER_CTX_free(c);
    return ok ? QByteArrayLiteral("BFK1") + nonce + ct.left(total + 16) : QByteArray();
}

QByteArray openAtRest(const QByteArray &dbKey, const QByteArray &label, const QByteArray &blob)
{
    if (dbKey.size() != 32 || blob.size() < 4 + 12 + 16 || !blob.startsWith("BFK1")) return {};
    const QByteArray k = hkdf(dbKey, QByteArrayLiteral("beaconfix.bfs3.rest"), label), nonce = blob.mid(4, 12), body = blob.mid(16, blob.size() - 32);
    QByteArray tag = blob.right(16), pt(body.size(), 0);
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); if (!c) return {};
    int len = 0, total = 0; bool ok = false;
    do {
        if (EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_DecryptInit_ex(c, nullptr, nullptr, u(k), u(nonce)) != 1) break;
        if (EVP_DecryptUpdate(c, nullptr, &len, u(label), label.size()) != 1) break;
        if (!body.isEmpty() && EVP_DecryptUpdate(c, reinterpret_cast<unsigned char *>(pt.data()), &len, u(body), body.size()) != 1) break;
        total = body.isEmpty() ? 0 : len;
        if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1) break;
        ok = EVP_DecryptFinal_ex(c, reinterpret_cast<unsigned char *>(pt.data()) + total, &len) == 1;
    } while (false);
    EVP_CIPHER_CTX_free(c);
    return ok ? pt : QByteArray();
}

} // namespace Bfs3
