#pragma once
// OpenStreetMap tags → BeaconFix place category, with the pediatric ER rules
// (docs/API.md "Pediatric ER"). Pure QtCore: the classifier test builds it on
// its own, and the Android PedsClassifier implements the same rules against the
// same fixture (tests/fixtures/pediatric_tags.json).
//
// Pediatric tiers (Result::peds):
//   0 none · 1 dedicated pediatric ER, confirmed · 2 children's hospital, ER not
//   confirmed · 3 general ER with a pediatrics department (stays in "health") ·
//   4 pediatric urgent care (not an ER)
#include <QJsonObject>
#include <QList>
#include <QString>

namespace PoiClassify {

struct Element {                      // one Overpass element (node lat/lon, or the way/relation center)
    QString type;                     // node | way | relation
    qint64  id = 0;
    double  lat = 0, lon = 0;
    QJsonObject tags;
    QString key() const { return type + QLatin1Char('/') + QString::number(id); }
    static Element fromOverpass(const QJsonObject &el);
};

struct Result {
    QString cat;                      // "" = not a place we show
    int     peds = 0;                 // tier, see above
    QString er;                       // "yes" | "no" | "" (unknown)
    QString campus;                   // tier 2: the ER hospital on the same campus (within 600 m)
    QString detail;                   // the confidence text ("ER not confirmed — call ahead", "not an ER", …)
    bool    emergency = false;        // er == "yes", or a general hospital open 24/7
    bool    hospitalEr = false;       // amenity/healthcare=hospital with an emergency department (campus candidate)
    QString phone;                    // phone || contact:phone
    QString address;                  // from addr:*
    bool    dropped = false;          // a duplicate of a nearby object with the same name
};

// Category and pediatric tier of one object (no campus / dedupe pass)
Result classify(const QJsonObject &tags);
// The rules that existed before the pediatric ones (fuel, food, police, …), with healthcare=hospital accepted
QString baseCategory(const QJsonObject &tags);
// classify each element, then the campus pass (600 m), then the dedupe (same name within 500 m).
// The result list is aligned with the input; duplicates come back with dropped = true.
QList<Result> classifyAll(const QList<Element> &elements);
// "123 Main St, Town, ST 12345" from addr:* tags
QString address(const QJsonObject &tags);
// "pediatric ER", "children's hospital — ER not confirmed", … (empty for 0)
QString tierLabel(int peds);

double distanceM(double lat1, double lon1, double lat2, double lon2);
// Straight-line distance × 1.4 at 70 km/h, rounded to 5 minutes (at least 5 minutes)
void driveEstimate(double distM, int *driveS, int *driveM);

// ── Nearest help ─────────────────────────────────────────────────────────────
// rank 0 = tier 1, or tier 2 with an ER on the same campus; rank 1 = tier 2 without one, or tier 3
int pedsRank(int peds, const QString &campus);

struct HelpCandidate {
    QString cat;
    int     peds = 0;
    QString campus;
    bool    emergency = false;
    double  distM = 0;
    int     driveS = 0;
};
struct HelpPicks {                    // indices into the candidate list, -1 = none
    int pediatric = -1;               // the lowest driveS among rank 0, else among rank 1
    int pediatricCloser = -1;         // a rank-1 site closer than a rank-0 pick
    int pediatricUrgent = -1;         // the nearest pediatric urgent care
    int hospital = -1;                // the nearest general ER (health with emergency), else the nearest health
};
HelpPicks pickHelp(const QList<HelpCandidate> &c);

} // namespace PoiClassify
