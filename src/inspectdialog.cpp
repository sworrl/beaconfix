// SPDX-License-Identifier: Apache-2.0
#include "inspectdialog.h"
#include "imagestore.h"
#include "locator.h"
#include "mapdb.h"
#include "platewatch.h"
#include "routeplanner.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>

InspectDialog::InspectDialog(Locator *loc, const QString &cameraId, QWidget *parent)
    : QDialog(parent), m_loc(loc), m_camId(cameraId)
{
    setWindowTitle(QStringLiteral("Inspect Camera Unseen — Blind-Spot & Route Inspection"));
    resize(760, 680);

    MapDb *db = m_loc ? m_loc->mapDb() : nullptr;
    bool found = false;
    const FlockCamera fc = db ? db->flockCamera(m_camId, &found) : FlockCamera();

    m_targetCam.id = fc.id;
    m_targetCam.lat = fc.lat;
    m_targetCam.lon = fc.lon;
    m_targetCam.operatorName = fc.operatorName.isEmpty() ? QStringLiteral("the operator") : fc.operatorName;
    m_targetCam.model = fc.model.isEmpty() ? QStringLiteral("ALPR Camera") : fc.model;
    m_targetCam.dirs = PlateEvents::parseDirections(fc.direction);
    PlateEvents::Camera peCam; peCam.id = fc.id; peCam.model = fc.model; peCam.type = fc.cameraType;
    m_targetCam.cone = PlateEvents::coneFor(peCam);
    const MapDb::CameraExtra extra = db ? db->cameraExtra(m_camId) : MapDb::CameraExtra();
    m_targetCam.trust = extra.hasTrust ? extra.trust : 1.0;

    auto *mainLayout = new QVBoxLayout(this);

    // 1. Camera Info Header
    auto *headerBox = new QGroupBox(QStringLiteral("Target Camera"), this);
    auto *headerLayout = new QVBoxLayout(headerBox);
    const QString dirStr = fc.direction.isEmpty() ? QStringLiteral("Omnidirectional / Unknown (sees all round)") : fc.direction;
    const QString trustStr = QStringLiteral("%1 %").arg(qRound(m_targetCam.trust * 100));
    m_camDetails = new QLabel(QStringLiteral("<b>%1</b> (%2) · %3<br>Location: %4, %5 · Facing: <b>%6</b> · Trust: %7")
                                  .arg(m_targetCam.model.toHtmlEscaped(), m_targetCam.operatorName.toHtmlEscaped(), m_camId.toHtmlEscaped())
                                  .arg(fc.lat, 0, 'f', 6).arg(fc.lon, 0, 'f', 6)
                                  .arg(dirStr.toHtmlEscaped(), trustStr), this);
    m_camDetails->setTextFormat(Qt::RichText);
    m_camDetails->setWordWrap(true);
    headerLayout->addWidget(m_camDetails);
    mainLayout->addWidget(headerBox);

    // 2. Limits Warning Notice
    auto *limitsLabel = new QLabel(
        QStringLiteral("<b>Limits:</b> Only <i>mapped</i> cameras with their mapped directions are avoided. "
                       "Unmapped cameras, PTZ / 360° domes, private CCTV, police-car ALPRs and cell tracking are not. "
                       "Look at stored photos first: the answer may not need a trip."), this);
    limitsLabel->setStyleSheet(QStringLiteral("background-color: #2b2614; color: #ffd166; padding: 6px; border-radius: 4px;"));
    limitsLabel->setWordWrap(true);
    mainLayout->addWidget(limitsLabel);

    // 3. Stored Remote Photos
    m_photosBox = new QWidget(this);
    loadPhotos();
    mainLayout->addWidget(m_photosBox);

    // 4. Plan Controls
    auto *controlsBox = new QGroupBox(QStringLiteral("Inspection Parameters"), this);
    auto *ctrlLayout = new QHBoxLayout(controlsBox);

    ctrlLayout->addWidget(new QLabel(QStringLiteral("Profile:"), this));
    m_profileCombo = new QComboBox(this);
    m_profileCombo->addItem(QStringLiteral("Foot (walk)"), QStringLiteral("foot"));
    m_profileCombo->addItem(QStringLiteral("Car (drive)"), QStringLiteral("car"));
    ctrlLayout->addWidget(m_profileCombo);

    ctrlLayout->addWidget(new QLabel(QStringLiteral("Min distance:"), this));
    m_minM = new QDoubleSpinBox(this);
    m_minM->setRange(10.0, 100.0);
    m_minM->setValue(20.0);
    m_minM->setSuffix(QStringLiteral(" m"));
    ctrlLayout->addWidget(m_minM);

    ctrlLayout->addWidget(new QLabel(QStringLiteral("Max distance:"), this));
    m_maxM = new QDoubleSpinBox(this);
    m_maxM->setRange(25.0, 200.0);
    m_maxM->setValue(60.0);
    m_maxM->setSuffix(QStringLiteral(" m"));
    ctrlLayout->addWidget(m_maxM);

    m_planBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("system-search")), QStringLiteral("Calculate Safe Vantage & Routes"), this);
    m_planBtn->setStyleSheet(QStringLiteral("font-weight: bold; padding: 4px 12px;"));
    connect(m_planBtn, &QPushButton::clicked, this, &InspectDialog::computePlan);
    ctrlLayout->addWidget(m_planBtn);

    mainLayout->addWidget(controlsBox);

    // 5. Status Banner
    m_statusBanner = new QLabel(this);
    m_statusBanner->setVisible(false);
    m_statusBanner->setWordWrap(true);
    mainLayout->addWidget(m_statusBanner);

    // 6. Candidate Vantage Points
    auto *vantagesBox = new QGroupBox(QStringLiteral("Safe Vantage Points (Outside Camera Field of View)"), this);
    auto *vbLayout = new QVBoxLayout(vantagesBox);

    m_vantagesTable = new QTableWidget(0, 5, this);
    m_vantagesTable->setHorizontalHeaderLabels({QStringLiteral("Side"), QStringLiteral("Distance"), QStringLiteral("Look Direction"), QStringLiteral("Reason"), QStringLiteral("Score")});
    m_vantagesTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_vantagesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_vantagesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_vantagesTable->verticalHeader()->setVisible(false);
    vbLayout->addWidget(m_vantagesTable);

    m_legsSummary = new QLabel(this);
    m_legsSummary->setWordWrap(true);
    vbLayout->addWidget(m_legsSummary);

    mainLayout->addWidget(vantagesBox);

    // 7. Dialog Bottom Buttons
    auto *btnLayout = new QHBoxLayout;
    m_gpxBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), QStringLiteral("Export Plan as GPX…"), this);
    m_gpxBtn->setEnabled(false);
    connect(m_gpxBtn, &QPushButton::clicked, this, &InspectDialog::exportGpx);
    btnLayout->addWidget(m_gpxBtn);

    m_mapBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("mark-location")), QStringLiteral("Show on Map"), this);
    m_mapBtn->setEnabled(false);
    connect(m_mapBtn, &QPushButton::clicked, this, &InspectDialog::showOnMapClicked);
    btnLayout->addWidget(m_mapBtn);

    btnLayout->addStretch(1);

    auto *closeBtn = new QPushButton(QStringLiteral("Close"), this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnLayout->addWidget(closeBtn);

    mainLayout->addLayout(btnLayout);

    // Initial plan computation
    computePlan();
}

