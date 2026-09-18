#include "mainwindow.h"
#include "locator.h"
#include "beaconview.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

static QString sourceName(const QString &s)
{
    if (s == QLatin1String("starlink")) return QStringLiteral("Starlink dish GPS");
    if (s == QLatin1String("wifi"))     return QStringLiteral("BeaconDB Wi-Fi");
    if (s == QLatin1String("ip"))       return QStringLiteral("IP geolocation (approximate)");
    return QStringLiteral("—");
}

MainWindow::MainWindow(Locator *loc, QWidget *parent) : QMainWindow(parent), m_loc(loc)
{
    setWindowTitle(QStringLiteral("BeaconFix"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("beaconfix"), QIcon::fromTheme(QStringLiteral("mark-location"))));
    resize(1100, 720);

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);

    // ── Fix card ──────────────────────────────────────────────────────────────
    auto *card = new QHBoxLayout;
    auto *texts = new QVBoxLayout;
    m_place = new QLabel; QFont big = m_place->font(); big.setPointSizeF(big.pointSizeF() * 1.8); big.setBold(true); m_place->setFont(big);
    m_place->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_coords = new QLabel; m_coords->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_meta = new QLabel; { QFont mf = m_meta->font(); mf.setPointSizeF(mf.pointSizeF() * 0.95); m_meta->setFont(mf); }
    m_chip = new QLabel; m_chip->setAlignment(Qt::AlignCenter);
    m_chip->setStyleSheet(QStringLiteral("QLabel { color: #0b101a; background: #35d6ff; border-radius: 9px; padding: 2px 10px; font-weight: 600; }"));
    auto *placeRow = new QHBoxLayout; placeRow->addWidget(m_place); placeRow->addWidget(m_chip); placeRow->addStretch();
    texts->addLayout(placeRow); texts->addWidget(m_coords); texts->addWidget(m_meta);
    card->addLayout(texts, 1);

    auto *buttons = new QVBoxLayout;
    m_refresh = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Re-check now"));
    connect(m_refresh, &QPushButton::clicked, m_loc, &Locator::Refresh);
    auto *copy = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy coordinates"));
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(QStringLiteral("%1, %2").arg(m_loc->fix().lat, 0, 'f', 6).arg(m_loc->fix().lon, 0, 'f', 6));
        statusBar()->showMessage(QStringLiteral("Coordinates copied"), 2000);
    });
    auto *osm = new QPushButton(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("Open in OpenStreetMap"));
    connect(osm, &QPushButton::clicked, this, [this] {
        const Fix &f = m_loc->fix();
        if (f.valid) QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=15/%1/%2").arg(f.lat).arg(f.lon)));
    });
    auto *gpx = new QPushButton(QIcon::fromTheme(QStringLiteral("document-export")), QStringLiteral("Export trip as GPX…"));
    connect(gpx, &QPushButton::clicked, this, [this] {
        const QString def = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/beaconfix-trip.gpx";
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export GPX"), def, QStringLiteral("GPX (*.gpx)"));
        if (path.isEmpty()) return;
        QString err;
        if (!m_loc->exportGpx(path, &err)) QMessageBox::warning(this, QStringLiteral("Export failed"), err);
        else statusBar()->showMessage(QStringLiteral("Exported %1 fixes to %2").arg(m_loc->history().size()).arg(path), 5000);
    });
    for (QPushButton *b : {m_refresh, copy, osm, gpx}) buttons->addWidget(b);
    buttons->addStretch();
    card->addLayout(buttons);
    outer->addLayout(card);

    // ── Tabs ──────────────────────────────────────────────────────────────────
    auto *tabs = new QTabWidget;
    m_map = new BeaconView(m_loc);
    tabs->addTab(m_map, QIcon::fromTheme(QStringLiteral("map-globe")), QStringLiteral("Beacons"));

    m_aps = new QTableWidget(0, 6);
    m_aps->setHorizontalHeaderLabels({QStringLiteral("SSID"), QStringLiteral("BSSID"), QStringLiteral("dBm"), QStringLiteral("MHz"), QStringLiteral("Status"), QStringLiteral("Where")});
    m_aps->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_aps->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_aps->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_aps->setContextMenuPolicy(Qt::CustomContextMenu);
    m_aps->verticalHeader()->hide();
    connect(m_aps, &QTableWidget::customContextMenuRequested, this, &MainWindow::apContextMenu);
    tabs->addTab(m_aps, QIcon::fromTheme(QStringLiteral("network-wireless")), QStringLiteral("Access points"));

    m_history = new QTableWidget(0, 6);
    m_history->setHorizontalHeaderLabels({QStringLiteral("When"), QStringLiteral("Place"), QStringLiteral("Latitude"), QStringLiteral("Longitude"), QStringLiteral("±m"), QStringLiteral("Source")});
    m_history->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_history->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_history->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_history->verticalHeader()->hide();
    tabs->addTab(m_history, QIcon::fromTheme(QStringLiteral("view-history")), QStringLiteral("Trip log"));

    tabs->addTab(buildSettings(), QIcon::fromTheme(QStringLiteral("configure")), QStringLiteral("Settings"));
    outer->addWidget(tabs, 1);
    setCentralWidget(central);

    m_status = new QLabel;
    statusBar()->addWidget(m_status, 1);

    connect(m_loc, &Locator::FixChanged, this, &MainWindow::refreshFix);
    connect(m_loc, &Locator::FixChanged, this, &MainWindow::refreshHistory);
    connect(m_loc, &Locator::scanUpdated, this, &MainWindow::refreshAps);
    connect(m_loc, &Locator::probeStarted, this, [this] { m_refresh->setEnabled(false); });
    connect(m_loc, &Locator::probeFinished, this, [this](bool, const QString &) { m_refresh->setEnabled(true); refreshFix(); });
    connect(m_loc, &Locator::statusMessage, this, [this](const QString &m) { m_status->setText(m.section('\n', 0, 0)); m_status->setToolTip(m); });
    refreshFix(); refreshAps(); refreshHistory();
}

