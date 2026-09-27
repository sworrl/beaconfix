// The pediatric ER classifier (src/poiclassify.cpp) against the shared fixture that the
// Android PedsClassifier test reads too. Build (no CMake target, like the other tests):
//   g++ -std=c++17 -O2 -Wall -Wextra -fPIC $(pkg-config --cflags Qt6Core) tests/poiclassify_test.cpp src/poiclassify.cpp -o build/poiclassify_test $(pkg-config --libs Qt6Core)
//   build/poiclassify_test tests/fixtures/pediatric_tags.json
// Also: build/poiclassify_test --overpass <response.json> <lat> <lon>  classifies a saved Overpass
// answer and prints the medical places and the help picks (a manual check, not a test).
#include "../src/poiclassify.h"
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

using namespace PoiClassify;
static int fails = 0, checks = 0;
#define CHECK(cond, fmt, ...) do { ++checks; if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } } while (0)

static QJsonObject readJson(const char *path)
{
    QFile f(QString::fromLocal8Bit(path));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open %s\n", path); return {}; }
    return QJsonDocument::fromJson(f.readAll()).object();
}

static QList<HelpCandidate> candidates(const QList<Element> &els, const QList<Result> &res, double lat, double lon, QList<int> *index)
{
    QList<HelpCandidate> c;
    for (int i = 0; i < els.size(); ++i) {
        if (res[i].dropped || res[i].cat.isEmpty()) continue;
        HelpCandidate h; h.cat = res[i].cat; h.peds = res[i].peds; h.campus = res[i].campus; h.emergency = res[i].emergency;
        h.distM = distanceM(lat, lon, els[i].lat, els[i].lon);
        driveEstimate(h.distM, &h.driveS, nullptr);
        c << h; index->append(i);
    }
    return c;
}

