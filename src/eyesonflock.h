// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// Eyes on Flock (https://eyesonflock.com, data CC BY-SA 4.0): the Flock transparency portals of ~1 500 agencies —
// cameras, retention, searches, the agencies they share with. Pure: parsing and the agency ↔ operator join
// (docs/SIGHTINGS.md §4.6). src/platewatch.cpp fetches it weekly and keeps it in the eof_portals table.
namespace EyesOnFlock {

constexpr const char *kUrl = "https://eyesonflock.com/api/v1/data";
constexpr const char *kSite = "https://eyesonflock.com/";
constexpr const char *kLicense = "CC BY-SA 4.0";
constexpr const char *kAttribution = "Eyes on Flock (eyesonflock.com), CC BY-SA 4.0, from the agencies' Flock transparency portals";

struct Portal {
    QString slug, url, city, county, state, type;   // type: PD | SD (sheriff)
    double population = -1, cameras = -1, searches = -1, retentionDays = -1, vehicles = -1, hotlistHits = -1, hotlistRate = -1;
    int    sharedWith = -1, receivedFrom = -1;      // agencies it shares with / receives from
    QString prohibitedUses, updated;
    bool   publicAudit = false;
    QStringList tokens;                             // place-name tokens (Hibf::nameTokens of the city / county)
};
struct Parsed {
    QList<Portal> portals;
    QJsonObject summary;                            // the site's totals and snapshot date
    QString error;
};
Parsed parse(const QByteArray &json);
QStringList portalTokens(const Portal &p);
// The portal of the agency running a camera (its OSM operator) or named in an audit log ("Chehalis WA PD"): every
// place token of the name in the portal's tokens, the state equal when both are known, a sheriff never matched to a
// police department or the other way round. Without a state a match is taken only when no other state's portal has
// the same tokens (*stateVerified = false then). -1 none (or ambiguous: *why says so).
int match(const QString &agencyName, const QString &state, const QList<Portal> &portals, bool *stateVerified = nullptr, QString *why = nullptr);
QString stateCode(const QString &nameOrCode);      // "West Virginia" / "WV" → "WV"; "" unknown
QJsonObject facts(const Portal &p, bool stateVerified = true);   // what events / the API show (with attribution)
QString summaryLine(const QJsonObject &facts);     // "30-day retention · 516 searches · shares with 655 agencies"
QJsonObject toJson(const Portal &p);
Portal fromJson(const QJsonObject &o);

} // namespace EyesOnFlock
