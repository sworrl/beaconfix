// SPDX-License-Identifier: Apache-2.0
#include "platewatch.h"
#include "hibf.h"
#include "imagestore.h"
#include "mapdb.h"
#include "routeplanner.h"
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>
#include <memory>

static const QByteArray kUa = "BeaconFix/" BEACONFIX_VERSION " (+https://github.com/sworrl/beaconfix)";
static const QByteArray kHibfUa = "BeaconFix/" BEACONFIX_VERSION " (personal plate watch)";
static const char *kHibfSearch = "https://haveibeenflocked.com/api/search/text";
static const char *kHibfSources = "https://haveibeenflocked.com/api/sources/stats";
static const char *kOverpass[] = {"https://overpass-api.de/api/interpreter", "https://overpass.private.coffee/api/interpreter"};
static const char *kPanoramax[] = {"https://api.panoramax.xyz", "https://panoramax.openstreetmap.fr", "https://panoramax.ign.fr"};
static constexpr int kPanoramaxHosts = 3;
static constexpr qint64 kMaxImageBytes = 25LL * 1024 * 1024;
static constexpr qint64 kMaxUploadBytes = 40LL * 1024 * 1024;

static QString nowIso() { return QDateTime::currentDateTime().toString(Qt::ISODate); }
static QString compact(const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
static QJsonObject parseObj(const QString &s) { return QJsonDocument::fromJson(s.toUtf8()).object(); }
static QString stripHtml(QString s) { s.remove(QRegularExpression(QStringLiteral("<[^>]*>"))); return s.simplified(); }

// §2.0: webcam stills are off by default and opted in per provider — QSettings "webcamStills": true (every
// provider) or a list of host names ("wv511.org"). WV511's terms forbid storing its images in a retrieval system.
bool PlateWatch::webcamStillsAllowed(const QString &host)
{
    const QVariant v = QSettings().value(QStringLiteral("webcamStills"));
    if (!v.isValid()) return false;
    if (v.typeId() == QMetaType::Bool || v.toString() == QLatin1String("true") || v.toString() == QLatin1String("false")) return v.toBool();
    for (const QString &h : v.toStringList()) {
        const QString t = h.trimmed().toLower();
        if (!t.isEmpty() && (host.toLower() == t || host.toLower().endsWith(QLatin1Char('.') + t))) return true;
    }
    return false;
}

// A stale camera (its source no longer confirms it, docs/DATABASE.md) keeps the passes it has but gets no new ones
static QList<FlockCamera> confirmedOnly(QList<FlockCamera> cams)
{
    cams.removeIf([](const FlockCamera &c) { return c.stale; });
    return cams;
}

PlateWatch::PlateWatch(MapDb *db, QObject *parent) : QObject(parent), m_db(db)
{
    m_incTimer.setInterval(5 * 60 * 1000);
    connect(&m_incTimer, &QTimer::timeout, this, [this] { startBackfill(false); });
    m_incSoon.setSingleShot(true);
    connect(&m_incSoon, &QTimer::timeout, this, [this] { startBackfill(false); });
    m_liveTimer.setInterval(30000);
    connect(&m_liveTimer, &QTimer::timeout, this, [this] { livePasses(m_live.tick(QDateTime::currentMSecsSinceEpoch())); });
    m_hibfTimer.setInterval(5 * 60 * 1000);
    connect(&m_hibfTimer, &QTimer::timeout, this, &PlateWatch::hibfTick);
    m_eofTimer.setInterval(6 * 3600 * 1000);
    connect(&m_eofTimer, &QTimer::timeout, this, &PlateWatch::eofTick);
    // docs/SIGHTINGS.md §8: the ALPRs (not stale) in the corridor, with the trust this session already knows
    m_router = new RoutePlanner([this](double latMin, double latMax, double lonMin, double lonMax) {
        QList<AvoidRoute::Cam> out;
        if (!m_db || !m_db->isOpen()) return out;
        for (PlateEvents::Camera &k : toCameras(confirmedOnly(m_db->loadFlockCamerasIn(latMin, latMax, lonMin, lonMax)))) {
            if (k.type != QLatin1String("alpr")) continue;
            const auto it = m_trustCache.constFind(k.id);
            if (it != m_trustCache.constEnd()) k.trust = it->trust;
            out << AvoidRoute::fromCamera(k);
        }
        return out;
    }, this);
}

void PlateWatch::start(bool full)
{
    if (m_started) return;
    m_started = true;
    m_full = full && m_db && m_db->isOpen() && !m_db->readOnly();
    if (!m_full) return;
    m_incTimer.start();
    m_liveTimer.start();
    m_hibfTimer.start();
    // The first start of this version runs the full backfill (no kv yet); later starts resume / go incremental
    QTimer::singleShot(15000, this, [this] { startBackfill(false); });
    QTimer::singleShot(25000, this, &PlateWatch::hibfTick);       // the first run checks immediately (all pages)
    // §2.6 / §2.7: every stored pass gets the camera's trust and the snap of its cached roads; missing roads are fetched
    QTimer::singleShot(20000, this, &PlateWatch::rescoreAll);
    m_eofTimer.start();
    QTimer::singleShot(40000, this, &PlateWatch::eofTick);        // §4.6 Eyes on Flock, weekly
}

// ── helpers ───────────────────────────────────────────────────────────────────
QString PlateWatch::activePlate() const { return m_db->activeLicensePlate().displayPlate; }

QStringList PlateWatch::plates() const
{
    QStringList out;
    for (const LicensePlate &p : m_db->loadLicensePlates()) {
        const QString d = p.displayPlate.isEmpty() ? p.plate : p.displayPlate;
        if (!d.trimmed().isEmpty() && !out.contains(d)) out << d;
    }
    return out;
}

QJsonArray PlateWatch::agencies() const { return parseObj(m_db->kv(QStringLiteral("hibf_sources"))).value(QLatin1String("agencies")).toArray(); }


QList<PlateEvents::Camera> PlateWatch::toCameras(const QList<FlockCamera> &cams) const
{
    QList<PlateEvents::Camera> out;
    out.reserve(cams.size());
    for (const FlockCamera &c : cams) {
        PlateEvents::Camera k;
        k.id = c.id; k.lat = c.lat; k.lon = c.lon; k.operatorName = c.operatorName; k.model = c.model; k.direction = c.direction; k.source = c.source;
        k.manufacturer = c.manufacturer;
        k.tags = parseObj(c.tags);
        k.type = c.cameraType.isEmpty() ? PlateEvents::classifyCamera(c.model, c.source, c.id, c.detectionMethod, k.tags) : c.cameraType;
        k.webcam = PlateEvents::webcamUrl(k.tags);
        k.sourceConfidence = c.confidence;
        // the state (§4.4 matching) rides along in the tags: addr:state, else ", TX" at the end of the notes
        if (!k.tags.contains(QLatin1String("addr:state"))) { const QString st = Hibf::stateFromText(c.notes); if (!st.isEmpty()) k.tags["_state"] = st; }
        out << k;
    }
    return out;
}

static QString cameraState(const PlateEvents::Camera &c)
{
    const QString s = c.tags.value(QLatin1String("addr:state")).toString(c.tags.value(QLatin1String("_state")).toString());
    return s.size() == 2 ? s.toUpper() : QString();
}

static QJsonObject eventFor(const PlateEvents::Pass &p, const PlateEvents::Camera &cam, const QString &plate, const QString &source, const QJsonArray &agencies)
{
    const QString match = Hibf::leakyMatch(cam.operatorName, cameraState(cam), agencies);
    PlateEvents::Camera clean = cam;
    clean.tags.remove(QStringLiteral("_state"));
    return PlateEvents::passEvent(p, clean, plate, source, QString(), !match.isEmpty(), match);
}

QNetworkReply *PlateWatch::get(const QUrl &url, const QByteArray &ua, const QList<QPair<QByteArray, QByteArray>> &headers)
{
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", ua.isEmpty() ? kUa : ua);
    for (const auto &h : headers) req.setRawHeader(h.first, h.second);
    req.setTransferTimeout(30000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *r = m_nam.get(req);
    connect(r, &QNetworkReply::downloadProgress, r, [r](qint64 got, qint64) { if (got > kMaxImageBytes) r->abort(); });
    return r;
}

// Encode on a worker (§3.1), store here. meta: kind, event_uid, camera_id, attribution, license, original_url, captured_at
void PlateWatch::storeEncoded(const QByteArray &bytes, const QJsonObject &meta, std::function<void(bool, const QString &)> done)
{
    QPointer<PlateWatch> self(this);
    QThreadPool::globalInstance()->start([self, bytes, meta, done] {
        const ImageStore::Encoded e = ImageStore::encodeLossless(bytes);
        QMetaObject::invokeMethod(self.data(), [self, e, meta, done] {
            if (!self) return;
            if (!e.ok()) { if (done) done(false, e.error); return; }
            MapDb::MediaRow m;
            m.eventUid = meta.value(QLatin1String("event_uid")).toString(); m.cameraId = meta.value(QLatin1String("camera_id")).toString();
            m.kind = meta.value(QLatin1String("kind")).toString(); m.mime = e.mime; m.width = e.width; m.height = e.height;
            m.originalUrl = meta.value(QLatin1String("original_url")).toString(); m.originalMime = e.originalMime; m.originalBytes = e.originalBytes;
            m.originalSha256 = e.originalSha256; m.jpegReconstructible = e.jpegReconstructible;
            m.attribution = meta.value(QLatin1String("attribution")).toString(); m.license = meta.value(QLatin1String("license")).toString();
            m.capturedAt = meta.value(QLatin1String("captured_at")).toString();
            const bool ok = self->m_db->storeMedia(m, e.data);
            if (ok) emit self->eventsChanged();
            if (done) done(ok, ok ? ImageStore::mediaUid(e.data) : QStringLiteral("could not store the image"));
        }, Qt::QueuedConnection);
    });
}

// ── backfill (§2.5) ───────────────────────────────────────────────────────────
QJsonObject PlateWatch::backfillState() const { return parseObj(m_db->kv(QStringLiteral("plate_events_backfill"))); }
QJsonObject PlateWatch::hibfState() const { return parseObj(m_db->kv(QStringLiteral("hibf_watch"))); }
void PlateWatch::setBackfillKv(const QJsonObject &o) { m_db->setKv(QStringLiteral("plate_events_backfill"), compact(o)); }

// [from, to) split at local midnights
static void addDays(QList<QPair<QString, QString>> *out, const QDateTime &from, const QDateTime &to)
{
    QDateTime a = from;
    while (a < to) {
        QDateTime b = QDateTime(a.date().addDays(1), QTime(0, 0));
        if (b > to) b = to;
        out->append({a.toString(Qt::ISODate), b.toString(Qt::ISODate)});
        a = b;
    }
}

void PlateWatch::startBackfill(bool restart)
{
    if (!m_full || !m_db->isOpen() || m_db->readOnly()) return;
    if (m_bfRunning) { if (restart) m_restartPending = true; return; }
    QJsonObject st = backfillState();
    QList<QPair<QString, QString>> days;
    const QDateTime now = QDateTime::currentDateTime();
    const bool fresh = restart || st.value(QLatin1String("version")).toInt() != 1;
    const bool resume = !fresh && st.value(QLatin1String("status")).toString() == QLatin1String("running") && st.value(QLatin1String("phase")).toString() == QLatin1String("full");
    if (fresh || resume) {
        const QString first = m_db->firstFixTime();
        if (first.isEmpty()) {
            setBackfillKv(QJsonObject{{"version", 1}, {"status", "done"}, {"phase", "full"}, {"through", ""}, {"throughSeq", double(m_db->currentSeq())},
                                      {"passes", 0}, {"started", nowIso()}, {"finished", nowIso()}});
            return;
        }
        QDateTime from = QDateTime(QDateTime::fromString(first, Qt::ISODate).date(), QTime(0, 0));
        if (resume) { const QDateTime t = QDateTime::fromString(st.value(QLatin1String("through")).toString(), Qt::ISODate); if (t.isValid()) from = t; }
        addDays(&days, from, QDateTime(now.date().addDays(1), QTime(0, 0)));
        m_bfFullRun = true;
        if (fresh) st = QJsonObject{{"version", 1}, {"started", nowIso()}};
        st["phase"] = "full";
    } else {
        qint64 maxSeq = 0;
        const qint64 since = qint64(st.value(QLatin1String("throughSeq")).toDouble());
        const QHash<QString, QPair<QString, QString>> ranges = m_db->newFixRanges(since, &maxSeq);
        if (ranges.isEmpty()) { st["lastIncremental"] = nowIso(); st["throughSeq"] = double(qMax(since, maxSeq)); setBackfillKv(st); return; }
        // the new fixes' time ranges (every device), ±30 min, overlaps joined; all devices' fixes in them are re-run
        QList<QPair<QDateTime, QDateTime>> iv;
        for (auto it = ranges.cbegin(); it != ranges.cend(); ++it)
            iv.append({QDateTime::fromString(it->first, Qt::ISODate).addSecs(-1800), QDateTime::fromString(it->second, Qt::ISODate).addSecs(1800)});
        std::sort(iv.begin(), iv.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        QList<QPair<QDateTime, QDateTime>> merged;
        for (const auto &x : std::as_const(iv)) {
            if (!x.first.isValid() || !x.second.isValid()) continue;
            if (!merged.isEmpty() && x.first <= merged.last().second) merged.last().second = std::max(merged.last().second, x.second);
            else merged.append(x);
        }
        for (const auto &x : std::as_const(merged)) addDays(&days, x.first, x.second);
        m_bfFullRun = false;
        st["phase"] = "incremental";
    }
    m_chunks.clear();
    for (const auto &d : std::as_const(days)) m_chunks.append({d.first, d.second});
    m_bfTargetSeq = m_db->currentSeq();
    m_bfRunning = true;
    m_bfNewAlpr = m_bfNewCamera = 0;
    m_bfFirstNew.clear();
    m_bfCameras.clear();
    m_bfThrough = st.value(QLatin1String("through")).toString();
    st["status"] = "running";
    st["chunks"] = int(m_chunks.size());
    setBackfillKv(st);
    if (m_bfFullRun) qInfo("beaconfix: plate events: %s backfill over %d day(s) of route fixes", resume ? "resuming the" : "full", int(m_chunks.size()));
    QTimer::singleShot(0, this, &PlateWatch::nextChunk);
}

// §2.6: metrics.snap — the match, and how the watched way was chosen
static QJsonObject snapJson(const RoadSnap::Result &r, const RoadSnap::Watched &w)
{
    QJsonObject o = r.toJson();
    o["watchedBasis"] = w.basis;
    if (!std::isnan(w.distM)) o["watchedDistanceM"] = std::round(w.distM * 10) / 10;
    return o;
}

// Match the pass to the camera's roads using one device's fixes; P(read) × the snap factor
static void snapPass(PlateEvents::Pass &p, const PlateEvents::Camera &cam, const PlateWatch::Roads &roads, const QList<PlateEvents::TrackFix> &stream)
{
    if (!roads.fresh) return;
    int closest = 0;
    const QList<RoadSnap::Sample> smp = RoadSnap::samplesFor(stream, p, cam.lat, cam.lon, &closest);
    const RoadSnap::Result res = RoadSnap::snap(smp, closest, roads.ways, roads.watched);
    p.snap = snapJson(res, roads.watched);
    p.snapFactor = res.status == QLatin1String("ok") ? res.factor : 1.0;
    PlateEvents::rescore(p, cam);
}

namespace {
// One chunk on the worker: the cameras near the track, each against the fixes around it (a 0.01° grid), §2.1
QList<QJsonObject> detectChunk(const QList<PlateEvents::TrackFix> &fixes, const QList<PlateEvents::Camera> &cams, const QString &plate,
                               const QJsonArray &agencies, qint64 fromMs, qint64 toMs, const QHash<QString, PlateWatch::Roads> &roads)
{
    QList<QJsonObject> out;
    QHash<QString, QList<PlateEvents::TrackFix>> byDev;
    for (const PlateEvents::TrackFix &f : fixes) byDev[f.device].append(f);
    QStringList devs = byDev.keys();
    std::sort(devs.begin(), devs.end());
    QList<QList<PlateEvents::TrackFix>> streams;
    for (const QString &d : std::as_const(devs)) streams.append(byDev.value(d));
    const auto key = [](double lat, double lon) { return (qint64(std::floor(lat * 100.0)) << 32) ^ (qint64(std::floor(lon * 100.0)) & 0xffffffff); };
    QHash<qint64, QList<QPair<int, int>>> grid;                  // cell → (stream, index)
    for (int s = 0; s < int(streams.size()); ++s)
        for (int i = 0; i < int(streams[s].size()); ++i) grid[key(streams[s][i].lat, streams[s][i].lon)].append({s, i});
    for (const PlateEvents::Camera &cam : cams) {
        if (cam.type == QLatin1String("not_camera")) continue;    // gunshot detectors, guards, traffic counters: no pass
        QHash<int, QSet<int>> idx;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const auto it = grid.constFind(key(cam.lat + dy * 0.01, cam.lon + dx * 0.01));
                if (it == grid.constEnd()) continue;
                for (const auto &si : *it) { idx[si.first].insert(si.second); idx[si.first].insert(si.second - 1); idx[si.first].insert(si.second + 1); }
            }
        if (idx.isEmpty()) continue;
        QList<PlateEvents::Pass> passes;
        for (auto it = idx.cbegin(); it != idx.cend(); ++it) {
            const QList<PlateEvents::TrackFix> &st = streams[it.key()];
            QList<int> ix(it->cbegin(), it->cend());
            std::sort(ix.begin(), ix.end());
            QList<PlateEvents::TrackFix> run;
            int prev = -2;
            auto flush = [&] { if (!run.isEmpty()) passes += PlateEvents::detectPasses(run, cam); run.clear(); };
            for (int i : std::as_const(ix)) {
                if (i < 0 || i >= int(st.size())) continue;
                if (i != prev + 1) flush();                       // a hole in the index: never interpolate across it
                run.append(st[i]); prev = i;
            }
            flush();
        }
        const auto rd = roads.constFind(cam.id);
        for (PlateEvents::Pass p : PlateEvents::mergePasses(passes)) {
            if (p.ms < fromMs || p.ms >= toMs) continue;           // the neighbouring chunk owns it
            if (rd != roads.constEnd()) {                          // §2.6 on the closest approach's device's own fixes
                const int si = int(devs.indexOf(p.devices.isEmpty() ? QString() : p.devices.first()));
                if (si >= 0) snapPass(p, cam, *rd, streams[si]);
            }
            out.append(eventFor(p, cam, plate, QStringLiteral("route_backfill"), agencies));
        }
    }
    return out;
}
}

void PlateWatch::nextChunk()
{
    if (!m_bfRunning) return;
    if (m_chunks.isEmpty()) { finishBackfill(); return; }
    const Chunk c = m_chunks.takeFirst();
    const QDateTime from = QDateTime::fromString(c.from, Qt::ISODate), to = QDateTime::fromString(c.to, Qt::ISODate);
    const QList<PlateEvents::TrackFix> fixes = m_db->trackFixes(from.addSecs(-1800).toString(Qt::ISODate), to.addSecs(1800).toString(Qt::ISODate), QStringLiteral("*"));
    QList<PlateEvents::Camera> cams;
    if (!fixes.isEmpty()) cams = toCameras(confirmedOnly(m_db->camerasNearTrack(fixes, 150.0)));
    if (fixes.isEmpty() || cams.isEmpty()) { chunkDone({}, c); return; }
    applyTrust(cams);                                             // §2.7
    QHash<QString, Roads> roads;                                  // §2.6: the cached roads of these cameras
    for (const PlateEvents::Camera &k : std::as_const(cams)) { const Roads r = roadsFor(k); if (r.fresh) roads.insert(k.id, r); }
    for (const PlateEvents::TrackFix &f : fixes) if (f.ms < to.toMSecsSinceEpoch()) m_bfThrough = qMax(m_bfThrough, PlateEvents::isoLocal(f.ms));
    const QString plate = activePlate();
    const QJsonArray ag = agencies();
    const qint64 fromMs = from.toMSecsSinceEpoch(), toMs = to.toMSecsSinceEpoch();
    QPointer<PlateWatch> self(this);
    QThreadPool::globalInstance()->start([self, fixes, cams, plate, ag, fromMs, toMs, c, roads] {
        const QList<QJsonObject> events = detectChunk(fixes, cams, plate, ag, fromMs, toMs, roads);
        QMetaObject::invokeMethod(self.data(), [self, events, c] { if (self) self->chunkDone(events, c); }, Qt::QueuedConnection);
    });
}

void PlateWatch::chunkDone(const QList<QJsonObject> &events, const Chunk &c)
{
    bool any = false;
    for (const QJsonObject &ev : events) {
        bool created = false, changed = false;
        const QString uid = m_db->mergePlateEvent(ev, true, &created, &changed);
        if (uid.isEmpty()) continue;
        any = any || changed;
        m_bfCameras.insert(ev.value(QLatin1String("camera_id")).toString());
        if (!created) continue;
        if (ev.value(QLatin1String("camera_type")).toString() == QLatin1String("alpr")) ++m_bfNewAlpr; else ++m_bfNewCamera;
        const QString t = ev.value(QLatin1String("time")).toString();
        if (m_bfFirstNew.isEmpty() || t < m_bfFirstNew) m_bfFirstNew = t;
    }
    QJsonObject st = backfillState();
    st["through"] = m_bfFullRun ? c.to : qMax(st.value(QLatin1String("through")).toString(), m_bfThrough);
    st["passes"] = m_db->plateEventCounts().value(QLatin1String("cameraPass")).toInt();
    st["chunks"] = int(m_chunks.size());
    setBackfillKv(st);
    if (any) emit eventsChanged();
    QTimer::singleShot(0, this, &PlateWatch::nextChunk);
}

void PlateWatch::finishBackfill()
{
    m_bfRunning = false;
    QJsonObject st = backfillState();
    const QJsonObject counts = m_db->plateEventCounts();
    st["status"] = "done";
    st["finished"] = nowIso();
    st["through"] = m_bfThrough.isEmpty() ? st.value(QLatin1String("through")).toString() : m_bfThrough;
    st["throughSeq"] = double(m_bfTargetSeq);
    st["passes"] = counts.value(QLatin1String("cameraPass")).toInt();
    st["alprPasses"] = counts.value(QLatin1String("alprPass")).toInt();
    st["cameraPasses"] = counts.value(QLatin1String("cameraOnlyPass")).toInt();
    st["lastNew"] = QJsonObject{{"alpr", m_bfNewAlpr}, {"camera", m_bfNewCamera}};
    st.remove(QStringLiteral("chunks"));
    if (!m_bfFullRun) st["lastIncremental"] = nowIso();
    setBackfillKv(st);
    m_db->recountPasses();
    m_db->scheduleFlush();
    if (m_bfFullRun) qInfo("beaconfix: plate events: backfill done, %d camera passes (%d ALPR, %d other cameras), %d new",
                           st["passes"].toInt(), st["alprPasses"].toInt(), st["cameraPasses"].toInt(), m_bfNewAlpr + m_bfNewCamera);
    if (m_bfNewAlpr > 0) emit backfillSummary(m_bfNewAlpr, m_bfNewCamera, m_bfFirstNew);
    if (m_bfNewAlpr + m_bfNewCamera > 0) emit eventsChanged();
    fetchCameraPhotos(QStringList(m_bfCameras.cbegin(), m_bfCameras.cend()));
    ensureRoads(QStringList(m_bfCameras.cbegin(), m_bfCameras.cend()));   // §2.6: their passes are matched again once the roads are in
    if (m_restartPending) { m_restartPending = false; startBackfill(true); }
}

void PlateWatch::scheduleIncremental(int delayMs) { if (m_full && !m_incSoon.isActive()) m_incSoon.start(delayMs); }

// ── live (§2.1, §2.0 webcams) ─────────────────────────────────────────────────
void PlateWatch::onOwnFix(const Fix &f)
{
    if (!m_full || !f.valid || (f.lat == 0.0 && f.lon == 0.0) || f.accuracy > 500.0) return;
    PlateEvents::TrackFix tf;
    tf.ms = (f.time.isValid() ? f.time : QDateTime::currentDateTime()).toMSecsSinceEpoch();
    tf.lat = f.lat; tf.lon = f.lon; tf.acc = f.accuracy; tf.source = f.source;
    QList<PlateEvents::Camera> nearby;
    for (const PlateEvents::Camera &c : toCameras(confirmedOnly(m_db->loadFlockCamerasIn(f.lat - 0.004, f.lat + 0.004, f.lon - 0.005, f.lon + 0.005))))
        if (c.type != QLatin1String("not_camera")) nearby << c;
    applyTrust(nearby);                                           // §2.7 (cached: a hash lookup per camera)
    const QList<PlateEvents::Pass> done = m_live.addFix(tf, nearby);
    {   // §2.6: the roads of the cameras we are passing, so the finished pass can be matched
        QStringList act;
        for (const PlateEvents::Camera &c : m_live.activeCameras()) if (!m_roadsFresh.contains(c.id)) act << c.id;
        if (!act.isEmpty()) ensureRoads(act);
    }
    for (const PlateEvents::Camera &c : m_live.activeCameras())   // §2.0: a public webcam's still, on a live pass, when opted in
        if (c.type == QLatin1String("webcam") && !c.webcam.isEmpty() && webcamStillsAllowed(QUrl(c.webcam).host()) && !m_stillFetched.contains(c.id)) {
            m_stillFetched.insert(c.id); fetchWebcam(c);
        }
    if (!done.isEmpty()) {
        // the cameras of the finished passes (no longer in the tracker): from this fix's neighbourhood, else the table
        QList<PlateEvents::Pass> passes = done;
        for (const PlateEvents::Pass &p : std::as_const(passes)) {
            bool found = false;
            for (const PlateEvents::Camera &c : nearby) if (c.id == p.cameraId) found = true;
            if (!found) { bool ok = false; const FlockCamera fc = m_db->flockCamera(p.cameraId, &ok); if (ok) m_liveCams.insert(p.cameraId, toCameras({fc}).first()); }
        }
        for (const PlateEvents::Camera &c : nearby) m_liveCams.insert(c.id, c);
        livePasses(passes);
    }
    for (const PlateEvents::Camera &c : m_live.activeCameras()) m_liveCams.insert(c.id, c);
}

void PlateWatch::livePasses(const QList<PlateEvents::Pass> &passes)
{
    if (passes.isEmpty()) return;
    const QString plate = activePlate();
    const QJsonArray ag = agencies();
    for (const PlateEvents::Pass &p : passes) {
        PlateEvents::Camera cam = m_liveCams.value(p.cameraId);
        if (cam.id.isEmpty()) { bool ok = false; const FlockCamera fc = m_db->flockCamera(p.cameraId, &ok); if (!ok) continue; cam = toCameras({fc}).first(); }
        QList<PlateEvents::Camera> one{cam};
        applyTrust(one);
        cam = one.first();
        PlateEvents::Pass sp = p;
        sp.trustFactor = cam.trust >= 0 ? cam.trust : 1.0;
        PlateEvents::rescore(sp, cam);
        snapPass(sp, cam, roadsFor(cam), m_live.buffer());       // §2.6 (when its roads are cached; else after the fetch)
        QJsonObject ev = eventFor(sp, cam, plate, QStringLiteral("live_route"), ag);
        bool created = false, changed = false;
        const QString uid = m_db->mergePlateEvent(ev, true, &created, &changed);
        if (uid.isEmpty()) continue;
        ev["uid"] = uid;
        if (created && PlateEvents::alertable(cam.type)) emit alert(m_db->plateEvent(uid, false, false));
        m_stillTarget.insert(cam.id, {uid, p.ms});
        m_stillFetched.remove(cam.id);
        flushStills(cam.id);
        fetchCameraPhotos({cam.id});
        m_liveCams.remove(cam.id);
        if (changed) emit eventsChanged();
    }
}

// WV511's camera list (https://wv511.org/wsvc/gmap.asmx/buildCamerasJSONjs: "var camera_data = {count, cams:[{md5, start_lat,
// start_lng, …}]}"), reduced to [{id, lat, lon, host}] and kept for a day. host: the roadsummary.com stream host the record names.
void PlateWatch::wv511List(std::function<void(const QJsonArray &)> done)
{
    if (m_wv511At.isValid() && m_wv511At.secsTo(QDateTime::currentDateTime()) < 86400) { done(m_wv511); return; }
    QNetworkReply *r = get(QUrl(QStringLiteral("https://wv511.org/wsvc/gmap.asmx/buildCamerasJSONjs")), QByteArray(), {{"Referer", "https://wv511.org/webmapi.aspx"}});
    connect(r, &QNetworkReply::finished, this, [this, r, done] {
        r->deleteLater();
        const QString text = QString::fromUtf8(r->readAll());
        static const QRegularExpression re(QStringLiteral("camera_data\\s*=\\s*(\\{.*\\})\\s*;?\\s*$"), QRegularExpression::DotMatchesEverythingOption);
        const auto m = re.match(text);
        QJsonArray out;
        if (m.hasMatch()) {
            static const QRegularExpression hostRe(QStringLiteral("vtc\\d+\\.roadsummary\\.com"));
            for (const QJsonValue &v : QJsonDocument::fromJson(m.captured(1).toUtf8()).object().value(QLatin1String("cams")).toArray()) {
                const QJsonObject c = v.toObject();
                const QString id = c.value(QLatin1String("md5")).toString().trimmed().toUpper();
                if (id.isEmpty()) continue;
                const auto h = hostRe.match(QString::fromUtf8(QJsonDocument(c).toJson(QJsonDocument::Compact)));
                out.append(QJsonObject{{"id", id}, {"lat", c.value(QLatin1String("start_lat")).toVariant().toDouble()}, {"lon", c.value(QLatin1String("start_lng")).toVariant().toDouble()},
                                       {"host", h.hasMatch() ? h.captured(0) : QString()}});
            }
        }
        if (!out.isEmpty()) { m_wv511 = out; m_wv511At = QDateTime::currentDateTime(); }
        done(out.isEmpty() ? m_wv511 : out);
    });
}

void PlateWatch::fetchWebcam(const PlateEvents::Camera &cam)
{
    const QUrl url(cam.webcam);
    if (!url.isValid()) return;
    m_stillBusy[cam.id] += 1;
    const QString camId = cam.id, page = cam.webcam;
    if (url.host().endsWith(QLatin1String("wv511.org"), Qt::CaseInsensitive)) {
        // WV511 (WVDOT) has no still images: each camera is an HLS stream https://vtc{1,2,3}.roadsummary.com/rtplive/<id>/playlist.m3u8.
        // The camera (id = the list's md5, e.g. CAM064) by the OSM ref, else the CAMID in contact:webcam, else the nearest within ~100 m.
        QString camid;
        for (const auto &it : QUrlQuery(url).queryItems()) if (it.first.compare(QLatin1String("CAMID"), Qt::CaseInsensitive) == 0) camid = it.second.toUpper();
        const QString ref = cam.tags.value(QLatin1String("ref")).toString().trimmed().toUpper();
        const double lat = cam.lat, lon = cam.lon;
        wv511List([this, camId, camid, ref, lat, lon, page](const QJsonArray &list) {
            QString id, host;
            auto pick = [&](const QString &want) {
                for (const QJsonValue &v : list) if (v.toObject().value(QLatin1String("id")).toString() == want) { id = want; host = v.toObject().value(QLatin1String("host")).toString(); return true; }
                return false;
            };
            if (!(ref.startsWith(QLatin1String("CAM")) && pick(ref)) && !(camid.size() > 0 && pick(camid))) {
                double best = 100.0;
                for (const QJsonValue &v : list) {
                    const QJsonObject o = v.toObject();
                    const double dd = PlateEvents::distanceM(lat, lon, o.value(QLatin1String("lat")).toDouble(), o.value(QLatin1String("lon")).toDouble());
                    if (dd < best) { best = dd; id = o.value(QLatin1String("id")).toString(); host = o.value(QLatin1String("host")).toString(); }
                }
            }
            if (id.isEmpty()) id = !ref.isEmpty() ? ref : camid;           // the list did not answer: the camera's own ids
            if (id.isEmpty()) { m_stillBusy[camId] -= 1; flushStills(camId); return; }
            if (!host.isEmpty()) { const QString s = QStringLiteral("https://%1/rtplive/%2/playlist.m3u8").arg(host, id); m_streamUrl.insert(camId, s); }
            if (m_streamUrl.contains(camId)) { grabStream(camId, m_streamUrl.value(camId), QStringLiteral("https://wv511.org/"), page); return; }
            // no host from the list: the stream hosts directly
            auto probe = std::make_shared<std::function<void(int)>>();
            *probe = [this, camId, id, page, probe](int n) {
                if (n > 3) { m_stillBusy[camId] -= 1; flushStills(camId); return; }
                const QString s = QStringLiteral("https://vtc%1.roadsummary.com/rtplive/%2/playlist.m3u8").arg(n).arg(id);
                QNetworkReply *pr = get(QUrl(s), QByteArray(), {{"Referer", "https://wv511.org/"}});
                connect(pr, &QNetworkReply::finished, this, [this, pr, camId, s, page, probe, n] {
                    pr->deleteLater();
                    if (pr->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 && pr->readAll().startsWith("#EXTM3U")) {
                        m_streamUrl.insert(camId, s); grabStream(camId, s, QStringLiteral("https://wv511.org/"), page); return;
                    }
                    (*probe)(n + 1);
                });
            };
            (*probe)(1);
        });
        return;
    }
    QNetworkReply *r = get(url);
    connect(r, &QNetworkReply::finished, this, [this, r, camId, page] {
        r->deleteLater();
        const QString type = r->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        const QByteArray body = r->readAll();
        if (r->error() == QNetworkReply::NoError && (type.startsWith(QLatin1String("image/")) || !ImageStore::sniffMime(body).isEmpty())) {
            m_stills[camId].append({QDateTime::currentMSecsSinceEpoch(), body, page});
        } else if (r->error() == QNetworkReply::NoError && (type.contains(QLatin1String("mpegurl")) || body.startsWith("#EXTM3U"))) {
            grabStream(camId, r->url().toString(), r->url().toString(), page);
            return;
        }
        m_stillBusy[camId] -= 1;
        flushStills(camId);
    });
}

// One frame of a live HLS stream (ffmpeg, when installed): the "still" of a feed that has none
void PlateWatch::grabStream(const QString &camId, const QString &streamUrl, const QString &referer, const QString &pageUrl)
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) { m_stillBusy[camId] -= 1; flushStills(camId); return; }
    auto *p = new QProcess(this);
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    connect(p, &QProcess::finished, this, [this, p, camId, pageUrl, started](int code, QProcess::ExitStatus st) {
        const QByteArray png = p->readAllStandardOutput();
        p->deleteLater();
        if (st == QProcess::NormalExit && code == 0 && png.startsWith("\x89PNG")) m_stills[camId].append({started, png, pageUrl});
        m_stillBusy[camId] -= 1;
        flushStills(camId);
    });
    QTimer::singleShot(25000, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
    p->start(ffmpeg, {QStringLiteral("-nostdin"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-headers"),
                      QStringLiteral("Referer: %1\r\n").arg(referer), QStringLiteral("-i"), streamUrl, QStringLiteral("-frames:v"), QStringLiteral("1"),
                      QStringLiteral("-f"), QStringLiteral("image2pipe"), QStringLiteral("-vcodec"), QStringLiteral("png"), QStringLiteral("pipe:1")});
}

void PlateWatch::flushStills(const QString &camId)
{
    if (m_stillBusy.value(camId) > 0) return;
    m_stillBusy.remove(camId);
    if (!m_stillTarget.contains(camId)) {                         // the pass is not finished yet: keep them (but not forever)
        auto &l = m_stills[camId];
        const qint64 old = QDateTime::currentMSecsSinceEpoch() - 15 * 60000;
        l.erase(std::remove_if(l.begin(), l.end(), [old](const Still &s) { return s.ms < old; }), l.end());
        if (l.isEmpty()) m_stills.remove(camId);
        return;
    }
    const auto target = m_stillTarget.take(camId);
    const QList<Still> stills = m_stills.take(camId);
    if (stills.isEmpty()) return;
    const Still *best = &stills.first();
    for (const Still &s : stills) if (std::llabs(s.ms - target.second) < std::llabs(best->ms - target.second)) best = &s;
    bool ok = false;
    const FlockCamera fc = m_db->flockCamera(camId, &ok);
    const QString op = ok && !fc.operatorName.isEmpty() ? fc.operatorName : QStringLiteral("the operator");
    storeEncoded(best->bytes, QJsonObject{{"kind", "webcam"}, {"event_uid", target.first}, {"camera_id", QString()},
                                          {"attribution", QStringLiteral("Public live feed of %1 (captured %2 s from the closest approach)").arg(op).arg((best->ms - target.second) / 1000)},
                                          {"license", QString()}, {"original_url", best->url}, {"captured_at", PlateEvents::isoLocal(best->ms)}});
}

// ── camera photos (§3.2) ──────────────────────────────────────────────────────
void PlateWatch::fetchCameraPhotos(const QStringList &cameraIds, bool force)
{
    if (!m_full) return;
    const QDateTime now = QDateTime::currentDateTime();
    for (const QString &id : cameraIds) {
        if (id.isEmpty() || m_photoQueue.contains(id)) continue;
        if (!force) {
            const QJsonObject k = parseObj(m_db->kv(QStringLiteral("camera_photo_checked:") + id));
            if (k.value(QLatin1String("found")).toInt() > 0) continue;
            const QDateTime t = QDateTime::fromString(k.value(QLatin1String("checked")).toString(), Qt::ISODate);
            if (t.isValid() && t.daysTo(now) < 30) continue;
        }
        m_photoQueue.append(id);
    }
    if (!m_photoBusy) nextPhoto();
}

void PlateWatch::nextPhoto()
{
    if (m_photoBusy || m_photoQueue.isEmpty()) return;
    m_photoBusy = true;
    const QString id = m_photoQueue.takeFirst();
    bool ok = false;
    const FlockCamera cam = m_db->flockCamera(id, &ok);
    if (!ok) { photoDone(id, 0); return; }
    static const QRegularExpression osm(QStringLiteral("^osm:node/(\\d+)$"));
    const auto m = osm.match(id);
    if (!m.hasMatch()) { photoCandidates(id, parseObj(cam.tags), cam.lat, cam.lon); return; }
    const qint64 wait = m_lastOsm.isValid() ? qMax<qint64>(0, 1100 - m_lastOsm.msecsTo(QDateTime::currentDateTime())) : 0;   // ≤ 1 request/s
    const QString node = m.captured(1);
    QTimer::singleShot(int(wait), this, [this, id, node, cam] {
        m_lastOsm = QDateTime::currentDateTime();
        QNetworkReply *r = get(QUrl(QStringLiteral("https://api.openstreetmap.org/api/0.6/node/%1.json").arg(node)));
        connect(r, &QNetworkReply::finished, this, [this, r, id, cam] {
            r->deleteLater();
            QJsonObject tags = parseObj(cam.tags);
            if (r->error() == QNetworkReply::NoError) {
                const QJsonArray els = QJsonDocument::fromJson(r->readAll()).object().value(QLatin1String("elements")).toArray();
                const QJsonObject t = els.isEmpty() ? QJsonObject() : els.first().toObject().value(QLatin1String("tags")).toObject();
                if (!t.isEmpty()) {
                    tags = t;
                    const QString type = PlateEvents::classifyCamera(cam.model, cam.source, cam.id, cam.detectionMethod, tags);
                    m_db->setCameraClassification(id, type, compact(tags));     // the passes' raw gets the tags; a reclassification follows
                    emit eventsChanged();
                    if (type != QLatin1String("enforcement")) { enforcementCheck(id, cam, tags); return; }
                }
            } else if (r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 410) {
                qInfo("beaconfix: camera %s was deleted from OpenStreetMap", qPrintable(id));
            }
            photoCandidates(id, tags, cam.lat, cam.lon);
        });
    });
}

// §2.0 rule 1: a member of a type=enforcement relation is an enforcement camera (one more OSM request, ≤ 1/s)
void PlateWatch::enforcementCheck(const QString &id, const FlockCamera &cam, const QJsonObject &tags)
{
    const QString node = id.mid(QStringLiteral("osm:node/").size());
    const qint64 wait = m_lastOsm.isValid() ? qMax<qint64>(0, 1100 - m_lastOsm.msecsTo(QDateTime::currentDateTime())) : 0;
    QTimer::singleShot(int(wait), this, [this, id, node, cam, tags] {
        m_lastOsm = QDateTime::currentDateTime();
        QNetworkReply *r = get(QUrl(QStringLiteral("https://api.openstreetmap.org/api/0.6/node/%1/relations.json").arg(node)));
        connect(r, &QNetworkReply::finished, this, [this, r, id, cam, tags] {
            r->deleteLater();
            QJsonObject t = tags;
            for (const QJsonValue &v : QJsonDocument::fromJson(r->readAll()).object().value(QLatin1String("elements")).toArray())
                if (v.toObject().value(QLatin1String("tags")).toObject().value(QLatin1String("type")).toString() == QLatin1String("enforcement")) t["_enforcement"] = true;
            if (t != tags) { m_db->setCameraClassification(id, PlateEvents::classifyCamera(cam.model, cam.source, cam.id, cam.detectionMethod, t), compact(t)); emit eventsChanged(); }
            photoCandidates(id, t, cam.lat, cam.lon);
        });
    });
}

void PlateWatch::photoCandidates(const QString &camId, const QJsonObject &tags, double lat, double lon)
{
    QList<QJsonObject> cands;
    for (const QString &u : tags.value(QLatin1String("image")).toString().split(QLatin1Char(';'), Qt::SkipEmptyParts))
        if (u.trimmed().startsWith(QLatin1String("http"))) cands.append({{"type", "url"}, {"url", u.trimmed()}, {"attribution", "OpenStreetMap image tag"}});
    for (const QString &c : tags.value(QLatin1String("wikimedia_commons")).toString().split(QLatin1Char(';'), Qt::SkipEmptyParts))
        if (c.trimmed().startsWith(QLatin1String("File:"))) cands.append({{"type", "commons"}, {"title", c.trimmed()}});
    for (const QString &p : tags.value(QLatin1String("panoramax")).toString().split(QLatin1Char(';'), Qt::SkipEmptyParts))
        if (!p.trimmed().isEmpty()) cands.append({{"type", "panoramax"}, {"id", p.trimmed()}, {"host", 0}});
    const QString mly = QSettings().value(QStringLiteral("mapillaryToken")).toString();
    if (!mly.isEmpty())
        for (const QString &p : tags.value(QLatin1String("mapillary")).toString().split(QLatin1Char(';'), Qt::SkipEmptyParts))
            if (!p.trimmed().isEmpty()) cands.append({{"type", "mapillary"}, {"id", p.trimmed()}});
    if (!cands.isEmpty()) { fetchPhoto(camId, cands, 0); return; }
    panoramaxNear(camId, lat, lon, 0, [this, camId](QList<QJsonObject> near) { fetchPhoto(camId, near, 0); });
}

// The nearest Panoramax picture within 25 m (STAC search). The meta-catalogue first, then the big instances.
void PlateWatch::panoramaxNear(const QString &camId, double lat, double lon, int host, std::function<void(QList<QJsonObject>)> done)
{
    if (host >= kPanoramaxHosts) { done({}); return; }
    const double dLat = 25.0 / 111194.93, dLon = 25.0 / (111194.93 * std::cos(lat * M_PI / 180.0));
    QUrl u(QString::fromLatin1(kPanoramax[host]) + QStringLiteral("/api/search"));
    u.setQuery(QStringLiteral("bbox=%1,%2,%3,%4&limit=50").arg(lon - dLon, 0, 'f', 6).arg(lat - dLat, 0, 'f', 6).arg(lon + dLon, 0, 'f', 6).arg(lat + dLat, 0, 'f', 6));
    QNetworkReply *r = get(u);
    connect(r, &QNetworkReply::finished, this, [this, r, camId, lat, lon, host, done] {
        r->deleteLater();
        const QJsonDocument d = QJsonDocument::fromJson(r->readAll());
        if (r->error() != QNetworkReply::NoError || !d.isObject()) { panoramaxNear(camId, lat, lon, host + 1, done); return; }
        QJsonObject best; double bestD = 25.0;
        for (const QJsonValue &v : d.object().value(QLatin1String("features")).toArray()) {
            const QJsonObject f = v.toObject();
            const QJsonArray c = f.value(QLatin1String("geometry")).toObject().value(QLatin1String("coordinates")).toArray();
            if (c.size() < 2) continue;
            const double dist = PlateEvents::distanceM(lat, lon, c[1].toDouble(), c[0].toDouble());
            if (dist <= bestD) { bestD = dist; best = f; }
        }
        if (best.isEmpty()) { if (host == 0) done({}); else panoramaxNear(camId, lat, lon, host + 1, done); return; }
        const QJsonObject assets = best.value(QLatin1String("assets")).toObject(), props = best.value(QLatin1String("properties")).toObject();
        const QString href = assets.value(QLatin1String("hd")).toObject().value(QLatin1String("href")).toString(assets.value(QLatin1String("sd")).toObject().value(QLatin1String("href")).toString());
        if (href.isEmpty()) { done({}); return; }
        QString who = props.value(QLatin1String("geovisio:producer")).toString();
        if (who.isEmpty()) for (const QJsonValue &p : best.value(QLatin1String("providers")).toArray()) { who = p.toObject().value(QLatin1String("name")).toString(); break; }
        done({QJsonObject{{"type", "url"}, {"url", href}, {"attribution", QStringLiteral("%1 via Panoramax (%2 m from the camera)").arg(who.isEmpty() ? QStringLiteral("Panoramax contributor") : who).arg(qRound(bestD))},
                          {"license", props.value(QLatin1String("license")).toString()}}});
    });
}

void PlateWatch::fetchPhoto(const QString &camId, QList<QJsonObject> cands, int found)
{
    if (cands.isEmpty() || found >= 3) { photoDone(camId, found); return; }
    const QJsonObject c = cands.takeFirst();
    const QString type = c.value(QLatin1String("type")).toString();
    if (type == QLatin1String("url")) {
        const QString url = c.value(QLatin1String("url")).toString();
        QNetworkReply *r = get(QUrl(url));
        connect(r, &QNetworkReply::finished, this, [this, r, camId, cands, found, c, url] {
            r->deleteLater();
            const QByteArray body = r->readAll();
            const QString type = r->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
            if (r->error() != QNetworkReply::NoError || !(type.startsWith(QLatin1String("image/")) || !ImageStore::sniffMime(body).isEmpty())) { fetchPhoto(camId, cands, found); return; }
            storeEncoded(body, QJsonObject{{"kind", "camera_photo"}, {"camera_id", camId}, {"event_uid", QString()}, {"attribution", c.value(QLatin1String("attribution")).toString()},
                                           {"license", c.value(QLatin1String("license")).toString()}, {"original_url", url}},
                         [this, camId, cands, found](bool ok, const QString &) { fetchPhoto(camId, cands, found + (ok ? 1 : 0)); });
        });
        return;
    }
    if (type == QLatin1String("commons")) {
        QUrl u(QStringLiteral("https://commons.wikimedia.org/w/api.php"));
        QUrlQuery q; q.addQueryItem(QStringLiteral("action"), QStringLiteral("query")); q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
        q.addQueryItem(QStringLiteral("prop"), QStringLiteral("imageinfo")); q.addQueryItem(QStringLiteral("iiprop"), QStringLiteral("url|mime|size|extmetadata"));
        q.addQueryItem(QStringLiteral("titles"), c.value(QLatin1String("title")).toString());
        u.setQuery(q);
        QNetworkReply *r = get(u);
        connect(r, &QNetworkReply::finished, this, [this, r, camId, cands, found] {
            r->deleteLater();
            QList<QJsonObject> next = cands;
            const QJsonObject pages = QJsonDocument::fromJson(r->readAll()).object().value(QLatin1String("query")).toObject().value(QLatin1String("pages")).toObject();
            for (const QJsonValue &pv : pages) {
                const QJsonObject ii = pv.toObject().value(QLatin1String("imageinfo")).toArray().first().toObject();
                const QJsonObject em = ii.value(QLatin1String("extmetadata")).toObject();
                if (ii.value(QLatin1String("url")).toString().isEmpty() || !ii.value(QLatin1String("mime")).toString().startsWith(QLatin1String("image/"))) continue;
                if (ii.value(QLatin1String("size")).toDouble() > double(kMaxImageBytes)) continue;
                const QString artist = stripHtml(em.value(QLatin1String("Artist")).toObject().value(QLatin1String("value")).toString());
                next.prepend(QJsonObject{{"type", "url"}, {"url", ii.value(QLatin1String("url")).toString()},
                                         {"attribution", QStringLiteral("%1 via Wikimedia Commons").arg(artist.isEmpty() ? QStringLiteral("Unknown author") : artist)},
                                         {"license", stripHtml(em.value(QLatin1String("LicenseShortName")).toObject().value(QLatin1String("value")).toString())}});
            }
            fetchPhoto(camId, next, found);
        });
        return;
    }
    if (type == QLatin1String("panoramax")) {
        const int host = c.value(QLatin1String("host")).toInt();
        if (host >= kPanoramaxHosts) { fetchPhoto(camId, cands, found); return; }
        QUrl u(QString::fromLatin1(kPanoramax[host]) + QStringLiteral("/api/search"));
        u.setQuery(QStringLiteral("ids=%1").arg(c.value(QLatin1String("id")).toString()));
        QNetworkReply *r = get(u);
        connect(r, &QNetworkReply::finished, this, [this, r, camId, cands, found, c, host] {
            r->deleteLater();
            QList<QJsonObject> next = cands;
            const QJsonArray feats = QJsonDocument::fromJson(r->readAll()).object().value(QLatin1String("features")).toArray();
            if (r->error() != QNetworkReply::NoError || feats.isEmpty()) {           // another instance may hold it
                QJsonObject again = c; again["host"] = host + 1; next.prepend(again);
                fetchPhoto(camId, next, found); return;
            }
            const QJsonObject f = feats.first().toObject(), assets = f.value(QLatin1String("assets")).toObject(), props = f.value(QLatin1String("properties")).toObject();
            const QString href = assets.value(QLatin1String("hd")).toObject().value(QLatin1String("href")).toString(assets.value(QLatin1String("sd")).toObject().value(QLatin1String("href")).toString());
            if (!href.isEmpty())
                next.prepend(QJsonObject{{"type", "url"}, {"url", href}, {"attribution", QStringLiteral("%1 via Panoramax").arg(props.value(QLatin1String("geovisio:producer")).toString(QStringLiteral("Panoramax contributor")))},
                                         {"license", props.value(QLatin1String("license")).toString()}});
            fetchPhoto(camId, next, found);
        });
        return;
    }
    if (type == QLatin1String("mapillary")) {
        const QString tok = QSettings().value(QStringLiteral("mapillaryToken")).toString();
        QUrl u(QStringLiteral("https://graph.mapillary.com/%1").arg(c.value(QLatin1String("id")).toString()));
        QUrlQuery q; q.addQueryItem(QStringLiteral("fields"), QStringLiteral("thumb_original_url,creator")); q.addQueryItem(QStringLiteral("access_token"), tok);
        u.setQuery(q);
        QNetworkReply *r = get(u);
        connect(r, &QNetworkReply::finished, this, [this, r, camId, cands, found] {
            r->deleteLater();
            QList<QJsonObject> next = cands;
            const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
            const QString url = o.value(QLatin1String("thumb_original_url")).toString();
            if (!url.isEmpty())
                next.prepend(QJsonObject{{"type", "url"}, {"url", url}, {"attribution", QStringLiteral("%1 via Mapillary").arg(o.value(QLatin1String("creator")).toObject().value(QLatin1String("username")).toString(QStringLiteral("Mapillary contributor")))},
                                         {"license", "CC-BY-SA-4.0"}});
            fetchPhoto(camId, next, found);
        });
        return;
    }
    fetchPhoto(camId, cands, found);
}

void PlateWatch::photoDone(const QString &camId, int found)
{
    const int have = m_db->cameraMediaCount(camId, QStringLiteral("camera_photo"));
    m_db->setKv(QStringLiteral("camera_photo_checked:") + camId, compact(QJsonObject{{"checked", nowIso()}, {"found", qMax(found, have)}}));
    m_photoBusy = false;
    QTimer::singleShot(1200, this, &PlateWatch::nextPhoto);
}

// ── HaveIBeenFlocked (§4) ─────────────────────────────────────────────────────
QString PlateWatch::hibfMode() const
{
    const QDateTime now = QDateTime::currentDateTime();
    const QString since = now.addSecs(-72 * 3600).toString(Qt::ISODate);
    bool flock = false, leaky = false;
    for (const QJsonValue &v : m_db->plateEventsLatest(200, QStringLiteral("camera_pass"))) {
        const QJsonObject e = v.toObject();
        if (e.value(QLatin1String("time")).toString() < since) break;
        if (e.value(QLatin1String("camera_type")).toString() != QLatin1String("alpr")) continue;
        flock = true;
        if (e.value(QLatin1String("leaky")).toInt() == 1) leaky = true;
    }
    if (leaky) return Hibf::modeName(Hibf::Mode::Leaky);
    if (flock) return Hibf::modeName(Hibf::Mode::Flock);
    const QList<PlateEvents::TrackFix> fx = m_db->trackFixes(now.addSecs(-1800).toString(Qt::ISODate), QString(), QStringLiteral("*"));
    QHash<QString, PlateEvents::TrackFix> last;
    for (const PlateEvents::TrackFix &f : fx) {
        const auto it = last.constFind(f.device);
        if (it != last.constEnd()) {
            const double dt = double(f.ms - it->ms) / 1000.0;
            if (dt >= 5 && dt <= 600 && PlateEvents::distanceM(it->lat, it->lon, f.lat, f.lon) / dt > 20.0 / 3.6) return Hibf::modeName(Hibf::Mode::Driving);
        }
        last[f.device] = f;
    }
    return Hibf::modeName(Hibf::Mode::Idle);
}

static Hibf::Mode modeOf(const QString &n)
{
    if (n == QLatin1String("leaky")) return Hibf::Mode::Leaky;
    if (n == QLatin1String("flock")) return Hibf::Mode::Flock;
    if (n == QLatin1String("driving")) return Hibf::Mode::Driving;
    return Hibf::Mode::Idle;
}

bool PlateWatch::hibfMayRequest(QString *why) const
{
    if (m_hibfLastRequest.isValid() && m_hibfLastRequest.secsTo(QDateTime::currentDateTime()) < Hibf::kMinGapSecs) { if (why) *why = QStringLiteral("10 s floor"); return false; }
    const QJsonObject st = hibfState();
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    if (st.value(QLatin1String("day")).toString() == today && st.value(QLatin1String("dayCount")).toInt() >= Hibf::kMaxPerDay) { if (why) *why = QStringLiteral("30 requests today"); return false; }
    return true;
}

void PlateWatch::hibfCount()
{
    m_hibfLastRequest = QDateTime::currentDateTime();
    QJsonObject st = hibfState();
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    if (st.value(QLatin1String("day")).toString() != today) { st["day"] = today; st["dayCount"] = 0; }
    st["dayCount"] = st.value(QLatin1String("dayCount")).toInt() + 1;
    m_db->setKv(QStringLiteral("hibf_watch"), compact(st));
}

void PlateWatch::checkHibfNow()
{
    if (!m_full || m_hibfBusy) return;
    QJsonObject st = hibfState();
    st["nextCheck"] = nowIso();
    st["failures"] = 0;
    st.remove(QStringLiteral("lastCheck"));
    m_db->setKv(QStringLiteral("hibf_watch"), compact(st));
    hibfTick();
}

void PlateWatch::hibfTick()
{
    if (!m_full || m_hibfBusy || qEnvironmentVariableIsSet("BEACONFIX_NO_HIBF")) return;
    if (plates().isEmpty()) return;
    QJsonObject st = hibfState();
    const QDateTime now = QDateTime::currentDateTime();
    // the leaky-agency list, weekly (a failed fetch is retried after an hour)
    const QJsonObject src = parseObj(m_db->kv(QStringLiteral("hibf_sources")));
    const QDateTime fetched = QDateTime::fromString(src.value(QLatin1String("fetched")).toString(), Qt::ISODate);
    const QDateTime tried = QDateTime::fromString(st.value(QLatin1String("sourcesTried")).toString(), Qt::ISODate);
    if ((!fetched.isValid() || fetched.daysTo(now) >= 7) && (!tried.isValid() || tried.secsTo(now) >= 3600)) {
        QString why;
        if (hibfMayRequest(&why)) { hibfSources(); return; }
    }
    const QString mode = hibfMode();
    const QDateTime last = QDateTime::fromString(st.value(QLatin1String("lastCheck")).toString(), Qt::ISODate);
    QDateTime next;
    if (st.value(QLatin1String("failures")).toInt() > 0) next = QDateTime::fromString(st.value(QLatin1String("retryAt")).toString(), Qt::ISODate);
    else if (last.isValid()) next = last.addSecs(Hibf::intervalSecs(modeOf(mode)));
    st["mode"] = mode;
    st["nextCheck"] = next.isValid() ? next.toString(Qt::ISODate) : nowIso();
    m_db->setKv(QStringLiteral("hibf_watch"), compact(st));
    if (next.isValid() && now < next) return;
    QString why;
    if (!hibfMayRequest(&why)) return;
    m_hibfBusy = true;
    m_hibfNewHits = 0; m_hibfRows = 0;
    hibfSearch(QString(), 1);
}

void PlateWatch::hibfSources()
{
    m_hibfBusy = true;
    hibfCount();
    QJsonObject st = hibfState(); st["sourcesTried"] = nowIso(); m_db->setKv(QStringLiteral("hibf_watch"), compact(st));
    QNetworkRequest req{QUrl(QString::fromLatin1(kHibfSources))};
    req.setRawHeader("User-Agent", kHibfUa); req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(60000);
    QNetworkReply *r = m_nam.get(req);
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        m_hibfBusy = false;
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = r->readAll();
        if (r->error() == QNetworkReply::NoError && http == 200) {
            const QJsonObject parsed = Hibf::parseSources(body, nowIso());
            if (!parsed.value(QLatin1String("agencies")).toArray().isEmpty()) {
                m_db->setKv(QStringLiteral("hibf_sources"), compact(parsed));
                const int changed = m_db->updateLeakyPasses(parsed.value(QLatin1String("agencies")).toArray());
                qInfo("beaconfix: HaveIBeenFlocked: %d audit-log files, %d agencies; %d passes changed their leaky flag",
                      parsed.value(QLatin1String("files")).toInt(), int(parsed.value(QLatin1String("agencies")).toArray().size()), changed);
                if (changed) emit eventsChanged();
            }
        } else {
            qWarning("beaconfix: HaveIBeenFlocked sources: HTTP %d %s", http, qPrintable(r->errorString()));
        }
        QTimer::singleShot(11000, this, &PlateWatch::hibfTick);  // then the search (≥ 10 s apart)
    });
}