static int overpassMode(const char *path, double lat, double lon)
{
    const QJsonObject o = readJson(path);
    QList<Element> els;
    for (const QJsonValue &v : o["elements"].toArray()) els << Element::fromOverpass(v.toObject());
    const QList<Result> res = classifyAll(els);
    std::printf("%d elements\n", int(els.size()));
    for (int i = 0; i < els.size(); ++i) {
        const Result &r = res[i];
        if (r.cat != QLatin1String("peds_er") && r.cat != QLatin1String("peds_urgent") && !(r.cat == QLatin1String("health") && r.emergency)) continue;
        std::printf("%-8s %-24s %-12s tier %d  %6.1f km  %s%s  [%s]\n", r.dropped ? "dropped" : "", qPrintable(els[i].key()), qPrintable(r.cat), r.peds,
                    distanceM(lat, lon, els[i].lat, els[i].lon) / 1000.0, qPrintable(els[i].tags["name"].toString()),
                    r.campus.isEmpty() ? "" : qPrintable(QStringLiteral(" (campus: ") + r.campus + QLatin1Char(')')), qPrintable(r.detail));
    }
    QList<int> idx;
    const QList<HelpCandidate> c = candidates(els, res, lat, lon, &idx);
    const HelpPicks p = pickHelp(c);
    auto show = [&](const char *what, int k) {
        if (k < 0) { std::printf("%-16s none\n", what); return; }
        const Element &e = els[idx[k]];
        std::printf("%-16s %s %s  %.1f km  ~%d min (est.)\n", what, qPrintable(e.key()), qPrintable(e.tags["name"].toString()), c[k].distM / 1000.0, c[k].driveS / 60);
    };
    show("pediatric", p.pediatric); show("pediatricCloser", p.pediatricCloser); show("pediatricUrgent", p.pediatricUrgent); show("hospital", p.hospital);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 5 && QByteArray(argv[1]) == "--overpass") return overpassMode(argv[2], QByteArray(argv[3]).toDouble(), QByteArray(argv[4]).toDouble());
    const QJsonObject fx = readJson(argc > 1 ? argv[1] : "tests/fixtures/pediatric_tags.json");
    const QJsonArray scenarios = fx["scenarios"].toArray();
    CHECK(!scenarios.isEmpty(), "fixture has scenarios");

    for (const QJsonValue &sv : scenarios) {
        const QJsonObject s = sv.toObject();
        const QString name = s["name"].toString();
        const int before = fails;
        QList<Element> els;
        for (const QJsonValue &v : s["elements"].toArray()) {
            const QJsonObject e = v.toObject();
            Element el; el.type = e["type"].toString(); el.id = qint64(e["id"].toDouble()); el.lat = e["lat"].toDouble(); el.lon = e["lon"].toDouble(); el.tags = e["tags"].toObject();
            els << el;
        }
        const QList<Result> res = classifyAll(els);
        CHECK(res.size() == els.size(), "%s: one result per element", qPrintable(name));
        QHash<QString, int> byKey;
        for (int i = 0; i < els.size(); ++i) byKey.insert(els[i].key(), i);

        for (const QJsonValue &xv : s["expect"].toArray()) {
            const QJsonObject x = xv.toObject();
            const QString id = x["id"].toString();
            const int i = byKey.value(id, -1);
            CHECK(i >= 0, "%s: %s is in the elements", qPrintable(name), qPrintable(id));
            if (i < 0) continue;
            const Result &r = res[i];
            const QByteArray label = QStringLiteral("%1: %2 (%3)").arg(name, id, els[i].tags["name"].toString()).toUtf8();
            if (x["dropped"].toBool()) { CHECK(r.dropped, "%s is dropped as a duplicate", label.constData()); continue; }
            CHECK(!r.dropped, "%s is kept", label.constData());
            if (x.contains("cat")) CHECK(r.cat == x["cat"].toString(), "%s cat %s, expected %s", label.constData(), qPrintable(r.cat), qPrintable(x["cat"].toString()));
            if (x.contains("catNot")) for (const QJsonValue &n : x["catNot"].toArray())
                CHECK(r.cat != n.toString(), "%s cat %s must not be %s", label.constData(), qPrintable(r.cat), qPrintable(n.toString()));
            if (x.contains("peds")) CHECK(r.peds == x["peds"].toInt(), "%s peds %d, expected %d", label.constData(), r.peds, x["peds"].toInt());
            if (x.contains("er")) CHECK(r.er == x["er"].toString(), "%s er '%s', expected '%s'", label.constData(), qPrintable(r.er), qPrintable(x["er"].toString()));
            if (x.contains("campusEr")) CHECK(r.campus == x["campusEr"].toString(), "%s campus '%s', expected '%s'", label.constData(), qPrintable(r.campus), qPrintable(x["campusEr"].toString()));
            if (x.contains("emergency")) CHECK(r.emergency == x["emergency"].toBool(), "%s emergency %d, expected %d", label.constData(), int(r.emergency), int(x["emergency"].toBool()));
            if (x.contains("phone")) CHECK(r.phone == x["phone"].toString(), "%s phone '%s', expected '%s'", label.constData(), qPrintable(r.phone), qPrintable(x["phone"].toString()));
            if (x.contains("detailContains")) CHECK(r.detail.contains(x["detailContains"].toString()), "%s detail '%s' contains '%s'", label.constData(), qPrintable(r.detail), qPrintable(x["detailContains"].toString()));
        }

        const QJsonObject origin = s["origin"].toObject(), help = s["help"].toObject();
        QList<int> idx;
        const QList<HelpCandidate> c = candidates(els, res, origin["lat"].toDouble(), origin["lon"].toDouble(), &idx);
        const HelpPicks p = pickHelp(c);
        auto keyOf = [&](int k) { return k < 0 ? QStringLiteral("null") : els[idx[k]].key(); };
        const QList<QPair<const char *, int>> picks{{"pediatric", p.pediatric}, {"pediatricCloser", p.pediatricCloser}, {"hospital", p.hospital}, {"pediatricUrgent", p.pediatricUrgent}};
        for (const auto &pk : picks) {
            if (!help.contains(QLatin1String(pk.first))) continue;
            const QJsonValue want = help[QLatin1String(pk.first)];
            const QString w = want.isNull() ? QStringLiteral("null") : want.toString();
            CHECK(keyOf(pk.second) == w, "%s: help.%s = %s, expected %s", qPrintable(name), pk.first, qPrintable(keyOf(pk.second)), qPrintable(w));
        }
        std::printf("%s scenario %s\n", fails == before ? "ok  " : "FAIL", qPrintable(name));
    }

    // Unit bits the fixture doesn't reach
    int s = 0, m = 0;
    driveEstimate(0, &s, &m); CHECK(s == 300 && m == 0, "drive estimate: at least 5 minutes (%d s)", s);
    driveEstimate(100000, &s, &m); CHECK(m == 140000 && s == 7200, "drive estimate: 100 km → 140 km road, 2 h (%d s, %d m)", s, m);
    CHECK(pedsRank(2, QStringLiteral("Some ER")) == 0 && pedsRank(2, QString()) == 1 && pedsRank(3, QString()) == 1 && pedsRank(1, QString()) == 0 && pedsRank(4, QString()) == -1, "pedsRank");
    CHECK(baseCategory(QJsonObject{{"healthcare", "hospital"}}) == QLatin1String("health"), "healthcare=hospital alone is a hospital");
    CHECK(classify(QJsonObject{{"amenity", "hospital"}, {"name", "Anytown General"}, {"opening_hours", "24/7"}}).emergency, "a general hospital open 24/7 counts as an ER");
    CHECK(!classify(QJsonObject{{"amenity", "hospital"}, {"name", "Anytown General"}, {"opening_hours", "24/7"}, {"emergency", "no"}}).emergency, "emergency=no beats 24/7");

    // The Urgent care help pick: an actual urgent care, not the nearest clinic (a chiropractor was shown under it)
    CHECK(isUrgentCare(QStringLiteral("Test Health Center"), QStringLiteral("urgent care")), "tagged urgent_care (detail \"urgent care\")");
    CHECK(isUrgentCare(QStringLiteral("Test MedExpress"), QStringLiteral("clinic")), "a MedExpress by name");
    CHECK(isUrgentCare(QStringLiteral("Test Walk-In Clinic"), QString()) && isUrgentCare(QStringLiteral("Test After Hours Care"), QString())
          && isUrgentCare(QStringLiteral("Test Immediate Care"), QString()), "walk-in, after hours, immediate care");
    CHECK(!isUrgentCare(QStringLiteral("Test Chiropractic"), QStringLiteral("chiropractic")), "a chiropractor is not urgent care");
    CHECK(!isUrgentCare(QStringLiteral("Test Family Practice"), QStringLiteral("doctor's office")), "a doctor's office is not urgent care");
    CHECK(!isUrgentCare(QStringLiteral("Test Urgently Needed Supplies"), QStringLiteral("clinic")), "a word containing 'urgent' is not enough");

    std::printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
