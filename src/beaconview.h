#pragma once
#include <QCache>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QPointer>
#include <QDialog>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>

#include "locator.h"
#include "flockdetector.h"
#include "tilesource.h"
struct AccessPoint;

// The map: slippy basemap (dark / streets / satellite / topo), our position with
// accuracy ring, pulse and radar sweep, the trip track, points of interest from
// OpenStreetMap, and every access point placed by its best estimate. Painted with
// QPainter — no WebEngine, no GL — so it's safe on the i915 vGPU.
class BeaconView : public QWidget {
    Q_OBJECT
public:
    // Pinpointing an AP to under a foot: zoom 23 is ~1.8 cm a pixel here; imagery stops at its source's own depth
    // (19 for Esri) and is scaled past it, while markers, ellipses, contours and the scale bar stay sharp
    static constexpr int MAX_VIEW_ZOOM = 23;
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
    void setSatSource(int s);                // TileSource::SatSource; also switches to the Satellite layer
    bool showContours() const { return m_showContours; }
    void setShowContours(bool on);           // satellite hybrid: contour lines (drawn by TileSource from terrain tiles)
    QSet<QString> hiddenCategories() const { return m_hiddenCats; }
    void setCategoryVisible(const QString &key, bool visible);
    bool showNames() const { return m_showNames; }
    void setShowNames(bool on);
    void setShowDevices(bool on);
    void setShowImported(bool on);           // the imported history track (Timeline / WiGLE / GPX / KML)
    bool showHeatmap() const { return m_showHeatmap; }
    void setShowHeatmap(bool on);
    bool showFlockCameras() const { return m_showFlockCameras; }
    void setShowFlockCameras(bool on);
    bool showApCircles() const { return m_showApCircles; }
    void setShowApCircles(bool on);
    void replayLastRefit();
    bool showLegend() const { return m_showLegend; }
    void setShowLegend(bool on);             // the grade legend (bottom right)
    // docs/SIGHTINGS.md §8: a route to (lat, lon) around the ALPR cameras, from the chosen start or our fix; drawn until cleared
    void routeAvoidingAlprs(double toLat, double toLon);
    void setRouteStart(double lat, double lon);
    void clearAvoidRoute();
    // docs/SIGHTINGS.md §9: inspect unseen
    void inspectCamera(const QString &cameraId);
    void showInspectPlan(const QJsonObject &plan);
    void clearInspectPlan();
    // Grade colours (Okabe–Ito, colour-blind safe): A–F, R (region only), M (mobile); grey otherwise
    static QColor gradeColor(const QString &grade);
    static QColor gradeTextColor(const QString &grade);   // black or white, whichever reads on gradeColor()

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
    enum HitKind { HitNone, HitBeacon, HitPoi, HitCluster, HitButton, HitAnchor, HitCamera };
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
    TileSource::Layer srcLayer(Layer l) const;                 // Satellite → the chosen imagery source
    // ov: 0 = the base layer, else the TileSource::Layer of a satellite-hybrid overlay (Labels, Roads, Contours)
    QString tileKey(Layer l, int z, int x, int y, int ov) const;
    void    ensureTile(Layer l, int z, int x, int y, int ov);
    void    drawTiles(QPainter &p, int ov);
    void    drawBase(QPainter &p);                              // the static layers, from m_base when the view hasn't changed
    void    drawHeatmap(QPainter &p);
    void    loadRouteFixes();
    void    startHeat();
    static QImage renderHeat(const QList<Fix> &fixes, const QPointF &tl, double zoom, const QSize &size);
    void    setFlockCameras(const QList<FlockCamera> &cams);
    void    loadFlockCameras();                                 // those around the view, from the database
    const Stats &stats();
    void    drawTrack(QPainter &p);
    void    drawImportedTrack(QPainter &p);
    void    drawPois(QPainter &p);
    void    drawFlockCameras(QPainter &p);
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
    void    drawLegend(QPainter &p);
    void    drawSuggestion(QPainter &p, int apIndex, const QPointF &from);   // "sample here next" crosshair

    void    zoomAt(double delta, const QPointF &anchor, bool animate = true);
    void    applyZoom(double z, const QPointF &anchor);
    void    buttonClicked(int b, const QPoint &globalPos);
    void    showItemMenu(const Hit &h, const QPoint &globalPos);
    int     hitAt(const QPointF &pos) const;
    QString beaconCard(int apIndex) const;
    QString poiCard(int poiIndex) const;
    QString cameraCard(int camIndex) const;
    static QString compass(double deg);
    static QString distText(double m);

