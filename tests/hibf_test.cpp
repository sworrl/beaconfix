// SPDX-License-Identifier: Apache-2.0
// Unit tests for the HaveIBeenFlocked watcher's pure parts (docs/SIGHTINGS.md §4): the site's hashing and O↔0 / I↔1
// expansion, the full-hash result filter (fixture), the leaky-agency list and matching (fixture), the backoff.
#include "../src/hibf.h"
#include <QFile>
#include <QJsonDocument>
#include <cstdio>

static int fails = 0;
#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } \
    else std::printf("ok   " fmt "\n", ##__VA_ARGS__); \
} while (0)

static QByteArray fixture(const char *dir, const char *name)
{
    QFile f(QString::fromLocal8Bit(dir) + QLatin1Char('/') + QLatin1String(name));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests/fixtures";
    // ── hashing: sha256(lower(trim(variant)))[:8], as the site's r1() ──
    CHECK(Hibf::hashPrefix(QStringLiteral("XYZ2345")) == "9f89b1d8", "XYZ2345 → 9f89b1d8 (%s)", qPrintable(Hibf::hashPrefix(QStringLiteral("XYZ2345"))));
    CHECK(Hibf::hashPrefix(QStringLiteral("XYZ-2345")) == "37b3a9d1", "XYZ-2345 → 37b3a9d1");
    CHECK(Hibf::hashPrefix(QStringLiteral("  xyz2345 ")) == "9f89b1d8", "trimmed and lower-cased");
    CHECK(Hibf::fullHash(QStringLiteral("XYZ2345")) == "9f89b1d8ade18387e252af469ed38aa4532ba9d19727a575b5fc51f6d5ea8223", "full hash");
    // ── variants: the display and the alnum form, each expanded O↔0 / I↔1 like the site's a1() ──
    CHECK(Hibf::expandOI(QStringLiteral("O1A")) == QStringList({"O1A", "OIA", "0IA", "01A"}), "O1A → %s", qPrintable(Hibf::expandOI(QStringLiteral("O1A")).join(',')));
    CHECK(Hibf::expandOI(QStringLiteral("abc")) == QStringList({"ABC"}), "no O/0/I/1: the upper-cased plate only");
    CHECK(Hibf::expandOI(QStringLiteral("OOOOO")).size() == 10 && Hibf::expandOI(QStringLiteral("OOOOO")).first() == "OOOOO", "≤ 10 per form, the original first");
    CHECK(Hibf::expandOI(QStringLiteral("0I"), 10) == QStringList({"0I", "OI", "O1", "01"}), "0I → %s", qPrintable(Hibf::expandOI(QStringLiteral("0I")).join(',')));
    CHECK(Hibf::plateForms(QStringLiteral("XYZ-2345")) == QStringList({"XYZ-2345", "XYZ2345"}), "display + alnum forms");
    CHECK(Hibf::plateForms(QStringLiteral("AB 12&3")) == QStringList({"AB123"}), "whitespace and & removed like the site; alnum the same → one form");
    const QList<Hibf::Variant> vars = Hibf::variants({QStringLiteral("XYZ-2345")});
    CHECK(Hibf::prefixes(vars) == QStringList({"37b3a9d1", "9f89b1d8"}), "XYZ-2345 sends [%s]", qPrintable(Hibf::prefixes(vars).join(',')));
    const QList<Hibf::Variant> v2 = Hibf::variants({QStringLiteral("B0B-1")});
    CHECK(v2.size() == 8, "B0B-1: 4 display + 4 alnum variants (%d)", int(v2.size()));
    // ── the result filter: full hash equal to one of ours; rows without a full hash kept unverified ──
    const QJsonObject page = QJsonDocument::fromJson(fixture(dir, "hibf_search.json")).object();
    CHECK(page["results"].toArray().size() == 3, "fixture has 3 rows");
    const QList<Hibf::Match> m = Hibf::filterResults(page["results"].toArray(), vars);
    CHECK(m.size() == 2, "another plate behind the same prefix is dropped (%d kept)", int(m.size()));
    if (m.size() == 2) {
        CHECK(m[0].verified && m[0].plate == "XYZ-2345", "row 1: verified by its full hash");
        CHECK(!m[1].verified, "row 3: no full hash → unverified");
        const QJsonObject e0 = Hibf::searchEvent(m[0]), e1 = Hibf::searchEvent(m[1]);
        CHECK(e0["uid"].toString() == "hibf:3679b82b23a3ba844fe55986", "uid %s", qPrintable(e0["uid"].toString()));
        CHECK(e0["kind"] == "plate_search" && e0["confidence"].toInt() == 100 && e0["agency"] == "Example County SO", "plate_search, confidence 100, agency");
        CHECK(e0["details"].toString() == "Example County SO searched XYZ-2345 — investigation (case 25-0001)", "details: %s", qPrintable(e0["details"].toString()));
        CHECK(e0["source_url"].toString().startsWith("https://www.muckrock.com/") && e0["source_name"].toString() == "HaveIBeenFlocked · Example County SO audit log", "source link and name");
        CHECK(e0["metrics"].toObject()["total_devices_searched"].toInt() == 1200 && e0["metrics"].toObject()["hashVerified"].toBool(), "metrics keep every field");
        CHECK(e0["raw"].toObject() == page["results"].toArray()[0].toObject(), "raw = the row");
        CHECK(e1["confidence"].toInt() == 50 && !e1["metrics"].toObject()["hashVerified"].toBool(true), "unverified: confidence 50");
        CHECK(e1["source_url"].toString() == "https://haveibeenflocked.com/", "a non-http(s) source_url falls back to the site");
        CHECK(e1["uid"].toString() == "hibf:83e5f9cb18a40417e603edb5", "missing fields are empty strings in the uid (%s)", qPrintable(e1["uid"].toString()));
        CHECK(!e0["time"].toString().endsWith('Z') && e0["time"].toString().size() == 19, "time converted to local ISO: %s", qPrintable(e0["time"].toString()));
    }
    // ── leaky agencies: tokens from the file name and the source URL ──
    QString st;
    CHECK(Hibf::fileTokens(QStringLiteral("Chehalis WA PD_Network_Audit_12_1_2024_1_1_2025 (1).csv"), QString(), &st) == QStringList({"chehalis"}) && st == "WA",
          "Chehalis WA PD file → [chehalis], WA");
    st.clear();
    CHECK(Hibf::fileTokens(QStringLiteral("1_1_2022-1_31_2022-Santa Clara_clean.csv"), QStringLiteral("https://haveibeenflocked.com/news/santa-clara-co-ca-jun2026"), &st)
          == QStringList({"clara", "santa"}) && st == "CA", "Santa Clara + slug → [clara, santa], CA (%s)", qPrintable(st));
    const QJsonObject src = Hibf::parseSources(fixture(dir, "hibf_sources.json"), QStringLiteral("2026-10-02T21:00:00"));
    const QJsonArray ag = src["agencies"].toArray();
    CHECK(src["files"].toInt() == 11 && ag.size() >= 8 && ag.size() <= 10, "11 files → %d agencies (the portal / undated files without a name drop out)", int(ag.size()));
    CHECK(!Hibf::leakyMatch(QStringLiteral("Chehalis Police Department"), QString(), ag).isEmpty(), "Chehalis Police Department is leaky");
    CHECK(Hibf::leakyMatch(QStringLiteral("Chehalis Police Department"), QString(), ag).contains("muckrock"), "…matched through the file and its URL");
    CHECK(!Hibf::leakyMatch(QStringLiteral("City of Bryan"), QStringLiteral("TX"), ag).isEmpty(), "City of Bryan (TX) is leaky");
    CHECK(Hibf::leakyMatch(QStringLiteral("Bryan Police Department"), QStringLiteral("OH"), ag).isEmpty(), "a Bryan in another state is not");
    CHECK(!Hibf::leakyMatch(QStringLiteral("Mount Prospect Police"), QString(), ag).isEmpty(), "Mount Prospect");
    CHECK(!Hibf::leakyMatch(QStringLiteral("Ventura Police Department"), QString(), ag).isEmpty(), "Ventura (only the MuckRock URL names it)");
    CHECK(Hibf::leakyMatch(QStringLiteral("Flock Safety"), QString(), ag).isEmpty(), "operator Flock Safety names no agency");
    CHECK(Hibf::leakyMatch(QStringLiteral("West Virginia Department of Transportation"), QString(), ag).isEmpty(), "WVDOT is not in the fixture");
    CHECK(Hibf::leakyMatch(QStringLiteral("Salem"), QStringLiteral("OR"), ag).isEmpty() && !Hibf::leakyMatch(QStringLiteral("Salem"), QStringLiteral("MA"), ag).isEmpty(), "state check");
    CHECK(Hibf::stateFromText(QStringLiteral("123 Main St, Bryan, TX")) == "TX" && Hibf::stateFromText(QStringLiteral("traffic")).isEmpty(), "state from an address");
    // ── schedule ──
    CHECK(Hibf::intervalSecs(Hibf::Mode::Idle) == 7 * 86400 && Hibf::intervalSecs(Hibf::Mode::Driving) == 86400 && Hibf::intervalSecs(Hibf::Mode::Flock) == 43200
          && Hibf::intervalSecs(Hibf::Mode::Leaky) == 10800, "intervals 7 d / 24 h / 12 h / 3 h");
    CHECK(Hibf::backoffSecs(1) == 3600 && Hibf::backoffSecs(2) == 7200 && Hibf::backoffSecs(5) == 57600 && Hibf::backoffSecs(6) == 86400 && Hibf::backoffSecs(20) == 86400,
          "backoff ×2 from 1 h up to 24 h");
    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
