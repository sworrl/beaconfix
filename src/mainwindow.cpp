#include "mainwindow.h"
#include "locator.h"
#include "beaconview.h"
#include "apiserver.h"
#include "mapdb.h"
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
#include <QGridLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>

// Sorts by the number in Qt::UserRole rather than the display text
struct NumericItem : QTableWidgetItem {
    using QTableWidgetItem::QTableWidgetItem;
    bool operator<(const QTableWidgetItem &o) const override { return data(Qt::UserRole).toDouble() < o.data(Qt::UserRole).toDouble(); }
};

static QString sourceName(const Fix &f)
{
    const QString &s = f.source;
    if (s == QLatin1String("starlink")) return QStringLiteral("Starlink dish GPS");
    if (s == QLatin1String("wifi"))     return f.provider == QLatin1String("apple") ? QStringLiteral("Apple Wi-Fi") : QStringLiteral("BeaconDB Wi-Fi");
    if (s == QLatin1String("ip"))       return QStringLiteral("IP geolocation (approximate)");
    return QStringLiteral("—");
}

MainWindow::MainWindow(Locator *loc, TileSource *tiles, QWidget *parent) : QMainWindow(parent), m_loc(loc)
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
    auto *share = new QPushButton(QIcon::fromTheme(QStringLiteral("document-share")), QStringLiteral("Share ▾"));
    auto *shareMenu = new QMenu(share);
    const std::pair<const char *, const char *> shares[] = {{"geo", "Copy geo: URI"}, {"text", "Copy place + link"}, {"osm", "Copy OpenStreetMap link"},
                                                            {"google", "Copy Google Maps link"}, {"apple", "Copy Apple Maps link"}};
    for (const auto &sh : shares) {
        const QString what = QString::fromLatin1(sh.first);
        connect(shareMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QString::fromLatin1(sh.second)), &QAction::triggered, this, [this, what] {
            if (m_loc->CopyToClipboard(what)) statusBar()->showMessage(QStringLiteral("Copied"), 2000); });
    }
    shareMenu->addSeparator();
    connect(shareMenu->addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("Open in Google Maps")), &QAction::triggered, this, [this] {
        if (m_loc->fix().valid) QDesktopServices::openUrl(QUrl(m_loc->googleMapsUrl())); });
    connect(shareMenu->addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("Open in Apple Maps")), &QAction::triggered, this, [this] {
        if (m_loc->fix().valid) QDesktopServices::openUrl(QUrl(m_loc->appleMapsUrl())); });
    shareMenu->addSeparator();
    connect(shareMenu->addAction(QIcon::fromTheme(QStringLiteral("document-export")), QStringLiteral("Save trip as GPX…")), &QAction::triggered, this, [this] {
        const QString def = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/beaconfix-trip.gpx";
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save trip as GPX"), def, QStringLiteral("GPX (*.gpx)"));
        if (path.isEmpty()) return;
        QString err;
        if (!m_loc->exportGpx(path, &err)) QMessageBox::warning(this, QStringLiteral("Export failed"), err);
        else statusBar()->showMessage(QStringLiteral("Exported %1 fixes to %2").arg(m_loc->history().size()).arg(path), 5000);
    });
    share->setMenu(shareMenu);
    for (QPushButton *b : {m_refresh, copy, osm, share}) buttons->addWidget(b);
    buttons->addStretch();
    card->addLayout(buttons);
    outer->addLayout(card);

    // ── Tabs ──────────────────────────────────────────────────────────────────
    auto *tabs = m_tabs = new QTabWidget;
    m_map = new BeaconView(m_loc, tiles);
    tabs->addTab(m_map, QIcon::fromTheme(QStringLiteral("map-globe")), QStringLiteral("Map"));

    // ── Nearby places ─────────────────────────────────────────────────────────
    auto *nearby = new QWidget;
    auto *nl = new QVBoxLayout(nearby);
    auto *nbar = new QHBoxLayout;
    m_poiFilter = new QLineEdit;
    m_poiFilter->setPlaceholderText(QStringLiteral("Filter — name, kind or detail (e.g. diesel, laundry, free)"));
    m_poiFilter->setClearButtonEnabled(true);
    connect(m_poiFilter, &QLineEdit::textChanged, this, &MainWindow::refreshPois);
    auto *reload = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Reload"));
    connect(reload, &QPushButton::clicked, m_loc, [this] { m_loc->refreshPois(true); });
    m_poiNote = new QLabel;
    nbar->addWidget(m_poiFilter, 1); nbar->addWidget(reload);
    nl->addLayout(nbar); nl->addWidget(m_poiNote);
    m_pois = new QTableWidget(0, 5);
    m_pois->setHorizontalHeaderLabels({QStringLiteral("Place"), QStringLiteral("Kind"), QStringLiteral("Distance"), QStringLiteral("Details"), QStringLiteral("Hours")});
    m_pois->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_pois->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_pois->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pois->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pois->verticalHeader()->hide();
    m_pois->setSortingEnabled(true);
    m_pois->setToolTip(QStringLiteral("Double-click to show on the map"));
    connect(m_pois, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const int idx = m_pois->item(row, 0)->data(Qt::UserRole).toInt();
        m_tabs->setCurrentWidget(m_map);
        m_map->selectPoi(idx);
    });
    nl->addWidget(m_pois, 1);
    tabs->addTab(nearby, QIcon::fromTheme(QStringLiteral("find-location")), QStringLiteral("Nearby"));

    m_aps = new QTableWidget(0, 7);
    m_aps->setHorizontalHeaderLabels({QStringLiteral("SSID"), QStringLiteral("BSSID"), QStringLiteral("dBm"), QStringLiteral("MHz"), QStringLiteral("Security"), QStringLiteral("Status"), QStringLiteral("Where")});
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
    tabs->addTab(buildTrip(), QIcon::fromTheme(QStringLiteral("flag")), QStringLiteral("Trip"));
    tabs->addTab(m_history, QIcon::fromTheme(QStringLiteral("view-history")), QStringLiteral("Trip log"));

    tabs->addTab(buildSettings(), QIcon::fromTheme(QStringLiteral("configure")), QStringLiteral("Settings"));
    if (m_loc->apiServer()) tabs->addTab(buildDevices(), QIcon::fromTheme(QStringLiteral("network-connect")), QStringLiteral("Devices"));
    outer->addWidget(tabs, 1);
    setCentralWidget(central);

    m_status = new QLabel;
    statusBar()->addWidget(m_status, 1);

    connect(m_loc, &Locator::FixChanged, this, &MainWindow::refreshFix);
    connect(m_loc, &Locator::FixChanged, this, &MainWindow::refreshHistory);
    connect(m_loc, &Locator::FixChanged, this, &MainWindow::refreshTrip);
    connect(m_loc, &Locator::elevationUpdated, this, &MainWindow::refreshFix);
    connect(m_loc, &Locator::elevationUpdated, this, &MainWindow::refreshTrip);
    connect(m_loc, &Locator::achievementUnlocked, this, [this](const QString &, const QString &) { refreshTrip(); });
    connect(m_loc, &Locator::scanUpdated, this, &MainWindow::refreshTrip);
    connect(m_loc, &Locator::scanUpdated, this, &MainWindow::refreshAps);
    connect(m_loc, &Locator::poisUpdated, this, &MainWindow::refreshPois);
    connect(m_loc, &Locator::FixChanged, this, &MainWindow::refreshPois);
    connect(m_loc, &Locator::probeStarted, this, [this] { m_refresh->setEnabled(false); });
    connect(m_loc, &Locator::probeFinished, this, [this](bool, const QString &) { m_refresh->setEnabled(true); refreshFix(); });
    connect(m_loc, &Locator::statusMessage, this, [this](const QString &m) { m_status->setText(m.section('\n', 0, 0)); m_status->setToolTip(m); });
    refreshFix(); refreshAps(); refreshHistory(); refreshPois(); refreshTrip();
}