    Locator *m_loc;
    TileSource *m_src;
    QCache<QString, QPixmap> m_tiles;
    struct TileReq { Layer l; int z, x, y; int ov; };
    QHash<QString, TileReq> m_pending;                          // asked of TileSource, not back yet
    QSet<QString> m_wanted;                                     // what the last base render looked for
    QHash<QString, QDateTime> m_failed;
    QTimer  m_anim;
    Layer   m_layer = Dark;
    int     m_satSource = 0;                                    // TileSource::SatSource
    bool    m_showContours = true;
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
    bool    m_showLegend = false;
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
    bool    m_showHeatmap = true;
    bool    m_showFlockCameras = true;
    bool    m_showApCircles = true;
    QList<Fix> m_routeFixes;
    bool    m_routeFixesDirty = true;
    QList<FlockCamera> m_flockCameras;
    QList<QPointF> m_cameraMerc;                                // per camera, mercator (no trig per frame)
    QHash<int, QPointF> m_cameraPos;                            // per drawn camera, this frame
    QRectF  m_camBox; double m_camZoom = 0;                     // what m_flockCameras covers (x = lon, y = lat)
    bool    m_camCapped = false, m_camStale = true;
    QPointF m_camCentre; double m_camK = 1, m_camR2 = 0;        // capped: only the disc (lat², (lon·k)²) ≤ r² about the centre
    QTimer  m_camKick;

    // Paint caches. Tiles, vignette, heat map and tracks change only with the view or the data, so
    // they're rendered into m_base and blitted under the live overlay; the animation tick repaints
    // just the pulse around us unless something else moves.
    struct BaseKey {
        QSize size; qreal dpr = 0; QPointF center; double zoom = 0; int layer = 0, tiles = 0, heat = 0, heatData = 0, track = 0;
        bool heatOn = false, imported = false, fixValid = false; qsizetype hist = 0, imp = 0;
        bool operator==(const BaseKey &o) const {
            return size == o.size && dpr == o.dpr && center == o.center && zoom == o.zoom && layer == o.layer && tiles == o.tiles
                && heat == o.heat && heatData == o.heatData && track == o.track && heatOn == o.heatOn && imported == o.imported && fixValid == o.fixValid
                && hist == o.hist && imp == o.imp;
        }
    };
    QPixmap m_base; BaseKey m_baseKey; QElapsedTimer m_baseAt; bool m_baseMissing = false, m_baseTopo = false;   // topo: offline stand-ins drawn
    int     m_tileVersion = 0, m_trackVersion = 0;
    qint64  m_fullAt = 0;                                       // last full-widget repaint from the tick
    QRectF  m_paintClip;                                        // this paint's area: markers outside it only record their hits
    Stats   m_stats; QElapsedTimer m_statsAt;
    QList<ApEstimate> m_apEst; QStringList m_apStatus; QElapsedTimer m_apEstAt;   // per AP index, drawBeacons()
    // The heat map is rasterised on m_pool for an area a bit larger than the view, then panned and
    // scaled with the view until a fresh one arrives
    struct Heat { QImage img; QPointF tl; double zoom = 0; int version = -1; };
    Heat    m_heat; int m_heatVersion = 0, m_heatSerial = 0; bool m_heatBusy = false;
    QTimer  m_heatKick;
    QElapsedTimer m_routeLoadedAt;
    // the ALPR-avoiding route (docs/SIGHTINGS.md §8)
    void    drawAvoidRoute(QPainter &p);
    void    showRouteResult(const QJsonObject &o);
    // the inspection plan (docs/SIGHTINGS.md §9)
    void    drawInspectPlan(QPainter &p);
    QJsonObject m_inspectPlan;
    bool    m_haveRouteStart = false, m_routing = false;
    double  m_routeStartLat = 0, m_routeStartLon = 0;
    QList<QPointF> m_avoidRoute;                                // mercator
    QList<QPointF> m_avoidPassed;                               // the cameras it still passes (in a cone), mercator
    QPointer<QDialog> m_routeDialog;
    QThreadPool m_pool;                                         // last: waits for the heat worker before the rest goes
};
