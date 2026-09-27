#include "identity.h"
#include "effwords.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>

// ── encoding helpers ──────────────────────────────────────────────────────────
static const char *CROCKFORD = "0123456789abcdefghjkmnpqrstvwxyz";

static QString crockford32(const QByteArray &bytes)
{
    QString out; int buffer = 0, bits = 0;
    for (unsigned char c : bytes) {
        buffer = (buffer << 8) | c; bits += 8;
        while (bits >= 5) { out += QLatin1Char(CROCKFORD[(buffer >> (bits - 5)) & 31]); bits -= 5; }
    }
    if (bits > 0) out += QLatin1Char(CROCKFORD[(buffer << (5 - bits)) & 31]);
    return out;
}

QByteArray Identity::base64url(const QByteArray &b) { return b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals); }
QByteArray Identity::fromBase64url(const QByteArray &s) { return QByteArray::fromBase64(s, QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals); }

static QByteArray randomBytes(int n)
{
    QByteArray b(n, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(b.data()), n) != 1) b.clear();
    return b;
}

QString Identity::idFor(const QByteArray &pub)
{
    if (pub.size() != 32) return {};
    return crockford32(QCryptographicHash::hash(pub, QCryptographicHash::Sha256).left(16));
}

QString Identity::groupId(const QString &id)
{
    QString out;
    for (int i = 0; i < id.size(); i += 4) { if (!out.isEmpty()) out += QLatin1Char('-'); out += id.mid(i, 4); }
    return out;
}

// ── Ed25519 ───────────────────────────────────────────────────────────────────
bool Identity::keypairFromSeed(const QByteArray &seed, QByteArray *pub) const
{
    if (seed.size() != 32) return false;
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(seed.constData()), 32);
    if (!k) return false;
    size_t len = 32; pub->resize(32);
    const bool ok = EVP_PKEY_get_raw_public_key(k, reinterpret_cast<unsigned char *>(pub->data()), &len) == 1 && len == 32;
    EVP_PKEY_free(k);
    return ok;
}

static QByteArray ed25519Sign(const QByteArray &seed, const QByteArray &msg)
{
    QByteArray sig;
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(seed.constData()), 32);
    if (!k) return sig;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    size_t len = 64; sig.resize(64);
    if (!ctx || EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, k) != 1
        || EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(sig.data()), &len, reinterpret_cast<const unsigned char *>(msg.constData()), size_t(msg.size())) != 1
        || len != 64)
        sig.clear();
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(k);
    return sig;
}

bool Identity::verify(const QByteArray &pub, const QByteArray &message, const QByteArray &sig)
{
    if (pub.size() != 32 || sig.size() != 64) return false;
    EVP_PKEY *k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(pub.constData()), 32);
    if (!k) return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const bool ok = ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, k) == 1
                    && EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char *>(sig.constData()), 64,
                                        reinterpret_cast<const unsigned char *>(message.constData()), size_t(message.size())) == 1;
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(k);
    return ok;
}

QByteArray Identity::sign(const QByteArray &message) const { return unlocked() ? ed25519Sign(m_seed, message) : QByteArray(); }

QByteArray Identity::authCanon(const QString &host, const QByteArray &nonceB64, const QString &id, const QString &deviceName)
{
    return "beaconfix-auth|v1|" + host.toUtf8() + "|" + nonceB64 + "|" + id.toUtf8() + "|" + deviceName.toUtf8();
}

// ── AES-256-GCM with AAD ──────────────────────────────────────────────────────
bool Identity::aeadSeal(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad, const QByteArray &plain, QByteArray *out)
{
    if (key.size() != 32 || nonce.size() != 12) return false;
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); if (!c) return false;
    QByteArray ct(plain.size() + 16, 0); int len = 0, total = 0; bool ok = false;
    do {
        if (EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) break;
        if (EVP_EncryptInit_ex(c, nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(nonce.constData())) != 1) break;
        if (!aad.isEmpty() && EVP_EncryptUpdate(c, nullptr, &len, reinterpret_cast<const unsigned char *>(aad.constData()), aad.size()) != 1) break;
        if (EVP_EncryptUpdate(c, reinterpret_cast<unsigned char *>(ct.data()), &len, reinterpret_cast<const unsigned char *>(plain.constData()), plain.size()) != 1) break;
        total = len;
        if (EVP_EncryptFinal_ex(c, reinterpret_cast<unsigned char *>(ct.data()) + total, &len) != 1) break;
        total += len;
        if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, ct.data() + total) != 1) break;
        ct.resize(total + 16); *out = ct; ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(c);
    return ok;
}

