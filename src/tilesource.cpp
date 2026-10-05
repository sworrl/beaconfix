#include "tilesource.h"
#include <QBuffer>
#include <QColor>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QtMath>
#include <QTcpSocket>
#include <QUrl>
#include <array>

static const char *TILE_UA = "BeaconFix/" BEACONFIX_VERSION " (KDE desktop locator; +https://github.com/sworrl/beaconfix)";

TileSource::TileSource(QObject *parent) : QObject(parent)
{
    auto *cache = new QNetworkDiskCache(this);
    cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/tiles");
    cache->setMaximumCacheSize(500 * 1024 * 1024);      // plenty for offline stretches
    m_nam.setCache(cache);
    m_images.setMaxCost(256);                           // ~64 MB decoded; the map keeps its own pixmaps
    m_encoded.setMaxCost(48 * 1024 * 1024);
    m_pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 2, 4));
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *s = m_server.nextPendingConnection()) {
            connect(s, &QTcpSocket::readyRead, this, [this, s] { serve(s); });
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        }
    });
}

int TileSource::maxZoom(Layer l) { return l == Topo || l == Dem ? (l == Dem ? 15 : 17) : l == SatUsgs ? 16 : l == SatViirs ? 9 : l == Contours ? 22 : 19; }   // deeper zooms scale these

QString TileSource::satName(SatSource s)
{
    switch (s) {
    case SatEsriClarity: return QStringLiteral("Esri World Imagery (Clarity)");
    case SatUsgsNaip:    return QStringLiteral("USGS National Map (US)");
    case SatNasaViirs:   return QStringLiteral("NASA VIIRS — yesterday");
    case SatEsri: break;
    }
    return QStringLiteral("Esri World Imagery");
}

QString TileSource::viirsDate() { return QDateTime::currentDateTimeUtc().addDays(-1).date().toString(Qt::ISODate); }

QString TileSource::key(Layer l, int z, int x, int y)
{
    return l == SatViirs ? QStringLiteral("%1@%5/%2/%3/%4").arg(int(l)).arg(z).arg(x).arg(y).arg(viirsDate())   // a new day is a new tile
                         : QStringLiteral("%1/%2/%3/%4").arg(int(l)).arg(z).arg(x).arg(y);
}

// All keyless: OSM (Dark = OSM through nightMode()), Esri imagery + place labels, OpenTopoMap,
// and the other satellite sources (USGS, NASA GIBS)
QString TileSource::upstreamUrl(Layer l, int z, int x, int y)
{
    switch (l) {
    case Roads:      return QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Transportation/MapServer/tile/%1/%3/%2").arg(z).arg(x).arg(y);
    case Dem:        return QStringLiteral("https://s3.amazonaws.com/elevation-tiles-prod/terrarium/%1/%2/%3.png").arg(z).arg(x).arg(y);
    case Contours:   return {};                            // drawn here (contourTile), from Dem tiles
    case SatClarity: return QStringLiteral("https://clarity.maptiles.arcgis.com/arcgis/rest/services/World_Imagery/MapServer/tile/%1/%3/%2").arg(z).arg(x).arg(y);
    case SatUsgs:    return QStringLiteral("https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/%1/%3/%2").arg(z).arg(x).arg(y);
    case SatViirs:   return QStringLiteral("https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/VIIRS_NOAA20_CorrectedReflectance_TrueColor/default/%4/GoogleMapsCompatible_Level9/%1/%3/%2.jpg")
                            .arg(z).arg(x).arg(y).arg(viirsDate());
    case Labels:    return QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/%1/%3/%2").arg(z).arg(x).arg(y);
    case Satellite: return QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/%1/%3/%2").arg(z).arg(x).arg(y);
    case Topo:      return QStringLiteral("https://tile.opentopomap.org/%1/%2/%3.png").arg(z).arg(x).arg(y);
    case Dark: case Streets: break;
    }
    return QStringLiteral("https://tile.openstreetmap.org/%1/%2/%3.png").arg(z).arg(x).arg(y);
}

