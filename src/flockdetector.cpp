// SPDX-License-Identifier: Apache-2.0
#include "flockdetector.h"
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <algorithm>
#include <atomic>
#include <memory>

namespace FlockDetector {

// ── the rule set ──────────────────────────────────────────────────────────────
struct ClassInfo { QString label, cameraType; bool flock = false; };
struct MacRule { QString prefix; QString cls, model, vendor; int tier = 0; bool family = false; };
struct RxRule { QString id; QRegularExpression rx; QString cls, model; int tier = 0, bssidSuffixTier = -1; };
struct CompanyRule { int id = -1; QString hex, cls, model, vendor; int tier = 0, corroboratedTier = -1; };
struct ServiceRule { QString uuid; QString cls, model; int tier = 0; };
struct RangeRule { int from = 0, to = 0; QString cls, model; int tier = 0; };

class Signatures {
public:
    int version = 0, detectTier = 2, ssidWithFamilyMacTier = 3;
    QList<int> confidence;
    QHash<QString, ClassInfo> classes;
    QHash<QString, MacRule> mac[13];          // by prefix length in hex digits: 6 (MA-L), 7 (MA-M), 9 (MA-S), 12 (exact)
    QList<RxRule> ssid, probe, bleName;
    QList<CompanyRule> company;
    QHash<QString, ServiceRule> service;      // "3100" / a lower-case 128-bit UUID
    QList<RangeRule> serviceRange;
    QRegularExpression tnSerial;
};

static int tierOf(const QJsonObject &o, const char *k = "tier") { return o.value(QLatin1String(k)).toInt(-1); }

const Signatures *parseSignatures(const QByteArray &json, QString *error)
{
    auto fail = [error](const QString &e) -> const Signatures * { if (error) *error = e; return nullptr; };
    QJsonParseError pe;
    const QJsonObject root = QJsonDocument::fromJson(json, &pe).object();
    if (root.isEmpty()) return fail(QStringLiteral("not a JSON object (%1)").arg(pe.errorString()));
    if (root.value(QLatin1String("format")).toInt() != 1) return fail(QStringLiteral("unknown format %1").arg(root.value(QLatin1String("format")).toInt()));
    auto *s = new Signatures;
    std::unique_ptr<Signatures> guard(s);
    s->version = root.value(QLatin1String("version")).toInt();
    s->detectTier = root.value(QLatin1String("detectTier")).toInt(2);
    s->ssidWithFamilyMacTier = root.value(QLatin1String("ssidWithFamilyMacTier")).toInt(3);
    for (const QJsonValue &v : root.value(QLatin1String("confidence")).toArray()) s->confidence << v.toInt();
    if (s->confidence.size() != 5) return fail(QStringLiteral("confidence needs 5 values (tiers 0-4)"));
    const QJsonObject classes = root.value(QLatin1String("classes")).toObject();
    for (auto it = classes.begin(); it != classes.end(); ++it) {
        const QJsonObject c = it.value().toObject();
        s->classes.insert(it.key(), {c.value(QLatin1String("label")).toString(it.key()), c.value(QLatin1String("cameraType")).toString(), c.value(QLatin1String("flock")).toBool()});
    }
    auto checkClass = [&](const QString &cls, const QString &what) { return s->classes.contains(cls) ? QString() : QStringLiteral("%1: unknown class \"%2\"").arg(what, cls); };
    auto checkTier = [](int t) { return t >= 0 && t <= 4; };
    for (const QJsonValue &v : root.value(QLatin1String("mac")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString raw = o.value(QLatin1String("prefix")).toString();
        QString hex = raw.toUpper();
        hex.remove(QRegularExpression(QStringLiteral("[^0-9A-F]")));
        const int n = int(hex.size());
        // 24 / 28 / 36 / 48 bits. 70:B3:D5 is the IEEE MA-S block: only its 36-bit assignments identify anyone
        if (n != 6 && n != 7 && n != 9 && n != 12) return fail(QStringLiteral("mac %1: a prefix is 24, 28, 36 or 48 bits").arg(raw));
        if (n == 6 && hex == QLatin1String("70B3D5")) return fail(QStringLiteral("mac %1: the bare MA-S block identifies no vendor").arg(raw));
        MacRule r{hex, o.value(QLatin1String("class")).toString(), o.value(QLatin1String("model")).toString(), o.value(QLatin1String("vendor")).toString(), tierOf(o), o.value(QLatin1String("family")).toBool()};
        if (!checkTier(r.tier)) return fail(QStringLiteral("mac %1: bad tier").arg(raw));
        if (const QString e = checkClass(r.cls, raw); !e.isEmpty()) return fail(e);
        s->mac[n].insert(hex, r);
    }
    auto rxList = [&](const QJsonArray &a, QList<RxRule> *out, const QString &what) -> QString {
        for (const QJsonValue &v : a) {
            const QJsonObject o = v.toObject();
            RxRule r{o.value(QLatin1String("id")).toString(), QRegularExpression(o.value(QLatin1String("regex")).toString()), o.value(QLatin1String("class")).toString(),
                     o.value(QLatin1String("model")).toString(), tierOf(o), o.value(QLatin1String("bssidSuffixTier")).toInt(-1)};
            if (!r.rx.isValid() || r.rx.pattern().isEmpty()) return QStringLiteral("%1 %2: bad regex").arg(what, r.id);
            if (!checkTier(r.tier)) return QStringLiteral("%1 %2: bad tier").arg(what, r.id);
            if (const QString e = checkClass(r.cls, what + QLatin1Char(' ') + r.id); !e.isEmpty()) return e;
            r.rx.optimize();
            *out << r;
        }
        return {};
    };
    if (const QString e = rxList(root.value(QLatin1String("ssid")).toArray(), &s->ssid, QStringLiteral("ssid")); !e.isEmpty()) return fail(e);
    if (const QString e = rxList(root.value(QLatin1String("probe")).toArray(), &s->probe, QStringLiteral("probe")); !e.isEmpty()) return fail(e);
    const QJsonObject ble = root.value(QLatin1String("ble")).toObject();
    if (const QString e = rxList(ble.value(QLatin1String("name")).toArray(), &s->bleName, QStringLiteral("ble name")); !e.isEmpty()) return fail(e);
    for (const QJsonValue &v : ble.value(QLatin1String("company")).toArray()) {
        const QJsonObject o = v.toObject();
        CompanyRule r{o.value(QLatin1String("id")).toInt(-1), o.value(QLatin1String("hex")).toString(), o.value(QLatin1String("class")).toString(), o.value(QLatin1String("model")).toString(),
                      o.value(QLatin1String("vendor")).toString(), tierOf(o), o.value(QLatin1String("corroboratedTier")).toInt(-1)};
        if (r.id < 0 || r.id > 0xFFFF || !checkTier(r.tier)) return fail(QStringLiteral("ble company %1: bad id or tier").arg(r.hex));
        if (const QString e = checkClass(r.cls, r.hex); !e.isEmpty()) return fail(e);
        s->company << r;
    }
    for (const QJsonValue &v : ble.value(QLatin1String("service")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString u = o.value(QLatin1String("uuid")).toString();
        ServiceRule r{u.size() == 4 ? u.toUpper() : u.toLower(), o.value(QLatin1String("class")).toString(), o.value(QLatin1String("model")).toString(), tierOf(o)};
        if ((u.size() != 4 && u.size() != 36) || !checkTier(r.tier)) return fail(QStringLiteral("ble service %1: bad uuid or tier").arg(u));
        if (const QString e = checkClass(r.cls, u); !e.isEmpty()) return fail(e);
        s->service.insert(r.uuid, r);
    }
    for (const QJsonValue &v : ble.value(QLatin1String("serviceRange")).toArray()) {
        const QJsonObject o = v.toObject();
        bool ok1 = false, ok2 = false;
        RangeRule r{o.value(QLatin1String("from")).toString().toInt(&ok1, 16), o.value(QLatin1String("to")).toString().toInt(&ok2, 16), o.value(QLatin1String("class")).toString(),
                    o.value(QLatin1String("model")).toString(), tierOf(o)};
        if (!ok1 || !ok2 || r.from > r.to || !checkTier(r.tier)) return fail(QStringLiteral("ble serviceRange: bad bounds or tier"));
        if (const QString e = checkClass(r.cls, QStringLiteral("serviceRange")); !e.isEmpty()) return fail(e);
        s->serviceRange << r;
    }
    s->tnSerial = QRegularExpression(ble.value(QLatin1String("tnSerialRegex")).toString());
    if (!s->tnSerial.isValid()) return fail(QStringLiteral("ble tnSerialRegex: bad regex"));
    return guard.release();
}

int signaturesVersion(const Signatures &s) { return s.version; }

QString overridePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/signatures/surveillance.json");
}

static std::atomic<const Signatures *> g_active{nullptr};

void setSignatures(const Signatures *s)
{
    if (s) g_active.store(s);   // the previous set is leaked on purpose: a scan on another thread may still hold it
}

const Signatures *loadSignatures()
{
    QString err;
    QFile builtinFile(QStringLiteral(":/signatures/surveillance.json"));
    const Signatures *builtin = builtinFile.open(QIODevice::ReadOnly) ? parseSignatures(builtinFile.readAll(), &err) : nullptr;
    if (!builtin) {
        qWarning("beaconfix: the compiled-in surveillance signatures are unusable: %s", qPrintable(err.isEmpty() ? builtinFile.errorString() : err));
        builtin = parseSignatures(QByteArrayLiteral("{\"format\":1,\"version\":0,\"confidence\":[0,0,0,0,0],\"classes\":{}}"));
    }
    QFile over(overridePath());
    if (!over.exists() || !over.open(QIODevice::ReadOnly)) return builtin;
    const Signatures *o = parseSignatures(over.readAll(), &err);
    if (!o) { qWarning("beaconfix: ignoring %s: %s", qPrintable(over.fileName()), qPrintable(err)); return builtin; }
    if (o->version < builtin->version) {
        qWarning("beaconfix: ignoring %s: version %d is older than the built-in %d", qPrintable(over.fileName()), o->version, builtin->version);
        delete o;
        return builtin;
    }
    qInfo("beaconfix: surveillance signatures v%d from %s", o->version, qPrintable(over.fileName()));
    delete builtin;
    return o;
}

const Signatures &signatures()
{
    if (const Signatures *s = g_active.load()) return *s;
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    if (const Signatures *s = g_active.load()) return *s;
    const Signatures *s = loadSignatures();
    g_active.store(s);
    return *s;
}

QString normalizeMac(const QString &mac)
{
    QString h;
    h.reserve(12);
    for (const QChar c : mac) {
        if (c == QLatin1Char(':') || c == QLatin1Char('-') || c == QLatin1Char('.') || c.isSpace()) continue;
        if (!c.isDigit() && !(c.toUpper() >= QLatin1Char('A') && c.toUpper() <= QLatin1Char('F'))) return {};
        h += c.toUpper();
    }
    return h.size() == 12 ? h : QString();
}

// ── evaluation (docs/DETECTION.md §2; collector/FlockDetectorKotlin.kt evaluates the same way) ──
namespace {
struct Hit { QString kind, cls, model, rule, text; int tier = 0; };

const MacRule *macRule(const Signatures &s, const QString &hex)
{
    if (hex.size() != 12) return nullptr;
    for (int n : {12, 9, 7, 6}) {                       // the longest assignment wins
        const auto it = s.mac[n].constFind(hex.left(n));
        if (it != s.mac[n].constEnd()) return &it.value();
    }
    return nullptr;
}

QString macText(const QString &hex, int n)
{
    QString out;
    for (int i = 0; i < n; ++i) { if (i && i % 2 == 0) out += QLatin1Char(':'); out += hex.at(i); }
    return out;
}

Detection decide(const Signatures &s, const QList<Hit> &hits, const char *medium)
{
    Detection d;
    const Hit *best = nullptr;
    for (const Hit &h : hits) {
        if (!best || h.tier > best->tier) best = &h;
        else if (h.tier == best->tier && s.classes.value(h.cls).flock && !s.classes.value(best->cls).flock) best = &h;   // a tie: Flock first
    }
    if (!best) return d;
    d.tier = best->tier;
    d.cls = best->cls;
    const ClassInfo ci = s.classes.value(d.cls);
    d.label = ci.label;
    d.cameraType = ci.cameraType;
    d.isFlock = ci.flock && d.tier >= s.detectTier;
    d.informational = !ci.flock && d.tier >= s.detectTier;
    d.confidence = s.confidence.value(qBound(0, d.tier, 4));
    QStringList kinds, texts;
    for (const char *k : {"mac", "ssid", "probe", "name", "uuid", "company"}) {
        for (const Hit &h : hits) {
            if (h.cls != d.cls || !h.kind.split(QLatin1Char('+')).contains(QLatin1String(k))) continue;
            if (!kinds.contains(QLatin1String(k))) kinds << QLatin1String(k);
        }
    }
    for (const Hit &h : hits) {
        if (h.cls != d.cls) continue;
        d.rules << h.rule;
        if (!h.text.isEmpty() && !texts.contains(h.text)) texts << h.text;
        if (d.model.isEmpty() && !h.model.isEmpty() && h.tier == d.tier) d.model = h.model;
    }
    for (const Hit &h : hits) if (d.model.isEmpty() && h.cls == d.cls && !h.model.isEmpty()) d.model = h.model;
    d.method = QLatin1String(medium) + QLatin1Char('_') + kinds.join(QLatin1Char('+'));
    d.details = QStringLiteral("%1 (tier %2): %3").arg(d.label).arg(d.tier).arg(texts.join(QStringLiteral(" · ")));
    return d;
}

void macHit(const Signatures &s, const QString &hex, QList<Hit> *hits, const MacRule **out = nullptr)
{
    const MacRule *m = macRule(s, hex);
    if (out) *out = m;
    if (!m) return;
    hits->append({QStringLiteral("mac"), m->cls, m->model, QStringLiteral("mac:") + m->prefix,
                  QStringLiteral("MAC %1 %2").arg(macText(m->prefix, int(m->prefix.size())), m->vendor), m->tier});
}

// 16-bit SIG-base UUIDs → "XXXX", other 128-bit → lower case, "" not a UUID
QString normalizeUuid(const QString &u)
{
    const QString t = u.trimmed();
    if (t.size() == 4) return t.toUpper();
    if (t.size() == 8 && t.startsWith(QLatin1String("0000"))) return t.mid(4).toUpper();
    if (t.size() != 36) return {};
    const QString l = t.toLower();
    if (l.startsWith(QLatin1String("0000")) && l.endsWith(QLatin1String("-0000-1000-8000-00805f9b34fb"))) return l.mid(4, 4).toUpper();
    return l;
}
}

Detection evaluateWifi(const QString &bssid, const QString &ssid, const QStringList &probedSsids)
{
    return evaluateWifi(signatures(), bssid, ssid, probedSsids);
}

Detection evaluateWifi(const Signatures &s, const QString &bssid, const QString &ssid, const QStringList &probedSsids)
{
    QList<Hit> hits;
    const QString hex = normalizeMac(bssid);
    const MacRule *m = nullptr;
    macHit(s, hex, &hits, &m);
    for (const RxRule &r : s.ssid) {
        const QRegularExpressionMatch mm = r.rx.match(ssid);
        if (!mm.hasMatch()) continue;
        int tier = r.tier;
        QString rule = QStringLiteral("ssid:") + r.id, text = QStringLiteral("SSID \"%1\"").arg(ssid);
        if (r.bssidSuffixTier >= 0 && hex.size() == 12 && mm.captured(1).compare(hex.right(6), Qt::CaseInsensitive) == 0) {
            tier = qMax(tier, r.bssidSuffixTier);
            rule += QStringLiteral("=bssid");
            text += QStringLiteral(" ends in the BSSID");
        }
        hits.append({QStringLiteral("ssid"), r.cls, r.model, rule, text, tier});
        if (m && m->family && m->cls == r.cls)          // a Flock SSID on a Flock-family radio
            hits.append({QStringLiteral("ssid+mac"), r.cls, r.model, QStringLiteral("ssid+mac"), QString(), qMax(tier, s.ssidWithFamilyMacTier)});
    }
    for (const QString &p : probedSsids)
        for (const RxRule &r : s.probe)
            if (r.rx.match(p).hasMatch()) hits.append({QStringLiteral("probe"), r.cls, r.model, QStringLiteral("probe:") + r.id, QStringLiteral("probes for \"%1\"").arg(p), r.tier});
    return decide(s, hits, "wifi");
}

Detection evaluateBle(const QString &mac, const QString &name, const QStringList &serviceUuids, const QList<BleCompany> &companies)
{
    return evaluateBle(signatures(), mac, name, serviceUuids, companies);
}

Detection evaluateBle(const Signatures &s, const QString &mac, const QString &name, const QStringList &serviceUuids, const QList<BleCompany> &companies)
{
    QList<Hit> hits;
    macHit(s, normalizeMac(mac), &hits);
    bool flockName = false;
    for (const RxRule &r : s.bleName) {
        if (!r.rx.match(name).hasMatch()) continue;
        hits.append({QStringLiteral("name"), r.cls, r.model, QStringLiteral("ble_name:") + r.id, QStringLiteral("name \"%1\"").arg(name), r.tier});
        if (s.classes.value(r.cls).flock) flockName = true;
    }
    for (const QString &raw : serviceUuids) {
        const QString u = normalizeUuid(raw);
        if (u.isEmpty()) continue;
        if (const auto it = s.service.constFind(u); it != s.service.constEnd()) {
            hits.append({QStringLiteral("uuid"), it->cls, it->model, QStringLiteral("ble_uuid:") + u, QStringLiteral("service %1").arg(u), it->tier});
            continue;
        }
        if (u.size() != 4) continue;
        const int v = u.toInt(nullptr, 16);
        for (const RangeRule &r : s.serviceRange)
            if (v >= r.from && v <= r.to) { hits.append({QStringLiteral("uuid"), r.cls, r.model, QStringLiteral("ble_uuid:") + u, QStringLiteral("service %1").arg(u), r.tier}); break; }
    }
    const bool tn = s.tnSerial.pattern().size() > 0
        && (s.tnSerial.match(name).hasMatch() || std::any_of(companies.begin(), companies.end(), [&s](const BleCompany &c) { return s.tnSerial.match(QString::fromLatin1(c.data)).hasMatch(); }));
    for (const BleCompany &c : companies) {
        for (const CompanyRule &r : s.company) {
            if (r.id != c.id) continue;
            const bool corroborated = r.corroboratedTier >= 0 && (flockName || tn);
            hits.append({QStringLiteral("company"), r.cls, r.model, QStringLiteral("ble_company:") + r.hex + (corroborated ? (tn ? QStringLiteral("+tn") : QStringLiteral("+name")) : QString()),
                          QStringLiteral("company %1 %2%3").arg(r.hex, r.vendor, corroborated ? (tn ? QStringLiteral(" with a TN serial") : QStringLiteral(" with a Flock name")) : QString()),
                          corroborated ? qMax(r.tier, r.corroboratedTier) : r.tier});
        }
    }
    return decide(s, hits, "ble");
}

} // namespace FlockDetector

QJsonObject FlockCamera::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("lat")] = lat;
    o[QStringLiteral("lon")] = lon;
    o[QStringLiteral("source")] = source;
    o[QStringLiteral("model")] = model;
    o[QStringLiteral("operator")] = operatorName;
    o[QStringLiteral("direction")] = direction;
    if (!bssid.isEmpty()) o[QStringLiteral("bssid")] = bssid;
    if (!bleMac.isEmpty()) o[QStringLiteral("bleMac")] = bleMac;
    o[QStringLiteral("confidence")] = confidence;
    o[QStringLiteral("detectionMethod")] = detectionMethod;
    if (firstSeen.isValid()) o[QStringLiteral("firstSeen")] = firstSeen.toString(Qt::ISODate);
    if (lastSeen.isValid()) o[QStringLiteral("lastSeen")] = lastSeen.toString(Qt::ISODate);
    o[QStringLiteral("sightingCount")] = sightingCount;
    o[QStringLiteral("passCount")] = passCount;
    o[QStringLiteral("vetted")] = vetted;
    if (vettedAt.isValid()) o[QStringLiteral("vettedAt")] = vettedAt.toString(Qt::ISODate);
    if (!notes.isEmpty()) o[QStringLiteral("notes")] = notes;
    o[QStringLiteral("seq")] = qint64(seq);
    if (!cameraType.isEmpty()) o[QStringLiteral("cameraType")] = cameraType;   // unclassified: no guess (the phone classifies from tags)
    if (!manufacturer.isEmpty()) o[QStringLiteral("manufacturer")] = manufacturer;
    if (osmVersion > 0) o[QStringLiteral("osmVersion")] = osmVersion;
    if (!osmTimestamp.isEmpty()) o[QStringLiteral("osmTimestamp")] = osmTimestamp;
    if (stale) o[QStringLiteral("stale")] = true;
    if (!tags.isEmpty()) {
        const QJsonObject t = QJsonDocument::fromJson(tags.toUtf8()).object();
        for (const char *k : {"contact:webcam", "webcam"})
            if (t.value(QLatin1String(k)).toString().startsWith(QLatin1String("http"))) { o[QStringLiteral("webcam")] = t.value(QLatin1String(k)).toString(); break; }
    }
    return o;
}