bool Identity::aeadOpen(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad, const QByteArray &ct, QByteArray *out)
{
    if (key.size() != 32 || nonce.size() != 12 || ct.size() < 16) return false;
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); if (!c) return false;
    const int n = ct.size() - 16;
    QByteArray plain(n + 1, 0); int len = 0, total = 0; bool ok = false;
    QByteArray tag = ct.right(16);
    do {
        if (EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) break;
        if (EVP_DecryptInit_ex(c, nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(nonce.constData())) != 1) break;
        if (!aad.isEmpty() && EVP_DecryptUpdate(c, nullptr, &len, reinterpret_cast<const unsigned char *>(aad.constData()), aad.size()) != 1) break;
        if (n > 0 && EVP_DecryptUpdate(c, reinterpret_cast<unsigned char *>(plain.data()), &len, reinterpret_cast<const unsigned char *>(ct.constData()), n) != 1) break;
        total = len;
        if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1) break;
        if (EVP_DecryptFinal_ex(c, reinterpret_cast<unsigned char *>(plain.data()) + total, &len) != 1) break;
        total += len; plain.resize(total); *out = plain; ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(c);
    return ok;
}

static QByteArray scryptKey(const QString &passphrase, const QByteArray &salt, bool *ok)
{
    QByteArray key(32, 0);
    const QByteArray pw = passphrase.toUtf8();
    *ok = EVP_PBE_scrypt(pw.constData(), size_t(pw.size()), reinterpret_cast<const unsigned char *>(salt.constData()), size_t(salt.size()),
                         32768, 8, 1, 64 * 1024 * 1024, reinterpret_cast<unsigned char *>(key.data()), 32) == 1;
    return key;
}

// ── structs ───────────────────────────────────────────────────────────────────
QJsonObject IdentityDevice::toJson() const
{
    QJsonObject o{{"name", name}, {"kind", kind}, {"added", added.toUTC().toString(Qt::ISODate)}};
    if (!pub.isEmpty()) o["pub"] = QString::fromLatin1(pub.toBase64());
    return o;
}
IdentityDevice IdentityDevice::fromJson(const QJsonObject &o)
{
    IdentityDevice d; d.name = o["name"].toString(); d.kind = o["kind"].toString();
    d.added = QDateTime::fromString(o["added"].toString(), Qt::ISODate); d.pub = QByteArray::fromBase64(o["pub"].toString().toLatin1());
    return d;
}

QByteArray LinkStatement::canon() const
{
    const QString lo = a < b ? a : b, hi = a < b ? b : a;
    return "beaconfix-link|v1|" + lo.toUtf8() + "|" + hi.toUtf8() + "|" + ts.toUtf8();
}
QJsonObject LinkStatement::toJson(bool withPubs) const
{
    QJsonObject o{{"v", 1}, {"a", a}, {"b", b}, {"ts", ts}, {"sigA", QString::fromLatin1(sigA.toBase64())}, {"sigB", QString::fromLatin1(sigB.toBase64())}};
    if (withPubs) { if (!pubA.isEmpty()) o["pubA"] = QString::fromLatin1(pubA.toBase64()); if (!pubB.isEmpty()) o["pubB"] = QString::fromLatin1(pubB.toBase64()); }
    return o;
}
LinkStatement LinkStatement::fromJson(const QJsonObject &o)
{
    LinkStatement s; s.a = o["a"].toString(); s.b = o["b"].toString(); s.ts = o["ts"].toString();
    s.sigA = QByteArray::fromBase64(o["sigA"].toString().toLatin1()); s.sigB = QByteArray::fromBase64(o["sigB"].toString().toLatin1());
    s.pubA = QByteArray::fromBase64(o["pubA"].toString().toLatin1()); s.pubB = QByteArray::fromBase64(o["pubB"].toString().toLatin1());
    return s;
}

