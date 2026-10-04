#include "beaconview.h"
#include "plateevents.h"
#include "locator.h"
#include "tilesource.h"
#include "anchordialog.h"
#include "ranging/rangingservice.h"
#include "fitjson.h"
#include "platewatch.h"
#include "routeplanner.h"
#include "inspectdialog.h"
#include <QDialogButtonBox>
#include <QPushButton>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QFontMetrics>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QUrl>
#include <QWheelEvent>
#include <QWindow>
#include <QtMath>
#include <algorithm>

static const int TILE = 256;
static const double EARTH_M = 40075016.686;
static const QColor C_BG(0x0b, 0x10, 0x1a);
static const QColor C_PANEL(8, 12, 20, 200);
static const QColor C_ME(0x35, 0xd6, 0xff);        // cyan
static const QColor C_USED(0x35, 0xd6, 0xff);
static const QColor C_LOCATED(0xff, 0xd1, 0x66);   // gold: real position known
static const QColor C_TRAVEL(0xff, 0x4f, 0xd8);    // magenta
static const QColor C_ACTIVE(0x6c, 0xff, 0x8a);    // green
static const QColor C_IGNORED(0x8a, 0x93, 0xa6);   // grey
static const QColor C_HOME(0xff, 0x9f, 0x43);      // orange: our own network (the RV)
static const QColor C_TEXT(0xe6, 0xed, 0xf7);
static const QColor C_DIM(0x9f, 0xb0, 0xc8);

enum { BtnZoomIn, BtnZoomOut, BtnLocate, BtnLayers, BtnPlaces, BtnCount };

// ── Grades (docs/GRADING.md): the shared Okabe–Ito palette ─────────────────────
QColor BeaconView::gradeColor(const QString &g)
{
    if (g == QLatin1String("A")) return QColor(0x00, 0x9E, 0x73);
    if (g == QLatin1String("B")) return QColor(0x56, 0xB4, 0xE9);
    if (g == QLatin1String("C")) return QColor(0xF0, 0xE4, 0x42);
    if (g == QLatin1String("D")) return QColor(0xE6, 0x9F, 0x00);
    if (g == QLatin1String("E")) return QColor(0xD5, 0x5E, 0x00);
    if (g == QLatin1String("F")) return QColor(0xCC, 0x79, 0xA7);
    if (g == QLatin1String("M")) return QColor(0x00, 0x72, 0xB2);
    return QColor(0x8A, 0x93, 0xA6);                     // R, and anything ungraded
}

QColor BeaconView::gradeTextColor(const QString &g)
{
    const QColor c = gradeColor(g);
    return (0.299 * c.red() + 0.587 * c.green() + 0.114 * c.blue()) / 255.0 > 0.55 ? QColor(0x10, 0x14, 0x1c) : QColor(Qt::white);
}

static bool gradedFix(const Estimator::Fit &f) { return f.kind == QLatin1String("fix") && f.grade.size() == 1 && f.grade[0] >= QLatin1Char('A') && f.grade[0] <= QLatin1Char('F'); }

static QString plural(int n, const char *one, const char *many) { return QStringLiteral("%1 %2").arg(n).arg(QString::fromLatin1(n == 1 ? one : many)); }

// The lead line of a beacon card / list row: "B · 72% within 25 m · 9 places · 3 devices"
static QString gradeLead(const ApEstimate &e)
{
    const Estimator::Fit &f = e.fit;
    if (e.kind == ApEstimate::Mobile) return QStringLiteral("M · travels with you");
    if (e.kind == ApEstimate::Region)
        return QStringLiteral("R · region ±%1 m · %2").arg(qRound(f.r95)).arg(plural(f.vantage, "place", "places"));
    if (e.kind == ApEstimate::Trilat && gradedFix(f)) {
        QString s = QStringLiteral("%1 · %2% within 25 m · %3").arg(f.grade).arg(qRound(f.pWithin25 * 100)).arg(plural(f.vantage, "place", "places"));
        if (f.devices > 0) s += QStringLiteral(" · ") + plural(f.devices, "device", "devices");
        return s;
    }
    return QString();
}

BeaconView::BeaconView(Locator *loc, TileSource *tiles, QWidget *parent) : QWidget(parent), m_loc(loc), m_src(tiles)
{
    m_tiles.setMaxCost(512);                                // 256 × 256 × 4 B each: ~128 MB at most
    setMinimumSize(360, 260);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);

    if (RangingService *rs = m_loc->ranging()) {            // measured distances to our devices (docs/RANGING.md)
        connect(rs, &RangingService::estimateChanged, this, [this, rs](const QString &device) {
            const QJsonObject e = rs->estimateJson(device);
            if (e.isEmpty()) m_ranges.remove(device); else m_ranges.insert(device, e);
            update();
        });
    }
    QSettings s;
    m_layer = Layer(qBound(0, s.value("map/layer", int(Satellite)).toInt(), int(Topo)));   // the hybrid by default: APs read against the real world
    m_satSource = qBound(0, s.value("map/satSource", 0).toInt(), int(TileSource::SatNasaViirs));
    m_showContours = s.value("map/showContours", true).toBool();
    const QStringList hidden = s.value("map/hiddenCategories", QStringList{QStringLiteral("toilets")}).toStringList();
    m_hiddenCats = QSet<QString>(hidden.begin(), hidden.end());
    m_showNames = s.value("map/showNames", true).toBool();
    m_showDevices = s.value("map/showDevices", true).toBool();
    m_showImported = s.value("map/showImported", true).toBool();
    m_showLegend = s.value("map/showLegend", false).toBool();
    m_showHeatmap = s.value("map/showHeatmap", true).toBool();
    m_showFlockCameras = s.value("map/showFlockCameras", true).toBool();
    m_showApCircles = s.value("map/showApCircles", true).toBool();
    m_camKick.setSingleShot(true);
    m_camKick.setInterval(150);
    connect(&m_camKick, &QTimer::timeout, this, &BeaconView::loadFlockCameras);
    connect(m_loc, &Locator::flockCamerasUpdated, this, [this] { m_camStale = true; update(); });   // the next paint reloads what's in view
    // The heat map's source: reloaded at most every 30 s for live fixes, at once for an import
    connect(m_loc, &Locator::probeFinished, this, [this](bool ok) {
        if (ok) { m_routeFixesDirty = true; ++m_trackVersion; update(); }
    });
    connect(m_loc, &Locator::importProgress, this, [this] {
        m_routeFixesDirty = true; m_routeLoadedAt.invalidate(); ++m_trackVersion;
        update();
    });
    connect(m_loc, &Locator::FixChanged, this, [this] {
        m_routeFixesDirty = true; ++m_trackVersion;
        update();
    });
    m_pool.setMaxThreadCount(1);
    m_heatKick.setSingleShot(true);
    m_heatKick.setInterval(60);                                // coalesce a burst of view changes into one render
    connect(&m_heatKick, &QTimer::timeout, this, &BeaconView::startHeat);

    // 30 fps while something moves. The pulse and the radar sweep only touch the area around us, so
    // that's all a plain tick repaints; zooming, event animations and (once a second) the ticker's
    // ages and fades get the whole widget. Nothing at all while the window isn't on screen.
    m_anim.setInterval(33);
    connect(&m_anim, &QTimer::timeout, this, [this] {
        const QWindow *w = window()->windowHandle();
        if (!isVisible() || (w && !w->isExposed())) return;
        m_phase = std::fmod(m_phase + 0.010, 1.0);
        m_sweep = std::fmod(m_sweep + 1.4, 360.0);
        bool full = !m_anims.isEmpty() || !m_labelBorn.isEmpty();
        if (std::abs(m_zoomTarget - m_zoom) > 0.002) {
            const double z = std::abs(m_zoomTarget - m_zoom) < 0.01 ? m_zoomTarget : m_zoom + (m_zoomTarget - m_zoom) * 0.3;
            applyZoom(z, m_zoomAnchor);
            full = true;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (full || now - m_fullAt >= 1000) { m_fullAt = now; update(); return; }
        const Fix &fix = m_loc->fix();
        if (!fix.valid) return;
        const QPointF me = toScreen(fix.lat, fix.lon);
        const double acc = fix.accuracy > 0 ? qBound(8.0, fix.accuracy / metersPerPixel(fix.lat), 20000.0) : 0;
        const double r = qMax(40.0, acc < 3000 ? acc + 3 : 0.0);   // pulse ≤ 36 px; the sweep fills the ring
        update(QRectF(me.x() - r, me.y() - r, 2 * r, 2 * r).toAlignedRect() & rect());
    });
    connect(m_loc, &Locator::FixChanged, this, [this] { if (m_follow) recenter(); update(); });
    connect(m_loc, &Locator::scanUpdated, this, [this] { m_apEstAt.invalidate(); rebuildLabelCache(); if (m_follow && m_autoZoom) fitBeacons(); update(); });
    connect(m_loc, &Locator::eventLogged, this, &BeaconView::onEvent);
    for (const BeaconEvent &e : m_loc->events())               // what happened before this view opened
        onEvent(QString::fromUtf8(QJsonDocument(e.toJson()).toJson(QJsonDocument::Compact)));
    rebuildLabelCache();
    connect(m_loc, &Locator::poisUpdated, this, [this] { m_selKind = HitNone; update(); });
    recenter();
}

// ── Geometry ─────────────────────────────────────────────────────────────────
QPointF BeaconView::merc(double lat, double lon)
{
    const double r = qDegreesToRadians(qBound(-85.05112878, lat, 85.05112878));
    return QPointF((lon + 180.0) / 360.0, (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / M_PI) / 2.0);
}
void BeaconView::unmerc(const QPointF &m, double *lat, double *lon)
{
    *lon = m.x() * 360.0 - 180.0;
    *lat = qRadiansToDegrees(std::atan(std::sinh(M_PI * (1 - 2 * m.y()))));
}
double  BeaconView::worldPx() const { return TILE * std::pow(2.0, m_zoom); }
QPointF BeaconView::toScreen(const QPointF &m) const
{
    const double ws = worldPx();
    double dx = m.x() - m_center.x();
    if (dx > 0.5) dx -= 1.0; else if (dx < -0.5) dx += 1.0;   // antimeridian
    return QPointF(width() / 2.0 + dx * ws, height() / 2.0 + (m.y() - m_center.y()) * ws);
}
QPointF BeaconView::toMerc(const QPointF &s) const
{
    const double ws = worldPx();
    return QPointF(m_center.x() + (s.x() - width() / 2.0) / ws, m_center.y() + (s.y() - height() / 2.0) / ws);
}
double BeaconView::metersPerPixelAt(double lat, double zoom) { return EARTH_M * std::cos(qDegreesToRadians(lat)) / (TILE * std::pow(2.0, zoom)); }
double BeaconView::metersPerPixel(double lat) const { return metersPerPixelAt(lat, m_zoom); }

QString BeaconView::compass(double deg)
{
    static const char *pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return QString::fromLatin1(pts[int(std::fmod(deg + 22.5 + 360.0, 360.0) / 45.0) % 8]);
}
QString BeaconView::distText(double m)
{
    if (m < 3) return QStringLiteral("%1 m").arg(m, 0, 'f', 2);          // measured ranges: centimetres matter
    if (m < 20) return QStringLiteral("%1 m").arg(m, 0, 'f', 1);
    if (m < 100) return QStringLiteral("%1 m").arg(qRound(m));
    if (m < 950) return QStringLiteral("%1 m").arg(qRound(m / 10.0) * 10);
    if (m < 9950) return QStringLiteral("%1 km").arg(m / 1000.0, 0, 'f', 1);
    return QStringLiteral("%1 km").arg(qRound(m / 1000.0));
}

// ── View control ─────────────────────────────────────────────────────────────
void BeaconView::recenter()
{
    const Fix &f = m_loc->fix();
    if (f.valid) m_center = merc(f.lat, f.lon);
    m_follow = true; m_autoZoom = true;
    fitBeacons();
    update();
}

// Zoom so the accuracy ring and every nearby beacon estimate fill ~40 % of the view
void BeaconView::fitBeacons()
{
    const Fix &f = m_loc->fix();
    if (!f.valid || width() < 50) return;
    // Fit the beacons, not the fix's error: an IP fix is tens of km wide. Cap at ~2.5 km.
    double maxM = qMax(80.0, f.source == QLatin1String("ip") ? 80.0 : qMin(f.accuracy, 2500.0));
    for (const AccessPoint &ap : m_loc->accessPoints()) {
        const ApEstimate e = m_loc->estimateFor(ap);
        if (e.kind == ApEstimate::Ring) maxM = qMax(maxM, e.radiusM);
        else if (e.kind != ApEstimate::None && m_loc->apStatus(ap) == QLatin1String("used")) {
            const double d = Locator::distanceM(f.lat, f.lon, e.lat, e.lon) + e.radiusM;
            if (d < 3000) maxM = qMax(maxM, d);
        }
    }
    const double target = qMin(width(), height()) * 0.42;
    double z = std::log2(target * EARTH_M * std::cos(qDegreesToRadians(f.lat)) / (TILE * maxM));
    z = qBound(13.0, std::floor(z * 2) / 2, 18.0);
    m_zoom = m_zoomTarget = qMin(z, double(maxZoom()));
}

void BeaconView::focusOn(double lat, double lon, double zoom)
{
    m_center = merc(lat, lon);
    m_follow = false; m_autoZoom = false;
    m_zoom = m_zoomTarget = qBound(3.0, zoom, double(MAX_VIEW_ZOOM));
    update();
}

void BeaconView::selectPoi(int index)
{
    if (index < 0 || index >= m_loc->pois().size()) return;
    const Poi &pt = m_loc->pois()[index];
    setCategoryVisible(pt.cat, true);
    focusOn(pt.lat, pt.lon, qMax(m_zoom, 16.0));
    m_selKind = HitPoi; m_selItem = index;
    update();
}

void BeaconView::setZoom(int z) { m_autoZoom = false; m_zoom = m_zoomTarget = qBound(3, z, MAX_VIEW_ZOOM); update(); }

void BeaconView::applyZoom(double z, const QPointF &anchor)
{
    const QPointF m = toMerc(anchor);
    m_zoom = z;
    const double ws = worldPx();
    m_center = QPointF(m.x() - (anchor.x() - width() / 2.0) / ws, m.y() - (anchor.y() - height() / 2.0) / ws);
    m_center.setY(qBound(0.0, m_center.y(), 1.0));
    m_center.setX(m_center.x() - std::floor(m_center.x()));
}

void BeaconView::zoomAt(double delta, const QPointF &anchor, bool animate)
{
    m_autoZoom = false;
    if (anchor != QPointF(width() / 2.0, height() / 2.0)) m_follow = false;
    m_zoomTarget = qBound(3.0, m_zoomTarget + delta, double(MAX_VIEW_ZOOM));
    m_zoomAnchor = anchor;
    if (!animate || !m_anim.isActive()) applyZoom(m_zoomTarget, anchor);
    update();
}

void BeaconView::setLayer(Layer l)
{
    m_layer = l;
    QSettings().setValue("map/layer", int(l));
    if (m_zoomTarget > maxZoom() + 2) { m_zoom = m_zoomTarget = maxZoom() + 2; }
    update();
}

void BeaconView::setSatSource(int src)
{
    m_satSource = qBound(0, src, int(TileSource::SatNasaViirs));
    QSettings().setValue("map/satSource", m_satSource);
    ++m_tileVersion;                                            // a new source: the cached base image is stale
    setLayer(Satellite);
}

void BeaconView::setShowContours(bool on)
{
    m_showContours = on;
    QSettings().setValue("map/showContours", on);
    ++m_tileVersion;
    update();
}

void BeaconView::setCategoryVisible(const QString &key, bool visible)
{
    if (visible) m_hiddenCats.remove(key); else m_hiddenCats.insert(key);
    QSettings().setValue("map/hiddenCategories", QStringList(m_hiddenCats.begin(), m_hiddenCats.end()));
    update();
}

void BeaconView::setShowNames(bool on)
{
    m_showNames = on;
    QSettings().setValue("map/showNames", on);
    update();
}

// Label text, width and colour per beacon are computed when the scan changes,
// not every frame: painting only places the cached rectangles.
void BeaconView::rebuildLabelCache()
{
    const auto &aps = m_loc->accessPoints();
    QFont f = font(); f.setPointSizeF(f.pointSizeF() * 0.85);
    const QFontMetricsF fm(f);
    const double maxW = fm.averageCharWidth() * 18;
    m_labels = QList<LabelInfo>(aps.size());
    m_apIndex.clear();
    for (int i = 0; i < aps.size(); ++i) {
        const AccessPoint &ap = aps[i];
        m_apIndex.insert(ap.bssid, i);
        LabelInfo &l = m_labels[i];
        l.hidden = ap.ssid.isEmpty();
        l.text = l.hidden ? QStringLiteral("(hidden)") : fm.elidedText(ap.ssid, Qt::ElideRight, int(maxW));
        l.width = fm.horizontalAdvance(l.text);
        l.band = ap.frequency >= 5925 ? QStringLiteral("6") : ap.frequency >= 4900 ? QStringLiteral("5") : QStringLiteral("2.4");
        const QString st = m_loc->apStatus(ap);
        const ApEstimate e = m_loc->estimateFor(ap);
        l.col = st == QLatin1String("home") ? C_HOME : st == QLatin1String("active") ? C_ACTIVE : st == QLatin1String("travelling") ? C_TRAVEL
              : st == QLatin1String("used") ? (e.kind != ApEstimate::Ring ? C_LOCATED : C_USED) : C_IGNORED;
    }
}

bool BeaconView::beaconScreenPos(const QString &bssid, QPointF *out) const
{
    const auto it = m_apIndex.constFind(bssid);
    if (it == m_apIndex.constEnd() || it.value() >= m_beaconPos.size() || m_beaconPos[it.value()].isNull()) return false;
    *out = m_beaconPos[it.value()];
    return true;
}

// ── Tiles ────────────────────────────────────────────────────────────────────
int BeaconView::maxZoom() const { return TileSource::maxZoom(srcLayer(m_layer)); }

TileSource::Layer BeaconView::srcLayer(Layer l) const
{
    return l == Satellite ? TileSource::satLayer(TileSource::SatSource(m_satSource)) : TileSource::Layer(l);
}

QString BeaconView::tileKey(Layer l, int z, int x, int y, int ov) const
{
    return ov ? QStringLiteral("O%1/%2/%3/%4").arg(ov).arg(z).arg(x).arg(y)
              : QStringLiteral("%1/%2/%3/%4").arg(int(srcLayer(l))).arg(z).arg(x).arg(y);
}

void BeaconView::ensureTile(Layer l, int z, int x, int y, int ov)
{
    const QString k = tileKey(l, z, x, y, ov);
    m_wanted.insert(k);
    if (m_tiles.contains(k) || m_pending.contains(k)) return;
    const auto failed = m_failed.constFind(k);
    if (failed != m_failed.constEnd() && failed->secsTo(QDateTime::currentDateTime()) < 60) return;
    if (m_pending.size() > 24) return;                  // the next render will ask again
    m_pending.insert(k, {l, z, x, y, ov});
    m_src->get(ov ? TileSource::Layer(ov) : srcLayer(l), z, x, y, this, [this, k](const QImage &img) {
        m_pending.remove(k);
        if (img.isNull()) {
            if (m_failed.size() > 4000) m_failed.clear();      // offline for a long drive: don't grow without bound
            m_failed.insert(k, QDateTime::currentDateTime());
        } else { m_tiles.insert(k, new QPixmap(QPixmap::fromImage(img))); m_failed.remove(k); }
        ++m_tileVersion;
        update();
    });
}