FlockCamera FlockCamera::fromJson(const QJsonObject &o)
{
    FlockCamera c;
    c.id = o[QStringLiteral("id")].toString();
    c.lat = o[QStringLiteral("lat")].toDouble();
    c.lon = o[QStringLiteral("lon")].toDouble();
    c.source = o[QStringLiteral("source")].toString();
    c.model = o[QStringLiteral("model")].toString();
    c.operatorName = o[QStringLiteral("operator")].toString();
    c.direction = o[QStringLiteral("direction")].toString();
    c.bssid = o[QStringLiteral("bssid")].toString();
    c.bleMac = o[QStringLiteral("bleMac")].toString();
    c.confidence = o[QStringLiteral("confidence")].toInt(100);
    c.detectionMethod = o[QStringLiteral("detectionMethod")].toString();
    if (o.contains(QStringLiteral("firstSeen")))
        c.firstSeen = QDateTime::fromString(o[QStringLiteral("firstSeen")].toString(), Qt::ISODate);
    if (o.contains(QStringLiteral("lastSeen")))
        c.lastSeen = QDateTime::fromString(o[QStringLiteral("lastSeen")].toString(), Qt::ISODate);
    c.sightingCount = o[QStringLiteral("sightingCount")].toInt(1);
    c.passCount = o[QStringLiteral("passCount")].toInt(0);
    c.vetted = o[QStringLiteral("vetted")].toBool();
    if (o.contains(QStringLiteral("vettedAt")))
        c.vettedAt = QDateTime::fromString(o[QStringLiteral("vettedAt")].toString(), Qt::ISODate);
    c.notes = o[QStringLiteral("notes")].toString();
    c.seq = qint64(o[QStringLiteral("seq")].toDouble());
    c.cameraType = o[QStringLiteral("cameraType")].toString();
    c.manufacturer = o[QStringLiteral("manufacturer")].toString();
    c.osmVersion = o[QStringLiteral("osmVersion")].toInt();
    c.osmTimestamp = o[QStringLiteral("osmTimestamp")].toString();
    c.stale = o[QStringLiteral("stale")].toBool();
    static const QStringList types{QStringLiteral("alpr"), QStringLiteral("webcam"), QStringLiteral("ptz"), QStringLiteral("cctv"), QStringLiteral("enforcement"), QStringLiteral("not_camera")};
    if (!types.contains(c.cameraType)) c.cameraType.clear();   // classified on insert
    return c;
}

