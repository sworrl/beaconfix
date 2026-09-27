#include "importers.h"
#include "locator.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>

namespace Importers {

static const int CHUNK = 1 << 20;          // stream buffer
static const int BATCH = 500;              // rows per Locator call

// ── Options / Summary ─────────────────────────────────────────────────────────
Options Options::fromJson(const QJsonObject &o)
{
    Options opt;
    if (o.contains("from")) opt.from = QDateTime::fromString(o["from"].toString(), Qt::ISODate);
    if (o.contains("to")) opt.to = QDateTime::fromString(o["to"].toString(), Qt::ISODate);
    if (o.contains("what")) {
        QStringList what; for (const QJsonValue &v : o["what"].toArray()) what << v.toString();
        if (o["what"].isString()) what = o["what"].toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (!what.isEmpty()) { opt.positions = what.contains(QStringLiteral("positions")); opt.wifi = what.contains(QStringLiteral("wifi")); opt.places = what.contains(QStringLiteral("places")); }
    }
    return opt;
}
QJsonObject Options::toJson() const
{
    QStringList what; if (positions) what << QStringLiteral("positions"); if (wifi) what << QStringLiteral("wifi"); if (places) what << QStringLiteral("places");
    return {{"from", from.isValid() ? QJsonValue(from.toString(Qt::ISODate)) : QJsonValue()}, {"to", to.isValid() ? QJsonValue(to.toString(Qt::ISODate)) : QJsonValue()}, {"what", QJsonArray::fromStringList(what)}};
}
QJsonObject Summary::toJson() const
{
    return {{"format", format}, {"file", file}, {"device", device}, {"positions", positions}, {"wifiScans", wifiScans}, {"observations", observations},
            {"visits", visits}, {"tracks", tracks}, {"skipped", skipped}, {"errors", errors}, {"beaconsTouched", beaconsTouched}, {"bytes", double(bytes)},
            {"first", first.isValid() ? QJsonValue(first.toString(Qt::ISODate)) : QJsonValue()}, {"last", last.isValid() ? QJsonValue(last.toString(Qt::ISODate)) : QJsonValue()},
            {"seconds", seconds}, {"error", error}};
}

// ── Streaming JSON array scanner ──────────────────────────────────────────────
JsonArrayStream::JsonArrayStream(QIODevice *dev) : m_dev(dev) { m_buf.reserve(CHUNK * 2); }

bool JsonArrayStream::fill()
{
    if (m_eof) return m_at < m_buf.size();
    if (m_at > 0 && m_at >= m_buf.size() / 2) { m_buf.remove(0, m_at); m_at = 0; }
    const QByteArray more = m_dev->read(CHUNK);
    if (more.isEmpty()) m_eof = true; else m_buf += more;
    return m_at < m_buf.size();
}

bool JsonArrayStream::seekKey(const QByteArray &key)
{
    const QByteArray needle = "\"" + key + "\"";
    while (true) {
        int idx = m_buf.indexOf(needle, m_at);
        while (idx < 0) {
            // keep a tail so a needle split across chunks is still found
            const int keep = qMin(m_buf.size(), needle.size());
            m_pos += m_buf.size() - keep - (m_at > m_buf.size() - keep ? 0 : 0);
            m_buf = m_buf.right(keep); m_at = 0;
            if (m_eof) return false;
            const QByteArray more = m_dev->read(CHUNK);
            if (more.isEmpty()) { m_eof = true; return false; }
            m_buf += more;
            idx = m_buf.indexOf(needle, 0);
        }
        int i = idx + needle.size();
        // `"key"` must be followed by ':' then '[' (whitespace allowed) — otherwise it was a value
        auto skipWs = [&] { while (true) { while (i < m_buf.size() && (m_buf[i] == ' ' || m_buf[i] == '\n' || m_buf[i] == '\r' || m_buf[i] == '\t')) ++i; if (i < m_buf.size() || m_eof) return; const QByteArray more = m_dev->read(CHUNK); if (more.isEmpty()) { m_eof = true; return; } m_buf += more; } };
        skipWs();
        if (i < m_buf.size() && m_buf[i] == ':') { ++i; skipWs(); if (i < m_buf.size() && m_buf[i] == '[') { m_at = i + 1; return true; } }
        m_at = idx + needle.size();
    }
}

bool JsonArrayStream::next(QByteArray *element)
{
    element->clear();
    // skip separators
    while (true) {
        if (!fill()) return false;
        const char c = m_buf[m_at];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ',') { ++m_at; continue; }
        if (c == ']') { ++m_at; return false; }
        break;
    }
    int depth = 0; bool inStr = false, esc = false; const int start = m_at; int i = m_at;
    const bool scalar = m_buf[start] != '{' && m_buf[start] != '[';
    while (true) {
        if (i >= m_buf.size()) {
            if (m_eof) { *element = m_buf.mid(start); m_at = m_buf.size(); return !element->trimmed().isEmpty(); }
            const QByteArray more = m_dev->read(CHUNK);
            if (more.isEmpty()) { m_eof = true; continue; }
            m_buf += more;
        }
        const char c = m_buf[i];
        if (scalar) {
            if (c == ',' || c == ']' || c == '\n') { *element = m_buf.mid(start, i - start).trimmed(); m_at = i; m_pos += i - start; return true; }
            ++i; continue;
        }
        if (inStr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') inStr = false; }
        else if (c == '"') inStr = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') { if (--depth == 0) { ++i; *element = m_buf.mid(start, i - start); m_at = i; m_pos += i - start; return true; } }
        ++i;
    }
}

// ── Small helpers ─────────────────────────────────────────────────────────────
QString macFromValue(const QJsonValue &v)
{
    if (v.isString()) {
        QString s = v.toString().trimmed().toUpper();
        if (s.contains(QLatin1Char(':')) && s.size() == 17) return s;
        if (s.contains(QLatin1Char('-')) && s.size() == 17) return s.replace(QLatin1Char('-'), QLatin1Char(':'));
        bool ok = false; const qulonglong n = s.toULongLong(&ok, s.startsWith(QLatin1String("0X")) ? 16 : 10);
        if (!ok) { if (s.size() == 12) { QString out; for (int i = 0; i < 12; i += 2) { if (i) out += QLatin1Char(':'); out += s.mid(i, 2); } return out; } return {}; }
        return macFromValue(QJsonValue(double(n)));
    }
    if (v.isDouble()) {
        const qulonglong n = qulonglong(v.toDouble());
        QString out;
        for (int i = 5; i >= 0; --i) { if (!out.isEmpty()) out += QLatin1Char(':'); out += QStringLiteral("%1").arg((n >> (8 * i)) & 0xff, 2, 16, QLatin1Char('0')); }
        return out.toUpper();
    }
    return {};
}

bool parseLatLng(const QString &s, double *lat, double *lon)
{
    static const QRegularExpression re(QStringLiteral("^\\s*(-?[0-9]+(?:\\.[0-9]+)?)\\s*°?\\s*,\\s*(-?[0-9]+(?:\\.[0-9]+)?)\\s*°?\\s*$"));
    const QRegularExpressionMatch m = re.match(s);
    if (!m.hasMatch()) return false;
    *lat = m.captured(1).toDouble(); *lon = m.captured(2).toDouble();
    return std::fabs(*lat) <= 90 && std::fabs(*lon) <= 180;
}

qint64 parseTime(const QJsonValue &v)
{
    if (v.isDouble()) { const double d = v.toDouble(); return d > 1e11 ? qint64(d / 1000) : qint64(d); }
    const QString s = v.toString().trimmed();
    if (s.isEmpty()) return 0;
    bool num = false; const double d = s.toDouble(&num);
    if (num) return d > 1e11 ? qint64(d / 1000) : qint64(d);
    const QDateTime t = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (t.isValid()) return t.toSecsSinceEpoch();
    const QDateTime t2 = QDateTime::fromString(s, Qt::ISODate);
    if (t2.isValid()) return t2.toSecsSinceEpoch();
    const QDateTime t3 = QDateTime::fromString(s, QStringLiteral("yyyy-MM-dd HH:mm:ss"));   // WiGLE
    return t3.isValid() ? t3.toSecsSinceEpoch() : 0;
}

QString wigleSecurity(const QString &a)
{
    const QString u = a.toUpper();
    if (u.contains(QLatin1String("WEP"))) return QStringLiteral("wep");
    const bool sae = u.contains(QLatin1String("SAE")), psk = u.contains(QLatin1String("PSK")), eap = u.contains(QLatin1String("EAP")), owe = u.contains(QLatin1String("OWE"));
    const bool rsn = u.contains(QLatin1String("RSN")) || u.contains(QLatin1String("WPA2")) || u.contains(QLatin1String("WPA3")), wpa1 = u.contains(QLatin1String("WPA-"));
    if (owe) return QStringLiteral("owe");
    if (u.contains(QLatin1String("SUITE-B-192")) || u.contains(QLatin1String("EAP-SUITE-B"))) return QStringLiteral("wpa3-eap192");
    if (sae && psk) return QStringLiteral("wpa2/3");
    if (sae) return QStringLiteral("wpa3");
    if (rsn && eap) return QStringLiteral("wpa2-eap");
    if (rsn && u.contains(QLatin1String("TKIP")) && !u.contains(QLatin1String("CCMP"))) return QStringLiteral("wpa2-tkip");
    if (rsn) return QStringLiteral("wpa2");
    if (wpa1) return QStringLiteral("wpa1");
    return QStringLiteral("open");
}

bool positionAt(const QList<PosSample> &sorted, qint64 t, PosSample *out)
{
    if (sorted.isEmpty()) return false;
    auto it = std::lower_bound(sorted.begin(), sorted.end(), t, [](const PosSample &p, qint64 v) { return p.t < v; });
    const PosSample *after = it == sorted.end() ? nullptr : &*it;
    const PosSample *before = it == sorted.begin() ? nullptr : &*(it - 1);
    if (before && after && after->t - t <= 120 && t - before->t <= 120 && after->t > before->t) {
        const double f = double(t - before->t) / double(after->t - before->t);
        PosSample p; p.t = t; p.lat = before->lat + (after->lat - before->lat) * f; p.lon = before->lon + (after->lon - before->lon) * f;
        p.acc = qMax(before->acc, after->acc); p.alt = before->alt;
        if (p.acc > 100) return false;
        *out = p; return true;
    }
    const PosSample *best = nullptr; qint64 bestDt = 61;
    if (before && t - before->t < bestDt) { best = before; bestDt = t - before->t; }
    if (after && after->t - t < bestDt) { best = after; bestDt = after->t - t; }
    if (!best || best->acc > 100) return false;
    *out = *best; out->t = t; return true;
}

QString detectFormat(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray head = f.read(64 * 1024);
    const QString lower = QFileInfo(path).fileName().toLower();
    if (head.startsWith("WigleWifi") || (lower.endsWith(QLatin1String(".csv")) && head.contains("CurrentLatitude"))) return QStringLiteral("wigle");
    if (head.contains("<gpx")) return QStringLiteral("gpx");
    if (head.contains("<kml")) return QStringLiteral("kml");
    if (head.contains("\"semanticSegments\"") || head.contains("\"rawSignals\"") || head.contains("\"userLocationProfile\"")) return QStringLiteral("timeline");
    if (head.contains("\"timelineObjects\"")) return QStringLiteral("semantic");
    if (head.contains("\"locations\"")) return QStringLiteral("records");
    if (head.contains("\"beaconfix\"") || (head.contains("\"aps\"") && head.contains("\"observations\""))) return QStringLiteral("beaconfix");
    // a Timeline.json whose first 64 KB are all semanticSegments still says so; a bare array of segments (some exports) too
    if (head.trimmed().startsWith("[") && (head.contains("\"visit\"") || head.contains("\"timelinePath\""))) return QStringLiteral("timeline");
    return {};
}

static bool inWindow(qint64 t, const Options &o)
{
    if (o.from.isValid() && t < o.from.toSecsSinceEpoch()) return false;
    if (o.to.isValid() && t > o.to.toSecsSinceEpoch()) return false;
    return true;
}
static void note(Summary *s, qint64 t)
{
    if (t <= 0) return;
    const QDateTime d = QDateTime::fromSecsSinceEpoch(t);
    if (!s->first.isValid() || d < s->first) s->first = d;
    if (!s->last.isValid() || d > s->last) s->last = d;
}
static double e7(const QJsonValue &v) { return v.toDouble() / 1e7; }
static double distanceM(double lat1, double lon1, double lat2, double lon2)
{
    const double k = M_PI / 180.0, dx = (lon2 - lon1) * k * std::cos((lat1 + lat2) * 0.5 * k), dy = (lat2 - lat1) * k;
    return 6371000.0 * std::sqrt(dx * dx + dy * dy);
}
static void report(Progress &p, QIODevice *dev, qint64 pos, const QString &stage) { if (p && dev) { const qint64 sz = dev->size(); p(sz > 0 ? int(qMin<qint64>(99, pos * 100 / sz)) : 0, stage); } }

// ── Google Timeline (on-device export, Timeline.json) ─────────────────────────
bool parseTimeline(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress)
{
    sum->format = QStringLiteral("timeline");
    QByteArray el;
    // Pass 1: positions from rawSignals (also visits' place locations + timelinePath points later)
    if (opt.positions || opt.wifi) {
        dev->seek(0);
        JsonArrayStream st(dev);
        QList<PosSample> batch;
        if (st.seekKey("rawSignals")) {
            int n = 0;
            while (st.next(&el)) {
                const QJsonObject o = QJsonDocument::fromJson(el).object();
                const QJsonObject pos = o["position"].toObject();
                if (pos.isEmpty()) continue;
                double lat, lon;
                const QString ll = pos["LatLng"].isString() ? pos["LatLng"].toString() : pos["point"].toString();
                if (!parseLatLng(ll, &lat, &lon)) { ++sum->skipped; continue; }
                PosSample p; p.t = parseTime(pos["timestamp"]); p.lat = lat; p.lon = lon; p.acc = pos["accuracyMeters"].toDouble(-1);
                if (pos["altitudeMeters"].isDouble()) p.alt = pos["altitudeMeters"].toDouble();
                if (p.t <= 0 || !inWindow(p.t, opt)) { ++sum->skipped; continue; }
                batch << p; ++sum->positions; note(sum, p.t);
                if (batch.size() >= BATCH) { if (sinks.positions) sinks.positions(batch); batch.clear(); if (++n % 10 == 0) report(progress, dev, st.pos(), QStringLiteral("positions")); }
            }
        }
        if (!batch.isEmpty() && sinks.positions) sinks.positions(batch);
    }
    // Pass 2: Wi-Fi scans
    if (opt.wifi) {
        dev->seek(0);
        JsonArrayStream st(dev);
        QList<WifiScan> batch;
        if (st.seekKey("rawSignals")) {
            int n = 0;
            while (st.next(&el)) {
                const QJsonObject o = QJsonDocument::fromJson(el).object();
                const QJsonObject ws = o["wifiScan"].toObject();
                if (ws.isEmpty()) continue;
                WifiScan s; s.t = parseTime(ws["deliveryTime"]);
                if (s.t <= 0 || !inWindow(s.t, opt)) { ++sum->skipped; continue; }
                for (const QJsonValue &v : ws["devicesRecords"].toArray()) {
                    const QJsonObject d = v.toObject();
                    const QString mac = macFromValue(d["mac"]);
                    if (mac.isEmpty()) continue;
                    s.devices << WifiRec{mac, d["rawRssi"].toInt(-80)};
                }
                if (s.devices.isEmpty()) continue;
                batch << s; ++sum->wifiScans; note(sum, s.t);
                if (batch.size() >= 200) { if (sinks.scans) sinks.scans(batch); batch.clear(); if (++n % 10 == 0) report(progress, dev, st.pos(), QStringLiteral("wifi scans")); }
            }
        }
        if (!batch.isEmpty() && sinks.scans) sinks.scans(batch);
    }
    // Pass 3: semantic segments → visits + timeline paths
    if (opt.places || opt.positions) {
        dev->seek(0);
        JsonArrayStream st(dev);
        QList<Visit> visits; QList<PosSample> path;
        if (st.seekKey("semanticSegments")) {
            while (st.next(&el)) {
                const QJsonObject o = QJsonDocument::fromJson(el).object();
                const qint64 t0 = parseTime(o["startTime"]), t1 = parseTime(o["endTime"]);
                const QJsonObject visit = o["visit"].toObject();
                if (opt.places && !visit.isEmpty()) {
                    const QJsonObject top = visit["topCandidate"].toObject();
                    double lat, lon;
                    if (parseLatLng(top["placeLocation"].toObject()["latLng"].toString(), &lat, &lon) && t0 > 0 && inWindow(t0, opt)) {
                        Visit v; v.start = t0; v.end = t1; v.lat = lat; v.lon = lon; v.type = top["semanticType"].toString(); v.name = top["placeId"].toString();
                        visits << v; ++sum->visits; note(sum, t0);
                    }
                }
                if (opt.positions) {
                    for (const QJsonValue &pv : o["timelinePath"].toArray()) {
                        const QJsonObject pt = pv.toObject();
                        double lat, lon;
                        if (!parseLatLng(pt["point"].toString(), &lat, &lon)) continue;
                        qint64 t = parseTime(pt["time"]);
                        if (t <= 0 && pt.contains("durationMinutesOffsetFromStartTime")) t = t0 + qint64(pt["durationMinutesOffsetFromStartTime"].toString().toDouble() * 60);
                        if (t <= 0 || !inWindow(t, opt)) continue;
                        PosSample p; p.t = t; p.lat = lat; p.lon = lon; p.acc = 50;
                        path << p; ++sum->tracks;
                        if (path.size() >= BATCH) { if (sinks.positions) sinks.positions(path); path.clear(); }
                    }
                }
                if (visits.size() >= 100) { if (sinks.visits) sinks.visits(visits); visits.clear(); }
            }
        }
        if (!visits.isEmpty() && sinks.visits) sinks.visits(visits);
        if (!path.isEmpty() && sinks.positions) sinks.positions(path);
    }
    return true;
}

// ── Google Takeout Records.json ───────────────────────────────────────────────
bool parseRecords(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress)
{
    sum->format = QStringLiteral("records");
    dev->seek(0);
    JsonArrayStream st(dev);
    if (!st.seekKey("locations")) return false;
    QByteArray el; QList<PosSample> batch; QList<WifiScan> scans; int n = 0;
    while (st.next(&el)) {
        const QJsonObject o = QJsonDocument::fromJson(el).object();
        if (!o.contains("latitudeE7") || !o.contains("longitudeE7")) { ++sum->skipped; continue; }
        PosSample p; p.t = parseTime(o.contains("timestamp") ? o["timestamp"] : o["timestampMs"]);
        p.lat = e7(o["latitudeE7"]); p.lon = e7(o["longitudeE7"]); p.acc = o["accuracy"].toDouble(-1);
        if (o["altitude"].isDouble()) p.alt = o["altitude"].toDouble();
        if (p.t <= 0 || !inWindow(p.t, opt)) { ++sum->skipped; continue; }
        if (opt.positions) { batch << p; ++sum->positions; }
        note(sum, p.t);
        if (opt.wifi && o.contains("wifiScan")) {
            WifiScan s; s.t = p.t;
            for (const QJsonValue &v : o["wifiScan"].toObject()["accessPoints"].toArray()) { const QJsonObject a = v.toObject(); const QString mac = macFromValue(a["mac"]); if (!mac.isEmpty()) s.devices << WifiRec{mac, a["strength"].toInt(-80)}; }
            if (!s.devices.isEmpty()) { scans << s; ++sum->wifiScans; }
        }
        if (batch.size() >= BATCH) { if (sinks.positions) sinks.positions(batch); batch.clear(); if (++n % 10 == 0) report(progress, dev, st.pos(), QStringLiteral("locations")); }
        if (scans.size() >= 200) { if (sinks.scans) sinks.scans(scans); scans.clear(); }
    }
    if (!batch.isEmpty() && sinks.positions) sinks.positions(batch);
    if (!scans.isEmpty() && sinks.scans) sinks.scans(scans);
    return true;
}

// ── Semantic Location History (monthly JSON) ──────────────────────────────────
bool parseSemantic(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress)
{
    sum->format = QStringLiteral("semantic");
    dev->seek(0);
    JsonArrayStream st(dev);
    if (!st.seekKey("timelineObjects")) return false;
    QByteArray el; QList<Visit> visits; QList<PosSample> path; int n = 0;
    while (st.next(&el)) {
        const QJsonObject o = QJsonDocument::fromJson(el).object();
        const QJsonObject pv = o["placeVisit"].toObject(), as = o["activitySegment"].toObject();
        if (!pv.isEmpty() && opt.places) {
            const QJsonObject loc = pv["location"].toObject(), dur = pv["duration"].toObject();
            const qint64 t0 = parseTime(dur["startTimestamp"].isUndefined() ? dur["startTimestampMs"] : dur["startTimestamp"]), t1 = parseTime(dur["endTimestamp"].isUndefined() ? dur["endTimestampMs"] : dur["endTimestamp"]);
            if (loc.contains("latitudeE7") && t0 > 0 && inWindow(t0, opt)) {
                Visit v; v.start = t0; v.end = t1; v.lat = e7(loc["latitudeE7"]); v.lon = e7(loc["longitudeE7"]); v.name = loc["name"].toString(); v.address = loc["address"].toString(); v.type = loc["semanticType"].toString();
                visits << v; ++sum->visits; note(sum, t0);
            }
        }
        if (!as.isEmpty() && opt.positions) {
            for (const QJsonValue &v : as["simplifiedRawPath"].toObject()["points"].toArray()) {
                const QJsonObject pt = v.toObject();
                PosSample p; p.t = parseTime(pt.contains("timestamp") ? pt["timestamp"] : pt["timestampMs"]); p.lat = e7(pt["latE7"]); p.lon = e7(pt["lngE7"]); p.acc = pt["accuracyMeters"].toDouble(-1);
                if (p.t <= 0 || !inWindow(p.t, opt)) continue;
                path << p; ++sum->tracks; note(sum, p.t);
            }
            const QJsonObject dur = as["duration"].toObject();
            const qint64 t0 = parseTime(dur["startTimestamp"].isUndefined() ? dur["startTimestampMs"] : dur["startTimestamp"]);
            const QJsonObject sl = as["startLocation"].toObject();
            if (sl.contains("latitudeE7") && t0 > 0 && inWindow(t0, opt)) { PosSample p; p.t = t0; p.lat = e7(sl["latitudeE7"]); p.lon = e7(sl["longitudeE7"]); p.acc = 100; path << p; ++sum->positions; }
        }
        if (visits.size() >= 100) { if (sinks.visits) sinks.visits(visits); visits.clear(); }
        if (path.size() >= BATCH) { if (sinks.positions) sinks.positions(path); path.clear(); if (++n % 5 == 0) report(progress, dev, st.pos(), QStringLiteral("segments")); }
    }
    if (!visits.isEmpty() && sinks.visits) sinks.visits(visits);
    if (!path.isEmpty() && sinks.positions) sinks.positions(path);
    return true;
}

// ── WiGLE CSV ─────────────────────────────────────────────────────────────────
static QStringList csvSplit(const QString &line)
{
    QStringList out; QString cur; bool q = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (q) { if (c == QLatin1Char('"')) { if (i + 1 < line.size() && line[i + 1] == QLatin1Char('"')) { cur += c; ++i; } else q = false; } else cur += c; }
        else if (c == QLatin1Char('"')) q = true;
        else if (c == QLatin1Char(',')) { out << cur; cur.clear(); }
        else cur += c;
    }
    out << cur;
    return out;
}

