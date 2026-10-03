// SPDX-License-Identifier: Apache-2.0
#include "cameraimport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

namespace CameraImport {

void FeatureStream::feed(const QByteArray &chunk)
{
    const char *p = chunk.constData();
    const qsizetype n = chunk.size();
    qsizetype i = 0;
    if (m_state == Seeking) {
        // "features" followed by ':' and '[' — only a key can be followed by ':' (a string value is followed by , } or ])
        const qsizetype keep = m_head.size();
        m_head.append(chunk);
        qsizetype from = 0;
        for (;;) {
            const qsizetype k = m_head.indexOf("\"features\"", from);
            if (k < 0) break;
            qsizetype j = k + 10;
            while (j < m_head.size() && QChar::isSpace(uchar(m_head.at(j)))) ++j;
            if (j >= m_head.size()) { from = -1; break; }            // undecided: wait for more bytes
            if (m_head.at(j) != ':') { from = k + 1; continue; }
            ++j;
            while (j < m_head.size() && QChar::isSpace(uchar(m_head.at(j)))) ++j;
            if (j >= m_head.size()) { from = -1; break; }
            if (m_head.at(j) != '[') { from = k + 1; continue; }
            m_state = InArray;
            i = j + 1 - keep;                                         // where the array starts in this chunk
            m_head.clear();
            break;
        }
        if (m_state == Seeking) {
            if (m_head.size() > (4 << 20)) m_head = m_head.right(64);   // a huge preamble: keep only a possible split key
            return;
        }
        i = qMax<qsizetype>(0, i);                                    // (an earlier feed would have found an earlier '[')
    }
    qsizetype start = m_state == InFeature ? i : -1;
    for (; i < n && m_state != Done; ++i) {
        const char c = p[i];
        if (m_state == InArray) {
            if (c == '{') { m_state = InFeature; m_depth = 1; m_inStr = m_esc = false; start = i; }
            else if (c == ']') m_state = Done;
            continue;                                                 // whitespace, commas
        }
        // InFeature
        if (m_inStr) {
            if (m_esc) m_esc = false;
            else if (c == '\\') m_esc = true;
            else if (c == '"') m_inStr = false;
            continue;
        }
        if (c == '"') m_inStr = true;
        else if (c == '{' || c == '[') ++m_depth;
        else if (c == '}' || c == ']') {
            if (--m_depth == 0) {
                m_cur.append(p + start, i - start + 1);
                m_ready.append(m_cur);
                m_cur.clear();
                m_state = InArray;
                start = -1;
            }
        }
    }
    if (m_state == InFeature && start >= 0) m_cur.append(p + start, n - start);
}

bool FeatureStream::next(QJsonObject *feature)
{
    while (!m_ready.isEmpty()) {
        const QByteArray raw = m_ready.takeFirst();
        const QJsonDocument d = QJsonDocument::fromJson(raw);
        if (!d.isObject()) { ++m_bad; continue; }
        ++m_count;
        *feature = d.object();
        return true;
    }
    return false;
}

static bool point(const QJsonObject &feature, double *lat, double *lon)
{
    const QJsonArray c = feature.value(QLatin1String("geometry")).toObject().value(QLatin1String("coordinates")).toArray();
    if (c.size() < 2 || !c.at(0).isDouble() || !c.at(1).isDouble()) return false;
    *lon = c.at(0).toDouble();
    *lat = c.at(1).toDouble();
    return !(*lat == 0.0 && *lon == 0.0) && *lat >= -90 && *lat <= 90 && *lon >= -180 && *lon <= 180;
}

static QString num(double v) { return QString::number(v, 'g', 10); }

QString deflockDirections(const QJsonObject &props)
{
    QStringList out;
    for (const QJsonValue &v : props.value(QLatin1String("directions")).toArray())
        if (v.isDouble()) out << num(v.toDouble());
    if (out.isEmpty()) {
        const QJsonValue d = props.value(QLatin1String("direction"));
        if (d.isDouble()) out << num(d.toDouble());
        else if (d.isString() && !d.toString().trimmed().isEmpty()) out << d.toString().trimmed();
    }
    return out.join(QLatin1Char(';'));
}

bool fromDeflock(const QJsonObject &feature, const QDateTime &now, FlockCamera *out)
{
    FlockCamera c;
    if (!point(feature, &c.lat, &c.lon)) return false;
    const QJsonObject p = feature.value(QLatin1String("properties")).toObject();
    const qint64 osmId = p.value(QLatin1String("osmId")).toVariant().toLongLong();
    const QString type = p.value(QLatin1String("osmType")).toString(QStringLiteral("node"));
    if (osmId <= 0 || (type != QLatin1String("node") && type != QLatin1String("way") && type != QLatin1String("relation"))) return false;
    c.id = QStringLiteral("osm:%1/%2").arg(type).arg(osmId);
    c.source = QStringLiteral("deflock");
    c.operatorName = p.value(QLatin1String("operator")).toString().trimmed();
    c.manufacturer = p.value(QLatin1String("brand")).toString().trimmed();   // DeFlock: brand, else manufacturer
    c.direction = deflockDirections(p);
    c.osmVersion = p.value(QLatin1String("osmVersion")).toInt();
    c.osmTimestamp = p.value(QLatin1String("osmTimestamp")).toString();
    QStringList notes;
    if (const QString z = p.value(QLatin1String("surveillanceZone")).toString(); !z.isEmpty()) notes << QStringLiteral("zone %1").arg(z);
    if (const QString m = p.value(QLatin1String("mountType")).toString(); !m.isEmpty()) notes << QStringLiteral("mount %1").arg(m);
    c.notes = notes.join(QStringLiteral(" · "));
    c.confidence = 90;
    c.detectionMethod = QStringLiteral("osm_tag");
    c.cameraType = QStringLiteral("alpr");                      // DeFlock lists only surveillance:type=ALPR
    c.firstSeen = c.lastSeen = now;
    c.sightingCount = 1;
    *out = c;
    return true;
}

bool fromFlockLocations(const QJsonObject &feature, const QDateTime &now, FlockCamera *out)
{
    const QJsonObject p = feature.value(QLatin1String("properties")).toObject();
    const QJsonValue osm = p.value(QLatin1String("osm_id"));
    if (!(osm.isNull() || osm.isUndefined() || (osm.isString() && osm.toString().isEmpty()))) return false;   // OSM-derived: DeFlock has it
    FlockCamera c;
    if (!point(feature, &c.lat, &c.lon)) return false;
    const QString id = p.value(QLatin1String("id")).toVariant().toString();
    if (id.isEmpty()) return false;
    c.id = QStringLiteral("flock:%1").arg(id);
    c.model = p.value(QLatin1String("camera_type")).toString().trimmed();   // the submitter's label ("Flock Safety Solar", …)
    QStringList loc;
    for (const char *k : {"address", "city", "state"})
        if (const QString v = p.value(QLatin1String(k)).toString().trimmed(); !v.isEmpty()) loc << v;
    c.notes = loc.join(QStringLiteral(", "));
    if (const QString n = p.value(QLatin1String("notes")).toString().trimmed(); !n.isEmpty()) c.notes = c.notes.isEmpty() ? n : c.notes + QStringLiteral(" · ") + n;
    const QDateTime reported = QDateTime::fromString(p.value(QLatin1String("reported_at")).toString(), Qt::ISODate);
    c.firstSeen = c.lastSeen = reported.isValid() ? reported : now;
    c.source = p.value(QLatin1String("source")).toString(QStringLiteral("community"));
    if (c.source.isEmpty()) c.source = QStringLiteral("community");
    c.vetted = p.value(QLatin1String("verified")).toBool();
    if (c.vetted) c.vettedAt = c.firstSeen;
    c.confidence = c.vetted ? 95 : 85;
    c.detectionMethod = QStringLiteral("community_database");
    c.sightingCount = 1;
    *out = c;
    return true;
}

} // namespace CameraImport