QJsonObject PendingLink::toJson() const
{
    return QJsonObject{{"id", id}, {"name", name}, {"ip", ip}, {"device", QJsonObject{{"name", deviceName}, {"kind", deviceKind}}},
                       {"pub", QString::fromLatin1(pub.toBase64())}, {"time", time.toUTC().toString(Qt::ISODate)}};
}
PendingLink PendingLink::fromJson(const QJsonObject &o)
{
    PendingLink p; p.id = o["id"].toString(); p.name = o["name"].toString(); p.ip = o["ip"].toString();
    p.deviceName = o["device"].toObject()["name"].toString(); p.deviceKind = o["device"].toObject()["kind"].toString();
    p.pub = QByteArray::fromBase64(o["pub"].toString().toLatin1()); p.time = QDateTime::fromString(o["time"].toString(), Qt::ISODate);
    return p;
}

// ── Identity ──────────────────────────────────────────────────────────────────
Identity::Identity(QObject *parent) : QObject(parent) {}

QString Identity::filePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/sworrl/identity.json");
}

void Identity::fromRecord(const QJsonObject &rec)
{
    m_id = rec["id"].toString(); m_name = rec["name"].toString();
    m_created = QDateTime::fromString(rec["created"].toString(), Qt::ISODate);
    m_pub = QByteArray::fromBase64(rec["pub"].toString().toLatin1());
    m_devices.clear(); for (const QJsonValue &v : rec["devices"].toArray()) m_devices << IdentityDevice::fromJson(v.toObject());
    m_links.clear();
    for (const QJsonValue &v : rec["links"].toArray()) { const LinkStatement s = LinkStatement::fromJson(v.toObject()); if (s.complete()) m_links << s; }
}

bool Identity::load(const QByteArray &dbKey)
{
    m_dbKey = dbKey;
    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o["v"].toInt() != 1) { m_error = QStringLiteral("unknown identity file version"); return false; }
    fromRecord(o["public"].toObject());
    m_pending.clear(); for (const QJsonValue &v : o["pending"].toArray()) m_pending << PendingLink::fromJson(v.toObject());
    m_seed.clear();
    if (!m_dbKey.isEmpty() && o.contains("sealed")) {
        const QByteArray blob = QByteArray::fromBase64(o["sealed"].toString().toLatin1());
        QByteArray plain;
        if (blob.size() > 28 && aeadOpen(m_dbKey, blob.left(12), "beaconfix-identity-store-v1", blob.mid(12), &plain)) {
            const QJsonObject p = QJsonDocument::fromJson(plain).object();
            const QByteArray seed = QByteArray::fromBase64(p["seed"].toString().toLatin1());
            QByteArray pub;
            if (seed.size() == 32 && keypairFromSeed(seed, &pub) && pub == m_pub) m_seed = seed;
            else m_error = QStringLiteral("the sealed key does not match the public record");
        } else m_error = QStringLiteral("could not unlock the identity (map-database key changed?)");
    }
    return exists();
}

bool Identity::save()
{
    QJsonObject o{{"v", 1}, {"public", record()}};
    if (unlocked()) {
        const QJsonObject plain{{"record", record()}, {"seed", QString::fromLatin1(m_seed.toBase64())}};
        const QByteArray nonce = randomBytes(12); QByteArray ct;
        if (m_dbKey.isEmpty() || nonce.isEmpty() || !aeadSeal(m_dbKey, nonce, "beaconfix-identity-store-v1", QJsonDocument(plain).toJson(QJsonDocument::Compact), &ct)) {
            m_error = QStringLiteral("could not seal the identity (no map-database key)"); return false;
        }
        o["sealed"] = QString::fromLatin1((nonce + ct).toBase64());
    }
    QJsonArray pend; for (const PendingLink &p : m_pending) pend.append(p.toJson()); o["pending"] = pend;
    QDir().mkpath(QFileInfo(filePath()).path());
    QSaveFile f(filePath());
    if (!f.open(QIODevice::WriteOnly)) { m_error = f.errorString(); return false; }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    if (!f.commit()) { m_error = f.errorString(); return false; }
    emit changed();
    return true;
}