bool parseWigle(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress)
{
    sum->format = QStringLiteral("wigle");
    dev->seek(0);
    QTextStream in(dev);
    QStringList header; int idx[14]; for (int &i : idx) i = -1;
    QList<PosSample> positions; int n = 0; qint64 read = 0;
    while (!in.atEnd()) {
        const QString line = in.readLine(); read += line.size() + 1;
        if (line.startsWith(QLatin1String("WigleWifi")) || line.trimmed().isEmpty()) continue;
        const QStringList cols = csvSplit(line);
        if (header.isEmpty()) {
            if (!cols.contains(QStringLiteral("MAC")) || !cols.contains(QStringLiteral("CurrentLatitude"))) continue;
            header = cols;
            const char *names[14] = {"MAC", "SSID", "AuthMode", "FirstSeen", "Channel", "Frequency", "RSSI", "CurrentLatitude", "CurrentLongitude", "AltitudeMeters", "AccuracyMeters", "RCOIs", "MfgrId", "Type"};
            for (int i = 0; i < 14; ++i) idx[i] = header.indexOf(QString::fromLatin1(names[i]));
            continue;
        }
        auto col = [&](int i) { return idx[i] >= 0 && idx[i] < cols.size() ? cols[idx[i]] : QString(); };
        if (idx[13] >= 0 && col(13).toUpper() != QLatin1String("WIFI")) { ++sum->skipped; continue; }
        const QString mac = macFromValue(QJsonValue(col(0)));
        const qint64 t = parseTime(QJsonValue(col(3)));
        const double lat = col(7).toDouble(), lon = col(8).toDouble(), acc = col(10).isEmpty() ? 30 : col(10).toDouble();
        if (mac.isEmpty() || t <= 0 || (lat == 0 && lon == 0) || !inWindow(t, opt)) { ++sum->skipped; continue; }
        PosSample p; p.t = t; p.lat = lat; p.lon = lon; p.acc = acc; if (!col(9).isEmpty()) p.alt = col(9).toDouble();
        note(sum, t);
        if (opt.wifi && sinks.wigle) { sinks.wigle({WifiRec{mac, col(6).toInt()}}, p, col(1), wigleSecurity(col(2))); ++sum->wifiScans; }
        if (opt.positions) { if (positions.isEmpty() || positions.last().t != t) { positions << p; ++sum->positions; } }
        if (positions.size() >= BATCH) { if (sinks.positions) sinks.positions(positions); positions.clear(); }
        if (++n % 5000 == 0) report(progress, dev, read, QStringLiteral("rows"));
    }
    if (!positions.isEmpty() && sinks.positions) sinks.positions(positions);
    return !header.isEmpty();
}

