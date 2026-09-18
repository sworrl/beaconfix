#include "beaconview.h"
#include "locator.h"
#include <QMouseEvent>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QWheelEvent>
#include <QtMath>

static const int TILE = 256;
static const QColor C_BG(0x0b, 0x10, 0x1a);
static const QColor C_ME(0x35, 0xd6, 0xff);        // cyan
static const QColor C_USED(0x35, 0xd6, 0xff);
static const QColor C_LOCATED(0xff, 0xd1, 0x66);   // gold: real position known
static const QColor C_TRAVEL(0xff, 0x4f, 0xd8);    // magenta
static const QColor C_ACTIVE(0x6c, 0xff, 0x8a);    // green
static const QColor C_IGNORED(0x8a, 0x93, 0xa6);   // grey
static const QColor C_TEXT(0xe6, 0xed, 0xf7);

BeaconView::BeaconView(Locator *loc, QWidget *parent) : QWidget(parent), m_loc(loc)
{
    auto *cache = new QNetworkDiskCache(this);
    cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/tiles-dark");
    cache->setMaximumCacheSize(300 * 1024 * 1024);
    m_nam.setCache(cache);
    setMinimumSize(360, 260);
    setMouseTracking(true);
    setAutoFillBackground(false);
    m_anim.setInterval(40);
    connect(&m_anim, &QTimer::timeout, this, [this] {
        m_phase = std::fmod(m_phase + 0.012, 1.0);
        m_sweep = std::fmod(m_sweep + 1.6, 360.0);
        update();
    });
    connect(m_loc, &Locator::FixChanged, this, [this] { if (m_followFix) recenter(); update(); });
    connect(m_loc, &Locator::scanUpdated, this, [this] { if (m_followFix) fitBeacons(); update(); });
    recenter();
}

void BeaconView::recenter()
{
    const Fix &f = m_loc->fix();
    if (f.valid) { m_centerLat = f.lat; m_centerLon = f.lon; }
    m_followFix = true;
    fitBeacons();
    update();
}

// Pick the zoom where the furthest beacon (or the accuracy ring) fills ~40 % of the view
void BeaconView::fitBeacons()
{
    const Fix &f = m_loc->fix();
    if (!f.valid) return;
    double maxM = qMax(60.0, f.accuracy);
    for (const AccessPoint &ap : m_loc->accessPoints()) {
        const ApEstimate e = m_loc->estimateFor(ap);
        if (e.kind == ApEstimate::Ring) maxM = qMax(maxM, e.radiusM);
        else if (e.kind != ApEstimate::None) maxM = qMax(maxM, Locator::distanceM(f.lat, f.lon, e.lat, e.lon) + e.radiusM);
    }
    if (f.source == QLatin1String("ip")) maxM = qMax(maxM, 20000.0);
    const double target = qMin(width(), height()) * 0.40;
    int z = 19;
    while (z > 3 && maxM / metersPerPixelAt(f.lat, z) > target) --z;
    m_zoom = z;
}

double BeaconView::metersPerPixelAt(double lat, int zoom)
{
    return 156543.03392 * std::cos(qDegreesToRadians(lat)) / (1 << zoom);
}

void BeaconView::setZoom(int z) { m_zoom = qBound(3, z, 19); update(); }
void BeaconView::wheelEvent(QWheelEvent *e) { setZoom(m_zoom + (e->angleDelta().y() > 0 ? 1 : -1)); }
void BeaconView::showEvent(QShowEvent *) { m_anim.start(); }
void BeaconView::hideEvent(QHideEvent *) { m_anim.stop(); }
void BeaconView::leaveEvent(QEvent *) { m_hover = -1; update(); }

void BeaconView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true; m_dragStart = e->position(); m_dragLat = m_centerLat; m_dragLon = m_centerLon;
}
void BeaconView::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging) {
        const int n = 1 << m_zoom;
        const double dx = (e->position().x() - m_dragStart.x()) / TILE, dy = (e->position().y() - m_dragStart.y()) / TILE;
        const double latR = qDegreesToRadians(m_dragLat);
        const double tx = (m_dragLon + 180.0) / 360.0 * n - dx;
        const double ty = (1.0 - std::log(std::tan(latR) + 1.0 / std::cos(latR)) / M_PI) / 2.0 * n - dy;
        m_centerLon = tx / n * 360.0 - 180.0;
        m_centerLat = qRadiansToDegrees(std::atan(std::sinh(M_PI * (1 - 2 * ty / n))));
        m_followFix = false;
        update();
        return;
    }
    int best = -1; double bestD = 18;
    for (int i = 0; i < m_placed.size(); ++i) {
        const double d = QLineF(m_placed[i].pos, e->position()).length();
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best != m_hover) { m_hover = best; update(); }
}
void BeaconView::mouseReleaseEvent(QMouseEvent *) { m_dragging = false; }
void BeaconView::mouseDoubleClickEvent(QMouseEvent *) { recenter(); }

