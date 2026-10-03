#include "linking.h"
#include "securechannel.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace Link {

static const unsigned char *u(const QByteArray &b) { return reinterpret_cast<const unsigned char *>(b.constData()); }

QByteArray hmacSha256(const QByteArray &key, const QByteArray &msg)
{
    QByteArray out(32, 0); unsigned int len = 32;
    static const unsigned char zero = 0;
    if (!HMAC(EVP_sha256(), key.isEmpty() ? &zero : u(key), key.size(), u(msg), size_t(msg.size()), reinterpret_cast<unsigned char *>(out.data()), &len) || len != 32) return {};
    return out;
}

QByteArray hkdfSha256(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int len)
{
    if (len <= 0 || len > 255 * 32) return {};
    const QByteArray prk = hmacSha256(salt.isEmpty() ? QByteArray(32, 0) : salt, ikm);
    QByteArray okm, t;
    for (int i = 1; okm.size() < len; ++i) { t = hmacSha256(prk, t + info + char(i)); okm += t; }
    return okm.left(len);
}

QString qrMac(const QByteArray &k, const QString &sid, const QString &name, const QString &kind, const QString &pubB64u)
{
    return QString::fromLatin1(hmacSha256(k, (QStringLiteral("bflink\n") + sid + QLatin1Char('\n') + name + QLatin1Char('\n') + kind + QLatin1Char('\n') + pubB64u).toUtf8()).toHex());
}

QString commit(const QString &name, const QString &kind, const QString &pubB64u)
{
    return QString::fromLatin1(QCryptographicHash::hash((QStringLiteral("bflink-commit\n") + name + QLatin1Char('\n') + kind + QLatin1Char('\n') + pubB64u).toUtf8(),
                                                        QCryptographicHash::Sha256).toHex());
}

QString code(const QByteArray &shared, const QString &sid)
{
    const QByteArray b = hkdfSha256(shared, sid.toUtf8(), QByteArrayLiteral("beaconfix-link-code-v1"), 4);
    if (b.size() != 4) return {};
    const quint32 v = (quint32(quint8(b[0])) << 24) | (quint32(quint8(b[1])) << 16) | (quint32(quint8(b[2])) << 8) | quint32(quint8(b[3]));
    return QStringLiteral("%1").arg(v % 1000000u, 6, 10, QLatin1Char('0'));
}

QString codeText(const QString &c) { return c.size() == 6 ? c.left(3) + QLatin1Char(' ') + c.mid(3) : c; }

QByteArray payloadKey(const QByteArray &shared, const QString &sid) { return hkdfSha256(shared, sid.toUtf8(), QByteArrayLiteral("beaconfix-link-payload-v1"), 32); }
QByteArray payloadAad(const QString &sid) { return QByteArrayLiteral("bflink-payload\n") + sid.toUtf8(); }

QString seal(const QByteArray &shared, const QString &sid, const QByteArray &nonce, const QByteArray &payload)
{
    QByteArray ct;
    if (nonce.size() != 12 || !Bfs3::aeadSeal(payloadKey(shared, sid), nonce, payloadAad(sid), payload, &ct)) return {};
    return QString::fromLatin1(Bfs3::b64u(nonce + ct));
}

bool open(const QByteArray &shared, const QString &sid, const QString &sealedB64u, QByteArray *payload)
{
    bool ok = false; const QByteArray raw = Bfs3::unb64u(sealedB64u.toLatin1(), &ok);
    if (!ok || raw.size() < 12 + 16) return false;
    return Bfs3::aeadOpen(payloadKey(shared, sid), raw.left(12), payloadAad(sid), raw.mid(12), payload);
}

// ── JSON in a fixed key order ────────────────────────────────────────────────
static QByteArray jsonValue(const QJsonValue &v)
{
    QByteArray a = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);   // "[…]"
    return a.mid(1, a.size() - 2);
}

QByteArray orderedJson(const QList<QPair<QString, QJsonValue>> &fields)
{
    QByteArray out = "{";
    for (int i = 0; i < fields.size(); ++i) out += (i ? "," : "") + jsonValue(fields[i].first) + ':' + jsonValue(fields[i].second);
    return out + '}';
}

