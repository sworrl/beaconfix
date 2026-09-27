#include "pairing.h"
#include "locator.h"
#include "wifiscanner.h"
#include <QHash>
#include <QSet>
#include <QRandomGenerator>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <algorithm>
#include <cmath>

namespace Pairing {

const char *const ICON_NAMES[ICON_COUNT] = {
    "anchor", "apple", "balloon", "bell", "bicycle", "book", "cactus", "camera", "car", "cat", "cloud", "coffee", "compass", "crown", "diamond", "dog",
    "duck", "feather", "fish", "flag", "flower", "fox", "gift", "guitar", "hammer", "hat", "heart", "house", "key", "kite", "leaf", "lightbulb",
    "moon", "mountain", "mushroom", "owl", "pencil", "pizza", "rocket", "snowflake", "star", "sun", "tent", "tree", "umbrella", "whale", "wrench", "zebra"};
const char *const ICON_GLYPHS[ICON_COUNT] = {
    "⚓", "🍎", "🎈", "🔔", "🚲", "📖", "🌵", "📷", "🚗", "🐱", "☁️", "☕", "🧭", "👑", "💎", "🐶",
    "🦆", "🪶", "🐟", "🚩", "🌸", "🦊", "🎁", "🎸", "🔨", "🎩", "❤️", "🏠", "🔑", "🪁", "🍃", "💡",
    "🌙", "⛰️", "🍄", "🦉", "✏️", "🍕", "🚀", "❄️", "⭐", "☀️", "⛺", "🌳", "☂️", "🐳", "🔧", "🦓"};

QByteArray x25519Generate(QByteArray *pub)
{
    QByteArray priv;
    EVP_PKEY *key = nullptr;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) return priv;
    if (EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &key) == 1) {
        size_t n = 32; priv.resize(32);
        if (EVP_PKEY_get_raw_private_key(key, reinterpret_cast<unsigned char *>(priv.data()), &n) != 1 || n != 32) priv.clear();
        if (pub) { pub->resize(32); n = 32; if (EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char *>(pub->data()), &n) != 1 || n != 32) pub->clear(); }
    }
    if (key) EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(ctx);
    return priv;
}

QByteArray x25519Shared(const QByteArray &priv, const QByteArray &peerPub)
{
    QByteArray out;
    if (priv.size() != 32 || peerPub.size() != 32) return out;
    EVP_PKEY *me = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char *>(priv.constData()), 32);
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char *>(peerPub.constData()), 32);
    EVP_PKEY_CTX *ctx = me ? EVP_PKEY_CTX_new(me, nullptr) : nullptr;
    if (me && peer && ctx && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, peer) == 1) {
        size_t n = 32; out.resize(32);
        if (EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(out.data()), &n) != 1 || n != 32) out.clear();
    }
    if (ctx) EVP_PKEY_CTX_free(ctx);
    if (me) EVP_PKEY_free(me);
    if (peer) EVP_PKEY_free(peer);
    return out;
}

QByteArray hkdfSha256(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int len)
{
    QByteArray out(len, 0);
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) return {};
    bool ok = EVP_PKEY_derive_init(ctx) == 1
           && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1
           && EVP_PKEY_CTX_set1_hkdf_salt(ctx, reinterpret_cast<const unsigned char *>(salt.constData()), salt.size()) == 1
           && EVP_PKEY_CTX_set1_hkdf_key(ctx, reinterpret_cast<const unsigned char *>(ikm.constData()), ikm.size()) == 1
           && EVP_PKEY_CTX_add1_hkdf_info(ctx, reinterpret_cast<const unsigned char *>(info.constData()), info.size()) == 1;
    size_t n = size_t(len);
    if (ok) ok = EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(out.data()), &n) == 1 && int(n) == len;
    EVP_PKEY_CTX_free(ctx);
    return ok ? out : QByteArray();
}

QByteArray sas(const QByteArray &shared, const QString &pairId)
{
    return hkdfSha256(shared, QByteArray(), "beaconfix-pair-sas-v1|" + pairId.toUtf8(), 32);
}

QList<int> sasIcons(const QByteArray &s)
{
    QList<int> out;
    if (s.size() < 6) return out;
    for (int i = 0; i < 3; ++i) out << (static_cast<unsigned char>(s[2 * i]) % ICON_COUNT);
    return out;
}

QList<QList<int>> decoys(const QList<int> &real, int count)
{
    QList<QList<int>> out;
    if (real.size() != 3) return out;
    int guard = 0;
    while (out.size() < count && ++guard < 200) {
        QList<int> t;
        for (int i = 0; i < 3; ++i) {
            int v;
            do { v = int(QRandomGenerator::system()->bounded(ICON_COUNT)); } while (v == real[i]);   // never the real icon at that position
            t << v;
        }
        bool dup = false;
        for (const QList<int> &o : out) if (o == t) dup = true;
        if (!dup) out << t;
    }
    return out;
}

QJsonArray tripleJson(const QList<int> &triple)
{
    QJsonArray arr;
    for (int id : triple) {
        const int i = qBound(0, id, ICON_COUNT - 1);
        arr.append(QJsonObject{{"id", i}, {"name", QString::fromLatin1(ICON_NAMES[i])}, {"glyph", QString::fromUtf8(ICON_GLYPHS[i])}});
    }
    return arr;
}

