#pragma once
#include <QDialog>
#include <QHash>
#include <QImage>
#include <QTimer>

class ApiServer;
class QLabel;
class QPushButton;
class QVBoxLayout;

// "Link a device" (docs/LINKING.md): the QR of a fresh link session (renewed every 10 min and after each use while
// open), this PC's name, a countdown and the live status — a QR scan links at once ("<phone> linked ✓ — code 123 456"),
// a phone that picked this PC from its mDNS list shows up as a prompt with the code and [Link] / [Reject]. Nothing typed.
class LinkDialog : public QDialog {
    Q_OBJECT
public:
    explicit LinkDialog(ApiServer *api, QWidget *parent = nullptr);
    ~LinkDialog() override;
    // A QR as an image: black modules on white, a 4-module quiet zone, scaled in whole pixels to about targetPx
    static QImage qrImage(const QString &text, int targetPx = 420);

protected:
    void showEvent(QShowEvent *e) override;   // a fresh QR, the timer runs
    void hideEvent(QHideEvent *e) override;   // closed, Esc or minimised away: that QR no longer links, no renewals

private:
    void renew();                 // cancel the shown QR, offer a fresh one
    void refresh();               // QR text (a hub invite may arrive later), requests, statuses
    void tick();                  // countdown; renew when due

    ApiServer *m_api;
    QString m_sid, m_qrText;
    QLabel *m_qr, *m_name, *m_hosts, *m_countdown, *m_status;
    QVBoxLayout *m_requests;
    QPushButton *m_copy;
    QHash<QString, QWidget *> m_requestRows;   // sid → its prompt
    QStringList m_done;                        // lines for the phones linked while the dialog was open
    QTimer m_timer;
};