// Night mode for the standard OSM style. A plain invert makes forests brighter than
// roads, so lightness goes through a piecewise curve instead: the paper background
// becomes the darkest navy, white roads lift to slate, dark text flips to light,
// and coloured areas stay dark but keep their hue (road classes stay tellable apart).
// OSM tiles are palette PNGs, so the curve runs over the ≤ 256 palette entries rather
// than 65 536 pixels; anything else goes per distinct colour.
QImage TileSource::nightMode(const QImage &src)
{
    static const auto lut = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const double L = i / 255.0;
            t[i] = float(L < 0.45 ? 0.90 - L * 0.85 : L < 0.85 ? 0.13 + (0.85 - L) * 0.22
                       : L < 0.975 ? 0.075 + (0.975 - L) * 0.44 : 0.075 + (L - 0.975) / 0.025 * 0.24);
        }
        return t;
    }();
    auto night = [](QRgb c) {
        float h, s, l, a;
        QColor::fromRgb(c).getHslF(&h, &s, &l, &a);
        float l2 = lut[qBound(0, int(l * 255.0f + 0.5f), 255)], s2 = s * 0.75f;
        if (s > 0.6f && l > 0.65f && l < 0.95f) { l2 = 0.24f; s2 = s * 0.55f; }   // road fills stay visible
        const QColor o = QColor::fromHslF(h < 0 ? 0 : h, s2, l2);
        return qRgba(int(o.red() * 0.90), int(o.green() * 0.96), qMin(255, int(o.blue() * 1.05 + 6)), qAlpha(c));
    };
    if (src.format() == QImage::Format_Indexed8) {
        QImage img = src;
        QList<QRgb> ct = img.colorTable();
        for (QRgb &c : ct) c = night(c);
        img.setColorTable(ct);
        return img.convertToFormat(QImage::Format_ARGB32);
    }
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    QHash<QRgb, QRgb> memo;
    QRgb lastIn = 0, lastOut = night(0);
    for (int y = 0; y < img.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (row[x] == lastIn) { row[x] = lastOut; continue; }
            lastIn = row[x];
            auto it = memo.constFind(lastIn);
            if (it == memo.constEnd()) it = memo.insert(lastIn, night(lastIn));
            row[x] = lastOut = *it;
        }
    }
    return img;
}

void TileSource::get(Layer l, int z, int x, int y, QObject *context, Done done)
{
    const QString k = key(l, z, x, y);
    if (QImage *img = m_images.object(k)) { done(*img); return; }
    const bool busy = m_inflight.contains(k);           // on the wire or being decoded
    m_inflight[k].append({QPointer<QObject>(context), std::move(done)});
    if (busy) return;
    if (l == Contours) { contourTile(z, x, y); return; }

    QNetworkRequest req{QUrl(upstreamUrl(l, z, x, y))};
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(TILE_UA));
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    req.setTransferTimeout(20000);
    QNetworkReply *rep = m_nam.get(req);
    m_replies.insert(k, rep);
    connect(rep, &QNetworkReply::finished, this, [this, rep, k, l] {
        rep->deleteLater();
        if (m_replies.value(k) != rep) return;          // cancelled (and k may belong to a newer request)
        m_replies.remove(k);
        if (rep->error() != QNetworkReply::NoError) { deliver(k, QImage()); return; }
        const QByteArray body = rep->readAll();
        QByteArray type = rep->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        if (!type.startsWith("image/")) type = body.startsWith("\xFF\xD8") ? "image/jpeg" : "image/png";
        // Decode (+ night filter) off the GUI thread; m_pool is drained before this object goes
        m_pool.start([this, k, l, body, type] {
            QImage img;
            if (img.loadFromData(body)) {
                if (l == Dark) img = nightMode(img);
                img = img.convertToFormat(img.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);   // QPixmap::fromImage without a conversion
            }
            QMetaObject::invokeMethod(this, [this, k, l, img, body, type] {
                if (!img.isNull() && l != Dark) m_encoded.insert(k, new Encoded{body, type}, body.size());   // the widget gets the bytes untouched
                deliver(k, img);
            }, Qt::QueuedConnection);
        });
    });
}

void TileSource::deliver(const QString &k, const QImage &img)
{
    if (!img.isNull()) m_images.insert(k, new QImage(img));
    const QList<Waiter> ws = m_inflight.take(k);
    for (const Waiter &w : ws)
        if (w.ctx) w.done(img);
}