void BeaconView::drawTiles(QPainter &p, int ov)
{
    // overlays have their own depth: labels and roads to 19, contours (drawn locally) to 22; deeper zooms scale them
    const int maxZ = ov ? TileSource::maxZoom(TileSource::Layer(ov)) : maxZoom();
    const int tz = qBound(2, int(std::floor(m_zoom + 0.5)), maxZ);
    const int n = 1 << tz;
    const double ts = TILE * std::pow(2.0, m_zoom - tz);      // on-screen tile size
    const QPointF tl = toMerc(QPointF(0, 0)), br = toMerc(QPointF(width(), height()));
    struct T { int x, y, wx; double d; };
    QList<T> want;
    const double cx = m_center.x() * n, cy = m_center.y() * n;
    for (int x = int(std::floor(tl.x() * n)); x <= int(std::floor(br.x() * n)); ++x)
        for (int y = qMax(0, int(std::floor(tl.y() * n))); y <= qMin(n - 1, int(std::floor(br.y() * n))); ++y)
            want.append({x, y, ((x % n) + n) % n, std::hypot(x + 0.5 - cx, y + 0.5 - cy)});
    std::sort(want.begin(), want.end(), [](const T &a, const T &b) { return a.d < b.d; });   // centre first

    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const bool topoStandIn = !ov && m_layer != Topo && tz <= TileSource::maxZoom(TileSource::Topo);
    int budget = 6;                                            // new downloads started per render
    for (const T &t : want) {
        const QPointF o = toScreen(QPointF(double(t.x) / n, double(t.y) / n));
        // snap to whole pixels so neighbouring tiles never leave hairline seams
        const QRectF dst(QPointF(std::floor(o.x()), std::floor(o.y())), QPointF(std::ceil(o.x() + ts), std::ceil(o.y() + ts)));
        const QString k = tileKey(m_layer, tz, t.wx, t.y, ov);
        m_wanted.insert(k);
        if (QPixmap *pm = m_tiles.object(k)) { p.drawPixmap(dst, *pm, pm->rect()); continue; }
        m_baseMissing = true;
        if (budget > 0 && !m_pending.contains(k)) { ensureTile(m_layer, tz, t.wx, t.y, ov); --budget; }
        // Meanwhile, stretch the nearest cached ancestor, then any cached children over it
        // (zooming out), so there's never a black hole
        for (int up = 1; up <= 6 && tz - up >= 0; ++up) {
            const QString ak = tileKey(m_layer, tz - up, t.wx >> up, t.y >> up, ov);
            if (QPixmap *pm = m_tiles.object(ak)) {
                const double sub = double(pm->width()) / (1 << up);
                const QRectF src((t.wx - ((t.wx >> up) << up)) * sub, (t.y - ((t.y >> up) << up)) * sub, sub, sub);
                p.drawPixmap(dst, *pm, src);
                break;
            }
        }
        if (tz < maxZ)
            for (int c = 0; c < 4; ++c)
                if (QPixmap *pm = m_tiles.object(tileKey(m_layer, tz + 1, 2 * t.wx + (c & 1), 2 * t.y + (c >> 1), ov))) {
                    const QPointF half(dst.width() / 2, dst.height() / 2);
                    p.drawPixmap(QRectF(dst.topLeft() + QPointF((c & 1) * half.x(), (c >> 1) * half.y()), QSizeF(half.x(), half.y())), *pm, pm->rect());
                }
        // Offline (this layer failed to load): the OpenTopoMap tile TileSource::prefetch() saved stands in
        if (topoStandIn && m_failed.contains(k)) {
            if (QPixmap *pm = m_tiles.object(tileKey(Topo, tz, t.wx, t.y, 0))) { p.drawPixmap(dst, *pm, pm->rect()); m_baseTopo = true; }
            else ensureTile(Topo, tz, t.wx, t.y, 0);
        }
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
}

// ── Painting ─────────────────────────────────────────────────────────────────
static void glowDot(QPainter &p, const QPointF &c, double r, const QColor &col, double glow = 3.0)
{
    QRadialGradient g(c, r * glow);
    QColor t = col; t.setAlpha(110); g.setColorAt(0, t);
    t.setAlpha(0); g.setColorAt(1, t);
    p.setPen(Qt::NoPen); p.setBrush(g); p.drawEllipse(c, r * glow, r * glow);
    p.setBrush(col); p.setPen(QPen(QColor(255, 255, 255, 200), 1.2)); p.drawEllipse(c, r, r);
}

static void badge(QPainter &p, const QPointF &at, const QString &text, const QColor &bg)
{
    QFont f = p.font(); f.setBold(true); f.setPointSizeF(f.pointSizeF() * 0.75); p.setFont(f);
    const double w = qMax(16.0, p.fontMetrics().horizontalAdvance(text) + 8.0), h = p.fontMetrics().height() + 1;
    const QRectF r(at.x() - w / 2, at.y() - h / 2, w, h);
    p.setPen(QPen(QColor(8, 12, 20), 1.5)); p.setBrush(bg); p.drawRoundedRect(r, h / 2, h / 2);
    p.setPen(QColor(8, 12, 20)); p.drawText(r, Qt::AlignCenter, text);
    f.setBold(false); f.setPointSizeF(f.pointSizeF() / 0.75); p.setFont(f);
}

// BEACONFIX_PAINT_PROFILE=1: average time per paint layer (and the heat map worker's), to stderr every 2 s
namespace {
struct PaintProfile {
    static constexpr int N = 16;
    const bool on = qEnvironmentVariableIsSet("BEACONFIX_PAINT_PROFILE");
    const char *names[N] = {}; double ms[N] = {}; int frames = 0, n = 0, partial = 0;
    QElapsedTimer t, window;
    void begin() { if (on) { t.start(); if (!window.isValid()) window.start(); } }
    void lap(const char *name) {
        if (!on) return;
        int i = 0;
        while (i < n && names[i] != name) ++i;
        if (i == N) return;
        if (i == n) names[n++] = name;
        ms[i] += t.nsecsElapsed() / 1e6; t.start();
    }
    void end(bool whole, const QSize &sz, const QString &note) {
        if (!on) return;
        ++frames; if (!whole) ++partial;
        if (window.elapsed() < 2000) return;
        double total = 0; QString s;
        for (int i = 0; i < n; ++i) { total += ms[i]; s += QStringLiteral(" %1 %2").arg(QLatin1String(names[i])).arg(ms[i] / frames, 0, 'f', 2); }
        fprintf(stderr, "paint %dx%d: %d frames (%d partial) in %.1f s, avg %.2f ms:%s ·%s\n", sz.width(), sz.height(), frames, partial,
                window.elapsed() / 1000.0, total / frames, qPrintable(s), qPrintable(note));
        std::fill(std::begin(ms), std::end(ms), 0.0); frames = partial = 0; window.start();
    }
};
}

void BeaconView::paintEvent(QPaintEvent *e)
{
    static PaintProfile prof;
    prof.begin();
    QPainter p(this);
    m_hits.clear();
    m_paintClip = QRectF(e->rect()).adjusted(-1, -1, 1, 1);
    drawBase(p);
    prof.lap("base");
    p.setRenderHint(QPainter::Antialiasing);

    const Fix &fix = m_loc->fix();
    if (fix.valid) {
        drawPois(p);
        prof.lap("pois");
        drawBeacons(p);
        prof.lap("beacons");
        if (m_showFlockCameras) drawFlockCameras(p);
        drawAvoidRoute(p);
        drawInspectPlan(p);
        drawAnchors(p);
        prof.lap("cams");
        drawMe(p);
        prof.lap("me");
        drawLabels(p);
        prof.lap("labels");
        if (m_showDevices) drawDevices(p);
        drawEvents(p);
        prof.lap("events");
    } else {
        if (m_showFlockCameras) drawFlockCameras(p);
        drawAvoidRoute(p);
        drawInspectPlan(p);
        p.setPen(C_TEXT);
        QFont f = p.font(); f.setPointSizeF(f.pointSizeF() * 1.4); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("Listening for beacons…"));
        p.setFont(font());
    }
    drawHud(p);
    prof.lap("hud");
    drawControls(p);
    drawScale(p);
    drawTicker(p);
    drawAttribution(p);
    if (m_showLegend) drawLegend(p);
    drawCard(p);
    prof.lap("chrome");
    if (prof.on) prof.end(e->rect() == rect(), size(), QStringLiteral(" z%1 route %2 cams %3 aps %4 pois %5 hist %6 imported %7").arg(m_zoom, 0, 'f', 1).arg(m_routeFixes.size()).arg(m_flockCameras.size())
                                  .arg(m_loc->accessPoints().size()).arg(m_loc->pois().size()).arg(m_loc->history().size()).arg(m_loc->importedHistory().size()));
}

// Basemap, satellite dimming, vignette, heat map and tracks: no hits, and they change only with
// the view or the data, so they're rendered once into m_base and blitted while the pulse runs.
void BeaconView::drawBase(QPainter &p)
{
    const Fix &fix = m_loc->fix();
    if (m_showHeatmap) loadRouteFixes();
    const BaseKey key{size(), devicePixelRatioF(), m_center, m_zoom, int(m_layer), m_tileVersion, m_heatSerial, m_heatVersion, m_trackVersion,
                      m_showHeatmap, m_showImported, fix.valid, m_loc->history().size(), m_loc->importedHistory().size()};
    // Tiles still missing: look again now and then (a failure's back-off runs out, a download slot frees up)
    const bool retry = m_baseMissing && m_baseAt.isValid() && m_baseAt.elapsed() > 1000;
    if (!(key == m_baseKey) || m_base.isNull() || retry) {
        m_baseKey = key; m_baseMissing = m_baseTopo = false; m_baseAt.start(); m_wanted.clear();
        const qreal dpr = devicePixelRatioF();
        if (m_base.size() != size() * dpr) m_base = QPixmap(size() * dpr);
        m_base.setDevicePixelRatio(dpr);
        m_base.fill(C_BG);
        QPainter bp(&m_base);
        bp.setRenderHint(QPainter::Antialiasing);
        drawTiles(bp, 0);
        if (m_layer == Satellite) {                           // the hybrid: imagery, contours, roads, place labels
            bp.fillRect(rect(), QColor(0, 0, 0, 45));         // markers need contrast over imagery
            if (m_showContours) drawTiles(bp, TileSource::Contours);
            drawTiles(bp, TileSource::Roads);
            drawTiles(bp, TileSource::Labels);
        }
        QRadialGradient v(rect().center(), qMax(width(), height()) * 0.75);   // vignette so panels read well
        v.setColorAt(0.6, QColor(0, 0, 0, 0)); v.setColorAt(1.0, QColor(0, 0, 0, m_layer == Streets ? 70 : 130));
        bp.fillRect(rect(), v);
        if (m_showHeatmap) drawHeatmap(bp);
        if (fix.valid) {
            if (m_showImported) drawImportedTrack(bp);
            drawTrack(bp);
        }
        // Downloads for tiles that have scrolled or zoomed out of view: let them go, the ones in view come sooner
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (m_wanted.contains(it.key())) { ++it; continue; }
            const TileReq r = it.value();
            it = m_pending.erase(it);
            m_src->cancel(r.ov ? TileSource::Layer(r.ov) : srcLayer(r.l), r.z, r.x, r.y, this);
        }
    }
    p.drawPixmap(0, 0, m_base);
}

// Liang–Barsky: shrink a→b to the part inside r; false when none of it is. *t0 = how far along a
// moved (0..1), to keep a dash pattern in place. Without it a long off-screen segment (an IP fix
// jumping across the country, seen at z17) costs the dasher every pixel of its full length.
static bool clipLine(QPointF &a, QPointF &b, const QRectF &r, double *t0out = nullptr)
{
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    const double pp[4] = {-dx, dx, -dy, dy}, q[4] = {a.x() - r.left(), r.right() - a.x(), a.y() - r.top(), r.bottom() - a.y()};
    double t0 = 0, t1 = 1;
    for (int i = 0; i < 4; ++i) {
        if (pp[i] == 0) { if (q[i] < 0) return false; continue; }
        const double t = q[i] / pp[i];
        if (pp[i] < 0) { if (t > t1) return false; t0 = qMax(t0, t); }
        else           { if (t < t0) return false; t1 = qMin(t1, t); }
    }
    const QPointF a0 = a;
    a = a0 + QPointF(dx, dy) * t0; b = a0 + QPointF(dx, dy) * t1;
    if (t0out) *t0out = t0;
    return true;
}

// Does a circle's outline cross the view? One that encloses the whole view needs only its fill;
// stroking it (dashed, 20 000 px across) is what made coarse fixes slow to pan.
static bool ringCrosses(const QPointF &c, double r, const QRectF &view)
{
    const double fx = qMax(qAbs(view.left() - c.x()), qAbs(view.right() - c.x())), fy = qMax(qAbs(view.top() - c.y()), qAbs(view.bottom() - c.y()));
    if (fx * fx + fy * fy < r * r) return false;              // the view is inside it
    const double nx = qMax(0.0, qMax(view.left() - c.x(), c.x() - view.right())), ny = qMax(0.0, qMax(view.top() - c.y(), c.y() - view.bottom()));
    return nx * nx + ny * ny <= r * r;                         // else: wholly outside
}

// Where you have been according to an imported export: a thin dotted trail under the live
// track, visits as hollow rings. Thinned to the screen so a decade of Timeline stays quick.
void BeaconView::drawImportedTrack(QPainter &p)
{
    const auto &h = m_loc->importedHistory();
    if (h.isEmpty()) return;
    const QRectF view = QRectF(rect()).adjusted(-20, -20, 20, 20);
    const QColor trail(0xd8, 0xb4, 0x6a, 120), visit(0xf2, 0xc9, 0x7a, 200);
    QPen pen(trail, 1.4, Qt::DotLine, Qt::RoundCap); p.setPen(pen);
    QPointF last; bool haveLast = false; qint64 lastT = 0;
    QList<QPointF> visits;
    for (const Fix &f : h) {
        const QPointF s = toScreen(f.lat, f.lon);
        const bool isVisit = f.source == QLatin1String("visit");
        if (isVisit) { if (view.contains(s)) visits << s; continue; }
        const qint64 t = f.time.toSecsSinceEpoch();
        if (haveLast) {
            const bool gap = t - lastT > 6 * 3600;           // a new day / a flight: don't join across it
            if (!gap && (view.contains(s) || view.contains(last)) && QLineF(last, s).length() >= 2) {
                QPointF a = last, b = s; double t0 = 0;
                if (clipLine(a, b, view, &t0)) { pen.setDashOffset(QLineF(last, s).length() * t0 / pen.widthF()); p.setPen(pen); p.drawLine(a, b); }
            }
            else if (!gap && QLineF(last, s).length() < 2) continue;   // same pixel: keep the earlier anchor
        }
        last = s; lastT = t; haveLast = true;
    }
    p.setPen(QPen(visit, 1.3)); p.setBrush(Qt::NoBrush);
    for (const QPointF &s : visits) p.drawEllipse(s, 3.5, 3.5);
}

void BeaconView::drawTrack(QPainter &p)
{
    const auto &h = m_loc->history();
    if (h.size() < 2) return;
    const QRectF view = QRectF(rect()).adjusted(-10, -10, 10, 10);
    for (int i = 1; i < h.size(); ++i) {
        const bool coarse = h[i - 1].source == QLatin1String("ip") || h[i].source == QLatin1String("ip");
        const QPointF a = toScreen(h[i - 1].lat, h[i - 1].lon), b = toScreen(h[i].lat, h[i].lon);
        if (QLineF(a, b).length() < 1) continue;
        QPointF ca = a, cb = b; double t0 = 0;
        if (!clipLine(ca, cb, view, &t0)) continue;
        QPen pen(coarse ? QColor(0x9f, 0xb0, 0xc8, 110) : QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 150), coarse ? 1.6 : 2.6,
                 coarse ? Qt::DashLine : Qt::SolidLine, Qt::RoundCap);
        if (coarse) pen.setDashOffset(QLineF(a, b).length() * t0 / pen.widthF());   // dashes stay put while panning
        p.setPen(pen); p.drawLine(ca, cb);
    }
    for (int i = 0; i + 1 < h.size(); ++i) {
        const QPointF s = toScreen(h[i].lat, h[i].lon);
        if (!rect().adjusted(-10, -10, 10, 10).contains(s.toPoint())) continue;
        const bool coarse = h[i].source == QLatin1String("ip");
        p.setPen(QPen(coarse ? C_DIM : Qt::white, 1.4));
        p.setBrush(coarse ? Qt::NoBrush : QBrush(C_ME.darker(130)));
        p.drawEllipse(s, 4, 4);
    }
}

void BeaconView::drawPois(QPainter &p)
{
    const auto &pois = m_loc->pois();
    m_poiPos = QList<QPointF>(pois.size());
    const QRectF view = QRectF(rect()).adjusted(-30, -30, 30, 30);
    // Cluster on a screen grid when zoomed out; individual pins when close in
    const double cell = m_zoom < 13 ? 56 : m_zoom < 15.5 ? 40 : 0;
    QHash<qint64, QList<int>> grid;
    QList<int> order;
    QList<int> pedsLast;                                     // pediatric ERs are drawn on top
    for (int i = 0; i < pois.size(); ++i) {
        if (m_hiddenCats.contains(pois[i].cat)) continue;
        const QPointF s = toScreen(pois[i].lat, pois[i].lon);
        if (!view.contains(s)) continue;
        m_poiPos[i] = s;
        if (cell > 0) grid[(qint64(std::floor(s.x() / cell)) << 32) ^ qint64(std::floor(s.y() / cell) + 100000)].append(i);
        else if (pois[i].cat == QLatin1String("peds_er")) pedsLast << i;
        else order << i;
    }
    order += pedsLast;
    QFont emoji = font(); emoji.setPointSizeF(font().pointSizeF() * 1.05);
    auto pin = [&](const QPointF &s, const PoiCategory *c, bool hot, bool wifi) {
        const double r = hot ? 14 : 11.5;
        if (!m_paintClip.intersects(QRectF(s.x() - r - 3, s.y() - r - 3, 2 * r + 6, 2 * r + 8))) return;   // outside this repaint
        QColor ring = c ? c->color : C_DIM;
        p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 90)); p.drawEllipse(s + QPointF(0, 1.5), r + 1, r + 1);
        p.setBrush(QColor(12, 17, 28, 235)); p.setPen(QPen(ring, hot ? 2.6 : 2.0)); p.drawEllipse(s, r, r);
        p.setFont(emoji); p.setPen(Qt::white);
        p.drawText(QRectF(s.x() - r, s.y() - r, 2 * r, 2 * r), Qt::AlignCenter, c ? c->icon : QStringLiteral("•"));
        if (wifi) { p.setPen(Qt::NoPen); p.setBrush(C_ME); p.drawEllipse(s + QPointF(r * 0.72, -r * 0.72), 3.2, 3.2); }
    };
    if (cell > 0) {
        // A cluster shows its most important member: a pediatric ER, then an ER, then police / fire, else the most common kind.
        // Clusters holding a pediatric ER are drawn last, on top.
        QList<QList<int>> clusters, pedsClusters;
        for (auto it = grid.begin(); it != grid.end(); ++it) {
            bool peds = false; for (int i : it.value()) if (pois[i].cat == QLatin1String("peds_er")) peds = true;
            (peds ? pedsClusters : clusters) << it.value();
        }
        clusters += pedsClusters;
        for (const QList<int> &ids : clusters) {
            QPointF c; QHash<QString, int> counts;
            bool peds = false, er = false, police = false, fire = false;
            for (int i : ids) {
                c += m_poiPos[i]; counts[pois[i].cat]++;
                const QString &cat = pois[i].cat;
                if (cat == QLatin1String("peds_er")) peds = true;
                else if (cat == QLatin1String("health") && pois[i].emergency) er = true;
                else if (cat == QLatin1String("police")) police = true;
                else if (cat == QLatin1String("fire")) fire = true;
            }
            c /= ids.size();
            QString top; int best = 0;
            if (peds) top = QStringLiteral("peds_er");
            else if (er) top = QStringLiteral("health");
            else if (police) top = QStringLiteral("police");
            else if (fire) top = QStringLiteral("fire");
            else for (auto ci = counts.begin(); ci != counts.end(); ++ci) if (ci.value() > best) { best = ci.value(); top = ci.key(); }
            const int hitIdx = m_hits.size();
            const bool hot = m_hover == hitIdx;
            pin(c, Locator::poiCategory(top), hot, false);
            if (ids.size() > 1 && m_paintClip.intersects(QRectF(c.x() - 20, c.y() - 30, 60, 50))) badge(p, c + QPointF(11, -11), QString::number(ids.size()), counts.size() > 1 ? C_TEXT : Locator::poiCategory(top)->color);
            for (int i : ids) m_poiPos[i] = c;
            m_hits.append({ids.size() > 1 ? HitCluster : HitPoi, c, 13, ids});
        }
    } else {
        for (int i : order) {
            const int hitIdx = m_hits.size();
            const bool hot = m_hover == hitIdx || (m_selKind == HitPoi && m_selItem == i);
            pin(m_poiPos[i], Locator::poiCategory(pois[i].cat), hot, pois[i].wifi && pois[i].cat != QLatin1String("wifi"));
            m_hits.append({HitPoi, m_poiPos[i], 13, {i}});
        }
    }
    p.setFont(font());
}

static QPointF offsetM(const QPointF &origin, double bearingDeg, double metres, double mpp)
{
    const double a = qDegreesToRadians(bearingDeg);
    return origin + QPointF(std::sin(a) * metres / mpp, -std::cos(a) * metres / mpp);
}

void BeaconView::drawBeacons(QPainter &p)
{
    const Fix &fix = m_loc->fix();
    const auto &aps = m_loc->accessPoints();
    const QPointF me = toScreen(fix.lat, fix.lon);
    const double mpp = metersPerPixel(fix.lat);
    m_beaconPos = QList<QPointF>(aps.size());

    // Estimates and statuses (pattern matches, centroids over every sample) come from a cache that
    // follows the scan and refits, not recomputed 30 times a second
    if (m_apEst.size() != aps.size() || !m_apEstAt.isValid() || m_apEstAt.elapsed() >= 1000) {
        m_apEst.resize(aps.size()); m_apStatus.resize(aps.size());
        for (int i = 0; i < aps.size(); ++i) { m_apEst[i] = m_loc->estimateFor(aps[i]); m_apStatus[i] = m_loc->apStatus(aps[i]); }
        m_apEstAt.start();
    }
    struct Item { int i; ApEstimate e; QColor col; QString st; QPointF pos; };
    QList<Item> items;
    QList<int> atMe;                                           // too close to draw apart at this zoom
    for (int i = 0; i < aps.size(); ++i) {
        const ApEstimate &e = m_apEst[i];
        if (e.kind == ApEstimate::None) continue;
        const QString &st = m_apStatus[i];
        QColor col = st == QLatin1String("used") ? C_USED
                   : st == QLatin1String("home") ? C_HOME
                   : st == QLatin1String("active") ? C_ACTIVE
                   : st == QLatin1String("travelling") ? C_TRAVEL : C_IGNORED;
        if (e.kind != ApEstimate::Ring && st == QLatin1String("used")) col = C_LOCATED;
        if (e.kind == ApEstimate::Ring && e.radiusM / mpp < 14) { atMe << i; m_beaconPos[i] = me; continue; }
        const QPointF pos = e.kind == ApEstimate::Ring ? offsetM(me, e.bearingDeg, e.radiusM, mpp) : toScreen(e.lat, e.lon);
        items.append({i, e, col, st, pos});
    }
    // RSSI distance orbits first (faint), then uncertainty rings, then dots so nothing is buried
    const QRectF screen(rect());
    for (const Item &it : items) {
        if (it.e.kind != ApEstimate::Ring || !ringCrosses(me, it.e.radiusM / mpp, screen)) continue;
        QColor c = it.col; c.setAlpha(26);
        p.setPen(QPen(c, 1, Qt::DotLine)); p.setBrush(Qt::NoBrush);
        p.drawEllipse(me, it.e.radiusM / mpp, it.e.radiusM / mpp);
    }
    const QRectF view = QRectF(rect()).adjusted(-40, -40, 40, 40);
    if (m_showApCircles) {
        // Regions first (grade R: only an area is known): faint discs of radius R95, cheap enough for hundreds
        {
            QColor rc = gradeColor(QStringLiteral("R"));
            rc.setAlpha(60);
            p.setPen(QPen(rc, 1)); p.setBrush(Qt::NoBrush);
            for (const Item &it : items) {
                if (it.e.kind != ApEstimate::Region) continue;
                const double r = it.e.radiusM / mpp;
                if (r < 4 || !view.intersects(QRectF(it.pos.x() - r, it.pos.y() - r, 2 * r, 2 * r))) continue;
                p.drawEllipse(it.pos, r, r);
            }
        }
        for (const Item &it : items) {
            if (it.e.kind == ApEstimate::Ring || it.e.kind == ApEstimate::Region || it.e.kind == ApEstimate::Mobile) continue;
            const Estimator::Fit &f = it.e.fit;
            if (it.e.kind == ApEstimate::Trilat && gradedFix(f) && f.semiMajor > 0) {
                // The 95 % error ellipse: semi-axes × √χ²₂(0.95) = 2.4477, major axis along the bearing orientDeg.
                // Screen y points south, so rotating by +bearing (clockwise on screen) turns "up" (north) onto it.
                const double fm = metersPerPixel(it.e.lat);
                const double a = f.semiMajor * 2.4477 / fm, b = qMax(f.semiMinor, 0.0) * 2.4477 / fm;
                if (!view.intersects(QRectF(it.pos.x() - a, it.pos.y() - a, 2 * a, 2 * a))) continue;
                const bool doubtful = !f.inHull || f.ambiguous || f.modes >= 2;   // Estimator::flags(): extrapolated / ambiguous
                const QColor gc = gradeColor(f.grade);
                if (a >= 3) {
                    QPen pen(gc, 1.2, doubtful ? Qt::DashLine : Qt::SolidLine);
                    p.save();
                    p.translate(it.pos); p.rotate(f.orientDeg);
                    p.setPen(pen); p.setBrush(Qt::NoBrush);
                    p.drawEllipse(QRectF(-qMax(b, 1.0), -a, 2 * qMax(b, 1.0), 2 * a));
                    p.restore();
                }
                if ((f.ambiguous || f.modes >= 2) && (f.altLat != 0 || f.altLon != 0)) {   // the mirror / second mode: a hollow ghost
                    const QPointF ghost = toScreen(f.altLat, f.altLon);
                    QColor lc = gc; lc.setAlpha(110);
                    p.setPen(QPen(lc, 1, Qt::DotLine)); p.setBrush(Qt::NoBrush);
                    p.drawLine(it.pos, ghost);
                    p.setPen(QPen(gc, 1.6, Qt::DashLine)); p.drawEllipse(ghost, 5.5, 5.5);
                }
                continue;
            }
            const double r = it.e.radiusM / mpp;
            if (r < 6 || !ringCrosses(it.pos, r, screen)) continue;
            QColor c = it.col; c.setAlpha(60);
            p.setPen(QPen(c, 1, Qt::DashLine)); p.setBrush(Qt::NoBrush);
            p.drawEllipse(it.pos, r, r);
        }
    }
    // Group markers that land on the same spot into one with a count
    QList<QList<int>> groups;                                  // indices into items
    for (int k = 0; k < items.size(); ++k) {
        bool placed = false;
        for (QList<int> &g : groups)
            if (QLineF(items[g.first()].pos, items[k].pos).length() < 9) { g << k; placed = true; break; }
        if (!placed) groups.append(QList<int>{k});
    }
    int focus = -1;                                            // the hovered / selected single beacon (index into items)
    for (const QList<int> &g : groups) {
        const int k0 = g.first();
        const Item &it = items[k0];
        const int hitIdx = m_hits.size();
        const bool hot = m_hover == hitIdx || (m_selKind == HitBeacon && g.size() == 1 && m_selItem == it.i);
        const double dot = it.st == QLatin1String("used") || it.st == QLatin1String("active") ? 5.0 : 3.5;
        if (!m_paintClip.intersects(QRectF(it.pos.x() - 30, it.pos.y() - 30, 60, 60))) {
            // outside this repaint (the pulse's): only the hit below
        } else if (it.e.kind == ApEstimate::Observed) {              // hollow diamond = from the internal map (heard here before)
            const double d = hot ? 8 : 6;
            QPainterPath path; path.moveTo(it.pos.x(), it.pos.y() - d); path.lineTo(it.pos.x() + d, it.pos.y());
            path.lineTo(it.pos.x(), it.pos.y() + d); path.lineTo(it.pos.x() - d, it.pos.y()); path.closeSubpath();
            p.setBrush(Qt::NoBrush); p.setPen(QPen(it.col, hot ? 2.2 : 1.6)); p.drawPath(path);
        } else if (it.e.kind == ApEstimate::Wigle) {          // diamond = ground truth
            const double d = hot ? 9 : 7;
            QPainterPath path; path.moveTo(it.pos.x(), it.pos.y() - d); path.lineTo(it.pos.x() + d, it.pos.y());
            path.lineTo(it.pos.x(), it.pos.y() + d); path.lineTo(it.pos.x() - d, it.pos.y()); path.closeSubpath();
            glowDot(p, it.pos, 1, it.col, 12);
            p.setBrush(it.col); p.setPen(QPen(Qt::white, 1.2)); p.drawPath(path);
        } else if (it.e.kind == ApEstimate::Mobile) {         // travels with us: a small "M" chip at the last place heard
            const double r = hot ? 8.5 : 7;
            p.setPen(QPen(QColor(255, 255, 255, 210), 1.2)); p.setBrush(gradeColor(QStringLiteral("M"))); p.drawEllipse(it.pos, r, r);
            QFont f = p.font(); const QFont keep = f; f.setBold(true); f.setPixelSize(int(r * 1.3)); p.setFont(f);
            p.setPen(Qt::white); p.drawText(QRectF(it.pos.x() - r, it.pos.y() - r, 2 * r, 2 * r), Qt::AlignCenter, QStringLiteral("M"));
            p.setFont(keep);
        } else {
            glowDot(p, it.pos, hot ? dot + 2 : dot, it.col, it.st == QLatin1String("used") ? 3.2 : 2.2);
        }
        if (hot && g.size() == 1) focus = k0;
        QList<int> apIdx;
        for (int k : g) { apIdx << items[k].i; m_beaconPos[items[k].i] = it.pos; }
        if (g.size() > 1 && m_paintClip.intersects(QRectF(it.pos.x() - 20, it.pos.y() - 30, 60, 50))) badge(p, it.pos + QPointF(9, -9), QString::number(g.size()), it.col);
        m_hits.append({HitBeacon, it.pos, 9, apIdx});
    }
    if (focus >= 0) drawSuggestion(p, items[focus].i, items[focus].pos);
    if (!atMe.isEmpty()) m_hits.append({HitBeacon, me, 12, atMe});
}