void PlateWatch::hibfSearch(const QString &cursor, int page)
{
    const QList<Hibf::Variant> vars = Hibf::variants(plates());
    if (vars.isEmpty()) { hibfFinish(0, QStringLiteral("no plates"), 0); return; }
    hibfCount();
    QNetworkRequest req{QUrl(QString::fromLatin1(kHibfSearch))};
    req.setRawHeader("User-Agent", kHibfUa);
    req.setRawHeader("Accept", "application/json");
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(60000);
    const QJsonObject body{{"plates", QJsonArray::fromStringList(Hibf::prefixes(vars))}, {"cursor", cursor.isEmpty() ? QJsonValue() : QJsonValue(cursor)}};
    QNetworkReply *r = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [this, r, vars, page] {
        r->deleteLater();
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray bytes = r->readAll();
        if (http == 429) {
            const QByteArray ra = r->rawHeader("Retry-After");
            int secs = ra.toInt();
            if (secs <= 0 && !ra.isEmpty()) { const QDateTime d = QDateTime::fromString(QString::fromLatin1(ra), Qt::RFC2822Date); if (d.isValid()) secs = int(QDateTime::currentDateTime().secsTo(d)); }
            hibfFinish(429, QStringLiteral("rate limited"), secs);
            return;
        }
        if (http != 200 && http != 404) { hibfFinish(http, r->error() != QNetworkReply::NoError ? r->errorString() : QStringLiteral("HTTP %1").arg(http), 0); return; }
        const QJsonObject o = QJsonDocument::fromJson(bytes).object();
        const QJsonArray results = o.value(QLatin1String("results")).toArray();
        m_hibfRows += int(results.size());
        bool any = false;
        for (const Hibf::Match &m : Hibf::filterResults(results, vars)) {
            QJsonObject ev = Hibf::searchEvent(m);
            bool created = false, changed = false;
            const QString uid = m_db->mergePlateEvent(ev, true, &created, &changed);
            any = any || changed;
            if (created && !uid.isEmpty()) { ++m_hibfNewHits; emit alert(m_db->plateEvent(uid, false, false)); }
        }
        if (any) emit eventsChanged();
        const QString nextCursor = o.value(QLatin1String("nextCursor")).isString() ? o.value(QLatin1String("nextCursor")).toString()
                                 : o.value(QLatin1String("nextCursor")).isDouble() ? QString::number(o.value(QLatin1String("nextCursor")).toDouble(), 'f', 0) : QString();
        if (http == 200 && o.value(QLatin1String("hasMore")).toBool() && !nextCursor.isEmpty() && page < 25) {
            QTimer::singleShot(int(Hibf::kMinGapSecs * 1000 + 500), this, [this, nextCursor, page] {
                QString why;
                if (!hibfMayRequest(&why)) { hibfFinish(200, QStringLiteral("stopped paging: %1").arg(why), 0); return; }
                hibfSearch(nextCursor, page + 1);
            });
            return;
        }
        hibfFinish(200, QString(), 0);
    });
}

