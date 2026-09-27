#pragma once
#include <QDialog>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QWidget>

class ApiServer;
class Locator;
class TileSource;
class QLabel;
class QPushButton;
class QLineEdit;
class QGridLayout;

// A small map with two markers (the requesting device and us), tiles from TileSource.
class MiniMap : public QWidget {
    Q_OBJECT
public:
    explicit MiniMap(TileSource *tiles, QWidget *parent = nullptr);
    void setPoints(bool haveThem, double tLat, double tLon, double tAcc, bool haveUs, double uLat, double uLon, double uAcc);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QPointF toScreen(double lat, double lon) const;
    TileSource *m_tiles;
    bool m_them = false, m_us = false;
    double m_tLat = 0, m_tLon = 0, m_tAcc = 0, m_uLat = 0, m_uLon = 0, m_uAcc = 0;
    int m_zoom = 15; QPointF m_center;               // mercator [0,1]
    QHash<QString, QImage> m_cache;
};

// "Pair <device>?" — proximity, mini map, picture match (docs/API.md, Pairing v2).
class PairDialog : public QDialog {
    Q_OBJECT
public:
    PairDialog(ApiServer *api, Locator *loc, TileSource *tiles, const QString &pendingId, QWidget *parent = nullptr);
    QString pendingId() const { return m_id; }
    void refresh();

private:
    void pick(int tripleIndex);
    void deny();
    void pairAnyway();
    QString verdictColor(const QString &v) const;

    ApiServer *m_api; Locator *m_loc; TileSource *m_tiles;
    QString m_id;
    QJsonObject m_detail;
    QLabel *m_title, *m_meta, *m_verdict, *m_beacons, *m_hint, *m_code, *m_result;
    MiniMap *m_map;
    QGridLayout *m_triples;
    QList<QPushButton *> m_tripleBtns;
    QPushButton *m_denyBtn, *m_anywayBtn, *m_codeBtn, *m_closeBtn, *m_controlBtn;
    QLineEdit *m_confirm;
    bool m_done = false;
};
