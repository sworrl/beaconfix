// SPDX-License-Identifier: Apache-2.0
#include "hibf.h"
#include "plateevents.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <QUrl>
#include <algorithm>
#include <functional>

namespace Hibf {

QString fullHash(const QString &variant)
{
    return QString::fromLatin1(QCryptographicHash::hash(variant.trimmed().toLower().toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString hashPrefix(const QString &variant) { return fullHash(variant).left(8); }

// a1() in the site's bundle: positions holding O/0 or I/1, every combination depth-first ("O" before "0",
// "I" before "1"), deduplicated in insertion order, the original first, at most `max`
QStringList expandOI(const QString &plate, int max)
{
    const QString o = plate.toUpper();
    struct Pos { qsizetype index; QChar a, b; };
    QList<Pos> pos;
    for (qsizetype u = 0; u < o.size(); ++u) {
        const QChar d = o[u];
        if (d == QLatin1Char('O') || d == QLatin1Char('0')) pos.append({u, QLatin1Char('O'), QLatin1Char('0')});
        else if (d == QLatin1Char('I') || d == QLatin1Char('1')) pos.append({u, QLatin1Char('I'), QLatin1Char('1')});
    }
    if (pos.isEmpty()) return {o};
    QStringList seen{o};
    QString cur = o;
    std::function<void(qsizetype)> walk = [&](qsizetype u) {
        if (u == pos.size()) { if (!seen.contains(cur)) seen << cur; return; }
        for (QChar c : {pos[u].a, pos[u].b}) { cur[pos[u].index] = c; walk(u + 1); }
    };
    walk(0);
    QStringList out{o};
    for (const QString &s : std::as_const(seen)) if (s != o) out << s;
    return out.mid(0, max);
}

QStringList plateForms(const QString &displayPlate)
{
    QString display = displayPlate;
    display.remove(QRegularExpression(QStringLiteral("[\\s&]")));
    QString alnum;
    for (QChar c : display) if (c.isLetterOrNumber()) alnum += c;
    QStringList out;
    if (!display.isEmpty()) out << display;
    if (!alnum.isEmpty() && alnum.compare(display, Qt::CaseInsensitive) != 0) out << alnum;
    return out;
}

QList<Variant> variants(const QStringList &displayPlates)
{
    QList<Variant> out;
    QSet<QString> have;
    for (const QString &p : displayPlates) {
        for (const QString &form : plateForms(p)) {
            for (const QString &v : expandOI(form, 10)) {
                const QString full = fullHash(v);
                if (have.contains(full)) continue;
                have.insert(full);
                out.append({p, v, full.left(8), full});
            }
        }
    }
    return out;
}

QStringList prefixes(const QList<Variant> &vars)
{
    QStringList out;
    for (const Variant &v : vars) if (!out.contains(v.prefix)) out << v.prefix;
    return out;
}

QList<Match> filterResults(const QJsonArray &results, const QList<Variant> &vars)
{
    QList<Match> out;
    for (const QJsonValue &rv : results) {
        const QJsonObject row = rv.toObject();
        const QString h = row.value(QLatin1String("license_plate_hash")).toString().trimmed().toLower();
        static const QRegularExpression full(QStringLiteral("^[0-9a-f]{64}$"));
        if (full.match(h).hasMatch()) {
            for (const Variant &v : vars) if (v.full == h) { out.append({row, v.plate, true}); break; }
            continue;                                                     // another plate behind the same prefix
        }
        QString plate = vars.isEmpty() ? QString() : vars.first().plate;
        for (const Variant &v : vars) if (!h.isEmpty() && v.prefix.startsWith(h.left(8))) { plate = v.plate; break; }
        out.append({row, plate, false});
    }
    return out;
}

QString utcToLocalIso(const QString &utc)
{
    QDateTime d = QDateTime::fromString(utc, Qt::ISODateWithMs);
    if (!d.isValid()) d = QDateTime::fromString(utc, Qt::ISODate);
    if (!d.isValid()) return utc;
    if (!(utc.endsWith(QLatin1Char('Z')) || utc.contains(QLatin1Char('+')) || utc.lastIndexOf(QLatin1Char('-')) > 16)) d.setTimeZone(QTimeZone::UTC);   // "…T12:00:00": UTC by the field's name
    return d.toLocalTime().toString(Qt::ISODate);
}

QJsonObject searchEvent(const Match &m)
{
    const QJsonObject &r = m.row;
    const QString agency = r.value(QLatin1String("org_name")).toString(r.value(QLatin1String("agency_name")).toString());
    const QString reason = r.value(QLatin1String("reason")).toString();
    QString url = r.value(QLatin1String("source_url")).toString().trimmed();
    if (!(url.startsWith(QLatin1String("https://")) || url.startsWith(QLatin1String("http://")))) url = QStringLiteral("https://haveibeenflocked.com/");
    QString srcOrg = r.value(QLatin1String("source_org_name")).toString();
    if (srcOrg.isEmpty()) srcOrg = agency;
    QJsonObject metrics = r;
    metrics["hashVerified"] = m.verified;
    const QString caseNo = r.value(QLatin1String("case_number")).toString();
    return QJsonObject{
        {"uid", PlateEvents::searchUid(r)}, {"kind", "plate_search"}, {"plate", m.plate},
        {"time", utcToLocalIso(r.value(QLatin1String("search_time_utc")).toString())},
        {"lat", QJsonValue()}, {"lon", QJsonValue()}, {"acc", QJsonValue()},
        {"operator", QJsonValue()}, {"agency", agency}, {"camera_type", QJsonValue()},
        {"source", "haveibeenflocked"}, {"source_url", url},
        {"source_name", QStringLiteral("HaveIBeenFlocked · %1 audit log").arg(srcOrg.isEmpty() ? QStringLiteral("released") : srcOrg)},
        {"confidence", m.verified ? 100 : 50}, {"leaky", 1},
        {"details", QStringLiteral("%1 searched %2 — %3%4").arg(agency.isEmpty() ? QStringLiteral("An agency") : agency, m.plate,
                                                               reason.isEmpty() ? QStringLiteral("no reason given") : reason,
                                                               caseNo.isEmpty() ? QString() : QStringLiteral(" (case %1)").arg(caseNo))},
        {"metrics", metrics}, {"raw", r}, {"device", ""}};
}

// ── §4.4 ──────────────────────────────────────────────────────────────────────
static const QSet<QString> &stopWords()
{
    static const QSet<QString> s = [] {
        QSet<QString> w;
        for (const char *x : {"pd", "police", "department", "dept", "sheriff", "sheriffs", "office", "county", "city", "of", "the", "network", "audit", "audits",
                              "organization", "org", "search", "searches", "csv", "flock", "safety", "foi", "and", "for", "request", "requests", "records", "record",
                              "public", "general", "order", "sharing", "data", "files", "file", "report", "reports", "clean", "redacted", "responsive", "alpr", "lpr",
                              "plate", "plates", "license", "camera", "cameras", "logs", "log", "transparency", "portal", "news", "township", "twp", "village",
                              "town", "borough", "commission", "comm", "parish", "division", "agency", "law", "enforcement", "services", "state", "patrol",
                              "highway", "marshal", "marshals", "constable", "precinct", "inc", "llc", "through", "from", "copy", "final", "export", "exported",
                              "full", "list", "lookup", "lookups", "history", "with", "xlsx", "pdf", "docs", "document", "documents", "response", "results",
                              "result", "part", "page", "sheet", "jurisdiction", "agencies", "department's", "pds", "dps", "sos", "fy", "ytd", "all", "usage",
                              "access", "information", "info", "attachment", "attachments", "released", "release", "updated", "update", "version", "final",
                              "solutions", "transportation", "dot", "district", "attorney", "attorneys", "courts", "court", "municipal", "regional", "school",
                              "schools", "isd", "campus", "cpra", "foia", "pra", "removed", "authority", "port", "airport", "transit", "fire", "rescue", "security", "private", "community",
                              "association", "hoa", "corp", "corporation", "company", "group", "partners", "properties", "property", "management", "store",
                              "stores", "mall", "center", "retail", "the", "per", "via", "fwd", "re", "email", "emails", "folder", "zip", "detail", "details"})
            w.insert(QLatin1String(x));
        for (const char *m : {"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december",
                              "jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec"})
            w.insert(QLatin1String(m));
        return w;
    }();
    return s;
}

static const QSet<QString> &usStates()
{
    static const QSet<QString> s = [] {
        QSet<QString> w;
        for (const char *x : {"AL", "AK", "AZ", "AR", "CA", "CO", "CT", "DE", "FL", "GA", "HI", "ID", "IL", "IN", "IA", "KS", "KY", "LA", "ME", "MD", "MA", "MI",
                              "MN", "MS", "MO", "MT", "NE", "NV", "NH", "NJ", "NM", "NY", "NC", "ND", "OH", "OK", "OR", "PA", "RI", "SC", "SD", "TN", "TX", "UT",
                              "VT", "VA", "WA", "WV", "WI", "WY", "DC"})
            w.insert(QLatin1String(x));
        return w;
    }();
    return s;
}

// Words only (digits and punctuation separate them), stop words and months out, tokens of 3+ letters. A two-letter
// US state code is taken as the state: upper case only in file names ("Chehalis WA PD"), the last one in a
// lower-case URL slug ("santa-clara-co-ca" → CA).
QStringList nameTokens(const QString &text, QString *state, bool upperStatesOnly)
{
    QStringList out;
    QString cur;
    QStringList raw;
    for (QChar c : text + QLatin1Char(' ')) {
        if (c.isLetter() || c == QLatin1Char('\'')) cur += c;
        else { if (!cur.isEmpty()) raw << cur; cur.clear(); }
    }
    for (QString t : std::as_const(raw)) {
        t.remove(QLatin1Char('\''));
        if (t.size() == 2) {
            const QString up = t.toUpper();
            if (state && usStates().contains(up) && (!upperStatesOnly || t == up)) { if (!upperStatesOnly || state->isEmpty()) *state = up; }
            continue;
        }
        const QString l = t.toLower();
        if (l.size() < 3 || stopWords().contains(l)) continue;
        if (l.endsWith(QLatin1String("s")) && stopWords().contains(l.chopped(1))) continue;   // "audits", "departments"
        if (!out.contains(l)) out << l;
    }
    return out;
}

QStringList fileTokens(const QString &filename, const QString &sourceUrl, QString *state)
{
    QString base = filename;
    base.remove(QRegularExpression(QStringLiteral("\\.(csv|xlsx?|pdf|txt|json)$"), QRegularExpression::CaseInsensitiveOption));
    QStringList toks = nameTokens(base, state, true);
    const QUrl u(sourceUrl);
    QString urlState;
    if (u.isValid() && !u.host().isEmpty()) {
        for (const QString &seg : u.path().split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            static const QSet<QString> structural{QStringLiteral("foi"), QStringLiteral("news"), QStringLiteral("files"), QStringLiteral("api"),
                                                  QStringLiteral("documents"), QStringLiteral("agency"), QStringLiteral("reports"), QStringLiteral("blog")};
            if (structural.contains(seg.toLower())) continue;
            for (const QString &t : nameTokens(seg, &urlState, false)) if (!toks.contains(t)) toks << t;
        }
    }
    if (state && state->isEmpty()) *state = urlState;
    std::sort(toks.begin(), toks.end());
    return toks;
}

QJsonObject parseSources(const QByteArray &json, const QString &fetchedIso)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonArray src = root.value(QLatin1String("sources")).toArray();
    struct Agg { QStringList tokens; QString state; int files = 0; double records = 0; QString latest, file, url; };
    QHash<QString, Agg> byKey;
    QStringList order;
    for (const QJsonValue &v : src) {
        const QJsonObject s = v.toObject();
        QString st;
        const QString fn = s.value(QLatin1String("filename")).toString(), url = s.value(QLatin1String("source_url")).toString();
        const QStringList toks = fileTokens(fn, url, &st);
        if (toks.isEmpty()) continue;
        const QString key = toks.join(QLatin1Char(' ')) + QLatin1Char('|') + st;
        if (!byKey.contains(key)) { order << key; Agg a; a.tokens = toks; a.state = st; a.file = fn; a.url = url; byKey.insert(key, a); }
        Agg &a = byKey[key];
        ++a.files;
        a.records += s.value(QLatin1String("total_records")).toDouble();
        const QString latest = s.value(QLatin1String("latest_search_time")).toString();
        if (latest > a.latest) a.latest = latest;
    }
    QJsonArray agencies;
    for (const QString &k : std::as_const(order)) {
        const Agg &a = byKey[k];
        agencies.append(QJsonObject{{"tokens", QJsonArray::fromStringList(a.tokens)}, {"state", a.state}, {"files", a.files}, {"records", a.records},
                                    {"latest", a.latest}, {"file", a.file}, {"url", a.url}});
    }
    return QJsonObject{{"fetched", fetchedIso}, {"updatedAt", root.value(QLatin1String("updatedAt")).toString()}, {"files", int(src.size())}, {"agencies", agencies}};
}

QString leakyMatch(const QString &operatorName, const QString &cameraState, const QJsonArray &agencies)
{
    QString opState;
    const QStringList op = nameTokens(operatorName, &opState, true);
    if (op.isEmpty()) return {};
    const QString state = !cameraState.isEmpty() ? cameraState : opState;
    QString best; double bestScore = 0;
    for (const QJsonValue &v : agencies) {
        const QJsonObject a = v.toObject();
        QSet<QString> toks;
        for (const QJsonValue &t : a.value(QLatin1String("tokens")).toArray()) toks.insert(t.toString());
        bool all = true;
        for (const QString &t : op) if (!toks.contains(t)) { all = false; break; }
        if (!all) continue;
        const QString as = a.value(QLatin1String("state")).toString();
        if (!state.isEmpty() && !as.isEmpty() && state != as) continue;
        const double score = double(op.size()) / double(toks.size()) + (state.isEmpty() || as.isEmpty() ? 0.0 : 1.0);
        if (score > bestScore) {
            bestScore = score;
            best = a.value(QLatin1String("file")).toString();
            const QString url = a.value(QLatin1String("url")).toString();
            if (!url.isEmpty()) best += QStringLiteral(" · ") + url;
        }
    }
    return best;
}

QString stateFromText(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("(?:^|[,\\s])([A-Z]{2})(?:\\s+\\d{5}(?:-\\d{4})?)?\\s*$"));
    const auto m = re.match(text.trimmed());
    return m.hasMatch() && usStates().contains(m.captured(1)) ? m.captured(1) : QString();
}

// ── §4.5 ──────────────────────────────────────────────────────────────────────
qint64 intervalSecs(Mode m)
{
    switch (m) {
    case Mode::Leaky: return 3 * 3600;
    case Mode::Flock: return 12 * 3600;
    case Mode::Driving: return 24 * 3600;
    case Mode::Idle: break;
    }
    return 7 * 24 * 3600;
}

QString modeName(Mode m)
{
    switch (m) {
    case Mode::Leaky: return QStringLiteral("leaky");
    case Mode::Flock: return QStringLiteral("flock");
    case Mode::Driving: return QStringLiteral("driving");
    case Mode::Idle: break;
    }
    return QStringLiteral("idle");
}

qint64 backoffSecs(int failures)
{
    if (failures <= 0) return 0;
    qint64 s = 3600;
    for (int i = 1; i < failures && s < 86400; ++i) s *= 2;
    return std::min<qint64>(s, 86400);
}

} // namespace Hibf