void PlateWatch::hibfFinish(int status, const QString &error, int retryAfter)
{
    m_hibfBusy = false;
    QJsonObject st = hibfState();
    const QDateTime now = QDateTime::currentDateTime();
    st["lastStatus"] = status;
    st["lastError"] = error;
    st["lastAttempt"] = now.toString(Qt::ISODate);
    st["mode"] = hibfMode();
    if (status == 200) {
        st["lastCheck"] = now.toString(Qt::ISODate);
        st["failures"] = 0;
        st.remove(QStringLiteral("retryAt"));
        st["nextCheck"] = now.addSecs(Hibf::intervalSecs(modeOf(st["mode"].toString()))).toString(Qt::ISODate);
        st["hits"] = m_db->plateEventCounts().value(QLatin1String("plateSearch")).toInt();
        st["lastRows"] = m_hibfRows;
        st["lastNewHits"] = m_hibfNewHits;
        qInfo("beaconfix: HaveIBeenFlocked: %d result row(s) for our prefixes, %d new search(es) of our plates", m_hibfRows, m_hibfNewHits);
    } else {
        const int failures = st.value(QLatin1String("failures")).toInt() + 1;
        const qint64 wait = retryAfter > 0 ? retryAfter : Hibf::backoffSecs(failures);
        st["failures"] = failures;
        st["retryAt"] = now.addSecs(wait).toString(Qt::ISODate);
        st["nextCheck"] = st["retryAt"];
        qWarning("beaconfix: HaveIBeenFlocked check failed (%d %s), next try in %lld s", status, qPrintable(error), (long long)wait);
    }
    m_db->setKv(QStringLiteral("hibf_watch"), compact(st));
    emit eventsChanged();
}

