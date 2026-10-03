// SPDX-License-Identifier: Apache-2.0
// The surveillance signatures (data/signatures/surveillance.json) over the shared cases in
// tests/fixtures/surveillance_cases.json — the Android unit test runs the same cases (SurveillanceSignaturesTest.kt).
// Usage: flockdetector_test <signatures.json> <surveillance_cases.json>
#include "../src/flockdetector.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>

static int fails = 0, checks = 0;
#define CHECK(cond, fmt, ...) do { ++checks; \
    if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } \
} while (0)

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static QStringList strings(const QJsonValue &v)
{
    QStringList out;
    for (const QJsonValue &x : v.toArray()) out << x.toString();
    return out;
}

static void expect(const QJsonObject &c, const FlockDetector::Detection &d)
{
    const QByteArray name = c.value(QLatin1String("case")).toString().toUtf8();
    const int tier = c.value(QLatin1String("tier")).toInt();
    CHECK(d.tier == tier, "%s: tier %d, want %d (%s)", name.constData(), d.tier, tier, qPrintable(d.rules.join(QLatin1Char(','))));
    if (tier >= 0) CHECK(d.cls == c.value(QLatin1String("class")).toString(), "%s: class %s", name.constData(), qPrintable(d.cls));
    CHECK(d.isFlock == c.value(QLatin1String("isFlock")).toBool(), "%s: isFlock %d", name.constData(), int(d.isFlock));
    CHECK(d.informational == c.value(QLatin1String("informational")).toBool(), "%s: informational %d", name.constData(), int(d.informational));
    if (tier < 0) CHECK(d.method.isEmpty() && d.confidence == 0, "%s: no match has no method / confidence", name.constData());
}