QJsonObject LicensePlate::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("plate")] = plate;
    o[QStringLiteral("displayPlate")] = displayPlate;
    o[QStringLiteral("state")] = state;
    o[QStringLiteral("vehicleDesc")] = vehicleDesc;
    o[QStringLiteral("make")] = make;
    o[QStringLiteral("model")] = model;
    o[QStringLiteral("color")] = color;
    o[QStringLiteral("active")] = active;
    if (addedAt.isValid()) o[QStringLiteral("addedAt")] = addedAt.toString(Qt::ISODate);
    if (!notes.isEmpty()) o[QStringLiteral("notes")] = notes;
    return o;
}

LicensePlate LicensePlate::fromJson(const QJsonObject &o)
{
    LicensePlate p;
    p.plate = o[QStringLiteral("plate")].toString().toUpper().remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
    p.displayPlate = o[QStringLiteral("displayPlate")].toString();
    if (p.displayPlate.isEmpty()) p.displayPlate = p.plate;
    p.state = o[QStringLiteral("state")].toString();
    p.vehicleDesc = o[QStringLiteral("vehicleDesc")].toString();
    p.make = o[QStringLiteral("make")].toString();
    p.model = o[QStringLiteral("model")].toString();
    p.color = o[QStringLiteral("color")].toString();
    p.active = o.contains(QStringLiteral("active")) ? o[QStringLiteral("active")].toBool() : true;
    if (o.contains(QStringLiteral("addedAt")))
        p.addedAt = QDateTime::fromString(o[QStringLiteral("addedAt")].toString(), Qt::ISODate);
    else
        p.addedAt = QDateTime::currentDateTime();
    p.notes = o[QStringLiteral("notes")].toString();
    return p;
}

