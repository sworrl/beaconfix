#pragma once
#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QThreadPool>
#include <functional>
#include <memory>

// Map tiles for the window and the Plasma widget, fetched once with a proper
// User-Agent (OSM's tile policy blocks generic clients such as QML's Image),
// kept in a shared disk cache for offline stretches, and night-filtered in one
// place. The widget reads them from a localhost-only HTTP endpoint:
//   GET http://127.0.0.1:<port>/t/<layer>/<z>/<x>/<y>.png     layer 0..3, L = satellite labels, or another
//       satellite source: C = Esri Clarity, U = USGS National Map, V = NASA GIBS VIIRS (yesterday's pass),
//       or a hybrid overlay: R = Esri roads, K = USGS contours (transparent)
//   GET http://127.0.0.1:<port>/h/<gen>/<z>/<x>/<y>.png       the route heat map (transparent), drawn here
// It serves only these tiles — it is not a general proxy. Decoding, the night filter
// and PNG encoding run on a small thread pool, never on the GUI thread.
//
// Tile policies (checked 2026-10): tile.openstreetmap.org forbids offline use and any
// pre-emptive fetching of tiles the user is not viewing, and Esri's World Imagery is
// licensed for offline use only through its export service, so prefetch() warms only
// OpenTopoMap (z ≤ 17), gently. Tiles the user has actually viewed stay in the disk cache.
class QTcpSocket;

class TileSource : public QObject {
    Q_OBJECT
public:
    enum Layer { Dark, Streets, Satellite, Topo, Labels, SatClarity, SatUsgs, SatViirs, Roads, Contours, Dem };
    // Contours are drawn here, from the AWS Terrain Tiles (Terrarium-encoded elevation, keyless; USGS 3DEP in the US,
    // to z15): USGS's own contour service renders on demand and times out too often to sit under a map
    // Satellite is shown as a hybrid: imagery, then USGS contours (US; elevation in feet), Esri roads, Esri place labels
    // Satellite imagery, all free and keyless (checked 2026-10):
    //   Esri World Imagery — Vantor/Maxar 30-50 cm, the newest high-resolution imagery anyone serves free
    //     (it equals Esri's latest Wayback release); Clarity — the same, older but sharper-processed;
    //   USGS National Map — US public-domain orthoimagery (NAIP), to z16;
    //   NASA GIBS VIIRS (NOAA-20) true colour — yesterday's global pass, ~375 m, to z9: the newest of all.
    enum SatSource { SatEsri, SatEsriClarity, SatUsgsNaip, SatNasaViirs };
    static Layer satLayer(SatSource s) { return s == SatEsriClarity ? SatClarity : s == SatUsgsNaip ? SatUsgs : s == SatNasaViirs ? SatViirs : Satellite; }
    static QString satName(SatSource s);
    using Done = std::function<void(const QImage &)>;
    // The route heat map's input: polylines in Web-Mercator [0,1]², rebuilt only when gen changes
    struct HeatInput { quint64 gen = 0; QList<QList<QPointF>> lines; QList<QPointF> points; };   // points: every fix (vantage points)
    using HeatFeed = std::function<HeatInput(quint64 haveGen)>;   // haveGen = what we hold; lines empty when unchanged
    void setHeatFeed(HeatFeed f) { m_heatFeed = std::move(f); }

    explicit TileSource(QObject *parent = nullptr);
    static int     maxZoom(Layer l);
    static QString upstreamUrl(Layer l, int z, int x, int y);
    static QImage  nightMode(const QImage &src);

    // Processed tile (night filter applied for Dark); null image on failure. Answers at
    // once from memory, else later on the GUI thread.
    void get(Layer l, int z, int x, int y, QObject *context, Done done);
    // context no longer wants it (scrolled away, zoomed past): drop its callback, and the
    // download too when nobody else is waiting
    void cancel(Layer l, int z, int x, int y, QObject *context);

    bool    listen();                      // localhost only
    QString baseUrl() const;               // "" when not serving

    // Offline: warm the disk cache with the OpenTopoMap tiles around a point (zoom zmin..zmax
    // within radiusKm), politely (one at a time, spaced out, capped) so a stop's surroundings
    // keep rendering when the link is gone; the map falls back to them when the chosen layer
    // can't load. l is ignored: OSM and Esri tiles may not be prefetched (see above).
    void    prefetch(double lat, double lon, Layer l, int zmin = 10, int zmax = 15, double radiusKm = 10, bool force = false);
    bool    prefetching() const { return m_prefetchTotal > 0 && m_prefetchDone < m_prefetchTotal; }
    int     prefetchDone() const { return m_prefetchDone; }
    int     prefetchTotal() const { return m_prefetchTotal; }

signals:
    void prefetchProgress(int done, int total);
    void prefetchFinished(int fetched, int failed);

private:
    struct Waiter { QPointer<QObject> ctx; Done done; };
    struct Encoded { QByteArray body, type; };
    void serve(QTcpSocket *s);
    void deliver(const QString &k, const QImage &img);
    static void reply(QTcpSocket *s, int code, const QByteArray &type, const QByteArray &body);
    static QString key(Layer l, int z, int x, int y);
    static QString viirsDate();            // yesterday, UTC: today's pass is still incomplete
    void serveHeat(QTcpSocket *s, quint64 gen, int z, int x, int y);
    struct HeatLine { QRectF box; QList<QPointF> pts; };
    struct HeatSnap { quint64 gen = 0; QList<HeatLine> lines; QList<QPointF> points; };
    static QByteArray renderHeat(const HeatSnap &snap, int z, int x, int y);   // PNG, or empty when the tile has no route
    void contourTile(int z, int x, int y);                                       // fetch the elevation it needs, then draw
    static QImage renderContours(const QList<QImage> &dem, int z, int x, int y);  // dem: tile, right, below, below-right
    HeatFeed m_heatFeed;
    std::shared_ptr<const HeatSnap> m_heat;

    QNetworkAccessManager m_nam;
    QCache<QString, QImage> m_images;      // processed, by "layer/z/x/y"
    QCache<QString, Encoded> m_encoded;    // for the widget: upstream bytes as they came (PNG / JPEG), PNG for Dark
    QHash<QString, QList<Waiter>> m_inflight;
    QHash<QString, QPointer<QNetworkReply>> m_replies;   // downloads in flight, by key
    QTcpServer m_server;
    struct TileRef { Layer l; int z, x, y; };
    void pumpPrefetch();
    QList<TileRef> m_prefetchQueue;
    int m_prefetchDone = 0, m_prefetchTotal = 0, m_prefetchFetched = 0, m_prefetchFailed = 0, m_prefetchActive = 0;
    QDateTime m_prefetchAt; double m_prefetchLat = 0, m_prefetchLon = 0;
    QThreadPool m_pool;                    // last: its destructor waits for the workers before anything else goes
};