QString BeaconView::key(int z, int x, int y) const { return QStringLiteral("%1/%2/%3").arg(z).arg(x).arg(y); }

void BeaconView::ensureTile(int z, int x, int y)
{
    const QString k = key(z, x, y);
    if (m_tiles.contains(k) || m_pending.contains(k)) return;
    m_pending.insert(k);
    QNetworkRequest req(QUrl(QStringLiteral("https://tile.openstreetmap.org/%1.png").arg(k)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("BeaconFix/" BEACONFIX_VERSION " (KDE desktop locator; +https://github.com/sworrl/beaconfix)"));
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    req.setTransferTimeout(15000);
    QNetworkReply *rep = m_nam.get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep, k] {
        rep->deleteLater();
        m_pending.remove(k);
        QImage img;
        if (rep->error() == QNetworkReply::NoError && img.loadFromData(rep->readAll())) m_tiles.insert(k, QPixmap::fromImage(darken(img)));
        update();
    });
}

// Night-mode filter for the standard OSM tile: invert, desaturate, tint blue-black.
QImage BeaconView::darken(const QImage &src)
{
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = row[x];
            int r = 255 - qRed(c), g = 255 - qGreen(c), b = 255 - qBlue(c);
            const int l = (r * 30 + g * 59 + b * 11) / 100;
            // 70 % towards grey, then scale into a navy palette
            r = (r * 3 + l * 7) / 10; g = (g * 3 + l * 7) / 10; b = (b * 3 + l * 7) / 10;
            r = 11 + r * 70 / 255; g = 16 + g * 82 / 255; b = 26 + b * 110 / 255;
            row[x] = qRgba(r, g, b, qAlpha(c));
        }
    }
    return img;
}

double BeaconView::metersPerPixel(double lat) const
{
    return 156543.03392 * std::cos(qDegreesToRadians(lat)) / (1 << m_zoom);
}

QPointF BeaconView::project(double lat, double lon) const
{
    const int n = 1 << m_zoom;
    auto tile = [n](double la, double lo) {
        const double r = qDegreesToRadians(la);
        return QPointF((lo + 180.0) / 360.0 * n, (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / M_PI) / 2.0 * n);
    };
    const QPointF c = tile(m_centerLat, m_centerLon), p = tile(lat, lon);
    return QPointF(width() / 2.0 + (p.x() - c.x()) * TILE, height() / 2.0 + (p.y() - c.y()) * TILE);
}

static QPointF offsetM(const QPointF &origin, double bearingDeg, double metres, double mpp)
{
    const double a = qDegreesToRadians(bearingDeg);
    return origin + QPointF(std::sin(a) * metres / mpp, -std::cos(a) * metres / mpp);
}

static void glowDot(QPainter &p, const QPointF &c, double r, const QColor &col, double glow = 3.0)
{
    QRadialGradient g(c, r * glow);
    QColor t = col; t.setAlpha(110); g.setColorAt(0, t);
    t.setAlpha(0); g.setColorAt(1, t);
    p.setPen(Qt::NoPen); p.setBrush(g); p.drawEllipse(c, r * glow, r * glow);
    p.setBrush(col); p.setPen(QPen(QColor(255, 255, 255, 200), 1.2)); p.drawEllipse(c, r, r);
}