// ── API helpers (§5) ──────────────────────────────────────────────────────────
QJsonObject PlateWatch::ingest(const QJsonArray &events, const QString &device, bool doAlert)
{
    QJsonArray uids, errors;
    int accepted = 0;
    bool any = false;
    QStringList cams;
    for (const QJsonValue &v : events) {
        QJsonObject ev = v.toObject();
        if (ev.value(QLatin1String("device")).toString().isEmpty()) ev["device"] = device;
        bool created = false, changed = false; QString err;
        const QString uid = m_db->mergePlateEvent(ev, false, &created, &changed, &err);
        if (uid.isEmpty()) { uids.append(QJsonValue()); errors.append(err); continue; }
        uids.append(uid); ++accepted; any = any || changed;
        const QJsonObject stored = m_db->plateEvent(uid, false, false);
        if (stored.value(QLatin1String("kind")).toString() == QLatin1String("camera_pass")) cams << stored.value(QLatin1String("camera_id")).toString();
        if (!created || !doAlert) continue;
        const QString src = stored.value(QLatin1String("source")).toString();
        if (stored.value(QLatin1String("kind")).toString() == QLatin1String("plate_search")
            || (stored.value(QLatin1String("camera_type")).toString() == QLatin1String("alpr") && (src == QLatin1String("phone_live") || src == QLatin1String("dashcam") || src == QLatin1String("live_route"))))
            emit alert(stored);
    }
    if (!cams.isEmpty()) {
        fetchCameraPhotos(cams);
        cams.removeDuplicates();
        for (const QString &id : std::as_const(cams)) if (resnapCamera(id) > 0) any = true;   // §2.6 / §2.7 on what the phone sent
        ensureRoads(cams);
    }
    if (any) emit eventsChanged();
    QJsonObject out{{"accepted", accepted}, {"uids", uids}};
    if (!errors.isEmpty()) out["errors"] = errors;
    return out;
}

