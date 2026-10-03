// BFS3 vectors (docs/SECURE-API.md, tests/fixtures/bfs3_vectors.json from tools/bfs3_ref.py) against src/securechannel.*:
// every value byte for byte, decrypt round trips, tamper rejection, the replay window (incl. out of order).
//   securechannel_test tests/fixtures/bfs3_vectors.json      (CMake target securechannel_test, cmake -DBEACONFIX_TESTS=ON)
#include "../src/securechannel.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

using namespace Bfs3;
static int fails = 0;
#define CHECK(cond, fmt, ...) do { if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } else { std::printf("ok   " fmt "\n", ##__VA_ARGS__); } } while (0)
static QByteArray d(const QJsonValue &v) { return unb64u(v.toString().toLatin1()); }
static QByteArray s(const QJsonValue &v) { return v.toString().toUtf8(); }

int main(int argc, char **argv)
{
    QFile f(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("tests/fixtures/bfs3_vectors.json"));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("FAIL cannot open the vectors\n"); return 1; }
    const QJsonObject v = QJsonDocument::fromJson(f.readAll()).object();

    // Identities, shared secret, root key
    const QByteArray ssk = d(v["server_sk"]), dsk = d(v["device_sk"]), spk = d(v["server_pk"]), dpk = d(v["device_pk"]);
    CHECK(x25519Public(ssk) == spk, "server_pk from server_sk");
    CHECK(x25519Public(dsk) == dpk, "device_pk from device_sk");
    CHECK(fingerprint(spk) == v["fingerprint"].toString(), "fingerprint");
    CHECK(deviceId(dpk) == v["device_id"].toString(), "device_id");
    const QByteArray shared = x25519(dsk, spk);
    CHECK(shared == d(v["shared"]), "shared (device side)");
    CHECK(x25519(ssk, dpk) == shared, "shared (server side)");
    const QByteArray rk = rootKey(shared, spk, dpk);
    CHECK(rk == d(v["root_key"]), "root_key");
    const QString did = v["device_id"].toString();
    CHECK(x25519(dsk, QByteArray(32, 0)).isEmpty(), "x25519 rejects the all-zero point");

    // Enrolment
    const QJsonObject e = v["enroll"].toObject();
    Invite inv;
    CHECK(Invite::decode(e["invite"].toString(), &inv), "invite decodes");
    CHECK(inv.url == QLatin1String("https://hub.example.com") && inv.serverPub == spk && inv.id == e["invite_id"].toString()
          && inv.secret == d(e["invite_secret"]) && inv.expires == qint64(e["ts"].toDouble()) + 900, "invite fields");
    CHECK(inv.encode() == e["invite"].toString(), "invite re-encodes byte for byte");
    const QString mac = enrollMac(d(e["invite_secret"]), e["invite_id"].toString(), e["name"].toString(), e["kind"].toString(), v["device_pk"].toString(), qint64(e["ts"].toDouble()));
    CHECK(mac == e["mac"].toString(), "enroll mac");
    CHECK(enrollMac(d(e["invite_secret"]), e["invite_id"].toString(), e["name"].toString() + QStringLiteral("x"), e["kind"].toString(), v["device_pk"].toString(), qint64(e["ts"].toDouble())) != mac, "enroll mac covers the name");
    CHECK(proof(rk, did) == e["proof"].toString(), "proof");

    // Requests
    for (const QJsonValue &rv : v["requests"].toArray()) {
        const QJsonObject r = rv.toObject();
        const quint64 c = quint64(r["counter"].toDouble());
        const qint64 ts = qint64(r["ts"].toDouble());
        const QByteArray m = s(r["method"]), t = s(r["target"]), nonce = d(r["nonce"]), pt = s(r["plaintext"]), sealed = d(r["sealed"]);
        CHECK(requestKey(rk, c) == d(r["key"]), "request key c=%llu", (unsigned long long)c);
        CHECK(requestAad(m, t, did, c, ts) == s(r["aad"]), "request aad c=%llu", (unsigned long long)c);
        CHECK(sealRequest(rk, did, m, t, c, ts, nonce, pt) == sealed, "request sealed c=%llu", (unsigned long long)c);
        QByteArray back;
        CHECK(openRequest(rk, did, m, t, c, ts, nonce, sealed, &back) && back == pt, "request opens c=%llu", (unsigned long long)c);
        QByteArray bad = sealed; bad[0] = char(bad[0] ^ 1);
        CHECK(!openRequest(rk, did, m, t, c, ts, nonce, bad, &back), "tampered body rejected c=%llu", (unsigned long long)c);
        QByteArray badTag = sealed; badTag[badTag.size() - 1] = char(badTag[badTag.size() - 1] ^ 0x80);
        CHECK(!openRequest(rk, did, m, t, c, ts, nonce, badTag, &back), "tampered tag rejected c=%llu", (unsigned long long)c);
        CHECK(!openRequest(rk, did, m, t + "x", c, ts, nonce, sealed, &back), "other target rejected c=%llu", (unsigned long long)c);
        CHECK(!openRequest(rk, did, m, t, c + 1, ts, nonce, sealed, &back), "other counter rejected c=%llu", (unsigned long long)c);
        CHECK(!openRequest(rk, did, m, t, c, ts + 1, nonce, sealed, &back), "other ts rejected c=%llu", (unsigned long long)c);
        CHECK(!openRequest(rk, did, m == "GET" ? QByteArray("POST") : QByteArray("GET"), t, c, ts, nonce, sealed, &back), "other method rejected c=%llu", (unsigned long long)c);
        CHECK(!openRequest(rk, QStringLiteral("dffffffffffffffffffffffff"), m, t, c, ts, nonce, sealed, &back), "other device rejected c=%llu", (unsigned long long)c);
    }
    // Responses
    for (const QJsonValue &rv : v["responses"].toArray()) {
        const QJsonObject r = rv.toObject();
        const quint64 c = quint64(r["counter"].toDouble()); const int st = r["status"].toInt();
        const QByteArray nonce = d(r["nonce"]), pt = s(r["plaintext"]), sealed = d(r["sealed"]);
        CHECK(sealResponse(rk, did, st, c, nonce, pt) == sealed, "response sealed c=%llu status=%d", (unsigned long long)c, st);
        QByteArray back;
        CHECK(openResponse(rk, did, st, c, nonce, sealed, &back) && back == pt, "response opens c=%llu", (unsigned long long)c);
        CHECK(!openResponse(rk, did, st == 200 ? 500 : 200, c, nonce, sealed, &back), "response with another status rejected c=%llu", (unsigned long long)c);
        CHECK(!openResponse(rk, did, st, c + 1, nonce, sealed, &back), "response for another request rejected c=%llu", (unsigned long long)c);
        QByteArray bad = sealed; bad[2] = char(bad[2] ^ 4);
        CHECK(!openResponse(rk, did, st, c, nonce, bad, &back), "tampered response rejected c=%llu", (unsigned long long)c);
    }
    // Events
    for (const QJsonValue &ev : v["events"].toArray()) {
        const QJsonObject r = ev.toObject();
        const quint64 c = quint64(r["counter"].toDouble()), seq = quint64(r["seq"].toDouble());
        CHECK(sealEvent(rk, did, c, seq, d(r["nonce"]), s(r["plaintext"])) == s(r["line"]), "event line seq=%llu", (unsigned long long)seq);
        QByteArray back;
        CHECK(openEvent(rk, did, c, seq, s(r["line"]), &back) && back == s(r["plaintext"]), "event opens seq=%llu", (unsigned long long)seq);
        CHECK(!openEvent(rk, did, c, seq + 1, s(r["line"]), &back), "event out of sequence rejected seq=%llu", (unsigned long long)seq);
    }
    // Fresh keys round trip (random nonces, both directions)
    {
        QByteArray p1, p2; const QByteArray k1 = x25519Generate(&p1), k2 = x25519Generate(&p2);
        CHECK(k1.size() == 32 && p1.size() == 32 && x25519(k1, p2) == x25519(k2, p1), "fresh key pair agreement");
        const QByteArray r2 = rootKey(x25519(k1, p2), p1, p2), n = random(12), body = QByteArray(5000, 'x');
        QByteArray back;
        CHECK(openRequest(r2, deviceId(p2), "POST", "/api/v3/db/sync", 9, 1, n, sealRequest(r2, deviceId(p2), "POST", "/api/v3/db/sync", 9, 1, n, body), &back) && back == body, "round trip 5000 bytes");
    }
    // Replay window: 128 wide, out of order inside it, everything at or below H − 128 refused
    {
        ReplayWindow w;
        CHECK(!w.fresh(0), "counter 0 refused");
        CHECK(w.fresh(1), "1 fresh"); w.accept(1);
        CHECK(!w.fresh(1), "1 replayed");
        CHECK(w.fresh(5), "5 fresh (skipping)"); w.accept(5);
        CHECK(w.fresh(3) && w.fresh(2) && w.fresh(4), "2, 3, 4 still fresh (out of order)");
        w.accept(3);
        CHECK(!w.fresh(3) && w.fresh(2) && w.fresh(4) && !w.fresh(5), "3 and 5 seen, 2 and 4 not");
        w.accept(200);
        CHECK(!w.fresh(72) && w.fresh(73) && !w.fresh(200) && w.fresh(199), "after 200: 72 (= H − 128) refused, 73 fresh");
        w.accept(73); w.accept(150);
        CHECK(!w.fresh(73) && !w.fresh(150) && w.fresh(74), "73 / 150 seen across the 64-bit halves");
        const ReplayWindow back = ReplayWindow::load(w.save());
        CHECK(back.save() == w.save() && !back.fresh(73) && back.fresh(74) && back.highest() == 200, "save / load %s", qPrintable(w.save()));
        w.accept(264);                                  // shift by 64 exactly
        CHECK(!w.fresh(200) && w.fresh(201) && !w.fresh(136) && w.fresh(137), "shift by 64 keeps 200, window now 137..264");
        w.accept(1000);
        CHECK(!w.fresh(264) && !w.fresh(872) && w.fresh(873) && !w.fresh(1000), "big jump clears the bitmap");
        const quint64 big = (quint64(1) << 40) + 7;
        ReplayWindow w2; w2.accept(big);
        CHECK(!w2.fresh(big) && w2.fresh(big - 1) && w2.fresh(big + 1) && !w2.fresh(big - 128), "64-bit counters");
    }
    // At rest
    {
        const QByteArray key = random(32), secret = random(32);
        const QByteArray blob = sealAtRest(key, "bfs3 server", secret);
        CHECK(openAtRest(key, "bfs3 server", blob) == secret, "at-rest round trip");
        CHECK(openAtRest(key, "bfs3 device", blob).isEmpty(), "at-rest label bound");
        CHECK(openAtRest(random(32), "bfs3 server", blob).isEmpty(), "at-rest wrong key");
    }
    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
