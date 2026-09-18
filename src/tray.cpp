#include "tray.h"
#include "locator.h"
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QUrl>

Tray::Tray(Locator *loc, QObject *parent) : QObject(parent), m_loc(loc)
{
    m_icon.setIcon(QIcon::fromTheme(QStringLiteral("mark-location"), QIcon::fromTheme(QStringLiteral("find-location"))));

    m_placeAct = m_menu.addAction(QString());
    QFont bold = m_placeAct->font(); bold.setBold(true); m_placeAct->setFont(bold);
    connect(m_placeAct, &QAction::triggered, this, &Tray::openWindowRequested);
    m_coordAct = m_menu.addAction(QString());
    connect(m_coordAct, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(QStringLiteral("%1, %2").arg(m_loc->fix().lat, 0, 'f', 6).arg(m_loc->fix().lon, 0, 'f', 6));
        m_icon.showMessage(QStringLiteral("BeaconFix"), QStringLiteral("Coordinates copied"), QSystemTrayIcon::Information, 2000);
    });
    m_ageAct = m_menu.addAction(QString()); m_ageAct->setEnabled(false);
    m_menu.addSeparator();
    m_refreshAct = m_menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Re-check location now"));
    connect(m_refreshAct, &QAction::triggered, m_loc, &Locator::Refresh);
    QAction *osm = m_menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("Open in OpenStreetMap"));
    connect(osm, &QAction::triggered, this, [this] {
        const Fix &f = m_loc->fix();
        if (f.valid) QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=15/%1/%2").arg(f.lat).arg(f.lon)));
    });
    QAction *open = m_menu.addAction(QIcon::fromTheme(QStringLiteral("window")), QStringLiteral("Open BeaconFix"));
    connect(open, &QAction::triggered, this, &Tray::openWindowRequested);

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
    connect(m_loc, &Locator::probeStarted, this, &Tray::rebuild);
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
                      : f.source == QLatin1String("wifi")     ? QStringLiteral("BeaconDB Wi-Fi")
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
    m_refreshAct->setEnabled(!m_loc->busy());
    for (QAction *a : m_intervalMenu->actions()) a->setChecked(a->data().toInt() == m_loc->intervalMinutes());

    const QString icon = f.source == QLatin1String("starlink") ? QStringLiteral("gps")
                       : f.source == QLatin1String("wifi")     ? QStringLiteral("mark-location")
                       : f.source == QLatin1String("ip")       ? QStringLiteral("network-server")
                       : QStringLiteral("dialog-warning");
    m_icon.setIcon(QIcon::fromTheme(icon, QIcon::fromTheme(QStringLiteral("mark-location"))));
    m_icon.setToolTip(f.valid ? QStringLiteral("%1\n±%2 m · %3 · %4").arg(f.place).arg(qRound(f.accuracy)).arg(src, ageText())
                              : QStringLiteral("BeaconFix — no location"));
}
