#include "mainwindow.h"
#include "anchordialog.h"
#include <QTreeWidget>
#include "locator.h"
#include "beaconview.h"
#include "apiserver.h"
#include "mdns.h"
#include "pairdialog.h"
#include "pairing.h"
#include <QComboBox>
#include "mapdb.h"
#include "identity.h"
#include "osintegration.h"
#include <QProcess>
#include <QEventLoop>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QPixmap>
#include <QHostInfo>
#include <QJsonDocument>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QLocale>
#include <QProgressBar>
#include <QDateEdit>
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

static QPixmap qrPixmap(const QString &text, int scale = 5);   // qrencode → pixmap (defined with the identity tab)

MainWindow::MainWindow(Locator *loc, TileSource *tiles, QWidget *parent) : QMainWindow(parent), m_loc(loc), m_tiles(tiles)
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
    reload->setToolTip(QStringLiteral("Ask OpenStreetMap again for the places around here and the pediatric ERs"));
    connect(reload, &QPushButton::clicked, m_loc, [this] { m_loc->RefreshPlaces(); });
    m_poiNote = new QLabel;
    nbar->addWidget(m_poiFilter, 1); nbar->addWidget(reload);
    nl->addLayout(nbar); nl->addWidget(m_poiNote);
    m_emergency = new QLabel; m_emergency->setWordWrap(true); m_emergency->setTextInteractionFlags(Qt::TextBrowserInteraction); m_emergency->setOpenExternalLinks(true);
    m_emergency->setStyleSheet(QStringLiteral("QLabel { background: rgba(255,77,77,0.10); border: 1px solid rgba(255,77,77,0.45); border-radius: 6px; padding: 6px; }"));
    nl->addWidget(m_emergency);
    m_pois = new QTableWidget(0, 7);
    m_pois->setHorizontalHeaderLabels({QStringLiteral("Place"), QStringLiteral("Kind"), QStringLiteral("Distance"), QStringLiteral("Details"), QStringLiteral("Phone"), QStringLiteral("Address"), QStringLiteral("Hours")});
    m_pois->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_pois->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_pois->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pois, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int row = m_pois->rowAt(pos.y()); if (row < 0) return;
        const int idx = m_pois->item(row, 0)->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= m_loc->pois().size()) return;
        const Poi pt = m_loc->pois()[idx];
        QMenu menu(this);
        QAction *call = pt.phone.isEmpty() ? nullptr : menu.addAction(QIcon::fromTheme(QStringLiteral("call-start")), QStringLiteral("Call %1").arg(pt.phone));
        QAction *cpPhone = pt.phone.isEmpty() ? nullptr : menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy phone number"));
        QAction *cpAddr = pt.address.isEmpty() ? nullptr : menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy address"));
        QAction *web = pt.website.isEmpty() ? nullptr : menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("Open website"));
        QAction *dir = menu.addAction(QIcon::fromTheme(QStringLiteral("go-next")), QStringLiteral("Directions (OpenStreetMap)"));
        QAction *osm = menu.addAction(QIcon::fromTheme(QStringLiteral("map-globe")), QStringLiteral("Open on OpenStreetMap"));
        QAction *ch = menu.exec(m_pois->viewport()->mapToGlobal(pos));
        if (!ch) return;
        if (ch == call) QDesktopServices::openUrl(QUrl(QStringLiteral("tel:") + QString(pt.phone).remove(QLatin1Char(' '))));
        else if (ch == cpPhone) QApplication::clipboard()->setText(pt.phone);
        else if (ch == cpAddr) QApplication::clipboard()->setText(pt.address);
        else if (ch == web) QDesktopServices::openUrl(QUrl(pt.website));
        else if (ch == dir && m_loc->fix().valid) QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.openstreetmap.org/directions?engine=fossgis_osrm_car&route=%1,%2;%3,%4").arg(m_loc->fix().lat).arg(m_loc->fix().lon).arg(pt.lat).arg(pt.lon)));
        else if (ch == osm) QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.openstreetmap.org/%1/%2").arg(pt.osmType).arg(pt.osmId)));
    });
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
    m_identityTab = buildIdentity();
    tabs->addTab(m_identityTab, QIcon::fromTheme(QStringLiteral("user-identity")), QStringLiteral("Identity"));
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
        imp->setToolTip(QStringLiteral("Import your location history: Google Timeline.json / Records.json / Semantic Location History, WiGLE CSV, GPX, KML or a BeaconFix export (docs/DATABASE.md)."));
        connect(imp, &QPushButton::clicked, this, [this] { importHistoryDialog(); });
        connect(rebuild, &QPushButton::clicked, this, [this] {
            const int n = m_loc->rebuildDbFromJson();
            statusBar()->showMessage(n > 0 ? QStringLiteral("Rebuilt from %1 JSON file(s)").arg(n) : n == 0 ? QStringLiteral("No *.migrated JSON files to rebuild from") : QStringLiteral("Database is not writable"), 5000);
        });
        if (MapDb *db = m_loc->mapDb()) {                   // stats() counts whole tables: coalesce bursts of changed() (imports, syncs, scans)
            auto *statsTimer = new QTimer(this); statsTimer->setSingleShot(true); statsTimer->setInterval(1500);
            connect(statsTimer, &QTimer::timeout, this, refreshDb);
            connect(db, &MapDb::changed, statsTimer, [statsTimer] { statsTimer->start(); });
        }
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

    // ── System: keep the desktop in step with where we are ──
    {
        OsIntegration *os = m_loc->os();
        auto *box = new QWidget; auto *col = new QVBoxLayout(box); col->setContentsMargins(0, 0, 0, 0);
        m_osTz = new QCheckBox(QStringLiteral("Keep the system time zone in step with the fix (timedated; the shipped polkit rule makes it prompt-free for admins)"));
        m_osTz->setChecked(os->timeZoneEnabled()); connect(m_osTz, &QCheckBox::toggled, os, &OsIntegration::setTimeZoneEnabled);
        m_osGeo = new QCheckBox(QStringLiteral("Publish the position to GeoClue (/etc/geolocation) so location-aware apps, browsers and Night Light get it"));
        m_osGeo->setChecked(os->geoclueEnabled()); connect(m_osGeo, &QCheckBox::toggled, os, &OsIntegration::setGeoclueEnabled);
        m_osNight = new QCheckBox(QStringLiteral("Point KWin Night Light at the fix (sunset colour follows where you are)"));
        m_osNight->setChecked(os->nightLightEnabled()); connect(m_osNight, &QCheckBox::toggled, os, &OsIntegration::setNightLightEnabled);
        m_osLocale = new QCheckBox(QStringLiteral("Expose locale hints (units, emergency number, dialling code) — suggestion only, never changes the system locale"));
        m_osLocale->setChecked(os->localeEnabled()); connect(m_osLocale, &QCheckBox::toggled, os, &OsIntegration::setLocaleEnabled);
        m_osStatus = new QLabel; m_osStatus->setWordWrap(true); m_osStatus->setTextInteractionFlags(Qt::TextSelectableByMouse); m_osStatus->setStyleSheet(QStringLiteral("color: palette(mid)"));
        auto *row = new QHBoxLayout;
        auto *applyBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("system-run")), QStringLiteral("Apply now"));
        auto *dryBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-information")), QStringLiteral("What would change?"));
        connect(applyBtn, &QPushButton::clicked, this, [this, os] { const QJsonObject r = os->apply(false, true); QMessageBox::information(this, QStringLiteral("OS integration"), QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Indented))); });
        connect(dryBtn, &QPushButton::clicked, this, [this, os] { const QJsonObject r = os->apply(true, true); QMessageBox::information(this, QStringLiteral("OS integration (dry run)"), QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Indented))); });
        row->addWidget(applyBtn); row->addWidget(dryBtn); row->addStretch();
        col->addWidget(m_osTz); col->addWidget(m_osGeo); col->addWidget(m_osNight); col->addWidget(m_osLocale); col->addLayout(row); col->addWidget(m_osStatus);
        auto refreshOs = [this, os] {
            const OsIntegration::LocaleHints h = os->locale();
            m_osStatus->setText(QStringLiteral("System zone: %1 · zone for the fix: %2 (%3) · helper: %4\nLocale hints: %5%6 · units %7 · emergency %8 · dialling %9")
                .arg(os->systemTimeZone(), os->lastZone().isEmpty() ? QStringLiteral("not resolved yet") : os->lastZone(), os->lastZoneSource().isEmpty() ? QStringLiteral("—") : os->lastZoneSource(),
                     OsIntegration::helperPath().isEmpty() ? QStringLiteral("beaconfix-osd NOT installed (GeoClue publishing off) — install.sh installs it with polkit") : OsIntegration::helperPath(),
                     h.country.isEmpty() ? QStringLiteral("unknown country") : h.country, h.countryCode.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(h.countryCode.toUpper()), h.units, h.emergency, h.dialing.isEmpty() ? QStringLiteral("?") : h.dialing));
        };
        connect(os, &OsIntegration::changed, this, refreshOs); refreshOs();
        form->addRow(QStringLiteral("System:"), box);
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

    // Anchors (docs/RANGING.md §4): the surveyed antennas / places, with edit + remove; new ones come from the map
    auto *anchorList = new QTreeWidget;
    anchorList->setHeaderLabels({QStringLiteral("Anchor"), QStringLiteral("Kind"), QStringLiteral("Where"), QStringLiteral("±"), QStringLiteral("RV"), QStringLiteral("Placed")});
    anchorList->setRootIsDecorated(false); anchorList->setMaximumHeight(150); anchorList->setAlternatingRowColors(true);
    anchorList->setToolTip(QStringLiteral("Right-click the map → “Place an antenna here…” to add one; drag a diamond on the map to move it."));
    auto fillAnchors = [this, anchorList] {
        anchorList->clear();
        for (const BfAnchor &a : m_loc->anchors()) {
            auto *it = new QTreeWidgetItem({a.name.isEmpty() ? QStringLiteral("(unnamed)") : a.name, a.kind, QStringLiteral("%1, %2").arg(a.lat, 0, 'f', 6).arg(a.lon, 0, 'f', 6),
                                            QStringLiteral("%1 m").arg(a.accM, 0, 'g', 2), a.rv ? (a.ref ? QStringLiteral("ref") : a.headingAssumed ? QStringLiteral("yes ?") : QStringLiteral("yes")) : QString(),
                                            QStringLiteral("%1 · %2").arg(a.placedBy, a.placedAt.left(16).replace(QLatin1Char('T'), QLatin1Char(' ')))});
            it->setData(0, Qt::UserRole, a.id);
            if (!a.bssids.isEmpty()) it->setToolTip(0, a.bssids.join(QStringLiteral(", ")));
            anchorList->addTopLevelItem(it);
        }
        for (int c = 0; c < anchorList->columnCount(); ++c) anchorList->resizeColumnToContents(c);
    };
    fillAnchors();
    connect(m_loc, &Locator::scanUpdated, anchorList, fillAnchors);
    auto *anchorBtns = new QVBoxLayout;
    auto *anchorEdit = new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), QStringLiteral("Edit…"));
    auto *anchorDel = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), QStringLiteral("Remove"));
    auto *anchorHere = new QPushButton(QIcon::fromTheme(QStringLiteral("mark-location")), QStringLiteral("At my position…"));
    anchorHere->setToolTip(QStringLiteral("Place an anchor at the current fix (then drag it on the map to the exact spot)."));
    auto selectedIndex = [this, anchorList]() -> int {
        const QTreeWidgetItem *it = anchorList->currentItem(); if (!it) return -1;
        const QList<BfAnchor> all = m_loc->anchors();
        for (int i = 0; i < all.size(); ++i) if (all[i].id == it->data(0, Qt::UserRole).toString()) return i;
        return -1;
    };
    auto editSelected = [this, selectedIndex, fillAnchors] {
        const int i = selectedIndex(); if (i < 0) return;
        const BfAnchor a = m_loc->anchors()[i];
        AnchorDialog d(m_loc, a.lat, a.lon, a.toJson(false), this);
        if (d.exec() != QDialog::Accepted) return;
        bool ok = false; QString err; m_loc->setAnchor(d.anchor(), QStringLiteral("desktop"), &ok, &err);
        if (!ok) QMessageBox::warning(this, QStringLiteral("Anchor"), err);
        fillAnchors();
    };
    connect(anchorEdit, &QPushButton::clicked, this, editSelected);
    connect(anchorList, &QTreeWidget::itemDoubleClicked, this, editSelected);
    connect(anchorDel, &QPushButton::clicked, this, [this, selectedIndex, fillAnchors] {
        const int i = selectedIndex(); if (i < 0) return;
        const BfAnchor a = m_loc->anchors()[i];
        if (QMessageBox::question(this, QStringLiteral("Remove anchor"), QStringLiteral("Remove “%1”? Every synced device drops it too.").arg(a.name.isEmpty() ? a.kind : a.name)) != QMessageBox::Yes) return;
        m_loc->removeAnchor(a.id); fillAnchors();
    });
    connect(anchorHere, &QPushButton::clicked, this, [this, fillAnchors] {
        if (!m_loc->fix().valid) { QMessageBox::information(this, QStringLiteral("Anchor"), QStringLiteral("No fix yet.")); return; }
        AnchorDialog d(m_loc, m_loc->fix().lat, m_loc->fix().lon, QJsonObject(), this);
        if (d.exec() != QDialog::Accepted) return;
        bool ok = false; QString err; m_loc->setAnchor(d.anchor(), QStringLiteral("desktop"), &ok, &err);
        if (!ok) QMessageBox::warning(this, QStringLiteral("Anchor"), err);
        fillAnchors();
    });
    anchorBtns->addWidget(anchorHere); anchorBtns->addWidget(anchorEdit); anchorBtns->addWidget(anchorDel); anchorBtns->addStretch(1);
    auto *anchorRow = new QHBoxLayout; auto *anchorBox = new QWidget; anchorBox->setLayout(anchorRow); anchorRow->setContentsMargins(0, 0, 0, 0);
    anchorRow->addWidget(anchorList, 1); anchorRow->addLayout(anchorBtns);
    form->addRow(QStringLiteral("Anchors:"), anchorBox);

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