// ── GPX / KML ─────────────────────────────────────────────────────────────────
bool parseGpx(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress)
{
    sum->format = QStringLiteral("gpx");
    dev->seek(0);
    QXmlStreamReader x(dev);
    QList<PosSample> batch; QList<Visit> wpts; int n = 0;
    while (!x.atEnd()) {
        x.readNext();
        if (!x.isStartElement()) continue;
        const QString name = x.name().toString();
        if (name == QLatin1String("trkpt") || name == QLatin1String("wpt") || name == QLatin1String("rtept")) {
            PosSample p; p.lat = x.attributes().value(QStringLiteral("lat")).toDouble(); p.lon = x.attributes().value(QStringLiteral("lon")).toDouble(); p.acc = 15;
            QString wname; const bool isWpt = name == QLatin1String("wpt");
            while (!(x.isEndElement() && x.name() == name) && !x.atEnd()) {
                x.readNext();
                if (!x.isStartElement()) continue;
                if (x.name() == QLatin1String("time")) p.t = parseTime(QJsonValue(x.readElementText()));
                else if (x.name() == QLatin1String("ele")) p.alt = x.readElementText().toDouble();
                else if (x.name() == QLatin1String("name")) wname = x.readElementText();
                else if (x.name() == QLatin1String("hdop")) p.acc = qMax(5.0, x.readElementText().toDouble() * 5);
            }
            if (p.t <= 0 || !inWindow(p.t, opt)) { ++sum->skipped; continue; }
            note(sum, p.t);
            if (isWpt) { if (opt.places) { Visit v; v.start = p.t; v.end = p.t; v.lat = p.lat; v.lon = p.lon; v.name = wname; v.type = QStringLiteral("waypoint"); wpts << v; ++sum->visits; } }
            else if (opt.positions) { batch << p; ++sum->tracks; }
            if (batch.size() >= BATCH) { if (sinks.positions) sinks.positions(batch); batch.clear(); if (++n % 5 == 0) report(progress, dev, dev->pos(), QStringLiteral("track points")); }
        }
    }
    if (!batch.isEmpty() && sinks.positions) sinks.positions(batch);
    if (!wpts.isEmpty() && sinks.visits) sinks.visits(wpts);
    return !x.hasError() || sum->tracks > 0;
}