int main(int argc, char **argv)
{
    // the override lives in the data directory: point it at a scratch one before Qt resolves it
    QTemporaryDir data;
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("sworrl"));
    QCoreApplication::setApplicationName(QStringLiteral("beaconfix"));
    if (argc < 3) { std::printf("usage: flockdetector_test <signatures.json> <surveillance_cases.json>\n"); return 2; }
    const QByteArray sigJson = readFile(QString::fromLocal8Bit(argv[1]));
    QString err;
    const FlockDetector::Signatures *sig = FlockDetector::parseSignatures(sigJson, &err);
    CHECK(sig, "the signature file parses: %s", qPrintable(err));
    if (!sig) return 1;

    // 1. the shared cases
    const QJsonObject cases = QJsonDocument::fromJson(readFile(QString::fromLocal8Bit(argv[2]))).object();
    int wifiCases = 0, bleCases = 0;
    for (const QJsonValue &v : cases.value(QLatin1String("wifi")).toArray()) {
        const QJsonObject c = v.toObject();
        expect(c, FlockDetector::evaluateWifi(*sig, c.value(QLatin1String("bssid")).toString(), c.value(QLatin1String("ssid")).toString(), strings(c.value(QLatin1String("probes")))));
        ++wifiCases;
    }
    for (const QJsonValue &v : cases.value(QLatin1String("ble")).toArray()) {
        const QJsonObject c = v.toObject();
        QList<FlockDetector::BleCompany> comp;
        for (const QJsonValue &x : c.value(QLatin1String("companies")).toArray())
            comp.append({x.toObject().value(QLatin1String("id")).toInt(), x.toObject().value(QLatin1String("ascii")).toString().toLatin1()});
        expect(c, FlockDetector::evaluateBle(*sig, c.value(QLatin1String("mac")).toString(), c.value(QLatin1String("name")).toString(), strings(c.value(QLatin1String("services"))), comp));
        ++bleCases;
    }
    CHECK(wifiCases >= 30 && bleCases >= 20, "fixture cases present: %d Wi-Fi, %d BLE", wifiCases, bleCases);

    // 2. what a detection carries
    {
        const auto d = FlockDetector::evaluateWifi(*sig, QStringLiteral("70:C9:4E:00:00:01"), QStringLiteral("Flock-112233"));
        CHECK(d.method == QLatin1String("wifi_mac+ssid"), "Flock SSID + Liteon: method %s", qPrintable(d.method));
        CHECK(d.model == QLatin1String("Falcon") && d.cameraType == QLatin1String("alpr") && d.confidence == 85, "model %s type %s confidence %d", qPrintable(d.model), qPrintable(d.cameraType), d.confidence);
        const auto r = FlockDetector::evaluateBle(*sig, QStringLiteral("C6:11:22:33:44:55"), QString(), {QStringLiteral("3100")});
        CHECK(r.model == QLatin1String("Raven") && r.cameraType == QLatin1String("not_camera") && r.method == QLatin1String("ble_uuid"), "Raven: model %s type %s method %s", qPrintable(r.model), qPrintable(r.cameraType), qPrintable(r.method));
        const auto x = FlockDetector::evaluateWifi(*sig, QStringLiteral("00:03:7F:50:00:01"), QString());
        CHECK(x.confidence == 98 && x.details.contains(QLatin1String("tier 4")), "conclusive: %d %s", x.confidence, qPrintable(x.details));
    }

    // 3. the loader refuses what must never match
    {
        QJsonObject o = QJsonDocument::fromJson(sigJson).object();
        QJsonArray mac = o.value(QLatin1String("mac")).toArray();
        mac.append(QJsonObject{{"prefix", "70:B3:D5"}, {"class", "flock"}, {"tier", 0}});
        o["mac"] = mac;
        CHECK(!FlockDetector::parseSignatures(QJsonDocument(o).toJson(), &err) && err.contains(QLatin1String("MA-S")), "a bare 70:B3:D5 entry is refused: %s", qPrintable(err));
        QJsonObject p = QJsonDocument::fromJson(sigJson).object();
        QJsonArray mac2 = p.value(QLatin1String("mac")).toArray();
        mac2.append(QJsonObject{{"prefix", "12:34:56:78"}, {"class", "flock"}, {"tier", 0}});
        p["mac"] = mac2;
        CHECK(!FlockDetector::parseSignatures(QJsonDocument(p).toJson(), &err), "a 32-bit prefix is refused");
        CHECK(!FlockDetector::parseSignatures("{\"format\":2}", &err), "an unknown format is refused");
    }

    // 4. compiled in, and overridable from the data directory when valid and at least as new
    {
        CHECK(readFile(QStringLiteral(":/signatures/surveillance.json")) == sigJson, "the compiled-in file is data/signatures/surveillance.json");
        const FlockDetector::Signatures *b = FlockDetector::loadSignatures();
        CHECK(FlockDetector::signaturesVersion(*b) == FlockDetector::signaturesVersion(*sig), "no override: the compiled-in set (v%d)", FlockDetector::signaturesVersion(*b));
        QDir().mkpath(QFileInfo(FlockDetector::overridePath()).absolutePath());
        auto writeOverride = [](const QByteArray &bytes) { QFile f(FlockDetector::overridePath()); if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(bytes); };
        QJsonObject o = QJsonDocument::fromJson(sigJson).object();
        QJsonArray ssid = o.value(QLatin1String("ssid")).toArray();
        ssid.append(QJsonObject{{"id", "test"}, {"regex", "^OverrideTest$"}, {"class", "flock"}, {"tier", 3}});
        o["ssid"] = ssid;
        o["version"] = FlockDetector::signaturesVersion(*sig) + 1;
        writeOverride(QJsonDocument(o).toJson());
        const FlockDetector::Signatures *n = FlockDetector::loadSignatures();
        CHECK(FlockDetector::evaluateWifi(*n, QStringLiteral("02:00:00:00:00:01"), QStringLiteral("OverrideTest")).tier == 3, "a newer override is used");
        o["version"] = FlockDetector::signaturesVersion(*sig) - 1;
        writeOverride(QJsonDocument(o).toJson());
        CHECK(FlockDetector::evaluateWifi(*FlockDetector::loadSignatures(), QStringLiteral("02:00:00:00:00:01"), QStringLiteral("OverrideTest")).tier == -1, "an older override is ignored");
        writeOverride("{ not json");
        CHECK(FlockDetector::signaturesVersion(*FlockDetector::loadSignatures()) == FlockDetector::signaturesVersion(*sig), "a broken override is ignored");
    }

    // 5. MAC notation
    CHECK(FlockDetector::normalizeMac(QStringLiteral("b4-1e-52-12-34-56")) == QLatin1String("B41E52123456"), "dashes");
    CHECK(FlockDetector::normalizeMac(QStringLiteral("b41e.5212.3456")) == QLatin1String("B41E52123456"), "dots");
    CHECK(FlockDetector::normalizeMac(QStringLiteral("B4:1E:52")).isEmpty(), "a prefix is not a MAC");

    std::printf("%d checks, %d failed (%d Wi-Fi + %d BLE shared cases)\n", checks, fails, wifiCases, bleCases);
    return fails ? 1 : 0;
}
