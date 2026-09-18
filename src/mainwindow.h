#pragma once
#include <QMainWindow>

class Locator;
class BeaconView;
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
    explicit MainWindow(Locator *loc, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    QWidget *buildSettings();
    void refreshFix();
    void refreshAps();
    void refreshHistory();
    void apContextMenu(const QPoint &pos);

    Locator *m_loc;
    BeaconView *m_map;
    QLabel *m_place, *m_coords, *m_meta, *m_status, *m_chip;
    QPushButton *m_refresh;
    QTableWidget *m_aps, *m_history;
    QSpinBox *m_interval, *m_threshold;
    QCheckBox *m_starlink, *m_ip, *m_ignoreActive;
    QLineEdit *m_starlinkHost, *m_wigle;
    QPlainTextEdit *m_ignore;
};
