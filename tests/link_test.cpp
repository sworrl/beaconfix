// Linking v3 vectors (docs/LINKING.md, tests/fixtures/link_vectors.json from tools/link_ref.py) against src/linking.*:
// QR text, MAC, commitment, code, sealed payload byte for byte; MAC / commitment rejection; code agreement on both sides;
// payload open + tamper; the session rules (expiry, single use, ≤ 3 pending, delivered once, rate limit).
//   link_test tests/fixtures/link_vectors.json      (CMake target link_test, cmake -DBEACONFIX_TESTS=ON)
#include "../src/linking.h"
#include "../src/securechannel.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

using namespace Link;
static int fails = 0;
#define CHECK(cond, fmt, ...) do { if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } else { std::printf("ok   " fmt "\n", ##__VA_ARGS__); } } while (0)
static QByteArray d(const QJsonValue &v) { return Bfs3::unb64u(v.toString().toLatin1()); }
static QString b64(const QByteArray &b) { return QString::fromLatin1(Bfs3::b64u(b)); }

int main(int argc, char **argv)
{
    QFile f(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("tests/fixtures/link_vectors.json"));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("FAIL cannot open the vectors\n"); return 1; }
    const QJsonObject v = QJsonDocument::fromJson(f.readAll()).object();

    // ── Vectors ──
    const QByteArray pcSk = d(v["pc_sk"]), phSk = d(v["phone_sk"]), pcPub = d(v["pc_pub"]), phPub = d(v["phone_pub"]), k = d(v["k"]);
    const QString sid = v["sid"].toString();
    CHECK(Bfs3::x25519Public(pcSk) == pcPub, "pc_pub from pc_sk");
    CHECK(Bfs3::x25519Public(phSk) == phPub, "phone_pub from phone_sk");
    const QByteArray shared = Bfs3::x25519(phSk, pcPub);
    CHECK(shared == d(v["shared"]), "shared (phone side)");
    CHECK(Bfs3::x25519(pcSk, phPub) == shared, "shared (PC side)");
    CHECK(qrText(sid, QStringLiteral("desk-pc"), {QStringLiteral("192.0.2.202"), QStringLiteral("desk-pc.local")}, 47897, pcPub, k, 1790900600, QStringLiteral("bfs3:example"))
          == v["qr"].toString(), "QR text byte for byte");
    QJsonObject qr;
    CHECK(decodeQr(v["qr"].toString(), &qr) && qr["sid"].toString() == sid && d(qr["pub"]) == pcPub && d(qr["k"]) == k && qr["e"].toDouble() == 1790900600
          && qr["hub"].toString() == QLatin1String("bfs3:example") && qr["port"].toInt() == 47897, "QR decodes");
    CHECK(!decodeQr(QStringLiteral("bfs3:") + v["qr"].toString().mid(7), &qr), "decodeQr refuses another prefix");
    const QString name = v["mac_name"].toString(), kind = v["mac_kind"].toString(), phPubText = v["phone_pub"].toString();
    CHECK(qrMac(k, sid, name, kind, phPubText) == v["mac"].toString(), "mac");
    CHECK(qrMac(k, sid, name + QStringLiteral("x"), kind, phPubText) != v["mac"].toString(), "mac covers the name");
    CHECK(qrMac(k, sid, name, QStringLiteral("laptop"), phPubText) != v["mac"].toString(), "mac covers the kind");
    CHECK(commit(name, kind, phPubText) == v["commit"].toString(), "commit");
    CHECK(code(shared, sid) == v["code"].toString(), "code %s", qPrintable(code(shared, sid)));
    CHECK(codeText(QStringLiteral("740948")) == QLatin1String("740 948") && codeText(QStringLiteral("012345")) == QLatin1String("012 345"), "code shown grouped");
    const QByteArray payload = v["payload"].toString().toUtf8();
    CHECK(payloadJson(QStringLiteral("tok_example"), {QStringLiteral("read"), QStringLiteral("control")}, QStringLiteral("desk-pc"), QString(),
                      {QStringLiteral("192.0.2.202")}, 47897, QStringLiteral("bfs3:example")) == payload, "payload JSON byte for byte");
    CHECK(payloadJson(QStringLiteral("t"), {QStringLiteral("read")}, QStringLiteral("P"), QStringLiteral("i"), {}, 1, QString()).endsWith(",\"hub\":null}"), "payload: no hub is null");
    CHECK(seal(shared, sid, d(v["nonce"]), payload) == v["sealed"].toString(), "sealed byte for byte");
    QByteArray back;
    CHECK(open(shared, sid, v["sealed"].toString(), &back) && back == payload, "sealed opens");
    QByteArray raw = d(v["sealed"]); raw[20] = char(raw[20] ^ 1);
    CHECK(!open(shared, sid, b64(raw), &back), "tampered ciphertext refused");
    raw = d(v["sealed"]); raw[3] = char(raw[3] ^ 1);
    CHECK(!open(shared, sid, b64(raw), &back), "tampered nonce refused");
    CHECK(!open(shared, QStringLiteral("a1b2c3d4e5f60719"), v["sealed"].toString(), &back), "another sid (AAD + key) refused");
    CHECK(!open(QByteArray(32, 7), sid, v["sealed"].toString(), &back), "another shared secret refused");
    const QStringList txt = mdnsTxt(QStringLiteral("desk-pc"), QStringLiteral("abc"), 47822);
    CHECK(txt.contains(QStringLiteral("name=desk-pc")) && txt.contains(QStringLiteral("id=abc")) && txt.contains(QStringLiteral("link=1"))
          && txt.contains(QStringLiteral("api=3")) && txt.contains(QStringLiteral("port=47822")), "mDNS TXT: name, id, link=1, api=3, port");

    // ── QR path through a Book ──
    const qint64 t0 = 1790900000, ms0 = t0 * 1000;
    Book book;
    Session *s = book.offer(t0, QStringLiteral("bfs3:hub"));
    CHECK(s && s->sid.size() == 16 && s->k.size() == 32 && s->pub.size() == 32 && s->expires == t0 + kQrSecs && s->state == Session::Open, "offer: sid, k, keys, 10 min");
    const QString qsid = s->sid; const QByteArray qk = s->k, qpub = s->pub;
    QByteArray ppub; const QByteArray psk = Bfs3::x25519Generate(&ppub);
    const QString ppubText = b64(ppub);
    QJsonObject out; QString got;
    QJsonObject req{{"sid", qsid}, {"name", "Pixel"}, {"kind", "android"}, {"pub", ppubText}, {"mac", qrMac(qk, qsid, QStringLiteral("Pixel"), QStringLiteral("android"), ppubText)}};
    QJsonObject bad = req; bad["mac"] = qrMac(qk, qsid, QStringLiteral("Pixel"), QStringLiteral("android"), b64(QByteArray(32, 9)));
    CHECK(book.request(bad, QStringLiteral("10.0.0.5"), ms0, &out, &got) == 403, "QR: a MAC for another key is refused");
    bad = req; bad["mac"] = qrMac(QByteArray(32, 1), qsid, QStringLiteral("Pixel"), QStringLiteral("android"), ppubText);
    CHECK(book.request(bad, QStringLiteral("10.0.0.5"), ms0, &out, &got) == 403, "QR: a MAC without k is refused");
    CHECK(book.find(qsid)->state == Session::Open, "QR: bad MACs do not burn the session");
    bad = req; bad["sid"] = QStringLiteral("ffffffffffffffff");
    CHECK(book.request(bad, QStringLiteral("10.0.0.5"), ms0, &out, &got) == 404, "QR: unknown sid");
    CHECK(book.request(req, QStringLiteral("10.0.0.6"), (t0 + kQrSecs + 1) * 1000, &out, &got) == 404, "QR: expired session");
    CHECK(book.request(req, QStringLiteral("10.0.0.6"), ms0, &out, &got) == 202 && got == qsid && out["status"].toString() == QLatin1String("approved")
          && d(out["pub"]) == qpub, "QR: a good MAC is approved at once");
    CHECK(book.request(req, QStringLiteral("10.0.0.6"), ms0, &out, &got) == 409, "QR: single use");
    const QByteArray pshared = Bfs3::x25519(psk, qpub);
    CHECK(book.find(qsid)->code == code(pshared, qsid) && book.find(qsid)->code.size() == 6, "QR: both sides show the same code");
    CHECK(book.poll(qsid, t0, &out) == 200 && out["status"].toString() == QLatin1String("pending"), "QR: pending until the payload is sealed");
    CHECK(book.approve(qsid, "{\"token\":\"x\"}", t0), "QR: approve");
    CHECK(book.poll(qsid, t0 + 1, &out) == 200 && out["status"].toString() == QLatin1String("approved") && open(pshared, qsid, out["sealed"].toString(), &back)
          && back == "{\"token\":\"x\"}", "QR: the phone opens the payload");
    CHECK(book.poll(qsid, t0 + 2, &out) == 404, "QR: delivered once, then closed");
    book.sweep(t0 + 3);
    CHECK(!book.find(qsid), "QR: swept after delivery");
    // a cancelled (renewed) QR no longer links
    s = book.offer(t0); const QString csid = s->sid; const QByteArray ck = s->k;
    CHECK(book.cancel(csid), "QR: cancel");
    QJsonObject creq{{"sid", csid}, {"name", "Pixel"}, {"kind", "android"}, {"pub", ppubText}, {"mac", qrMac(ck, csid, QStringLiteral("Pixel"), QStringLiteral("android"), ppubText)}};
    CHECK(book.request(creq, QStringLiteral("10.0.0.7"), ms0, &out, &got) == 404, "QR: a cancelled session is gone");
    // approved but never fetched: swept, its device reported as an orphan
    s = book.offer(t0); const QString osid = s->sid; const QByteArray ok2 = s->k;
    QJsonObject oreq{{"sid", osid}, {"name", "Tab"}, {"kind", "android"}, {"pub", ppubText}, {"mac", qrMac(ok2, osid, QStringLiteral("Tab"), QStringLiteral("android"), ppubText)}};
    CHECK(book.request(oreq, QStringLiteral("10.0.0.8"), ms0, &out, &got) == 202, "QR: second session approved");
    book.find(osid)->deviceId = QStringLiteral("dev1");
    CHECK(book.approve(osid, "{}", t0), "QR: sealed");
    QStringList orphans;
    book.sweep(t0 + kDeliverSecs + 1, &orphans);
    CHECK(!book.find(osid) && orphans == QStringList{QStringLiteral("dev1")}, "approved, never fetched: expired, device reported");

    // ── mDNS path ──
    const QString cm = commit(QStringLiteral("Pixel"), QStringLiteral("android"), ppubText);
    QJsonObject mreq{{"name", "Pixel"}, {"kind", "android"}, {"commit", cm}};
    CHECK(book.request(QJsonObject{{"name", "Pixel"}, {"kind", "android"}, {"pub", ppubText}}, QStringLiteral("10.0.1.1"), ms0, &out, &got) == 400, "mDNS: a key without a commitment is refused");
    CHECK(book.request(mreq, QStringLiteral("10.0.1.1"), ms0, &out, &got) == 202 && out["status"].toString() == QLatin1String("commit") && got.size() == 16, "mDNS: commitment accepted");
    const QString msid = got; const QByteArray mpub = d(out["pub"]);
    CHECK(book.find(msid)->expires == t0 + kRequestSecs, "mDNS: 2 minutes");
    CHECK(book.poll(msid, t0, &out) == 200 && out["status"].toString() == QLatin1String("pending"), "mDNS: pending before the key");
    CHECK(book.reveal(msid, QJsonObject{{"pub", ppubText}}, QStringLiteral("10.0.1.2"), ms0, &out) == 403, "mDNS: the key from another address is refused");
    QByteArray xpub; Bfs3::x25519Generate(&xpub);
    {   // a key other than the committed one: the request is denied (a MITM that ground a key would be caught here)
        Book b2; QString g2;
        CHECK(b2.request(mreq, QStringLiteral("10.0.1.1"), ms0, &out, &g2) == 202, "mDNS: second book");
        CHECK(b2.reveal(g2, QJsonObject{{"pub", b64(xpub)}}, QStringLiteral("10.0.1.1"), ms0, &out) == 403, "mDNS: a key that does not match the commitment is refused");
        CHECK(b2.poll(g2, t0, &out) == 200 && out["status"].toString() == QLatin1String("denied"), "mDNS: … and the request is denied");
    }
    CHECK(book.reveal(msid, QJsonObject{{"pub", ppubText}}, QStringLiteral("10.0.1.1"), ms0, &out) == 202 && out["status"].toString() == QLatin1String("pending"), "mDNS: key revealed");
    CHECK(book.reveal(msid, QJsonObject{{"pub", ppubText}}, QStringLiteral("10.0.1.1"), ms0, &out) == 409, "mDNS: the key once");
    const QByteArray mshared = Bfs3::x25519(psk, mpub);
    CHECK(book.find(msid)->state == Session::Pending && book.find(msid)->code == code(mshared, msid), "mDNS: both sides show the same code");
    CHECK(!book.approve(msid, "{}", t0), "mDNS: no payload before the user taps Link");
    CHECK(book.startApproval(msid, t0) && book.approve(msid, "{\"token\":\"m\"}", t0), "mDNS: Link → sealed");
    CHECK(book.poll(msid, t0, &out) == 200 && open(mshared, msid, out["sealed"].toString(), &back) && back == "{\"token\":\"m\"}", "mDNS: the phone opens the payload");
    CHECK(book.poll(msid, t0, &out) == 404, "mDNS: delivered once");
    // Reject
    CHECK(book.request(QJsonObject{{"name", "Other"}, {"kind", "android"}, {"commit", cm}}, QStringLiteral("10.0.1.3"), ms0, &out, &got) == 202, "mDNS: another request");
    const QString rsid = got;
    CHECK(book.deny(rsid, QStringLiteral("rejected on the PC"), t0) && book.poll(rsid, t0, &out) == 200 && out["status"].toString() == QLatin1String("denied")
          && out["reason"].toString() == QLatin1String("rejected on the PC"), "mDNS: Reject → denied with the reason");
    CHECK(!book.startApproval(rsid, t0), "mDNS: a rejected request cannot be approved");
    // ≤ 3 pending, expiry, the same phone replacing its own request
    Book b3; QString g;
    for (int i = 0; i < 3; ++i) CHECK(b3.request(QJsonObject{{"name", QStringLiteral("P%1").arg(i)}, {"kind", "android"}, {"commit", cm}}, QStringLiteral("10.0.2.%1").arg(i), ms0, &out, &g) == 202, "mDNS: pending %d", i + 1);
    CHECK(b3.request(QJsonObject{{"name", "P9"}, {"kind", "android"}, {"commit", cm}}, QStringLiteral("10.0.2.9"), ms0, &out, &g) == 429, "mDNS: a 4th pending request is refused");
    CHECK(b3.request(QJsonObject{{"name", "P0"}, {"kind", "android"}, {"commit", cm}}, QStringLiteral("10.0.2.0"), ms0, &out, &g) == 202 && b3.pendingCount(t0) == 3, "mDNS: the same phone replaces its request");
    CHECK(b3.pendingCount(t0 + kRequestSecs + 1) == 0, "mDNS: requests expire after 2 minutes");
    CHECK(b3.request(QJsonObject{{"name", "P9"}, {"kind", "android"}, {"commit", cm}}, QStringLiteral("10.0.2.9"), (t0 + kRequestSecs + 1) * 1000, &out, &g) == 202, "mDNS: room again after expiry");
    CHECK(b3.reveal(g, QJsonObject{{"pub", ppubText}}, QStringLiteral("10.0.2.9"), (t0 + kRequestSecs + 1) * 1000, &out) == 403, "mDNS: P9's key does not match a commitment made for Pixel");
    b3.sweep(t0 + 10 * kRequestSecs);
    CHECK(b3.sessions().isEmpty(), "sweep empties the book");
    // per-source rate limit
    Book b4; int last = 0, n = 0;
    for (int i = 0; i < kPerSourcePerMin + 2; ++i) { last = b4.request(QJsonObject{{"sid", "0000000000000000"}, {"name", "x"}, {"kind", "android"}, {"pub", ppubText}, {"mac", "00"}}, QStringLiteral("10.9.9.9"), ms0 + i, &out, &g); if (last == 404) ++n; }
    CHECK(last == 429 && n == kPerSourcePerMin, "rate limit: %d per source and minute", kPerSourcePerMin);
    CHECK(b4.request(mreq, QStringLiteral("10.9.9.10"), ms0, &out, &g) == 202, "rate limit: per source");
    CHECK(b4.request(mreq, QStringLiteral("10.9.9.9"), ms0 + 61000, &out, &g) == 202, "rate limit: a minute later the source may try again");

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all link checks passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