// Where one more sample would tighten this AP the most (Fit::suggest*): a small crosshair, dotted from the estimate
void BeaconView::drawSuggestion(QPainter &p, int apIndex, const QPointF &from)
{
    const auto &aps = m_loc->accessPoints();
    if (apIndex < 0 || apIndex >= aps.size()) return;
    const ApEstimate e = m_loc->estimateFor(aps[apIndex]);
    const Estimator::Fit &f = e.fit;
    if (!(f.suggestGain > 0) || e.kind == ApEstimate::Mobile) return;
    const QPointF at = toScreen(f.suggestLat, f.suggestLon);
    if (QLineF(from, at).length() < 4) return;
    const QColor gc = e.kind == ApEstimate::Trilat || e.kind == ApEstimate::Region ? gradeColor(f.grade) : C_TEXT;
    QColor lc = gc; lc.setAlpha(170);
    p.setPen(QPen(lc, 1.2, Qt::DotLine)); p.setBrush(Qt::NoBrush);
    p.drawLine(from, at);
    QPen pen(QColor(8, 12, 20, 200), 3.2); p.setPen(pen);           // dark under-stroke so it reads on any basemap
    p.drawEllipse(at, 6, 6);
    p.drawLine(at + QPointF(-11, 0), at + QPointF(-3, 0)); p.drawLine(at + QPointF(3, 0), at + QPointF(11, 0));
    p.drawLine(at + QPointF(0, -11), at + QPointF(0, -3)); p.drawLine(at + QPointF(0, 3), at + QPointF(0, 11));
    pen.setColor(gc); pen.setWidthF(1.5); p.setPen(pen);
    p.drawEllipse(at, 6, 6);
    p.drawLine(at + QPointF(-11, 0), at + QPointF(-3, 0)); p.drawLine(at + QPointF(3, 0), at + QPointF(11, 0));
    p.drawLine(at + QPointF(0, -11), at + QPointF(0, -3)); p.drawLine(at + QPointF(0, 3), at + QPointF(0, 11));
    QFont sf = font(); sf.setPointSizeF(sf.pointSizeF() * 0.8); p.setFont(sf);
    const QString t = QStringLiteral("sample here");
    const QRectF tr(at.x() + 13, at.y() - 9, p.fontMetrics().horizontalAdvance(t) + 10, 18);
    p.setPen(Qt::NoPen); p.setBrush(QColor(8, 12, 20, 200)); p.drawRoundedRect(tr, 4, 4);
    p.setPen(C_TEXT); p.drawText(tr, Qt::AlignCenter, t);
    p.setFont(font());
}

void BeaconView::drawMe(QPainter &p)
{
    const Fix &fix = m_loc->fix();
    const QPointF me = toScreen(fix.lat, fix.lon);
    const double mpp = metersPerPixel(fix.lat);
    const bool coarse = fix.source == QLatin1String("ip");
    if (fix.accuracy > 0) {
        const double r = qBound(8.0, fix.accuracy / mpp, 20000.0);
        const QColor fill(C_ME.red(), C_ME.green(), C_ME.blue(), coarse ? 12 : 22);
        if (ringCrosses(me, r, QRectF(rect()).adjusted(-4, -4, 4, 4))) {
            p.setPen(QPen(QColor(C_ME.red(), C_ME.green(), C_ME.blue(), coarse ? 90 : 130), 1.5, coarse ? Qt::DashLine : Qt::SolidLine));
            p.setBrush(fill);
            p.drawEllipse(me, r, r);
        } else if (QLineF(me, rect().center()).length() < r) p.fillRect(rect(), fill);   // the whole view is inside it
        if (r < 3000) {                                        // radar sweep inside the accuracy ring
            QConicalGradient sweep(me, -m_sweep);
            sweep.setColorAt(0.0, QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 80));
            sweep.setColorAt(0.16, QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 0));
            sweep.setColorAt(1.0, QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 0));
            // only the leading 16 % of the cone has colour: fill just that wedge (a quarter of the pixels)
            p.setPen(Qt::NoPen); p.setBrush(sweep); p.drawPie(QRectF(me.x() - r, me.y() - r, 2 * r, 2 * r), qRound(-m_sweep * 16), qRound(0.17 * 360 * 16));
        }
    }
    const double pr = 10 + 26 * m_phase;
    QColor pc = C_ME; pc.setAlpha(int(160 * (1 - m_phase)));
    p.setPen(QPen(pc, 2)); p.setBrush(Qt::NoBrush); p.drawEllipse(me, pr, pr);
    glowDot(p, me, 7, C_ME, 3.5);
    p.setBrush(Qt::white); p.setPen(Qt::NoPen); p.drawEllipse(me, 2.5, 2.5);
    // "n heard here" when the beacons are too close to separate at this zoom
    int n = 0;
    for (const Hit &h : m_hits) if (h.kind == HitBeacon && h.pos == me) n = h.items.size();
    if (n > 0) badge(p, me + QPointF(12, 12), QStringLiteral("%1 📶").arg(n), C_USED);
}