QString qrText(const QString &sid, const QString &name, const QStringList &hosts, int port, const QByteArray &pub, const QByteArray &k, qint64 expires, const QString &hub)
{
    const QByteArray j = orderedJson({{"v", 1}, {"sid", sid}, {"name", name}, {"hosts", QJsonArray::fromStringList(hosts)}, {"port", port},
                                      {"pub", QString::fromLatin1(Bfs3::b64u(pub))}, {"k", QString::fromLatin1(Bfs3::b64u(k))}, {"e", expires},
                                      {"hub", hub.isEmpty() ? QJsonValue() : QJsonValue(hub)}});
    return QStringLiteral("bflink:") + QString::fromLatin1(Bfs3::b64u(j));
}

bool decodeQr(const QString &text, QJsonObject *out)
{
    const QString t = text.trimmed();
    if (!t.startsWith(QLatin1String("bflink:"), Qt::CaseInsensitive)) return false;
    bool ok = false; const QByteArray j = Bfs3::unb64u(t.mid(7).toLatin1(), &ok);
    if (!ok) return false;
    const QJsonObject o = QJsonDocument::fromJson(j).object();
    if (o["v"].toInt() != 1 || o["sid"].toString().isEmpty()) return false;
    *out = o;
    return true;
}

QByteArray payloadJson(const QString &token, const QStringList &scopes, const QString &pcName, const QString &pcId, const QStringList &hosts, int port, const QString &hub)
{
    const QByteArray pc = orderedJson({{"name", pcName}, {"id", pcId}, {"hosts", QJsonArray::fromStringList(hosts)}, {"port", port}});
    const QByteArray head = orderedJson({{"token", token}, {"scopes", QJsonArray::fromStringList(scopes)}});
    return head.left(head.size() - 1) + ",\"pc\":" + pc + ",\"hub\":" + jsonValue(hub.isEmpty() ? QJsonValue() : QJsonValue(hub)) + '}';
}

QStringList mdnsTxt(const QString &name, const QString &id, int port)
{
    return {QStringLiteral("name=%1").arg(name.left(60)), QStringLiteral("id=%1").arg(id), QStringLiteral("link=1"), QStringLiteral("api=3"), QStringLiteral("port=%1").arg(port)};
}

// ── Sessions ─────────────────────────────────────────────────────────────────
QString Session::stateName() const
{
    static const char *n[] = {"open", "commit", "pending", "approving", "approved", "denied", "delivered", "cancelled"};
    return QString::fromLatin1(n[state]);
}

static bool newKeys(Session *s)
{
    s->sk = Bfs3::x25519Generate(&s->pub);
    const QByteArray id = Bfs3::random(8);
    if (s->sk.isEmpty() || id.size() != 8) return false;
    s->sid = QString::fromLatin1(id.toHex());
    return true;
}

Session *Book::offer(qint64 now, const QString &hub)
{
    Session s; s.origin = Session::Qr; s.state = Session::Open; s.created = now; s.expires = now + kQrSecs; s.hub = hub;
    s.k = Bfs3::random(32);
    if (!newKeys(&s) || s.k.size() != 32) return nullptr;
    m_s << s;
    return &m_s.last();
}

Session *Book::find(const QString &sid)
{
    for (Session &s : m_s) if (s.sid == sid) return &s;
    return nullptr;
}
const Session *Book::find(const QString &sid) const
{
    for (const Session &s : m_s) if (s.sid == sid) return &s;
    return nullptr;
}

bool Book::allow(const QString &ip, qint64 nowMs, int cost)
{
    QList<qint64> &l = m_hits[ip];
    while (!l.isEmpty() && l.first() < nowMs - 60000) l.removeFirst();
    const bool ok = l.size() + cost <= kPerSourcePerMin;
    for (int i = 0; i < cost; ++i) l.append(nowMs);      // spent either way: hammering keeps it shut
    return ok;
}

int Book::pendingCount(qint64 now) const
{
    int n = 0;
    for (const Session &s : m_s)
        if (s.origin == Session::Mdns && (s.state == Session::Committed || s.state == Session::Pending || s.state == Session::Approving) && s.expires >= now) ++n;
    return n;
}

