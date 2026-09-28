#pragma once
#include <QMainWindow>
#include <QJsonObject>
#include <QHash>

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
    void showApDetails(const QString &bssid);   // the graded estimate of one AP: score components, flags, R95 …
    QWidget *buildTrip();
    QWidget *buildDevices();
    void refreshDevices();
    QWidget *buildIdentity();
    void refreshIdentity();
public:
    void showIdentity();
    void showEmergency();
    void showPairRequest(const QString &id);   // the pairing dialog for one pending request (raised if already open)
    void importHistoryDialog();               // Settings → Map database → Import…
private:

    Locator *m_loc;
    TileSource *m_tiles = nullptr;
    BeaconView *m_map;
    QHash<QString, class PairDialog *> m_pairDialogs;
    class QComboBox *m_pairPolicy = nullptr;
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
    QTableWidget *m_peerTable = nullptr; QPushButton *m_peerLink = nullptr, *m_peerSync = nullptr, *m_peerScan = nullptr; QLabel *m_peerNote = nullptr;
    void refreshPeers();
    void linkWithPeer(const QJsonObject &peer);
    QLabel *m_emergency = nullptr;
};