void BeaconView::drawLabels(QPainter &p)
{
    QFont f = font(); f.setPointSizeF(f.pointSizeF() * 0.85); p.setFont(f);
    QList<QRectF> taken;
    for (const Hit &h : m_hits) taken << QRectF(h.pos.x() - h.radius, h.pos.y() - h.radius, 2 * h.radius, 2 * h.radius);
    const auto &pois = m_loc->pois();
    const auto &aps = m_loc->accessPoints();
    const Fix &fix = m_loc->fix();

    struct Lbl { QPointF at; QString text; QColor col; double prio; };
    QList<Lbl> lbls;
    if (m_zoom >= 15.5) {
        for (const Hit &h : m_hits) {
            if (h.kind != HitPoi) continue;
            const Poi &pt = pois[h.items.first()];
            if (pt.name.isEmpty()) continue;
            const PoiCategory *c = Locator::poiCategory(pt.cat);
            lbls.append({h.pos, pt.name, c ? c->color : C_TEXT, Locator::distanceM(fix.lat, fix.lon, pt.lat, pt.lon)});
        }
    }
    std::sort(lbls.begin(), lbls.end(), [](const Lbl &a, const Lbl &b) { return a.prio < b.prio; });
    const double lh = p.fontMetrics().height() + 4;
    auto place = [&](const QPointF &c, const QSizeF &sz, QRectF *out) {
        // right, above, below, left — first one that fits and doesn't cover anything
        const QPointF cands[] = {QPointF(c.x() + 13, c.y() - sz.height() / 2), QPointF(c.x() - sz.width() / 2, c.y() - 13 - sz.height()),
                                 QPointF(c.x() - sz.width() / 2, c.y() + 13), QPointF(c.x() - 13 - sz.width(), c.y() - sz.height() / 2)};
        for (const QPointF &tl : cands) {
            const QRectF box(tl, sz);
            if (!QRectF(rect()).contains(box)) continue;
            if (std::any_of(taken.begin(), taken.end(), [&](const QRectF &t) { return t.intersects(box); })) continue;
            taken << box; *out = box; return true;
        }
        return false;
    };
    int shown = 0;
    for (const Lbl &l : lbls) {                                // places
        if (shown >= 40) break;
        const QString text = p.fontMetrics().elidedText(l.text, Qt::ElideRight, 180);
        const QSizeF sz(p.fontMetrics().horizontalAdvance(text) + 10, lh);
        QRectF box;
        if (!place(l.at, sz, &box)) continue;
        if (!m_paintClip.intersects(box)) { ++shown; continue; }
        p.setPen(Qt::NoPen); p.setBrush(QColor(8, 12, 20, 185)); p.drawRoundedRect(box, 4, 4);
        p.setBrush(l.col); p.drawRoundedRect(QRectF(box.left(), box.top() + 3, 2.5, box.height() - 6), 1, 1);
        p.setPen(QColor(C_TEXT.red(), C_TEXT.green(), C_TEXT.blue(), 225));
        p.drawText(box.adjusted(6, 0, -3, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
        ++shown;
    }

    // Wi-Fi names: everyone at z ≥ 15, the 12 loudest at z 13–15, none further out
    if (m_showNames && m_zoom >= 13 && m_labels.size() == aps.size()) {
        const QPointF me = toScreen(fix.lat, fix.lon);
        struct NL { int i; QPointF at; int extra; };
        QList<NL> names;
        for (const Hit &h : m_hits) {
            if (h.kind != HitBeacon || h.pos == me) continue;
            int best = h.items.first();
            for (int i : h.items) if (aps[i].dbm > aps[best].dbm) best = i;   // a group is labelled by its loudest
            names.append({best, h.pos, int(h.items.size()) - 1});
        }
        std::sort(names.begin(), names.end(), [&](const NL &a, const NL &b) { return aps[a.i].dbm > aps[b.i].dbm; });
        const int limit = m_zoom >= 15 ? names.size() : 12;
        const bool bands = m_zoom >= 16;
        QFont it = p.font(); it.setItalic(true);
        QFont bf = p.font(); bf.setPointSizeF(bf.pointSizeF() * 0.75); bf.setBold(true);
        const QFontMetricsF bfm(bf);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        int n = 0;
        for (const NL &nl : names) {
            if (n >= limit) break;
            const LabelInfo &li = m_labels[nl.i];
            const QString text = nl.extra > 0 ? QStringLiteral("%1 +%2").arg(li.text).arg(nl.extra) : li.text;
            const double tw = nl.extra > 0 ? p.fontMetrics().horizontalAdvance(text) : li.width;
            const double bw = bands ? bfm.horizontalAdvance(li.band) + 8 : 0;
            const QSizeF sz(tw + 12 + (bands ? bw + 4 : 0), lh);
            QRectF box;
            if (!place(nl.at, sz, &box)) continue;
            if (!m_paintClip.intersects(box.adjusted(-16, 0, 16, 0))) { ++n; continue; }   // placed, but outside this repaint
            // a beacon that just appeared slides its name in from the marker
            double alpha = 1.0, slide = 0.0;
            const auto born = m_labelBorn.constFind(aps[nl.i].bssid);
            if (born != m_labelBorn.constEnd()) {
                const double t = qBound(0.0, (now - born.value()) / 900.0, 1.0);
                alpha = t; slide = (1 - t) * (1 - t) * 14;
                if (t < 1) box.translate(box.left() > nl.at.x() ? -slide : slide, 0);
            }
            QColor bg(8, 12, 20, int(190 * alpha)), col = li.col; col.setAlphaF(alpha);
            p.setPen(Qt::NoPen); p.setBrush(bg); p.drawRoundedRect(box, 4, 4);
            p.setBrush(col); p.drawRoundedRect(QRectF(box.left(), box.top() + 3, 2.5, box.height() - 6), 1, 1);
            QColor tc = li.hidden ? C_DIM : col.lighter(115); tc.setAlphaF(0.95 * alpha);
            p.setPen(tc); p.setFont(li.hidden ? it : f);
            p.drawText(QRectF(box.left() + 6, box.top(), tw + 4, box.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
            if (bands) {
                const QRectF br(box.right() - bw - 4, box.top() + 3, bw, box.height() - 6);
                QColor bc = col; bc.setAlphaF(0.85 * alpha);
                p.setFont(bf); p.setBrush(bc); p.drawRoundedRect(br, br.height() / 2, br.height() / 2);
                p.setPen(QColor(8, 12, 20, int(255 * alpha))); p.drawText(br, Qt::AlignCenter, li.band);
            }
            p.setFont(f);
            ++n;
        }
    }
    p.setFont(font());
}

// ── Events: what just happened, as motion ────────────────────────────────────
static double easeOut(double t)  { t = qBound(0.0, t, 1.0); return 1 - (1 - t) * (1 - t) * (1 - t); }
static double easeBounce(double t)
{
    t = qBound(0.0, t, 1.0);
    if (t < 1 / 2.75) return 7.5625 * t * t;
    if (t < 2 / 2.75) { t -= 1.5 / 2.75; return 7.5625 * t * t + 0.75; }
    if (t < 2.5 / 2.75) { t -= 2.25 / 2.75; return 7.5625 * t * t + 0.9375; }
    t -= 2.625 / 2.75; return 7.5625 * t * t + 0.984375;
}
static QString eventGlyph(const QString &type)
{
    if (type == QLatin1String("ap_new")) return QStringLiteral("📶");
    if (type == QLatin1String("ap_lost")) return QStringLiteral("💤");
    if (type == QLatin1String("ap_up")) return QStringLiteral("▲");
    if (type == QLatin1String("ap_down")) return QStringLiteral("▼");
    if (type == QLatin1String("ap_placed")) return QStringLiteral("💎");
    if (type == QLatin1String("ap_refit")) return QStringLiteral("🎯");
    if (type == QLatin1String("device")) return QStringLiteral("📱");
    if (type == QLatin1String("device_online")) return QStringLiteral("🟢");
    if (type == QLatin1String("device_offline")) return QStringLiteral("⚫");
    if (type == QLatin1String("fix")) return QStringLiteral("◎");
    if (type == QLatin1String("stop")) return QStringLiteral("📍");
    if (type == QLatin1String("achievement")) return QStringLiteral("🏆");
    if (type == QLatin1String("region")) return QStringLiteral("🚩");
    if (type == QLatin1String("prefetch")) return QStringLiteral("💾");
    if (type == QLatin1String("error")) return QStringLiteral("⚠");
    return QStringLiteral("•");
}
static QColor eventColor(const QString &type)
{
    if (type == QLatin1String("ap_up") || type == QLatin1String("achievement") || type == QLatin1String("region")) return C_ACTIVE;
    if (type == QLatin1String("ap_down")) return QColor(0xff, 0xa5, 0x3d);
    if (type == QLatin1String("ap_lost") || type == QLatin1String("error")) return C_IGNORED;
    if (type == QLatin1String("ap_placed") || type == QLatin1String("stop") || type == QLatin1String("ap_refit")) return C_LOCATED;
    if (type == QLatin1String("device") || type == QLatin1String("device_online")) return C_ACTIVE;
    return C_ME;
}

void BeaconView::onEvent(const QString &json)
{
    m_apEstAt.invalidate();                                    // a refit / placement moves an estimate
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    Anim a;
    a.type = o["type"].toString(); a.bssid = o["bssid"].toString(); a.ssid = o["ssid"].toString(); a.text = o["text"].toString();
    a.glyph = eventGlyph(a.type); a.col = eventColor(a.type);
    const QDateTime t = QDateTime::fromString(o["time"].toString(), Qt::ISODate);
    a.start = t.isValid() ? t.toMSecsSinceEpoch() : QDateTime::currentMSecsSinceEpoch();
    a.hasPos = o.contains("lat"); a.lat = o["lat"].toDouble(); a.lon = o["lon"].toDouble();
    a.hasFrom = o.contains("fromLat"); a.fromLat = o["fromLat"].toDouble(); a.fromLon = o["fromLon"].toDouble();
    a.r = o["r"].toDouble(); a.bearing = o["bearing"].toDouble(); a.delta = o["delta"].toInt();
    if (a.type == QLatin1String("ap_refit")) {                 // precompute everything the animation needs: nothing allocates per frame
        a.acc = o["acc"].toDouble(); a.prevAcc = o["prevAcc"].toDouble(); a.n = o["n"].toInt(); a.vantage = o["vantage"].toInt();
        for (const QJsonValue &v : o["vantagePoints"].toArray()) { const QJsonObject q = v.toObject(); a.vp << QPointF(q["lat"].toDouble(), q["lon"].toDouble()); a.vpDbm << q["dbm"].toInt(); a.vpDev << q["device"].toString(); }
        m_lastRefit = a; m_haveRefit = true;
    }
    a.durationMs = a.type == QLatin1String("ap_new") ? 2500 : a.type == QLatin1String("ap_lost") ? 2000 : a.type == QLatin1String("ap_refit") ? 3200
                 : a.type == QLatin1String("ap_up") || a.type == QLatin1String("ap_down") ? 3000
                 : a.type == QLatin1String("ap_placed") ? 1600 : a.type == QLatin1String("fix") ? 1800
                 : a.type == QLatin1String("stop") ? 1400 : 4000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - a.start < a.durationMs + 500) {                  // only animate what is actually fresh
        m_anims.append(a);
        while (m_anims.size() > 24) m_anims.removeFirst();
        if (a.type == QLatin1String("ap_new")) m_labelBorn.insert(a.bssid, now);
    }
    if (now - a.start < 60000 || m_ticker.size() < 5) {
        m_ticker.prepend(a);
        while (m_ticker.size() > 5) m_ticker.removeLast();
    }
    update();
}

void BeaconView::drawEvents(QPainter &p)
{
    if (m_anims.isEmpty() && m_labelBorn.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const Fix &fix = m_loc->fix();
    const QPointF me = toScreen(fix.lat, fix.lon);
    const double mpp = metersPerPixel(fix.lat);
    for (auto it = m_labelBorn.begin(); it != m_labelBorn.end();) { if (now - it.value() > 1500) it = m_labelBorn.erase(it); else ++it; }

    QFont small = font(); small.setPointSizeF(font().pointSizeF() * 0.85); small.setBold(true);
    QFont emoji = font(); emoji.setPointSizeF(font().pointSizeF() * 1.3);
    double toastY = -1;                                        // stacked under the HUD box
    for (int k = 0; k < m_anims.size();) {
        Anim &a = m_anims[k];
        const double t = double(now - a.start) / a.durationMs;
        if (t >= 1) { m_anims.removeAt(k); continue; }
        ++k;
        // Where on screen? Live marker if we still hear it, else the event's own position.
        QPointF pos; bool have = beaconScreenPos(a.bssid, &pos) && !a.bssid.isEmpty();
        if (!have && a.hasPos) {
            if (a.r > 0 && a.type != QLatin1String("ap_placed")) pos = offsetM(toScreen(a.lat, a.lon), a.bearing, a.r, mpp);
            else pos = toScreen(a.lat, a.lon);
            have = true;
        }
        const bool onScreen = have && rect().adjusted(-60, -60, 60, 60).contains(pos.toPoint());
        if (a.type == QLatin1String("ap_new") && onScreen) {
            const double rr = 6 + 40 * easeOut(t);
            QColor c = a.col; c.setAlphaF((1 - t) * 0.9);
            p.setPen(QPen(c, 2)); p.setBrush(Qt::NoBrush); p.drawEllipse(pos, rr, rr);
            c.setAlphaF((1 - t) * 0.35); p.setPen(QPen(c, 1)); p.drawEllipse(pos, rr * 0.6, rr * 0.6);
        } else if (a.type == QLatin1String("ap_lost") && onScreen) {
            const double s = 1 - easeOut(t);
            QColor c = a.col; c.setAlphaF(s);
            p.setPen(QPen(c, 1.5, Qt::DotLine)); p.setBrush(Qt::NoBrush); p.drawEllipse(pos, 14 * s, 14 * s);
            c.setAlphaF(s * 0.9); p.setPen(Qt::NoPen); p.setBrush(c); p.drawEllipse(pos, 4 * s, 4 * s);
            if (t < 0.6) { p.setFont(small); p.setPen(c); p.drawText(QRectF(pos.x() - 80, pos.y() - 30 - 10 * t, 160, 16), Qt::AlignCenter, a.ssid.isEmpty() ? QStringLiteral("(hidden) faded") : a.ssid + QStringLiteral(" faded")); }
        } else if ((a.type == QLatin1String("ap_up") || a.type == QLatin1String("ap_down")) && onScreen) {
            const bool up = a.delta > 0;
            const double pulse = 0.5 + 0.5 * std::sin(t * M_PI * 6);
            QColor c = a.col; c.setAlphaF((1 - t * t) * (0.55 + 0.45 * pulse));
            const QPointF at(pos.x() + 11, pos.y() - 6 - (up ? 4 * pulse : -4 * pulse));
            QPainterPath tri;
            if (up) { tri.moveTo(at.x(), at.y() - 6); tri.lineTo(at.x() + 5, at.y() + 3); tri.lineTo(at.x() - 5, at.y() + 3); }
            else    { tri.moveTo(at.x(), at.y() + 6); tri.lineTo(at.x() + 5, at.y() - 3); tri.lineTo(at.x() - 5, at.y() - 3); }
            tri.closeSubpath();
            p.setPen(Qt::NoPen); p.setBrush(c); p.drawPath(tri);
            p.setFont(small); p.setPen(c);
            p.drawText(QRectF(at.x() + 7, at.y() - 9, 70, 18), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("%1%2 dB").arg(a.delta > 0 ? QStringLiteral("+") : QString()).arg(a.delta));
        } else if (a.type == QLatin1String("ap_refit") && a.hasPos) {
            drawRefit(p, a, t, mpp);
        } else if (a.type == QLatin1String("ap_placed") && a.hasPos) {
            const QPointF to = toScreen(a.lat, a.lon);
            const QPointF from = a.hasFrom ? (a.r > 0 ? offsetM(toScreen(a.fromLat, a.fromLon), a.bearing, a.r, mpp) : toScreen(a.fromLat, a.fromLon)) : me;
            const double e = easeOut(t);
            const QPointF at = from + (to - from) * e;
            QColor trail = C_LOCATED; trail.setAlphaF(0.35 * (1 - t));
            p.setPen(QPen(trail, 1.5, Qt::DashLine)); p.drawLine(from, at);
            glowDot(p, at, 4 + 2 * (1 - t), C_LOCATED, 4);
            if (t > 0.7) {                                     // gold flash on arrival
                const double f = (t - 0.7) / 0.3;
                QColor c = C_LOCATED; c.setAlphaF(1 - f);
                p.setPen(QPen(c, 2)); p.setBrush(Qt::NoBrush); p.drawEllipse(to, 8 + 26 * f, 8 + 26 * f);
            }
        } else if (a.type == QLatin1String("fix") && a.hasPos) {
            const QPointF to = toScreen(a.lat, a.lon);
            if (a.hasFrom) {                                   // an eased dashed arrow draws itself from the old spot
                const QPointF from = toScreen(a.fromLat, a.fromLon);
                const double e = easeOut(t);
                const QPointF head = from + (to - from) * e;
                QColor c = C_ME; c.setAlphaF(0.9 * (1 - t * 0.6));
                p.setPen(QPen(c, 2.2, Qt::DashLine, Qt::RoundCap)); p.drawLine(from, head);
                const QLineF l(from, head);
                if (l.length() > 8) {
                    const double ang = std::atan2(-(head.y() - from.y()), head.x() - from.x());
                    QPainterPath ah; ah.moveTo(head);
                    ah.lineTo(head + QPointF(-std::cos(ang - 0.5) * 12, std::sin(ang - 0.5) * 12));
                    ah.lineTo(head + QPointF(-std::cos(ang + 0.5) * 12, std::sin(ang + 0.5) * 12));
                    ah.closeSubpath(); p.setPen(Qt::NoPen); p.setBrush(c); p.drawPath(ah);
                }
            }
            const double burst = easeOut(t);                   // halo burst at the fix
            QColor c = C_ME; c.setAlphaF(1 - burst);
            p.setPen(QPen(c, 3 * (1 - burst) + 0.5)); p.setBrush(Qt::NoBrush); p.drawEllipse(to, 10 + 60 * burst, 10 + 60 * burst);
        } else if (a.type == QLatin1String("stop") && a.hasPos) {
            const QPointF to = toScreen(a.lat, a.lon);
            const double drop = easeBounce(t);
            const QPointF at(to.x(), to.y() - 48 * (1 - drop));
            QColor sh(0, 0, 0, int(90 * drop)); p.setPen(Qt::NoPen); p.setBrush(sh); p.drawEllipse(to + QPointF(0, 2), 6 * drop + 2, 3 * drop + 1);
            QPainterPath pin;                                  // teardrop
            pin.moveTo(at.x(), at.y());
            pin.cubicTo(at.x() - 12, at.y() - 14, at.x() - 9, at.y() - 30, at.x(), at.y() - 30);
            pin.cubicTo(at.x() + 9, at.y() - 30, at.x() + 12, at.y() - 14, at.x(), at.y());
            p.setPen(QPen(Qt::white, 1.2)); p.setBrush(C_LOCATED); p.drawPath(pin);
            p.setBrush(QColor(8, 12, 20)); p.setPen(Qt::NoPen); p.drawEllipse(QPointF(at.x(), at.y() - 20), 4, 4);
        } else if (a.type == QLatin1String("achievement") || a.type == QLatin1String("region") || a.type == QLatin1String("prefetch") || a.type == QLatin1String("error")) {
            // Toast under the HUD, top-left; fades over the last second
            const double alpha = t > 0.75 ? (1 - t) / 0.25 : 1.0;
            const double slide = (1 - easeOut(qMin(1.0, t * 5))) * -16;
            p.setFont(small);
            const QString text = p.fontMetrics().elidedText(a.text, Qt::ElideRight, width() - 140);
            const double w = p.fontMetrics().horizontalAdvance(text) + 46, hgt = p.fontMetrics().height() + 14;
            if (toastY < 0) toastY = m_hudBottom + 8;         // just below the HUD panel
            const QRectF box(12 + slide, toastY, w, hgt);
            QColor bg = C_PANEL; bg.setAlphaF(0.85 * alpha);
            QColor edge = a.col; edge.setAlphaF(0.8 * alpha);
            p.setPen(QPen(edge, 1.2)); p.setBrush(bg); p.drawRoundedRect(box, 8, 8);
            p.setFont(emoji); p.setPen(QColor(255, 255, 255, int(255 * alpha)));
            p.drawText(QRectF(box.left() + 8, box.top(), 26, box.height()), Qt::AlignCenter, a.glyph);
            p.setFont(small); QColor tc = C_TEXT; tc.setAlphaF(alpha); p.setPen(tc);
            p.drawText(QRectF(box.left() + 38, box.top(), w - 44, box.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
            toastY += hgt + 6;
        }
    }
    p.setFont(font());
}

// Bottom-left ticker: the last five events, newest on top, each fading out over a
// minute. Hovering it pauses the clock so you can read.
void BeaconView::drawTicker(QPainter &p)
{
    if (m_ticker.isEmpty()) { m_tickerRect = QRectF(); return; }
    const qint64 now = m_tickerHover && m_tickerPausedAt ? m_tickerPausedAt : QDateTime::currentMSecsSinceEpoch();
    QFont small = font(); small.setPointSizeF(font().pointSizeF() * 0.82);
    const QFontMetricsF fm(small);
    const double lh = fm.height() + 6, wmax = qMin(360.0, width() * 0.45);
    struct Row { const Anim *a; double alpha; QString text, age; };
    QList<Row> rows;
    for (const Anim &a : m_ticker) {
        const double ageS = (now - a.start) / 1000.0;
        double alpha = ageS < 45 ? 1.0 : ageS < 60 ? (60 - ageS) / 15.0 : 0.0;
        if (m_tickerHover) alpha = qMax(alpha, 0.85);
        if (alpha <= 0.02) continue;
        const QString age = ageS < 5 ? QStringLiteral("now") : ageS < 60 ? QStringLiteral("%1 s").arg(int(ageS)) : QStringLiteral("%1 min").arg(int(ageS / 60));
        rows.append({&a, alpha, fm.elidedText(a.text, Qt::ElideRight, int(wmax - 70)), age});
    }
    if (rows.isEmpty()) { m_tickerRect = QRectF(); return; }
    const double h = rows.size() * lh + 10;
    const QRectF box(12, height() - 44 - h, wmax, h);
    m_tickerRect = box;
    p.setFont(small);
    QColor bg = C_PANEL; bg.setAlphaF(0.78);
    p.setPen(QPen(QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 60), 1)); p.setBrush(bg); p.drawRoundedRect(box, 8, 8);
    double y = box.top() + 5;
    for (const Row &r : rows) {
        QColor gc = r.a->col; gc.setAlphaF(r.alpha);
        QColor tc = C_TEXT; tc.setAlphaF(0.92 * r.alpha);
        QColor ac = C_DIM; ac.setAlphaF(0.8 * r.alpha);
        p.setPen(gc); p.drawText(QRectF(box.left() + 8, y, 20, lh), Qt::AlignCenter, r.a->glyph);
        p.setPen(tc); p.drawText(QRectF(box.left() + 30, y, wmax - 82, lh), Qt::AlignLeft | Qt::AlignVCenter, r.text);
        p.setPen(ac); p.drawText(QRectF(box.right() - 50, y, 44, lh), Qt::AlignRight | Qt::AlignVCenter, r.age);
        y += lh;
    }
    p.setFont(font());
}

void BeaconView::drawHud(QPainter &p)
{
    const Stats &st = stats();
    const Fix &fix = m_loc->fix();
    QFont base = font();
    QFont title = base; title.setPointSizeF(base.pointSizeF() * 1.25); title.setBold(true);
    QFont small = base; small.setPointSizeF(base.pointSizeF() * 0.85);

    QStringList lines;
    lines << st.rank.toUpper();
    lines << QStringLiteral("Lv %1 · %2 beacons logged%3").arg(st.rankLevel).arg(st.beaconsTotal)
             .arg(st.nextRankAt ? QStringLiteral(" · next rank at %1").arg(st.nextRankAt) : QString());
    lines << QStringLiteral("In range %1 · used %2 · located %3 · with you %4").arg(st.beaconsNow).arg(st.usedNow).arg(st.locatedNow).arg(st.travellingNow);
    if (st.moving && st.speedKmh >= 0) lines << QStringLiteral("Moving ~%1 km/h %2 · today %3 km").arg(qRound(st.speedKmh)).arg(Locator::compass(st.headingDeg)).arg(st.distanceTodayKm, 0, 'f', 1);
    else if (st.dwellSecs > 0) lines << QStringLiteral("Here %1 · trip %2 km · %3 stops").arg(Locator::durationText(st.dwellSecs)).arg(st.distanceTripKm, 0, 'f', 0).arg(st.stopsTrip);
    if (fix.valid && fix.hasElevation()) lines << QStringLiteral("Elevation %1 m · %2/%3 milestones").arg(qRound(fix.elevation)).arg(st.achievementsUnlocked).arg(st.achievementsTotal);
    if (fix.valid)
        lines << QStringLiteral("Fix ±%1 via %2").arg(distText(fix.accuracy), fix.source == QLatin1String("wifi") ? (fix.provider == QLatin1String("apple") ? QStringLiteral("Apple") : QStringLiteral("BeaconDB"))
                                                        : fix.source == QLatin1String("starlink") ? QStringLiteral("Starlink GPS") : QStringLiteral("IP (coarse)"));
    int shownPois = 0;
    for (const Poi &pt : m_loc->pois()) if (!m_hiddenCats.contains(pt.cat)) ++shownPois;
    QString places = m_loc->poisLoading() ? m_loc->poiNote()
                   : QStringLiteral("%1 places within %2 km").arg(shownPois).arg(m_loc->poiRadiusKm());
    if (!m_loc->poisLoading() && !m_loc->poiNote().isEmpty()) places += QStringLiteral(" · ") + m_loc->poiNote();
    lines << places;

    p.setFont(title);
    double w = p.fontMetrics().horizontalAdvance(lines[0]);
    p.setFont(small);
    for (int i = 1; i < lines.size(); ++i) w = qMax(w, double(p.fontMetrics().horizontalAdvance(lines[i])));
    w = qMin(w, width() - 90.0);
    const double lh = p.fontMetrics().height();
    const double th = QFontMetrics(title).height();
    QRectF box(12, 12, w + 28, th + lh * (lines.size() - 1) + 26);
    p.setPen(QPen(QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 90), 1));
    p.setBrush(C_PANEL);
    p.drawRoundedRect(box, 8, 8);
    m_hudBottom = box.bottom();
    if (st.nextRankAt) {                                       // XP bar toward the next rank
        const int prevAt = st.rankAt;
        const double frac = qBound(0.0, double(st.beaconsTotal - prevAt) / double(qMax(1, st.nextRankAt - prevAt)), 1.0);
        QRectF bar(box.left() + 14, box.bottom() - 8, box.width() - 28, 3);
        p.setPen(Qt::NoPen); p.setBrush(QColor(255, 255, 255, 40)); p.drawRoundedRect(bar, 1.5, 1.5);
        p.setBrush(C_LOCATED); p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * frac, bar.height()), 1.5, 1.5);
    }
    double y = box.top() + 10;
    p.setFont(title); p.setPen(C_LOCATED);
    p.drawText(QRectF(box.left() + 14, y, box.width(), th), Qt::AlignLeft | Qt::AlignVCenter, lines[0]);
    y += th;
    p.setFont(small);
    for (int i = 1; i < lines.size(); ++i) {
        p.setPen(i == lines.size() - 1 && (m_loc->poisLoading() || !m_loc->poiNote().isEmpty()) ? C_LOCATED : C_TEXT);
        p.drawText(QRectF(box.left() + 14, y, box.width() - 20, lh), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(lines[i], Qt::ElideRight, int(box.width() - 24)));
        y += lh;
    }
    p.setFont(base);
}

void BeaconView::drawControls(QPainter &p)
{
    static const char *icons[] = {"zoom-in", "zoom-out", "mark-location", "map-flat", "view-filter"};
    static const char *fallback[] = {"+", "−", "◎", "▦", "📍"};
    const double s = 34, gap = 6, x = width() - 12 - s;
    for (int b = 0; b < BtnCount; ++b) {
        const double y = 12 + b * (s + gap) + (b >= BtnLayers ? 10 : 0);
        const QRectF r(x, y, s, s);
        const int hitIdx = m_hits.size();
        const bool hot = m_hover == hitIdx;
        const bool on = b == BtnLocate && m_follow;
        p.setPen(QPen(on ? C_ME : QColor(C_ME.red(), C_ME.green(), C_ME.blue(), hot ? 180 : 80), on ? 1.6 : 1));
        p.setBrush(hot ? QColor(24, 34, 52, 235) : C_PANEL);
        p.drawRoundedRect(r, 8, 8);
        static const QList<QIcon> themed = [] { QList<QIcon> l; for (const char *n : icons) l << QIcon::fromTheme(QString::fromLatin1(n)); return l; }();
        const QIcon &ic = themed[b];
        if (!ic.isNull()) ic.paint(&p, r.adjusted(8, 8, -8, -8).toRect());
        else { p.setPen(C_TEXT); p.drawText(r, Qt::AlignCenter, QString::fromUtf8(fallback[b])); }
        m_hits.append({HitButton, r.center(), s / 2, {}, b});
    }
}

void BeaconView::drawScale(QPainter &p)
{
    const Fix &fix = m_loc->fix();
    double lat, lon; unmerc(m_center, &lat, &lon);
    Q_UNUSED(fix);
    const double mpp = metersPerPixel(lat);
    const double maxPx = 110;
    static const double steps[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000};
    double m = steps[0];
    for (double s : steps) if (s / mpp <= maxPx) m = s;
    const double px = m / mpp;
    QFont f = font(); f.setPointSizeF(f.pointSizeF() * 0.8); p.setFont(f);
    // metric and US units side by side: pinpointing is done in both (1 ft = 30.48 cm)
    const QString metric = m >= 1000 ? QStringLiteral("%1 km").arg(m / 1000) : m >= 1 ? QStringLiteral("%1 m").arg(m) : QStringLiteral("%1 cm").arg(qRound(m * 100));
    const QString us = m >= 1609.344 ? QStringLiteral("%1 mi").arg(m / 1609.344, 0, 'f', m >= 16093 ? 0 : 1)
                     : m >= 0.3048 ? QStringLiteral("%1 ft").arg(qRound(m / 0.3048 * 10) / 10.0) : QStringLiteral("%1 in").arg(m / 0.0254, 0, 'f', 1);
    const QString t = metric + QStringLiteral(" · ") + us;
    const QRectF box(12, height() - 34, px + 20 + p.fontMetrics().horizontalAdvance(t) + 8, 22);
    p.setPen(Qt::NoPen); p.setBrush(C_PANEL); p.drawRoundedRect(box, 5, 5);
    const double y = box.center().y() + 3, x0 = box.left() + 10;
    p.setPen(QPen(C_TEXT, 1.6));
    p.drawLine(QPointF(x0, y), QPointF(x0 + px, y));
    p.drawLine(QPointF(x0, y - 5), QPointF(x0, y)); p.drawLine(QPointF(x0 + px, y - 5), QPointF(x0 + px, y));
    p.drawText(QRectF(x0 + px + 6, box.top(), 200, box.height()), Qt::AlignLeft | Qt::AlignVCenter, t);
    p.setFont(font());
}

// What each tile source asks for (checked 2026-10; Esri's from the services' copyrightText). The
// places and cameras are OpenStreetMap data too, so OSM is credited on every layer.
void BeaconView::drawAttribution(QPainter &p)
{
    QFont f = font(); f.setPointSizeF(f.pointSizeF() * 0.75); p.setFont(f);
    const QString satLabels = QStringLiteral(" · Labels: Esri, HERE, Garmin · © OpenStreetMap contributors");
    QString src = m_layer == Satellite && m_satSource == TileSource::SatUsgsNaip ? QStringLiteral("Imagery: USGS The National Map (public domain)") + satLabels
                : m_layer == Satellite && m_satSource == TileSource::SatNasaViirs ? QStringLiteral("Imagery: NASA EOSDIS GIBS, VIIRS NOAA-20, %1").arg(QDateTime::currentDateTimeUtc().addDays(-1).date().toString(Qt::ISODate)) + satLabels
                : m_layer == Satellite ? QStringLiteral("Powered by Esri · Imagery: Esri, Vantor, Earthstar Geographics, GIS User Community · Labels: Esri, HERE, Garmin · © OpenStreetMap contributors")
                : m_layer == Topo ? QStringLiteral("Map data: © OpenStreetMap contributors, SRTM · Map style: © OpenTopoMap (CC-BY-SA)")
                : QStringLiteral("© OpenStreetMap contributors");
    if (m_baseTopo) src += QStringLiteral(" · offline: © OpenTopoMap (CC-BY-SA)");
    // the camera layer's data: DeFlock (OpenStreetMap-derived, ODbL) and flocklocations.com community reports (CC BY 4.0)
    if (m_showFlockCameras && !m_flockCameras.isEmpty()) src += QStringLiteral(" · Cameras: DeFlock / OSM (ODbL), flocklocations.com (CC BY 4.0)");
    const QString attr = QStringLiteral("%1 · z%2").arg(src).arg(m_zoom, 0, 'f', 1);
    const int maxW = qMax(120, width() - 24);                  // wraps rather than running off a narrow window
    const QRect tr = p.fontMetrics().boundingRect(QRect(0, 0, maxW, 400), Qt::TextWordWrap | Qt::AlignRight, attr).adjusted(-6, -2, 6, 2);
    const QRect ab(width() - tr.width() - 8, height() - tr.height() - 8, tr.width(), tr.height());
    p.setPen(Qt::NoPen); p.setBrush(QColor(8, 12, 20, 170)); p.drawRoundedRect(ab, 4, 4);
    p.setPen(QColor(C_TEXT.red(), C_TEXT.green(), C_TEXT.blue(), 170)); p.drawText(ab.adjusted(6, 2, -6, -2), Qt::AlignRight | Qt::AlignVCenter | Qt::TextWordWrap, attr);
    p.setFont(font());
}

QString BeaconView::beaconCard(int i) const
{
    const AccessPoint &ap = m_loc->accessPoints()[i];
    const ApEstimate e = m_loc->estimateFor(ap);
    const QString st = m_loc->apStatus(ap);
    const Fix &fix = m_loc->fix();
    const QString band = ap.frequency >= 5925 ? QStringLiteral("6 GHz") : ap.frequency >= 4900 ? QStringLiteral("5 GHz") : QStringLiteral("2.4 GHz");
    const QString how = e.kind == ApEstimate::Wigle ? QStringLiteral("WiGLE position · %1 %2 from you")
                                                          .arg(distText(Locator::distanceM(fix.lat, fix.lon, e.lat, e.lon)), compass(Locator::bearingDeg(fix.lat, fix.lon, e.lat, e.lon)))
                      : e.kind == ApEstimate::Trilat ? QStringLiteral("Fitted from %1 of your samples at %2 places, ±%3 (%4) · %5 %6 from you").arg(e.fit.n).arg(e.fit.vantage).arg(distText(e.radiusM), e.fit.quality)
                                                          .arg(distText(Locator::distanceM(fix.lat, fix.lon, e.lat, e.lon)), compass(Locator::bearingDeg(fix.lat, fix.lon, e.lat, e.lon)))
                      : e.kind == ApEstimate::Peer ? QStringLiteral("Positioned by a synced device, ±%1 · %2 %3 from you").arg(distText(e.radiusM))
                                                          .arg(distText(Locator::distanceM(fix.lat, fix.lon, e.lat, e.lon)), compass(Locator::bearingDeg(fix.lat, fix.lon, e.lat, e.lon)))
                      : e.kind == ApEstimate::Centroid ? QStringLiteral("Multilaterated from %1 places, ±%2 · %3 %4 from you").arg(e.vantage).arg(distText(e.radiusM))
                                                          .arg(distText(Locator::distanceM(fix.lat, fix.lon, e.lat, e.lon)), compass(Locator::bearingDeg(fix.lat, fix.lon, e.lat, e.lon)))
                      : e.kind == ApEstimate::Observed ? QStringLiteral("Heard here before (internal map), ±%1 · %2 %3 from you").arg(distText(e.radiusM))
                                                          .arg(distText(Locator::distanceM(fix.lat, fix.lon, e.lat, e.lon)), compass(Locator::bearingDeg(fix.lat, fix.lon, e.lat, e.lon)))
                      : e.kind == ApEstimate::Region ? QStringLiteral("Only an area so far: within %1 of this centre (%2 samples) · %3 %4 from you").arg(distText(e.radiusM)).arg(e.fit.n)
                                                          .arg(distText(Locator::distanceM(fix.lat, fix.lon, e.lat, e.lon)), compass(Locator::bearingDeg(fix.lat, fix.lon, e.lat, e.lon)))
                      : e.kind == ApEstimate::Mobile ? QStringLiteral("Heard in too many places to pin down · shown where it was last heard")
                      : QStringLiteral("~%1 away by signal — direction unknown").arg(distText(e.radiusM));
    // The graded lead ("B · 72% within 25 m · 9 places · 3 devices") and what the grade is wary of
    QString lead = gradeLead(e);
    if (!lead.isEmpty() && e.kind == ApEstimate::Trilat)
        lead += QStringLiteral(" · ±%1 (95 %)").arg(distText(e.fit.r95));
    QStringList fl;
    for (const QJsonValue &v : Estimator::flags(e.fit)) {
        const QString k = v.toString();
        fl << (k == QLatin1String("extrapolated") ? QStringLiteral("outside the places heard from")
               : k == QLatin1String("ambiguous") ? QStringLiteral("a mirror position fits about as well")
               : k == QLatin1String("moved") ? QStringLiteral("moved — only recent samples used")
               : k == QLatin1String("fragile") ? QStringLiteral("rests on one or two places")
               : k == QLatin1String("rangeScale") ? QStringLiteral("no close sample, distance scale uncertain") : k);
    }
    if (!lead.isEmpty() && !fl.isEmpty() && e.kind != ApEstimate::Mobile) lead += QStringLiteral("\n⚠ ") + fl.join(QStringLiteral(" · "));
    if (!lead.isEmpty() && e.fit.suggestGain > 0 && e.kind != ApEstimate::Mobile) lead += QStringLiteral("\n⌖ One more sample at the crosshair would help most");
    const QString stText = st == QLatin1String("used") ? QStringLiteral("used for the fix") : st == QLatin1String("home") ? QStringLiteral("home network · the RV") : st == QLatin1String("active") ? QStringLiteral("connected · travels with you")
                         : st == QLatin1String("travelling") ? QStringLiteral("travels with you") : st == QLatin1String("nomap") ? QStringLiteral("opted out (_nomap)") : QStringLiteral("ignored");
    const QString sec = ap.security.isEmpty() ? QString()
                      : QStringLiteral("\n🔒 %1%2%3").arg(ap.security.toUpper(), ap.adhoc ? QStringLiteral(" · ad-hoc") : QString(),
                                                    AccessPoint::insecure(ap.security) ? QStringLiteral("  ⚠ insecure — traffic can be read or the network joined by anyone nearby") : QString());
    return QStringLiteral("📶 %1\n%9%2 · %3 dBm · %4 (%5 MHz)%8\n%6\n%7")
        .arg(ap.ssid.isEmpty() ? QStringLiteral("(hidden network)") : ap.ssid, ap.bssid).arg(ap.dbm).arg(band).arg(ap.frequency).arg(stText, how, sec, lead.isEmpty() ? QString() : lead + QStringLiteral("\n"));
}

QString BeaconView::poiCard(int i) const
{
    const Poi &pt = m_loc->pois()[i];
    const PoiCategory *c = Locator::poiCategory(pt.cat);
    const Fix &fix = m_loc->fix();
    QString s = QStringLiteral("%1 %2\n%3 · %4 %5").arg(c ? c->icon : QString(), pt.name.isEmpty() ? (c ? c->label : pt.cat) : pt.name,
                                                        c ? c->label : pt.cat, distText(Locator::distanceM(fix.lat, fix.lon, pt.lat, pt.lon)),
                                                        compass(Locator::bearingDeg(fix.lat, fix.lon, pt.lat, pt.lon)));
    if (fix.source == QLatin1String("ip")) s += QStringLiteral(" (from IP estimate)");
    if (!pt.detail.isEmpty()) s += QStringLiteral("\n") + pt.detail;
    if (pt.driveS > 0) {
        const int min = pt.driveS / 60;
        s += QStringLiteral("\n🚗 ~%1 drive%2").arg(min < 60 ? QStringLiteral("%1 min").arg(min) : min % 60 ? QStringLiteral("%1 h %2 min").arg(min / 60).arg(min % 60) : QStringLiteral("%1 h").arg(min / 60),
                                                    pt.driveEst ? QStringLiteral(" (est.)") : QString());
    }
    if (!pt.address.isEmpty()) s += QStringLiteral("\n") + pt.address;
    if (!pt.hours.isEmpty()) s += QStringLiteral("\n🕑 ") + pt.hours;
    if (!pt.phone.isEmpty()) s += QStringLiteral("\n☎ ") + pt.phone;
    s += QStringLiteral("\nRight-click for directions & more");
    return s;
}

void BeaconView::drawCard(QPainter &p)
{
    QString text; QPointF anchor; QColor accent = C_ME, leadCol;
    auto beaconAccent = [this, &accent, &leadCol](int i) {
        accent = C_LOCATED;
        const ApEstimate e = m_loc->estimateFor(m_loc->accessPoints()[i]);
        if (!gradeLead(e).isEmpty()) {
            accent = leadCol = gradeColor(e.fit.grade.isEmpty() ? QStringLiteral("M") : e.fit.grade);
            if (leadCol.lightnessF() < 0.45) leadCol = leadCol.lighter(160);   // M blue: readable on the dark card
        }
    };
    const Hit *h = m_hover >= 0 && m_hover < m_hits.size() ? &m_hits[m_hover] : nullptr;
    if (h && h->kind == HitButton) {
        static const char *tips[] = {"Zoom in (+)", "Zoom out (−)", "Follow my position (0)", "Map style", "Places to show"};
        text = QString::fromLatin1(tips[h->button]); anchor = h->pos - QPointF(20, 0);
    } else if (h && h->kind == HitPoi) {
        text = poiCard(h->items.first()); anchor = h->pos;
        if (const PoiCategory *c = Locator::poiCategory(m_loc->pois()[h->items.first()].cat)) accent = c->color;
    } else if (h && h->kind == HitBeacon && h->items.size() == 1) {
        text = beaconCard(h->items.first()); anchor = h->pos; beaconAccent(h->items.first());
    } else if (h && h->kind == HitCamera && !h->items.isEmpty()) {
        text = cameraCard(h->items.first()); anchor = h->pos;
        if (h->items.first() >= 0 && h->items.first() < m_flockCameras.size())
            accent = m_flockCameras[h->items.first()].vetted ? QColor(0, 220, 255) : QColor(255, 175, 40);
    } else if (h && (h->kind == HitCluster || h->kind == HitBeacon)) {
        QStringList l;
        const bool poi = h->kind == HitCluster;
        l << (poi ? QStringLiteral("%1 places — click to zoom in").arg(h->items.size()) : QStringLiteral("%1 beacons here").arg(h->items.size()));
        QList<int> ids = h->items;
        if (!poi) std::sort(ids.begin(), ids.end(), [this](int a, int b) { return m_loc->accessPoints()[a].dbm > m_loc->accessPoints()[b].dbm; });
        for (int k = 0; k < qMin(10, ids.size()); ++k) {
            if (poi) {
                const Poi &pt = m_loc->pois()[ids[k]];
                const PoiCategory *c = Locator::poiCategory(pt.cat);
                l << QStringLiteral("%1 %2").arg(c ? c->icon : QString(), pt.name.isEmpty() ? (c ? c->label : pt.cat) : pt.name);
            } else {
                const AccessPoint &ap = m_loc->accessPoints()[ids[k]];
                l << QStringLiteral("%1  %2 dBm · %3").arg(ap.ssid.isEmpty() ? QStringLiteral("(hidden)") : ap.ssid).arg(ap.dbm).arg(m_loc->apStatus(ap));
            }
        }
        if (ids.size() > 10) l << QStringLiteral("… and %1 more").arg(ids.size() - 10);
        text = l.join('\n'); anchor = h->pos;
    } else if (m_selKind == HitPoi && m_selItem >= 0 && m_selItem < m_poiPos.size() && !m_poiPos[m_selItem].isNull()) {
        text = poiCard(m_selItem); anchor = m_poiPos[m_selItem];
        if (const PoiCategory *c = Locator::poiCategory(m_loc->pois()[m_selItem].cat)) accent = c->color;
    } else if (m_selKind == HitBeacon && m_selItem >= 0 && m_selItem < m_beaconPos.size() && !m_beaconPos[m_selItem].isNull()) {
        text = beaconCard(m_selItem); anchor = m_beaconPos[m_selItem]; beaconAccent(m_selItem);
    } else if (m_selKind == HitCamera && m_cameraPos.contains(m_selItem)) {
        text = cameraCard(m_selItem); anchor = m_cameraPos.value(m_selItem);
        if (m_selItem >= 0 && m_selItem < m_flockCameras.size())
            accent = m_flockCameras[m_selItem].vetted ? QColor(0, 220, 255) : QColor(255, 175, 40);
    }
    if (text.isEmpty()) return;

    const QStringList lines = text.split('\n');
    QFont title = font(); title.setBold(true);
    QFont small = font(); small.setPointSizeF(font().pointSizeF() * 0.85);
    const QFontMetrics tf(title), sf(small);
    double w = tf.horizontalAdvance(lines[0]);
    for (int i = 1; i < lines.size(); ++i) w = qMax(w, double(sf.horizontalAdvance(lines[i])));
    w = qMin(w + 24, 380.0);
    const double hgt = tf.height() + sf.height() * (lines.size() - 1) + 16;
    QPointF tl(anchor.x() + 18, anchor.y() - hgt / 2);
    if (tl.x() + w > width() - 8) tl.setX(anchor.x() - 18 - w);
    tl.setY(qBound(8.0, tl.y(), height() - hgt - 8));
    tl.setX(qBound(8.0, tl.x(), width() - w - 8));
    const QRectF box(tl, QSizeF(w, hgt));
    p.setPen(QPen(QColor(accent.red(), accent.green(), accent.blue(), 160), 1.2));
    p.setBrush(QColor(8, 12, 20, 240));
    p.drawRoundedRect(box, 7, 7);
    double y = box.top() + 8;
    p.setFont(title); p.setPen(Qt::white);
    p.drawText(QRectF(box.left() + 12, y, w - 20, tf.height()), Qt::AlignLeft | Qt::AlignVCenter, tf.elidedText(lines[0], Qt::ElideRight, int(w - 20)));
    y += tf.height();
    p.setFont(small);
    for (int i = 1; i < lines.size(); ++i) {
        p.setPen(i == 1 && leadCol.isValid() ? leadCol : i == lines.size() - 1 && lines[i].startsWith(QLatin1String("Right-click")) ? C_DIM : C_TEXT);
        p.drawText(QRectF(box.left() + 12, y, w - 20, sf.height()), Qt::AlignLeft | Qt::AlignVCenter, sf.elidedText(lines[i], Qt::ElideRight, int(w - 20)));
        y += sf.height();
    }
    p.setFont(font());
}

// ── Interaction ──────────────────────────────────────────────────────────────
int BeaconView::hitAt(const QPointF &pos) const
{
    for (int i = 0; i < m_hits.size(); ++i)             // buttons win
        if (m_hits[i].kind == HitButton && QLineF(m_hits[i].pos, pos).length() <= m_hits[i].radius + 2) return i;
    int best = -1; double bestD = 1e9;
    for (int i = 0; i < m_hits.size(); ++i) {
        const double d = QLineF(m_hits[i].pos, pos).length();
        if (d <= m_hits[i].radius + 5 && d < bestD) { bestD = d; best = i; }
    }
    return best;
}

void BeaconView::wheelEvent(QWheelEvent *e)
{
    const double steps = e->angleDelta().y() / 120.0;
    if (steps != 0) zoomAt(steps * 0.5, e->position());
    e->accept();
}

void BeaconView::keyPressEvent(QKeyEvent *e)
{
    const QPointF c(width() / 2.0, height() / 2.0);
    const double pan = 120.0 / worldPx();
    switch (e->key()) {
    case Qt::Key_Plus: case Qt::Key_Equal: zoomAt(1, c); break;
    case Qt::Key_Minus: zoomAt(-1, c); break;
    case Qt::Key_0: case Qt::Key_Home: recenter(); break;
    case Qt::Key_Left:  m_center.rx() -= pan; m_follow = false; update(); break;
    case Qt::Key_Right: m_center.rx() += pan; m_follow = false; update(); break;
    case Qt::Key_Up:    m_center.ry() -= pan; m_follow = false; update(); break;
    case Qt::Key_Down:  m_center.ry() += pan; m_follow = false; update(); break;
    case Qt::Key_Escape: m_selKind = HitNone; update(); break;
    default: QWidget::keyPressEvent(e);
    }
}

void BeaconView::setShowDevices(bool on)
{
    m_showDevices = on; QSettings().setValue("map/showDevices", on); update();
}

void BeaconView::setShowLegend(bool on)
{
    m_showLegend = on; QSettings().setValue("map/showLegend", on); update();
}

// Bottom right, above the attribution: what the ellipse colours and strokes mean
void BeaconView::drawLegend(QPainter &p)
{
    QFont small = font(); small.setPointSizeF(font().pointSizeF() * 0.78);
    QFont bold = small; bold.setBold(true);
    const QFontMetricsF fm(small);
    const double lh = fm.height() + 5, chip = fm.height() + 2;
    const QStringList notes{QStringLiteral("R  region only (disc = R95)"), QStringLiteral("M  travels with you"),
                            QStringLiteral("ellipse: 95 % of the position"), QStringLiteral("dashed: extrapolated / ambiguous"),
                            QStringLiteral("○ ghost: the mirror position"), QStringLiteral("⌖ sample here next")};
    double w = 6 * (chip + 3) + 16;
    for (const QString &n : notes) w = qMax(w, fm.horizontalAdvance(n) + chip + 24);
    const double h = 10 + lh + notes.size() * lh + 4;
    const double attrH = fm.height() + 12;
    const QRectF box(width() - 12 - w, height() - attrH - 12 - h, w, h);
    p.setPen(QPen(QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 60), 1)); p.setBrush(QColor(8, 12, 20, 215)); p.drawRoundedRect(box, 8, 8);
    double x = box.left() + 8, y = box.top() + 6;
    p.setFont(bold);
    for (const QString &g : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D"), QStringLiteral("E"), QStringLiteral("F")}) {
        const QRectF c(x, y + (lh - chip) / 2, chip, chip);
        p.setPen(Qt::NoPen); p.setBrush(gradeColor(g)); p.drawRoundedRect(c, 3, 3);
        p.setPen(gradeTextColor(g)); p.drawText(c, Qt::AlignCenter, g);
        x += chip + 3;
    }
    p.setFont(small); p.setPen(C_DIM);
    p.drawText(QRectF(x + 4, y, box.right() - x - 8, lh), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("best → worst"));
    y += lh;
    for (int i = 0; i < notes.size(); ++i) {
        const QRectF c(box.left() + 8, y + (lh - chip) / 2, chip, chip);
        const QString g = i == 0 ? QStringLiteral("R") : i == 1 ? QStringLiteral("M") : QString();
        QString t = notes[i];
        if (!g.isEmpty()) {
            p.setPen(Qt::NoPen); p.setBrush(gradeColor(g)); p.drawRoundedRect(c, 3, 3);
            p.setFont(bold); p.setPen(gradeTextColor(g)); p.drawText(c, Qt::AlignCenter, g); p.setFont(small);
            t = t.mid(3);
        } else if (i == 2 || i == 3) {
            const QColor gc = gradeColor(QStringLiteral("B"));
            p.setPen(QPen(gc, 1.3, i == 3 ? Qt::DashLine : Qt::SolidLine)); QColor f = gc; f.setAlpha(34); p.setBrush(f);
            p.drawEllipse(c.center(), chip / 2 - 1, chip / 2 - 4);
        } else {
            p.setPen(C_TEXT); p.drawText(c, Qt::AlignCenter, t.left(1)); t = t.mid(2);
        }
        p.setPen(C_TEXT);
        p.drawText(QRectF(c.right() + 6, y, box.right() - c.right() - 10, lh), Qt::AlignLeft | Qt::AlignVCenter, t);
        y += lh;
    }
    p.setFont(font());
}

void BeaconView::setShowImported(bool on)
{
    m_showImported = on; QSettings().setValue("map/showImported", on); update();
}

void BeaconView::setShowHeatmap(bool on)
{
    if (m_showHeatmap == on) return;
    m_showHeatmap = on;
    QSettings().setValue("map/showHeatmap", on);
    if (on) m_routeFixesDirty = true;
    update();
}

void BeaconView::setShowFlockCameras(bool on)
{
    if (m_showFlockCameras == on) return;
    m_showFlockCameras = on;
    QSettings().setValue("map/showFlockCameras", on);
    m_camStale = true;
    if (!on) {                                                 // paint stops drawing them, so nothing else drops the pinned card
        m_cameraPos.clear();
        if (m_selKind == HitCamera) m_selKind = HitNone;
    }
    update();
}

void BeaconView::setShowApCircles(bool on)
{
    if (m_showApCircles == on) return;
    m_showApCircles = on;
    QSettings().setValue("map/showApCircles", on);
    update();
}

// The route heat map. Rasterising thousands of fixes took most of a second, so it never happens
// in a paint: startHeat() renders an area a quarter larger than the view on every side on m_pool,
// and this draws the latest result moved and scaled to the view, asking for a fresh one when the
// data, the zoom or the coverage no longer match.
void BeaconView::drawHeatmap(QPainter &p)
{
    if (m_routeFixes.size() < 2) return;
    const double ws = TILE * std::pow(2.0, m_heat.zoom);
    const QRectF have(m_heat.tl, QSizeF(m_heat.img.width() / ws, m_heat.img.height() / ws));
    if (m_heat.img.isNull() || m_heat.version != m_heatVersion || std::abs(m_heat.zoom - m_zoom) > 1e-6
        || !have.contains(QRectF(toMerc(QPointF(0, 0)), toMerc(QPointF(width(), height())))))
        if (!m_heatKick.isActive()) m_heatKick.start();
    if (m_heat.img.isNull()) return;
    const double scale = std::pow(2.0, m_zoom - m_heat.zoom);
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, std::abs(scale - 1) > 1e-6);
    p.drawImage(QRectF(toScreen(m_heat.tl), QSizeF(m_heat.img.width() * scale, m_heat.img.height() * scale)), m_heat.img);
    p.restore();
}