// "~25 min (est.)", "~1 h 20 min (est.)"
static QString driveText(int secs, bool estimate)
{
    const int min = qMax(1, secs / 60);
    const QString t = min < 60 ? QStringLiteral("~%1 min").arg(min)
                    : min % 60 ? QStringLiteral("~%1 h %2 min").arg(min / 60).arg(min % 60) : QStringLiteral("~%1 h").arg(min / 60);
    return estimate ? t + QStringLiteral(" (est.)") : t;
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
        m_pois->setItem(row, 3, new QTableWidgetItem(pt.detail));
        m_pois->setItem(row, 4, new QTableWidgetItem(pt.phone));
        m_pois->setItem(row, 5, new QTableWidgetItem(pt.address));
        m_pois->setItem(row, 6, new QTableWidgetItem(pt.hours));
    }
    m_pois->setSortingEnabled(true);
    // Emergency strip: nearest help + the local number
    {
        const QJsonObject e = m_loc->emergencyJson();
        auto one = [&](const char *key, const QString &label) -> QString {
            const QJsonValue v = e[key]; if (v.isNull() || v.isUndefined()) return QString();
            const QJsonObject q = v.toObject();
            QString t = QStringLiteral("<b>%1:</b> %2").arg(label, q["name"].toString().toHtmlEscaped());
            if (q.contains("tier")) {                       // pediatric: always say how sure we are
                const int tier = q["tier"].toInt();
                const QString campus = q["campusEr"].toString();
                const QString conf = tier == 1 ? QStringLiteral("pediatric ER")
                                   : tier == 2 ? (campus.isEmpty() ? QStringLiteral("ER not confirmed — call ahead") : QStringLiteral("ER on campus: %1 — call ahead").arg(campus))
                                   : tier == 3 ? QStringLiteral("general ER · pediatrics dept.")
                                   : tier == 4 ? QStringLiteral("not an ER") : QString();
                if (!conf.isEmpty()) t += QStringLiteral(" <i>(%1)</i>").arg(conf.toHtmlEscaped());
            }
            if (q.contains("d")) t += QStringLiteral(" · %1 km %2").arg(q["d"].toDouble() / 1000.0, 0, 'f', 1).arg(Locator::compass(q["brg"].toDouble()));
            if (q["driveS"].toInt() > 0) t += QStringLiteral(" · %1").arg(driveText(q["driveS"].toInt(), q["driveEst"].toBool(true)));
            if (!q["phone"].toString().isEmpty()) t += QStringLiteral(" · <a href=\"tel:%1\">%2</a>").arg(QString(q["phone"].toString()).remove(QLatin1Char(' ')), q["phone"].toString().toHtmlEscaped());
            if (!q["address"].toString().isEmpty()) t += QStringLiteral(" · %1").arg(q["address"].toString().toHtmlEscaped());
            return t;
        };
        QStringList parts{QStringLiteral("<b>🚨 Emergency number here: %1</b>").arg(e["number"].toString())};
        // The general ER stays above the pediatric one: never hide the nearest ER behind a children's hospital
        for (const auto &k : QList<QPair<const char *, QString>>{{"police", QStringLiteral("🚔 Police")}, {"fire", QStringLiteral("🚒 Fire")}, {"hospital", QStringLiteral("🏥 ER / hospital")},
                                                                 {"pediatric", QStringLiteral("🧸 Pediatric ER")}, {"pediatricCloser", QStringLiteral("Closer")},
                                                                 {"pediatricUrgent", QStringLiteral("🩹 Pediatric urgent care (not an ER)")},
                                                                 {"urgent", QStringLiteral("🩺 Urgent care")}, {"pharmacy", QStringLiteral("💊 Pharmacy")}})
            { const QString t = one(k.first, k.second); if (!t.isEmpty()) parts << t; }
        if (!e["pediatricNote"].toString().isEmpty()) parts << QStringLiteral("<i>🧸 %1</i>").arg(e["pediatricNote"].toString().toHtmlEscaped());
        m_emergency->setText(parts.join(QStringLiteral("<br>")));
    }
    m_pois->sortItems(2);
    int nearCount = 0; for (const Poi &pt : pois) if (pt.scope != QLatin1String("far")) ++nearCount;
    QString note = m_loc->poisLoading() ? m_loc->poiNote()
                 : QStringLiteral("%1 places within %2 km from OpenStreetMap").arg(nearCount).arg(m_loc->poiRadiusKm());
    if (!m_loc->poisLoading() && !m_loc->poiNote().isEmpty()) note += QStringLiteral(" — ") + m_loc->poiNote();
    if (m_loc->pedsLoading()) note += QStringLiteral(" · looking for pediatric ERs within %1 km…").arg(m_loc->pedsRadiusKm());
    else if (pois.size() > nearCount) note += QStringLiteral(" · %1 pediatric / ER places out to %2 km").arg(pois.size() - nearCount).arg(m_loc->pedsRadiusKm());
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
    m_pairPolicy = new QComboBox;
    m_pairPolicy->addItem(QStringLiteral("Proximity required (only adjacent / nearby devices can pair)"), QStringLiteral("required"));
    m_pairPolicy->addItem(QStringLiteral("Proximity warns only"), QStringLiteral("warn"));
    m_pairPolicy->addItem(QStringLiteral("Proximity off"), QStringLiteral("off"));
    m_pairPolicy->setCurrentIndex(qMax(0, m_pairPolicy->findData(api->pairPolicy())));
    m_pairPolicy->setToolTip(QStringLiteral("A device asking to pair sends the Wi-Fi beacons it hears and its position. \"Adjacent\" = it hears mostly the same beacons at similar levels; \"near\" = a few shared beacons or within 150 m; \"far\" = nothing in common. With \"required\", far/unknown devices need the explicit override in the dialog."));
    connect(m_pairPolicy, &QComboBox::currentIndexChanged, this, [this, api](int i) { api->setPairPolicy(m_pairPolicy->itemData(i).toString()); });
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
    pairRow->addWidget(m_pairPolicy);
    v->addLayout(pairRow);

    v->addWidget(new QLabel(QStringLiteral("<b>Pending requests</b> — the device shows the same code; approve only a code you recognise")));
    m_pendingTable = new QTableWidget(0, 6);
    m_pendingTable->setHorizontalHeaderLabels({QStringLiteral("Device"), QStringLiteral("Address"), QStringLiteral("Code"), QStringLiteral("Asks for"), QStringLiteral("Requested"), QString()});
    m_pendingTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_pendingTable->verticalHeader()->hide(); m_pendingTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_pendingTable->setMaximumHeight(150);
    v->addWidget(m_pendingTable);

    v->addWidget(new QLabel(QStringLiteral("<b>Identity sign-ins to link</b> — devices that signed in with an identity that is not yours (yet). Link only identities you own.")));
    m_linkTable = new QTableWidget(0, 5);
    m_linkTable->setHorizontalHeaderLabels({QStringLiteral("Identity"), QStringLiteral("Device"), QStringLiteral("Address"), QStringLiteral("When"), QString()});
    m_linkTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_linkTable->verticalHeader()->hide(); m_linkTable->setSelectionMode(QAbstractItemView::NoSelection); m_linkTable->setMaximumHeight(120);
    v->addWidget(m_linkTable);
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
            const QString id = p.id;
            auto *pics = new QPushButton(QIcon::fromTheme(QStringLiteral("view-preview")), QStringLiteral("Pictures…"));
            pics->setToolTip(QStringLiteral("Match the three pictures the device shows (with its position and the beacons it hears)"));
            connect(pics, &QPushButton::clicked, this, [this, id] { showPairRequest(id); });
            auto *ok = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok")), QStringLiteral("Approve by code"));
            ok->setEnabled(p.lifted || Pairing::verdictAllowed(p.proximity["verdict"].toString(), api->pairPolicy()));
            auto *no = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), QStringLiteral("Deny"));
            connect(ok, &QPushButton::clicked, this, [api, id] { api->approve(id); });
            connect(no, &QPushButton::clicked, this, [api, id] { api->deny(id); });
            h->addWidget(pics); h->addWidget(ok); h->addWidget(no);
        }
        m_pendingTable->setCellWidget(i, 5, cell);
    }
    m_pendingTable->resizeColumnsToContents(); m_pendingTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);

    if (m_linkTable && m_loc->identity()) {
        const QList<PendingLink> pl = m_loc->identity()->pending();
        m_linkTable->setRowCount(pl.size());
        for (int i = 0; i < pl.size(); ++i) {
            const PendingLink &p = pl[i];
            m_linkTable->setItem(i, 0, new QTableWidgetItem(QStringLiteral("%1  %2").arg(p.name.isEmpty() ? QStringLiteral("(unnamed)") : p.name, Identity::groupId(p.id))));
            m_linkTable->setItem(i, 1, new QTableWidgetItem(QStringLiteral("%1 (%2)").arg(p.deviceName, p.deviceKind)));
            m_linkTable->setItem(i, 2, new QTableWidgetItem(p.ip));
            m_linkTable->setItem(i, 3, new QTableWidgetItem(p.time.toString(QStringLiteral("d MMM HH:mm"))));
            auto *cell = new QWidget; auto *h = new QHBoxLayout(cell); h->setContentsMargins(2, 0, 2, 0);
            auto *link = new QPushButton(QIcon::fromTheme(QStringLiteral("insert-link")), QStringLiteral("Link this identity"));
            auto *drop = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), QStringLiteral("Dismiss"));
            const QString id = p.id; const QByteArray pub = p.pub; const QString devName = p.deviceName;
            connect(link, &QPushButton::clicked, this, [this, id, devName] {
                Identity *idn = m_loc->identity();
                if (!idn->unlocked()) { QMessageBox::warning(this, QStringLiteral("Identity"), QStringLiteral("This identity is locked (map-database key missing)")); return; }
                // A device co-signs only a statement bound to a link QR it displayed itself, so the link starts on ITS
                // screen: it scans our offer (valid 10 min), signs, and POSTs the statement to /api/v1/identity/link.
                const QString payload = Identity::encodeUri(QStringLiteral("link"), idn->linkPayload());
                QDialog dlg(this); dlg.setWindowTitle(QStringLiteral("Link with %1").arg(Identity::groupId(id)));
                auto *lay = new QVBoxLayout(&dlg);
                auto *lbl = new QLabel(QStringLiteral("On <b>%1</b>, open Identity → Link and scan this QR (or paste the text). That device signs the link and sends it back here; both identities then become one owner set.<br>"
                                                      "This QR is valid for 10 minutes and links only with the device that scans it.").arg(devName.toHtmlEscaped()));
                lbl->setWordWrap(true); lay->addWidget(lbl);
                auto *img = new QLabel; img->setAlignment(Qt::AlignCenter); img->setPixmap(qrPixmap(payload, 4)); if (img->pixmap().isNull()) img->setText(QStringLiteral("(install qrencode for a QR code)")); lay->addWidget(img);
                auto *ed = new QPlainTextEdit(payload); ed->setReadOnly(true); ed->setMaximumHeight(80); lay->addWidget(ed);
                auto *bb = new QDialogButtonBox(QDialogButtonBox::Close); auto *cp = bb->addButton(QStringLiteral("Copy"), QDialogButtonBox::ActionRole);
                connect(cp, &QPushButton::clicked, &dlg, [payload] { QApplication::clipboard()->setText(payload); }); connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject); lay->addWidget(bb);
                dlg.exec();
            });
            connect(drop, &QPushButton::clicked, this, [this, id] { m_loc->identity()->removePending(id); refreshDevices(); });
            h->addWidget(link); h->addWidget(drop);
            m_linkTable->setCellWidget(i, 4, cell);
        }
        m_linkTable->resizeColumnsToContents(); m_linkTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    }

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

