#include "tray.h"
#include "locator.h"
#include "identity.h"
#include "apiserver.h"
#include "mdns.h"
#include "osintegration.h"
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QStandardPaths>
#include <QUrl>
#include <QPainter>
#include <QRadialGradient>

// Theme-independent beacon icon: dark disc, rings, a dot coloured by source
static QIcon beaconIcon(const QColor &dot, bool busy)
{
    QIcon icon;
    for (int sz : {16, 22, 24, 32, 48, 64}) {
        QPixmap px(sz, sz); px.fill(Qt::transparent);
        QPainter p(&px); p.setRenderHint(QPainter::Antialiasing);
        const QPointF c(sz / 2.0, sz / 2.0); const double R = sz / 2.0;
        p.setPen(Qt::NoPen); p.setBrush(QColor(11, 16, 26)); p.drawEllipse(c, R, R);
        QRadialGradient g(c, R * 0.9); QColor t = dot; t.setAlpha(130); g.setColorAt(0, t); t.setAlpha(0); g.setColorAt(1, t);
        p.setBrush(g); p.drawEllipse(c, R * 0.9, R * 0.9);
        p.setBrush(Qt::NoBrush);
        QColor ring = dot; ring.setAlpha(busy ? 70 : 150); p.setPen(QPen(ring, qMax(1.0, sz / 20.0)));
        p.drawEllipse(c, R * 0.68, R * 0.68); p.drawEllipse(c, R * 0.42, R * 0.42);
        p.setPen(QPen(Qt::white, qMax(1.0, sz / 24.0))); p.setBrush(dot); p.drawEllipse(c, R * 0.2, R * 0.2);
        icon.addPixmap(px);
    }
    return icon;
}