static QJsonObject err(const char *e) { return QJsonObject{{"error", QString::fromLatin1(e)}}; }

int Book::request(const QJsonObject &b, const QString &ip, qint64 nowMs, QJsonObject *out, QString *sidOut)
{
    const qint64 now = nowMs / 1000;
    if (!allow(ip, nowMs)) { *out = err("rate limited"); return 429; }
    const QString sid = b["sid"].toString().trimmed(), name = b["name"].toString().trimmed(), kind = b["kind"].toString().trimmed();
    if (name.isEmpty() || name.size() > 64 || kind.size() > 16) { *out = err("name (1-64) and kind (≤16) required"); return 400; }
    if (!sid.isEmpty()) {                                       // QR path: the MAC proves the phone saw our screen
        const QString pubText = b["pub"].toString().trimmed();
        bool ok = false; const QByteArray pub = Bfs3::unb64u(pubText.toLatin1(), &ok);
        if (!ok || pub.size() != 32) { *out = err("pub: b64u X25519 public key required"); return 400; }
        Session *s = find(sid);
        if (!s || s->origin != Session::Qr || s->expires < now || s->state == Session::Cancelled) { *out = err("unknown or expired link session"); return 404; }
        if (s->state != Session::Open) { *out = err("this QR was already used: the PC shows a new one"); return 409; }
        if (!Bfs3::constantTimeEqual(qrMac(s->k, sid, name, kind, pubText).toLatin1(), b["mac"].toString().trimmed().toLower().toLatin1())) {
            allow(ip, nowMs, 2);                                // a bad MAC costs 3
            *out = err("bad mac"); return 403;
        }
        const QByteArray shared = Bfs3::x25519(s->sk, pub);
        if (shared.isEmpty()) { *out = err("bad pub"); return 400; }
        s->name = name; s->kind = kind; s->ip = ip; s->theirPub = pub; s->shared = shared; s->code = code(shared, sid);
        s->state = Session::Approving; s->expires = now + kDeliverSecs;
        *sidOut = sid;
        *out = QJsonObject{{"sid", sid}, {"pub", QString::fromLatin1(Bfs3::b64u(s->pub))}, {"status", "approved"}, {"expires", double(s->expires)}};
        return 202;
    }
    // mDNS path: the phone commits to its key first (docs/LINKING.md "Why the commitment")
    const QString c = b["commit"].toString().trimmed().toLower();
    static const QByteArray hexd("0123456789abcdef");
    bool hex = c.size() == 64;
    for (QChar ch : c) if (!hexd.contains(char(ch.toLatin1()))) hex = false;
    if (!hex) { *out = err("commit required: hex(SHA-256(\"bflink-commit\\n\" + name + \"\\n\" + kind + \"\\n\" + pub)), the key follows in POST /api/v1/link/<sid> (docs/LINKING.md)"); return 400; }
    for (Session &s : m_s)                                      // the same phone asking again replaces its earlier request
        if (s.origin == Session::Mdns && s.ip == ip && s.name == name && (s.state == Session::Committed || s.state == Session::Pending)) s.state = Session::Cancelled;
    if (pendingCount(now) >= kMaxPending) { *out = err("too many link requests waiting on this PC"); return 429; }
    Session s; s.origin = Session::Mdns; s.state = Session::Committed; s.created = now; s.expires = now + kRequestSecs;
    s.name = name; s.kind = kind; s.ip = ip; s.commitHex = c;
    if (!newKeys(&s)) { *out = err("rng"); return 500; }
    m_s << s;
    *sidOut = s.sid;
    *out = QJsonObject{{"sid", s.sid}, {"pub", QString::fromLatin1(Bfs3::b64u(s.pub))}, {"status", "commit"}, {"expires", double(s.expires)}};
    return 202;
}

