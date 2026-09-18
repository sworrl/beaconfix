#include "tilemap.h"
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QStandardPaths>
#include <QWheelEvent>
#include <QtMath>

static const int TILE = 256;

TileMap::TileMap(QWidget *parent) : QWidget(parent)
{
    auto *cache = new QNetworkDiskCache(this);
    cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/tiles");
    cache->setMaximumCacheSize(200 * 1024 * 1024);
    m_nam.setCache(cache);
    setMinimumSize(320, 240);
}

void TileMap::setFix(double lat, double lon, double accuracyM, bool valid)
{
    m_lat = lat; m_lon = lon; m_acc = accuracyM; m_valid = valid;
    update();
}

void TileMap::setZoom(int z) { m_zoom = qBound(2, z, 19); update(); }

void TileMap::wheelEvent(QWheelEvent *e)
{
    setZoom(m_zoom + (e->angleDelta().y() > 0 ? 1 : -1));
}

QString TileMap::key(int z, int x, int y) const { return QStringLiteral("%1/%2/%3").arg(z).arg(x).arg(y); }

void TileMap::ensureTile(int z, int x, int y)
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
        QPixmap px;
        if (rep->error() == QNetworkReply::NoError && px.loadFromData(rep->readAll()))
            m_tiles.insert(k, px);
        update();
    });
}

void TileMap::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0xe8, 0xe4, 0xdc));
    if (!m_valid) {
        p.setPen(palette().color(QPalette::Text));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("No fix yet"));
        return;
    }
    const int n = 1 << m_zoom;
    const double latR = qDegreesToRadians(m_lat);
    const double tx = (m_lon + 180.0) / 360.0 * n;
    const double ty = (1.0 - std::log(std::tan(latR) + 1.0 / std::cos(latR)) / M_PI) / 2.0 * n;
    const double cx = width() / 2.0, cy = height() / 2.0;

    const int x0 = int(std::floor(tx - cx / TILE)), x1 = int(std::floor(tx + cx / TILE));
    const int y0 = int(std::floor(ty - cy / TILE)), y1 = int(std::floor(ty + cy / TILE));
    for (int x = x0; x <= x1; ++x) {
        for (int y = y0; y <= y1; ++y) {
            if (y < 0 || y >= n) continue;
            const int wx = ((x % n) + n) % n;
            const QPointF pos(cx + (x - tx) * TILE, cy + (y - ty) * TILE);
            const QString k = key(m_zoom, wx, y);
            if (m_tiles.contains(k)) p.drawPixmap(pos, m_tiles.value(k));
            else ensureTile(m_zoom, wx, y);
        }
    }

    p.setRenderHint(QPainter::Antialiasing);
    if (m_acc > 0) {
        const double mpp = 156543.03392 * std::cos(latR) / n;   // metres per pixel
        const double r = qBound(6.0, m_acc / mpp, 4000.0);
        p.setPen(QPen(QColor(30, 120, 255, 180), 2));
        p.setBrush(QColor(30, 120, 255, 40));
        p.drawEllipse(QPointF(cx, cy), r, r);
    }
    p.setPen(QPen(Qt::white, 2));
    p.setBrush(QColor(220, 40, 40));
    p.drawEllipse(QPointF(cx, cy), 7, 7);

    const QString attr = QStringLiteral("© OpenStreetMap contributors · z%1").arg(m_zoom);
    QFont f = p.font(); f.setPointSizeF(f.pointSizeF() * 0.8); p.setFont(f);
    const QRect tr = p.fontMetrics().boundingRect(attr).adjusted(-6, -2, 6, 2);
    QRect box(width() - tr.width() - 4, height() - tr.height() - 4, tr.width(), tr.height());
    p.setPen(Qt::NoPen); p.setBrush(QColor(255, 255, 255, 200)); p.drawRect(box);
    p.setPen(Qt::black); p.drawText(box, Qt::AlignCenter, attr);
}