void PlateWatch::addMedia(const QString &uidIn, const QJsonObject &body, std::function<void(int, const QJsonObject &)> done)
{
    const QString uid = m_db->resolvePlateUid(uidIn);
    const QJsonObject ev = m_db->plateEvent(uid, false, false);
    if (ev.isEmpty()) { done(404, QJsonObject{{"error", "no such plate event"}, {"uid", uidIn}}); return; }
    const QByteArray data = QByteArray::fromBase64(body.value(QLatin1String("data")).toString().toLatin1());
    if (data.isEmpty()) { done(400, QJsonObject{{"error", "data (base64 image) required"}}); return; }
    if (data.size() > kMaxUploadBytes) { done(413, QJsonObject{{"error", "image larger than 40 MB"}}); return; }
    const QString kind = body.value(QLatin1String("kind")).toString(QStringLiteral("dashcam"));
    if (kind != QLatin1String("dashcam") && kind != QLatin1String("webcam") && kind != QLatin1String("camera_photo")) { done(400, QJsonObject{{"error", "kind: dashcam | webcam | camera_photo"}}); return; }
    const bool camPhoto = kind == QLatin1String("camera_photo");
    QString captured = body.value(QLatin1String("capturedAt")).toString();
    if (!captured.isEmpty()) { const QDateTime t = QDateTime::fromString(captured, Qt::ISODateWithMs); if (t.isValid()) captured = t.toLocalTime().toString(Qt::ISODate); }
    QString attribution = body.value(QLatin1String("attribution")).toString();
    if (attribution.isEmpty() && kind == QLatin1String("dashcam")) attribution = QStringLiteral("Your dash cam (%1)").arg(ev.value(QLatin1String("device")).toString().isEmpty() ? QStringLiteral("this host") : ev.value(QLatin1String("device")).toString());
    const QJsonObject meta{{"kind", kind}, {"event_uid", camPhoto ? QString() : uid}, {"camera_id", camPhoto ? ev.value(QLatin1String("camera_id")).toString() : QString()},
                           {"attribution", attribution}, {"license", body.value(QLatin1String("license")).toString()},
                           {"original_url", body.value(QLatin1String("originalUrl")).toString()}, {"captured_at", captured}};
    storeEncoded(data, meta, [done, uid](bool ok, const QString &r) {
        if (ok) done(200, QJsonObject{{"uid", r}, {"event", uid}});
        else done(422, QJsonObject{{"error", r}});
    });
}

