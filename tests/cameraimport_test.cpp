// SPDX-License-Identifier: Apache-2.0
// The camera map's bulk sources (src/cameraimport.h): the streaming GeoJSON reader in any chunking, the DeFlock and
// flocklocations row mapping, DeFlock-style direction parsing and the §2.0 classification of the imported rows.
// Usage: cameraimport_test <deflock_sample.geojson>
#include "../src/cameraimport.h"
#include "../src/plateevents.h"
#include <QFile>
#include <QJsonDocument>
#include <cmath>
#include <cstdio>

static int fails = 0, checks = 0;
#define CHECK(cond, fmt, ...) do { ++checks; \
    if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } \
} while (0)

static QList<QJsonObject> readAll(const QByteArray &doc, int chunk, CameraImport::FeatureStream *fs)
{
    QList<QJsonObject> out;
    for (qsizetype i = 0; i < doc.size(); i += chunk) {
        fs->feed(doc.mid(i, chunk));
        QJsonObject f;
        while (fs->next(&f)) out << f;
    }
    return out;
}

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

int main(int argc, char **argv)
{
    if (argc < 2) { std::printf("usage: cameraimport_test <deflock_sample.geojson>\n"); return 2; }
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::ReadOnly)) { std::printf("cannot read %s\n", argv[1]); return 2; }
    const QByteArray doc = file.readAll();

    // 1. the reader: every chunking gives the same 6 features, the decoy "features" strings / keys are skipped
    for (int chunk : {1, 2, 3, 7, 10, 64, 4096, 1 << 20}) {
        CameraImport::FeatureStream fs;
        const QList<QJsonObject> f = readAll(doc, chunk, &fs);
        CHECK(f.size() == 6 && fs.done() && fs.bad() == 0, "chunk %d: %d features, done %d", chunk, int(f.size()), int(fs.done()));
        if (f.size() == 6)
            CHECK(f[2].value(QLatin1String("properties")).toObject().value(QLatin1String("operator")).toString()
                  == QLatin1String("Town of \"Example\" {braces} [brackets] \\ backslash"), "chunk %d: escapes and brackets inside strings", chunk);
    }
    {
        CameraImport::FeatureStream fs;
        fs.feed("{\"type\":\"FeatureCollection\",\"features\":[{\"a\":1},  oops ,{\"b\":2}]}");
        QJsonObject f; int n = 0;
        while (fs.next(&f)) ++n;
        CHECK(n == 2 && fs.done(), "garbage between features is skipped (%d)", n);
        CameraImport::FeatureStream none;
        none.feed("{\"type\":\"FeatureCollection\",\"name\":\"x\"}");
        CHECK(!none.started() && !none.next(&f), "no features array: nothing");
    }

    // 2. DeFlock rows
    CameraImport::FeatureStream fs;
    const QList<QJsonObject> feats = readAll(doc, 4096, &fs);
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-02T12:00:00"), Qt::ISODate);
    QList<FlockCamera> cams;
    for (const QJsonObject &f : feats) { FlockCamera c; if (CameraImport::fromDeflock(f, now, &c)) cams << c; }
    CHECK(cams.size() == 4, "4 usable rows (no osmId and 0,0 dropped): %d", int(cams.size()));
    if (cams.size() == 4) {
        const FlockCamera &a = cams[0];
        CHECK(a.id == QLatin1String("osm:node/1001") && a.source == QLatin1String("deflock") && near(a.lat, 40.0) && near(a.lon, -75.0), "id / source / position: %s", qPrintable(a.id));
        CHECK(a.operatorName == QLatin1String("Example Police Department") && a.manufacturer == QLatin1String("Flock Safety"), "operator and brand → manufacturer");
        CHECK(a.model.isEmpty(), "no invented model: '%s'", qPrintable(a.model));
        CHECK(a.osmVersion == 6 && a.osmTimestamp == QLatin1String("2026-05-26T03:54:51Z"), "the real OSM version / timestamp");
        CHECK(a.direction == QLatin1String("270") && a.cameraType == QLatin1String("alpr"), "direction %s type %s", qPrintable(a.direction), qPrintable(a.cameraType));
        CHECK(a.notes == QString::fromUtf8("zone traffic · mount pole"), "zone / mount noted: %s", qPrintable(a.notes));
        CHECK(cams[1].id == QLatin1String("osm:way/1002") && cams[1].direction == QLatin1String("90;270"), "a way, every direction: %s %s", qPrintable(cams[1].id), qPrintable(cams[1].direction));
        CHECK(cams[2].direction == QLatin1String("0") && PlateEvents::parseDirections(cams[2].direction).isEmpty(), "direction 0 is kept as given and means unknown");
        CHECK(cams[3].operatorName.isEmpty(), "no operator given: none invented ('%s')", qPrintable(cams[3].operatorName));
        CHECK(PlateEvents::classifyCamera(cams[3].model, cams[3].source, cams[3].id, cams[3].detectionMethod, QJsonObject()) == QLatin1String("alpr"),
              "a tagless DeFlock row is an ALPR");
        PlateEvents::Camera k; k.model = cams[1].model; k.manufacturer = cams[1].manufacturer;
        CHECK(PlateEvents::coneFor(k).name.contains(QLatin1String("Motorola")), "the manufacturer picks the cone: %s", qPrintable(PlateEvents::coneFor(k).name));
    }

    // 3. flocklocations: only the community rows (no osm_id)
    {
        const QJsonObject osmRow = QJsonDocument::fromJson(R"({"type":"Feature","geometry":{"type":"Point","coordinates":[-75.01,40.01]},
            "properties":{"id":7,"camera_type":"ALPR Camera","osm_id":"123","source":"openstreetmap"}})").object();
        const QJsonObject community = QJsonDocument::fromJson(R"({"type":"Feature","geometry":{"type":"Point","coordinates":[-75.02,40.02]},
            "properties":{"id":8,"address":"1 Example St","city":"Exampleton","state":"PA","camera_type":"Flock Safety Solar","verified":true,
            "reported_at":"2026-08-08T09:30:59.000Z","source":"community","osm_id":null}})").object();
        const QJsonObject suspected = QJsonDocument::fromJson(R"({"type":"Feature","geometry":{"type":"Point","coordinates":[-75.03,40.03]},
            "properties":{"id":9,"camera_type":"Other surveillance camera","source":"3rd Party / Suspected"}})").object();
        FlockCamera c;
        CHECK(!CameraImport::fromFlockLocations(osmRow, now, &c), "an OSM-derived row is DeFlock's, not imported here");
        CHECK(CameraImport::fromFlockLocations(community, now, &c), "a community row is imported");
        CHECK(c.id == QLatin1String("flock:8") && c.operatorName.isEmpty() && c.manufacturer.isEmpty() && c.model == QLatin1String("Flock Safety Solar"),
              "community row: id %s, no invented operator", qPrintable(c.id));
        CHECK(c.notes.startsWith(QLatin1String("1 Example St, Exampleton, PA")) && c.vetted && c.confidence == 95, "notes / vetting: %s", qPrintable(c.notes));
        CHECK(PlateEvents::classifyCamera(c.model, c.source, c.id, c.detectionMethod, QJsonObject()) == QLatin1String("alpr"), "a community Flock row is an ALPR");
        CHECK(CameraImport::fromFlockLocations(suspected, now, &c) && PlateEvents::classifyCamera(c.model, c.source, c.id, c.detectionMethod, QJsonObject()) == QLatin1String("cctv"),
              "\"Other surveillance camera\" is CCTV");
    }

    // 4. RF detections and the §2.0 classes
    CHECK(PlateEvents::classifyCamera(QStringLiteral("Raven"), QStringLiteral("ble_scan"), QStringLiteral("det:C6:11:22:33:44:55"), QStringLiteral("ble_uuid"), QJsonObject())
          == QLatin1String("not_camera"), "a Raven detection is a gunshot detector");
    CHECK(PlateEvents::classifyCamera(QStringLiteral("Falcon"), QStringLiteral("wifi_scan"), QStringLiteral("det:B4:1E:52:12:34:56"), QStringLiteral("wifi_mac"), QJsonObject())
          == QLatin1String("alpr"), "a Flock Wi-Fi detection is an ALPR");
    {
        const QJsonObject condor{{"man_made", "surveillance"}, {"surveillance:type", "camera"}, {"manufacturer", "Flock Safety"}};
        CHECK(PlateEvents::classifyCamera(QString(), QStringLiteral("osm"), QStringLiteral("osm:node/5"), QStringLiteral("osm_tag"), condor) == QLatin1String("ptz"), "a Flock Condor is a PTZ");
    }

    // 5. directions in DeFlock's grammar (lib.mjs): lists with ; or ,, spelled-out and bound directions, ranges
    auto dirs = [](const char *s) { return PlateEvents::parseDirections(QString::fromLatin1(s)); };
    CHECK(dirs("90,270").size() == 2 && near(dirs("90,270")[1], 270), "comma list");
    CHECK(dirs("NORTHEAST").size() == 1 && near(dirs("NORTHEAST")[0], 45), "spelled-out intercardinal");
    CHECK(dirs("southwest").size() == 1 && near(dirs("southwest")[0], 225), "lower case");
    CHECK(near(dirs("EB")[0], 90) && near(dirs("NB")[0], 0), "bound directions");
    CHECK(dirs("338-23").size() == 1 && near(dirs("338-23")[0], 0.5), "a range across north: %f", dirs("338-23").value(0));
    CHECK(near(dirs("WSW-ESE")[0], 0), "a cardinal range (clockwise WSW → ESE)");
    CHECK(dirs("0").isEmpty() && dirs("0-360").isEmpty() && dirs("").isEmpty(), "0 and the full circle are no direction");
    CHECK(dirs("N; 180").size() == 2, "N is a direction when written as N");

    std::printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
