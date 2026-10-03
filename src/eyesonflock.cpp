// SPDX-License-Identifier: Apache-2.0
#include "eyesonflock.h"
#include "hibf.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace EyesOnFlock {

static double numOr(const QJsonValue &v, double dflt = -1)
{
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) { bool ok = false; const double d = v.toString().toDouble(&ok); if (ok) return d; }
    return dflt;
}

QStringList portalTokens(const Portal &p)
{
    QString st;
    QString name = !p.city.isEmpty() ? p.city : p.county;
    if (name.isEmpty()) { name = p.slug; name.replace(QLatin1Char('-'), QLatin1Char(' ')); }
    QStringList t = Hibf::nameTokens(name, &st, true);
    std::sort(t.begin(), t.end());
    return t;
}

Parsed parse(const QByteArray &json)
{
    Parsed out;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (!doc.isObject()) { out.error = pe.error != QJsonParseError::NoError ? pe.errorString() : QStringLiteral("not a JSON object"); return out; }
    const QJsonObject root = doc.object();
    if (!root.value(QLatin1String("portals")).isArray()) { out.error = QStringLiteral("no portals in the answer"); return out; }
    const QJsonObject s = root.value(QLatin1String("summary")).toObject();
    QString snap = s.value(QLatin1String("snapshot_date")).toObject().value(QLatin1String("$date")).toString();
    if (snap.isEmpty()) snap = s.value(QLatin1String("snapshot_date")).toString();
    for (const char *k : {"total_portals_found", "total_cameras", "total_searches", "total_hotlist_hits", "total_vehicles_captured", "estimated_total_cameras"})
        if (s.contains(QLatin1String(k))) out.summary[QLatin1String(k)] = s.value(QLatin1String(k));
    out.summary["snapshot"] = snap;
    for (const QJsonValue &v : root.value(QLatin1String("portals")).toArray()) {
        const QJsonObject o = v.toObject();
        Portal p;
        p.url = o.value(QLatin1String("portal_url")).toString();
        p.slug = o.value(QLatin1String("slug")).toString();
        if (p.slug.isEmpty()) p.slug = p.url.section(QLatin1Char('/'), -1);
        if (p.slug.isEmpty()) continue;
        p.city = o.value(QLatin1String("city")).toString();
        p.county = o.value(QLatin1String("county")).toString();
        p.state = o.value(QLatin1String("state")).toString().trimmed().toUpper();
        p.type = o.value(QLatin1String("type")).toString().trimmed().toUpper();
        p.population = numOr(o.value(QLatin1String("population")));
        p.cameras = numOr(o.value(QLatin1String("total_cameras")));
        p.searches = numOr(o.value(QLatin1String("total_searches")));
        p.retentionDays = numOr(o.value(QLatin1String("data_retention")));
        p.vehicles = numOr(o.value(QLatin1String("vehicles_captured")));
        p.hotlistHits = numOr(o.value(QLatin1String("hotlist_hits")));
        p.hotlistRate = numOr(o.value(QLatin1String("hotlist_hit_rate")));
        const QJsonArray sw = o.value(QLatin1String("organizations_shared_with")).toArray(), rf = o.value(QLatin1String("organizations_received_from")).toArray();
        p.sharedWith = int(numOr(o.value(QLatin1String("organization_count")), sw.isEmpty() ? -1 : double(sw.size())));
        if (p.sharedWith < 0 && !sw.isEmpty()) p.sharedWith = int(sw.size());
        p.receivedFrom = int(numOr(o.value(QLatin1String("receiving_organization_count")), rf.isEmpty() ? -1 : double(rf.size())));
        p.prohibitedUses = o.value(QLatin1String("prohibited_uses")).toString();
        p.publicAudit = o.value(QLatin1String("public_search_audit")).toBool();
        p.updated = o.value(QLatin1String("data_last_updated")).toString();
        p.tokens = portalTokens(p);
        out.portals << p;
    }
    return out;
}