void PlateWatch::mediaFor(const QString &mediaUid, bool display, std::function<void(int, const QByteArray &, const QByteArray &)> done)
{
    MapDb::MediaRow meta;
    const QByteArray data = m_db->mediaData(mediaUid, &meta);
    if (data.isEmpty()) { done(404, "application/json", "{\"error\":\"no such media\"}"); return; }
    if (!display || meta.mime != QLatin1String("image/jxl")) { done(200, meta.mime.toLatin1(), data); return; }
    QPointer<PlateWatch> self(this);
    const QString mime = meta.mime; const bool recon = meta.jpegReconstructible;
    QThreadPool::globalInstance()->start([self, data, mime, recon, done] {
        QString outMime;
        const QByteArray out = ImageStore::displayBytes(data, mime, recon, &outMime);
        QMetaObject::invokeMethod(self.data(), [out, outMime, done] {
            if (out.isEmpty()) done(500, "application/json", "{\"error\":\"cannot decode the stored image (no JPEG XL decoder)\"}");
            else done(200, outMime.toLatin1(), out);
        }, Qt::QueuedConnection);
    });
}

QJsonObject PlateWatch::status() const
{
    QJsonObject bf = backfillState();
    bf["running"] = m_bfRunning;
    if (m_bfRunning) bf["chunksLeft"] = int(m_chunks.size());
    QJsonObject hb = hibfState();
    hb["enabled"] = m_full && !qEnvironmentVariableIsSet("BEACONFIX_NO_HIBF");
    hb["plates"] = int(plates().size());
    const QJsonObject src = parseObj(m_db->kv(QStringLiteral("hibf_sources")));
    hb["sources"] = QJsonObject{{"fetched", src.value(QLatin1String("fetched"))}, {"updatedAt", src.value(QLatin1String("updatedAt"))},
                                {"files", src.value(QLatin1String("files"))}, {"agencies", int(src.value(QLatin1String("agencies")).toArray().size())}};
    QJsonObject eof = eyesOnFlockState();
    eof.remove(QStringLiteral("summary"));
    return QJsonObject{{"backfill", bf}, {"hibf", hb}, {"hibfSources", src.value(QLatin1String("agencies")).toArray()}, {"counts", m_db->plateEventCounts()},
                       {"eyesOnFlock", eof}, {"routing", m_router ? m_router->status() : QJsonObject()},
                       {"roads", QJsonObject{{"queued", int(m_roadQueue.size())}, {"fresh", int(m_roadsFresh.size())}, {"busy", m_roadBusy}}},
                       {"active", m_full}, {"tools", QJsonObject{{"cjxl", ImageStore::haveCjxl()}, {"djxl", ImageStore::haveDjxl()},
                                                                   {"ffmpeg", !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()}}}};
}