void BeaconView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), C_BG);
    p.setRenderHint(QPainter::Antialiasing);
    const Fix &fix = m_loc->fix();

    // ── basemap ──────────────────────────────────────────────────────────────
    {
        const int n = 1 << m_zoom;
        const double latR = qDegreesToRadians(m_centerLat);
        const double tx = (m_centerLon + 180.0) / 360.0 * n;
        const double ty = (1.0 - std::log(std::tan(latR) + 1.0 / std::cos(latR)) / M_PI) / 2.0 * n;
        const double cx = width() / 2.0, cy = height() / 2.0;
        for (int x = int(std::floor(tx - cx / TILE)); x <= int(std::floor(tx + cx / TILE)); ++x)
            for (int y = int(std::floor(ty - cy / TILE)); y <= int(std::floor(ty + cy / TILE)); ++y) {
                if (y < 0 || y >= n) continue;
                const int wx = ((x % n) + n) % n;
                const QString k = key(m_zoom, wx, y);
                const QPointF pos(cx + (x - tx) * TILE, cy + (y - ty) * TILE);
                if (m_tiles.contains(k)) p.drawPixmap(pos, m_tiles.value(k));
                else ensureTile(m_zoom, wx, y);
            }
        // subtle vignette so the HUD reads well
        QRadialGradient v(rect().center(), qMax(width(), height()) * 0.75);
        v.setColorAt(0.55, QColor(0, 0, 0, 0)); v.setColorAt(1.0, QColor(0, 0, 0, 140));
        p.fillRect(rect(), v);
    }

    m_placed.clear();
    if (!fix.valid) {
        p.setPen(C_TEXT);
        QFont f = p.font(); f.setPointSizeF(f.pointSizeF() * 1.4); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("Listening for beacons…"));
        drawHud(p);
        return;
    }

    const QPointF me = project(fix.lat, fix.lon);
    const double mpp = metersPerPixel(fix.lat);

    // ── access points ────────────────────────────────────────────────────────
    const auto &aps = m_loc->accessPoints();
    struct Item { int i; ApEstimate e; QColor col; QString st; };
    QList<Item> items;
    for (int i = 0; i < aps.size(); ++i) {
        const ApEstimate e = m_loc->estimateFor(aps[i]);
        if (e.kind == ApEstimate::None) continue;
        const QString st = m_loc->apStatus(aps[i]);
        QColor col = st == QLatin1String("used") ? C_USED
                   : st == QLatin1String("active") ? C_ACTIVE
                   : st == QLatin1String("travelling") ? C_TRAVEL : C_IGNORED;
        if (e.kind != ApEstimate::Ring && st == QLatin1String("used")) col = C_LOCATED;
        items.append({i, e, col, st});
    }
    // RSSI rings first (faint orbits), then dots, so nothing is buried
    for (const Item &it : items) {
        if (it.e.kind != ApEstimate::Ring) continue;
        const double r = it.e.radiusM / mpp;
        QColor c = it.col; c.setAlpha(28);
        p.setPen(QPen(c, 1, Qt::DotLine)); p.setBrush(Qt::NoBrush);
        p.drawEllipse(me, r, r);
    }
    for (const Item &it : items) {
        QPointF pos;
        if (it.e.kind == ApEstimate::Ring) pos = offsetM(me, it.e.bearingDeg, it.e.radiusM, mpp);
        else {
            pos = project(it.e.lat, it.e.lon);
            const double r = it.e.radiusM / mpp;
            QColor c = it.col; c.setAlpha(60);
            p.setPen(QPen(c, 1, Qt::DashLine)); p.setBrush(Qt::NoBrush);
            p.drawEllipse(pos, r, r);
        }
        const double dot = it.st == QLatin1String("used") || it.st == QLatin1String("active") ? 5.0 : 3.5;
        if (it.e.kind == ApEstimate::Wigle) {   // diamond = ground truth
            QPainterPath d; d.moveTo(pos.x(), pos.y() - 7); d.lineTo(pos.x() + 7, pos.y()); d.lineTo(pos.x(), pos.y() + 7); d.lineTo(pos.x() - 7, pos.y()); d.closeSubpath();
            glowDot(p, pos, 1, it.col, 12);
            p.setBrush(it.col); p.setPen(QPen(Qt::white, 1.2)); p.drawPath(d);
        } else {
            glowDot(p, pos, dot, it.col, it.st == QLatin1String("used") ? 3.2 : 2.2);
        }
        m_placed.append({pos, dot, it.i});
    }

    // ── me ───────────────────────────────────────────────────────────────────
    {
        if (fix.accuracy > 0) {
            const double r = qBound(8.0, fix.accuracy / mpp, 6000.0);
            p.setPen(QPen(QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 120), 1.5));
            p.setBrush(QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 22));
            p.drawEllipse(me, r, r);
            // radar sweep inside the accuracy ring
            QConicalGradient sweep(me, -m_sweep);
            sweep.setColorAt(0.0, QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 90));
            sweep.setColorAt(0.18, QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 0));
            sweep.setColorAt(1.0, QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 0));
            p.setPen(Qt::NoPen); p.setBrush(sweep); p.drawEllipse(me, r, r);
        }
        const double pr = 10 + 26 * m_phase;
        QColor pc = C_ME; pc.setAlpha(int(160 * (1 - m_phase)));
        p.setPen(QPen(pc, 2)); p.setBrush(Qt::NoBrush); p.drawEllipse(me, pr, pr);
        glowDot(p, me, 7, C_ME, 3.5);
        p.setBrush(Qt::white); p.setPen(Qt::NoPen); p.drawEllipse(me, 2.5, 2.5);
    }

    // ── labels: strongest few (all when zoomed in), placed to avoid overlaps ──
    {
        QFont f = p.font(); f.setPointSizeF(f.pointSizeF() * 0.85); p.setFont(f);
        const int maxLabels = m_zoom >= 18 ? 999 : m_zoom >= 16 ? 12 : 6;
        int shown = 0;
        QList<QRectF> taken;
        for (int k = 0; k < m_placed.size(); ++k) {
            const AccessPoint &ap = aps[m_placed[k].index];
            const bool hovered = (k == m_hover);
            if (!hovered && shown >= maxLabels) continue;
            if (!hovered && m_loc->apStatus(ap) != QLatin1String("used") && m_zoom < 18) continue;
            QString text = ap.ssid.isEmpty() ? QStringLiteral("(hidden)") : ap.ssid;
            if (hovered) {
                const ApEstimate e = m_loc->estimateFor(ap);
                const QString how = e.kind == ApEstimate::Wigle ? QStringLiteral("WiGLE position")
                                  : e.kind == ApEstimate::Centroid ? QStringLiteral("estimated from %1 observations").arg(m_loc->record(ap.bssid) ? m_loc->record(ap.bssid)->obs.size() : 0)
                                  : QStringLiteral("~%1 m away, bearing unknown").arg(qRound(e.radiusM));
                text += QStringLiteral("\n%1 · %2 dBm · %3 MHz\n%4 · %5").arg(ap.bssid).arg(ap.dbm).arg(ap.frequency).arg(m_loc->apStatus(ap), how);
            }
            const QSizeF sz = p.fontMetrics().boundingRect(QRect(0, 0, 420, 200), Qt::AlignLeft, text).adjusted(-6, -3, 6, 3).size();
            const QPointF c = m_placed[k].pos;
            const QPointF cands[] = { QPointF(c.x() + 10, c.y() - sz.height() / 2), QPointF(c.x() - 10 - sz.width(), c.y() - sz.height() / 2),
                                      QPointF(c.x() - sz.width() / 2, c.y() - 12 - sz.height()), QPointF(c.x() - sz.width() / 2, c.y() + 12) };
            QRectF box; bool ok = false;
            for (const QPointF &tl : cands) {
                box = QRectF(tl, sz);
                if (box.left() < 0 || box.right() > width() || box.top() < 0 || box.bottom() > height()) continue;
                bool hit = false;
                for (const QRectF &t : taken) if (t.intersects(box)) { hit = true; break; }
                if (!hit) { ok = true; break; }
            }
            if (!ok && !hovered) continue;
            if (!ok) box = QRectF(cands[0], sz);
            taken.append(box);
            if (hovered) { p.setPen(QPen(QColor(255, 255, 255, 120), 1)); p.drawLine(c, box.center()); }
            p.setPen(Qt::NoPen); p.setBrush(QColor(8, 12, 20, hovered ? 235 : 175));
            p.drawRoundedRect(box, 4, 4);
            p.setPen(hovered ? Qt::white : QColor(C_TEXT.red(), C_TEXT.green(), C_TEXT.blue(), 215));
            p.drawText(box.adjusted(6, 3, -6, -3), Qt::AlignLeft | Qt::AlignVCenter, text);
            ++shown;
        }
    }

    drawHud(p);
    drawLegend(p);
}

