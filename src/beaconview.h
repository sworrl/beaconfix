#pragma once
#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QSet>
#include <QTimer>
#include <QWidget>

class Locator;
class TileSource;
struct AccessPoint;

// The map: slippy basemap (dark / streets / satellite / topo), our position with
// accuracy ring, pulse and radar sweep, the trip track, points of interest from
// OpenStreetMap, and every access point placed by its best estimate. Painted with
// QPainter — no WebEngine, no GL — so it's safe on the i915 vGPU.
class BeaconView : public QWidget {
    Q_OBJECT
public:
    enum Layer { Dark, Streets, Satellite, Topo };

    explicit BeaconView(Locator *loc, TileSource *tiles, QWidget *parent = nullptr);
    void setZoom(int z);
    int  zoom() const { return qRound(m_zoom); }
    void recenter();
    void fitBeacons();
    void focusOn(double lat, double lon, double zoom = 17);
    void selectPoi(int index);
    void setLayer(Layer l);
    Layer layer() const { return m_layer; }
    QSet<QString> hiddenCategories() const { return m_hiddenCats; }
    void setCategoryVisible(const QString &key, bool visible);
    bool showNames() const { return m_showNames; }
    void setShowNames(bool on);
    void setShowDevices(bool on);
    void setShowImported(bool on);           // the imported history track (Timeline / WiGLE / GPX / KML)
    void replayLastRefit();

protected:
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void leaveEvent(QEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    enum HitKind { HitNone, HitBeacon, HitPoi, HitCluster, HitButton, HitAnchor };
    struct Hit { HitKind kind; QPointF pos; double radius; QList<int> items; int button = -1; };

    // Web-Mercator, normalised to [0,1]²
    static QPointF merc(double lat, double lon);
    static void    unmerc(const QPointF &m, double *lat, double *lon);
    double  worldPx() const;                                   // world size in logical px at m_zoom
    QPointF toScreen(const QPointF &m) const;
    QPointF toScreen(double lat, double lon) const { return toScreen(merc(lat, lon)); }
    QPointF toMerc(const QPointF &screen) const;
    double  metersPerPixel(double lat) const;
    static double metersPerPixelAt(double lat, double zoom);

    int     maxZoom() const;
    QString tileKey(Layer l, int z, int x, int y, bool labels) const;
    void    ensureTile(Layer l, int z, int x, int y, bool labels);
    void    drawTiles(QPainter &p, bool labels);
    void    drawTrack(QPainter &p);
    void    drawImportedTrack(QPainter &p);
    void    drawPois(QPainter &p);
    void    drawBeacons(QPainter &p);
    void    drawMe(QPainter &p);
    void    drawLabels(QPainter &p);
    void    drawEvents(QPainter &p);
    void    drawTicker(QPainter &p);
    void    rebuildLabelCache();
    void    onEvent(const QString &json);
    bool    beaconScreenPos(const QString &bssid, QPointF *out) const;
    void    drawHud(QPainter &p);
    void    drawControls(QPainter &p);
    void    drawCard(QPainter &p);
    void    drawScale(QPainter &p);
    void    drawAttribution(QPainter &p);

    void    zoomAt(double delta, const QPointF &anchor, bool animate = true);
    void    applyZoom(double z, const QPointF &anchor);
    void    buttonClicked(int b, const QPoint &globalPos);
    void    showItemMenu(const Hit &h, const QPoint &globalPos);
    int     hitAt(const QPointF &pos) const;
    QString beaconCard(int apIndex) const;
    QString poiCard(int poiIndex) const;
    static QString compass(double deg);
    static QString distText(double m);

    Locator *m_loc;
    TileSource *m_src;
    QCache<QString, QPixmap> m_tiles;
    QSet<QString> m_pending;
    QHash<QString, QDateTime> m_failed;
    QTimer  m_anim;
    Layer   m_layer = Dark;
    QSet<QString> m_hiddenCats;

    double  m_zoom = 15, m_zoomTarget = 15;
    QPointF m_zoomAnchor;
    QPointF m_center{0.5, 0.5};                                 // view centre, mercator
    bool    m_follow = true, m_autoZoom = true;
    QPointF m_dragStart, m_dragCenter; bool m_dragging = false, m_dragMoved = false;

    QList<Hit> m_hits;
    int     m_hover = -1;                                       // index into m_hits
    HitKind m_selKind = HitNone; int m_selItem = -1;            // pinned card
    QList<QPointF> m_beaconPos;                                 // per AP index, this frame
    QHash<QString, int> m_apIndex;                              // bssid → AP index (rebuilt on scan)
    struct LabelInfo { QString text; double width = 0; QString band; QColor col; bool hidden = false; };
    QList<LabelInfo> m_labels;                                  // per AP index, rebuilt on scan (not per frame)
    bool    m_showNames = true;
    // Live events, animated on the map and listed in the ticker
    struct Anim {
        QString type, bssid, ssid, text, glyph;
        qint64  start = 0; int durationMs = 2000;
        bool    hasPos = false, hasFrom = false;
        double  lat = 0, lon = 0, fromLat = 0, fromLon = 0, r = 0, bearing = 0;
        int     delta = 0; QColor col;
        // ap_refit: the vantage points that made the fit (lat, lon, dbm, device) and the numbers to show
        QList<QPointF> vp; QList<int> vpDbm; QStringList vpDev;
        double  acc = 0, prevAcc = 0; int n = 0, vantage = 0;
    };
    Anim    m_lastRefit; bool m_haveRefit = false;              // "Replay last refit"
    bool    m_showDevices = true;
    bool    m_showImported = true;
    void    drawDevices(QPainter &p);
    void    drawAnchors(QPainter &p);
    void    drawRangeInset(QPainter &p, const QList<QJsonObject> &close);   // sub-pixel gaps: a to-scale inset next to us
    void    editAnchor(int index);                              // index into m_loc->anchors()
    void    placeAnchorAt(double lat, double lon);
    QHash<QString, QJsonObject> m_ranges;                       // device → its ranging estimate (RangingService)
    int     m_anchorDrag = -1; QPointF m_anchorDragPos; bool m_anchorMoved = false;
    void    drawRefit(QPainter &p, const Anim &a, double t, double mpp);
    static QColor deviceColor(const QString &device);
    QList<Anim> m_anims;                                        // in flight (≤ 24)
    QList<Anim> m_ticker;                                       // last 5 events, newest first
    QHash<QString, qint64> m_labelBorn;                         // bssid → when its label should slide in
    QRectF  m_tickerRect; bool m_tickerHover = false; qint64 m_tickerPausedAt = 0;
    double  m_hudBottom = 0;
    QList<QPointF> m_poiPos;                                    // per POI index, this frame (null = hidden)
    double  m_phase = 0, m_sweep = 0;
};