Tray::Tray(Locator *loc, QObject *parent) : QObject(parent), m_loc(loc)
{
    m_icon.setIcon(beaconIcon(QColor(0x35, 0xd6, 0xff), false));

    m_placeAct = m_menu.addAction(QString());
    QFont bold = m_placeAct->font(); bold.setBold(true); m_placeAct->setFont(bold);
    connect(m_placeAct, &QAction::triggered, this, &Tray::openWindowRequested);
    m_coordAct = m_menu.addAction(QString());
    connect(m_coordAct, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(QStringLiteral("%1, %2").arg(m_loc->fix().lat, 0, 'f', 6).arg(m_loc->fix().lon, 0, 'f', 6));
        m_icon.showMessage(QStringLiteral("BeaconFix"), QStringLiteral("Coordinates copied"), QSystemTrayIcon::Information, 2000);
    });
    m_ageAct = m_menu.addAction(QString()); m_ageAct->setEnabled(false);
    m_tripAct = m_menu.addAction(QString()); m_tripAct->setEnabled(false);
    m_sunAct = m_menu.addAction(QString()); m_sunAct->setEnabled(false);
    m_menu.addSeparator();
    m_refreshAct = m_menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Re-check location now"));
    connect(m_refreshAct, &QAction::triggered, m_loc, &Locator::Refresh);
    QAction *osm = m_menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("Open in OpenStreetMap"));
    connect(osm, &QAction::triggered, this, [this] {
        const Fix &f = m_loc->fix();
        if (f.valid) QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=15/%1/%2").arg(f.lat).arg(f.lon)));
    });
    m_shareMenu = m_menu.addMenu(QIcon::fromTheme(QStringLiteral("document-share")), QStringLiteral("Share"));
    const std::pair<const char *, const char *> shares[] = {{"coords", "Copy coordinates"}, {"geo", "Copy geo: URI"}, {"text", "Copy place + link"},
                                                            {"osm", "Copy OpenStreetMap link"}, {"google", "Copy Google Maps link"}, {"apple", "Copy Apple Maps link"}};
    for (const auto &sh : shares) {
        const QString what = QString::fromLatin1(sh.first);
        connect(m_shareMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QString::fromLatin1(sh.second)), &QAction::triggered, this, [this, what] {
            if (m_loc->CopyToClipboard(what)) m_icon.showMessage(QStringLiteral("BeaconFix"), QStringLiteral("Copied to clipboard"), QSystemTrayIcon::Information, 1500); });
    }
    m_shareMenu->addSeparator();
    connect(m_shareMenu->addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("Open in Google Maps")), &QAction::triggered, this, [this] {
        if (m_loc->fix().valid) QDesktopServices::openUrl(QUrl(m_loc->googleMapsUrl())); });
    connect(m_shareMenu->addAction(QIcon::fromTheme(QStringLiteral("document-export")), QStringLiteral("Save trip as GPX…")), &QAction::triggered, this, [this] {
        const QString def = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/beaconfix-trip.gpx";
        const QString path = QFileDialog::getSaveFileName(nullptr, QStringLiteral("Save trip as GPX"), def, QStringLiteral("GPX (*.gpx)"));
        if (!path.isEmpty()) m_loc->ExportGpx(path); });
    QAction *open = m_menu.addAction(QIcon::fromTheme(QStringLiteral("window")), QStringLiteral("Open BeaconFix"));
    connect(open, &QAction::triggered, this, &Tray::openWindowRequested);
    QAction *offline = m_menu.addAction(QIcon::fromTheme(QStringLiteral("document-save")), QStringLiteral("Save map around here for offline"));
    connect(offline, &QAction::triggered, m_loc, &Locator::PrefetchTiles);
    QAction *emerg = m_menu.addAction(QIcon::fromTheme(QStringLiteral("dialog-warning")), QStringLiteral("Nearest help (police, fire, ER)…"));
    connect(emerg, &QAction::triggered, this, &Tray::openEmergencyRequested);
    m_identityAct = m_menu.addAction(QIcon::fromTheme(QStringLiteral("user-identity")), QStringLiteral("Identity…"));
    connect(m_identityAct, &QAction::triggered, this, &Tray::openIdentityRequested);
    QAction *trip = m_menu.addAction(QIcon::fromTheme(QStringLiteral("flag")), QStringLiteral("Start a new trip here"));
    connect(trip, &QAction::triggered, m_loc, &Locator::StartTrip);
    QAction *pair = m_menu.addAction(QIcon::fromTheme(QStringLiteral("list-add-user")), QStringLiteral("Allow a device to pair (10 min)"));
    connect(pair, &QAction::triggered, m_loc, [this] { m_loc->OpenPairing(10); m_icon.showMessage(QStringLiteral("BeaconFix"), QStringLiteral("Pairing open for 10 minutes — approve requests in Devices"), QSystemTrayIcon::Information, 4000); });
    connect(m_loc, &Locator::pairingRequested, this, [this] { emit openWindowRequested(); });

    m_intervalMenu = m_menu.addMenu(QIcon::fromTheme(QStringLiteral("chronometer")), QStringLiteral("Check every"));
    auto *grp = new QActionGroup(this);
    for (int m : {5, 10, 15, 30, 60}) {
        QAction *a = m_intervalMenu->addAction(QStringLiteral("%1 min").arg(m));
        a->setCheckable(true); a->setData(m); grp->addAction(a);
        connect(a, &QAction::triggered, this, [this, m] { m_loc->setIntervalMinutes(m); });
    }
    m_menu.addSeparator();
    QAction *quit = m_menu.addAction(QIcon::fromTheme(QStringLiteral("application-exit")), QStringLiteral("Quit"));
    connect(quit, &QAction::triggered, this, &Tray::quitRequested);

    m_icon.setContextMenu(&m_menu);
    connect(&m_icon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) emit openWindowRequested();
    });

    connect(m_loc, &Locator::FixChanged, this, &Tray::rebuild);
    connect(m_loc, &Locator::peersChanged, this, &Tray::rebuild);
    connect(m_loc, &Locator::elevationUpdated, this, &Tray::rebuild);
    connect(m_loc, &Locator::probeStarted, this, &Tray::rebuild);
    connect(m_loc, &Locator::notificationFallback, this, [this](const QString &s, const QString &b) { m_icon.showMessage(s, b, QSystemTrayIcon::Information, 6000); });
    connect(m_loc, &Locator::probeFinished, this, [this](bool ok, const QString &msg) {
        rebuild();
        if (!ok) m_icon.showMessage(QStringLiteral("BeaconFix: no location"), msg, QSystemTrayIcon::Warning, 5000);
    });
    m_ageTimer.setInterval(30000);
    connect(&m_ageTimer, &QTimer::timeout, this, &Tray::rebuild);
    m_ageTimer.start();
    rebuild();
    m_icon.show();
}

QString Tray::ageText() const
{
    const Fix &f = m_loc->fix();
    if (!f.valid || !f.time.isValid()) return QStringLiteral("never checked");
    const qint64 s = f.time.secsTo(QDateTime::currentDateTime());
    if (s < 90) return QStringLiteral("just now");
    if (s < 3600) return QStringLiteral("%1 min ago").arg(s / 60);
    if (s < 86400) return QStringLiteral("%1 h ago").arg(s / 3600);
    return f.time.toString(QStringLiteral("ddd d MMM HH:mm"));
}