static const QHash<QString, QString> &stateNames()
{
    static const QHash<QString, QString> m = [] {
        QHash<QString, QString> h;
        const char *pairs[][2] = {{"alabama", "AL"}, {"alaska", "AK"}, {"arizona", "AZ"}, {"arkansas", "AR"}, {"california", "CA"}, {"colorado", "CO"},
                                  {"connecticut", "CT"}, {"delaware", "DE"}, {"florida", "FL"}, {"georgia", "GA"}, {"hawaii", "HI"}, {"idaho", "ID"},
                                  {"illinois", "IL"}, {"indiana", "IN"}, {"iowa", "IA"}, {"kansas", "KS"}, {"kentucky", "KY"}, {"louisiana", "LA"},
                                  {"maine", "ME"}, {"maryland", "MD"}, {"massachusetts", "MA"}, {"michigan", "MI"}, {"minnesota", "MN"},
                                  {"mississippi", "MS"}, {"missouri", "MO"}, {"montana", "MT"}, {"nebraska", "NE"}, {"nevada", "NV"},
                                  {"new hampshire", "NH"}, {"new jersey", "NJ"}, {"new mexico", "NM"}, {"new york", "NY"}, {"north carolina", "NC"},
                                  {"north dakota", "ND"}, {"ohio", "OH"}, {"oklahoma", "OK"}, {"oregon", "OR"}, {"pennsylvania", "PA"},
                                  {"rhode island", "RI"}, {"south carolina", "SC"}, {"south dakota", "SD"}, {"tennessee", "TN"}, {"texas", "TX"},
                                  {"utah", "UT"}, {"vermont", "VT"}, {"virginia", "VA"}, {"washington", "WA"}, {"west virginia", "WV"},
                                  {"wisconsin", "WI"}, {"wyoming", "WY"}, {"district of columbia", "DC"}};
        for (const auto &p : pairs) { h.insert(QLatin1String(p[0]), QLatin1String(p[1])); h.insert(QString::fromLatin1(p[1]).toLower(), QLatin1String(p[1])); }
        return h;
    }();
    return m;
}

QString stateCode(const QString &s) { return stateNames().value(s.simplified().toLower()); }

// What kind of agency a name says it is: "SD" a sheriff, "PD" police, "" it does not say
static QString kindOf(const QString &name)
{
    static const QRegularExpression sheriff(QStringLiteral("sheriff|\\bs\\.?o\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression police(QStringLiteral("police|\\bp\\.?d\\b|\\bdps\\b|public safety|marshal"), QRegularExpression::CaseInsensitiveOption);
    if (sheriff.match(name).hasMatch()) return QStringLiteral("SD");
    if (police.match(name).hasMatch()) return QStringLiteral("PD");
    return {};
}

int match(const QString &agencyName, const QString &stateIn, const QList<Portal> &portals, bool *stateVerified, QString *why)
{
    if (stateVerified) *stateVerified = false;
    QString nameState;
    const QStringList toks = Hibf::nameTokens(agencyName, &nameState, true);
    if (toks.isEmpty()) { if (why) *why = QStringLiteral("no place name in \"%1\"").arg(agencyName); return -1; }
    const QString state = !stateIn.isEmpty() ? stateIn.toUpper() : nameState;
    const QString kind = kindOf(agencyName);
    struct Hit { int i; double score; };
    QList<Hit> hits;
    for (int i = 0; i < int(portals.size()); ++i) {
        const Portal &p = portals[i];
        if (p.tokens.isEmpty()) continue;
        bool all = true;
        for (const QString &t : toks) if (!p.tokens.contains(t)) { all = false; break; }
        if (!all) continue;
        if (!state.isEmpty() && !p.state.isEmpty() && state != p.state) continue;
        if (!kind.isEmpty() && !p.type.isEmpty() && kind != p.type) continue;    // a sheriff is not the city police
        double score = double(toks.size()) / double(p.tokens.size());
        if (!kind.isEmpty() && kind == p.type) score += 0.5;
        hits.append({i, score});
    }
    if (hits.isEmpty()) { if (why) *why = QStringLiteral("no portal for \"%1\"%2").arg(agencyName, state.isEmpty() ? QString() : QStringLiteral(" in ") + state); return -1; }
    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.score > b.score; });
    if (state.isEmpty()) {
        QSet<QString> states;
        for (const Hit &h : std::as_const(hits)) states.insert(portals[h.i].state);
        if (states.size() > 1) { if (why) *why = QStringLiteral("ambiguous without a state: %1 portals in %2 states").arg(hits.size()).arg(states.size()); return -1; }
    } else if (stateVerified) *stateVerified = true;
    if (hits.size() > 1 && std::fabs(hits[0].score - hits[1].score) < 1e-9 && portals[hits[0].i].type == portals[hits[1].i].type) {
        if (why) *why = QStringLiteral("ambiguous: %1 and %2").arg(portals[hits[0].i].slug, portals[hits[1].i].slug);
        return -1;
    }
    if (why) *why = QStringLiteral("%1 → %2").arg(toks.join(QLatin1Char(' ')), portals[hits[0].i].slug);
    return hits[0].i;
}

static QJsonValue n(double v) { return v < 0 ? QJsonValue() : QJsonValue(v); }