void TileSource::cancel(Layer l, int z, int x, int y, QObject *context)
{
    const QString k = key(l, z, x, y);
    auto it = m_inflight.find(k);
    if (it == m_inflight.end()) return;
    it->removeIf([context](const Waiter &w) { return !w.ctx || w.ctx == context; });
    if (!it->isEmpty()) return;
    // Nobody wants it any more. A finished download is left to decode (it's cheap and gets cached);
    // one still on the wire is dropped, so the tiles in view come in sooner.
    if (QPointer<QNetworkReply> rep = m_replies.take(k)) {
        m_inflight.erase(it);
        rep->abort();
    }
}

bool TileSource::listen()
{
    for (quint16 port = 47821; port < 47831; ++port)
        if (m_server.listen(QHostAddress::LocalHost, port)) return true;
    return false;
}

QString TileSource::baseUrl() const
{
    return m_server.isListening() ? QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()) : QString();
}

void TileSource::reply(QTcpSocket *s, int code, const QByteArray &type, const QByteArray &body)
{
    const QByteArray status = code == 200 ? "200 OK" : code == 404 ? "404 Not Found" : code == 400 ? "400 Bad Request" : "502 Bad Gateway";
    s->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size())
             + "\r\nCache-Control: max-age=86400\r\nConnection: close\r\n\r\n" + body);
    s->disconnectFromHost();
}

void TileSource::serve(QTcpSocket *s)
{
    if (s->property("handled").toBool()) return;
    QByteArray buf = s->property("buf").toByteArray() + s->readAll();
    if (buf.size() > 8192) { reply(s, 400, "text/plain", "bad request"); return; }
    if (!buf.contains("\r\n\r\n")) { s->setProperty("buf", buf); return; }
    s->setProperty("handled", true);

    static const QRegularExpression heatRe(QStringLiteral("^GET /h/(\\d{1,20})/(\\d{1,2})/(\\d{1,7})/(\\d{1,7})\\.png HTTP/1\\.[01]\r\n"));
    if (const auto h = heatRe.match(QString::fromLatin1(buf)); h.hasMatch()) {
        serveHeat(s, h.captured(1).toULongLong(), h.captured(2).toInt(), h.captured(3).toInt(), h.captured(4).toInt());
        return;
    }
    static const QRegularExpression re(QStringLiteral("^GET /t/(L|C|U|V|R|K|[0-3])/(\\d{1,2})/(\\d{1,7})/(\\d{1,7})\\.png HTTP/1\\.[01]\r\n"));
    const auto m = re.match(QString::fromLatin1(buf));
    if (!m.hasMatch()) { reply(s, 404, "text/plain", "not found"); return; }
    static const QHash<QString, Layer> letters{{QStringLiteral("L"), Labels}, {QStringLiteral("C"), SatClarity}, {QStringLiteral("U"), SatUsgs}, {QStringLiteral("V"), SatViirs},
                                                    {QStringLiteral("R"), Roads}, {QStringLiteral("K"), Contours}};
    const Layer l = letters.value(m.captured(1), Layer(m.captured(1).toInt()));
    const int z = m.captured(2).toInt(), x = m.captured(3).toInt(), y = m.captured(4).toInt();
    if (z > maxZoom(l) || x >= (1 << z) || y >= (1 << z)) { reply(s, 404, "text/plain", "no such tile"); return; }

    const QString k = key(l, z, x, y);
    if (const Encoded *e = m_encoded.object(k)) { reply(s, 200, e->type, e->body); return; }
    QPointer<QTcpSocket> sock(s);
    get(l, z, x, y, s, [this, sock, k](const QImage &img) {
        if (!sock) return;
        if (img.isNull()) { reply(sock, 502, "text/plain", "upstream failed"); return; }
        if (const Encoded *e = m_encoded.object(k)) { reply(sock, 200, e->type, e->body); return; }
        // Dark (or bytes since evicted): encode on the pool
        m_pool.start([this, sock, k, img] {
            QByteArray png;
            QBuffer b(&png); b.open(QIODevice::WriteOnly);
            img.save(&b, "PNG");
            QMetaObject::invokeMethod(this, [this, sock, k, png] {
                m_encoded.insert(k, new Encoded{png, "image/png"}, png.size());
                if (sock) reply(sock, 200, "image/png", png);
            }, Qt::QueuedConnection);
        });
    });
}

