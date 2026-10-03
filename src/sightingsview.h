// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QTimer>
#include <QWidget>

class Locator;
class QComboBox;
class QLabel;
class QTableWidget;

// The main window's Sightings tab (docs/SIGHTINGS.md §6): every plate event — ALPR passes (your plate was likely
// read), traffic-camera passes (no plate reading), plate searches from released Flock audit logs — newest first.
class SightingsView : public QWidget {
    Q_OBJECT
public:
    explicit SightingsView(Locator *loc, QWidget *parent = nullptr);
    void refresh();
    void openEvent(const QString &uid);                 // the event dialog

signals:
    void showOnMap(double lat, double lon);

private:
    Locator *m_loc;
    QTableWidget *m_table;
    QComboBox *m_filter;
    QLabel *m_status;
    QTimer m_debounce;
};

// One event: what happened, every number, the images (dash cam, the camera, a webcam still), the raw record
class PlateEventDialog : public QDialog {
    Q_OBJECT
public:
    PlateEventDialog(Locator *loc, const QJsonObject &event, QWidget *parent = nullptr);

signals:
    void showOnMap(double lat, double lon);
};
