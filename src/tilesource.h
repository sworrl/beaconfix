#pragma once
#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <functional>

// Map tiles for the window and the Plasma widget, fetched once with a proper
// User-Agent (OSM's tile policy blocks generic clients such as QML's Image),
// kept in a shared disk cache for offline stretches, and night-filtered in one
// place. The widget reads them from a localhost-only HTTP endpoint:
//   GET http://127.0.0.1:<port>/t/<layer>/<z>/<x>/<y>.png     layer 0..3, or L = satellite labels
// It serves only these tiles — it is not a general proxy.
class QTcpSocket;

class TileSource : public QObject {
    Q_OBJECT
public:
    enum Layer { Dark, Streets, Satellite, Topo, Labels };
    using Done = std::function<void(const QImage &)>;

    explicit TileSource(QObject *parent = nullptr);
    static int     maxZoom(Layer l);
    static QString upstreamUrl(Layer l, int z, int x, int y);
    static QImage  nightMode(const QImage &src);

    // Processed tile (night filter applied for Dark); null image on failure
    void get(Layer l, int z, int x, int y, QObject *context, Done done);

    bool    listen();                      // localhost only
    QString baseUrl() const;               // "" when not serving

    // Offline: warm the disk cache with the tiles around a point (zoom zmin..zmax within
    // radiusKm), politely (two at a time, spaced out, capped) so a stop's surroundings
    // keep rendering when the link is gone. Only cache misses touch the network.
    void    prefetch(double lat, double lon, Layer l, int zmin = 10, int zmax = 15, double radiusKm = 10, bool force = false);
    bool    prefetching() const { return m_prefetchTotal > 0 && m_prefetchDone < m_prefetchTotal; }
    int     prefetchDone() const { return m_prefetchDone; }
    int     prefetchTotal() const { return m_prefetchTotal; }

signals:
    void prefetchProgress(int done, int total);
    void prefetchFinished(int fetched, int failed);

private:
    struct Waiter { QPointer<QObject> ctx; Done done; };
    void serve(QTcpSocket *s);
    static void reply(QTcpSocket *s, int code, const QByteArray &type, const QByteArray &body);

    QNetworkAccessManager m_nam;
    QCache<QString, QImage> m_images;      // processed, by "layer/z/x/y"
    QCache<QString, QByteArray> m_png;     // encoded for the widget
    QHash<QString, QList<Waiter>> m_inflight;
    QTcpServer m_server;
    struct TileRef { Layer l; int z, x, y; };
    void pumpPrefetch();
    QList<TileRef> m_prefetchQueue;
    int m_prefetchDone = 0, m_prefetchTotal = 0, m_prefetchFetched = 0, m_prefetchFailed = 0, m_prefetchActive = 0;
    QDateTime m_prefetchAt; double m_prefetchLat = 0, m_prefetchLon = 0;
};