// ── Contour tiles ─────────────────────────────────────────────────────────────
// The elevation under a tile comes from the Terrarium tile at min(z, 15) that holds it, plus the tiles right of and
// below that one, so the grid reaches one sample past the tile's edges and lines meet their neighbours'.
void TileSource::contourTile(int z, int x, int y)
{
    const QString k = key(Contours, z, x, y);
    const int dz = qMin(z, 15), sh = z - dz, n = 1 << dz;
    const int tx = x >> sh, ty = y >> sh;
    struct Pending { QList<QImage> dem{QImage(), QImage(), QImage(), QImage()}; int left = 4; };
    auto pend = std::make_shared<Pending>();
    const QPoint offs[4] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int i = 0; i < 4; ++i) {
        const int ax = ((tx + offs[i].x()) % n + n) % n, ay = qMin(ty + offs[i].y(), n - 1);
        get(Dem, dz, ax, ay, this, [this, pend, i, k, z, x, y](const QImage &img) {
            pend->dem[i] = img;
            if (--pend->left) return;
            if (pend->dem[0].isNull()) { deliver(k, QImage()); return; }      // no elevation here: nothing to draw
            m_pool.start([this, pend, k, z, x, y] {
                QImage out = renderContours(pend->dem, z, x, y);
                if (out.isNull()) { out = QImage(256, 256, QImage::Format_ARGB32_Premultiplied); out.fill(Qt::transparent); }   // flat / sea: no lines
                QMetaObject::invokeMethod(this, [this, k, out] { deliver(k, out); }, Qt::QueuedConnection);
            });
        });
    }
}