void BeaconView::drawHud(QPainter &p)
{
    const Stats st = m_loc->stats();
    const Fix &fix = m_loc->fix();
    QFont base = font();
    QFont title = base; title.setPointSizeF(base.pointSizeF() * 1.35); title.setBold(true);
    QFont small = base; small.setPointSizeF(base.pointSizeF() * 0.85);

    QStringList lines;
    lines << QStringLiteral("%1").arg(st.rank.toUpper());
    lines << QStringLiteral("Lv %1 · %2 beacons logged%3").arg(st.rankLevel).arg(st.beaconsTotal)
             .arg(st.nextRankAt ? QStringLiteral(" · next rank at %1").arg(st.nextRankAt) : QString());
    lines << QStringLiteral("In range %1 · used %2 · located %3 · travelling %4").arg(st.beaconsNow).arg(st.usedNow).arg(st.locatedNow).arg(st.travellingNow);
    lines << QStringLiteral("Stops %1 · %2 km travelled · best fix ±%3 m").arg(st.stops).arg(st.distanceKm, 0, 'f', 1).arg(st.bestAccuracy >= 0 ? QString::number(qRound(st.bestAccuracy)) : QStringLiteral("—"));
    if (fix.valid) lines << QStringLiteral("Now: ±%1 m via %2").arg(qRound(fix.accuracy)).arg(fix.source == QLatin1String("wifi") ? QStringLiteral("BeaconDB") : fix.source == QLatin1String("starlink") ? QStringLiteral("Starlink GPS") : QStringLiteral("IP"));

    p.setFont(title);
    double w = p.fontMetrics().horizontalAdvance(lines[0]);
    p.setFont(small);
    for (int i = 1; i < lines.size(); ++i) w = qMax(w, double(p.fontMetrics().horizontalAdvance(lines[i])));
    const double lh = p.fontMetrics().height();
    const double th = QFontMetrics(title).height();
    QRectF box(12, 12, w + 28, th + lh * (lines.size() - 1) + 26);
    p.setPen(QPen(QColor(C_ME.red(), C_ME.green(), C_ME.blue(), 90), 1));
    p.setBrush(QColor(8, 12, 20, 190));
    p.drawRoundedRect(box, 8, 8);
    // XP bar toward next rank
    if (st.nextRankAt) {
        const int prevAt = st.rankLevel >= 2 ? (st.nextRankAt == 10 ? 0 : st.nextRankAt == 50 ? 10 : st.nextRankAt == 150 ? 50 : st.nextRankAt == 400 ? 150 : st.nextRankAt == 1000 ? 400 : 1000) : 0;
        const double frac = qBound(0.0, double(st.beaconsTotal - prevAt) / double(st.nextRankAt - prevAt), 1.0);
        QRectF bar(box.left() + 14, box.bottom() - 8, box.width() - 28, 3);
        p.setPen(Qt::NoPen); p.setBrush(QColor(255, 255, 255, 40)); p.drawRoundedRect(bar, 1.5, 1.5);
        p.setBrush(C_LOCATED); p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * frac, bar.height()), 1.5, 1.5);
    }
    double y = box.top() + 10;
    p.setFont(title); p.setPen(C_LOCATED);
    p.drawText(QRectF(box.left() + 14, y, box.width(), th), Qt::AlignLeft | Qt::AlignVCenter, lines[0]);
    y += th;
    p.setFont(small); p.setPen(C_TEXT);
    for (int i = 1; i < lines.size(); ++i) { p.drawText(QRectF(box.left() + 14, y, box.width(), lh), Qt::AlignLeft | Qt::AlignVCenter, lines[i]); y += lh; }
}

