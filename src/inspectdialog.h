// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "avoidroute.h"
#include <QDialog>
#include <QJsonObject>
#include <QPointer>

class Locator;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

class InspectDialog : public QDialog {
    Q_OBJECT
public:
    explicit InspectDialog(Locator *loc, const QString &cameraId, QWidget *parent = nullptr);

signals:
    void showPlanOnMap(const QJsonObject &plan);

private slots:
    void computePlan();
    void exportGpx();
    void showOnMapClicked();

private:
    void loadPhotos();
    void updateUiWithPlan(const QJsonObject &plan);

    Locator *m_loc = nullptr;
    QString m_camId;
    AvoidRoute::Cam m_targetCam;
    QJsonObject m_currentPlan;

    QLabel *m_camDetails = nullptr;
    QLabel *m_statusBanner = nullptr;
    QWidget *m_photosBox = nullptr;
    QComboBox *m_profileCombo = nullptr;
    QDoubleSpinBox *m_minM = nullptr;
    QDoubleSpinBox *m_maxM = nullptr;
    QTableWidget *m_vantagesTable = nullptr;
    QLabel *m_legsSummary = nullptr;
    QPushButton *m_planBtn = nullptr;
    QPushButton *m_gpxBtn = nullptr;
    QPushButton *m_mapBtn = nullptr;
};