void BeaconView::loadRouteFixes()
{
    if (!m_routeFixesDirty) return;
    if (m_routeLoadedAt.isValid() && m_routeLoadedAt.elapsed() < 30000) return;   // a live fix every few seconds: twice a minute is plenty
    QElapsedTimer t; t.start();
    m_routeFixes = m_loc->allRouteFixes();
    if (qEnvironmentVariableIsSet("BEACONFIX_PAINT_PROFILE")) fprintf(stderr, "route fixes: %d in %.1f ms\n", int(m_routeFixes.size()), t.nsecsElapsed() / 1e6);
    m_routeFixesDirty = false; m_routeLoadedAt.start();
    ++m_heatVersion;
}

void BeaconView::startHeat()
{
    if (m_heatBusy || !m_showHeatmap || m_routeFixes.size() < 2 || width() < 2) return;   // a busy worker looks again when it lands
    Heat job;
    job.zoom = m_zoom; job.version = m_heatVersion;
    job.tl = toMerc(QPointF(-width() * 0.25, -height() * 0.25));
    const QSize sz(qRound(width() * 1.5), qRound(height() * 1.5));
    m_heatBusy = true;
    m_pool.start([this, job, fixes = m_routeFixes, sz]() mutable {
        QElapsedTimer t; t.start();
        job.img = renderHeat(fixes, job.tl, job.zoom, sz);
        if (qEnvironmentVariableIsSet("BEACONFIX_PAINT_PROFILE")) fprintf(stderr, "heat %dx%d from %d fixes: %.1f ms (worker)\n", sz.width(), sz.height(), int(fixes.size()), t.nsecsElapsed() / 1e6);
        QMetaObject::invokeMethod(this, [this, job] {
            m_heatBusy = false; m_heat = job; ++m_heatSerial;
            update();                                          // the base redraws with it, and asks again if the view has moved on
        }, Qt::QueuedConnection);
    });
}

// Adds a round-capped, antialiased stroke (half-width hw, value v) to an Alpha8 image with saturation:
// what QPainter's CompositionMode_Plus would leave, at a fraction of the stroker's cost (it spent
// ~0.3 s on a few thousand route segments).
static void addStroke(QImage &img, const QPointF &a, const QPointF &b, double hw, int v)
{
    const int W = img.width(), H = img.height();
    const double dx = b.x() - a.x(), dy = b.y() - a.y(), len2 = dx * dx + dy * dy, m = hw + 1;
    const int y0 = qMax(0, int(std::floor(qMin(a.y(), b.y()) - m))), y1 = qMin(H - 1, int(std::ceil(qMax(a.y(), b.y()) + m)));
    for (int y = y0; y <= y1; ++y) {
        const double py = y + 0.5;
        double xa = qMin(a.x(), b.x()), xb = qMax(a.x(), b.x());
        if (std::abs(dy) > 1e-9) {                             // the stretch of the segment within reach of this row
            const double t0 = qBound(0.0, (py - m - a.y()) / dy, 1.0), t1 = qBound(0.0, (py + m - a.y()) / dy, 1.0);
            xa = qMin(a.x() + dx * t0, a.x() + dx * t1); xb = qMax(a.x() + dx * t0, a.x() + dx * t1);
        }
        const int x0 = qMax(0, int(std::floor(xa - m))), x1 = qMin(W - 1, int(std::ceil(xb + m)));
        uchar *row = img.scanLine(y);
        for (int x = x0; x <= x1; ++x) {
            const double px = x + 0.5;
            const double t = len2 > 0 ? qBound(0.0, ((px - a.x()) * dx + (py - a.y()) * dy) / len2, 1.0) : 0.0;
            const double ex = a.x() + t * dx - px, ey = a.y() + t * dy - py;
            const double cov = hw + 0.5 - std::sqrt(ex * ex + ey * ey);
            if (cov > 0) row[x] = uchar(qMin(255, row[x] + int(v * qMin(cov, 1.0) + 0.5)));
        }
    }
}

// Thread-safe: plain data in, an image out. tl = mercator of the image's top-left corner.
QImage BeaconView::renderHeat(const QList<Fix> &fixes, const QPointF &tl, double zoom, const QSize &sz)
{
    const double ws = TILE * std::pow(2.0, zoom);
    const double cx = tl.x() + sz.width() / 2.0 / ws;           // longitudes wrap around the image centre
    auto px = [&](const Fix &f) {
        const QPointF m = merc(f.lat, f.lon);
        double dx = m.x() - cx;
        if (dx > 0.5) dx -= 1.0; else if (dx < -0.5) dx += 1.0;
        return QPointF((cx + dx - tl.x()) * ws, (m.y() - tl.y()) * ws);
    };
    const QRectF vp = QRectF(QPointF(0, 0), QSizeF(sz)).adjusted(-60, -60, 60, 60);
    QImage alphaMap(sz, QImage::Format_Alpha8);
    alphaMap.fill(0);
    QList<QPoint> dots;
    {
        QPointF prevPt;
        QDateTime prevTime;
        double prevLat = 0, prevLon = 0;
        bool hasPrev = false;
        for (const Fix &f : fixes) {
            if (!f.valid) { hasPrev = false; continue; }
            const QPointF pt = px(f);
            const bool inVp = vp.contains(pt);
            if (hasPrev) {
                const qint64 dt = prevTime.isValid() && f.time.isValid() ? std::abs(prevTime.secsTo(f.time)) : 0;
                const double d = Locator::distanceM(prevLat, prevLon, f.lat, f.lon);
                const bool timeOk = dt > 0 && dt <= 900;
                const bool speedOk = dt == 0 || (d / dt) <= 38.0;
                QPointF a = prevPt, b = pt;
                if (timeOk && speedOk && d <= 4000.0 && (inVp || vp.contains(prevPt)) && clipLine(a, b, vp)) addStroke(alphaMap, a, b, 4.0, 20);
            }
            if (inVp) dots << pt.toPoint();
            prevPt = pt; prevTime = f.time; prevLat = f.lat; prevLon = f.lon;
            hasPrev = true;
        }
    }
    // Each fix's glow (35 at the centre, 15 at 0.6 r, nothing at the rim) is one precomputed stamp
    // added with saturation, as CompositionMode_Plus would, instead of a QRadialGradient per fix
    const double r = qBound(12.0, 30.0 - zoom * 0.5, 28.0);
    const int R = int(std::ceil(r)), side = 2 * R + 1, W = sz.width(), H = sz.height();
    QList<uchar> stamp(side * side);
    for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx) {
            const double d = std::hypot(dx, dy) / r;
            stamp[(dy + R) * side + dx + R] = uchar(qRound(d < 0.6 ? 35 - 20 * d / 0.6 : d < 1 ? 15 * (1 - d) / 0.4 : 0.0));
        }
    for (const QPoint &c : dots) {
        const int x0 = qMax(0, c.x() - R), x1 = qMin(W - 1, c.x() + R);
        for (int y = qMax(0, c.y() - R); y <= qMin(H - 1, c.y() + R); ++y) {
            uchar *row = alphaMap.scanLine(y);
            const uchar *k = stamp.constData() + (y - c.y() + R) * side + (x0 - c.x() + R);
            for (int x = x0; x <= x1; ++x, ++k) row[x] = uchar(qMin(255, row[x] + *k));
        }
    }

    static const QVector<QRgb> kRamp = []{
        QVector<QRgb> r(256);
        for (int i = 0; i < 256; ++i) {
            if (i == 0) {
                r[i] = 0;
            } else if (i < 40) {
                const double t = i / 40.0;
                r[i] = qRgba(0, int(80 * t), int(220 * t + 35), int(140 * t));
            } else if (i < 100) {
                const double t = (i - 40) / 60.0;
                r[i] = qRgba(int(40 * t), int(80 * (1 - t) + 210 * t), int(255 * (1 - t) + 120 * t), int(140 + 40 * t));
            } else if (i < 170) {
                const double t = (i - 100) / 70.0;
                r[i] = qRgba(int(40 * (1 - t) + 255 * t), int(210 * (1 - t) + 230 * t), int(120 * (1 - t)), int(180 + 35 * t));
            } else {
                const double t = (i - 170) / 85.0;
                r[i] = qRgba(255, int(230 * (1 - t) + 70 * t), int(200 * t * t), int(215 + 40 * t));
            }
            r[i] = qPremultiply(r[i]);                         // the image is premultiplied: straight colour above alpha wraps when blended
        }
        return r;
    }();

    QImage colorized(sz, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < H; ++y) {
        const uchar *src = alphaMap.constScanLine(y);
        QRgb *dst = reinterpret_cast<QRgb *>(colorized.scanLine(y));
        for (int x = 0; x < W; ++x) dst[x] = kRamp[src[x]];
    }
    return colorized;
}