QJsonObject CameraEncounter::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("id")] = qint64(id);
    o[QStringLiteral("cameraId")] = cameraId;
    if (time.isValid()) o[QStringLiteral("time")] = time.toString(Qt::ISODate);
    o[QStringLiteral("lat")] = lat;
    o[QStringLiteral("lon")] = lon;
    o[QStringLiteral("distanceM")] = distanceM;
    o[QStringLiteral("speedKmh")] = speedKmh;
    o[QStringLiteral("plate")] = plate;
    o[QStringLiteral("vehicleDesc")] = vehicleDesc;
    o[QStringLiteral("device")] = device;
    o[QStringLiteral("encounterNum")] = encounterNum;
    if (!notes.isEmpty()) o[QStringLiteral("notes")] = notes;
    return o;
}

CameraEncounter CameraEncounter::fromJson(const QJsonObject &o)
{
    CameraEncounter e;
    e.id = qint64(o[QStringLiteral("id")].toDouble());
    e.cameraId = o[QStringLiteral("cameraId")].toString();
    if (o.contains(QStringLiteral("time")))
        e.time = QDateTime::fromString(o[QStringLiteral("time")].toString(), Qt::ISODate);
    e.lat = o[QStringLiteral("lat")].toDouble();
    e.lon = o[QStringLiteral("lon")].toDouble();
    e.distanceM = o[QStringLiteral("distanceM")].toDouble();
    e.speedKmh = o[QStringLiteral("speedKmh")].toDouble();
    e.plate = o[QStringLiteral("plate")].toString();
    e.vehicleDesc = o[QStringLiteral("vehicleDesc")].toString();
    e.device = o[QStringLiteral("device")].toString();
    e.encounterNum = o[QStringLiteral("encounterNum")].toInt(1);
    e.notes = o[QStringLiteral("notes")].toString();
    return e;
}