QJsonObject Identity::record() const
{
    QJsonArray devs; for (const IdentityDevice &d : m_devices) devs.append(d.toJson());
    QJsonArray links; for (const LinkStatement &l : m_links) links.append(l.toJson(false));
    return QJsonObject{{"v", 1}, {"id", m_id}, {"name", m_name}, {"created", m_created.toUTC().toString(Qt::ISODate)},
                       {"pub", QString::fromLatin1(m_pub.toBase64())}, {"devices", devs}, {"links", links}};
}

QJsonObject Identity::publicJson() const
{
    QJsonObject o = record();
    o["linkedIds"] = QJsonArray::fromStringList(linkedIds());
    o["grouped"] = groupedId();
    o["unlocked"] = unlocked();
    return o;
}

QJsonObject Identity::linkPayload() const
{
    // The timestamp doubles as a one-time nonce: nine random fractional digits make it unguessable, and a
    // statement arriving over the network is only co-signed when its ts matches a link QR shown here in the
    // last 10 minutes (acceptLink). Without that, anyone on the LAN could mint an identity, sign a statement
    // linking it to ours, and get it co-signed — then sign in with full control. It stays a valid ISO-8601
    // instant, and both apps treat ts as an opaque string inside the canonical form.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString ts = now.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"))
                     + QStringLiteral(".%1Z").arg(QRandomGenerator::system()->bounded(1000000000u), 9, 10, QLatin1Char('0'));
    for (int i = m_offers.size() - 1; i >= 0; --i) if (m_offers[i].second < now) m_offers.removeAt(i);
    while (m_offers.size() >= 64) m_offers.removeFirst();
    m_offers.append(qMakePair(ts, now.addSecs(600)));
    return QJsonObject{{"v", 1}, {"t", "beaconfix-link"}, {"id", m_id}, {"name", m_name}, {"pub", QString::fromLatin1(m_pub.toBase64())},
                       {"ts", ts}};
}

QStringList Identity::linkedIds() const
{
    // transitive closure over the stored (complete) links
    QStringList out; QStringList frontier{m_id};
    while (!frontier.isEmpty()) {
        const QString cur = frontier.takeFirst();
        for (const LinkStatement &l : m_links) {
            const QString other = l.a == cur ? l.b : l.b == cur ? l.a : QString();
            if (other.isEmpty() || other == m_id || out.contains(other)) continue;
            out << other; frontier << other;
        }
    }
    return out;
}

bool Identity::create(const QString &name, const QString &deviceName, const QString &deviceKind, QString *error)
{
    if (m_dbKey.isEmpty()) { if (error) *error = QStringLiteral("no map-database key to seal the identity with"); return false; }
    const QByteArray seed = randomBytes(32); QByteArray pub;
    if (seed.isEmpty() || !keypairFromSeed(seed, &pub)) { if (error) *error = QStringLiteral("key generation failed"); return false; }
    m_seed = seed; m_pub = pub; m_id = idFor(pub);
    m_name = name.trimmed().left(64); if (m_name.isEmpty()) m_name = QStringLiteral("BeaconFix");
    m_created = QDateTime::currentDateTimeUtc();
    m_devices.clear(); m_links.clear();
    IdentityDevice d; d.name = deviceName; d.kind = deviceKind; d.added = m_created; d.pub = pub; m_devices << d;
    if (!save()) { if (error) *error = m_error; return false; }
    return true;
}

QString Identity::wordCode(int words)
{
    QStringList w;
    for (int i = 0; i < words; ++i) w << QString::fromLatin1(EFF_SHORT_WORDS[QRandomGenerator::system()->bounded(1296)]);
    return w.join(QLatin1Char(' '));
}