QWidget *MainWindow::buildSettings()
{
    auto *w = new QWidget;
    auto *form = new QFormLayout(w);
    m_interval = new QSpinBox; m_interval->setRange(1, 1440); m_interval->setSuffix(QStringLiteral(" min")); m_interval->setValue(m_loc->intervalMinutes());
    connect(m_interval, &QSpinBox::valueChanged, m_loc, &Locator::setIntervalMinutes);
    form->addRow(QStringLiteral("Check every:"), m_interval);

    m_threshold = new QSpinBox; m_threshold->setRange(0, 100000); m_threshold->setSuffix(QStringLiteral(" m")); m_threshold->setValue(m_loc->moveThresholdM());
    m_threshold->setToolTip(QStringLiteral("A new fix closer than this to the current one is treated as 'still here' and not logged."));
    connect(m_threshold, &QSpinBox::valueChanged, m_loc, &Locator::setMoveThresholdM);
    form->addRow(QStringLiteral("Movement threshold:"), m_threshold);

    m_starlink = new QCheckBox(QStringLiteral("Ask the Starlink dish for GPS first (needs 'Allow access on local network' in the Starlink app)"));
    m_starlink->setChecked(m_loc->useStarlink());
    connect(m_starlink, &QCheckBox::toggled, m_loc, &Locator::setUseStarlink);
    form->addRow(QString(), m_starlink);
    m_starlinkHost = new QLineEdit(m_loc->starlinkHost());
    connect(m_starlinkHost, &QLineEdit::editingFinished, this, [this] { m_loc->setStarlinkHost(m_starlinkHost->text()); });
    form->addRow(QStringLiteral("Starlink dish address:"), m_starlinkHost);

    m_ip = new QCheckBox(QStringLiteral("Fall back to IP geolocation when Wi-Fi gives nothing (coarse; on Starlink it's the ground-station city)"));
    m_ip->setChecked(m_loc->useIp());
    connect(m_ip, &QCheckBox::toggled, m_loc, &Locator::setUseIp);
    form->addRow(QString(), m_ip);

    m_ignoreActive = new QCheckBox(QStringLiteral("The network I'm connected to travels with me (exclude it from lookups)"));
    m_ignoreActive->setChecked(m_loc->ignoreActiveAp());
    connect(m_ignoreActive, &QCheckBox::toggled, m_loc, &Locator::setIgnoreActiveAp);
    form->addRow(QString(), m_ignoreActive);

    m_wigle = new QLineEdit(m_loc->wigleToken());
    m_wigle->setEchoMode(QLineEdit::Password);
    m_wigle->setPlaceholderText(QStringLiteral("optional — the 'Encoded for use' token from wigle.net/account"));
    m_wigle->setToolTip(QStringLiteral("With a WiGLE API token, beacons you hear are looked up one by one (1.5 s apart, cached) and drawn at their real mapped position as gold diamonds."));
    connect(m_wigle, &QLineEdit::editingFinished, this, [this] { m_loc->setWigleToken(m_wigle->text()); });
    form->addRow(QStringLiteral("WiGLE API token:"), m_wigle);

    m_ignore = new QPlainTextEdit(m_loc->ignorePatterns().join('\n'));
    m_ignore->setPlaceholderText(QStringLiteral("One glob per line, matched against BSSID and SSID, e.g.\nAA:BB:CC:??:EE:FF\nMyHotspot*"));
    m_ignore->setMaximumHeight(120);
    connect(m_ignore, &QPlainTextEdit::textChanged, this, [this] { m_loc->setIgnorePatterns(m_ignore->toPlainText().split('\n')); });
    form->addRow(QStringLiteral("Always ignore:"), m_ignore);

    auto *note = new QLabel(QStringLiteral(
        "Access points seen at two stops more than ~5 km apart are flagged as travelling with you automatically. "
        "Right-click a row in the Access points tab to flag or clear one by hand.\n"
        "Beacon positions: WiGLE (if a token is set) → signal-weighted centroid once a beacon has been heard from two spots ≥ 25 m apart → "
        "otherwise a ring at the RSSI-estimated distance with a stable pseudo-bearing (the direction is NOT known).\n"
        "Data: BeaconDB (api.beacondb.net) for the fix, Nominatim for place names, CARTO dark tiles on OpenStreetMap data for the map. "
        "State lives in ") + Locator::stateDir());
    note->setWordWrap(true); note->setStyleSheet(QStringLiteral("color: palette(mid)"));
    form->addRow(note);
    return w;
}