QWidget *MainWindow::buildTrip()
{
    auto *w = new QWidget;
    auto *l = new QVBoxLayout(w);
    auto *top = new QHBoxLayout;
    m_tripSummary = new QLabel; m_tripSummary->setTextInteractionFlags(Qt::TextSelectableByMouse); m_tripSummary->setWordWrap(true);
    m_tripPlaces = new QLabel; m_tripPlaces->setTextInteractionFlags(Qt::TextSelectableByMouse); m_tripPlaces->setWordWrap(true);
    m_tripRecords = new QLabel; m_tripRecords->setWordWrap(true); m_tripRecords->setStyleSheet(QStringLiteral("color: palette(mid)"));
    auto *left = new QVBoxLayout; left->addWidget(m_tripSummary); left->addWidget(m_tripPlaces); left->addWidget(m_tripRecords); left->addStretch();
    top->addLayout(left, 3);
    auto *btns = new QVBoxLayout;
    auto *newTrip = new QPushButton(QIcon::fromTheme(QStringLiteral("flag")), QStringLiteral("Start a new trip here"));
    newTrip->setToolTip(QStringLiteral("Trip distance, stops and time count from now on. All-time totals keep everything."));
    connect(newTrip, &QPushButton::clicked, m_loc, &Locator::StartTrip);
    auto *offline = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), QStringLiteral("Save map around here for offline"));
    offline->setToolTip(QStringLiteral("Downloads the tiles for ~10 km around the fix (zoom 10–15, current style) into the tile cache."));
    connect(offline, &QPushButton::clicked, m_loc, &Locator::PrefetchTiles);
    btns->addWidget(newTrip); btns->addWidget(offline); btns->addStretch();
    top->addLayout(btns, 1);
    l->addLayout(top);

    auto *split = new QHBoxLayout;
    m_stops = new QTableWidget(0, 6);
    m_stops->setHorizontalHeaderLabels({QStringLiteral("Arrived"), QStringLiteral("Place"), QStringLiteral("Stayed"), QStringLiteral("Leg"), QStringLiteral("Travel"), QStringLiteral("Elev.")});
    m_stops->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_stops->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_stops->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_stops->verticalHeader()->hide();
    m_stops->setToolTip(QStringLiteral("Precise stops in bold; IP-only entries are the ground-station city and don't count toward distances"));
    connect(m_stops, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const int idx = m_stops->item(row, 0)->data(Qt::UserRole).toInt();
        const auto &h = m_loc->history();
        if (idx >= 0 && idx < h.size()) { m_tabs->setCurrentWidget(m_map); m_map->focusOn(h[idx].lat, h[idx].lon, 14); }
    });
    split->addWidget(m_stops, 3);
    auto *achBox = new QVBoxLayout;
    auto *achTitle = new QLabel(QStringLiteral("Milestones")); { QFont f = achTitle->font(); f.setBold(true); achTitle->setFont(f); }
    m_achList = new QListWidget;
    m_achList->setToolTip(QStringLiteral("A display, not a game: nothing to click, nothing to win."));
    achBox->addWidget(achTitle); achBox->addWidget(m_achList, 1);
    split->addLayout(achBox, 2);
    l->addLayout(split, 1);
    return w;
}

void MainWindow::refreshTrip()
{
    const Stats st = m_loc->stats();
    const Fix &f = m_loc->fix();
    QString s = QStringLiteral("<b style='font-size:larger'>%1</b> · level %2 of %3 · %4 beacons logged%5<br>")
                    .arg(st.rank).arg(st.rankLevel).arg(st.rankCount).arg(st.beaconsTotal)
                    .arg(st.nextRankAt ? QStringLiteral(" · next rank at %1").arg(st.nextRankAt) : QStringLiteral(" · top of the ladder"));
    s += QStringLiteral("<b>Today</b> %1 km · %2 stops&nbsp;&nbsp; <b>This trip</b> %3 km · %4 stops · day %5%6&nbsp;&nbsp; <b>All time</b> %7 km · %8 stops<br>")
             .arg(st.distanceTodayKm, 0, 'f', 1).arg(st.stopsToday).arg(st.distanceTripKm, 0, 'f', 0).arg(st.stopsTrip).arg(st.tripDays)
             .arg(st.tripStart.isValid() ? QStringLiteral(" (since %1)").arg(st.tripStart.toString(QStringLiteral("d MMM"))) : QString())
             .arg(st.distanceAllKm, 0, 'f', 0).arg(st.stops);
    s += QStringLiteral("<b>Moving</b> %1 · <b>stopped</b> %2 (trip: %3 / %4)<br>")
             .arg(Locator::durationText(st.movingSecs), Locator::durationText(st.stoppedSecs), Locator::durationText(st.movingTripSecs), Locator::durationText(st.stoppedTripSecs));
    if (st.moving && st.speedKmh >= 0) s += QStringLiteral("<b>Moving</b> ~%1 km/h heading %2 (%3°) on the last leg<br>").arg(qRound(st.speedKmh)).arg(Locator::compass(st.headingDeg)).arg(qRound(st.headingDeg));
    else if (st.dwellSecs >= 0) s += QStringLiteral("<b>Here</b> for %1%2<br>").arg(Locator::durationText(st.dwellSecs))
                                          .arg(st.headingDeg >= 0 ? QStringLiteral(" · arrived heading %1 at ~%2 km/h").arg(Locator::compass(st.headingDeg)).arg(qRound(qMax(0.0, st.speedKmh))) : QString());
    if (f.valid) {
        s += QStringLiteral("<b>Elevation</b> %1%2<br>").arg(f.hasElevation() ? m_loc->elevationText() : (f.precise() ? QStringLiteral("looking up…") : QStringLiteral("n/a for an approximate (IP) fix")))
                 .arg(m_loc->elevationNote().isEmpty() ? QString() : QStringLiteral(" (%1)").arg(m_loc->elevationNote()));
        const SunTimes su = m_loc->sun();
        if (su.valid) {
            if (su.polarDay) s += QStringLiteral("<b>Sun</b> up all day<br>");
            else if (su.polarNight) s += QStringLiteral("<b>Sun</b> below the horizon all day<br>");
            else s += QStringLiteral("<b>Sun</b> rise %1 · set %2 · day %3 · golden hour until %4 and from %5 · civil dusk %6<br>")
                          .arg(su.sunrise.toString(QStringLiteral("HH:mm")), su.sunset.toString(QStringLiteral("HH:mm")), Locator::durationText(su.dayLengthSecs),
                               su.goldenMorningEnd.isValid() ? su.goldenMorningEnd.toString(QStringLiteral("HH:mm")) : QStringLiteral("—"),
                               su.goldenEveningStart.isValid() ? su.goldenEveningStart.toString(QStringLiteral("HH:mm")) : QStringLiteral("—"),
                               su.civilDusk.isValid() ? su.civilDusk.toString(QStringLiteral("HH:mm")) : QStringLiteral("—"));
        }
    }
    m_tripSummary->setText(s);
    QString pl = QStringLiteral("<b>Places visited</b> (precise fixes): ");
    if (st.cities.isEmpty() && st.regions.isEmpty()) pl += QStringLiteral("none yet");
    else pl += QStringLiteral("%1 cities · %2 states/provinces · %3 countries<br>%4%5")
                   .arg(st.cities.size()).arg(st.regions.size()).arg(st.countries.size())
                   .arg(st.regions.isEmpty() ? QString() : st.regions.join(QStringLiteral(", ")))
                   .arg(st.countries.isEmpty() ? QString() : QStringLiteral(" — ") + st.countries.join(QStringLiteral(", ")));
    m_tripPlaces->setText(pl);
    m_tripRecords->setText(QStringLiteral("Longest leg %1 km · longest stay %2%3 · best fix ±%4 m · %5 of %6 milestones")
                               .arg(st.longestLegKm, 0, 'f', 0).arg(Locator::durationText(st.longestStaySecs))
                               .arg(st.longestStayPlace.isEmpty() ? QString() : QStringLiteral(" at %1").arg(st.longestStayPlace))
                               .arg(st.bestAccuracy >= 0 ? qRound(st.bestAccuracy) : 0).arg(st.achievementsUnlocked).arg(st.achievementsTotal));

    const QList<Stop> stops = m_loc->stops();
    m_stops->setRowCount(stops.size());
    for (int i = 0; i < stops.size(); ++i) {
        const Stop &sp = stops[stops.size() - 1 - i];      // newest first
        const int idx = stops.size() - 1 - i;
        auto *c0 = new QTableWidgetItem(sp.fix.time.toString(QStringLiteral("ddd d MMM HH:mm"))); c0->setData(Qt::UserRole, idx);
        auto *c1 = new QTableWidgetItem(sp.fix.place);
        if (sp.fix.precise()) { QFont b = c1->font(); b.setBold(true); c1->setFont(b); } else c1->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        m_stops->setItem(i, 0, c0); m_stops->setItem(i, 1, c1);
        m_stops->setItem(i, 2, new QTableWidgetItem(sp.dwellSecs >= 0 ? Locator::durationText(sp.dwellSecs) : QString()));
        m_stops->setItem(i, 3, new QTableWidgetItem(sp.legKm >= 0 ? QStringLiteral("%1 km").arg(sp.legKm, 0, 'f', sp.legKm < 10 ? 1 : 0) : QString()));
        m_stops->setItem(i, 4, new QTableWidgetItem(sp.legSecs >= 0 ? QStringLiteral("%1%2").arg(Locator::durationText(sp.legSecs))
                                                                        .arg(sp.legKm > 0 ? QStringLiteral(" · %1 km/h").arg(qRound(sp.legKm / (sp.legSecs / 3600.0))) : QString()) : QString()));
        m_stops->setItem(i, 5, new QTableWidgetItem(sp.fix.hasElevation() ? QStringLiteral("%1 m").arg(qRound(sp.fix.elevation)) : QString()));
    }
    m_achList->clear();
    QList<Achievement> ach = m_loc->achievements();
    std::stable_sort(ach.begin(), ach.end(), [](const Achievement &a, const Achievement &b) { return a.unlocked.isValid() && !b.unlocked.isValid(); });
    for (const Achievement &a : ach) {
        auto *it = new QListWidgetItem(QStringLiteral("%1  %2 — %3%4").arg(a.icon, a.title, a.desc)
                                          .arg(a.unlocked.isValid() ? QStringLiteral("  (%1)").arg(a.unlocked.toString(QStringLiteral("d MMM"))) : QString()));
        if (!a.unlocked.isValid()) it->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        else { QFont b = it->font(); b.setBold(true); it->setFont(b); }
        m_achList->addItem(it);
    }
}

