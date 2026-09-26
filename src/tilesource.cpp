#include "tilesource.h"
#include <QBuffer>
#include <QColor>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
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
    m_images.setMaxCost(900);
    m_png.setMaxCost(96 * 1024 * 1024);
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *s = m_server.nextPendingConnection()) {
            connect(s, &QTcpSocket::readyRead, this, [this, s] { serve(s); });
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        }
    });
}

int TileSource::maxZoom(Layer l) { return l == Topo ? 17 : 19; }

// All keyless: OSM (Dark = OSM through nightMode()), Esri imagery + place labels, OpenTopoMap
QString TileSource::upstreamUrl(Layer l, int z, int x, int y)
{
    switch (l) {
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
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            float h, s, l, a;
            QColor::fromRgb(row[x]).getHslF(&h, &s, &l, &a);
            float l2 = lut[qBound(0, int(l * 255.0f + 0.5f), 255)], s2 = s * 0.75f;
            if (s > 0.6f && l > 0.65f && l < 0.95f) { l2 = 0.24f; s2 = s * 0.55f; }   // road fills stay visible
            const QColor o = QColor::fromHslF(h < 0 ? 0 : h, s2, l2);
            row[x] = qRgba(int(o.red() * 0.90), int(o.green() * 0.96), qMin(255, int(o.blue() * 1.05 + 6)), qAlpha(row[x]));
        }
    }
    return img;
}

void TileSource::get(Layer l, int z, int x, int y, QObject *context, Done done)
{
    const QString k = QStringLiteral("%1/%2/%3/%4").arg(int(l)).arg(z).arg(x).arg(y);
    if (QImage *img = m_images.object(k)) { done(*img); return; }
    auto &waiters = m_inflight[k];
    waiters.append({QPointer<QObject>(context), std::move(done)});
    if (waiters.size() > 1) return;                     // already on its way

    QNetworkRequest req{QUrl(upstreamUrl(l, z, x, y))};
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(TILE_UA));
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    req.setTransferTimeout(20000);
    QNetworkReply *rep = m_nam.get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep, k, l] {
        rep->deleteLater();
        QImage img;
        if (rep->error() == QNetworkReply::NoError && img.loadFromData(rep->readAll())) {
            if (l == Dark) img = nightMode(img);
            m_images.insert(k, new QImage(img));
        }
        const QList<Waiter> ws = m_inflight.take(k);
        for (const Waiter &w : ws)
            if (w.ctx) w.done(img);
    });
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

    static const QRegularExpression re(QStringLiteral("^GET /t/(L|[0-3])/(\\d{1,2})/(\\d{1,7})/(\\d{1,7})\\.png HTTP/1\\.[01]\r\n"));
    const auto m = re.match(QString::fromLatin1(buf));
    if (!m.hasMatch()) { reply(s, 404, "text/plain", "not found"); return; }
    const Layer l = m.captured(1) == QLatin1String("L") ? Labels : Layer(m.captured(1).toInt());
    const int z = m.captured(2).toInt(), x = m.captured(3).toInt(), y = m.captured(4).toInt();
    if (z > maxZoom(l) || x >= (1 << z) || y >= (1 << z)) { reply(s, 404, "text/plain", "no such tile"); return; }

    const QString k = QStringLiteral("%1/%2/%3/%4").arg(int(l)).arg(z).arg(x).arg(y);
    if (QByteArray *png = m_png.object(k)) { reply(s, 200, "image/png", *png); return; }
    QPointer<QTcpSocket> sock(s);
    get(l, z, x, y, s, [this, sock, k](const QImage &img) {
        if (!sock) return;
        if (img.isNull()) { reply(sock, 502, "text/plain", "upstream failed"); return; }
        QByteArray png;
        QBuffer b(&png); b.open(QIODevice::WriteOnly);
        img.save(&b, "PNG");
        m_png.insert(k, new QByteArray(png), png.size());
        reply(sock, 200, "image/png", png);
    });
}

// ── Offline prefetch ──────────────────────────────────────────────────────────
void TileSource::prefetch(double lat, double lon, Layer l, int zmin, int zmax, double radiusKm, bool force)
{
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
    const int cap = 400;                                   // keep well inside the tile servers' comfort zone
    for (int z = qMax(2, zmin); z <= qMin(zmax, maxZoom(l)); ++z) {
        const int x0 = tileX(lon - dLon, z), x1 = tileX(lon + dLon, z), y0 = tileY(lat + dLat, z), y1 = tileY(lat - dLat, z);
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) {
                if (m_prefetchQueue.size() >= cap) break;
                m_prefetchQueue.append({l, z, ((x % (1 << z)) + (1 << z)) % (1 << z), qBound(0, y, (1 << z) - 1)});
                if (l == Satellite && m_prefetchQueue.size() < cap) m_prefetchQueue.append({Labels, z, ((x % (1 << z)) + (1 << z)) % (1 << z), qBound(0, y, (1 << z) - 1)});
            }
    }
    // Centre first, so the useful tiles arrive even if the link drops mid-way
    std::stable_sort(m_prefetchQueue.begin(), m_prefetchQueue.end(), [&](const TileRef &a, const TileRef &b) {
        auto d = [&](const TileRef &t) { const double n = 1 << t.z; return std::hypot((t.x + 0.5) / n - (lon + 180.0) / 360.0, (t.y + 0.5) / n - (1.0 - std::log(std::tan(qDegreesToRadians(lat)) + 1.0 / std::cos(qDegreesToRadians(lat))) / M_PI) / 2.0) * n; };
        return a.z != b.z ? a.z < b.z : d(a) < d(b);
    });
    m_prefetchTotal = m_prefetchQueue.size(); m_prefetchDone = m_prefetchFetched = m_prefetchFailed = m_prefetchActive = 0;
    emit prefetchProgress(0, m_prefetchTotal);
    pumpPrefetch();
}

void TileSource::pumpPrefetch()
{
    while (m_prefetchActive < 2 && !m_prefetchQueue.isEmpty()) {
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
            QTimer::singleShot(fromCache ? 0 : 150, this, &TileSource::pumpPrefetch);   // don't hammer upstream
        });
    }
}
