// SPDX-License-Identifier: Apache-2.0
// Unit tests for camera trust (docs/SIGHTINGS.md §2.7) and the Eyes on Flock agency join (§4.6): the log-odds terms,
// the portal parser on a synthetic answer, and the operator / audit-log name → portal match (state, sheriff vs police,
// ambiguity). All agencies here are made up.
#include "../src/cameratrust.h"
#include "../src/eyesonflock.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } \
    else std::printf("ok   " fmt "\n", ##__VA_ARGS__); \
} while (0)

static bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

int main()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-01T12:00:00Z"), Qt::ISODate);
    // ── §2.7 trust ──
    {
        CameraTrust::Inputs in; in.id = QStringLiteral("osm:node/1"); in.source = QStringLiteral("deflock"); in.now = now;
        CameraTrust::Result r = CameraTrust::compute(in);
        CHECK(near(r.logit, 2.2) && near(r.trust, 1.0 / (1.0 + std::exp(-2.2))), "OSM / DeFlock prior +2.2 → %.3f", r.trust);
        CHECK(r.detail.value(QLatin1String("weights")).toString().contains(QLatin1String("not measured")), "the detail says the weights are suggested");

        in.osmTimestamp = QStringLiteral("2022-10-01T12:00:00Z");
        r = CameraTrust::compute(in);
        CHECK(near(r.logit, 2.2 - 0.25 * 4.0, 0.01), "−0.25 per year since the OSM edit (4 years): logit %.3f", r.logit);

        in.rfTier = 3; in.rfDistanceM = 20;
        r = CameraTrust::compute(in);
        CHECK(near(r.logit, 2.2 - 1.0 + 1.5, 0.01), "+1.5 for Flock hardware detected by RF at tier ≥ 2");
        in.rfTier = 1;
        r = CameraTrust::compute(in);
        CHECK(near(r.logit, 2.2 - 1.0 + 0.7, 0.01), "+0.7 at a lower tier");
        in.rfTier = -1;

        in.verdict = QStringLiteral("present");
        CHECK(near(CameraTrust::compute(in).logit, 2.2 - 1.0 + 1.0, 0.01), "user confirm +1");
        in.verdict = QStringLiteral("absent");
        r = CameraTrust::compute(in);
        CHECK(near(r.logit, 2.2 - 1.0 - 1.5, 0.01) && r.trust < 0.5, "\"not there\" −1.5 → %.3f", r.trust);
        CHECK(r.detail.value(QLatin1String("terms")).toArray().size() == 3, "terms: prior, age, user");

        CameraTrust::Inputs c; c.id = QStringLiteral("flock:7"); c.source = QStringLiteral("community"); c.now = now;
        CHECK(near(CameraTrust::compute(c).logit, 1.0), "community prior +1.0");
        CameraTrust::Inputs s; s.id = QStringLiteral("flock:8"); s.source = QStringLiteral("3rd Party / Suspected"); s.now = now;
        CHECK(near(CameraTrust::compute(s).logit, -0.5), "suspected prior −0.5");
        CameraTrust::Inputs d; d.id = QStringLiteral("det:B4:1E:52:00:00:01"); d.source = QStringLiteral("wifi_scan"); d.rfTier = 2; d.rfDistanceM = 0; d.now = now;
        CHECK(near(CameraTrust::compute(d).logit, 1.5), "our own RF detection: prior 0 + RF 1.5");
        CHECK(CameraTrust::tierFromConfidence(98) == 4 && CameraTrust::tierFromConfidence(65) == 2 && CameraTrust::tierFromConfidence(40) == 1
              && CameraTrust::tierFromConfidence(15) == 0, "tier from the detection confidence");
    }

    // ── §4.6 Eyes on Flock ──
    const QByteArray json = R"({"summary":{"total_portals_found":5,"total_cameras":200,"snapshot_date":{"$date":"2026-09-23T10:15:16.751Z"}},
      "portals":[
       {"portal_url":"https://transparency.flocksafety.com/exampleville-pa-pd","slug":"exampleville-pa-pd","city":"Exampleville","county":null,"state":"PA",
        "type":"PD","population":12000,"total_cameras":14,"total_searches":321,"data_retention":30,"vehicles_captured":50000,"hotlist_hits":12,
        "organization_count":77,"organizations_shared_with":["A","B"],"receiving_organization_count":null,"organizations_received_from":["C"],
        "prohibited_uses":"Immigration enforcement","public_search_audit":true,"data_last_updated":"2026-09-20T00:00:00Z"},
       {"portal_url":"https://transparency.flocksafety.com/sample-county-pa-so","slug":"sample-county-pa-so","city":null,"county":"Sample","state":"PA",
        "type":"SD","total_cameras":40,"total_searches":1000,"data_retention":45,"organization_count":12,"organizations_shared_with":[]},
       {"portal_url":"https://transparency.flocksafety.com/sample-pa-pd","slug":"sample-pa-pd","city":"Sample","county":null,"state":"PA",
        "type":"PD","total_cameras":9,"total_searches":10,"data_retention":30,"organization_count":3,"organizations_shared_with":[]},
       {"portal_url":"https://transparency.flocksafety.com/twin-oaks-oh-pd","slug":"twin-oaks-oh-pd","city":"Twin Oaks","state":"OH","type":"PD",
        "total_cameras":5,"data_retention":30,"organization_count":1,"organizations_shared_with":[]},
       {"portal_url":"https://transparency.flocksafety.com/twin-oaks-mo-pd","slug":"twin-oaks-mo-pd","city":"Twin Oaks","state":"MO","type":"PD",
        "total_cameras":3,"data_retention":30,"organization_count":2,"organizations_shared_with":[]}]})";
    const EyesOnFlock::Parsed parsed = EyesOnFlock::parse(json);
    CHECK(parsed.error.isEmpty() && parsed.portals.size() == 5, "5 portals parsed");
    CHECK(parsed.summary.value(QLatin1String("snapshot")).toString().startsWith(QLatin1String("2026-09-23")), "the snapshot date");
    const QList<EyesOnFlock::Portal> &ps = parsed.portals;
    if (ps.size() == 5) {
        CHECK(ps[0].retentionDays == 30 && ps[0].searches == 321 && ps[0].sharedWith == 77 && ps[0].receivedFrom == 1 && ps[0].publicAudit,
              "retention, searches, shared-with count, received-from (from the list when the count is null)");
        CHECK(ps[0].tokens == QStringList{QStringLiteral("exampleville")} && ps[1].tokens == QStringList{QStringLiteral("sample")}, "place tokens");
        bool verified = false;
        CHECK(EyesOnFlock::match(QStringLiteral("Exampleville Police Department"), QStringLiteral("PA"), ps, &verified) == 0 && verified,
              "OSM operator \"Exampleville Police Department\" in PA → its portal");
        CHECK(EyesOnFlock::match(QStringLiteral("Exampleville PA PD"), QString(), ps, &verified) == 0 && verified,
              "an audit-log agency \"Exampleville PA PD\": the state comes from the name");
        CHECK(EyesOnFlock::match(QStringLiteral("Exampleville Police Department"), QStringLiteral("OH"), ps) == -1, "another state: no match");
        CHECK(EyesOnFlock::match(QStringLiteral("Sample County Sheriff's Office"), QStringLiteral("PA"), ps) == 1, "a sheriff → the sheriff's portal, not the city police");
        CHECK(EyesOnFlock::match(QStringLiteral("Sample Police"), QStringLiteral("PA"), ps) == 2, "the city police → the PD portal");
        QString why;
        CHECK(EyesOnFlock::match(QStringLiteral("Twin Oaks Police"), QString(), ps, &verified, &why) == -1 && why.contains(QLatin1String("ambiguous")),
              "no state and two states' portals: ambiguous (%s)", qPrintable(why));
        CHECK(EyesOnFlock::match(QStringLiteral("Exampleville Police"), QString(), ps, &verified) == 0 && !verified, "no state but only one portal: matched, state not verified");
        CHECK(EyesOnFlock::match(QStringLiteral("Flock Safety"), QStringLiteral("PA"), ps) == -1, "\"Flock Safety\" names no agency");
        const QJsonObject f = EyesOnFlock::facts(ps[0]);
        CHECK(f.value(QLatin1String("retentionDays")).toInt() == 30 && f.value(QLatin1String("license")).toString() == QLatin1String("CC BY-SA 4.0")
              && f.value(QLatin1String("attribution")).toString().contains(QLatin1String("Eyes on Flock")), "facts carry the numbers and the CC BY-SA attribution");
        CHECK(EyesOnFlock::summaryLine(f) == QStringLiteral("30-day retention · 321 searches · shares with 77 agencies · 14 cameras"),
              "summary: %s", qPrintable(EyesOnFlock::summaryLine(f)));
        const EyesOnFlock::Portal back = EyesOnFlock::fromJson(EyesOnFlock::toJson(ps[1]));
        CHECK(back.slug == ps[1].slug && back.type == QLatin1String("SD") && back.tokens == ps[1].tokens && back.retentionDays == 45, "portal JSON round trip");
    }
    CHECK(EyesOnFlock::stateCode(QStringLiteral("West Virginia")) == QLatin1String("WV") && EyesOnFlock::stateCode(QStringLiteral("pa")) == QLatin1String("PA")
          && EyesOnFlock::stateCode(QStringLiteral("Narnia")).isEmpty(), "state names → codes");
    CHECK(!EyesOnFlock::parse("<html>blocked</html>").error.isEmpty() && !EyesOnFlock::parse("{}").error.isEmpty(), "a blocked / empty answer is an error, not an empty list");

    std::printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
