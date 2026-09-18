#pragma once
#include <QHash>
#include <QNetworkAccessManager>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QSet>
#include <QTimer>
#include <QWidget>

class Locator;
struct AccessPoint;

// The beacon view: dark basemap, our position with a pulsing halo and radar
// sweep, every access point placed by its estimate (WiGLE / centroid / RSSI
// ring), and a HUD with rank + counters. Painted with QPainter — no WebEngine,
// no GL — so it's safe on the i915 vGPU.
class BeaconView : public QWidget {
    Q_OBJECT
public:
    explicit BeaconView(Locator *loc, QWidget *parent = nullptr);
    void setZoom(int z);
    int  zoom() const { return m_zoom; }
    void recenter();
    void fitBeacons();

protected:
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    struct Placed { QPointF pos; double radiusPx; int index; };
    QString key(int z, int x, int y) const;
    void ensureTile(int z, int x, int y);
    QPointF project(double lat, double lon) const;     // to widget coords
    double metersPerPixel(double lat) const;
    static double metersPerPixelAt(double lat, int zoom);
    static QImage darken(const QImage &src);
    void drawHud(QPainter &p);
    void drawLegend(QPainter &p);

    Locator *m_loc;
    QNetworkAccessManager m_nam;
    QHash<QString, QPixmap> m_tiles;
    QSet<QString> m_pending;
    QTimer m_anim;
    int    m_zoom = 15;
    double m_centerLat = 39, m_centerLon = -105;      // view centre (pan)
    bool   m_followFix = true;
    QPointF m_dragStart; double m_dragLat = 0, m_dragLon = 0; bool m_dragging = false;
    QList<Placed> m_placed;
    int    m_hover = -1;
    double m_phase = 0;                                 // animation phase 0..1
    double m_sweep = 0;                                 // radar sweep angle
};