// ── trust (§2.7) ──────────────────────────────────────────────────────────────
CameraTrust::Result PlateWatch::computeTrust(const FlockCamera &c)
{
    CameraTrust::Inputs in;
    in.id = c.id; in.source = c.source; in.model = c.model; in.osmTimestamp = c.osmTimestamp;
    const MapDb::CameraExtra x = m_db->cameraExtra(c.id);
    in.verdict = x.verdict; in.verdictAt = x.verdictAt;
    // RF evidence: our det: rows (their confidence is the detection tier's) and cameras field-confirmed by a detection
    // (recordFlockSighting vets a mapped camera instead of adding a det: row; only tier ≥ 2 detections do that)
    for (const FlockCamera &d : m_db->rfDetectionsNear(c.lat, c.lon, CameraTrust::kRfRadiusM)) {
        const int tier = d.id.startsWith(QLatin1String("det:")) ? CameraTrust::tierFromConfidence(d.confidence) : 2;
        const double dist = PlateEvents::distanceM(c.lat, c.lon, d.lat, d.lon);
        if (tier > in.rfTier || (tier == in.rfTier && dist < in.rfDistanceM)) {
            in.rfTier = tier; in.rfDistanceM = dist;
            in.rfWhat = d.id == c.id ? QStringLiteral("this camera, field-confirmed") : QStringLiteral("%1, %2 m away").arg(d.id).arg(qRound(dist));
        }
    }
    return CameraTrust::compute(in);
}

void PlateWatch::applyTrust(QList<PlateEvents::Camera> &cams)
{
    if (!m_db || !m_db->isOpen()) return;
    const QDateTime now = QDateTime::currentDateTime();
    for (PlateEvents::Camera &c : cams) {
        auto it = m_trustCache.find(c.id);
        if (it == m_trustCache.end() || it->at.secsTo(now) > 600) {
            TrustCache tc;
            tc.at = now;
            const MapDb::CameraExtra x = m_db->cameraExtra(c.id);
            const QDateTime computed = QDateTime::fromString(x.trustDetail.value(QLatin1String("computed")).toString(), Qt::ISODate);
            bool ok = false;
            const FlockCamera fc = m_db->flockCamera(c.id, &ok);
            // the stored trust holds while it is < 30 days old and its inputs (the OSM edit time, the verdict) are unchanged
            const bool current = x.hasTrust && computed.isValid() && computed.daysTo(now) < 30 && ok
                              && x.trustDetail.value(QLatin1String("osmTimestamp")).toString() == fc.osmTimestamp
                              && x.trustDetail.value(QLatin1String("verdict")).toString() == x.verdict;
            if (current) { tc.trust = x.trust; tc.detail = x.trustDetail; }
            else {
                if (ok) {
                    const CameraTrust::Result r = computeTrust(fc);
                    tc.trust = r.trust; tc.detail = r.detail;
                    if (!m_db->readOnly()) m_db->setCameraTrust(c.id, r.trust, r.detail);
                }
            }
            it = m_trustCache.insert(c.id, tc);
        }
        c.trust = it->trust;
        c.trustDetail = it->detail;
    }
}

QJsonObject PlateWatch::setCameraVerdict(const QString &cameraId, const QString &verdictIn)
{
    const QString verdict = verdictIn.trimmed().toLower();
    if (verdict != QLatin1String("present") && verdict != QLatin1String("absent") && verdict != QLatin1String("clear"))
        return QJsonObject{{"error", "verdict: present | absent | clear"}};
    if (!m_db || !m_db->isOpen() || m_db->readOnly()) return QJsonObject{{"error", "map database not writable"}};
    bool ok = false;
    m_db->flockCamera(cameraId, &ok);
    if (!ok) return QJsonObject{{"error", "no such camera"}, {"id", cameraId}, {"status", 404}};
    m_db->setCameraVerdict(cameraId, verdict == QLatin1String("clear") ? QString() : verdict);
    m_trustCache.remove(cameraId);
    const int n = resnapCamera(cameraId);                         // the trust is recomputed on the way
    emit eventsChanged();
    const MapDb::CameraExtra x = m_db->cameraExtra(cameraId);
    return QJsonObject{{"camera", cameraId}, {"verdict", x.verdict}, {"verdictAt", x.verdictAt}, {"trust", x.hasTrust ? QJsonValue(x.trust) : QJsonValue()},
                       {"trustDetail", x.trustDetail}, {"passesRescored", n}};
}

// ── roads (§2.6) ──────────────────────────────────────────────────────────────
PlateWatch::Roads PlateWatch::roadsFor(const PlateEvents::Camera &cam)
{
    Roads r;
    if (!m_db || !m_db->isOpen()) return r;
    const MapDb::CameraExtra x = m_db->cameraExtra(cam.id);
    const QDateTime f = QDateTime::fromString(x.waysFetched, Qt::ISODate);
    if (!f.isValid() || f.daysTo(QDateTime::currentDateTime()) >= RoadSnap::kTtlDays) { m_roadsFresh.remove(cam.id); return r; }
    r.fresh = true;
    m_roadsFresh.insert(cam.id);
    r.ways = m_db->waysNear(cam.lat, cam.lon, RoadSnap::kSampleRadiusM + 50.0);
    r.watched = RoadSnap::watchedWay(r.ways, cam.lat, cam.lon, PlateEvents::parseDirections(PlateEvents::directionText(cam)), cam.tags);
    const bool moved = std::fabs(x.watchedDetail.value(QLatin1String("cameraLat")).toDouble() - cam.lat) > 1e-6
                    || std::fabs(x.watchedDetail.value(QLatin1String("cameraLon")).toDouble() - cam.lon) > 1e-6;
    if ((r.watched.wayId != x.watchedWay || moved || x.watchedDetail.value(QLatin1String("basis")).toString() != r.watched.basis) && !m_db->readOnly()) {
        QJsonObject d = r.watched.toJson();
        d["cameraLat"] = cam.lat; d["cameraLon"] = cam.lon;
        m_db->setCameraRoads(cam.id, x.waysFetched, r.watched.wayId, d);
    }
    return r;
}

void PlateWatch::ensureRoads(const QStringList &ids)
{
    if (!m_full || !m_db || !m_db->isOpen()) return;
    const QDateTime now = QDateTime::currentDateTime();
    for (const QString &id : ids) {
        if (id.isEmpty() || m_roadsFresh.contains(id) || m_roadQueue.contains(id) || id == m_roadInFlight) continue;
        const auto t = m_roadTried.constFind(id);
        if (t != m_roadTried.constEnd() && t->secsTo(now) < 6 * 3600) continue;
        const QDateTime f = QDateTime::fromString(m_db->cameraExtra(id).waysFetched, Qt::ISODate);
        if (f.isValid() && f.daysTo(now) < RoadSnap::kTtlDays) { m_roadsFresh.insert(id); continue; }
        bool ok = false;
        const FlockCamera fc = m_db->flockCamera(id, &ok);
        if (!ok || fc.cameraType != QLatin1String("alpr")) continue;   // only plate readers are worth an Overpass request
        m_roadQueue.append(id);
    }
    if (!m_roadBusy) nextRoads();
}

// ≤ 1 Overpass request every 2 s; 429 / 504 cool down for a minute (and the camera is tried once more)
void PlateWatch::nextRoads()
{
    if (m_roadBusy || m_roadQueue.isEmpty()) return;
    const QDateTime now = QDateTime::currentDateTime();
    qint64 wait = m_lastOverpass.isValid() ? qMax<qint64>(0, 2000 - m_lastOverpass.msecsTo(now)) : 0;
    if (m_overpassCool.isValid() && now < m_overpassCool) wait = qMax(wait, now.msecsTo(m_overpassCool));
    m_roadBusy = true;
    QTimer::singleShot(int(wait), this, [this] {
        if (m_roadQueue.isEmpty()) { m_roadBusy = false; return; }
        const QString id = m_roadQueue.takeFirst();
        bool ok = false;
        const FlockCamera fc = m_db->flockCamera(id, &ok);
        if (!ok) { m_roadBusy = false; nextRoads(); return; }
        m_lastOverpass = QDateTime::currentDateTime();
        m_roadInFlight = id;
        QNetworkRequest req{QUrl(QString::fromLatin1(kOverpass[m_overpassMirror % 2]))};
        req.setRawHeader("User-Agent", kUa);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
        req.setTransferTimeout(40000);
        QNetworkReply *r = m_nam.post(req, "data=" + QUrl::toPercentEncoding(RoadSnap::overpassQuery(fc.lat, fc.lon)));
        connect(r, &QNetworkReply::finished, this, [this, r, id] {
            r->deleteLater();
            m_roadInFlight.clear();
            const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray bytes = r->readAll();
            const QByteArray blocked = r->rawHeader("blocked-by");
            QString err;
            QList<RoadSnap::Way> ways;
            if (!blocked.isEmpty()) err = QStringLiteral("Overpass is blocked by %1 (DNS filter)").arg(QString::fromLatin1(blocked));
            else if (http == 429 || http == 504) {
                m_overpassCool = QDateTime::currentDateTime().addSecs(60);
                if (m_roadRetries.value(id) < 1) { m_roadRetries[id] += 1; m_roadQueue.append(id); m_roadBusy = false; nextRoads(); return; }
                err = QStringLiteral("Overpass busy (HTTP %1)").arg(http);
            } else if (http != 200) { err = QStringLiteral("Overpass HTTP %1 %2").arg(http).arg(r->errorString()); ++m_overpassMirror; }
            else {
                const QString remark = QJsonDocument::fromJson(bytes).object().value(QLatin1String("remark")).toString();
                if (remark.contains(QLatin1String("error"), Qt::CaseInsensitive)) err = QStringLiteral("Overpass: %1").arg(remark.left(200));
                else ways = RoadSnap::parseOverpass(bytes, &err);
            }
            if (err.isEmpty()) {
                const QString now = nowIso();
                m_db->storeWays(ways, now);
                bool ok = false;
                const FlockCamera fc = m_db->flockCamera(id, &ok);
                if (ok) {
                    const PlateEvents::Camera cam = toCameras({fc}).first();
                    const RoadSnap::Watched w = RoadSnap::watchedWay(m_db->waysNear(cam.lat, cam.lon, RoadSnap::kSampleRadiusM + 50.0), cam.lat, cam.lon,
                                                                     PlateEvents::parseDirections(PlateEvents::directionText(cam)), cam.tags);
                    QJsonObject d = w.toJson(); d["cameraLat"] = cam.lat; d["cameraLon"] = cam.lon;
                    m_db->setCameraRoads(id, now, w.wayId, d);
                    m_roadsFresh.insert(id);
                    m_roadTried.remove(id);
                    const int n = resnapCamera(id);
                    qInfo("beaconfix: roads around %s: %d ways, watched way %lld (%s); %d pass(es) rescored", qPrintable(id), int(ways.size()),
                          (long long)w.wayId, qPrintable(w.basis), n);
                    if (n > 0) emit eventsChanged();
                }
            } else {
                qWarning("beaconfix: roads around %s: %s", qPrintable(id), qPrintable(err));
                m_roadTried[id] = QDateTime::currentDateTime();
            }
            m_roadBusy = false;
            nextRoads();
        });
    });
}