QImage TileSource::renderContours(const QList<QImage> &dem, int z, int x, int y)
{
    const int dz = qMin(z, 15), sh = z - dz, scale = 1 << sh;
    const double win = 256.0 / scale;                        // DEM pixels this tile spans (256 … 1/128)
    const double ox = (x % scale) * win, oy = (y % scale) * win;
    auto elev = [&](double px, double py) -> double {        // metres at DEM pixel (px, py), py/px may run into the neighbours
        const int ix = int(px), iy = int(py);
        const int q = (ix >= 256 ? 1 : 0) + (iy >= 256 ? 2 : 0);
        const QImage &im = dem[q].isNull() ? dem[0] : dem[q];
        const int cx = qBound(0, dem[q].isNull() ? qMin(ix, 255) : ix % 256, 255), cy = qBound(0, dem[q].isNull() ? qMin(iy, 255) : iy % 256, 255);
        const QRgb c = im.pixel(cx, cy);
        return qRed(c) * 256.0 + qGreen(c) + qBlue(c) / 256.0 - 32768.0;
    };
    // Sample grid: at least 64 cells across (bilinear between DEM pixels when zoomed past 15, for smooth lines)
    const int cells = qMax(64, int(std::ceil(win)));
    const double step = win / cells, px = 256.0 / cells;
    QList<double> g((cells + 1) * (cells + 1));
    double lo = 1e9, hi = -1e9;
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i) {
            const double sx = ox + i * step - 0.5, sy = oy + j * step - 0.5;     // pixel centres
            const double fx = std::floor(sx), fy = std::floor(sy), ax = sx - fx, ay = sy - fy;
            const double x0 = qMax(0.0, fx), y0 = qMax(0.0, fy), x1 = qMin(511.0, fx + 1), y1 = qMin(511.0, fy + 1);
            const double v = (elev(x0, y0) * (1 - ax) + elev(x1, y0) * ax) * (1 - ay) + (elev(x0, y1) * (1 - ax) + elev(x1, y1) * ax) * ay;
            g[j * (cells + 1) + i] = v; lo = qMin(lo, v); hi = qMax(hi, v);
        }
    if (hi - lo < 0.01 || lo < -500) return {};             // flat (or sea / no data): no lines
    // Round intervals in feet, finer as the map zooms in; every 5th is an index line with its elevation
    const double ft = z <= 14 ? 100 : z == 15 ? 50 : z == 16 ? 20 : z == 17 ? 10 : 5;   // sparse zoomed out: lines, not texture
    const double m2ft = 3.28084;
    QPainterPath minor, index;
    struct Lab { QPointF at; double ang; int feet; };
    QList<Lab> labels;
    const int first = int(std::ceil(lo * m2ft / ft)), last = int(std::floor(hi * m2ft / ft));
    if (last - first > 400) return {};
    for (int li = first; li <= last; ++li) {
        const int feet = int(li * ft);
        const double L = feet / m2ft;
        const bool idx = feet % int(ft * 5) == 0;
        QPainterPath &path = idx ? index : minor;
        double bestDx = 1e9; QLineF bestSeg;
        for (int j = 0; j < cells; ++j)
            for (int i = 0; i < cells; ++i) {
                const double a = g[j * (cells + 1) + i], b = g[j * (cells + 1) + i + 1], c = g[(j + 1) * (cells + 1) + i + 1], d = g[(j + 1) * (cells + 1) + i];
                const int code = (a >= L) | (b >= L) << 1 | (c >= L) << 2 | (d >= L) << 3;
                if (code == 0 || code == 15) continue;
                auto lerp = [&](double v0, double v1) { return (L - v0) / (v1 - v0); };
                const double X = i * px, Y = j * px;
                const QPointF top(X + lerp(a, b) * px, Y), right(X + px, Y + lerp(b, c) * px), bottom(X + lerp(d, c) * px, Y + px), left(X, Y + lerp(a, d) * px);
                QLineF segs[2]; int ns = 0;
                switch (code) {
                case 1: case 14: segs[ns++] = {left, top}; break;
                case 2: case 13: segs[ns++] = {top, right}; break;
                case 3: case 12: segs[ns++] = {left, right}; break;
                case 4: case 11: segs[ns++] = {right, bottom}; break;
                case 6: case 9:  segs[ns++] = {top, bottom}; break;
                case 7: case 8:  segs[ns++] = {left, bottom}; break;
                case 5: case 10: {                                          // saddle: the centre value decides
                    const bool up = (a + b + c + d) / 4 >= L;
                    if ((code == 5) == up) { segs[ns++] = {left, bottom}; segs[ns++] = {top, right}; }
                    else { segs[ns++] = {left, top}; segs[ns++] = {right, bottom}; }
                    break;
                }
                }
                for (int si = 0; si < ns; ++si) {
                    path.moveTo(segs[si].p1()); path.lineTo(segs[si].p2());
                    if (idx) {                                              // its label: where the line crosses the middle column
                        const double mx = (segs[si].p1().x() + segs[si].p2().x()) / 2, dxm = std::abs(mx - 128);
                        if (dxm < bestDx && segs[si].length() > 0.5) { bestDx = dxm; bestSeg = segs[si]; }
                    }
                }
            }
        if (idx && bestDx < 40) {
            double ang = std::atan2(bestSeg.dy(), bestSeg.dx()) * 180 / M_PI;
            if (ang > 90) ang -= 180; else if (ang < -90) ang += 180;     // upright
            labels.append({bestSeg.center(), ang, feet});
        }
    }
    QImage img(256, 256, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0, 0, 0, 60), 2.2)); p.drawPath(index);   // a soft dark edge: reads on bright fields and snow
    p.setPen(QPen(QColor(255, 214, 150, 95), 0.8)); p.drawPath(minor);
    p.setPen(QPen(QColor(255, 214, 150, 220), 1.5)); p.drawPath(index);
    QFont f; f.setPixelSize(10); f.setBold(true); p.setFont(f);
    QList<QPointF> placed;
    for (const Lab &l : std::as_const(labels)) {
        bool near = false;
        for (const QPointF &q : std::as_const(placed)) if (QLineF(q, l.at).length() < 16) near = true;
        if (near) continue;
        placed.append(l.at);
        const QString t = QStringLiteral("%1 ft").arg(l.feet);
        p.save(); p.translate(l.at); p.rotate(l.ang);
        QPainterPath tp; tp.addText(-p.fontMetrics().horizontalAdvance(t) / 2.0, 3.5, f, t);
        p.setPen(QPen(QColor(0, 0, 0, 200), 3)); p.setBrush(Qt::NoBrush); p.drawPath(tp);
        p.setPen(Qt::NoPen); p.setBrush(QColor(255, 226, 180)); p.drawPath(tp);
        p.restore();
    }
    p.end();
    return img;
}