QString Identity::exportBundle(const QString &passphrase, QString *error) const
{
    if (!unlocked()) { if (error) *error = QStringLiteral("identity is locked"); return {}; }
    if (passphrase.size() < 8) { if (error) *error = QStringLiteral("passphrase must be at least 8 characters"); return {}; }
    const QByteArray salt = randomBytes(16), nonce = randomBytes(12);
    bool ok = false; const QByteArray key = scryptKey(passphrase, salt, &ok);
    if (!ok || salt.isEmpty() || nonce.isEmpty()) { if (error) *error = QStringLiteral("key derivation failed"); return {}; }
    const QJsonObject plain{{"record", record()}, {"seed", QString::fromLatin1(m_seed.toBase64())}};
    QByteArray ct;
    if (!aeadSeal(key, nonce, "beaconfix-identity-v1", QJsonDocument(plain).toJson(QJsonDocument::Compact), &ct)) { if (error) *error = QStringLiteral("encryption failed"); return {}; }
    const QJsonObject bundle{{"v", 1}, {"t", "beaconfix-identity"},
                             {"kdf", QJsonObject{{"name", "scrypt"}, {"n", 32768}, {"r", 8}, {"p", 1}, {"salt", QString::fromLatin1(salt.toBase64())}}},
                             {"aead", QJsonObject{{"name", "aes-256-gcm"}, {"nonce", QString::fromLatin1(nonce.toBase64())}}},
                             {"ct", QString::fromLatin1(ct.toBase64())}};
    return encodeUri(QStringLiteral("identity"), bundle);
}

QString Identity::encodeUri(const QString &kind, const QJsonObject &o)
{
    return QStringLiteral("beaconfix://%1/%2").arg(kind, QString::fromLatin1(base64url(QJsonDocument(o).toJson(QJsonDocument::Compact))));
}

QJsonObject Identity::decodePayload(const QString &text, QString *kind)
{
    QString t = text.trimmed();
    if (kind) kind->clear();
    auto b64 = [&](const QString &body, const char *k) { if (kind) *kind = QString::fromLatin1(k); return QJsonDocument::fromJson(fromBase64url(body.trimmed().toLatin1())).object(); };
    if (t.startsWith(QLatin1String("beaconfix://"), Qt::CaseInsensitive)) {
        const QString rest = t.mid(12);
        const int slash = rest.indexOf(QLatin1Char('/'));
        if (slash < 0) return {};
        const QString k = rest.left(slash).toLower(); QString body = rest.mid(slash + 1);
        const int q = body.indexOf(QLatin1Char('?')); if (q >= 0) body = body.left(q);   // tolerate trailing query / fragment
        const int h = body.indexOf(QLatin1Char('#')); if (h >= 0) body = body.left(h);
        if (k == QLatin1String("link") || k == QLatin1String("statement") || k == QLatin1String("identity")) return b64(body, k.toLatin1().constData());
        return {};
    }
    if (t.startsWith(QLatin1String("BFLNK1:"))) return b64(t.mid(7), "link");
    if (t.startsWith(QLatin1String("BFLINK1:"))) return b64(t.mid(8), "statement");
    if (t.startsWith(QLatin1String("BFID1:"))) return b64(t.mid(6), "identity");
    const QJsonObject o = QJsonDocument::fromJson(t.toUtf8()).object();
    if (!o.isEmpty() && kind) *kind = QStringLiteral("json");
    return o;
}