int Book::reveal(const QString &sid, const QJsonObject &b, const QString &ip, qint64 nowMs, QJsonObject *out)
{
    const qint64 now = nowMs / 1000;
    if (!allow(ip, nowMs)) { *out = err("rate limited"); return 429; }
    Session *s = find(sid);
    if (!s || s->origin != Session::Mdns || s->expires < now || !s->live()) { *out = err("unknown or expired link session"); return 404; }
    if (s->state != Session::Committed) { *out = err("the key was already sent"); return 409; }
    if (s->ip != ip) { *out = err("not the address that asked"); return 403; }
    const QString pubText = b["pub"].toString().trimmed();
    bool ok = false; const QByteArray pub = Bfs3::unb64u(pubText.toLatin1(), &ok);
    if (!ok || pub.size() != 32) { *out = err("pub: b64u X25519 public key required"); return 400; }
    if (!Bfs3::constantTimeEqual(commit(s->name, s->kind, pubText).toLatin1(), s->commitHex.toLatin1())) {
        s->state = Session::Denied; s->reason = QStringLiteral("the key does not match the commitment"); s->expires = now + kDeliverSecs;
        *out = err("the key does not match the commitment"); return 403;
    }
    const QByteArray shared = Bfs3::x25519(s->sk, pub);
    if (shared.isEmpty()) { *out = err("bad pub"); return 400; }
    s->theirPub = pub; s->shared = shared; s->code = code(shared, sid); s->state = Session::Pending;
    *out = QJsonObject{{"sid", sid}, {"status", "pending"}, {"expires", double(s->expires)}};
    return 202;
}

int Book::poll(const QString &sid, qint64 now, QJsonObject *out)
{
    Session *s = find(sid);
    if (!s || !s->live() || s->expires < now) { *out = err("unknown or closed link session"); return 404; }
    switch (s->state) {
    case Session::Denied:   *out = QJsonObject{{"status", "denied"}, {"reason", s->reason}}; break;
    case Session::Approved: *out = QJsonObject{{"status", "approved"}, {"sealed", s->sealed}}; s->sealed.clear(); s->state = Session::Delivered; break;   // once
    default:                *out = QJsonObject{{"status", "pending"}}; break;
    }
    return 200;
}

bool Book::startApproval(const QString &sid, qint64 now)
{
    Session *s = find(sid);
    if (!s || s->state != Session::Pending || s->expires < now) return false;
    s->state = Session::Approving; s->expires = qMax(s->expires, now + 30);   // the hub invite may take a few seconds
    return true;
}

bool Book::approve(const QString &sid, const QByteArray &payload, qint64 now, const QByteArray &nonceIn)
{
    Session *s = find(sid);
    if (!s || s->state != Session::Approving || s->shared.isEmpty()) return false;
    const QByteArray nonce = nonceIn.size() == 12 ? nonceIn : Bfs3::random(12);
    s->sealed = seal(s->shared, sid, nonce, payload);
    if (s->sealed.isEmpty()) return false;
    s->state = Session::Approved; s->expires = now + kDeliverSecs;
    return true;
}

bool Book::deny(const QString &sid, const QString &reason, qint64 now)
{
    Session *s = find(sid);
    if (!s || !(s->state == Session::Committed || s->state == Session::Pending || s->state == Session::Approving)) return false;
    s->state = Session::Denied; s->reason = reason; s->expires = now + kDeliverSecs;
    return true;
}

bool Book::cancel(const QString &sid)
{
    Session *s = find(sid);
    if (!s || !(s->state == Session::Open || s->state == Session::Committed || s->state == Session::Pending)) return false;
    s->state = Session::Cancelled;
    return true;
}

bool Book::setHub(const QString &sid, const QString &hub)
{
    Session *s = find(sid);
    if (!s || s->state != Session::Open) return false;      // the QR's invite; once used, the payload carries what it had
    s->hub = hub;
    return true;
}

bool Book::sweep(qint64 now, QStringList *orphans)
{
    bool any = false;
    for (int i = m_s.size() - 1; i >= 0; --i) {
        const Session &s = m_s[i];
        if (s.live() && s.expires >= now) continue;
        if (s.state == Session::Approved && !s.deviceId.isEmpty() && orphans) *orphans << s.deviceId;   // a token nobody fetched
        m_s.removeAt(i); any = true;
    }
    const qint64 ms = now * 1000;
    for (auto it = m_hits.begin(); it != m_hits.end();) {
        while (!it->isEmpty() && it->first() < ms - 60000) it->removeFirst();
        if (it->isEmpty()) it = m_hits.erase(it); else ++it;
    }
    return any;
}

} // namespace Link