// ── Route heat map tiles ──────────────────────────────────────────────────────
// The widget's Canvas stroked the whole route (a 14 px translucent glow + core, thousands of points) on the shell's
// GUI thread at every settle: 60-300 ms. Here it is drawn per tile on the pool, once per route generation, and the
// widget shows it as another tile layer the GPU scales while zooming. Same look: glow, core, and node dots from z13.
void TileSource::serveHeat(QTcpSocket *s, quint64 gen, int z, int x, int y)
{
    if (!m_heatFeed || z < 1 || z > 22 || x >= (1 << z) || y >= (1 << z)) { reply(s, 404, "text/plain", "no such tile"); return; }
    if (!m_heat || m_heat->gen != gen) {                       // the route moved on: one snapshot per generation
        HeatInput in = m_heatFeed(m_heat ? m_heat->gen : 0);
        if (!m_heat || in.gen != m_heat->gen) {
            auto snap = std::make_shared<HeatSnap>();
            snap->gen = in.gen;
            snap->points = in.points;
            for (const QList<QPointF> &l : std::as_const(in.lines)) {
                if (l.size() < 2) continue;
                double x0 = 1, y0 = 1, x1 = 0, y1 = 0;
                for (const QPointF &p : l) { x0 = qMin(x0, p.x()); x1 = qMax(x1, p.x()); y0 = qMin(y0, p.y()); y1 = qMax(y1, p.y()); }
                snap->lines.append({QRectF(QPointF(x0, y0), QPointF(x1, y1)), l});
            }
            m_heat = snap;
        }
    }
    // An older generation than ours is answered with ours: the widget's next poll brings the new number
    const QString k = QStringLiteral("h/%1/%2/%3/%4").arg(m_heat->gen).arg(z).arg(x).arg(y);
    if (const Encoded *e = m_encoded.object(k)) { reply(s, 200, e->type, e->body); return; }
    QPointer<QTcpSocket> sock(s);
    m_pool.start([this, sock, k, snap = m_heat, z, x, y] {
        static const QByteArray empty = [] {                   // a transparent tile for the (many) without route
            QImage none(256, 256, QImage::Format_ARGB32_Premultiplied); none.fill(Qt::transparent);
            QByteArray png; QBuffer b(&png); b.open(QIODevice::WriteOnly); none.save(&b, "PNG"); return png;
        }();
        QByteArray png = renderHeat(*snap, z, x, y);
        if (png.isEmpty()) png = empty;
        QMetaObject::invokeMethod(this, [this, sock, k, png] {
            m_encoded.insert(k, new Encoded{png, "image/png"}, png.size());
            if (sock) reply(sock, 200, "image/png", png);
        }, Qt::QueuedConnection);
    });
}