void Tray::rebuild()
{
    const Fix &f = m_loc->fix();
    const QString src = f.source == QLatin1String("starlink") ? QStringLiteral("Starlink GPS")
                      : f.source == QLatin1String("wifi")     ? (f.provider == QLatin1String("apple") ? QStringLiteral("Apple Wi-Fi") : QStringLiteral("BeaconDB Wi-Fi"))
                      : f.source == QLatin1String("ip")       ? QStringLiteral("IP (approximate)") : QStringLiteral("unknown");
    if (f.valid) {
        m_placeAct->setText(f.place);
        m_coordAct->setText(QStringLiteral("%1, %2  ·  ±%3 m  ·  %4  (copy)")
                            .arg(f.lat, 0, 'f', 5).arg(f.lon, 0, 'f', 5).arg(qRound(f.accuracy)).arg(src));
        m_coordAct->setEnabled(true);
    } else {
        m_placeAct->setText(QStringLiteral("No location yet"));
        m_coordAct->setText(m_loc->lastError().isEmpty() ? QStringLiteral("—") : m_loc->lastError());
        m_coordAct->setEnabled(false);
    }
    m_ageAct->setText(m_loc->busy() ? QStringLiteral("Checking…") : QStringLiteral("Updated ") + ageText());
    const Stats st = m_loc->stats();
    QString trip = QStringLiteral("%1 · Lv %2").arg(st.rank).arg(st.rankLevel);
    if (st.moving && st.speedKmh >= 0) trip += QStringLiteral(" · %1 km/h %2").arg(qRound(st.speedKmh)).arg(Locator::compass(st.headingDeg));
    else if (st.dwellSecs > 0) trip += QStringLiteral(" · here %1").arg(Locator::durationText(st.dwellSecs));
    trip += QStringLiteral(" · today %1 km · trip %2 km").arg(st.distanceTodayKm, 0, 'f', st.distanceTodayKm < 10 ? 1 : 0).arg(st.distanceTripKm, 0, 'f', 0);
    if (!st.awayText.isEmpty()) trip += QStringLiteral(" · 🏠 ") + st.awayText;
    m_tripAct->setText(trip);
    const SunTimes su = m_loc->sun();
    QString sunText;
    if (f.valid && f.hasElevation()) sunText = QStringLiteral("⛰ %1 m").arg(qRound(f.elevation));
    if (su.valid && su.sunrise.isValid()) sunText += (sunText.isEmpty() ? QString() : QStringLiteral(" · ")) + QStringLiteral("☀ %1 – %2").arg(su.sunrise.toString(QStringLiteral("HH:mm")), su.sunset.toString(QStringLiteral("HH:mm")));
    m_sunAct->setText(sunText.isEmpty() ? QStringLiteral("—") : sunText);
    m_sunAct->setVisible(!sunText.isEmpty());
    if (m_identityAct) m_identityAct->setText(m_loc->identity() && m_loc->identity()->exists() ? QStringLiteral("Identity: %1…").arg(m_loc->identity()->name()) : QStringLiteral("Create or import your identity…"));
    if (m_loc->os() && !m_loc->os()->lastZone().isEmpty() && !sunText.isEmpty()) m_sunAct->setText(sunText + QStringLiteral(" · 🕓 ") + m_loc->os()->lastZone());
    m_shareMenu->setEnabled(f.valid);
    m_refreshAct->setEnabled(!m_loc->busy());
    for (QAction *a : m_intervalMenu->actions()) a->setChecked(a->data().toInt() == m_loc->intervalMinutes());

    const QColor dot = f.source == QLatin1String("starlink") ? QColor(0x6c, 0xff, 0x8a)
                     : f.source == QLatin1String("wifi")     ? QColor(0x35, 0xd6, 0xff)
                     : f.source == QLatin1String("ip")       ? QColor(0xff, 0xd1, 0x66) : QColor(0xff, 0x4f, 0x4f);
    m_icon.setIcon(beaconIcon(dot, m_loc->busy()));
    QString peers;
    if (m_loc->apiServer() && m_loc->apiServer()->mdns()) {
        const int n = m_loc->apiServer()->mdns()->peers(false).size();
        if (n > 0) peers = QStringLiteral("\n📡 %1 other BeaconFix device%2 on this network").arg(n).arg(n == 1 ? QString() : QStringLiteral("s"));
    }
    m_icon.setToolTip(f.valid ? QStringLiteral("%1\n±%2 m · %3 · %4\n%5%6%7").arg(f.place).arg(qRound(f.accuracy)).arg(src, ageText(), trip, sunText.isEmpty() ? QString() : QStringLiteral("\n") + sunText, peers)
                              : QStringLiteral("BeaconFix — no location") + peers);
}