QJsonObject PlateAudit::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("id")] = qint64(id);
    o[QStringLiteral("plate")] = plate;
    o[QStringLiteral("cameraId")] = cameraId;
    o[QStringLiteral("lat")] = lat;
    o[QStringLiteral("lon")] = lon;
    o[QStringLiteral("operator")] = operatorName;
    if (timestamp.isValid()) o[QStringLiteral("timestamp")] = timestamp.toString(Qt::ISODate);
    o[QStringLiteral("source")] = source;
    o[QStringLiteral("confidence")] = confidence;
    o[QStringLiteral("details")] = details;
    return o;
}

PlateAudit PlateAudit::fromJson(const QJsonObject &o)
{
    PlateAudit a;
    a.id = qint64(o[QStringLiteral("id")].toDouble());
    a.plate = o[QStringLiteral("plate")].toString();
    a.cameraId = o[QStringLiteral("cameraId")].toString();
    a.lat = o[QStringLiteral("lat")].toDouble();
    a.lon = o[QStringLiteral("lon")].toDouble();
    a.operatorName = o[QStringLiteral("operator")].toString();
    if (o.contains(QStringLiteral("timestamp")))
        a.timestamp = QDateTime::fromString(o[QStringLiteral("timestamp")].toString(), Qt::ISODate);
    a.source = o[QStringLiteral("source")].toString();
    a.confidence = o[QStringLiteral("confidence")].toInt(100);
    a.details = o[QStringLiteral("details")].toString();
    return a;
}