void BeaconView::drawLegend(QPainter &p)
{
    QFont small = font(); small.setPointSizeF(small.pointSizeF() * 0.8); p.setFont(small);
    struct L { QColor c; QString t; };
    const L items[] = {{C_ME, QStringLiteral("you")}, {C_LOCATED, QStringLiteral("located beacon")}, {C_USED, QStringLiteral("heard · distance only")},
                       {C_ACTIVE, QStringLiteral("connected")}, {C_TRAVEL, QStringLiteral("travels with you")}, {C_IGNORED, QStringLiteral("ignored")}};
    double w = 0; const double lh = p.fontMetrics().height() + 3;
    for (const L &l : items) w = qMax(w, double(p.fontMetrics().horizontalAdvance(l.t)));
    QRectF box(12, height() - 12 - (lh * 6 + 14), w + 34, lh * 6 + 14);
    p.setPen(Qt::NoPen); p.setBrush(QColor(8, 12, 20, 170)); p.drawRoundedRect(box, 8, 8);
    double y = box.top() + 7;
    for (const L &l : items) {
        p.setBrush(l.c); p.setPen(Qt::NoPen); p.drawEllipse(QPointF(box.left() + 14, y + lh / 2), 4, 4);
        p.setPen(C_TEXT); p.drawText(QRectF(box.left() + 26, y, w + 4, lh), Qt::AlignLeft | Qt::AlignVCenter, l.t);
        y += lh;
    }
    const QString attr = QStringLiteral("© OpenStreetMap contributors · z%1 · drag to pan, wheel to zoom, double-click to recentre").arg(m_zoom);
    const QRect tr = p.fontMetrics().boundingRect(attr).adjusted(-6, -2, 6, 2);
    QRect ab(width() - tr.width() - 8, height() - tr.height() - 8, tr.width(), tr.height());
    p.setPen(Qt::NoPen); p.setBrush(QColor(8, 12, 20, 170)); p.drawRoundedRect(ab, 4, 4);
    p.setPen(QColor(C_TEXT.red(), C_TEXT.green(), C_TEXT.blue(), 180)); p.drawText(ab, Qt::AlignCenter, attr);
}