bool Identity::importBundle(const QString &textOrJson, const QString &passphrase, const QString &deviceName, const QString &deviceKind, QString *error)
{
    if (m_dbKey.isEmpty()) { if (error) *error = QStringLiteral("no map-database key to seal the identity with"); return false; }
    QString t = textOrJson.trimmed();
    QJsonObject bundle;
    bundle = decodePayload(t);                       // beaconfix://identity/…, BFID1:… or the bundle JSON itself
    if (bundle["t"].toString() != QLatin1String("beaconfix-identity") || bundle["v"].toInt() != 1) { if (error) *error = QStringLiteral("not a BeaconFix identity bundle"); return false; }
    const QJsonObject kdf = bundle["kdf"].toObject(), aead = bundle["aead"].toObject();
    if (kdf["name"].toString() != QLatin1String("scrypt") || aead["name"].toString() != QLatin1String("aes-256-gcm")) { if (error) *error = QStringLiteral("unsupported bundle algorithms"); return false; }
    const QByteArray salt = QByteArray::fromBase64(kdf["salt"].toString().toLatin1()), nonce = QByteArray::fromBase64(aead["nonce"].toString().toLatin1());
    const QByteArray ct = QByteArray::fromBase64(bundle["ct"].toString().toLatin1());
    bool ok = false;
    QByteArray key(32, 0);
    {
        const QByteArray pw = passphrase.toUtf8();
        ok = EVP_PBE_scrypt(pw.constData(), size_t(pw.size()), reinterpret_cast<const unsigned char *>(salt.constData()), size_t(salt.size()),
                            quint64(kdf["n"].toInt(32768)), quint64(kdf["r"].toInt(8)), quint64(kdf["p"].toInt(1)), 64 * 1024 * 1024,
                            reinterpret_cast<unsigned char *>(key.data()), 32) == 1;
    }
    QByteArray plain;
    if (!ok || !aeadOpen(key, nonce, "beaconfix-identity-v1", ct, &plain)) { if (error) *error = QStringLiteral("wrong passphrase (or a damaged bundle)"); return false; }
    const QJsonObject p = QJsonDocument::fromJson(plain).object();
    const QByteArray seed = QByteArray::fromBase64(p["seed"].toString().toLatin1());
    QByteArray pub;
    if (seed.size() != 32 || !keypairFromSeed(seed, &pub)) { if (error) *error = QStringLiteral("bundle holds no usable key"); return false; }
    const QJsonObject rec = p["record"].toObject();
    if (QByteArray::fromBase64(rec["pub"].toString().toLatin1()) != pub || rec["id"].toString() != idFor(pub)) { if (error) *error = QStringLiteral("bundle record does not match its key"); return false; }
    m_seed = seed; m_pub = pub;
    fromRecord(rec);
    bool have = false; for (const IdentityDevice &d : m_devices) if (d.name == deviceName && d.kind == deviceKind) have = true;
    if (!have) { IdentityDevice d; d.name = deviceName; d.kind = deviceKind; d.added = QDateTime::currentDateTimeUtc(); d.pub = pub; m_devices << d; }
    if (!save()) { if (error) *error = m_error; return false; }
    return true;
}

bool Identity::rename(const QString &name)
{
    if (!exists()) return false;
    m_name = name.trimmed().left(64);
    return save();
}

bool Identity::addDevice(const QString &name, const QString &kind, const QByteArray &pub)
{
    if (!exists()) return false;
    for (IdentityDevice &d : m_devices) if (d.name == name && d.kind == kind) { if (!pub.isEmpty()) d.pub = pub; return save(); }
    IdentityDevice d; d.name = name; d.kind = kind; d.added = QDateTime::currentDateTimeUtc(); d.pub = pub; m_devices << d;
    return save();
}

bool Identity::forget()
{
    m_seed.clear(); m_pub.clear(); m_id.clear(); m_name.clear(); m_devices.clear(); m_links.clear(); m_pending.clear();
    const bool ok = !QFile::exists(filePath()) || QFile::remove(filePath());
    emit changed();
    return ok;
}

LinkStatement Identity::startLink(const QString &otherId, const QByteArray &otherPub, const QString &ts) const
{
    LinkStatement s; s.a = m_id; s.b = otherId; s.ts = ts.isEmpty() ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate) : ts;
    s.pubA = m_pub; s.pubB = otherPub;
    s.sigA = sign(s.canon());
    return s;
}

bool Identity::hasLinkWith(const QString &id) const
{
    for (const LinkStatement &l : m_links) if (l.a == id || l.b == id) return true;
    return false;
}