const Stats &BeaconView::stats()
{
    if (!m_statsAt.isValid() || m_statsAt.elapsed() >= 1000) { m_stats = m_loc->stats(); m_statsAt.start(); }   // walks every stop: not per frame
    return m_stats;
}

// Surveillance cameras: ~140 k after a flocklocations sync, so the view holds only those around
// what's on screen (the view plus half again on every side, nearest first, capped), reloaded
// when the view leaves that box. Zoomed out past CAM_MIN_ZOOM they're not shown at all.
static const double CAM_MIN_ZOOM = 9;
static const int CAM_CAP = 5000;

void BeaconView::setFlockCameras(const QList<FlockCamera> &cams)
{
    const QString sel = m_selKind == HitCamera && m_selItem >= 0 && m_selItem < m_flockCameras.size() ? m_flockCameras[m_selItem].id : QString();
    m_flockCameras = cams;
    m_cameraMerc.resize(cams.size());
    for (int i = 0; i < cams.size(); ++i) m_cameraMerc[i] = merc(cams[i].lat, cams[i].lon);
    m_cameraPos.clear();
    if (!sel.isEmpty()) {                                      // the pinned card follows its camera into the new list
        m_selItem = -1;
        for (int i = 0; i < cams.size() && m_selItem < 0; ++i) if (cams[i].id == sel) m_selItem = i;
        if (m_selItem < 0) m_selKind = HitNone;
    }
}

void BeaconView::loadFlockCameras()
{
    if (!m_showFlockCameras || m_zoom < CAM_MIN_ZOOM || width() < 2) return;
    double latN, lonW, latS, lonE;
    unmerc(toMerc(QPointF(-width() * 0.5, -height() * 0.5)), &latN, &lonW);
    unmerc(toMerc(QPointF(width() * 1.5, height() * 1.5)), &latS, &lonE);
    lonW = qMax(-180.0, lonW); lonE = qMin(180.0, lonE);
    QElapsedTimer t; t.start();
    const QList<FlockCamera> cams = m_loc->flockCamerasIn(latS, latN, lonW, lonE, CAM_CAP);
    if (qEnvironmentVariableIsSet("BEACONFIX_PAINT_PROFILE")) fprintf(stderr, "cameras: %d around the view in %.1f ms\n", int(cams.size()), t.nsecsElapsed() / 1e6);
    m_camBox = QRectF(QPointF(lonW, latS), QPointF(lonE, latN));
    m_camZoom = m_zoom; m_camCapped = cams.size() >= CAM_CAP; m_camStale = false;
    // Capped, the rows are the nearest to the box centre (MapDb's metric): a disc out to the farthest one, not the box
    m_camCentre = m_camBox.center(); m_camK = std::cos(qDegreesToRadians(m_camCentre.y())); m_camR2 = 0;
    if (m_camCapped)
        for (const FlockCamera &c : cams) {
            const double dy = c.lat - m_camCentre.y(), dx = (c.lon - m_camCentre.x()) * m_camK;
            m_camR2 = qMax(m_camR2, dy * dy + dx * dx);
        }
    setFlockCameras(cams);
    update();
}

void BeaconView::drawFlockCameras(QPainter &p)
{
    m_cameraPos.clear();
    if (!m_showFlockCameras || m_zoom < CAM_MIN_ZOOM) return;
    double latN, lonW, latS, lonE;
    unmerc(toMerc(QPointF(0, 0)), &latN, &lonW);
    unmerc(toMerc(QPointF(width(), height())), &latS, &lonE);
    const QRectF view(QPointF(qMax(-180.0, lonW), latS), QPointF(qMin(180.0, lonE), latN));
    bool reload = m_camStale || !m_camBox.contains(view) || (m_camCapped && m_zoom > m_camZoom + 0.9);
    if (!reload && m_camCapped) {                              // a corner past the disc has cameras not loaded; when the disc
        const auto d2 = [this](QPointF ll) {                   // can't hold the view at all, only once the centre is r/4 off
            const double dy = ll.y() - m_camCentre.y(), dx = (ll.x() - m_camCentre.x()) * m_camK;
            return dy * dy + dx * dx;
        };
        const bool out = d2(view.topLeft()) > m_camR2 || d2(view.topRight()) > m_camR2
                      || d2(view.bottomLeft()) > m_camR2 || d2(view.bottomRight()) > m_camR2;
        reload = out && d2(view.center()) > m_camR2 / 16;   // (each load is ~50 ms on this thread)
    }
    if (reload && !m_camKick.isActive()) m_camKick.start();
    if (m_flockCameras.isEmpty()) return;

    const QRectF vp = rect().adjusted(-40, -40, 40, 40);
    // Zoomed out, thousands can share the screen: plain dots, one per few pixels, no cones
    const bool simple = m_zoom < 13;
    const double cell = m_zoom < 11 ? 6 : simple ? 4 : 0;
    const int gw = cell > 0 ? int(vp.width() / cell) + 1 : 0, gh = cell > 0 ? int(vp.height() / cell) + 1 : 0;
    QList<bool> taken(gw * gh, false);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    for (int i = 0; i < m_flockCameras.size(); ++i) {
        const FlockCamera &c = m_flockCameras[i];
        const QPointF at = toScreen(m_cameraMerc[i]);
        if (!vp.contains(at)) continue;
        const bool vetted = c.vetted;
        const QColor mainCol = vetted ? QColor(0, 220, 255) : QColor(255, 175, 40);
        if (cell > 0) {
            const int g = int((at.y() - vp.top()) / cell) * gw + int((at.x() - vp.left()) / cell);
            if (taken[g]) continue;
            taken[g] = true;
        }
        if (!m_paintClip.intersects(QRectF(at.x() - 26, at.y() - 26, 52, 52))) {   // outside this repaint: hit only
            m_cameraPos.insert(i, at);
            m_hits.append({HitCamera, at, simple ? 4.0 : 12.0, {i}});
            continue;
        }
        if (simple) {
            p.setPen(Qt::NoPen); p.setBrush(mainCol); p.drawEllipse(at, 2.6, 2.6);
            m_cameraPos.insert(i, at);
            m_hits.append({HitCamera, at, 4, {i}});
            continue;
        }
        const QColor glowCol = vetted ? QColor(0, 220, 255, 60) : QColor(255, 175, 40, 50);

        if (!c.direction.isEmpty()) {
            bool ok = false;
            double heading = c.direction.toDouble(&ok);
            if (!ok) {
                const QString d = c.direction.trimmed().toUpper();
                if (d == QLatin1String("N") || d == QLatin1String("NB")) { heading = 0; ok = true; }
                else if (d == QLatin1String("NE")) { heading = 45; ok = true; }
                else if (d == QLatin1String("E") || d == QLatin1String("EB")) { heading = 90; ok = true; }
                else if (d == QLatin1String("SE")) { heading = 135; ok = true; }
                else if (d == QLatin1String("S") || d == QLatin1String("SB")) { heading = 180; ok = true; }
                else if (d == QLatin1String("SW")) { heading = 225; ok = true; }
                else if (d == QLatin1String("W") || d == QLatin1String("WB")) { heading = 270; ok = true; }
                else if (d == QLatin1String("NW")) { heading = 315; ok = true; }
            }
            if (ok) {
                const double rad = (heading - 90.0) * M_PI / 180.0;
                const double spread = 25.0 * M_PI / 180.0;
                const double len = 24.0;
                QPainterPath cone;
                cone.moveTo(at);
                cone.lineTo(at.x() + len * std::cos(rad - spread), at.y() + len * std::sin(rad - spread));
                cone.arcTo(QRectF(at.x() - len, at.y() - len, len * 2, len * 2), -heading + 90 - 25, 50);
                cone.closeSubpath();
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(mainCol.red(), mainCol.green(), mainCol.blue(), 45));
                p.drawPath(cone);
            }
        }

        const double r = 8.0;
        p.setPen(Qt::NoPen);
        p.setBrush(glowCol);
        p.drawEllipse(at, r + 4, r + 4);

        p.setPen(QPen(Qt::white, 1.2));
        p.setBrush(mainCol);
        p.drawEllipse(at, r, r);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(15, 20, 30));
        p.drawEllipse(at, 3.2, 3.2);

        if (vetted) {
            p.setPen(QPen(QColor(0, 255, 180), 1.5));
            p.drawPoint(at);
        }

        m_cameraPos.insert(i, at);
        m_hits.append({HitCamera, at, r + 4, {i}});
    }

    p.restore();
}

QString BeaconView::cameraCard(int camIndex) const
{
    if (camIndex < 0 || camIndex >= m_flockCameras.size()) return {};
    const FlockCamera &c = m_flockCameras[camIndex];
    QStringList lines;
    // only what the source says (docs/SIGHTINGS.md §2.0): no invented "Flock Safety" / "Falcon"
    const QString what = !c.model.isEmpty() ? c.model : !c.manufacturer.isEmpty() ? QStringLiteral("%1 %2").arg(c.manufacturer, PlateEvents::typeLabel(c.cameraType))
                                                                                  : PlateEvents::typeLabel(c.cameraType);
    lines << QStringLiteral("%1 · %2").arg(what, c.operatorName.isEmpty() ? QStringLiteral("operator unknown") : c.operatorName);
    if (!c.model.isEmpty() && !c.manufacturer.isEmpty()) lines << QStringLiteral("Made by %1").arg(c.manufacturer);
    lines << QStringLiteral("Status: %1").arg(c.stale ? QStringLiteral("No longer listed by its source (kept for your passes)")
                                             : c.vetted ? QStringLiteral("Field Vetted (Active RF)") : QStringLiteral("Candidate Location"));
    if (c.osmVersion > 0) lines << QStringLiteral("OpenStreetMap v%1 · %2").arg(c.osmVersion).arg(c.osmTimestamp.left(10));
    if (!c.direction.isEmpty()) lines << QStringLiteral("Direction: %1").arg(c.direction);
    if (!c.bssid.isEmpty()) lines << QStringLiteral("Wi-Fi BSSID: %1").arg(c.bssid);
    if (!c.bleMac.isEmpty()) lines << QStringLiteral("BLE MAC: %1").arg(c.bleMac);
    if (c.sightingCount > 1) lines << QStringLiteral("Sightings: %1 · Last: %2").arg(c.sightingCount).arg(c.lastSeen.toString(QStringLiteral("yyyy-MM-dd hh:mm")));
    if (!c.notes.isEmpty()) lines << c.notes;
    return lines.join('\n');
}

void BeaconView::replayLastRefit()
{
    if (!m_haveRefit) return;
    Anim a = m_lastRefit; a.start = QDateTime::currentMSecsSinceEpoch();
    m_anims.append(a); while (m_anims.size() > 24) m_anims.removeFirst();
    if (!m_anim.isActive()) m_anim.start();
    update();
}

QColor BeaconView::deviceColor(const QString &device)
{
    if (device.isEmpty()) return C_ME;
    uint h = 2166136261u; for (const QChar &c : device) { h ^= c.unicode(); h *= 16777619u; }
    return QColor::fromHsvF((h % 360) / 360.0, 0.75, 1.0);
}