bool parseKml(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress)
{
    sum->format = QStringLiteral("kml");
    dev->seek(0);
    QXmlStreamReader x(dev);
    QList<PosSample> batch; QList<Visit> marks; QList<qint64> whens; QList<QString> coords; QString pmName; qint64 pmTime = 0; int n = 0;
    auto flushTrack = [&] {
        for (int i = 0; i < qMin(whens.size(), coords.size()); ++i) {
            const QStringList c = coords[i].trimmed().split(QRegularExpression(QStringLiteral("[ ,]+")), Qt::SkipEmptyParts);
            if (c.size() < 2 || whens[i] <= 0 || !inWindow(whens[i], opt)) continue;
            PosSample p; p.t = whens[i]; p.lon = c[0].toDouble(); p.lat = c[1].toDouble(); p.acc = 15; if (c.size() > 2) p.alt = c[2].toDouble();
            batch << p; ++sum->tracks; note(sum, p.t);
        }
        whens.clear(); coords.clear();
    };
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const QString name = x.name().toString();
            if (name == QLatin1String("Placemark")) { pmName.clear(); pmTime = 0; }
            else if (name == QLatin1String("name")) pmName = x.readElementText();
            else if (name == QLatin1String("when")) { const qint64 t = parseTime(QJsonValue(x.readElementText())); if (pmTime == 0) pmTime = t; whens << t; }
            else if (name == QLatin1String("coord")) coords << x.readElementText();
            else if (name == QLatin1String("coordinates")) {
                const QString text = x.readElementText().trimmed();
                if (!text.contains(QLatin1Char(' ')) && opt.places && pmTime > 0 && inWindow(pmTime, opt)) {   // a Point placemark with a TimeStamp
                    const QStringList c = text.split(QLatin1Char(','));
                    if (c.size() >= 2) { Visit v; v.start = v.end = pmTime; v.lon = c[0].toDouble(); v.lat = c[1].toDouble(); v.name = pmName; v.type = QStringLiteral("placemark"); marks << v; ++sum->visits; note(sum, pmTime); }
                } else ++sum->skipped;                            // LineString without times: nothing to date it
            }
        } else if (x.isEndElement() && (x.name() == QLatin1String("Track") || x.name() == QLatin1String("Placemark"))) {
            if (!whens.isEmpty()) { flushTrack(); if (batch.size() >= BATCH) { if (sinks.positions) sinks.positions(batch); batch.clear(); if (++n % 5 == 0) report(progress, dev, dev->pos(), QStringLiteral("track")); } }
            whens.clear();
        }
    }
    if (!batch.isEmpty() && sinks.positions) sinks.positions(batch);
    if (!marks.isEmpty() && sinks.visits) sinks.visits(marks);
    return !x.hasError() || sum->tracks > 0 || sum->visits > 0;
}