bool Identity::acceptLink(LinkStatement st, QString *error, LinkStatement *completed)
{
    if (!exists()) { if (error) *error = QStringLiteral("no identity on this device"); return false; }
    if (st.a.isEmpty() || st.b.isEmpty() || st.ts.isEmpty() || st.a == st.b) { if (error) *error = QStringLiteral("malformed link statement"); return false; }
    const bool weAreA = st.a == m_id, weAreB = st.b == m_id;
    if (weAreA) st.pubA = m_pub;
    if (weAreB) st.pubB = m_pub;
    // Verify whatever signatures are present against the pubs we know (the id must match its pub)
    auto check = [&](const QString &id, const QByteArray &pub, const QByteArray &sig) -> int {   // 1 ok, 0 absent, -1 bad
        if (sig.isEmpty()) return 0;
        if (pub.size() != 32 || idFor(pub) != id) return -1;
        return verify(pub, st.canon(), sig) ? 1 : -1;
    };
    const int okA = check(st.a, st.pubA, st.sigA), okB = check(st.b, st.pubB, st.sigB);
    if (okA < 0 || okB < 0) { if (error) *error = QStringLiteral("a signature does not verify"); return false; }
    if (!weAreA && !weAreB) {                                   // third-party statement: store only when both signed
        if (okA == 1 && okB == 1) { if (!hasLinkWith(st.a) || !hasLinkWith(st.b)) { m_links << st; save(); } if (completed) *completed = st; return true; }
        if (error) *error = QStringLiteral("statement incomplete and we are not a party to it");
        return false;
    }
    // We are a party: the other side must have signed (or be about to); add ours
    const int other = weAreA ? okB : okA;
    if (other != 1) { if (error) *error = QStringLiteral("the other identity has not signed this statement"); return false; }
    if (!unlocked()) { if (error) *error = QStringLiteral("identity is locked on this device"); return false; }
    // Adding our signature links the other identity to ours: it can then sign in and sync as us. Only do that
    // for a statement bound to a link QR we displayed (its ts is that QR's one-time nonce), once. Statements that
    // already carry our valid signature (we started them) need no offer.
    if ((weAreA && st.sigA.isEmpty()) || (weAreB && st.sigB.isEmpty())) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        int hit = -1;
        for (int i = 0; i < m_offers.size(); ++i) if (m_offers[i].first == st.ts && m_offers[i].second >= now) { hit = i; break; }
        if (hit < 0) {
            if (error) *error = QStringLiteral("this link was not started from a link QR shown on this device, or that QR expired: show the link QR again and scan it");
            return false;
        }
        m_offers.removeAt(hit);
    }
    if (weAreA && st.sigA.isEmpty()) st.sigA = sign(st.canon());
    if (weAreB && st.sigB.isEmpty()) st.sigB = sign(st.canon());
    if (completed) *completed = st;
    bool dup = false;
    for (const LinkStatement &l : m_links) if (l.canon() == st.canon()) dup = true;
    if (!dup) { m_links << st; removePending(weAreA ? st.b : st.a); save(); }
    return true;
}

void Identity::addPending(const PendingLink &p)
{
    for (PendingLink &x : m_pending) if (x.id == p.id) { x = p; save(); return; }
    while (m_pending.size() >= 10) m_pending.removeFirst();
    m_pending << p; save();
}

bool Identity::removePending(const QString &id)
{
    for (int i = 0; i < m_pending.size(); ++i) if (m_pending[i].id == id) { m_pending.removeAt(i); return true; }
    return false;
}

QJsonObject Identity::selftest()
{
    Identity t; QByteArray seed(32, char(0x01)), pub;
    if (!t.keypairFromSeed(seed, &pub)) return QJsonObject{{"ok", false}};
    const QByteArray msg = "beaconfix-selftest|v1";
    const QByteArray sig = ed25519Sign(seed, msg);
    const QByteArray canon = authCanon(QStringLiteral("host"), "AAAA", idFor(pub), QStringLiteral("dev"));
    return QJsonObject{{"ok", verify(pub, msg, sig)}, {"seed", QString::fromLatin1(seed.toBase64())}, {"pub", QString::fromLatin1(pub.toBase64())},
                       {"id", idFor(pub)}, {"message", QString::fromLatin1(msg)}, {"sig", QString::fromLatin1(sig.toBase64())},
                       {"authCanonExample", QString::fromLatin1(canon)}, {"authSigExample", QString::fromLatin1(ed25519Sign(seed, canon).toBase64())},
                       {"linkCanonExample", QString::fromLatin1(LinkStatement{QStringLiteral("aaaa"), QStringLiteral("bbbb"), QStringLiteral("2026-01-01T00:00:00Z"), {}, {}, {}, {}}.canon())}};
}