// Linked devices: 📱 / 💻 / 🖥 + name + age, dashed line to us with the distance when it is close
void BeaconView::drawDevices(QPainter &p)
{
    const QList<DevicePos> devs = m_loc->devicePositions();
    const Fix &fix = m_loc->fix();
    const QPointF me = toScreen(fix.lat, fix.lon);
    const double mpp = metersPerPixel(fix.valid ? fix.lat : 0);
    QFont emoji = font(); emoji.setPointSizeF(font().pointSizeF() * 1.4);
    QFont small = font(); small.setPointSizeF(font().pointSizeF() * 0.85); small.setBold(true);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // Measured ranges (RTT / BLE / differential Wi-Fi): a ring around us at the distance, the band is the 16–84 % interval
    QList<QJsonObject> close;
    QSet<QString> ranged;
    for (auto it = m_ranges.constBegin(); it != m_ranges.constEnd(); ++it) {
        const QJsonObject &r = it.value();
        const QDateTime upd = QDateTime::fromString(r["updated"].toString(), Qt::ISODateWithMs);
        if (!fix.valid || !r["distanceM"].isDouble() || !upd.isValid() || upd.secsTo(QDateTime::currentDateTime()) > 600) continue;
        ranged.insert(it.key());
        const double D = r["distanceM"].toDouble(), lo = r["lowM"].toDouble(D), hi = r["highM"].toDouble(D);
        const QColor col = deviceColor(it.key());
        if (D / mpp < 8) { close << r; continue; }
        QColor band = col; band.setAlphaF(0.14);
        QPainterPath ring; ring.addEllipse(me, hi / mpp, hi / mpp); ring.addEllipse(me, lo / mpp, lo / mpp);
        p.setPen(Qt::NoPen); p.setBrush(band); p.drawPath(ring);
        p.setPen(QPen(col, 1.6, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawEllipse(me, D / mpp, D / mpp);
        QPointF lab = me + QPointF(0, D / mpp + 20);                  // below the ring: the inset sits above-right
        if (r["lat"].isDouble()) {                                  // bearing known: the device itself sits on the ring
            const QPointF at = toScreen(r["lat"].toDouble(), r["lon"].toDouble());
            glowDot(p, at, 6, col, 3); lab = at;
        }
        p.setFont(small); p.setPen(col);
        p.drawText(QRectF(lab.x() - 120, lab.y() - 18, 240, 14), Qt::AlignCenter,
                   QStringLiteral("%1 · %2 measured (%3–%4) · %5").arg(it.key(), distText(D), distText(lo), distText(hi), r["class"].toString()));
    }
    drawRangeInset(p, close);
    if (devs.isEmpty()) return;
    for (const DevicePos &d : devs) {
        if (!d.time.isValid()) continue;
        const QPointF at = toScreen(d.lat, d.lon);
        if (!rect().adjusted(-120, -120, 120, 120).contains(at.toPoint())) continue;
        const QColor col = deviceColor(d.device);
        if (fix.valid) {
            const double dist = Locator::distanceM(fix.lat, fix.lon, d.lat, d.lon);
            if (dist < 2000 && dist > 3 && !ranged.contains(d.device)) {   // fix-to-fix only when nothing was measured
                QColor c = col; c.setAlphaF(d.online ? 0.7 : 0.35);
                p.setPen(QPen(c, 1.3, Qt::DashLine)); p.drawLine(me, at);
                p.setFont(small); p.setPen(c);
                p.drawText(QRectF((me + at) / 2 - QPointF(50, 18), QSizeF(100, 14)), Qt::AlignCenter, distText(dist));
            }
        }
        if (d.acc > 0) { QColor c = col; c.setAlphaF(0.10); p.setPen(QPen(col, 1, Qt::DotLine)); p.setBrush(c); p.drawEllipse(at, d.acc / mpp, d.acc / mpp); }
        p.setFont(emoji); p.setPen(Qt::NoPen);
        QColor fill = col; fill.setAlphaF(d.online ? 0.95 : 0.45);
        p.setBrush(fill); p.drawEllipse(at, 12, 12);
        p.setPen(Qt::black);
        p.drawText(QRectF(at.x() - 12, at.y() - 12, 24, 24), Qt::AlignCenter, d.kind == QLatin1String("android") ? QStringLiteral("📱") : d.kind == QLatin1String("desktop") ? QStringLiteral("🖥") : QStringLiteral("💻"));
        p.setFont(small); p.setPen(d.online ? C_TEXT : C_DIM);
        const qint64 age = now - d.time.toSecsSinceEpoch();
        const QString ageText = age < 90 ? QStringLiteral("now") : age < 3600 ? QStringLiteral("%1 min").arg(age / 60) : age < 86400 ? QStringLiteral("%1 h").arg(age / 3600) : QStringLiteral("%1 d").arg(age / 86400);
        p.drawText(QRectF(at.x() - 90, at.y() + 13, 180, 16), Qt::AlignCenter, QStringLiteral("%1 · %2%3").arg(d.device, ageText, d.online ? QString() : QStringLiteral(" · offline")));
    }
}

// The refit animation (3.2 s): vantage points pop in, dashed range rings expand from each to its
// distance to the new position (so they visibly intersect at the answer), the marker glides from the
// old estimate along a dashed trail while its uncertainty circle shrinks, then a crosshair locks on,
// pulses twice and the "±38 m · 24 samples" label rises. Rings are coloured by the device that heard it.
void BeaconView::drawRefit(QPainter &p, const Anim &a, double t, double mpp)
{
    const QPointF to = toScreen(a.lat, a.lon), from = a.hasFrom ? toScreen(a.fromLat, a.fromLon) : to;
    QFont small = font(); small.setPointSizeF(font().pointSizeF() * 0.85); small.setBold(true);
    // 1. vantage points pop in (0 – 0.25), each with its ring growing (0.15 – 0.65)
    for (int i = 0; i < a.vp.size(); ++i) {
        const double pop = qBound(0.0, (t - 0.04 * i) / 0.2, 1.0);
        if (pop <= 0) continue;
        const QPointF v = toScreen(a.vp[i].x(), a.vp[i].y());
        const QColor col = deviceColor(a.vpDev.value(i));
        const double s = easeOut(pop) * 7;
        QPainterPath tri; tri.moveTo(v.x(), v.y() - s); tri.lineTo(v.x() + s * 0.9, v.y() + s * 0.6); tri.lineTo(v.x() - s * 0.9, v.y() + s * 0.6); tri.closeSubpath();
        QColor fc = col; fc.setAlphaF(0.9 * (t < 0.85 ? 1.0 : (1 - t) / 0.15));
        p.setPen(QPen(Qt::white, 1)); p.setBrush(fc); p.drawPath(tri);
        const double ring = qBound(0.0, (t - 0.15 - 0.03 * i) / 0.5, 1.0);
        if (ring > 0) {
            const double full = Locator::distanceM(a.vp[i].x(), a.vp[i].y(), a.lat, a.lon) / mpp;
            const double r = full * easeOut(ring);
            QColor rc = col; rc.setAlphaF((0.85 - 0.5 * ring) * (t < 0.85 ? 1.0 : (1 - t) / 0.15));
            p.setPen(QPen(rc, 1.4, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawEllipse(v, r, r);
            if (ring < 1) { p.setFont(small); p.setPen(rc); p.drawText(QRectF(v.x() + 8, v.y() - 8, 90, 14), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("%1 dBm").arg(a.vpDbm.value(i))); }
        }
    }
    // 2. marker glides old → new (0.3 – 0.7) while the uncertainty circle shrinks
    const double g = qBound(0.0, (t - 0.3) / 0.4, 1.0), e = easeOut(g);
    const QPointF at = from + (to - from) * e;
    if (a.hasFrom && g > 0 && g < 1.2) {
        QColor trail = C_LOCATED; trail.setAlphaF(0.5 * (1 - qMax(0.0, t - 0.7) / 0.3));
        p.setPen(QPen(trail, 1.5, Qt::DashLine)); p.drawLine(from, at);
    }
    const double accNow = (a.prevAcc + (a.acc - a.prevAcc) * e) / mpp;
    QColor ac = C_LOCATED; ac.setAlphaF(0.18); p.setPen(QPen(C_LOCATED, 1, Qt::DotLine)); p.setBrush(ac); p.drawEllipse(at, accNow, accNow);
    glowDot(p, at, 5, C_LOCATED, 4);
    // 3. lock: crosshair pulses twice (0.7 – 1.0), label rises, sparkle
    if (t > 0.7) {
        const double l = (t - 0.7) / 0.3;
        const double pulse = 0.5 + 0.5 * std::cos(l * M_PI * 4);
        QColor c = C_ACTIVE; c.setAlphaF(0.4 + 0.6 * pulse);
        p.setPen(QPen(c, 1.5)); p.setBrush(Qt::NoBrush);
        const double R = 14 + 6 * pulse;
        p.drawEllipse(at, R, R);
        for (int k = 0; k < 4; ++k) { const double ang = k * M_PI / 2; p.drawLine(at + QPointF(std::cos(ang) * (R - 5), std::sin(ang) * (R - 5)), at + QPointF(std::cos(ang) * (R + 6), std::sin(ang) * (R + 6))); }
        p.setFont(small); p.setPen(c);
        p.drawText(QRectF(at.x() - 90, at.y() - 34 - 10 * l, 180, 16), Qt::AlignCenter, QStringLiteral("±%1 m · %2 samples").arg(qRound(a.acc)).arg(a.n));
        if (l < 0.5) {                                          // sparkle
            const double sp = l / 0.5;
            QColor sc = Qt::white; sc.setAlphaF(1 - sp);
            p.setPen(QPen(sc, 1.2));
            for (int k = 0; k < 6; ++k) { const double ang = k * M_PI / 3 + sp; const double r0 = 6 + 10 * sp, r1 = r0 + 6 * (1 - sp); p.drawLine(at + QPointF(std::cos(ang) * r0, std::sin(ang) * r0), at + QPointF(std::cos(ang) * r1, std::sin(ang) * r1)); }
        }
    }
}

void BeaconView::showEvent(QShowEvent *) { m_anim.start(); if (m_autoZoom) fitBeacons(); }
void BeaconView::hideEvent(QHideEvent *) { m_anim.stop(); }
void BeaconView::leaveEvent(QEvent *) { m_hover = -1; m_tickerHover = false; m_tickerPausedAt = 0; update(); }
void BeaconView::resizeEvent(QResizeEvent *) { if (m_autoZoom) fitBeacons(); }

void BeaconView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    const int hi = hitAt(e->position());
    if (hi >= 0 && m_hits[hi].kind == HitAnchor) {           // drag an anchor to where it really is
        m_anchorDrag = m_hits[hi].items.first(); m_anchorMoved = false; m_anchorDragPos = e->position();
        m_dragStart = e->position();
        return;
    }
    m_dragging = true; m_dragMoved = false;
    m_dragStart = e->position(); m_dragCenter = m_center;
}

void BeaconView::mouseMoveEvent(QMouseEvent *e)
{
    if (m_anchorDrag >= 0) {
        if (!m_anchorMoved && (e->position() - m_dragStart).manhattanLength() < 4) return;
        m_anchorMoved = true; m_anchorDragPos = e->position();
        setCursor(Qt::ClosedHandCursor);
        update();
        return;
    }
    if (m_dragging) {
        const QPointF d = e->position() - m_dragStart;
        if (!m_dragMoved && d.manhattanLength() < 4) return;
        m_dragMoved = true;
        setCursor(Qt::ClosedHandCursor);
        const double ws = worldPx();
        m_center = QPointF(m_dragCenter.x() - d.x() / ws, qBound(0.0, m_dragCenter.y() - d.y() / ws, 1.0));
        m_follow = false;
        update();
        return;
    }
    const bool overTicker = m_tickerRect.isValid() && m_tickerRect.contains(e->position());
    if (overTicker != m_tickerHover) { m_tickerHover = overTicker; m_tickerPausedAt = overTicker ? QDateTime::currentMSecsSinceEpoch() : 0; update(); }
    const int h = hitAt(e->position());
    if (h != m_hover) {
        m_hover = h;
        setCursor(h >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void BeaconView::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_anchorDrag >= 0) {
        const int idx = m_anchorDrag; const bool moved = m_anchorMoved;
        m_anchorDrag = -1; m_anchorMoved = false; unsetCursor();
        const QList<BfAnchor> all = m_loc->anchors();
        if (idx >= all.size()) { update(); return; }
        if (!moved) { editAnchor(idx); return; }              // a click: edit it
        double lat, lon; unmerc(toMerc(e->position()), &lat, &lon);
        QJsonObject o = all[idx].toJson(false);
        o["lat"] = lat; o["lon"] = lon; o.remove(QStringLiteral("rvOffset"));
        o["placedBy"] = QStringLiteral("desktop"); o["source"] = QStringLiteral("map-pick");
        o["placedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        bool ok = false; QString err;
        m_loc->setAnchor(o, QStringLiteral("desktop"), &ok, &err);
        if (!ok) QMessageBox::warning(this, QStringLiteral("Anchor"), err);
        update();
        return;
    }
    const bool click = m_dragging && !m_dragMoved;
    m_dragging = false;
    unsetCursor();
    if (!click || e->button() != Qt::LeftButton) return;
    const int hi = hitAt(e->position());
    if (hi < 0) { m_selKind = HitNone; update(); return; }
    const Hit h = m_hits[hi];
    switch (h.kind) {
    case HitButton: buttonClicked(h.button, mapToGlobal(h.pos.toPoint() + QPoint(-120, 18))); break;
    case HitCluster: zoomAt(2, h.pos); break;
    case HitPoi:    m_selKind = HitPoi; m_selItem = h.items.first(); break;
    case HitBeacon:
        if (h.items.size() == 1) { m_selKind = HitBeacon; m_selItem = h.items.first(); }
        else if (m_zoom < 18) zoomAt(2, h.pos);
        break;
    case HitCamera: m_selKind = HitCamera; m_selItem = h.items.first(); break;
    case HitAnchor: break;
    case HitNone: break;
    }
    update();
}

void BeaconView::mouseDoubleClickEvent(QMouseEvent *e)
{
    const int hi = hitAt(e->position());
    if (hi >= 0 && m_hits[hi].kind == HitButton) { buttonClicked(m_hits[hi].button, QCursor::pos()); return; }
    zoomAt(1, e->position());
}

void BeaconView::buttonClicked(int b, const QPoint &globalPos)
{
    const QPointF c(width() / 2.0, height() / 2.0);
    switch (b) {
    case BtnZoomIn:  zoomAt(1, c); break;
    case BtnZoomOut: zoomAt(-1, c); break;
    case BtnLocate:  recenter(); break;
    case BtnLayers: {
        QMenu menu(this);
        auto *grp = new QActionGroup(&menu);
        const std::pair<Layer, const char *> layers[] = {{Dark, "Dark"}, {Streets, "Streets"}, {Satellite, "Satellite"}, {Topo, "Topographic"}};
        for (const auto &l : layers) {
            QAction *a = menu.addAction(QString::fromLatin1(l.second));
            a->setCheckable(true); a->setChecked(m_layer == l.first); grp->addAction(a);
            connect(a, &QAction::triggered, this, [this, l] { setLayer(l.first); });
        }
        // Free, keyless imagery (TileSource::SatSource): the newest high-res is Esri's; NASA's is yesterday's, coarse
        QMenu *sat = menu.addMenu(QIcon::fromTheme(QStringLiteral("internet-services")), QStringLiteral("Satellite imagery"));
        sat->setToolTipsVisible(true);
        auto *sgrp = new QActionGroup(sat);
        const std::pair<TileSource::SatSource, const char *> tips[] = {
            {TileSource::SatEsri, "Vantor/Maxar 30–50 cm: the newest high-resolution imagery served free"},
            {TileSource::SatEsriClarity, "The same imagery, sharper processing; can be an older capture"},
            {TileSource::SatUsgsNaip, "US only, public domain (USDA NAIP and others), to zoom 16"},
            {TileSource::SatNasaViirs, "Yesterday's global VIIRS pass (~375 m): the newest, but coarse — smoke, snow, clouds"}};
        for (const auto &t : tips) {
            QAction *a = sat->addAction(TileSource::satName(t.first));
            a->setToolTip(QString::fromUtf8(t.second));
            a->setCheckable(true); a->setChecked(m_layer == Satellite && m_satSource == int(t.first)); sgrp->addAction(a);
            connect(a, &QAction::triggered, this, [this, t] { setSatSource(int(t.first)); });
        }
        QAction *cont = menu.addAction(QStringLiteral("Contour lines over satellite"));
        cont->setToolTip(QStringLiteral("Elevation contours in feet, drawn from the free AWS Terrain Tiles (USGS 3DEP in the US)"));
        cont->setCheckable(true); cont->setChecked(m_showContours);
        connect(cont, &QAction::toggled, this, &BeaconView::setShowContours);
        menu.exec(globalPos);
        break;
    }
    case BtnPlaces: {
        QMenu menu(this);
        menu.setToolTipsVisible(true);
        QHash<QString, int> counts;
        for (const Poi &pt : m_loc->pois()) counts[pt.cat]++;
        QAction *all = menu.addAction(QStringLiteral("Show all"));
        QAction *none = menu.addAction(QStringLiteral("Hide all"));
        menu.addSeparator();
        for (const QString &grp : {QStringLiteral("civic"), QStringLiteral("kids"), QStringLiteral("services")}) {
            QMenu *gm = menu.addMenu(Locator::poiGroupLabel(grp));
            QAction *gAll = gm->addAction(QStringLiteral("Show all in this group"));
            QAction *gNone = gm->addAction(QStringLiteral("Hide all in this group"));
            connect(gAll, &QAction::triggered, this, [this, grp] { for (const PoiCategory &c : Locator::poiCategories()) if (c.group == grp) m_hiddenCats.remove(c.key); QSettings().setValue("map/hiddenCategories", QStringList(m_hiddenCats.begin(), m_hiddenCats.end())); update(); });
            connect(gNone, &QAction::triggered, this, [this, grp] { for (const PoiCategory &c : Locator::poiCategories()) if (c.group == grp) m_hiddenCats.insert(c.key); QSettings().setValue("map/hiddenCategories", QStringList(m_hiddenCats.begin(), m_hiddenCats.end())); update(); });
            gm->addSeparator();
            for (const PoiCategory &c : Locator::poiCategories()) {
                if (c.group != grp) continue;
                QAction *a = gm->addAction(QStringLiteral("%1  %2  (%3)").arg(c.icon, c.label).arg(counts.value(c.key)));
                a->setCheckable(true); a->setChecked(!m_hiddenCats.contains(c.key));
                const QString key = c.key;
                connect(a, &QAction::toggled, this, [this, key](bool on) { setCategoryVisible(key, on); });
            }
        }
        menu.addSeparator();
        QMenu *rad = menu.addMenu(QStringLiteral("Search radius: %1 km").arg(m_loc->poiRadiusKm()));
        for (int km : {2, 4, 6, 10, 15, 25}) {
            QAction *a = rad->addAction(QStringLiteral("%1 km").arg(km));
            a->setCheckable(true); a->setChecked(km == m_loc->poiRadiusKm());
            connect(a, &QAction::triggered, this, [this, km] { m_loc->setPoiRadiusKm(km); });
        }
        QMenu *peds = menu.addMenu(QStringLiteral("Pediatric ER search: %1 km").arg(m_loc->pedsRadiusKm()));
        peds->setToolTipsVisible(true);
        auto *pedsGrp = new QActionGroup(peds);
        pedsGrp->setExclusive(true);
        for (int km : {50, 100, 150, 200, 300}) {
            QAction *a = peds->addAction(QStringLiteral("%1 km").arg(km));
            a->setCheckable(true); a->setChecked(km == m_loc->pedsRadiusKm()); pedsGrp->addAction(a);
            a->setToolTip(QStringLiteral("How far to look for children's hospitals (a separate, rarer search)"));
            connect(a, &QAction::triggered, this, [this, km] { m_loc->setPedsRadiusKm(km); });
        }
        QAction *reload = menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Reload places"));
        QAction *chosen = menu.exec(globalPos);
        if (chosen == all || chosen == none) {
            m_hiddenCats.clear();
            if (chosen == none) for (const PoiCategory &c : Locator::poiCategories()) m_hiddenCats.insert(c.key);
            QSettings().setValue("map/hiddenCategories", QStringList(m_hiddenCats.begin(), m_hiddenCats.end()));
            update();
        } else if (chosen == reload) m_loc->RefreshPlaces();
        break;
    }
    }
}

void BeaconView::contextMenuEvent(QContextMenuEvent *e)
{
    const int hi = hitAt(e->pos());
    if (hi >= 0 && m_hits[hi].kind != HitButton) { showItemMenu(m_hits[hi], e->globalPos()); return; }
    double lat, lon; unmerc(toMerc(e->pos()), &lat, &lon);
    QMenu menu(this);
    const QString coords = QStringLiteral("%1, %2").arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6);
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy %1").arg(coords), this, [coords] { QApplication::clipboard()->setText(coords); });
    menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("Open here in OpenStreetMap"), this, [lat, lon] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=17/%1/%2").arg(lat).arg(lon))); });
    menu.addAction(QIcon::fromTheme(QStringLiteral("mark-location")), QStringLiteral("Centre here"), this, [this, lat, lon] { focusOn(lat, lon, m_zoom); });
    menu.addAction(QIcon::fromTheme(QStringLiteral("network-wireless")), QStringLiteral("Place an antenna here…"), this, [this, lat, lon] { placeAnchorAt(lat, lon); });
    {   // docs/SIGHTINGS.md §8
        QMenu *route = menu.addMenu(QIcon::fromTheme(QStringLiteral("routeplanning"), QIcon::fromTheme(QStringLiteral("go-jump"))), QStringLiteral("Route avoiding ALPRs"));
        route->addAction(QStringLiteral("To here (from %1)").arg(m_haveRouteStart ? QStringLiteral("the chosen start") : QStringLiteral("my position")), this,
                         [this, lat, lon] { routeAvoidingAlprs(lat, lon); });
        route->addAction(QStringLiteral("Start from here"), this, [this, lat, lon] { setRouteStart(lat, lon); });
        QAction *clr = route->addAction(QStringLiteral("Clear the route"), this, &BeaconView::clearAvoidRoute);
        clr->setEnabled(!m_avoidRoute.isEmpty() || m_haveRouteStart);
    }
    if (!m_inspectPlan.isEmpty()) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-clear")), QStringLiteral("Clear inspection plan"), this, &BeaconView::clearInspectPlan);
    }
    QAction *names = menu.addAction(QStringLiteral("Show Wi-Fi names"));
    names->setCheckable(true); names->setChecked(m_showNames);
    connect(names, &QAction::toggled, this, &BeaconView::setShowNames);
    QAction *devs = menu.addAction(QStringLiteral("Show my other devices"));
    devs->setCheckable(true); devs->setChecked(m_showDevices);
    connect(devs, &QAction::toggled, this, &BeaconView::setShowDevices);
    if (!m_loc->importedHistory().isEmpty()) {
        QAction *imp = menu.addAction(QStringLiteral("Show imported history"));
        imp->setCheckable(true); imp->setChecked(m_showImported);
        connect(imp, &QAction::toggled, this, &BeaconView::setShowImported);
    }
    QAction *legend = menu.addAction(QStringLiteral("Show grade legend"));
    legend->setCheckable(true); legend->setChecked(m_showLegend);
    connect(legend, &QAction::toggled, this, &BeaconView::setShowLegend);
    QAction *heat = menu.addAction(QStringLiteral("Show route heat map"));
    heat->setCheckable(true); heat->setChecked(m_showHeatmap);
    connect(heat, &QAction::toggled, this, &BeaconView::setShowHeatmap);
    QAction *cams = menu.addAction(QStringLiteral("Show surveillance cameras"));
    cams->setCheckable(true); cams->setChecked(m_showFlockCameras);
    connect(cams, &QAction::toggled, this, &BeaconView::setShowFlockCameras);
    QAction *apCirc = menu.addAction(QStringLiteral("Show beacon uncertainty circles"));
    apCirc->setCheckable(true); apCirc->setChecked(m_showApCircles);
    connect(apCirc, &QAction::toggled, this, &BeaconView::setShowApCircles);
    menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Refresh surveillance cameras"), this, [this] { m_loc->refreshFlockCameras(true); });
    QAction *replay = menu.addAction(QIcon::fromTheme(QStringLiteral("media-playback-start")), QStringLiteral("Replay last refit"));
    replay->setEnabled(m_haveRefit);
    connect(replay, &QAction::triggered, this, &BeaconView::replayLastRefit);
    if (m_loc->fix().valid) {
        menu.addSeparator();
        QMenu *share = menu.addMenu(QIcon::fromTheme(QStringLiteral("document-share")), QStringLiteral("Share my fix"));
        const std::pair<const char *, const char *> shares[] = {{"coords", "Copy coordinates"}, {"geo", "Copy geo: URI"}, {"text", "Copy place + link"},
                                                                {"osm", "Copy OpenStreetMap link"}, {"google", "Copy Google Maps link"}, {"apple", "Copy Apple Maps link"}};
        for (const auto &sh : shares) { const QString what = QString::fromLatin1(sh.first); share->addAction(QString::fromLatin1(sh.second), this, [this, what] { m_loc->CopyToClipboard(what); }); }
        // OSM and Esri tiles may not be saved ahead (their tile policies); TileSource::prefetch() saves OpenTopoMap,
        // which stands in for the chosen layer when it can't load
        QAction *save = menu.addAction(QIcon::fromTheme(QStringLiteral("document-save")), QStringLiteral("Save map around here for offline (topographic)"), m_loc, &Locator::PrefetchTiles);
        save->setToolTip(QStringLiteral("OpenStreetMap and Esri don't allow saving their tiles ahead of time. OpenTopoMap tiles are saved instead "
                                        "and shown when the chosen map can't load; areas you have viewed stay cached in any style."));
        menu.setToolTipsVisible(true);
    }
    menu.exec(e->globalPos());
}

void BeaconView::showItemMenu(const Hit &h, const QPoint &globalPos)
{
    QMenu menu(this);
    const Fix &fix = m_loc->fix();
    if (h.kind == HitCamera) {
        const int idx = h.items.first();
        if (idx < 0 || idx >= m_flockCameras.size()) return;
        const FlockCamera c = m_flockCameras[idx];
        const QString coords = QStringLiteral("%1, %2").arg(c.lat, 0, 'f', 6).arg(c.lon, 0, 'f', 6);
        menu.addSection(c.model.isEmpty() ? QStringLiteral("Flock Camera") : c.model);
        menu.addAction(QIcon::fromTheme(QStringLiteral("security-high")), QStringLiteral("Inspect unseen…"), this, [this, c] { inspectCamera(c.id); });
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy %1").arg(coords), this, [coords] { QApplication::clipboard()->setText(coords); });
        if (c.source == QLatin1String("osm") && c.id.startsWith(QLatin1String("osm:"))) {
            const QString osmPath = c.id.mid(4);
            menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("View on OpenStreetMap"), this, [osmPath] {
                QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.openstreetmap.org/%1").arg(osmPath)));
            });
        }
        if (!c.bssid.isEmpty())
            menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy BSSID %1").arg(c.bssid), this, [c] { QApplication::clipboard()->setText(c.bssid); });
        if (!c.bleMac.isEmpty())
            menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy BLE MAC %1").arg(c.bleMac), this, [c] { QApplication::clipboard()->setText(c.bleMac); });
        menu.exec(globalPos);
        return;
    }
    if (h.kind == HitAnchor) {
        const int idx = h.items.first();
        const QList<BfAnchor> all = m_loc->anchors();
        if (idx >= all.size()) return;
        const BfAnchor a = all[idx];
        const QString coords = QStringLiteral("%1, %2").arg(a.lat, 0, 'f', 7).arg(a.lon, 0, 'f', 7);
        menu.addSection(a.name.isEmpty() ? a.kind : a.name);
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-edit")), QStringLiteral("Edit anchor…"), this, [this, idx] { editAnchor(idx); });
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy %1").arg(coords), this, [coords] { QApplication::clipboard()->setText(coords); });
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), QStringLiteral("Remove anchor"), this, [this, a] {
            if (QMessageBox::question(this, QStringLiteral("Remove anchor"), QStringLiteral("Remove “%1”? Every synced device drops it too.").arg(a.name.isEmpty() ? a.kind : a.name)) == QMessageBox::Yes)
                m_loc->removeAnchor(a.id);
            update();
        });
        menu.exec(globalPos);
        return;
    }
    if (h.kind == HitPoi || (h.kind == HitCluster && h.items.size() == 1)) {
        const Poi pt = m_loc->pois()[h.items.first()];
        const QString dest = QStringLiteral("%1,%2").arg(pt.lat, 0, 'f', 6).arg(pt.lon, 0, 'f', 6);
        menu.addSection(pt.name.isEmpty() ? QStringLiteral("Place") : pt.name);
        menu.addAction(QIcon::fromTheme(QStringLiteral("go-next")), QStringLiteral("Directions (OpenStreetMap)"), this, [fix, dest] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.openstreetmap.org/directions?engine=fossgis_osrm_car&route=%1,%2;%3")
                                           .arg(fix.lat, 0, 'f', 6).arg(fix.lon, 0, 'f', 6).arg(dest))); });
        menu.addAction(QIcon::fromTheme(QStringLiteral("go-next")), QStringLiteral("Directions (Google Maps)"), this, [dest] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.google.com/maps/dir/?api=1&destination=%1").arg(dest))); });
        menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("View on OpenStreetMap"), this, [pt] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.openstreetmap.org/%1/%2").arg(pt.osmType).arg(pt.osmId))); });
        if (!pt.website.isEmpty())
            menu.addAction(QIcon::fromTheme(QStringLiteral("globe")), QStringLiteral("Website"), this, [pt] {
                QDesktopServices::openUrl(QUrl::fromUserInput(pt.website)); });
        if (!pt.phone.isEmpty())
            menu.addAction(QIcon::fromTheme(QStringLiteral("call-start")), QStringLiteral("Copy phone %1").arg(pt.phone), this, [pt] { QApplication::clipboard()->setText(pt.phone); });
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy coordinates"), this, [dest] { QApplication::clipboard()->setText(dest); });
    } else if (h.kind == HitBeacon && h.items.size() == 1) {
        const AccessPoint ap = m_loc->accessPoints()[h.items.first()];
        menu.addSection(ap.ssid.isEmpty() ? ap.bssid : ap.ssid);
        const bool home = m_loc->isHome(ap);
        QAction *hm = menu.addAction(QIcon::fromTheme(QStringLiteral("go-home")), home ? QStringLiteral("Not a home network") : QStringLiteral("Mark as home network (the RV)"));
        connect(hm, &QAction::triggered, this, [this, ap, home] {
            if (home) { for (const QString &p : m_loc->homeNetworks()) { const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(p), QRegularExpression::CaseInsensitiveOption); if (re.match(ap.bssid).hasMatch() || re.match(ap.ssid).hasMatch()) m_loc->removeHomeNetwork(p); } }
            else m_loc->addHomeNetwork(ap.ssid.isEmpty() ? ap.bssid : ap.ssid);
        });
        QAction *trav = menu.addAction(QStringLiteral("Travels with me (exclude)"));
        trav->setCheckable(true); trav->setChecked(m_loc->apStatus(ap) == QLatin1String("travelling"));
        connect(trav, &QAction::toggled, this, [this, ap](bool on) { m_loc->setTravelling(ap.bssid, on); });
        if (!ap.ssid.isEmpty())
            menu.addAction(QStringLiteral("Always ignore SSID \"%1\"").arg(ap.ssid), this, [this, ap] { m_loc->addIgnorePattern(ap.ssid); });
        menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("Look up on WiGLE"), this, [ap] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://wigle.net/search?netid=%1").arg(ap.bssid))); });
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy BSSID"), this, [ap] { QApplication::clipboard()->setText(ap.bssid); });
    } else {
        zoomAt(2, h.pos);
        return;
    }
    menu.exec(globalPos);
    m_apEstAt.invalidate();                                    // home / travelling / ignored may have changed
}

// ── Anchors + measured ranges (docs/RANGING.md) ──────────────────────────────
static QColor anchorColor(const QString &kind)
{
    if (kind == QLatin1String("this-computer")) return C_ME;
    if (kind == QLatin1String("wifi-ap")) return C_LOCATED;
    if (kind == QLatin1String("gnss")) return C_ACTIVE;
    if (kind == QLatin1String("rtt-responder")) return C_TRAVEL;
    if (kind == QLatin1String("ble")) return QColor(0x9d, 0x8c, 0xff);
    return C_IGNORED;
}