QJsonObject facts(const Portal &p, bool stateVerified)
{
    const QString name = QStringLiteral("%1 %2 %3").arg(!p.city.isEmpty() ? p.city : p.county + QStringLiteral(" County"), p.state,
                                                         p.type == QLatin1String("SD") ? QStringLiteral("Sheriff") : QStringLiteral("PD")).simplified();
    return QJsonObject{{"agency", name}, {"slug", p.slug}, {"portal", p.url}, {"state", p.state}, {"type", p.type},
                       {"retentionDays", n(p.retentionDays)}, {"cameras", n(p.cameras)}, {"searches", n(p.searches)},
                       {"sharedWith", p.sharedWith < 0 ? QJsonValue() : QJsonValue(p.sharedWith)},
                       {"receivedFrom", p.receivedFrom < 0 ? QJsonValue() : QJsonValue(p.receivedFrom)},
                       {"vehiclesCaptured", n(p.vehicles)}, {"hotlistHits", n(p.hotlistHits)}, {"publicSearchAudit", p.publicAudit},
                       {"prohibitedUses", p.prohibitedUses}, {"updated", p.updated}, {"stateVerified", stateVerified},
                       {"source", "Eyes on Flock"}, {"sourceUrl", QLatin1String(kSite)}, {"license", QLatin1String(kLicense)},
                       {"attribution", QLatin1String(kAttribution)}};
}

QString summaryLine(const QJsonObject &f)
{
    if (f.isEmpty()) return {};
    QStringList parts;
    if (f.value(QLatin1String("retentionDays")).isDouble()) parts << QStringLiteral("%1-day retention").arg(f.value(QLatin1String("retentionDays")).toInt());
    if (f.value(QLatin1String("searches")).isDouble()) parts << QStringLiteral("%1 searches").arg(qint64(f.value(QLatin1String("searches")).toDouble()));
    if (f.value(QLatin1String("sharedWith")).isDouble()) parts << QStringLiteral("shares with %1 agencies").arg(f.value(QLatin1String("sharedWith")).toInt());
    if (f.value(QLatin1String("cameras")).isDouble()) parts << QStringLiteral("%1 cameras").arg(qint64(f.value(QLatin1String("cameras")).toDouble()));
    QString s = parts.join(QStringLiteral(" · "));
    if (!f.value(QLatin1String("stateVerified")).toBool(true)) s += QStringLiteral(" (state not verified)");
    return s;
}

QJsonObject toJson(const Portal &p)
{
    return QJsonObject{{"slug", p.slug}, {"url", p.url}, {"city", p.city}, {"county", p.county}, {"state", p.state}, {"type", p.type},
                       {"population", p.population}, {"cameras", p.cameras}, {"searches", p.searches}, {"retentionDays", p.retentionDays},
                       {"vehicles", p.vehicles}, {"hotlistHits", p.hotlistHits}, {"hotlistRate", p.hotlistRate}, {"sharedWith", p.sharedWith},
                       {"receivedFrom", p.receivedFrom}, {"prohibitedUses", p.prohibitedUses}, {"publicAudit", p.publicAudit}, {"updated", p.updated},
                       {"tokens", QJsonArray::fromStringList(p.tokens)}};
}

Portal fromJson(const QJsonObject &o)
{
    Portal p;
    p.slug = o.value(QLatin1String("slug")).toString(); p.url = o.value(QLatin1String("url")).toString();
    p.city = o.value(QLatin1String("city")).toString(); p.county = o.value(QLatin1String("county")).toString();
    p.state = o.value(QLatin1String("state")).toString(); p.type = o.value(QLatin1String("type")).toString();
    p.population = o.value(QLatin1String("population")).toDouble(-1); p.cameras = o.value(QLatin1String("cameras")).toDouble(-1);
    p.searches = o.value(QLatin1String("searches")).toDouble(-1); p.retentionDays = o.value(QLatin1String("retentionDays")).toDouble(-1);
    p.vehicles = o.value(QLatin1String("vehicles")).toDouble(-1); p.hotlistHits = o.value(QLatin1String("hotlistHits")).toDouble(-1);
    p.hotlistRate = o.value(QLatin1String("hotlistRate")).toDouble(-1);
    p.sharedWith = o.value(QLatin1String("sharedWith")).toInt(-1); p.receivedFrom = o.value(QLatin1String("receivedFrom")).toInt(-1);
    p.prohibitedUses = o.value(QLatin1String("prohibitedUses")).toString(); p.publicAudit = o.value(QLatin1String("publicAudit")).toBool();
    p.updated = o.value(QLatin1String("updated")).toString();
    for (const QJsonValue &t : o.value(QLatin1String("tokens")).toArray()) p.tokens << t.toString();
    if (p.tokens.isEmpty()) p.tokens = portalTokens(p);
    return p;
}

} // namespace EyesOnFlock