QJsonObject Proximity::toJson() const
{
    return {{"shared", shared}, {"theirs", theirs}, {"ours", ours}, {"strongestShared", QJsonArray::fromStringList(strongestShared)},
            {"rssiDelta", rssiDelta < 0 ? QJsonValue() : QJsonValue(rssiDelta)}, {"gainOffsetDb", gainOffsetDb}, {"distanceM", distanceM < 0 ? QJsonValue() : QJsonValue(distanceM)},
            {"verdict", verdict}};
}

Proximity Proximity::fromJson(const QJsonObject &o)
{
    Proximity p;
    p.shared = o["shared"].toInt(); p.theirs = o["theirs"].toInt(); p.ours = o["ours"].toInt();
    for (const QJsonValue &v : o["strongestShared"].toArray()) p.strongestShared << v.toString();
    p.rssiDelta = o["rssiDelta"].isDouble() ? o["rssiDelta"].toDouble() : -1;
    p.gainOffsetDb = o["gainOffsetDb"].toDouble();
    p.distanceM = o["distanceM"].isDouble() ? o["distanceM"].toDouble() : -1;
    p.verdict = o["verdict"].toString(QStringLiteral("unknown"));
    return p;
}

static double median(QList<double> v)
{
    if (v.isEmpty()) return 0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2.0;
}

Proximity score(const QJsonObject &theirs, const QList<AccessPoint> &ours, const Fix &ourFix)
{
    Proximity p;
    QHash<QString, int> mine;                       // bssid → dbm, every beacon we hear (home / travelling included)
    QHash<QString, QString> names;
    for (const AccessPoint &ap : ours) {
        const QString mac = ap.bssid.toUpper().trimmed();
        if (mac.size() != 17) continue;
        mine.insert(mac, ap.dbm);
        names.insert(mac, ap.ssid);
    }
    p.ours = mine.size();
    struct Shared { QString label; int theirDbm; double delta; };
    QList<Shared> shared;
    const QJsonArray beacons = theirs["beacons"].toArray();
    p.theirs = 0;
    QSet<QString> seen;
    for (const QJsonValue &v : beacons) {
        const QJsonObject b = v.toObject();
        const QString mac = b["bssid"].toString().toUpper().trimmed();
        if (mac.size() != 17 || seen.contains(mac)) continue;
        seen.insert(mac);
        ++p.theirs;
        const auto it = mine.constFind(mac);
        if (it == mine.constEnd()) continue;
        const int td = b["dbm"].toInt(-100);
        const QString nm = names.value(mac);
        shared.append({nm.isEmpty() ? mac : nm, td, double(td - it.value())});
    }
    p.shared = shared.size();
    std::sort(shared.begin(), shared.end(), [](const Shared &a, const Shared &b) { return a.theirDbm > b.theirDbm; });
    for (int i = 0; i < shared.size() && i < 3; ++i) p.strongestShared << shared[i].label;
    if (!shared.isEmpty()) {
        QList<double> deltas; for (const Shared &s : shared) deltas << s.delta;
        p.gainOffsetDb = median(deltas);            // one radio simply hears everything a few dB louder: not distance
        QList<double> resid; for (double d : deltas) resid << std::fabs(d - p.gainOffsetDb);
        p.rssiDelta = median(resid);                // how differently the two see the same beacons once that is removed
    }
    if (theirs["lat"].isDouble() && theirs["lon"].isDouble() && ourFix.valid && ourFix.source != QLatin1String("ip") && theirs["source"].toString() != QLatin1String("ip"))
        p.distanceM = Locator::distanceM(theirs["lat"].toDouble(), theirs["lon"].toDouble(), ourFix.lat, ourFix.lon);
    // Verdict (docs/SECURITY.md "Pairing v2"): a differential test, then the fix distance
    const bool adjacent = p.shared >= 4 && p.rssiDelta >= 0 && p.rssiDelta <= 4;
    const bool room = !adjacent && p.shared >= 2 && p.rssiDelta >= 0 && p.rssiDelta <= 7;
    const bool near = p.shared >= 1 || (p.distanceM >= 0 && p.distanceM <= 150);
    const bool far = (p.shared == 0 && p.distanceM > 500) || (p.theirs > 5 && p.ours > 5 && p.shared == 0);
    p.verdict = adjacent ? QStringLiteral("adjacent") : room ? QStringLiteral("room") : near ? QStringLiteral("near") : far ? QStringLiteral("far") : QStringLiteral("unknown");
    return p;
}

QString verdictLabel(const QString &v)
{
    if (v == QLatin1String("adjacent")) return QStringLiteral("Right next to you");
    if (v == QLatin1String("room")) return QStringLiteral("In the same room");
    if (v == QLatin1String("near")) return QStringLiteral("Nearby");
    if (v == QLatin1String("far")) return QStringLiteral("NOT nearby");
    return QStringLiteral("Unknown distance");
}

bool verdictAllowed(const QString &verdict, const QString &policy)
{
    if (policy == QLatin1String("off") || policy == QLatin1String("warn")) return true;
    return verdict == QLatin1String("adjacent") || verdict == QLatin1String("room") || verdict == QLatin1String("near");
}

}