// Zoomed out, the route (glow + core) shows where you travel. Zoomed in, where APs get pinpointed, a stroke through
// Route density heatmap rasterization: round-capped, antialiased density strokes + fix stamps
static void addHeatStroke(QImage &img, const QPointF &a, const QPointF &b, double hw, int v)
{
    const int W = img.width(), H = img.height();
    const double dx = b.x() - a.x(), dy = b.y() - a.y(), len2 = dx * dx + dy * dy, m = hw + 1;
    const int y0 = qMax(0, int(std::floor(qMin(a.y(), b.y()) - m))), y1 = qMin(H - 1, int(std::ceil(qMax(a.y(), b.y()) + m)));
    for (int y = y0; y <= y1; ++y) {
        const double py = y + 0.5;
        double xa = qMin(a.x(), b.x()), xb = qMax(a.x(), b.x());
        if (std::abs(dy) > 1e-9) {
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

static const QVector<QRgb> kHeatRamp = []{
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
        r[i] = qPremultiply(r[i]);
    }
    return r;
}();

// True continuous density heatmap for route tiles: renders smooth density accumulation into Alpha8
// and colors with kHeatRamp, keeping map and building detail completely legible
QByteArray TileSource::renderHeat(const HeatSnap &snap, int z, int x, int y)
{
    const double n = double(1 << z) * 256, ox = x * 256.0, oy = y * 256.0, pad = 16;
    const double bx0 = (ox - pad) / n, by0 = (oy - pad) / n, bx1 = (ox + 256 + pad) / n, by1 = (oy + 256 + pad) / n;

    QImage alphaMap(256, 256, QImage::Format_Alpha8);
    alphaMap.fill(0);
    bool hasData = false;

    // 1. Draw route segments into alphaMap with wide ambient glow and warm core
    const double glowW = (z <= 15) ? 7.0 : 10.0;
    const double coreW = (z <= 15) ? 2.5 : 3.5;

    for (const HeatLine &hl : snap.lines) {
        if (hl.box.right() < bx0 || hl.box.left() > bx1 || hl.box.bottom() < by0 || hl.box.top() > by1) continue;
        const QList<QPointF> &pts = hl.pts;
        if (pts.size() < 2) continue;
        QPointF last(pts[0].x() * n - ox, pts[0].y() * n - oy);
        for (int i = 1; i < pts.size(); ++i) {
            const QPointF q(pts[i].x() * n - ox, pts[i].y() * n - oy);
            if (i < pts.size() - 1 && std::abs(q.x() - last.x()) + std::abs(q.y() - last.y()) < 1.2) continue;
            const bool out = (last.x() < -pad && q.x() < -pad) || (last.x() > 256 + pad && q.x() > 256 + pad)
                          || (last.y() < -pad && q.y() < -pad) || (last.y() > 256 + pad && q.y() > 256 + pad);
            if (!out) {
                addHeatStroke(alphaMap, last, q, glowW, 30);
                addHeatStroke(alphaMap, last, q, coreW, 70);
                hasData = true;
            }
            last = q;
        }
    }

    // 2. Add fix point stamps (vantage points & fix density)
    const double r = qBound(6.0, 18.0 - z * 0.5, 14.0);
    const int R = int(std::ceil(r)), side = 2 * R + 1;
    QList<uchar> stamp(side * side);
    for (int dy = -R; dy <= R; ++dy) {
        for (int dx = -R; dx <= R; ++dx) {
            const double d = std::hypot(dx, dy) / r;
            stamp[(dy + R) * side + dx + R] = uchar(qRound(d < 0.6 ? 28 - 16 * d / 0.6 : d < 1.0 ? 12 * (1.0 - d) / 0.4 : 0.0));
        }
    }

    for (const QPointF &pt : snap.points) {
        if (pt.x() < bx0 || pt.x() > bx1 || pt.y() < by0 || pt.y() > by1) continue;
        const int px = qRound(pt.x() * n - ox), py = qRound(pt.y() * n - oy);
        if (px < -R || px > 256 + R || py < -R || py > 256 + R) continue;
        const int x0 = qMax(0, px - R), x1 = qMin(255, px + R);
        for (int rowY = qMax(0, py - R); rowY <= qMin(255, py + R); ++rowY) {
            uchar *row = alphaMap.scanLine(rowY);
            const uchar *k = stamp.constData() + (rowY - py + R) * side + (x0 - px + R);
            for (int colX = x0; colX <= x1; ++colX, ++k) {
                row[colX] = uchar(qMin(255, row[colX] + *k));
            }
        }
        hasData = true;
    }

    if (!hasData) return {};

    // 3. Colorize through true continuous heat gradient ramp
    QImage colorized(256, 256, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 256; ++y) {
        const uchar *srcLine = alphaMap.constScanLine(y);
        QRgb *dstLine = reinterpret_cast<QRgb *>(colorized.scanLine(y));
        for (int x = 0; x < 256; ++x) dstLine[x] = kHeatRamp[srcLine[x]];
    }

    QByteArray png;
    QBuffer b(&png);
    b.open(QIODevice::WriteOnly);
    colorized.save(&b, "PNG");
    return png;
}

// ── Offline prefetch ──────────────────────────────────────────────────────────
void TileSource::prefetch(double lat, double lon, Layer, int zmin, int zmax, double radiusKm, bool force)
{
    const Layer l = Topo;                                  // the one source whose terms allow it (see the header)
    if (prefetching()) return;
    // Once per stop: skip if we did this area less than 30 min ago
    if (!force && m_prefetchAt.isValid() && m_prefetchAt.secsTo(QDateTime::currentDateTime()) < 1800
        && std::hypot((lat - m_prefetchLat) * 111.32, (lon - m_prefetchLon) * 111.32 * std::cos(qDegreesToRadians(lat))) < radiusKm / 2) return;
    m_prefetchAt = QDateTime::currentDateTime(); m_prefetchLat = lat; m_prefetchLon = lon;
    m_prefetchQueue.clear();
    const double dLat = radiusKm / 111.32, dLon = radiusKm / (111.32 * std::cos(qDegreesToRadians(lat)));
    auto tileX = [](double lo, int z) { return int(std::floor((lo + 180.0) / 360.0 * (1 << z))); };
    auto tileY = [](double la, int z) {
        const double r = qDegreesToRadians(qBound(-85.05112878, la, 85.05112878));
        return int(std::floor((1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / M_PI) / 2.0 * (1 << z)));
    };
    const int cap = 300;                                   // OpenTopoMap is volunteer-run: no mass downloads
    for (int z = qMax(2, zmin); z <= qMin(zmax, maxZoom(l)); ++z) {
        const int x0 = tileX(lon - dLon, z), x1 = tileX(lon + dLon, z), y0 = tileY(lat + dLat, z), y1 = tileY(lat - dLat, z);
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                m_prefetchQueue.append({l, z, ((x % (1 << z)) + (1 << z)) % (1 << z), qBound(0, y, (1 << z) - 1)});
    }
    // Centre first, so the useful tiles arrive even if the link drops mid-way
    std::stable_sort(m_prefetchQueue.begin(), m_prefetchQueue.end(), [&](const TileRef &a, const TileRef &b) {
        auto d = [&](const TileRef &t) { const double n = 1 << t.z; return std::hypot((t.x + 0.5) / n - (lon + 180.0) / 360.0, (t.y + 0.5) / n - (1.0 - std::log(std::tan(qDegreesToRadians(lat)) + 1.0 / std::cos(qDegreesToRadians(lat))) / M_PI) / 2.0) * n; };
        return a.z != b.z ? a.z < b.z : d(a) < d(b);
    });
    if (m_prefetchQueue.size() > cap) m_prefetchQueue.resize(cap);   // the outer ring of the closest zoom goes
    m_prefetchTotal = m_prefetchQueue.size(); m_prefetchDone = m_prefetchFetched = m_prefetchFailed = m_prefetchActive = 0;
    emit prefetchProgress(0, m_prefetchTotal);
    pumpPrefetch();
}

void TileSource::pumpPrefetch()
{
    while (m_prefetchActive < 1 && !m_prefetchQueue.isEmpty()) {
        const TileRef t = m_prefetchQueue.takeFirst();
        QNetworkRequest req{QUrl(upstreamUrl(t.l, t.z, t.x, t.y))};
        req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(TILE_UA));
        req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
        req.setTransferTimeout(20000);
        ++m_prefetchActive;
        QNetworkReply *rep = m_nam.get(req);
        connect(rep, &QNetworkReply::finished, this, [this, rep] {
            rep->deleteLater();
            --m_prefetchActive; ++m_prefetchDone;
            const bool fromCache = rep->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();
            if (rep->error() != QNetworkReply::NoError) ++m_prefetchFailed;
            else if (!fromCache) ++m_prefetchFetched;
            rep->readAll();                                    // the disk cache keeps the bytes
            emit prefetchProgress(m_prefetchDone, m_prefetchTotal);
            if (m_prefetchQueue.isEmpty() && m_prefetchActive == 0) {
                const int f = m_prefetchFetched, fl = m_prefetchFailed;
                m_prefetchTotal = m_prefetchDone = 0;
                emit prefetchFinished(f, fl);
                return;
            }
            QTimer::singleShot(fromCache ? 0 : 250, this, &TileSource::pumpPrefetch);   // don't hammer upstream
        });
    }
}
