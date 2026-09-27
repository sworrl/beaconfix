#pragma once
#include <QMainWindow>

class Locator;
class BeaconView;
class TileSource;
class QLabel;
class QTableWidget;
class QPushButton;
class QSpinBox;
class QCheckBox;
class QLineEdit;
class QPlainTextEdit;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Locator *loc, TileSource *tiles, QWidget *parent = nullptr);
    BeaconView *map() const { return m_map; }

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    QWidget *buildSettings();
    void refreshFix();
    void refreshAps();
    void refreshHistory();
    void refreshPois();
    void refreshTrip();
    void apContextMenu(const QPoint &pos);
    QWidget *buildTrip();
    QWidget *buildDevices();
    void refreshDevices();
    QWidget *buildIdentity();
    void refreshIdentity();
public:
    void showIdentity();
    void showEmergency();
private:

    Locator *m_loc;
    BeaconView *m_map;
    QLabel *m_place, *m_coords, *m_meta, *m_status, *m_chip;
    QPushButton *m_refresh;
    QTableWidget *m_aps, *m_history, *m_pois;
    QLineEdit *m_poiFilter;
    QLabel *m_poiNote;
    class QTabWidget *m_tabs;
    QSpinBox *m_interval, *m_threshold, *m_liveScan;
    QCheckBox *m_names;
    QCheckBox *m_starlink, *m_ip, *m_apple, *m_ignoreActive, *m_notifyStops, *m_notifyRegions, *m_notifyAch, *m_prefetch, *m_elev;
    QLineEdit *m_starlinkHost, *m_wigle;
    QPlainTextEdit *m_ignore, *m_home;
    QLabel *m_tripSummary, *m_tripPlaces, *m_tripRecords;
    QTableWidget *m_stops;
    class QListWidget *m_achList;
    // Devices (LAN API)
    QCheckBox *m_apiEnabled; QSpinBox *m_apiPort; QLabel *m_apiStatus, *m_pairLabel; QPushButton *m_pairBtn;
    QTableWidget *m_pendingTable, *m_devTable, *m_knownTable; QPlainTextEdit *m_accessLog; class QTimer *m_devTimer; QCheckBox *m_knownOnly;
    QTableWidget *m_linkTable = nullptr;
    // System (OS integration)
    QCheckBox *m_osTz, *m_osGeo, *m_osNight, *m_osLocale; QLabel *m_osStatus;
    // Identity
    QWidget *m_identityTab = nullptr; QLabel *m_idSummary, *m_idQr; QPushButton *m_idCreate, *m_idImport, *m_idExport, *m_idLink, *m_idForget; QPlainTextEdit *m_idDetails;
    QLabel *m_emergency = nullptr;
};