void InspectDialog::loadPhotos()
{
    MapDb *db = m_loc ? m_loc->mapDb() : nullptr;
    if (!db) return;
    const QJsonArray media = db->plateEventMedia(QString(), m_camId);

    auto *layout = new QVBoxLayout(m_photosBox);
    layout->setContentsMargins(0, 0, 0, 0);

    if (media.isEmpty()) {
        auto *lbl = new QLabel(QStringLiteral("<i>No stored remote street photos for this camera yet.</i>"), m_photosBox);
        layout->addWidget(lbl);
        return;
    }

    auto *scroll = new QScrollArea(m_photosBox);
    scroll->setFixedHeight(140);
    scroll->setWidgetResizable(true);
    auto *inner = new QWidget(scroll);
    auto *rowLayout = new QHBoxLayout(inner);

    for (const QJsonValue &v : media) {
        const QJsonObject m = v.toObject();
        const QString uid = m.value(QLatin1String("uid")).toString();
        const QString mime = m.value(QLatin1String("mime")).toString();
        const QString attr = m.value(QLatin1String("attribution")).toString();
        const QByteArray bytes = db->mediaData(uid);
        if (bytes.isEmpty()) continue;

        QImage img = ImageStore::decode(bytes, mime);
        if (img.isNull()) continue;

        auto *card = new QWidget(inner);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(4, 4, 4, 4);

        auto *imgLabel = new QLabel(card);
        imgLabel->setPixmap(QPixmap::fromImage(img.scaled(140, 90, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        cardLayout->addWidget(imgLabel);

        auto *attrLabel = new QLabel(attr.isEmpty() ? QStringLiteral("Public photo") : attr, card);
        attrLabel->setStyleSheet(QStringLiteral("font-size: 10px; color: #888;"));
        attrLabel->setWordWrap(true);
        cardLayout->addWidget(attrLabel);

        rowLayout->addWidget(card);
    }
    rowLayout->addStretch(1);
    scroll->setWidget(inner);
    layout->addWidget(scroll);
}

void InspectDialog::computePlan()
{
    PlateWatch *pw = m_loc ? m_loc->plateWatch() : nullptr;
    RoutePlanner *rp = pw ? pw->routePlanner() : nullptr;
    MapDb *db = m_loc ? m_loc->mapDb() : nullptr;
    if (!rp || !db) return;

    m_planBtn->setEnabled(false);
    m_statusBanner->setText(QStringLiteral("Calculating blind-spot vantage points and routing legs…"));
    m_statusBanner->setStyleSheet(QStringLiteral("background-color: #1e3a5f; color: #a5d8ff; padding: 6px; border-radius: 4px;"));
    m_statusBanner->setVisible(true);

    const Fix &f = m_loc->fix();
    AvoidRoute::LatLon from = f.valid ? AvoidRoute::LatLon{f.lat, f.lon} : AvoidRoute::LatLon{m_targetCam.lat, m_targetCam.lon};

    const QString profile = m_profileCombo->currentData().toString();
    const double minM = m_minM->value();
    const double maxM = m_maxM->value();

    const QList<RoadSnap::Way> ways = db->waysNear(m_targetCam.lat, m_targetCam.lon, maxM + 50.0);
    const QJsonArray photos = db->plateEventMedia(QString(), m_camId);

    QPointer<InspectDialog> self(this);
    rp->inspect(m_targetCam, from, std::nullopt, profile, minM, maxM, QString(), ways, photos, [self](int code, const QJsonObject &res) {
        if (!self) return;
        self->m_planBtn->setEnabled(true);
        if (code != 200) {
            self->m_statusBanner->setText(QStringLiteral("Inspection error (HTTP %1): %2").arg(code).arg(res.value(QLatin1String("error")).toString()));
            self->m_statusBanner->setStyleSheet(QStringLiteral("background-color: #5c1d1d; color: #ff9999; padding: 6px; border-radius: 4px;"));
            return;
        }
        self->updateUiWithPlan(res);
    });
}

void InspectDialog::updateUiWithPlan(const QJsonObject &plan)
{
    m_currentPlan = plan;
    const bool safe = plan.value(QLatin1String("safe")).toBool();
    const QString note = plan.value(QLatin1String("note")).toString();
    const QJsonArray exposures = plan.value(QLatin1String("exposures")).toArray();

    if (!exposures.isEmpty()) {
        m_statusBanner->setText(QStringLiteral("<b>EXPOSURE DETECTED:</b> Route enters field of view of %1 camera(s)! Do not use this path if you must stay unseen.")
                                    .arg(exposures.size()));
        m_statusBanner->setStyleSheet(QStringLiteral("background-color: #661010; color: #ffcccc; padding: 8px; border-radius: 4px; font-size: 13px;"));
    } else if (safe) {
        m_statusBanner->setText(QStringLiteral("<b>CLEAN / SAFE:</b> Vantage is outside camera view, and route avoids all known ALPRs.")
                                + (note.isEmpty() ? QString() : QStringLiteral("<br><i>%1</i>").arg(note)));
        m_statusBanner->setStyleSheet(QStringLiteral("background-color: #144620; color: #b3ffcc; padding: 8px; border-radius: 4px; font-size: 13px;"));
    } else {
        m_statusBanner->setText(note.isEmpty() ? QStringLiteral("No safe path could be verified.") : note);
        m_statusBanner->setStyleSheet(QStringLiteral("background-color: #4a3e14; color: #fff3cc; padding: 8px; border-radius: 4px;"));
    }
    m_statusBanner->setVisible(true);

    // Update vantages table
    const QJsonArray vArr = plan.value(QLatin1String("vantages")).toArray();
    m_vantagesTable->setRowCount(0);
    for (const QJsonValue &vVal : vArr) {
        const QJsonObject v = vVal.toObject();
        const int row = m_vantagesTable->rowCount();
        m_vantagesTable->insertRow(row);
        m_vantagesTable->setItem(row, 0, new QTableWidgetItem(v.value(QLatin1String("side")).toString()));
        m_vantagesTable->setItem(row, 1, new QTableWidgetItem(QStringLiteral("%1 m").arg(v.value(QLatin1String("distanceM")).toDouble(), 0, 'f', 1)));
        m_vantagesTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("%1°").arg(v.value(QLatin1String("bearingToCamera")).toInt())));
        m_vantagesTable->setItem(row, 3, new QTableWidgetItem(v.value(QLatin1String("reason")).toString()));
        m_vantagesTable->setItem(row, 4, new QTableWidgetItem(QString::number(v.value(QLatin1String("score")).toDouble(), 'f', 1)));
    }

    // Legs summary
    const QJsonObject legs = plan.value(QLatin1String("legs")).toObject();
    const QJsonObject toV = legs.value(QLatin1String("toVantage")).toObject();
    const QJsonObject away = legs.value(QLatin1String("away")).toObject();
    if (toV.value(QLatin1String("ok")).toBool()) {
        const double d1 = toV.value(QLatin1String("distanceM")).toDouble();
        const double t1 = toV.value(QLatin1String("durationS")).toDouble();
        const double d2 = away.value(QLatin1String("distanceM")).toDouble();
        const double t2 = away.value(QLatin1String("durationS")).toDouble();
        m_legsSummary->setText(QStringLiteral("<b>Navigation Legs:</b> Approach: %1 m (~%2 min) · Departure: %3 m (~%4 min) · Provider: %5")
                                   .arg(qRound(d1)).arg(qRound(t1 / 60.0))
                                   .arg(qRound(d2)).arg(qRound(t2 / 60.0))
                                   .arg(plan.value(QLatin1String("providerName")).toString()));
    } else {
        m_legsSummary->setText(note);
    }

    m_gpxBtn->setEnabled(!vArr.isEmpty());
    m_mapBtn->setEnabled(true);
}

void InspectDialog::exportGpx()
{
    const QJsonArray vArr = m_currentPlan.value(QLatin1String("vantages")).toArray();
    if (vArr.isEmpty()) return;
    const QJsonObject v0 = vArr.first().toObject();
    AvoidRoute::Vantage vantage;
    vantage.pt = {v0.value(QLatin1String("lat")).toDouble(), v0.value(QLatin1String("lon")).toDouble()};
    vantage.distanceM = v0.value(QLatin1String("distanceM")).toDouble();
    vantage.bearingToCamera = v0.value(QLatin1String("bearingToCamera")).toDouble();
    vantage.side = v0.value(QLatin1String("side")).toString();
    vantage.reason = v0.value(QLatin1String("reason")).toString();

    auto parseCoords = [](const QJsonObject &legObj) {
        QList<AvoidRoute::LatLon> pts;
        for (const QJsonValue &v : legObj.value(QLatin1String("route")).toObject().value(QLatin1String("coordinates")).toArray()) {
            const QJsonArray c = v.toArray();
            if (c.size() >= 2) pts.append({c[1].toDouble(), c[0].toDouble()});
        }
        return pts;
    };

    const QJsonObject legs = m_currentPlan.value(QLatin1String("legs")).toObject();
    const QList<AvoidRoute::LatLon> toV = parseCoords(legs.value(QLatin1String("toVantage")).toObject());
    const QList<AvoidRoute::LatLon> away = parseCoords(legs.value(QLatin1String("away")).toObject());

    const QString gpxContent = AvoidRoute::toGpx(toV, away, vantage, m_targetCam);

    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save Inspection Plan GPX"),
                                                      QStringLiteral("inspection_%1.gpx").arg(m_camId.replace(QLatin1Char(':'), QLatin1Char('_')).replace(QLatin1Char('/'), QLatin1Char('_'))),
                                                      QStringLiteral("GPX files (*.gpx)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(gpxContent.toUtf8());
        f.close();
        QMessageBox::information(this, QStringLiteral("GPX Export"), QStringLiteral("Inspection route exported successfully to %1").arg(path));
    } else {
        QMessageBox::warning(this, QStringLiteral("GPX Export"), QStringLiteral("Failed to write to %1").arg(path));
    }
}

void InspectDialog::showOnMapClicked()
{
    emit showPlanOnMap(m_currentPlan);
    accept();
}