QWidget *MainWindow::buildSettings()
{
    auto *w = new QWidget;
    auto *form = new QFormLayout(w);
    m_interval = new QSpinBox; m_interval->setRange(1, 1440); m_interval->setSuffix(QStringLiteral(" min")); m_interval->setValue(m_loc->intervalMinutes());
    connect(m_interval, &QSpinBox::valueChanged, m_loc, &Locator::setIntervalMinutes);
    form->addRow(QStringLiteral("Check every:"), m_interval);
    m_liveScan = new QSpinBox; m_liveScan->setRange(0, 600); m_liveScan->setSingleStep(15); m_liveScan->setSuffix(QStringLiteral(" s")); m_liveScan->setSpecialValueText(QStringLiteral("off"));
    m_liveScan->setValue(m_loc->liveScanSeconds());
    m_liveScan->setToolTip(QStringLiteral("Between position checks, rescan Wi-Fi this often to keep the beacons live (new / faded / louder / quieter). No geolocation is done unless the neighbourhood changes."));
    connect(m_liveScan, &QSpinBox::valueChanged, m_loc, &Locator::setLiveScanSeconds);
    form->addRow(QStringLiteral("Live beacon scan:"), m_liveScan);
    m_names = new QCheckBox(QStringLiteral("Show Wi-Fi names on the map (all at z ≥ 15, the loudest 12 from z 13)"));
    m_names->setChecked(m_map->showNames());
    connect(m_names, &QCheckBox::toggled, m_map, &BeaconView::setShowNames);
    form->addRow(QStringLiteral("Map:"), m_names);

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
    m_apple = new QCheckBox(QStringLiteral("When BeaconDB has no match, ask Apple's Wi-Fi positioning service (keyless; sends the BSSIDs you hear to Apple)"));
    m_apple->setChecked(m_loc->useApple());
    connect(m_apple, &QCheckBox::toggled, m_loc, &Locator::setUseApple);
    form->addRow(QString(), m_apple);

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

    m_notifyStops = new QCheckBox(QStringLiteral("Notify when a new stop is logged"));
    m_notifyStops->setChecked(m_loc->notifyStops()); connect(m_notifyStops, &QCheckBox::toggled, m_loc, &Locator::setNotifyStops);
    m_notifyRegions = new QCheckBox(QStringLiteral("Notify on entering a new state / province / country"));
    m_notifyRegions->setChecked(m_loc->notifyRegions()); connect(m_notifyRegions, &QCheckBox::toggled, m_loc, &Locator::setNotifyRegions);
    m_notifyAch = new QCheckBox(QStringLiteral("Notify on milestones"));
    m_notifyAch->setChecked(m_loc->notifyAchievements()); connect(m_notifyAch, &QCheckBox::toggled, m_loc, &Locator::setNotifyAchievements);
    form->addRow(QStringLiteral("Notifications:"), m_notifyStops); form->addRow(QString(), m_notifyRegions); form->addRow(QString(), m_notifyAch);
    // ── Internal mapping database ──
    {
        auto *dbBox = new QWidget; auto *dv = new QVBoxLayout(dbBox); dv->setContentsMargins(0, 0, 0, 0);
        auto *dbInfo = new QLabel; dbInfo->setTextInteractionFlags(Qt::TextSelectableByMouse); dbInfo->setWordWrap(true);
        auto *dbRow = new QHBoxLayout;
        auto *exp = new QPushButton(QIcon::fromTheme(QStringLiteral("document-export")), QStringLiteral("Export…"));
        auto *imp = new QPushButton(QIcon::fromTheme(QStringLiteral("document-import")), QStringLiteral("Import…"));
        auto *rebuild = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Rebuild from JSON"));
        rebuild->setToolTip(QStringLiteral("Re-import the *.migrated JSON state files (aps.json, history.jsonl, …) into the database."));
        dbRow->addWidget(exp); dbRow->addWidget(imp); dbRow->addWidget(rebuild); dbRow->addStretch();
        dv->addWidget(dbInfo); dv->addLayout(dbRow);
        auto refreshDb = [this, dbInfo] {
            MapDb *db = m_loc->mapDb();
            if (!db) { dbInfo->setText(QStringLiteral("no database")); return; }
            const QJsonObject s = db->stats();
            if (!s["open"].toBool()) { dbInfo->setText(QStringLiteral("Not open: %1").arg(s["error"].toString())); return; }
            dbInfo->setText(QStringLiteral("%1\n%2 · key from %3 · %4 KB on disk%5\n%6 access points (%7 positioned, %8 home) · %9 observations · %10 stops · %11 places · %12 elevation cells")
                            .arg(s["path"].toString(), s["cipher"].toString(), s["keySource"].toString()).arg(qRound(s["sizeBytes"].toDouble() / 1024.0))
                            .arg(s["migrated"].isString() ? QStringLiteral(" · migrated from JSON %1").arg(s["migrated"].toString().left(10)) : QString())
                            .arg(s["aps"].toInt()).arg(s["apsPositioned"].toInt()).arg(s["apsHome"].toInt()).arg(s["observations"].toInt()).arg(s["fixes"].toInt()).arg(s["pois"].toInt()).arg(s["elevation"].toInt()));
        };
        connect(exp, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export the map database"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/beaconfix-mapdb.json"), QStringLiteral("JSON (*.json)"));
            if (path.isEmpty()) return;
            if (m_loc->DbExport(path)) statusBar()->showMessage(QStringLiteral("Exported to %1").arg(path), 4000); else QMessageBox::warning(this, QStringLiteral("Export failed"), QStringLiteral("Could not write %1").arg(path));
        });
        connect(imp, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import a map database dump"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), QStringLiteral("JSON (*.json)"));
            if (path.isEmpty()) return;
            const int n = m_loc->DbImport(path);
            if (n < 0) QMessageBox::warning(this, QStringLiteral("Import failed"), QStringLiteral("Not a BeaconFix database export, or the database is read-only.")); else statusBar()->showMessage(QStringLiteral("Imported %1 row(s)").arg(n), 4000);
        });
        connect(rebuild, &QPushButton::clicked, this, [this] {
            const int n = m_loc->rebuildDbFromJson();
            statusBar()->showMessage(n > 0 ? QStringLiteral("Rebuilt from %1 JSON file(s)").arg(n) : n == 0 ? QStringLiteral("No *.migrated JSON files to rebuild from") : QStringLiteral("Database is not writable"), 5000);
        });
        if (MapDb *db = m_loc->mapDb()) connect(db, &MapDb::changed, this, refreshDb);
        refreshDb();
        form->addRow(QStringLiteral("Map database:"), dbBox);
    }

    // ── Positioning: our own fits ──
    {
        auto *box = new QWidget; auto *v = new QVBoxLayout(box); v->setContentsMargins(0, 0, 0, 0);
        auto *info = new QLabel; info->setWordWrap(true);
        auto *row = new QHBoxLayout;
        auto *refit = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Refit all beacons"));
        refit->setToolTip(QStringLiteral("Re-estimate every beacon's position from all its samples with the least-squares fit (a log-distance path-loss model, robust weights). Beacons with new samples are refit automatically ten seconds after a scan."));
        row->addWidget(refit); row->addStretch();
        v->addWidget(info); v->addLayout(row);
        auto refreshInfo = [this, info] {
            int fitted = m_loc->refitCount(), withSamples = 0, samples = 0;
            for (const AccessPoint &ap : m_loc->accessPoints()) if (const ApRecord *r = m_loc->record(ap.bssid)) { if (r->obs.size() >= 3) ++withSamples; samples += r->obs.size(); }
            info->setText(QStringLiteral("%1 beacons positioned from your own samples · %2 in range with enough samples to fit · %3 samples on the beacons in range\nEvery scan taken while the fix is fresh and tight (GPS, or Wi-Fi within two minutes) adds a sample; walk or drive around and the positions tighten. Samples from paired devices and synced peers count too.")
                          .arg(fitted).arg(withSamples).arg(samples));
        };
        connect(refit, &QPushButton::clicked, this, [this, refit] { refit->setEnabled(false); const int n = m_loc->Refit(); statusBar()->showMessage(QStringLiteral("Refit done: %1 beacons positioned").arg(n), 5000); refit->setEnabled(true); });
        connect(m_loc, &Locator::refitDone, this, refreshInfo);
        connect(m_loc, &Locator::scanUpdated, this, refreshInfo);
        refreshInfo();
        form->addRow(QStringLiteral("Positioning:"), box);
    }
    // ── Sync with another BeaconFix ──
    {
        auto *box = new QWidget; auto *v = new QVBoxLayout(box); v->setContentsMargins(0, 0, 0, 0);
        auto *hint = new QLabel(QStringLiteral("A laptop carried around feeds this BeaconFix its samples and gets the map back (and the other way round). Pair this machine with the other one's LAN API first (its Devices tab → token or pairing), then enter its address and token here."));
        hint->setWordWrap(true); hint->setStyleSheet(QStringLiteral("color: palette(mid)"));
        auto *grid = new QGridLayout;
        auto *url = new QLineEdit; url->setPlaceholderText(QStringLiteral("http://<beaconfix-host>:47822"));
        auto *tok = new QLineEdit; tok->setPlaceholderText(QStringLiteral("token from the other BeaconFix")); tok->setEchoMode(QLineEdit::Password);
        auto *mins = new QSpinBox; mins->setRange(0, 1440); mins->setSuffix(QStringLiteral(" min")); mins->setSpecialValueText(QStringLiteral("manual only")); mins->setValue(15);
        auto *now = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Sync now"));
        auto *status = new QLabel; status->setWordWrap(true);
        const QList<Locator::SyncPeer> peers = m_loc->syncPeers();
        if (!peers.isEmpty()) { url->setText(peers.first().url); tok->setText(peers.first().token); mins->setValue(peers.first().minutes); }
        grid->addWidget(new QLabel(QStringLiteral("Other BeaconFix:")), 0, 0); grid->addWidget(url, 0, 1);
        grid->addWidget(new QLabel(QStringLiteral("Token:")), 1, 0); grid->addWidget(tok, 1, 1);
        grid->addWidget(new QLabel(QStringLiteral("Every:")), 2, 0); grid->addWidget(mins, 2, 1);
        auto *row = new QHBoxLayout; row->addWidget(now); row->addStretch();
        v->addWidget(hint); v->addLayout(grid); v->addLayout(row); v->addWidget(status);
        auto apply = [this, url, tok, mins] {
            QList<Locator::SyncPeer> l;
            if (!url->text().trimmed().isEmpty()) { Locator::SyncPeer p; p.url = url->text().trimmed(); p.token = tok->text().trimmed(); p.minutes = mins->value(); l << p; }
            m_loc->setSyncPeers(l);
        };
        auto refreshStatus = [this, status] {
            const QList<Locator::SyncPeer> l = m_loc->syncPeers();
            if (l.isEmpty()) { status->setText(QStringLiteral("No peer configured.")); return; }
            const Locator::SyncPeer &p = l.first();
            status->setText(p.last.isValid() ? QStringLiteral("%1 · last sync %2: %3").arg(p.ok ? QStringLiteral("✓") : QStringLiteral("✗"), p.last.toString(QStringLiteral("ddd HH:mm")), p.lastResult) : QStringLiteral("Never synced yet."));
        };
        connect(url, &QLineEdit::editingFinished, this, apply);
        connect(tok, &QLineEdit::editingFinished, this, apply);
        connect(mins, &QSpinBox::valueChanged, this, [apply](int) { apply(); });
        connect(now, &QPushButton::clicked, this, [this, apply, url, tok, now, status] {
            apply();
            if (url->text().trimmed().isEmpty() || tok->text().trimmed().isEmpty()) { status->setText(QStringLiteral("Enter the other BeaconFix's address and token first.")); return; }
            now->setEnabled(false); status->setText(QStringLiteral("Syncing…"));
            const QJsonObject r = QJsonDocument::fromJson(m_loc->Sync(url->text().trimmed(), tok->text().trimmed()).toUtf8()).object();
            now->setEnabled(true);
            statusBar()->showMessage(r["ok"].toBool() ? QStringLiteral("Synced: %1").arg(r["message"].toString()) : QStringLiteral("Sync failed: %1").arg(r["message"].toString().isEmpty() ? r["error"].toString() : r["message"].toString()), 6000);
        });
        connect(m_loc, &Locator::syncFinished, this, refreshStatus);
        refreshStatus();
        form->addRow(QStringLiteral("Sync:"), box);
    }

    m_prefetch = new QCheckBox(QStringLiteral("Save map tiles around each new stop for offline use (~10 km, zoom 10–15, ≤400 tiles)"));
    m_prefetch->setChecked(m_loc->prefetchTiles()); connect(m_prefetch, &QCheckBox::toggled, m_loc, &Locator::setPrefetchTiles);
    form->addRow(QStringLiteral("Offline:"), m_prefetch);
    m_elev = new QCheckBox(QStringLiteral("Look up elevation at each precise stop (Open Topo Data, SRTM 30 m)"));
    m_elev->setChecked(m_loc->useElevation()); connect(m_elev, &QCheckBox::toggled, m_loc, &Locator::setUseElevation);
    form->addRow(QStringLiteral("Elevation:"), m_elev);

    m_home = new QPlainTextEdit(m_loc->homeNetworks().join('\n'));
    m_home->setPlaceholderText(QStringLiteral("One glob per line — the networks that travel WITH you (SSIDs or BSSIDs), e.g.\nMyRouter*\nAA:BB:CC:?D:EE:F?"));
    m_home->setMaximumHeight(110);
    m_home->setToolTip(QStringLiteral("Home networks are never used for positioning and never 'appear' or 'fade'. Hearing one means 'at the RV'; the last precise fix taken at the RV is where the RV is, so when you walk away with the laptop BeaconFix can say how far you are from it."));
    connect(m_home, &QPlainTextEdit::textChanged, this, [this] { m_loc->setHomeNetworks(m_home->toPlainText().split('\n')); });
    auto *homeRow = new QHBoxLayout; auto *homeBox = new QWidget; homeBox->setLayout(homeRow); homeRow->setContentsMargins(0, 0, 0, 0);
    homeRow->addWidget(m_home, 1);
    auto *suggest = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-find")), QStringLiteral("Suggest"));
    suggest->setToolTip(QStringLiteral("Propose the network you are connected to and its sibling radios (same router, other bands / SSIDs)."));
    connect(suggest, &QPushButton::clicked, this, [this] {
        const QStringList sug = m_loc->suggestHomeNetworks();
        if (sug.isEmpty()) { QMessageBox::information(this, QStringLiteral("Suggest home networks"), QStringLiteral("No access points heard yet.")); return; }
        if (QMessageBox::question(this, QStringLiteral("Suggest home networks"), QStringLiteral("Add these as home networks?\n\n%1").arg(sug.join('\n'))) != QMessageBox::Yes) return;
        QStringList l = m_loc->homeNetworks(); for (const QString &s : sug) if (!l.contains(s, Qt::CaseInsensitive)) l << s;
        m_loc->setHomeNetworks(l); m_home->setPlainText(l.join('\n'));
    });
    homeRow->addWidget(suggest, 0, Qt::AlignTop);
    form->addRow(QStringLiteral("Home networks:"), homeBox);

    m_ignore = new QPlainTextEdit(m_loc->ignorePatterns().join('\n'));
    m_ignore->setPlaceholderText(QStringLiteral("One glob per line, matched against BSSID and SSID, e.g.\nAA:BB:CC:??:EE:FF\nMyHotspot*"));
    m_ignore->setMaximumHeight(120);
    connect(m_ignore, &QPlainTextEdit::textChanged, this, [this] { m_loc->setIgnorePatterns(m_ignore->toPlainText().split('\n')); });
    form->addRow(QStringLiteral("Always ignore:"), m_ignore);

    auto *note = new QLabel(QStringLiteral(
        "Access points seen at two stops more than ~5 km apart are flagged as travelling with you automatically. "
        "Right-click a row in the Access points tab to flag or clear one by hand.\n"
        "Beacon positions: WiGLE (if a token is set) → multilaterated once a beacon has been heard from two or more places further apart than the fixes' own error → "
        "otherwise a ring at the RSSI-estimated distance with a stable pseudo-bearing (the direction is NOT known).\n"
        "Data: BeaconDB (api.beacondb.net) for the fix, Nominatim for place names, Overpass/OpenStreetMap for nearby places, "
        "Open Topo Data for elevation, OSM / Esri / OpenTopoMap tiles for the map. Sun times are computed locally. "
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
    QString meta = QStringLiteral("%1 · ±%2 m · %3").arg(sourceName(f)).arg(qRound(f.accuracy)).arg(f.time.toString(QStringLiteral("ddd d MMM HH:mm")));
    if (f.source == QLatin1String("wifi")) meta += QStringLiteral(" · %1 of %2 APs used").arg(f.apUsed).arg(f.apCount);
    if (!m_loc->lastError().isEmpty()) meta += QStringLiteral("\nLast attempt failed: ") + m_loc->lastError();
    if (!m_loc->coarseNote().isEmpty()) meta += QStringLiteral("\n") + m_loc->coarseNote();
    const Stats st = m_loc->stats();
    meta += QStringLiteral("\n%1 · Lv %2 · %3 beacons logged · %4 stops · today %5 km · trip %6 km").arg(st.rank).arg(st.rankLevel).arg(st.beaconsTotal).arg(st.stops)
                .arg(st.distanceTodayKm, 0, 'f', 1).arg(st.distanceTripKm, 0, 'f', 0);
    QString line;
    if (f.hasElevation()) line += QStringLiteral("⛰ %1 m (%2 ft)").arg(qRound(f.elevation)).arg(qRound(f.elevation * 3.28084));
    const SunTimes su = m_loc->sun();
    if (su.valid && su.sunrise.isValid())
        line += (line.isEmpty() ? QString() : QStringLiteral("  ·  ")) + QStringLiteral("☀ %1 – %2 · golden %3 / %4")
                    .arg(su.sunrise.toString(QStringLiteral("HH:mm")), su.sunset.toString(QStringLiteral("HH:mm")),
                         su.goldenMorningEnd.isValid() ? su.goldenMorningEnd.toString(QStringLiteral("HH:mm")) : QStringLiteral("—"),
                         su.goldenEveningStart.isValid() ? su.goldenEveningStart.toString(QStringLiteral("HH:mm")) : QStringLiteral("—"));
    if (st.moving && st.speedKmh >= 0) line += (line.isEmpty() ? QString() : QStringLiteral("  ·  ")) + QStringLiteral("→ %1 km/h %2").arg(qRound(st.speedKmh)).arg(Locator::compass(st.headingDeg));
    else if (st.dwellSecs > 0) line += (line.isEmpty() ? QString() : QStringLiteral("  ·  ")) + QStringLiteral("here %1").arg(Locator::durationText(st.dwellSecs));
    if (!st.awayText.isEmpty()) line += (line.isEmpty() ? QString() : QStringLiteral("  ·  ")) + QStringLiteral("🏠 %1").arg(st.awayText);
    if (!line.isEmpty()) meta += QStringLiteral("\n") + line;
    m_meta->setText(meta);
}

void MainWindow::refreshPois()
{
    const Fix &f = m_loc->fix();
    const QString needle = m_poiFilter->text().trimmed();
    m_pois->setSortingEnabled(false);
    m_pois->setRowCount(0);
    const auto &pois = m_loc->pois();
    for (int i = 0; i < pois.size(); ++i) {
        const Poi &pt = pois[i];
        const PoiCategory *c = Locator::poiCategory(pt.cat);
        const QString kind = c ? c->label : pt.cat;
        const QString name = pt.name.isEmpty() ? QStringLiteral("(unnamed)") : pt.name;
        if (!needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive) && !kind.contains(needle, Qt::CaseInsensitive)
            && !pt.detail.contains(needle, Qt::CaseInsensitive))
            continue;
        const int row = m_pois->rowCount();
        m_pois->insertRow(row);
        auto *c0 = new QTableWidgetItem(QStringLiteral("%1  %2").arg(c ? c->icon : QString(), name));
        c0->setData(Qt::UserRole, i);
        m_pois->setItem(row, 0, c0);
        m_pois->setItem(row, 1, new QTableWidgetItem(kind));
        const double d = f.valid ? Locator::distanceM(f.lat, f.lon, pt.lat, pt.lon) : 0;
        static const char *pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        const QString dir = QString::fromLatin1(pts[int(std::fmod(Locator::bearingDeg(f.lat, f.lon, pt.lat, pt.lon) + 22.5, 360.0) / 45.0) % 8]);
        auto *c2 = new NumericItem(QStringLiteral("%1 km %2").arg(d / 1000.0, 0, 'f', 1).arg(dir));
        c2->setData(Qt::UserRole, d);
        c2->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_pois->setItem(row, 2, c2);
        m_pois->setItem(row, 3, new QTableWidgetItem(pt.detail + (pt.phone.isEmpty() ? QString() : QStringLiteral("  ☎ ") + pt.phone)));
        m_pois->setItem(row, 4, new QTableWidgetItem(pt.hours));
    }
    m_pois->setSortingEnabled(true);
    m_pois->sortItems(2);
    QString note = m_loc->poisLoading() ? m_loc->poiNote()
                 : QStringLiteral("%1 places within %2 km from OpenStreetMap").arg(pois.size()).arg(m_loc->poiRadiusKm());
    if (!m_loc->poisLoading() && !m_loc->poiNote().isEmpty()) note += QStringLiteral(" — ") + m_loc->poiNote();
    m_poiNote->setText(note);
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
                            : st == QLatin1String("home") ? QStringLiteral("home network (the RV)")
                            : st == QLatin1String("active") ? QStringLiteral("connected — travels with me")
                            : st == QLatin1String("travelling") ? QStringLiteral("travels with me")
                            : st == QLatin1String("nomap") ? QStringLiteral("opted out (_nomap)") : QStringLiteral("ignored");
        auto *cs = new QTableWidgetItem(ap.security.toUpper() + (ap.adhoc ? QStringLiteral(" ad-hoc") : QString()));
        if (AccessPoint::insecure(ap.security)) { cs->setForeground(QColor(0xff, 0x4f, 0x4f)); cs->setToolTip(QStringLiteral("Insecure: anyone nearby can read this network's traffic or join it")); }
        m_aps->setItem(i, 4, cs);
        auto *c4 = new QTableWidgetItem(label);
        if (st == QLatin1String("home")) c4->setForeground(QColor(0xff, 0x9f, 0x43));
        else if (st != QLatin1String("used")) c4->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        m_aps->setItem(i, 5, c4);
        const ApEstimate e = m_loc->estimateFor(ap);
        const ApRecord *r = m_loc->record(ap.bssid);
        const QString where = e.kind == ApEstimate::Wigle ? QStringLiteral("WiGLE %1, %2").arg(e.lat, 0, 'f', 5).arg(e.lon, 0, 'f', 5)
                            : e.kind == ApEstimate::Trilat ? QStringLiteral("fit %1, %2 ±%3 m (%4 samples from %5 places, %6)").arg(e.lat, 0, 'f', 5).arg(e.lon, 0, 'f', 5).arg(qRound(e.radiusM)).arg(e.fit.n).arg(e.fit.vantage).arg(e.fit.quality)
                            : e.kind == ApEstimate::Peer ? QStringLiteral("from %1: %2, %3 ±%4 m").arg(r ? r->peerFrom : QString()).arg(e.lat, 0, 'f', 5).arg(e.lon, 0, 'f', 5).arg(qRound(e.radiusM))
                            : e.kind == ApEstimate::Centroid ? QStringLiteral("est. %1, %2 ±%3 m (%4 obs)").arg(e.lat, 0, 'f', 5).arg(e.lon, 0, 'f', 5).arg(qRound(e.radiusM)).arg(r ? r->obs.size() : 0)
                            : e.kind == ApEstimate::Observed ? QStringLiteral("heard here before %1, %2 ±%3 m").arg(e.lat, 0, 'f', 5).arg(e.lon, 0, 'f', 5).arg(qRound(e.radiusM))
                            : e.kind == ApEstimate::Ring ? QStringLiteral("~%1 m away, bearing unknown").arg(qRound(e.radiusM)) : QString();
        m_aps->setItem(i, 6, new QTableWidgetItem(where));
    }
}