// ── The whole run ─────────────────────────────────────────────────────────────
bool run(const QString &path, const Options &opt, Locator *loc, Progress progress, Summary *out)
{
    QElapsedTimer timer; timer.start();
    Summary sum; sum.file = path;
    const QString fmt = detectFormat(path);
    sum.format = fmt;
    QFile f(path);
    if (fmt.isEmpty() || !f.open(QIODevice::ReadOnly)) { sum.error = fmt.isEmpty() ? QStringLiteral("unrecognised format (Timeline.json, Records.json, Semantic Location History, WiGLE CSV, GPX, KML, BeaconFix export)") : f.errorString(); if (out) *out = sum; return false; }
    sum.bytes = f.size();
    if (fmt == QLatin1String("beaconfix")) {
        const int n = loc ? loc->DbImport(path) : -1;
        if (n < 0) sum.error = QStringLiteral("import failed"); else sum.observations = n;
        sum.seconds = timer.elapsed() / 1000.0; if (out) *out = sum; return n >= 0;
    }
    sum.device = fmt == QLatin1String("wigle") ? QStringLiteral("wigle") : fmt == QLatin1String("gpx") ? QStringLiteral("gpx") : fmt == QLatin1String("kml") ? QStringLiteral("kml") : QStringLiteral("timeline");
    const QString device = sum.device;
    QList<PosSample> allPositions;                  // for pairing scans (sorted after pass 1)
    QSet<QString> touched;
    QJsonArray fixBatch, obsBatch;
    auto pump = [] { QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20); };
    auto flushFixes = [&] { if (fixBatch.isEmpty()) return; if (loc) loc->appendPeerFixes(fixBatch, device); fixBatch = QJsonArray(); pump(); };
    auto flushObs = [&] { if (obsBatch.isEmpty()) return; if (loc) { const int n = loc->ingestObservations(obsBatch, device); if (n < 0) ++sum.errors; } obsBatch = QJsonArray(); pump(); };
    auto iso = [](qint64 t) { return QDateTime::fromSecsSinceEpoch(t).toString(Qt::ISODate); };
    Sinks sinks;
    PosSample lastKept; bool haveKept = false;
    sinks.positions = [&](const QList<PosSample> &l) {
        for (const PosSample &p : l) {
            allPositions << p;
            if (!opt.positions) continue;
            // Thin the stored track: one point per 30 s unless we moved 25 m (a Records.json logs every few seconds while driving)
            if (haveKept && p.t - lastKept.t < 30 && p.t >= lastKept.t && distanceM(lastKept.lat, lastKept.lon, p.lat, p.lon) < 25) { ++sum.skipped; continue; }
            lastKept = p; haveKept = true;
            QJsonObject fx{{"time", iso(p.t)}, {"lat", p.lat}, {"lon", p.lon}, {"acc", p.acc > 0 ? p.acc : 50.0}, {"source", QStringLiteral("import")}, {"provider", fmt}};
            if (p.alt > -9000) fx["elev"] = p.alt;
            fixBatch.append(fx);
            if (fixBatch.size() >= BATCH) flushFixes();
        }
    };
    sinks.visits = [&](const QList<Visit> &l) {
        for (const Visit &v : l) {
            QJsonObject fx{{"time", iso(v.start)}, {"lat", v.lat}, {"lon", v.lon}, {"acc", 30.0}, {"source", QStringLiteral("visit")}, {"provider", fmt},
                           {"place", v.name.isEmpty() ? v.type : v.name}, {"city", v.address}, {"departed", v.end > v.start ? iso(v.end) : QString()}};
            fixBatch.append(fx);
            if (fixBatch.size() >= BATCH) flushFixes();
        }
    };
    sinks.wigle = [&](const QList<WifiRec> &recs, const PosSample &p, const QString &ssid, const QString &security) {
        for (const WifiRec &r : recs) {
            obsBatch.append(QJsonObject{{"bssid", r.bssid}, {"ssid", ssid}, {"security", security}, {"time", iso(p.t)}, {"lat", p.lat}, {"lon", p.lon}, {"acc", p.acc > 0 ? p.acc : 30.0}, {"dbm", r.dbm}, {"source", QStringLiteral("wigle")}});
            touched.insert(r.bssid); ++sum.observations;
            if (obsBatch.size() >= BATCH) flushObs();
        }
    };
    QList<WifiScan> pendingScans;                   // scans arrive in pass 2, positions are complete by then
    sinks.scans = [&](const QList<WifiScan> &l) { pendingScans += l; if (pendingScans.size() >= 2000) { /* pair now, positions are already complete */ } };
    bool ok = false;
    Progress prog = [&](int pct, const QString &stage) { if (progress) progress(pct, stage); pump(); };
    if (fmt == QLatin1String("timeline")) ok = parseTimeline(&f, opt, sinks, &sum, prog);
    else if (fmt == QLatin1String("records")) ok = parseRecords(&f, opt, sinks, &sum, prog);
    else if (fmt == QLatin1String("semantic")) ok = parseSemantic(&f, opt, sinks, &sum, prog);
    else if (fmt == QLatin1String("wigle")) ok = parseWigle(&f, opt, sinks, &sum, prog);
    else if (fmt == QLatin1String("gpx")) ok = parseGpx(&f, opt, sinks, &sum, prog);
    else if (fmt == QLatin1String("kml")) ok = parseKml(&f, opt, sinks, &sum, prog);
    flushFixes();
    // Pair Wi-Fi scans with positions
    if (!pendingScans.isEmpty()) {
        std::sort(allPositions.begin(), allPositions.end(), [](const PosSample &a, const PosSample &b) { return a.t < b.t; });
        if (progress) progress(95, QStringLiteral("pairing scans"));
        int i = 0;
        for (const WifiScan &s : pendingScans) {
            PosSample at;
            if (!positionAt(allPositions, s.t, &at)) { ++sum.skipped; continue; }
            for (const WifiRec &r : s.devices) {
                obsBatch.append(QJsonObject{{"bssid", r.bssid}, {"time", iso(s.t)}, {"lat", at.lat}, {"lon", at.lon}, {"acc", at.acc > 0 ? at.acc : 50.0}, {"dbm", r.dbm}, {"source", QStringLiteral("timeline")}});
                touched.insert(r.bssid); ++sum.observations;
                if (obsBatch.size() >= BATCH) flushObs();
            }
            if (++i % 500 == 0 && progress) progress(95 + int(4.0 * i / pendingScans.size()), QStringLiteral("pairing scans"));
        }
    }
    flushObs();
    sum.beaconsTouched = touched.size();
    if (!ok && sum.error.isEmpty()) sum.error = QStringLiteral("could not parse the file as %1").arg(fmt);
    sum.seconds = timer.elapsed() / 1000.0;
    if (progress) progress(100, QStringLiteral("done"));
    if (out) *out = sum;
    return ok;
}

}