// ── Identity tab (docs/IDENTITY.md) ────────────────────────────────────────────
static QPixmap qrPixmap(const QString &text, int scale)
{
    const QString bin = QStandardPaths::findExecutable(QStringLiteral("qrencode"));
    QPixmap px;
    if (bin.isEmpty() || text.isEmpty()) return px;
    QProcess q; q.start(bin, {QStringLiteral("-o"), QStringLiteral("-"), QStringLiteral("-t"), QStringLiteral("PNG"), QStringLiteral("-s"), QString::number(scale), QStringLiteral("-m"), QStringLiteral("2"), text});
    q.waitForFinished(8000);
    px.loadFromData(q.readAllStandardOutput(), "PNG");
    return px;
}

QWidget *MainWindow::buildIdentity()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_idSummary = new QLabel; m_idSummary->setWordWrap(true); m_idSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    { QFont f = m_idSummary->font(); f.setPointSizeF(f.pointSizeF() * 1.1); m_idSummary->setFont(f); }
    v->addWidget(m_idSummary);
    auto *row = new QHBoxLayout;
    m_idCreate = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add-user")), QStringLiteral("Create a new identity…"));
    m_idImport = new QPushButton(QIcon::fromTheme(QStringLiteral("document-import")), QStringLiteral("Import…"));
    m_idExport = new QPushButton(QIcon::fromTheme(QStringLiteral("document-export")), QStringLiteral("Export / move to another device…"));
    m_idLink   = new QPushButton(QIcon::fromTheme(QStringLiteral("insert-link")), QStringLiteral("Link with another identity…"));
    m_idForget = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), QStringLiteral("Forget this device"));
    for (QPushButton *b : {m_idCreate, m_idImport, m_idExport, m_idLink, m_idForget}) row->addWidget(b);
    row->addStretch(); v->addLayout(row);
    auto *mid = new QHBoxLayout;
    m_idQr = new QLabel; m_idQr->setAlignment(Qt::AlignTop | Qt::AlignHCenter); m_idQr->setMinimumWidth(220);
    m_idQr->setToolTip(QStringLiteral("Our link QR: another BeaconFix (phone, laptop) scans it to link its identity with this one. It carries only public data."));
    m_idDetails = new QPlainTextEdit; m_idDetails->setReadOnly(true);
    { QFont mono = m_idDetails->font(); mono.setFamily(QStringLiteral("monospace")); mono.setStyleHint(QFont::Monospace); m_idDetails->setFont(mono); }
    mid->addWidget(m_idDetails, 1); mid->addWidget(m_idQr);
    v->addLayout(mid, 1);

    // ── BeaconFix devices on this network (mDNS + optional subnet scan) ──
    auto *peerHead = new QHBoxLayout;
    auto *peerTitle = new QLabel(QStringLiteral("<b>BeaconFix devices on this network</b>")); peerHead->addWidget(peerTitle);
    m_peerNote = new QLabel; m_peerNote->setStyleSheet(QStringLiteral("color: palette(mid)")); peerHead->addWidget(m_peerNote, 1);
    m_peerLink = new QPushButton(QIcon::fromTheme(QStringLiteral("insert-link")), QStringLiteral("Link…")); m_peerLink->setEnabled(false);
    m_peerLink->setToolTip(QStringLiteral("Link our identity with the selected device's in one step over the network: we sign first, it co-signs, both keep the statement. If you are on the same LAN it is most likely your own device."));
    m_peerSync = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Sync now")); m_peerSync->setEnabled(false);
    m_peerSync->setToolTip(QStringLiteral("One sync round with the selected device. With the same or a linked identity no token is needed."));
    m_peerScan = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-find")), QStringLiteral("Scan the network"));
    m_peerScan->setToolTip(QStringLiteral("Probe every host of the local /24 networks for the BeaconFix API — for networks that block multicast DNS."));
    peerHead->addWidget(m_peerLink); peerHead->addWidget(m_peerSync); peerHead->addWidget(m_peerScan);
    v->addLayout(peerHead);
    m_peerTable = new QTableWidget(0, 6);
    m_peerTable->setHorizontalHeaderLabels({QStringLiteral("Device"), QStringLiteral("Identity"), QStringLiteral("Relation"), QStringLiteral("Address"), QStringLiteral("Version"), QStringLiteral("Seen")});
    m_peerTable->horizontalHeader()->setStretchLastSection(true); m_peerTable->verticalHeader()->setVisible(false);
    m_peerTable->setSelectionBehavior(QAbstractItemView::SelectRows); m_peerTable->setSelectionMode(QAbstractItemView::SingleSelection); m_peerTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_peerTable->setMaximumHeight(150);
    v->addWidget(m_peerTable);
    connect(m_peerTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = m_peerTable->currentRow();
        const QJsonObject peer = row >= 0 && m_peerTable->item(row, 0) ? QJsonDocument::fromJson(m_peerTable->item(row, 0)->data(Qt::UserRole).toByteArray()).object() : QJsonObject();
        Identity *idn = m_loc->identity();
        m_peerLink->setEnabled(!peer.isEmpty() && idn && idn->unlocked() && !peer["identityId"].toString().isEmpty() && !peer["sameIdentity"].toBool() && !peer["linked"].toBool());
        m_peerSync->setEnabled(!peer.isEmpty() && peer["features"].toArray().contains(QJsonValue(QStringLiteral("sync"))));
    });
    connect(m_peerTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { if (m_peerTable->item(row, 0)) linkWithPeer(QJsonDocument::fromJson(m_peerTable->item(row, 0)->data(Qt::UserRole).toByteArray()).object()); });
    connect(m_peerLink, &QPushButton::clicked, this, [this] { const int row = m_peerTable->currentRow(); if (row >= 0 && m_peerTable->item(row, 0)) linkWithPeer(QJsonDocument::fromJson(m_peerTable->item(row, 0)->data(Qt::UserRole).toByteArray()).object()); });
    connect(m_peerSync, &QPushButton::clicked, this, [this] {
        const int row = m_peerTable->currentRow(); if (row < 0 || !m_peerTable->item(row, 0)) return;
        const QJsonObject peer = QJsonDocument::fromJson(m_peerTable->item(row, 0)->data(Qt::UserRole).toByteArray()).object();
        m_peerSync->setEnabled(false); statusBar()->showMessage(QStringLiteral("Syncing with %1…").arg(peer["host"].toString()));
        QApplication::processEvents();
        const QJsonObject r = QJsonDocument::fromJson(m_loc->Sync(peer["url"].toString(), QString()).toUtf8()).object();
        statusBar()->showMessage((r["ok"].toBool() ? QStringLiteral("Synced with %1: %2") : QStringLiteral("Sync with %1 failed: %2")).arg(peer["host"].toString(), r["message"].toString().isEmpty() ? r["error"].toString() : r["message"].toString()), 8000);
        if (!r["ok"].toBool()) QMessageBox::warning(this, QStringLiteral("Sync"), r["message"].toString().isEmpty() ? r["error"].toString() : r["message"].toString());
        m_peerSync->setEnabled(true);
    });
    connect(m_peerScan, &QPushButton::clicked, this, [this] {
        if (!m_loc->apiServer()) return;
        m_peerScan->setEnabled(false); m_peerNote->setText(QStringLiteral("scanning the local networks…"));
        m_loc->apiServer()->scanPeers([this] { m_peerScan->setEnabled(true); refreshPeers(); });
    });
    connect(m_loc, &Locator::peersChanged, this, &MainWindow::refreshPeers);
    QTimer::singleShot(0, this, &MainWindow::refreshPeers);

    auto *note = new QLabel(QStringLiteral("An identity is an Ed25519 key pair plus a name. Its private key is sealed with the map-database key and never leaves this machine unencrypted; "
                                           "exports are scrypt + AES-256-GCM bundles (QR, text or file) protected by a passphrase or a 6-word code. Devices that hold the same identity, "
                                           "or one linked to it, sign in to the LAN API with a challenge signature — no pairing codes. See docs/IDENTITY.md."));
    note->setWordWrap(true); note->setStyleSheet(QStringLiteral("color: palette(mid)")); v->addWidget(note);

    Identity *idn = m_loc->identity();
    connect(m_idCreate, &QPushButton::clicked, this, [this, idn] {
        if (idn->exists() && QMessageBox::question(this, QStringLiteral("Replace identity?"), QStringLiteral("An identity already exists here (%1). Creating a new one replaces it on this device — export it first if you want to keep it. Continue?").arg(idn->groupedId())) != QMessageBox::Yes) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("New identity"), QStringLiteral("Display name (shown to your other devices):"), QLineEdit::Normal, QHostInfo::localHostName(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        if (idn->exists()) idn->forget();
        QString err;
        if (!idn->create(name, QHostInfo::localHostName(), QStringLiteral("desktop"), &err)) { QMessageBox::warning(this, QStringLiteral("Identity"), err); return; }
        statusBar()->showMessage(QStringLiteral("Identity created: %1").arg(idn->groupedId()), 4000);
        refreshIdentity();
    });
    connect(m_idImport, &QPushButton::clicked, this, [this, idn] {
        QDialog dlg(this); dlg.setWindowTitle(QStringLiteral("Import an identity"));
        auto *lay = new QFormLayout(&dlg);
        auto *text = new QPlainTextEdit; text->setPlaceholderText(QStringLiteral("Paste the beaconfix://identity/… (or BFID1:…) text here, or use the buttons below")); text->setMinimumSize(520, 120);
        auto *fileBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open")), QStringLiteral("Open a bundle file…"));
        connect(fileBtn, &QPushButton::clicked, &dlg, [text, this] { const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Open identity bundle")); if (p.isEmpty()) return; QFile f(p); if (f.open(QIODevice::ReadOnly)) text->setPlainText(QString::fromUtf8(f.readAll()).trimmed()); });
        auto *lanRow = new QHBoxLayout; auto *host = new QLineEdit; host->setPlaceholderText(QStringLiteral("other-beaconfix:47822")); auto *code = new QLineEdit; code->setPlaceholderText(QStringLiteral("6-digit code")); code->setMaximumWidth(110);
        auto *fetch = new QPushButton(QStringLiteral("Fetch from that BeaconFix"));
        lanRow->addWidget(host, 1); lanRow->addWidget(code); lanRow->addWidget(fetch);
        connect(fetch, &QPushButton::clicked, &dlg, [text, host, code, this] {
            QNetworkAccessManager nam; QString h = host->text().trimmed(); if (!h.contains(QLatin1Char(':'))) h += QStringLiteral(":47822");
            QNetworkRequest req(QUrl(QStringLiteral("http://%1/api/v1/identity/export/%2").arg(h, code->text().trimmed()))); req.setTransferTimeout(8000);
            QNetworkReply *rep = nam.get(req); QEventLoop loop; connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit); loop.exec();
            const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object(); rep->deleteLater();
            if (o["bundle"].toString().isEmpty()) QMessageBox::warning(this, QStringLiteral("Fetch failed"), o["error"].toString().isEmpty() ? rep->errorString() : o["error"].toString());
            else text->setPlainText(o["bundle"].toString());
        });
        auto *pass = new QLineEdit; pass->setEchoMode(QLineEdit::Password); pass->setPlaceholderText(QStringLiteral("passphrase or the 6-word code"));
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        lay->addRow(QStringLiteral("Bundle:"), text); lay->addRow(QString(), fileBtn); lay->addRow(QStringLiteral("From the LAN:"), lanRow); lay->addRow(QStringLiteral("Passphrase:"), pass); lay->addRow(bb);
        connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept); connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;
        if (idn->exists() && QMessageBox::question(this, QStringLiteral("Replace identity?"), QStringLiteral("Importing replaces the identity on this device (%1). Continue?").arg(idn->groupedId())) != QMessageBox::Yes) return;
        QString err;
        if (!idn->importBundle(text->toPlainText(), pass->text(), QHostInfo::localHostName(), QStringLiteral("desktop"), &err)) { QMessageBox::warning(this, QStringLiteral("Import failed"), err); return; }
        statusBar()->showMessage(QStringLiteral("Identity imported: %1").arg(idn->groupedId()), 4000);
        refreshIdentity();
    });
    connect(m_idExport, &QPushButton::clicked, this, [this, idn] {
        if (!idn->unlocked()) { QMessageBox::warning(this, QStringLiteral("Identity"), QStringLiteral("No unlocked identity to export.")); return; }
        QDialog dlg(this); dlg.setWindowTitle(QStringLiteral("Export identity"));
        auto *lay = new QFormLayout(&dlg);
        auto *words = new QCheckBox(QStringLiteral("Protect with a generated 6-word code (shown once) instead of a passphrase")); words->setChecked(true);
        auto *pass = new QLineEdit; pass->setEchoMode(QLineEdit::Password); pass->setPlaceholderText(QStringLiteral("8+ characters")); pass->setEnabled(false);
        connect(words, &QCheckBox::toggled, pass, [pass](bool on) { pass->setEnabled(!on); });
        auto *lan = new QCheckBox(QStringLiteral("Also hold it for 10 minutes for a device on this network (it enters a 6-digit code instead of scanning)")); lan->setChecked(true);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        lay->addRow(words); lay->addRow(QStringLiteral("Passphrase:"), pass); lay->addRow(lan); lay->addRow(bb);
        connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept); connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;
        const QString secret = words->isChecked() ? Identity::wordCode() : pass->text();
        QString err; const QString bundle = idn->exportBundle(secret, &err);
        if (bundle.isEmpty()) { QMessageBox::warning(this, QStringLiteral("Export failed"), err); return; }
        QString code; if (lan->isChecked() && m_loc->apiServer()) code = m_loc->apiServer()->holdIdentityExport(bundle);
        QDialog show(this); show.setWindowTitle(QStringLiteral("Identity bundle — scan, paste or fetch on the other device"));
        auto *sv = new QVBoxLayout(&show);
        if (words->isChecked()) { auto *wl = new QLabel(QStringLiteral("<b>Word code (shown once, needed on the other device):</b><br><span style='font-size:16pt'>%1</span>").arg(secret.toHtmlEscaped())); wl->setTextInteractionFlags(Qt::TextSelectableByMouse); sv->addWidget(wl); }
        if (!code.isEmpty()) { auto *cl = new QLabel(QStringLiteral("On the same network: the other BeaconFix can fetch it from <b>%1:%2</b> with code <b style='font-size:16pt'>%3</b> for the next 10 minutes.").arg(QHostInfo::localHostName()).arg(m_loc->apiServer()->boundPort()).arg(code)); cl->setWordWrap(true); sv->addWidget(cl); }
        auto *img = new QLabel; img->setAlignment(Qt::AlignCenter); img->setPixmap(qrPixmap(bundle, 4)); if (img->pixmap().isNull()) img->setText(QStringLiteral("(install qrencode for a QR code)")); sv->addWidget(img);
        auto *ed = new QPlainTextEdit(bundle); ed->setReadOnly(true); ed->setMaximumHeight(100); sv->addWidget(ed);
        auto *sb = new QDialogButtonBox(QDialogButtonBox::Close);
        auto *cp = sb->addButton(QStringLiteral("Copy text"), QDialogButtonBox::ActionRole); auto *sf = sb->addButton(QStringLiteral("Save to file…"), QDialogButtonBox::ActionRole);
        connect(cp, &QPushButton::clicked, &show, [bundle] { QApplication::clipboard()->setText(bundle); });
        connect(sf, &QPushButton::clicked, &show, [bundle, this] { const QString p = QFileDialog::getSaveFileName(this, QStringLiteral("Save identity bundle"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/beaconfix-identity.bfid"), QStringLiteral("BeaconFix identity (*.bfid)")); if (p.isEmpty()) return; QFile f(p); if (f.open(QIODevice::WriteOnly)) { f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); f.write(bundle.toUtf8() + "\n"); } });
        connect(sb, &QDialogButtonBox::rejected, &show, &QDialog::reject); sv->addWidget(sb);
        show.exec();
    });
    connect(m_idLink, &QPushButton::clicked, this, [this, idn] {
        if (!idn->unlocked()) { QMessageBox::warning(this, QStringLiteral("Identity"), QStringLiteral("No unlocked identity here.")); return; }
        QDialog dlg(this); dlg.setWindowTitle(QStringLiteral("Link with another identity"));
        auto *lay = new QVBoxLayout(&dlg);
        lay->addWidget(new QLabel(QStringLiteral("Paste what the other device shows: its <b>link payload</b> (beaconfix://link/…) to start a link, or a <b>link statement</b> (beaconfix://statement/… / JSON) it already signed to complete one. The older BFLNK1: / BFLINK1: texts still work.")));
        auto *text = new QPlainTextEdit; text->setMinimumSize(520, 110); lay->addWidget(text);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); lay->addWidget(bb);
        connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept); connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;
        const QString t = text->toPlainText().trimmed();
        QString kind; QJsonObject o = Identity::decodePayload(t, &kind);
        if (o.isEmpty()) { QMessageBox::warning(this, QStringLiteral("Link"), QStringLiteral("That is not a BeaconFix link payload or statement.")); return; }
        if (kind == QLatin1String("identity")) { QMessageBox::warning(this, QStringLiteral("Link"), QStringLiteral("That is an identity export bundle, not a link: use Import… for it.")); return; }
        if (o["t"].toString() == QLatin1String("beaconfix-link") || (o.contains("pub") && o.contains("id") && !o.contains("a"))) {
            const QByteArray pub = QByteArray::fromBase64(o["pub"].toString().toLatin1()); const QString id = o["id"].toString();
            if (pub.size() != 32 || Identity::idFor(pub) != id) { QMessageBox::warning(this, QStringLiteral("Link"), QStringLiteral("That payload's id does not match its key.")); return; }
            if (o["ts"].toString().isEmpty()) { QMessageBox::warning(this, QStringLiteral("Link"), QStringLiteral("That link payload carries no offer timestamp; show the link QR on the other device again.")); return; }
            const LinkStatement st = idn->startLink(id, pub, o["ts"].toString());   // bound to the offer that device displayed
            const QString out = Identity::encodeUri(QStringLiteral("statement"), st.toJson(true));
            QDialog show(this); show.setWindowTitle(QStringLiteral("Half-signed link statement")); auto *sv = new QVBoxLayout(&show);
            sv->addWidget(new QLabel(QStringLiteral("Signed by us. Give this to %1 to co-sign (scan / paste on that device, Identity → Link); once it does, both sides are one owner set.").arg(o["name"].toString().isEmpty() ? Identity::groupId(id) : o["name"].toString())));
            auto *img = new QLabel; img->setAlignment(Qt::AlignCenter); img->setPixmap(qrPixmap(out, 4)); sv->addWidget(img);
            auto *ed = new QPlainTextEdit(out); ed->setReadOnly(true); ed->setMaximumHeight(90); sv->addWidget(ed);
            auto *sb = new QDialogButtonBox(QDialogButtonBox::Close); auto *cp = sb->addButton(QStringLiteral("Copy"), QDialogButtonBox::ActionRole);
            connect(cp, &QPushButton::clicked, &show, [out] { QApplication::clipboard()->setText(out); }); connect(sb, &QDialogButtonBox::rejected, &show, &QDialog::reject); sv->addWidget(sb);
            show.exec();
            return;
        }
        QString err; LinkStatement done;
        if (!idn->acceptLink(LinkStatement::fromJson(o.contains("statement") ? o["statement"].toObject() : o), &err, &done)) { QMessageBox::warning(this, QStringLiteral("Link failed"), err); return; }
        if (done.complete()) {
            const QString out = Identity::encodeUri(QStringLiteral("statement"), done.toJson(true));
            QApplication::clipboard()->setText(out);
            QMessageBox::information(this, QStringLiteral("Linked"), QStringLiteral("Identities %1 and %2 are now linked here. The completed statement is on the clipboard — paste it on the other device too (or it will pick it up on its next sync).").arg(Identity::groupId(done.a), Identity::groupId(done.b)));
        }
        refreshIdentity();
    });
    connect(m_idForget, &QPushButton::clicked, this, [this, idn] {
        if (!idn->exists()) return;
        if (QMessageBox::question(this, QStringLiteral("Forget this device?"), QStringLiteral("Remove the identity %1 from this machine? Other devices keep their copies; export first if this is the only one.").arg(idn->groupedId()), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        idn->forget(); refreshIdentity();
    });
    connect(idn, &Identity::changed, this, &MainWindow::refreshIdentity);
    refreshIdentity();
    return w;
}

void MainWindow::refreshIdentity()
{
    Identity *idn = m_loc->identity();
    if (!idn || !m_idSummary) return;
    const bool have = idn->exists();
    m_idExport->setEnabled(have && idn->unlocked()); m_idLink->setEnabled(have && idn->unlocked()); m_idForget->setEnabled(have);
    if (!have) {
        m_idSummary->setText(QStringLiteral("<b>No identity on this BeaconFix yet.</b> Create one, or import the one you made on your phone or laptop."));
        m_idQr->clear(); m_idDetails->clear();
        if (m_linkTable) refreshDevices();
        return;
    }
    m_idSummary->setText(QStringLiteral("<b>%1</b> &nbsp; <span style='font-family:monospace'>%2</span> &nbsp;·&nbsp; %3 &nbsp;·&nbsp; %4 device(s) &nbsp;·&nbsp; %5 linked identit%6")
                         .arg(idn->name().toHtmlEscaped(), idn->groupedId(), idn->unlocked() ? QStringLiteral("unlocked") : QStringLiteral("<span style='color:#ff9f43'>LOCKED — map-database key missing or changed</span>"))
                         .arg(idn->devices().size()).arg(idn->linkedIds().size()).arg(idn->linkedIds().size() == 1 ? QStringLiteral("y") : QStringLiteral("ies")));
    QString d = QStringLiteral("id:       %1\npub:      %2\ncreated:  %3\nfile:     %4\n\nDevices:\n").arg(idn->id(), QString::fromLatin1(idn->pub().toBase64()), idn->created().toString(Qt::ISODate), Identity::filePath());
    for (const IdentityDevice &dv : idn->devices()) d += QStringLiteral("  %1 (%2) added %3\n").arg(dv.name, dv.kind, dv.added.toString(QStringLiteral("d MMM yyyy")));
    d += QStringLiteral("\nLinks:\n");
    if (idn->links().isEmpty()) d += QStringLiteral("  none\n");
    for (const LinkStatement &l : idn->links()) d += QStringLiteral("  %1 ⇄ %2  (%3)\n").arg(Identity::groupId(l.a), Identity::groupId(l.b), l.ts);
    if (!idn->pending().isEmpty()) { d += QStringLiteral("\nSign-ins waiting to be linked (Devices tab):\n"); for (const PendingLink &p : idn->pending()) d += QStringLiteral("  %1  %2 (%3) from %4\n").arg(Identity::groupId(p.id), p.deviceName, p.deviceKind, p.ip); }
    m_idDetails->setPlainText(d);
    const QString payload = Identity::encodeUri(QStringLiteral("link"), idn->linkPayload());   // registers the offer: only statements bound to it get co-signed
    QPixmap px = qrPixmap(payload, 4);
    if (px.isNull()) m_idQr->setText(QStringLiteral("link QR needs qrencode")); else m_idQr->setPixmap(px);
    if (m_linkTable) refreshDevices();
}

void MainWindow::refreshPeers()
{
    if (!m_peerTable || !m_loc->apiServer()) return;
    const QJsonArray peers = m_loc->apiServer()->peersJson(false);
    const QString sel = m_peerTable->currentRow() >= 0 && m_peerTable->item(m_peerTable->currentRow(), 0) ? m_peerTable->item(m_peerTable->currentRow(), 0)->text() : QString();
    m_peerTable->setRowCount(peers.size());
    int row = 0, selRow = -1;
    const QDateTime now = QDateTime::currentDateTime();
    for (const QJsonValue &v : peers) {
        const QJsonObject p = v.toObject();
        const QString rel = p["sameIdentity"].toBool() ? QStringLiteral("same identity") : p["linked"].toBool() ? QStringLiteral("linked") : p["identityId"].toString().isEmpty() ? QStringLiteral("no identity yet") : QStringLiteral("other identity — link?");
        const QDateTime seen = QDateTime::fromString(p["lastSeen"].toString(), Qt::ISODate);
        const QStringList cells{p["host"].toString() + (p["kind"].toString() == QLatin1String("desktop") ? QString() : QStringLiteral(" (%1)").arg(p["kind"].toString())),
                                p["identityName"].toString().isEmpty() ? QStringLiteral("—") : QStringLiteral("%1  %2").arg(p["identityName"].toString(), Identity::groupId(p["identityId"].toString())),
                                rel, p["url"].toString(), p["version"].toString(), seen.isValid() ? (seen.secsTo(now) < 90 ? QStringLiteral("now") : QStringLiteral("%1 min ago").arg(seen.secsTo(now) / 60)) + (p["source"].toString() == QLatin1String("scan") ? QStringLiteral(" (scan)") : QString()) : QString()};
        for (int c = 0; c < cells.size(); ++c) {
            auto *it = new QTableWidgetItem(cells[c]);
            if (c == 0) { it->setData(Qt::UserRole, QJsonDocument(p).toJson(QJsonDocument::Compact)); it->setToolTip(QStringLiteral("all addresses: %1").arg(QJsonDocument(p["addresses"].toArray()).toJson(QJsonDocument::Compact))); }
            if (c == 2) it->setForeground(p["sameIdentity"].toBool() || p["linked"].toBool() ? QColor(0x6c, 0xff, 0x8a) : p["identityId"].toString().isEmpty() ? QColor(0x8a, 0x93, 0xa6) : QColor(0xff, 0xd1, 0x66));
            m_peerTable->setItem(row, c, it);
        }
        if (!sel.isEmpty() && cells[0] == sel) selRow = row;
        ++row;
    }
    m_peerTable->resizeColumnsToContents();
    if (selRow >= 0) m_peerTable->selectRow(selRow);
    else { m_peerLink->setEnabled(false); m_peerSync->setEnabled(false); }
    const bool mdns = m_loc->apiServer()->mdns() && m_loc->apiServer()->mdns()->published();
    m_peerNote->setText(peers.isEmpty() ? (mdns ? QStringLiteral("none seen yet — devices on the same LAN are most likely yours; a phone with the app shows up here as soon as it is on the network")
                                                : QStringLiteral("mDNS not available (Avahi) — use Scan"))
                                         : QStringLiteral("%1 device%2 · %3").arg(peers.size()).arg(peers.size() == 1 ? QString() : QStringLiteral("s"), mdns ? QStringLiteral("advertised over mDNS") : QStringLiteral("mDNS off")));
}

// One-step link with a device on the LAN: fetch its public identity, sign a link statement, POST it for
// co-signing, keep the completed statement. The other side needs no manual action beyond accepting.
void MainWindow::linkWithPeer(const QJsonObject &peer)
{
    Identity *idn = m_loc->identity();
    if (!idn || !idn->unlocked()) { QMessageBox::warning(this, QStringLiteral("Link"), QStringLiteral("No unlocked identity here — create or import one first.")); return; }
    const QString url = peer["url"].toString(), who = peer["identityName"].toString().isEmpty() ? peer["host"].toString() : peer["identityName"].toString();
    if (url.isEmpty()) return;
    QNetworkAccessManager nam;
    auto call = [&](const QString &ep, const QByteArray &body = QByteArray()) {
        QNetworkRequest req(QUrl(url + QStringLiteral("/api/v1/") + ep)); req.setTransferTimeout(10000); req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QNetworkReply *rep = body.isNull() ? nam.get(req) : nam.post(req, body);
        QEventLoop loop; connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit); loop.exec();
        QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        if (rep->error() != QNetworkReply::NoError && o["error"].toString().isEmpty()) o["error"] = rep->errorString();
        rep->deleteLater(); return o;
    };
    const QJsonObject other = call(QStringLiteral("identity"));
    const QString id = other["id"].toString(); const QByteArray pub = QByteArray::fromBase64(other["pub"].toString().toLatin1());
    if (id.isEmpty() || pub.size() != 32) { QMessageBox::warning(this, QStringLiteral("Link"), QStringLiteral("%1 has no identity yet (%2). Create one there first.").arg(who, other["error"].toString())); return; }
    if (Identity::idFor(pub) != id) { QMessageBox::warning(this, QStringLiteral("Link"), QStringLiteral("%1's identity record is inconsistent (id does not match its key).").arg(who)); return; }
    if (id == idn->id()) { QMessageBox::information(this, QStringLiteral("Link"), QStringLiteral("%1 already holds the same identity — nothing to link.").arg(who)); return; }
    if (idn->isOwner(id)) { QMessageBox::information(this, QStringLiteral("Link"), QStringLiteral("%1's identity is already linked with ours.").arg(who)); return; }
    if (QMessageBox::question(this, QStringLiteral("Link identities?"),
                              QStringLiteral("Link our identity <b>%1</b> (%2) with <b>%3</b> (%4) on %5?<br><br>Both become one owner set: either signs in to the other without pairing, and their map data merges. Only do this for your own devices.")
                                  .arg(idn->name().toHtmlEscaped(), idn->groupedId(), other["name"].toString().toHtmlEscaped(), Identity::groupId(id), peer["host"].toString().toHtmlEscaped())) != QMessageBox::Yes) return;
    const LinkStatement half = idn->startLink(id, pub);
    const QJsonObject res = call(QStringLiteral("identity/link"), QJsonDocument(QJsonObject{{"statement", half.toJson(true)}}).toJson(QJsonDocument::Compact));
    const QJsonObject stJson = res["statement"].toObject();
    if (stJson.isEmpty()) { QMessageBox::warning(this, QStringLiteral("Link failed"), QStringLiteral("%1 did not co-sign: %2").arg(who, res["error"].toString())); return; }
    QString err; LinkStatement done;
    if (!idn->acceptLink(LinkStatement::fromJson(stJson), &err, &done)) { QMessageBox::warning(this, QStringLiteral("Link failed"), QStringLiteral("The returned statement did not verify: %1").arg(err)); return; }
    statusBar()->showMessage(QStringLiteral("Linked with %1 (%2)").arg(who, Identity::groupId(id)), 6000);
    refreshIdentity(); refreshPeers();
}

void MainWindow::showIdentity() { if (m_identityTab) m_tabs->setCurrentWidget(m_identityTab); show(); raise(); activateWindow(); }

// Settings → Map database → Import…: file, time window, what to take; progress; summary (docs/DATABASE.md)
void MainWindow::importHistoryDialog()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import your location history"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
                                                      QStringLiteral("History exports (*.json *.csv *.gpx *.kml);;Google Timeline / Records (*.json);;WiGLE CSV (*.csv);;GPX tracks (*.gpx);;KML (*.kml);;All files (*)"));
    if (path.isEmpty()) return;
    QDialog dlg(this); dlg.setWindowTitle(QStringLiteral("Import %1").arg(QFileInfo(path).fileName())); dlg.setMinimumWidth(460);
    auto *lay = new QVBoxLayout(&dlg);
    auto *info = new QLabel(QStringLiteral("<b>%1</b> · %2<br>Positions become history (a dotted trail on the map), Wi-Fi scans become samples for the beacons heard, visits become stops. Nothing here ever moves the live fix.")
                            .arg(QFileInfo(path).fileName(), QLocale().formattedDataSize(QFileInfo(path).size())));
    info->setWordWrap(true); lay->addWidget(info);
    auto *form = new QFormLayout;
    auto *from = new QDateEdit; from->setCalendarPopup(true); from->setDisplayFormat(QStringLiteral("yyyy-MM-dd")); from->setDate(QDate(2010, 1, 1)); from->setSpecialValueText(QStringLiteral("(beginning)")); from->setMinimumDate(QDate(2000, 1, 1)); from->setDate(from->minimumDate());
    auto *to = new QDateEdit; to->setCalendarPopup(true); to->setDisplayFormat(QStringLiteral("yyyy-MM-dd")); to->setMaximumDate(QDate::currentDate().addDays(1)); to->setSpecialValueText(QStringLiteral("(today)")); to->setMinimumDate(QDate(2000, 1, 1)); to->setDate(to->minimumDate());
    form->addRow(QStringLiteral("From:"), from); form->addRow(QStringLiteral("To:"), to);
    auto *positions = new QCheckBox(QStringLiteral("Positions (history trail)")); positions->setChecked(true);
    auto *wifi = new QCheckBox(QStringLiteral("Wi-Fi scans (beacon samples)")); wifi->setChecked(true);
    auto *places = new QCheckBox(QStringLiteral("Visits / places (stops)")); places->setChecked(true);
    form->addRow(QStringLiteral("Take:"), positions); form->addRow(QString(), wifi); form->addRow(QString(), places);
    lay->addLayout(form);
    auto *bar = new QProgressBar; bar->setRange(0, 100); bar->setValue(0); bar->setTextVisible(true); bar->setFormat(QStringLiteral("%p%")); lay->addWidget(bar);
    auto *stage = new QLabel; stage->setWordWrap(true); lay->addWidget(stage);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Import"));
    lay->addWidget(bb);
    bool running = false, done = false; QJsonObject summary; bool ok = false;
    connect(bb, &QDialogButtonBox::rejected, &dlg, [&] { if (!running) dlg.reject(); });
    connect(m_loc, &Locator::importProgress, &dlg, [&](const QString &json) {
        const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
        if (o["file"].toString() != path) return;
        bar->setValue(o["percent"].toInt()); stage->setText(o["stage"].toString());
    });
    connect(bb, &QDialogButtonBox::accepted, &dlg, [&] {
        if (running) return;
        if (done) { dlg.accept(); return; }
        QJsonObject opts;
        if (from->date() > from->minimumDate()) opts["from"] = from->date().startOfDay().toString(Qt::ISODate);
        if (to->date() > to->minimumDate()) opts["to"] = to->date().endOfDay().toString(Qt::ISODate);
        QStringList what; if (positions->isChecked()) what << QStringLiteral("positions"); if (wifi->isChecked()) what << QStringLiteral("wifi"); if (places->isChecked()) what << QStringLiteral("places");
        if (what.isEmpty()) { QMessageBox::information(&dlg, QStringLiteral("Nothing to import"), QStringLiteral("Tick at least one of positions, Wi-Fi scans or visits.")); return; }
        opts["what"] = QJsonArray::fromStringList(what);
        running = true; bb->button(QDialogButtonBox::Ok)->setEnabled(false); bb->button(QDialogButtonBox::Cancel)->setEnabled(false);
        for (QWidget *w : QList<QWidget *>{from, to, positions, wifi, places}) w->setEnabled(false);
        stage->setText(QStringLiteral("Reading…"));
        const QJsonObject res = QJsonDocument::fromJson(m_loc->Import(path, QString::fromUtf8(QJsonDocument(opts).toJson(QJsonDocument::Compact))).toUtf8()).object();
        running = false; done = true; ok = res["ok"].toBool(); summary = res["summary"].toObject();
        bar->setValue(100);
        if (!ok) { stage->setText(QStringLiteral("<span style='color:#e06c75'>Import failed: %1</span>").arg(res["error"].toString().toHtmlEscaped())); }
        else stage->setText(QStringLiteral("Done: %1 positions, %2 track points, %3 Wi-Fi scans → %4 samples on %5 beacons, %6 visits, %7 skipped, from %8 to %9, in %10 s.")
                            .arg(summary["positions"].toInt()).arg(summary["tracks"].toInt()).arg(summary["wifiScans"].toInt()).arg(summary["observations"].toInt()).arg(summary["beaconsTouched"].toInt())
                            .arg(summary["visits"].toInt()).arg(summary["skipped"].toInt()).arg(summary["first"].toString().left(10), summary["last"].toString().left(10)).arg(summary["seconds"].toDouble(), 0, 'f', 1));
        bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Close")); bb->button(QDialogButtonBox::Ok)->setEnabled(true);
    });
    dlg.exec();
    if (ok) statusBar()->showMessage(QStringLiteral("Imported %1: %2 samples, %3 positions, %4 visits").arg(QFileInfo(path).fileName()).arg(summary["observations"].toInt()).arg(summary["positions"].toInt() + summary["tracks"].toInt()).arg(summary["visits"].toInt()), 8000);
}

void MainWindow::showPairRequest(const QString &id)
{
    ApiServer *api = m_loc->apiServer();
    if (!api || id.isEmpty()) return;
    // Land on the Devices tab too, so the request is visible even after the dialog closes
    for (int i = 0; i < m_tabs->count(); ++i) if (m_tabs->tabText(i).startsWith(QStringLiteral("Devices"))) m_tabs->setCurrentIndex(i);
    show(); raise(); activateWindow();
    PairDialog *dlg = m_pairDialogs.value(id);
    if (!dlg) {
        dlg = new PairDialog(api, m_loc, m_tiles, id, this);
        m_pairDialogs.insert(id, dlg);
        connect(dlg, &QObject::destroyed, this, [this, id] { m_pairDialogs.remove(id); });
    }
    dlg->show(); dlg->raise(); dlg->activateWindow();
}
void MainWindow::showEmergency() { m_tabs->setCurrentIndex(1); show(); raise(); activateWindow(); }