void MainWindow::apContextMenu(const QPoint &pos)
{
    const int row = m_aps->rowAt(pos.y());
    if (row < 0) return;
    const QString bssid = m_aps->item(row, 0)->data(Qt::UserRole).toString();
    const QString ssid = m_aps->item(row, 0)->text();
    QMenu menu(this);
    AccessPoint apx; apx.bssid = bssid; apx.ssid = m_aps->item(row, 0)->text() == QLatin1String("(hidden)") ? QString() : ssid;
    const bool home = m_loc->isHome(apx);
    QAction *hm = menu.addAction(QIcon::fromTheme(QStringLiteral("go-home")), home ? QStringLiteral("Not a home network") : QStringLiteral("Mark as home network (the RV)"));
    QAction *trav = menu.addAction(QStringLiteral("Travels with me (exclude)"));
    trav->setCheckable(true); trav->setChecked(m_loc->isTravelling(bssid));
    QAction *ign = menu.addAction(QStringLiteral("Always ignore SSID \"%1\"").arg(ssid));
    QAction *cp = menu.addAction(QStringLiteral("Copy BSSID"));
    QAction *chosen = menu.exec(m_aps->viewport()->mapToGlobal(pos));
    if (chosen == hm) {
        if (home) { for (const QString &p : m_loc->homeNetworks()) { const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(p), QRegularExpression::CaseInsensitiveOption); if (re.match(bssid).hasMatch() || re.match(apx.ssid).hasMatch()) m_loc->removeHomeNetwork(p); } }
        else m_loc->addHomeNetwork(apx.ssid.isEmpty() ? bssid : apx.ssid);
        m_home->setPlainText(m_loc->homeNetworks().join('\n'));
    }
    else if (chosen == trav) m_loc->setTravelling(bssid, trav->isChecked());
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