// A stored pass again: the snap (when the camera's roads are cached; otherwise the snap it has) and the trust
bool PlateWatch::rescoreStored(const QJsonObject &ev, const PlateEvents::Camera &cam, const Roads &roads)
{
    QJsonObject m = ev.value(QLatin1String("metrics")).toObject();
    double snapF = m.value(QLatin1String("snapFactor")).toDouble(1.0);
    QJsonObject snapJ = m.value(QLatin1String("snap")).toObject();
    const QDateTime t = QDateTime::fromString(ev.value(QLatin1String("time")).toString(), Qt::ISODate);
    if (roads.fresh && t.isValid()) {
        PlateEvents::Pass p;
        p.ms = t.toMSecsSinceEpoch();
        p.lat = ev.value(QLatin1String("lat")).toDouble(); p.lon = ev.value(QLatin1String("lon")).toDouble();
        p.acc = ev.value(QLatin1String("acc")).isDouble() ? ev.value(QLatin1String("acc")).toDouble() : -1.0;
        p.headingDeg = ev.value(QLatin1String("heading_deg")).isDouble() ? ev.value(QLatin1String("heading_deg")).toDouble() : PlateEvents::nan();
        const QList<PlateEvents::TrackFix> all = m_db->trackFixes(t.addSecs(-150).toString(Qt::ISODate), t.addSecs(150).toString(Qt::ISODate), QStringLiteral("*"));
        const QJsonArray devs = m.value(QLatin1String("fixDevices")).toArray();
        const QString dev = !devs.isEmpty() ? devs.first().toString() : ev.value(QLatin1String("device")).toString();
        QList<PlateEvents::TrackFix> stream;
        for (const PlateEvents::TrackFix &f : all) if (f.device == dev) stream << f;
        int closest = 0;
        const QList<RoadSnap::Sample> smp = RoadSnap::samplesFor(stream, p, cam.lat, cam.lon, &closest);
        const RoadSnap::Result res = RoadSnap::snap(smp, closest, roads.ways, roads.watched);
        snapJ = snapJson(res, roads.watched);
        snapF = res.status == QLatin1String("ok") ? res.factor : 1.0;
    }
    const int conf = PlateEvents::rescoreMetrics(m, ev.value(QLatin1String("camera_type")).toString(), snapF, snapJ, cam.trust >= 0 ? cam.trust : 1.0, cam.trustDetail);
    return m_db->updatePassScore(ev.value(QLatin1String("uid")).toString(), conf, m);
}

int PlateWatch::resnapCamera(const QString &cameraId)
{
    if (!m_db || !m_db->isOpen() || m_db->readOnly()) return 0;
    bool ok = false;
    const FlockCamera fc = m_db->flockCamera(cameraId, &ok);
    if (!ok) return 0;
    QList<PlateEvents::Camera> cl = toCameras({fc});
    applyTrust(cl);
    const Roads roads = roadsFor(cl.first());
    int n = 0;
    for (const QJsonValue &v : m_db->passesOfCamera(cameraId)) if (rescoreStored(v.toObject(), cl.first(), roads)) ++n;
    return n;
}

void PlateWatch::rescoreAll()
{
    if (!m_full || !m_db || !m_db->isOpen() || m_db->readOnly()) return;
    const QStringList ids = m_db->camerasWithPasses();
    int n = 0;
    for (const QString &id : ids) n += resnapCamera(id);
    if (n > 0) { qInfo("beaconfix: plate events: %d pass(es) rescored (camera trust, road snapping)", n); emit eventsChanged(); }
    ensureRoads(ids);
}

QJsonObject PlateWatch::cameraInfo(const QString &cameraId)
{
    bool ok = false;
    const FlockCamera fc = m_db ? m_db->flockCamera(cameraId, &ok) : FlockCamera();
    if (!ok) return {};
    QList<PlateEvents::Camera> cl = toCameras({fc});
    if (!m_db->readOnly()) applyTrust(cl);
    const MapDb::CameraExtra x = m_db->cameraExtra(cameraId);
    QJsonObject o = fc.toJson();
    o["cameraType"] = cl.first().type;
    o["trust"] = cl.first().trust >= 0 ? QJsonValue(cl.first().trust) : QJsonValue();
    o["trustDetail"] = cl.first().trustDetail;
    o["verdict"] = x.verdict;
    o["verdictAt"] = x.verdictAt;
    o["roads"] = QJsonObject{{"fetched", x.waysFetched}, {"watchedWay", double(x.watchedWay)}, {"watched", x.watchedDetail}};
    QJsonObject probe{{"kind", "camera_pass"}, {"camera_id", cameraId}, {"operator", fc.operatorName}};
    const QJsonObject f = agencyFacts(probe);
    if (!f.isEmpty()) o["agencyFacts"] = f;
    return o;
}

// ── Eyes on Flock (§4.6) ──────────────────────────────────────────────────────
QJsonObject PlateWatch::eyesOnFlockState() const { return parseObj(m_db ? m_db->kv(QStringLiteral("eyesonflock")) : QString()); }
void PlateWatch::eofTick() { refreshEyesOnFlock(false); }

void PlateWatch::refreshEyesOnFlock(bool force)
{
    if (!m_full || m_eofBusy || qEnvironmentVariableIsSet("BEACONFIX_NO_EOF")) return;
    QJsonObject st = eyesOnFlockState();
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime fetched = QDateTime::fromString(st.value(QLatin1String("fetched")).toString(), Qt::ISODate);
    const QDateTime tried = QDateTime::fromString(st.value(QLatin1String("lastAttempt")).toString(), Qt::ISODate);
    if (!force) {
        if (fetched.isValid() && fetched.daysTo(now) < 7) return;                       // weekly
        if (tried.isValid() && tried.secsTo(now) < 6 * 3600) return;                   // a failed fetch: again after 6 h
    }
    m_eofBusy = true;
    st["lastAttempt"] = nowIso();
    m_db->setKv(QStringLiteral("eyesonflock"), compact(st));
    QNetworkRequest req{QUrl(QString::fromLatin1(EyesOnFlock::kUrl))};
    req.setRawHeader("User-Agent", kUa);
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(180000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *r = m_nam.get(req);
    connect(r, &QNetworkReply::downloadProgress, r, [r](qint64 got, qint64) { if (got > 64LL * 1024 * 1024) r->abort(); });   // ~16.5 MB today
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        eofDone(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), r->readAll(), QString::fromLatin1(r->rawHeader("blocked-by")),
                r->error() != QNetworkReply::NoError ? r->errorString() : QString());
    });
}

void PlateWatch::eofDone(int http, const QByteArray &body, const QString &blockedBy, const QString &netError)
{
    auto fail = [this](const QString &status, const QString &error) {
        m_eofBusy = false;
        QJsonObject st = eyesOnFlockState();
        st["status"] = status; st["error"] = error;
        m_db->setKv(QStringLiteral("eyesonflock"), compact(st));
        qWarning("beaconfix: Eyes on Flock: %s — agency facts stay as they were", qPrintable(error));
    };
    if (!blockedBy.isEmpty()) { fail(QStringLiteral("blocked"), QStringLiteral("eyesonflock.com is blocked by %1 (DNS filter)").arg(blockedBy)); return; }
    if (http != 200) { fail(QStringLiteral("error"), QStringLiteral("HTTP %1 %2").arg(http).arg(netError)); return; }
    if (body.trimmed().isEmpty()) { fail(QStringLiteral("error"), QStringLiteral("empty answer")); return; }
    QPointer<PlateWatch> self(this);
    const qint64 bytes = body.size();
    QThreadPool::globalInstance()->start([self, body, bytes] {
        const EyesOnFlock::Parsed parsed = EyesOnFlock::parse(body);
        QMetaObject::invokeMethod(self.data(), [self, parsed, bytes] {
            if (!self) return;
            if (!parsed.error.isEmpty() || parsed.portals.isEmpty()) {
                self->m_eofBusy = false;
                QJsonObject st = self->eyesOnFlockState();
                st["status"] = "error"; st["error"] = parsed.error.isEmpty() ? QStringLiteral("no portals in the answer") : parsed.error;
                self->m_db->setKv(QStringLiteral("eyesonflock"), compact(st));
                return;
            }
            const QString now = nowIso();
            const int n = self->m_db->storePortals(parsed.portals, now);
            self->m_portals = parsed.portals;
            self->m_portalsLoaded = true;
            self->m_agencyCache.clear();
            // the cameras with passes: their portal again (the join is by name tokens; the list changes weekly)
            int matched = 0;
            for (const QString &id : self->m_db->camerasWithPasses()) {
                bool ok = false;
                const FlockCamera fc = self->m_db->flockCamera(id, &ok);
                if (!ok) continue;
                self->m_db->setCameraAgency(id, QString());
                if (!self->agencyFacts(QJsonObject{{"kind", "camera_pass"}, {"camera_id", id}, {"operator", fc.operatorName}}).isEmpty()) ++matched;
            }
            QJsonObject st = self->eyesOnFlockState();
            st["status"] = "ok"; st["error"] = QString(); st["fetched"] = now; st["portals"] = n; st["bytes"] = double(bytes);
            st["summary"] = parsed.summary; st["camerasMatched"] = matched;
            st["license"] = QLatin1String(EyesOnFlock::kLicense); st["attribution"] = QLatin1String(EyesOnFlock::kAttribution);
            self->m_db->setKv(QStringLiteral("eyesonflock"), compact(st));
            self->m_eofBusy = false;
            qInfo("beaconfix: Eyes on Flock: %d transparency portals (%lld bytes); %d passed camera(s) matched to an agency", n, (long long)bytes, matched);
            emit self->eventsChanged();
        }, Qt::QueuedConnection);
    });
}

void PlateWatch::loadPortals()
{
    if (m_portalsLoaded || !m_db || !m_db->isOpen()) return;
    m_portals = m_db->loadPortals();
    m_portalsLoaded = true;
}

QString PlateWatch::stateHint()
{
    const QDateTime now = QDateTime::currentDateTime();
    if (!m_stateHintAt.isValid() || m_stateHintAt.secsTo(now) > 3600) {
        m_stateHint = EyesOnFlock::stateCode(m_db->latestRegion());
        m_stateHintAt = now;
    }
    return m_stateHint;
}

QString PlateWatch::agencySlug(const QString &name, const QString &state, bool *stateVerified)
{
    const QString key = name.simplified().toLower() + QLatin1Char('|') + state;
    const auto c = m_agencyCache.constFind(key);
    if (c != m_agencyCache.constEnd()) { if (stateVerified) *stateVerified = c->second; return c->first; }
    bool verified = false;
    int i = EyesOnFlock::match(name, state, m_portals, &verified);
    if (i < 0 && state.isEmpty()) {                               // ambiguous without a state: the state we were last in, unverified
        const QString hint = stateHint();
        if (!hint.isEmpty()) { i = EyesOnFlock::match(name, hint, m_portals, &verified); verified = false; }
    }
    const QString slug = i >= 0 ? m_portals[i].slug : QString();
    m_agencyCache.insert(key, {slug, verified});
    if (stateVerified) *stateVerified = verified;
    return slug;
}

QJsonObject PlateWatch::agencyFacts(const QJsonObject &ev)
{
    if (!m_db || !m_db->isOpen()) return {};
    loadPortals();
    if (m_portals.isEmpty()) return {};
    QString slug;
    bool verified = true;
    if (ev.value(QLatin1String("kind")).toString() == QLatin1String("camera_pass")) {
        const QString camId = ev.value(QLatin1String("camera_id")).toString();
        const MapDb::CameraExtra x = m_db->cameraExtra(camId);
        if (!x.agencyPortal.isEmpty()) { slug = x.agencyPortal; verified = !slug.startsWith(QLatin1Char('~')); if (!verified) slug = slug.mid(1); }
        else {
            bool ok = false;
            const FlockCamera fc = m_db->flockCamera(camId, &ok);
            const QString op = ok && !fc.operatorName.isEmpty() ? fc.operatorName : ev.value(QLatin1String("operator")).toString();
            const QString st = ok ? cameraState(toCameras({fc}).first()) : QString();
            if (op.isEmpty()) return {};
            slug = agencySlug(op, st, &verified);
            if (!slug.isEmpty() && ok && !m_db->readOnly()) m_db->setCameraAgency(camId, verified ? slug : QStringLiteral("~") + slug);
        }
    } else {
        const QJsonObject m = ev.value(QLatin1String("metrics")).toObject();
        const QString name = ev.value(QLatin1String("agency")).toString();
        if (name.isEmpty()) return {};
        QString st = m.value(QLatin1String("org_state")).toString().trimmed();
        if (st.size() != 2) st = EyesOnFlock::stateCode(st);
        slug = agencySlug(name, st.toUpper(), &verified);
    }
    if (slug.isEmpty()) return {};
    for (const EyesOnFlock::Portal &p : std::as_const(m_portals)) if (p.slug == slug) return EyesOnFlock::facts(p, verified);
    return {};
}