void MainWindow::refreshFix()
{
    const Fix &f = m_loc->fix();
    if (!f.valid) {
        m_place->setText(QStringLiteral("No location yet"));
        m_coords->setText(m_loc->lastError());
        m_meta->clear();
        m_chip->setText(QStringLiteral("NO FIX")); m_chip->setStyleSheet(QStringLiteral("QLabel { color: white; background: #ff4f4f; border-radius: 9px; padding: 2px 10px; font-weight: 600; }"));
        return;
    }
    const QString chipBg = f.source == QLatin1String("starlink") ? QStringLiteral("#6cff8a") : f.source == QLatin1String("wifi") ? QStringLiteral("#35d6ff") : QStringLiteral("#ffd166");
    m_chip->setText(f.source == QLatin1String("starlink") ? QStringLiteral("GPS") : f.source == QLatin1String("wifi") ? QStringLiteral("WI-FI") : QStringLiteral("IP"));
    m_chip->setStyleSheet(QStringLiteral("QLabel { color: #0b101a; background: %1; border-radius: 9px; padding: 2px 10px; font-weight: 600; }").arg(chipBg));
    m_place->setText(f.place);
    m_coords->setText(QStringLiteral("%1, %2").arg(f.lat, 0, 'f', 6).arg(f.lon, 0, 'f', 6));
    QString meta = QStringLiteral("%1 · ±%2 m · %3").arg(sourceName(f.source)).arg(qRound(f.accuracy)).arg(f.time.toString(QStringLiteral("ddd d MMM HH:mm")));
    if (f.source == QLatin1String("wifi")) meta += QStringLiteral(" · %1 of %2 APs used").arg(f.apUsed).arg(f.apCount);
    if (!m_loc->lastError().isEmpty()) meta += QStringLiteral("\nLast attempt failed: ") + m_loc->lastError();
    const Stats st = m_loc->stats();
    meta += QStringLiteral("\n%1 · Lv %2 · %3 beacons logged · %4 stops · %5 km").arg(st.rank).arg(st.rankLevel).arg(st.beaconsTotal).arg(st.stops).arg(st.distanceKm, 0, 'f', 1);
    m_meta->setText(meta);
    if (f.source == QLatin1String("ip") && m_map->zoom() > 11) m_map->setZoom(10);
    else if (f.source != QLatin1String("ip") && m_map->zoom() < 14) m_map->setZoom(15);
}