// ── Devices: the LAN API (pairing, tokens, access log) ────────────────────────
QWidget *MainWindow::buildDevices()
{
    ApiServer *api = m_loc->apiServer();
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    auto *top = new QHBoxLayout;
    m_apiEnabled = new QCheckBox(QStringLiteral("Enable LAN API"));
    m_apiEnabled->setChecked(api->enabled());
    m_apiEnabled->setToolTip(QStringLiteral("Answer other devices on your local network (photo frame, phone, laptop). Only private-network peers are accepted, and every call except 'hello' and pairing needs a device token."));
    connect(m_apiEnabled, &QCheckBox::toggled, this, [api](bool on) { api->setEnabled(on); });
    m_apiPort = new QSpinBox; m_apiPort->setRange(1024, 65535); m_apiPort->setValue(api->port()); m_apiPort->setPrefix(QStringLiteral("port "));
    connect(m_apiPort, &QSpinBox::editingFinished, this, [this, api] { api->setPort(m_apiPort->value()); });
    m_apiStatus = new QLabel; m_apiStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(m_apiEnabled); top->addWidget(m_apiPort); top->addWidget(m_apiStatus, 1);
    v->addLayout(top);

    auto *pairRow = new QHBoxLayout;
    m_pairBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add-user")), QStringLiteral("Allow pairing for 10 minutes"));
    m_pairBtn->setToolTip(QStringLiteral("While pairing is open, a device can POST /api/v1/pair. Each request shows up below with a 4-digit code that the device also displays — approve the one whose code matches."));
    connect(m_pairBtn, &QPushButton::clicked, this, [api] { if (api->pairingOpen()) api->closePairing(); else api->openPairing(10); });
    m_pairLabel = new QLabel;
    auto *tokenBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-password")), QStringLiteral("Create token…"));
    tokenBtn->setToolTip(QStringLiteral("For devices that cannot pair on their own (scripts, curl): make a token now and paste it into them."));
    connect(tokenBtn, &QPushButton::clicked, this, [this, api] {
        QDialog dlg(this); dlg.setWindowTitle(QStringLiteral("Create a device token"));
        auto *form = new QFormLayout(&dlg);
        auto *name = new QLineEdit; name->setPlaceholderText(QStringLiteral("e.g. Frameo kitchen"));
        auto *ctrl = new QCheckBox(QStringLiteral("Also allow control (re-check now, save map offline)"));
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(QStringLiteral("Device name:"), name); form->addRow(QString(), ctrl); form->addRow(bb);
        connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept); connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;
        const QString tok = api->createToken(name->text(), ctrl->isChecked() ? QStringList{QStringLiteral("read"), QStringLiteral("control")} : QStringList{QStringLiteral("read")});
        QDialog show(this); show.setWindowTitle(QStringLiteral("Token for %1").arg(name->text().isEmpty() ? QStringLiteral("Device") : name->text()));
        auto *sv = new QVBoxLayout(&show);
        sv->addWidget(new QLabel(QStringLiteral("This token is shown once. Put it in the device now:")));
        auto *edit = new QLineEdit(tok); edit->setReadOnly(true); edit->setMinimumWidth(460); sv->addWidget(edit);
        sv->addWidget(new QLabel(QStringLiteral("Use it as:  Authorization: Bearer <token>")));
        auto *sb = new QDialogButtonBox(QDialogButtonBox::Close);
        auto *cp = sb->addButton(QStringLiteral("Copy"), QDialogButtonBox::ActionRole);
        connect(cp, &QPushButton::clicked, &show, [tok, this] { QApplication::clipboard()->setText(tok); statusBar()->showMessage(QStringLiteral("Token copied"), 2000); });
        connect(sb, &QDialogButtonBox::rejected, &show, &QDialog::reject);
        sv->addWidget(sb);
        show.exec();
    });
    pairRow->addWidget(m_pairBtn); pairRow->addWidget(m_pairLabel, 1); pairRow->addWidget(tokenBtn);
    v->addLayout(pairRow);

    v->addWidget(new QLabel(QStringLiteral("<b>Pending requests</b> — the device shows the same code; approve only a code you recognise")));
    m_pendingTable = new QTableWidget(0, 6);
    m_pendingTable->setHorizontalHeaderLabels({QStringLiteral("Device"), QStringLiteral("Address"), QStringLiteral("Code"), QStringLiteral("Asks for"), QStringLiteral("Requested"), QString()});
    m_pendingTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_pendingTable->verticalHeader()->hide(); m_pendingTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_pendingTable->setMaximumHeight(150);
    v->addWidget(m_pendingTable);

    v->addWidget(new QLabel(QStringLiteral("<b>Paired devices</b>")));
    m_devTable = new QTableWidget(0, 6);
    m_devTable->setHorizontalHeaderLabels({QStringLiteral("Device"), QStringLiteral("Scopes"), QStringLiteral("Created"), QStringLiteral("Last seen"), QStringLiteral("From"), QString()});
    m_devTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_devTable->verticalHeader()->hide(); m_devTable->setSelectionMode(QAbstractItemView::NoSelection);
    v->addWidget(m_devTable, 1);

    auto *knownHead = new QHBoxLayout;
    knownHead->addWidget(new QLabel(QStringLiteral("<b>Known devices (ours)</b> — from the UniFi export; pairing from these is approved automatically")));
    m_knownOnly = new QCheckBox(QStringLiteral("Only known devices may use tokens"));
    m_knownOnly->setChecked(api->knownOnly());
    m_knownOnly->setToolTip(QStringLiteral("Peers are matched by MAC (kernel neighbour table) or by their known / fixed IP. Unknown peers can still ask to pair while pairing is open — you approve them by hand."));
    connect(m_knownOnly, &QCheckBox::toggled, this, [api](bool on) { api->setKnownOnly(on); });
    knownHead->addStretch(); knownHead->addWidget(m_knownOnly);
    v->addLayout(knownHead);
    m_knownTable = new QTableWidget(0, 5);
    m_knownTable->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("MAC"), QStringLiteral("Address"), QStringLiteral("Status"), QStringLiteral("Pairs as")});
    m_knownTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_knownTable->verticalHeader()->hide(); m_knownTable->setSelectionBehavior(QAbstractItemView::SelectRows); m_knownTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_knownTable->setMaximumHeight(170); m_knownTable->setSortingEnabled(true);
    m_knownTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_knownTable, &QTableWidget::customContextMenuRequested, this, [this, api](const QPoint &pos) {
        QMenu menu(this);
        const int row = m_knownTable->rowAt(pos.y());
        QAction *add = menu.addAction(QIcon::fromTheme(QStringLiteral("list-add")), QStringLiteral("Add a device…"));
        QAction *rm = row >= 0 ? menu.addAction(QIcon::fromTheme(QStringLiteral("list-remove")), QStringLiteral("Remove %1").arg(m_knownTable->item(row, 0)->text())) : nullptr;
        QAction *imp = menu.addAction(QIcon::fromTheme(QStringLiteral("document-import")), QStringLiteral("Import known-devices.json…"));
        QAction *ch = menu.exec(m_knownTable->viewport()->mapToGlobal(pos));
        if (ch == add) {
            const QString mac = QInputDialog::getText(this, QStringLiteral("Add known device"), QStringLiteral("MAC address (aa:bb:cc:dd:ee:ff, ? and * allowed):"));
            if (mac.trimmed().isEmpty()) return;
            const QString name = QInputDialog::getText(this, QStringLiteral("Add known device"), QStringLiteral("Name:"));
            if (!api->knownAdd(mac, name)) QMessageBox::warning(this, QStringLiteral("Known devices"), QStringLiteral("That is not a MAC address."));
        } else if (rm && ch == rm) api->knownRemove(m_knownTable->item(row, 1)->text());
        else if (ch == imp) {
            const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import known devices"), QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/sworrl"), QStringLiteral("JSON (*.json)"));
            if (path.isEmpty()) return;
            QString err; const int n = api->knownImport(path, &err);
            if (n < 0) QMessageBox::warning(this, QStringLiteral("Import failed"), err); else statusBar()->showMessage(QStringLiteral("%1 new device(s) imported").arg(n), 4000);
        }
    });
    v->addWidget(m_knownTable);

    v->addWidget(new QLabel(QStringLiteral("<b>Access log</b> (last 100)")));
    m_accessLog = new QPlainTextEdit; m_accessLog->setReadOnly(true); m_accessLog->setMaximumBlockCount(100); m_accessLog->setMaximumHeight(140);
    { QFont mono = m_accessLog->font(); mono.setFamily(QStringLiteral("monospace")); mono.setStyleHint(QFont::Monospace); m_accessLog->setFont(mono); }
    v->addWidget(m_accessLog);

    connect(api, &ApiServer::changed, this, &MainWindow::refreshDevices);
    connect(api, &ApiServer::accessLogged, this, [this, api] {
        const ApiServer::AccessEntry e = api->accessLog().last();
        m_accessLog->appendPlainText(QStringLiteral("%1  %2  %3 %4 → %5").arg(e.time.toString(QStringLiteral("HH:mm:ss")), e.ip.leftJustified(15), e.method.leftJustified(4), e.path).arg(e.status));
    });
    m_devTimer = new QTimer(this); m_devTimer->setInterval(1000);
    connect(m_devTimer, &QTimer::timeout, this, [this, api] {
        if (api->pairingOpen()) {
            const qint64 left = QDateTime::currentDateTime().secsTo(api->pairingUntil());
            m_pairLabel->setText(QStringLiteral("Pairing OPEN — %1:%2 left. A device can pair now.").arg(left / 60).arg(left % 60, 2, 10, QLatin1Char('0')));
        }
    });
    m_devTimer->start();
    for (const ApiServer::AccessEntry &e : api->accessLog())
        m_accessLog->appendPlainText(QStringLiteral("%1  %2  %3 %4 → %5").arg(e.time.toString(QStringLiteral("HH:mm:ss")), e.ip.leftJustified(15), e.method.leftJustified(4), e.path).arg(e.status));
    refreshDevices();
    return w;
}

