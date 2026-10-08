#pragma once
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>

class Locator;

class Tray : public QObject {
    Q_OBJECT
public:
    explicit Tray(Locator *loc, QObject *parent = nullptr);

signals:
    void openWindowRequested();
    void openIdentityRequested();
    void openLinkRequested();
    void openEmergencyRequested();
    void quitRequested();

private:
    void rebuild();
    QString ageText() const;

    Locator *m_loc;
    QSystemTrayIcon m_icon;
    QMenu m_menu;
    QAction *m_placeAct = nullptr, *m_coordAct = nullptr, *m_ageAct = nullptr, *m_refreshAct = nullptr;
    QMenu *m_intervalMenu = nullptr, *m_shareMenu = nullptr, *m_meshMenu = nullptr;
    QAction *m_tripAct = nullptr, *m_sunAct = nullptr, *m_identityAct = nullptr;
    QTimer m_ageTimer;
};