void BeaconView::drawAnchors(QPainter &p)
{
    const QList<BfAnchor> all = m_loc->anchors();
    if (all.isEmpty()) return;
    const double mpp = metersPerPixel(all.first().lat);
    QFont small = font(); small.setPointSizeF(font().pointSizeF() * 0.8); small.setBold(true);
    for (int i = 0; i < all.size(); ++i) {
        const BfAnchor &a = all[i];
        const QPointF at = (i == m_anchorDrag && m_anchorMoved) ? m_anchorDragPos : toScreen(a.lat, a.lon);
        if (!rect().adjusted(-40, -40, 40, 40).contains(at.toPoint())) continue;
        const QColor col = anchorColor(a.kind);
        const double r = a.accM / mpp;
        if (r > 4) { QColor c = col; c.setAlphaF(0.10); p.setPen(QPen(col, 1, Qt::DotLine)); p.setBrush(c); p.drawEllipse(at, r, r); }
        QPainterPath d; d.moveTo(at.x(), at.y() - 8); d.lineTo(at.x() + 7, at.y()); d.lineTo(at.x(), at.y() + 8); d.lineTo(at.x() - 7, at.y()); d.closeSubpath();
        p.setPen(QPen(Qt::black, 1.5)); p.setBrush(col); p.drawPath(d);
        p.setPen(QPen(Qt::white, 1)); p.setBrush(Qt::NoBrush); p.drawPath(d);
        if (a.ref) { p.setPen(QPen(col, 1.4)); p.drawEllipse(at, 11, 11); }
        if (a.headingAssumed) { p.setFont(small); p.setPen(C_TRAVEL); p.drawText(QRectF(at.x() + 7, at.y() - 16, 14, 14), Qt::AlignCenter, QStringLiteral("?")); }
        const bool hovered = m_hover >= 0 && m_hover < m_hits.size() && m_hits[m_hover].kind == HitAnchor && m_hits[m_hover].items.value(0) == i;
        if (m_zoom >= 16 || hovered || (i == m_anchorDrag && m_anchorMoved)) {
            p.setFont(small); p.setPen(C_TEXT);
            const QString label = (a.name.isEmpty() ? a.kind : a.name) + (hovered ? QStringLiteral(" · ±%1 m%2").arg(a.accM, 0, 'g', 2).arg(a.rv ? QStringLiteral(" · RV") : QString()) : QString());
            p.drawText(QRectF(at.x() - 100, at.y() + 9, 200, 14), Qt::AlignCenter, label);
        }
        m_hits.append({HitAnchor, at, 9, {i}});
    }
}

// Devices closer than a few pixels at this zoom (the phone 60 cm from the desktop): drawn in a to-scale inset
void BeaconView::drawRangeInset(QPainter &p, const QList<QJsonObject> &close)
{
    if (close.isEmpty()) return;
    const Fix &fix = m_loc->fix();
    const QPointF me = toScreen(fix.lat, fix.lon);
    double span = 0.5;
    for (const QJsonObject &r : close) span = std::max(span, r["highM"].toDouble(r["distanceM"].toDouble()) * 1.25);
    static const double nice[] = {0.5, 1, 2, 3, 5, 10, 20};
    for (double n : nice) if (n >= span) { span = n; break; }
    QFont small = font(); small.setPointSizeF(font().pointSizeF() * 0.8);
    QFont bold = small; bold.setBold(true);
    auto rowText = [this](const QJsonObject &r) {
        const double D = r["distanceM"].toDouble(), lo = r["lowM"].toDouble(D), hi = r["highM"].toDouble(D);
        QStringList m; for (const QJsonValue &v : r["method"].toArray()) m << v.toString().toUpper();
        return QStringLiteral("%1 %2  %3 (%4–%5) · %6 · %7").arg(r["kind"].toString() == QLatin1String("android") ? QStringLiteral("📱") : QStringLiteral("💻"),
                                                                 r["device"].toString().left(18), distText(D), distText(lo), distText(hi), r["class"].toString(), m.join(QLatin1Char('+')));
    };
    double textW = 0;
    for (const QJsonObject &r : close) textW = std::max(textW, double(QFontMetrics(small).horizontalAdvance(rowText(r))));
    const double w = std::max(250.0, textW + 36), rowH = 34, h = 30 + rowH * close.size();
    QRectF box(me.x() + 26, me.y() - h - 14, w, h);
    if (box.right() > width() - 6) box.moveRight(me.x() - 26);
    if (box.top() < 6) box.moveTop(me.y() + 20);
    p.setPen(QPen(QColor(255, 255, 255, 60), 1)); p.setBrush(C_PANEL); p.drawRoundedRect(box, 8, 8);
    p.setPen(QPen(QColor(255, 255, 255, 50), 1, Qt::DotLine)); p.drawLine(me, box.left() < me.x() ? box.topRight() + QPointF(0, 14) : box.topLeft() + QPointF(0, 14));
    p.setFont(bold); p.setPen(C_DIM);
    p.drawText(box.adjusted(10, 4, -10, 0), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("Measured distance · to scale (%1 m)").arg(span, 0, 'g', 2));
    const double x0 = box.left() + 16, x1 = box.right() - 16;
    for (int i = 0; i < close.size(); ++i) {
        const QJsonObject &r = close[i];
        const double y = box.top() + 30 + rowH * i + 8;
        auto xAt = [&](double m) { return x0 + (x1 - x0) * qBound(0.0, m / span, 1.0); };
        p.setPen(QPen(QColor(255, 255, 255, 40), 1)); p.drawLine(QPointF(x0, y), QPointF(x1, y));
        for (int t = 0; t <= 4; ++t) { const double x = x0 + (x1 - x0) * t / 4.0; p.drawLine(QPointF(x, y - 3), QPointF(x, y + 3)); }
        const QColor col = deviceColor(r["device"].toString());
        const double D = r["distanceM"].toDouble(), lo = r["lowM"].toDouble(D), hi = r["highM"].toDouble(D);
        QColor band = col; band.setAlphaF(0.35);
        p.setPen(Qt::NoPen); p.setBrush(band); p.drawRoundedRect(QRectF(QPointF(xAt(lo), y - 4), QPointF(xAt(hi), y + 4)), 3, 3);
        glowDot(p, QPointF(x0, y), 4, C_ME, 2);
        glowDot(p, QPointF(xAt(D), y), 5, col, 3);
        p.setFont(small); p.setPen(C_TEXT);
        p.drawText(QRectF(x0 - 6, y + 5, x1 - x0 + 12, 14), Qt::AlignLeft | Qt::AlignVCenter, rowText(r));
    }
}

void BeaconView::editAnchor(int index)
{
    const QList<BfAnchor> all = m_loc->anchors();
    if (index < 0 || index >= all.size()) return;
    AnchorDialog d(m_loc, all[index].lat, all[index].lon, all[index].toJson(false), this);
    if (d.exec() != QDialog::Accepted) return;
    bool ok = false; QString err;
    m_loc->setAnchor(d.anchor(), QStringLiteral("desktop"), &ok, &err);
    if (!ok) QMessageBox::warning(this, QStringLiteral("Anchor"), err);
    update();
}

void BeaconView::placeAnchorAt(double lat, double lon)
{
    AnchorDialog d(m_loc, lat, lon, QJsonObject(), this);
    if (d.exec() != QDialog::Accepted) return;
    bool ok = false; QString err;
    m_loc->setAnchor(d.anchor(), QStringLiteral("desktop"), &ok, &err);
    if (!ok) QMessageBox::warning(this, QStringLiteral("Anchor"), err);
    update();
}

// ── the ALPR-avoiding route (docs/SIGHTINGS.md §8) ─────────────────────────────────
void BeaconView::setRouteStart(double lat, double lon)
{
    m_haveRouteStart = true; m_routeStartLat = lat; m_routeStartLon = lon;
    update();
}

void BeaconView::clearAvoidRoute()
{
    m_haveRouteStart = false;
    m_avoidRoute.clear(); m_avoidPassed.clear();
    if (m_routeDialog) m_routeDialog->close();
    update();
}

void BeaconView::routeAvoidingAlprs(double toLat, double toLon)
{
    PlateWatch *pw = m_loc->plateWatch();
    RoutePlanner *rp = pw ? pw->routePlanner() : nullptr;
    if (!rp) return;
    if (!rp->status().value(QLatin1String("ready")).toBool()) {
        QMessageBox::information(this, QStringLiteral("Route avoiding ALPRs"),
            QStringLiteral("Routing around ALPR cameras needs your own free API key from OpenRouteService (openrouteservice.org/dev) or "
                           "GraphHopper (graphhopper.com).\n\nSet one in Settings → Routing (avoid ALPRs). Without a key nothing is sent anywhere."));
        return;
    }
    AvoidRoute::LatLon from;
    if (m_haveRouteStart) from = {m_routeStartLat, m_routeStartLon};
    else if (m_loc->fix().valid) from = {m_loc->fix().lat, m_loc->fix().lon};
    else { QMessageBox::information(this, QStringLiteral("Route avoiding ALPRs"), QStringLiteral("No position yet: right-click → Route avoiding ALPRs → Start from here first.")); return; }
    if (m_routing) return;
    m_routing = true;
    update();
    QPointer<BeaconView> self(this);
    rp->route(from, {toLat, toLon}, QString(), [self](int code, const QJsonObject &o) {
        if (!self) return;
        self->m_routing = false;
        if (code != 200) {
            QMessageBox::warning(self, QStringLiteral("Route avoiding ALPRs"), o.value(QLatin1String("error")).toString(QStringLiteral("No route (HTTP %1)").arg(code)));
            self->update();
            return;
        }
        self->m_avoidRoute.clear(); self->m_avoidPassed.clear();
        for (const QJsonValue &v : o.value(QLatin1String("route")).toObject().value(QLatin1String("coordinates")).toArray()) {
            const QJsonArray c = v.toArray();
            if (c.size() >= 2) self->m_avoidRoute << merc(c[1].toDouble(), c[0].toDouble());
        }
        for (const QJsonValue &v : o.value(QLatin1String("passes")).toArray())
            if (v.toObject().value(QLatin1String("inCone")).toBool()) self->m_avoidPassed << merc(v.toObject().value(QLatin1String("lat")).toDouble(), v.toObject().value(QLatin1String("lon")).toDouble());
        self->update();
        self->showRouteResult(o);
    });
}

void BeaconView::drawAvoidRoute(QPainter &p)
{
    if (m_avoidRoute.size() < 2 && !m_haveRouteStart && !m_routing) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    if (m_avoidRoute.size() >= 2) {
        QPolygonF line;
        for (const QPointF &m : std::as_const(m_avoidRoute)) line << toScreen(m);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(20, 20, 30, 200), 7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);
        p.setPen(QPen(QColor(230, 60, 230), 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);
        p.setPen(QPen(QColor(255, 60, 60), 3));
        for (const QPointF &m : std::as_const(m_avoidPassed)) p.drawEllipse(toScreen(m), 11, 11);   // ALPRs it still passes
        p.setPen(QPen(Qt::white, 2)); p.setBrush(QColor(230, 60, 230));
        p.drawEllipse(line.last(), 6, 6);
    }
    if (m_haveRouteStart) { p.setPen(QPen(Qt::white, 2)); p.setBrush(QColor(40, 200, 90)); p.drawEllipse(toScreen(m_routeStartLat, m_routeStartLon), 6, 6); }
    if (m_routing) {
        p.setPen(C_TEXT);
        p.drawText(QRectF(0, height() - 60, width(), 24), Qt::AlignCenter, QStringLiteral("Routing around ALPR cameras…"));
    }
    p.restore();
}

void BeaconView::showRouteResult(const QJsonObject &o)
{
    if (m_routeDialog) m_routeDialog->close();
    auto *d = new QDialog(this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(QStringLiteral("Route avoiding ALPRs"));
    d->resize(620, 380);
    auto *v = new QVBoxLayout(d);
    const QJsonObject av = o.value(QLatin1String("avoided")).toObject();
    const QJsonArray passes = o.value(QLatin1String("passes")).toArray();
    const int inCone = o.value(QLatin1String("passesInCone")).toInt();
    QString head = QStringLiteral("<b>%1 km, %2 min</b> via %3. Avoided %4 camera(s) (%5 polygons) in the corridor")
                       .arg(o.value(QLatin1String("distanceM")).toDouble() / 1000.0, 0, 'f', 1).arg(qRound(o.value(QLatin1String("durationS")).toDouble() / 60.0))
                       .arg(o.value(QLatin1String("providerName")).toString()).arg(av.value(QLatin1String("cameras")).toInt()).arg(av.value(QLatin1String("areas")).toInt());
    if (av.value(QLatin1String("capped")).toInt() > 0) head += QStringLiteral("; %1 more left out (the provider's polygon limits)").arg(av.value(QLatin1String("capped")).toInt());
    head += QStringLiteral(".<br>");
    head += inCone == 0 ? QStringLiteral("The route passes no known ALPR's field of view.")
                        : QStringLiteral("<span style='color:#d33'>It still passes %1 ALPR(s)</span> — there was no way around them.").arg(inCone);
    head += QStringLiteral("<br><small>%1</small>").arg(o.value(QLatin1String("attribution")).toString().toHtmlEscaped());
    auto *l = new QLabel(head); l->setWordWrap(true); l->setTextFormat(Qt::RichText);
    v->addWidget(l);
    auto *t = new QTableWidget(int(passes.size()), 4);
    t->setHorizontalHeaderLabels({QStringLiteral("Camera"), QStringLiteral("Operator"), QStringLiteral("Off the route"), QStringLiteral("")});
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->verticalHeader()->setVisible(false);
    t->horizontalHeader()->setStretchLastSection(true);
    for (int i = 0; i < int(passes.size()); ++i) {
        const QJsonObject c = passes[i].toObject();
        const QStringList cells{c.value(QLatin1String("id")).toString(), QStringList{c.value(QLatin1String("operator")).toString(), c.value(QLatin1String("model")).toString()}.join(QLatin1Char(' ')).simplified(),
                                QStringLiteral("%1 m").arg(qRound(c.value(QLatin1String("distanceM")).toDouble())),
                                c.value(QLatin1String("inCone")).toBool() ? QStringLiteral("passes through its field of view") : QStringLiteral("near, not facing the route")};
        for (int k = 0; k < 4; ++k) {
            auto *it = new QTableWidgetItem(cells[k]);
            if (k == 0) it->setData(Qt::UserRole, QPointF(c.value(QLatin1String("lat")).toDouble(), c.value(QLatin1String("lon")).toDouble()));
            t->setItem(i, k, it);
        }
    }
    t->resizeColumnsToContents();
    connect(t, &QTableWidget::cellDoubleClicked, this, [this, t](int row, int) {
        const QPointF ll = t->item(row, 0)->data(Qt::UserRole).toPointF();
        focusOn(ll.x(), ll.y(), 18);
    });

    auto *tabs = new QTabWidget(d);
    tabs->addTab(t, QStringLiteral("Passed Cameras (%1)").arg(passes.size()));

    const QJsonArray steps = o.value(QLatin1String("steps")).toArray();
    if (!steps.isEmpty()) {
        auto *st = new QTableWidget(int(steps.size()), 4, d);
        st->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("Road / Street"), QStringLiteral("Distance"), QStringLiteral("Instruction")});
        st->setEditTriggers(QAbstractItemView::NoEditTriggers);
        st->setSelectionBehavior(QAbstractItemView::SelectRows);
        st->verticalHeader()->setVisible(false);
        st->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        for (int i = 0; i < int(steps.size()); ++i) {
            const QJsonObject s = steps[i].toObject();
            st->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
            st->setItem(i, 1, new QTableWidgetItem(s.value(QLatin1String("streetName")).toString()));
            st->setItem(i, 2, new QTableWidgetItem(QStringLiteral("%1 m").arg(qRound(s.value(QLatin1String("distanceM")).toDouble()))));
            st->setItem(i, 3, new QTableWidgetItem(s.value(QLatin1String("instruction")).toString()));
        }
        st->resizeColumnsToContents();
        tabs->addTab(st, QStringLiteral("Turn-by-Turn Directions (%1)").arg(steps.size()));
    }
    v->addWidget(tabs, 1);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    QPushButton *clr = bb->addButton(QStringLiteral("Clear the route"), QDialogButtonBox::ResetRole);
    connect(clr, &QPushButton::clicked, this, &BeaconView::clearAvoidRoute);
    connect(bb, &QDialogButtonBox::rejected, d, &QDialog::close);
    v->addWidget(bb);
    m_routeDialog = d;
    d->show();
}

void BeaconView::inspectCamera(const QString &cameraId)
{
    auto *d = new InspectDialog(m_loc, cameraId, this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    connect(d, &InspectDialog::showPlanOnMap, this, &BeaconView::showInspectPlan);
    d->show();
    d->raise();
    d->activateWindow();
}

void BeaconView::showInspectPlan(const QJsonObject &plan)
{
    m_inspectPlan = plan;
    const QJsonObject cam = plan.value(QLatin1String("camera")).toObject();
    if (cam.contains(QLatin1String("lat")) && cam.contains(QLatin1String("lon"))) {
        focusOn(cam.value(QLatin1String("lat")).toDouble(), cam.value(QLatin1String("lon")).toDouble(), 18);
    }
    update();
}

void BeaconView::clearInspectPlan()
{
    m_inspectPlan = QJsonObject();
    update();
}

void BeaconView::drawInspectPlan(QPainter &p)
{
    if (m_inspectPlan.isEmpty()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    // 1. Draw avoid regions (semi-transparent red polygons and discs)
    const QJsonArray avoidArr = m_inspectPlan.value(QLatin1String("avoidRegions")).toArray();
    for (const QJsonValue &v : avoidArr) {
        const QJsonObject a = v.toObject();
        const QJsonArray coords = a.value(QLatin1String("coordinates")).toArray();
        if (coords.size() < 3) continue;
        QPolygonF poly;
        for (const QJsonValue &ptVal : coords) {
            const QJsonArray pt = ptVal.toArray();
            if (pt.size() >= 2) {
                // coordinates in GeoJSON ring are [lon, lat]
                poly << toScreen(pt[1].toDouble(), pt[0].toDouble());
            }
        }
        if (!poly.isEmpty()) {
            p.setBrush(QColor(230, 40, 40, 45));
            p.setPen(QPen(QColor(230, 40, 40, 160), 1.5, Qt::DashLine));
            p.drawPolygon(poly);
        }
    }

    // 2. Draw route legs: toVantage (approach) and away (departure)
    const QJsonObject legs = m_inspectPlan.value(QLatin1String("legs")).toObject();
    auto drawLeg = [this, &p](const QJsonObject &legObj, const QColor &lineCol) {
        if (!legObj.value(QLatin1String("ok")).toBool()) return;
        const QJsonArray coords = legObj.value(QLatin1String("route")).toObject().value(QLatin1String("coordinates")).toArray();
        if (coords.size() < 2) return;
        QPolygonF line;
        for (const QJsonValue &ptVal : coords) {
            const QJsonArray pt = ptVal.toArray();
            if (pt.size() >= 2) line << toScreen(pt[1].toDouble(), pt[0].toDouble());
        }
        if (line.size() >= 2) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(20, 20, 30, 200), 7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(line);
            p.setPen(QPen(lineCol, 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(line);
        }
    };

    // Approach leg: Vibrant green
    drawLeg(legs.value(QLatin1String("toVantage")).toObject(), QColor(50, 215, 95));
    // Departure leg: Vibrant cyan
    drawLeg(legs.value(QLatin1String("away")).toObject(), QColor(50, 190, 255));

    // 3. Highlight exposures in bright red
    const QJsonArray exposures = m_inspectPlan.value(QLatin1String("exposures")).toArray();
    for (const QJsonValue &eVal : exposures) {
        const QJsonObject exp = eVal.toObject();
        const QJsonArray entry = exp.value(QLatin1String("entry")).toArray();
        const QJsonArray exit = exp.value(QLatin1String("exit")).toArray();
        if (entry.size() >= 2 && exit.size() >= 2) {
            QPointF p1 = toScreen(entry[0].toDouble(), entry[1].toDouble());
            QPointF p2 = toScreen(exit[0].toDouble(), exit[1].toDouble());
            p.setPen(QPen(QColor(255, 30, 30), 6, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(p1, p2);
            p.setBrush(QColor(255, 30, 30));
            p.setPen(QPen(Qt::white, 1.5));
            p.drawEllipse(p1, 5, 5);
            p.drawEllipse(p2, 5, 5);
        }
    }

    // 4. Draw Vantage points with look-direction arrows pointing at target camera
    const QJsonArray vantages = m_inspectPlan.value(QLatin1String("vantages")).toArray();
    for (int i = 0; i < vantages.size(); ++i) {
        const QJsonObject v = vantages[i].toObject();
        const double vLat = v.value(QLatin1String("lat")).toDouble();
        const double vLon = v.value(QLatin1String("lon")).toDouble();
        const double bearing = v.value(QLatin1String("bearingToCamera")).toDouble();
        const QPointF sc = toScreen(vLat, vLon);

        // Draw arrow pointing along bearing to camera (0 deg = North)
        const double rad = qDegreesToRadians(bearing);
        const double arrowLen = 26.0;
        const double dx = std::sin(rad) * arrowLen;
        const double dy = -std::cos(rad) * arrowLen;
        const QPointF tip = sc + QPointF(dx, dy);

        // Arrow line
        p.setPen(QPen(QColor(20, 20, 30, 220), 4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(sc, tip);
        p.setPen(QPen(QColor(255, 215, 0), 2.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(sc, tip);

        // Arrowhead
        const double b1 = qDegreesToRadians(bearing + 150.0);
        const double b2 = qDegreesToRadians(bearing - 150.0);
        const QPointF a1 = tip + QPointF(std::sin(b1) * 8.0, -std::cos(b1) * 8.0);
        const QPointF a2 = tip + QPointF(std::sin(b2) * 8.0, -std::cos(b2) * 8.0);
        QPolygonF headPoly;
        headPoly << tip << a1 << a2;
        p.setBrush(QColor(255, 215, 0));
        p.setPen(QPen(QColor(20, 20, 30), 1.0));
        p.drawPolygon(headPoly);

        // Vantage station circle
        const bool isPrimary = (i == 0);
        const double r = isPrimary ? 8.0 : 6.0;
        p.setBrush(isPrimary ? QColor(255, 215, 0) : QColor(200, 200, 200));
        p.setPen(QPen(QColor(20, 20, 30), 2.0));
        p.drawEllipse(sc, r, r);

        // Number 1, 2, 3
        p.setPen(QColor(20, 20, 30));
        QFont f = p.font();
        f.setPixelSize(isPrimary ? 11 : 9);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(sc.x() - r, sc.y() - r, 2 * r, 2 * r), Qt::AlignCenter, QString::number(i + 1));
    }

    // 5. Target Camera highlight ring
    const QJsonObject cam = m_inspectPlan.value(QLatin1String("camera")).toObject();
    if (cam.contains(QLatin1String("lat")) && cam.contains(QLatin1String("lon"))) {
        const QPointF camSc = toScreen(cam.value(QLatin1String("lat")).toDouble(), cam.value(QLatin1String("lon")).toDouble());
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 60, 60, 220), 2.5, Qt::DashLine));
        p.drawEllipse(camSc, 18, 18);
        p.setPen(QPen(QColor(255, 215, 0, 180), 1.5));
        p.drawEllipse(camSc, 23, 23);
    }

    // 6. Floating Status Card at the top-center
    const bool safe = m_inspectPlan.value(QLatin1String("safe")).toBool();
    const int expCount = exposures.size();
    QString bannerText = safe ? QStringLiteral("INSPECT UNSEEN · CLEAN / SAFE")
                              : QStringLiteral("INSPECT UNSEEN · %1 EXPOSURE(S)").arg(expCount);
    const QColor bgCol = safe ? QColor(20, 70, 35, 220) : QColor(100, 20, 20, 230);
    const QColor borderCol = safe ? QColor(60, 220, 100) : QColor(255, 80, 80);

    QFont bannerFont = p.font();
    bannerFont.setPixelSize(12);
    bannerFont.setBold(true);
    p.setFont(bannerFont);
    QFontMetrics fm(bannerFont);
    int textW = fm.horizontalAdvance(bannerText);
    QRectF cardRect((width() - textW - 40) / 2.0, 10, textW + 40, 28);

    p.setBrush(bgCol);
    p.setPen(QPen(borderCol, 1.5));
    p.drawRoundedRect(cardRect, 5, 5);

    p.setPen(Qt::white);
    p.drawText(cardRect, Qt::AlignCenter, bannerText);

    p.restore();
}