void MainWindow::refreshDevices()
{
    ApiServer *api = m_loc->apiServer();
    if (!api) return;
    const QJsonObject st = api->statusJson();
    QStringList addrs; for (const QJsonValue &a : st["addresses"].toArray()) addrs << QStringLiteral("%1:%2").arg(a.toString()).arg(api->boundPort());
    m_apiStatus->setText(api->listening()
        ? QStringLiteral("%1 http%2://%3  ·  devices discover it as _beaconfix._tcp").arg(api->tls() ? QStringLiteral("TLS") : QStringLiteral("HTTP"), api->tls() ? QStringLiteral("s") : QString(), addrs.isEmpty() ? QStringLiteral("<no LAN address>") : addrs.join(QStringLiteral("  ")))
        : (api->enabled() ? QStringLiteral("Not listening: %1").arg(api->error()) : QStringLiteral("Disabled")));
    m_apiEnabled->setChecked(api->enabled());
    m_pairBtn->setText(api->pairingOpen() ? QStringLiteral("Close pairing") : QStringLiteral("Allow pairing for 10 minutes"));
    m_pairBtn->setEnabled(api->listening());
    if (!api->pairingOpen()) m_pairLabel->setText(QStringLiteral("Pairing closed — devices cannot ask for access until you open it."));

    const QList<ApiServer::Pending> pend = api->pending();
    m_pendingTable->setRowCount(pend.size());
    for (int i = 0; i < pend.size(); ++i) {
        const ApiServer::Pending &p = pend[i];
        m_pendingTable->setItem(i, 0, new QTableWidgetItem(p.name));
        m_pendingTable->setItem(i, 1, new QTableWidgetItem(p.ip));
        auto *code = new QTableWidgetItem(p.code); { QFont b = code->font(); b.setBold(true); b.setPointSizeF(b.pointSizeF() * 1.2); code->setFont(b); }
        m_pendingTable->setItem(i, 2, code);
        m_pendingTable->setItem(i, 3, new QTableWidgetItem(p.scopes.join(QStringLiteral(", "))));
        m_pendingTable->setItem(i, 4, new QTableWidgetItem(p.state == ApiServer::Pending::Waiting ? p.created.toString(QStringLiteral("HH:mm:ss")) : p.state == ApiServer::Pending::Approved ? QStringLiteral("approved — waiting for the device to fetch its token") : QStringLiteral("denied")));
        auto *cell = new QWidget; auto *h = new QHBoxLayout(cell); h->setContentsMargins(2, 0, 2, 0);
        if (p.state == ApiServer::Pending::Waiting) {
            auto *ok = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok")), QStringLiteral("Approve"));
            auto *no = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), QStringLiteral("Deny"));
            const QString id = p.id;
            connect(ok, &QPushButton::clicked, this, [api, id] { api->approve(id); });
            connect(no, &QPushButton::clicked, this, [api, id] { api->deny(id); });
            h->addWidget(ok); h->addWidget(no);
        }
        m_pendingTable->setCellWidget(i, 5, cell);
    }
    m_pendingTable->resizeColumnsToContents(); m_pendingTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);

    const QList<ApiServer::Device> devs = api->devices();
    m_devTable->setRowCount(devs.size());
    for (int i = 0; i < devs.size(); ++i) {
        const ApiServer::Device &d = devs[i];
        auto *name = new QTableWidgetItem(d.name + (d.revoked ? QStringLiteral("  (revoked)") : QString()));
        if (d.revoked) name->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        m_devTable->setItem(i, 0, name);
        m_devTable->setItem(i, 1, new QTableWidgetItem(d.scopes.join(QStringLiteral(", "))));
        m_devTable->setItem(i, 2, new QTableWidgetItem(d.created.toString(QStringLiteral("d MMM HH:mm"))));
        m_devTable->setItem(i, 3, new QTableWidgetItem(d.lastSeen.isValid() ? d.lastSeen.toString(QStringLiteral("d MMM HH:mm")) : QStringLiteral("never")));
        m_devTable->setItem(i, 4, new QTableWidgetItem(d.lastIp));
        auto *cell = new QWidget; auto *h = new QHBoxLayout(cell); h->setContentsMargins(2, 0, 2, 0);
        const QString id = d.id;
        if (!d.revoked) { auto *rv = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), QStringLiteral("Revoke")); connect(rv, &QPushButton::clicked, this, [api, id] { api->revoke(id); }); h->addWidget(rv); }
        else { auto *rm = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-clear")), QStringLiteral("Remove")); connect(rm, &QPushButton::clicked, this, [api, id] { api->remove(id); }); h->addWidget(rm); }
        m_devTable->setCellWidget(i, 5, cell);
    }
    m_devTable->resizeColumnsToContents(); m_devTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);

    m_knownOnly->setChecked(api->knownOnly());
    const QList<ApiServer::Known> kn = api->known();
    m_knownTable->setSortingEnabled(false);
    m_knownTable->setRowCount(kn.size());
    for (int i = 0; i < kn.size(); ++i) {
        const ApiServer::Known &k = kn[i];
        m_knownTable->setItem(i, 0, new QTableWidgetItem(k.name + (k.hostname.isEmpty() || k.hostname == k.name ? QString() : QStringLiteral("  (%1)").arg(k.hostname))));
        m_knownTable->setItem(i, 1, new QTableWidgetItem(k.mac));
        m_knownTable->setItem(i, 2, new QTableWidgetItem(k.fixedIp.isEmpty() ? k.ip : k.fixedIp + (k.ip.isEmpty() || k.ip == k.fixedIp ? QString() : QStringLiteral(" (now %1)").arg(k.ip))));
        auto *stI = new QTableWidgetItem(k.online ? QStringLiteral("online") : k.lastSeen.isValid() ? QStringLiteral("seen %1").arg(k.lastSeen.toString(QStringLiteral("d MMM yyyy"))) : QStringLiteral("—"));
        if (k.online) stI->setForeground(QColor(0x6c, 0xff, 0x8a));
        m_knownTable->setItem(i, 3, stI);
        m_knownTable->setItem(i, 4, new QTableWidgetItem(k.scopes.isEmpty() ? QStringLiteral("read") : k.scopes.join(QStringLiteral(", "))));
    }
    m_knownTable->setSortingEnabled(true);
    m_knownTable->resizeColumnsToContents(); m_knownTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
}
