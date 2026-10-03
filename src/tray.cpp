#include "tray.h"
#include "locator.h"
#include "identity.h"
#include "apiserver.h"
#include "hubclient.h"
#include "mdns.h"
#include "osintegration.h"
#include "appicon.h"
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUrl>
#include <QPainter>

// The BeaconFix icon (appicon.h) with a status dot coloured by the fix source, dimmed while a check runs
static QIcon beaconIcon(const QColor &dot, bool busy)
{
    QIcon icon;
    for (int sz : {16, 22, 24, 32, 48, 64}) {
        QPixmap px(sz, sz); px.fill(Qt::transparent);
        QPainter p(&px); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setOpacity(busy ? 0.55 : 1.0);
        p.drawPixmap(0, 0, QPixmap(appIconPath(sz)));
        p.setOpacity(1.0);
        const double r = qMax(2.5, sz * 0.17), c = sz - r - qMax(0.5, sz / 32.0);
        p.setPen(QPen(QColor(11, 16, 26), qMax(1.0, sz / 20.0))); p.setBrush(dot); p.drawEllipse(QPointF(c, c), r, r);
        icon.addPixmap(px);
    }
    return icon;
}

Tray::Tray(Locator *loc, QObject *parent) : QObject(parent), m_loc(loc)
{
    QApplication::setWindowIcon(appIcon());
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
        if (m_loc->fix().valid) QDesktopServices::openUrl(QUrl(m_loc->osmUrl()));   // fixed 6 decimals: arg(double) alone can give "1e-05"
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
    QAction *emerg = m_menu.addAction(QIcon::fromTheme(QStringLiteral("dialog-warning")), QStringLiteral("Nearest help (police, fire, ER, pediatric ER)…"));
    connect(emerg, &QAction::triggered, this, &Tray::openEmergencyRequested);
    m_identityAct = m_menu.addAction(QIcon::fromTheme(QStringLiteral("user-identity")), QStringLiteral("Identity…"));
    connect(m_identityAct, &QAction::triggered, this, &Tray::openIdentityRequested);
    // The hub (docs/SECURE-API.md): connect with an invite, then the entry shows the link's state and syncs on click
    QAction *hubAct = m_menu.addAction(QIcon::fromTheme(QStringLiteral("network-server")), QStringLiteral("Connect to a hub…"));
    auto hubText = [this, hubAct] {
        HubClient *h = m_loc->hubClient();
        hubAct->setVisible(h != nullptr);
        if (!h) return;
        if (!h->enrolled()) { hubAct->setText(QStringLiteral("Connect to a hub…")); return; }
        const QJsonObject st = h->statusJson();
        const QDateTime last = QDateTime::fromString(st["lastSync"].toString(), Qt::ISODate);
        const QString host = QUrl(st["url"].toString()).host();
        if (!st["lastError"].toString().isEmpty() && !st["lastOk"].toBool()) hubAct->setText(QStringLiteral("Hub %1: offline, retrying (sync now)").arg(host));
        else if (last.isValid()) hubAct->setText(QStringLiteral("Hub %1: synced %2 (sync now)").arg(host, last.secsTo(QDateTime::currentDateTime()) < 90 ? QStringLiteral("just now") : QStringLiteral("%1 min ago").arg(last.secsTo(QDateTime::currentDateTime()) / 60)));
        else hubAct->setText(QStringLiteral("Hub %1: connecting… (sync now)").arg(host));
        hubAct->setToolTip(st["lastError"].toString().isEmpty() ? st["lastResult"].toString() : st["lastError"].toString());
    };
    connect(hubAct, &QAction::triggered, this, [this] {
        HubClient *h = m_loc->hubClient();
        if (!h) return;
        if (h->enrolled()) {
            h->syncNow([this](bool ok, const QString &msg) { m_icon.showMessage(QStringLiteral("BeaconFix hub"), (ok ? QStringLiteral("Synced: ") : QStringLiteral("Sync failed: ")) + msg, ok ? QSystemTrayIcon::Information : QSystemTrayIcon::Warning, 5000); });
            return;
        }
        bool ok = false;
        const QString inv = QInputDialog::getMultiLineText(nullptr, QStringLiteral("Connect to a BeaconFix hub"),
                                                           QStringLiteral("Paste the invite (bfs3:…) printed by\n  beaconfix --server --invite \"%1\"\non the hub:").arg(Locator::deviceName()), QString(), &ok);
        if (!ok || inv.trimmed().isEmpty()) return;
        h->enroll(inv.trimmed(), QString(), [this](bool ok, const QJsonObject &o) {
            m_icon.showMessage(QStringLiteral("BeaconFix hub"), ok ? QStringLiteral("Enrolled with %1 (hub key %2). Syncing now.").arg(o["url"].toString(), o["fingerprint"].toString())
                                                                   : QStringLiteral("Not enrolled: ") + o["error"].toString(), ok ? QSystemTrayIcon::Information : QSystemTrayIcon::Warning, 8000);
        });
    });
    if (HubClient *h = m_loc->hubClient()) connect(h, &HubClient::changed, this, hubText);
    connect(&m_ageTimer, &QTimer::timeout, this, hubText);
    hubText();
    QAction *trip = m_menu.addAction(QIcon::fromTheme(QStringLiteral("flag")), QStringLiteral("Start a new trip here"));
    connect(trip, &QAction::triggered, m_loc, &Locator::StartTrip);
    QAction *link = m_menu.addAction(QIcon::fromTheme(QStringLiteral("insert-link")), QStringLiteral("Link a device…"));   // QR / mDNS, nothing typed (docs/LINKING.md)
    connect(link, &QAction::triggered, this, &Tray::openLinkRequested);

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