void MainWindow::refreshAps()
{
    const auto &aps = m_loc->accessPoints();
    m_aps->setRowCount(aps.size());
    for (int i = 0; i < aps.size(); ++i) {
        const AccessPoint &ap = aps[i];
        const QString st = m_loc->apStatus(ap);
        auto *c0 = new QTableWidgetItem(ap.ssid.isEmpty() ? QStringLiteral("(hidden)") : ap.ssid);
        c0->setData(Qt::UserRole, ap.bssid);
        m_aps->setItem(i, 0, c0);
        m_aps->setItem(i, 1, new QTableWidgetItem(ap.bssid));
        auto *c2 = new QTableWidgetItem(QString::number(ap.dbm)); c2->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_aps->setItem(i, 2, c2);
        auto *c3 = new QTableWidgetItem(QString::number(ap.frequency)); c3->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_aps->setItem(i, 3, c3);
        const QString label = st == QLatin1String("used") ? QStringLiteral("used")
                            : st == QLatin1String("active") ? QStringLiteral("connected — travels with me")
                            : st == QLatin1String("travelling") ? QStringLiteral("travels with me")
                            : st == QLatin1String("nomap") ? QStringLiteral("opted out (_nomap)") : QStringLiteral("ignored");
        auto *c4 = new QTableWidgetItem(label);
        if (st != QLatin1String("used")) c4->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        m_aps->setItem(i, 4, c4);
        const ApEstimate e = m_loc->estimateFor(ap);
        const ApRecord *r = m_loc->record(ap.bssid);
        const QString where = e.kind == ApEstimate::Wigle ? QStringLiteral("WiGLE %1, %2").arg(e.lat, 0, 'f', 5).arg(e.lon, 0, 'f', 5)
                            : e.kind == ApEstimate::Centroid ? QStringLiteral("est. %1, %2 ±%3 m (%4 obs)").arg(e.lat, 0, 'f', 5).arg(e.lon, 0, 'f', 5).arg(qRound(e.radiusM)).arg(r ? r->obs.size() : 0)
                            : e.kind == ApEstimate::Ring ? QStringLiteral("~%1 m away, bearing unknown").arg(qRound(e.radiusM)) : QString();
        m_aps->setItem(i, 5, new QTableWidgetItem(where));
    }
}

void MainWindow::apContextMenu(const QPoint &pos)
{
    const int row = m_aps->rowAt(pos.y());
    if (row < 0) return;
    const QString bssid = m_aps->item(row, 0)->data(Qt::UserRole).toString();
    const QString ssid = m_aps->item(row, 0)->text();
    QMenu menu(this);
    QAction *trav = menu.addAction(QStringLiteral("Travels with me (exclude)"));
    trav->setCheckable(true); trav->setChecked(m_loc->isTravelling(bssid));
    QAction *ign = menu.addAction(QStringLiteral("Always ignore SSID \"%1\"").arg(ssid));
    QAction *cp = menu.addAction(QStringLiteral("Copy BSSID"));
    QAction *chosen = menu.exec(m_aps->viewport()->mapToGlobal(pos));
    if (chosen == trav) m_loc->setTravelling(bssid, trav->isChecked());
    else if (chosen == ign) m_loc->addIgnorePattern(ssid), m_ignore->setPlainText(m_loc->ignorePatterns().join('\n'));
    else if (chosen == cp) QApplication::clipboard()->setText(bssid);
}

void MainWindow::refreshHistory()
{
    const auto &h = m_loc->history();
    m_history->setRowCount(h.size());
    for (int i = 0; i < h.size(); ++i) {
        const Fix &f = h[h.size() - 1 - i];   // newest first
        m_history->setItem(i, 0, new QTableWidgetItem(f.time.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        m_history->setItem(i, 1, new QTableWidgetItem(f.place));
        m_history->setItem(i, 2, new QTableWidgetItem(QString::number(f.lat, 'f', 5)));
        m_history->setItem(i, 3, new QTableWidgetItem(QString::number(f.lon, 'f', 5)));
        m_history->setItem(i, 4, new QTableWidgetItem(f.accuracy >= 0 ? QString::number(qRound(f.accuracy)) : QString()));
        m_history->setItem(i, 5, new QTableWidgetItem(f.source));
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    hide();       // the tray keeps running
    e->ignore();
}
