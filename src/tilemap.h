#pragma once
#include <QHash>
#include <QNetworkAccessManager>
#include <QPixmap>
#include <QSet>
#include <QWidget>

// Minimal OpenStreetMap slippy-map view: draws the tiles around a fix with a
// marker and accuracy ring. No WebEngine, no GL — safe on the i915 vGPU.
class TileMap : public QWidget {
    Q_OBJECT
public:
    explicit TileMap(QWidget *parent = nullptr);
    void setFix(double lat, double lon, double accuracyM, bool valid);
    void setZoom(int z);
    int  zoom() const { return m_zoom; }

protected:
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    QString key(int z, int x, int y) const;
    void ensureTile(int z, int x, int y);

    QNetworkAccessManager m_nam;
    QHash<QString, QPixmap> m_tiles;
    QSet<QString> m_pending;
    double m_lat = 39.0, m_lon = -105.0, m_acc = -1;
    bool   m_valid = false;
    int    m_zoom = 13;
};
