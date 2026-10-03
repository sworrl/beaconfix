#include "linkdialog.h"
#include "apiserver.h"
#include "linking.h"
#include "third_party/qrcodegen.hpp"
#include <QApplication>
#include <QClipboard>
#include <QHideEvent>
#include <QShowEvent>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

QImage LinkDialog::qrImage(const QString &text, int targetPx)
{
    if (text.isEmpty()) return {};
    try {
        const QByteArray utf8 = text.toUtf8();
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(utf8.constData(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int n = qr.getSize(), quiet = 4, scale = qMax(2, targetPx / (n + 2 * quiet)), side = (n + 2 * quiet) * scale;
        QImage img(side, side, QImage::Format_RGB32);
        img.fill(Qt::white);
        QPainter p(&img);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (qr.getModule(x, y)) p.fillRect((x + quiet) * scale, (y + quiet) * scale, scale, scale, Qt::black);
        return img;
    } catch (const std::exception &) {
        return {};                                         // longer than a version-40 QR holds
    }
}

LinkDialog::LinkDialog(ApiServer *api, QWidget *parent) : QDialog(parent), m_api(api)
{
    setWindowTitle(QStringLiteral("Link a device"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *v = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral("On the phone open BeaconFix → <b>Link</b> and scan this code — or pick this PC in its list. "
                                            "Both screens then show the same six-digit code. Nothing to type."));
    intro->setWordWrap(true);
    v->addWidget(intro);
    m_name = new QLabel; m_name->setAlignment(Qt::AlignCenter);
    { QFont f = m_name->font(); f.setPointSizeF(f.pointSizeF() * 1.6); f.setBold(true); m_name->setFont(f); }
    v->addWidget(m_name);
    m_qr = new QLabel; m_qr->setAlignment(Qt::AlignCenter); m_qr->setMinimumSize(300, 300);
    m_qr->setStyleSheet(QStringLiteral("QLabel { background: white; color: black; }"));   // white around the quiet zone in a dark theme too
    v->addWidget(m_qr, 1);
    m_hosts = new QLabel; m_hosts->setAlignment(Qt::AlignCenter); m_hosts->setTextInteractionFlags(Qt::TextSelectableByMouse);
    { QFont f = m_hosts->font(); f.setPointSizeF(f.pointSizeF() * 0.9); m_hosts->setFont(f); }
    v->addWidget(m_hosts);
    m_countdown = new QLabel; m_countdown->setAlignment(Qt::AlignCenter);
    v->addWidget(m_countdown);
    m_requests = new QVBoxLayout;
    v->addLayout(m_requests);
    m_status = new QLabel; m_status->setWordWrap(true); m_status->setAlignment(Qt::AlignCenter); m_status->setTextFormat(Qt::RichText);
    { QFont f = m_status->font(); f.setPointSizeF(f.pointSizeF() * 1.15); m_status->setFont(f); }
    v->addWidget(m_status);
    auto *row = new QHBoxLayout;
    m_copy = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy link text"));
    m_copy->setToolTip(QStringLiteral("The QR's text (bflink:…), for a device that cannot scan"));
    connect(m_copy, &QPushButton::clicked, this, [this] { if (!m_qrText.isEmpty()) QApplication::clipboard()->setText(m_qrText); });
    auto *close = new QPushButton(QIcon::fromTheme(QStringLiteral("window-close")), QStringLiteral("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    row->addWidget(m_copy); row->addStretch(); row->addWidget(close);
    v->addLayout(row);

    connect(m_api, &ApiServer::linkChanged, this, &LinkDialog::refresh);
    connect(m_api, &ApiServer::linked, this, [this](const QString &, const QString &name, const QString &code) {
        m_done.prepend(QStringLiteral("<span style='color:#2e9d48'>✓</span> <b>%1</b> linked — code <b>%2</b>").arg(name.toHtmlEscaped(), Link::codeText(code)));
        while (m_done.size() > 4) m_done.removeLast();
        refresh();
    });
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &LinkDialog::tick);
    resize(580, 860);
}

LinkDialog::~LinkDialog()
{
    disconnect(m_api, nullptr, this, nullptr);
    if (!m_sid.isEmpty()) m_api->linkCancel(m_sid);
}

void LinkDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    if (m_sid.isEmpty()) renew();
    m_timer.start();
}

void LinkDialog::hideEvent(QHideEvent *e)
{
    QDialog::hideEvent(e);
    if (e->spontaneous()) return;                          // minimised / another desktop: the window is still "open"
    m_timer.stop();
    if (!m_sid.isEmpty()) m_api->linkCancel(m_sid);       // a QR on a closed dialog must not link anything
    m_sid.clear();
}

void LinkDialog::renew()
{
    if (!m_sid.isEmpty()) m_api->linkCancel(m_sid);
    m_sid.clear();
    m_sid = m_api->linkOffer();
    refresh();
}

void LinkDialog::tick()
{
    const qint64 left = m_api->linkExpires(m_sid) - QDateTime::currentSecsSinceEpoch();
    if (m_sid.isEmpty() || m_api->linkQr(m_sid).isEmpty() || left <= 3) { renew(); return; }   // used, expired or never made
    m_countdown->setText(QStringLiteral("This code changes in %1:%2 — a new one appears by itself").arg(left / 60).arg(left % 60, 2, 10, QLatin1Char('0')));
}

void LinkDialog::refresh()
{
    m_name->setText(m_api->linkName());
    const QString text = m_api->linkQr(m_sid);
    if (text != m_qrText || m_qr->pixmap().isNull()) {
        m_qrText = text;
        const qreal dpr = devicePixelRatioF();                // whole device pixels per module: crisp on HiDPI too
        const QImage img = qrImage(text, int(500 * dpr));
        if (img.isNull()) { m_qr->setPixmap(QPixmap()); m_qr->setText(m_api->listening() ? QStringLiteral("…") : QStringLiteral("The LAN API is not listening: %1").arg(m_api->error())); }
        else { QPixmap px = QPixmap::fromImage(img); px.setDevicePixelRatio(dpr); m_qr->setPixmap(px); }
    }
    m_copy->setEnabled(!m_qrText.isEmpty());
    m_hosts->setText(QStringLiteral("%1 · port %2").arg(m_api->linkHosts().join(QStringLiteral(" · "))).arg(m_api->boundPort()));

    // mDNS requests waiting for Link / Reject (rebuilt: they change seldom), and what is in flight
    for (QWidget *w : std::as_const(m_requestRows)) w->deleteLater();
    m_requestRows.clear();
    QStringList busy;
    for (const Link::Session &s : m_api->linkSessions()) {
        if (s.state == Link::Session::Approving || s.state == Link::Session::Approved)
            busy << QStringLiteral("Linking <b>%1</b> — code <b>%2</b>…").arg(s.name.toHtmlEscaped(), Link::codeText(s.code));
        if (s.origin != Link::Session::Mdns || s.state != Link::Session::Pending) continue;
        auto *box = new QFrame; box->setFrameShape(QFrame::StyledPanel);
        auto *bv = new QVBoxLayout(box);
        auto *who = new QLabel(QStringLiteral("<b>%1</b> wants to link<br><small>%2 · %3 · distance: %4 (information only)</small>")
                                   .arg(s.name.toHtmlEscaped(), s.kind.isEmpty() ? QStringLiteral("device") : s.kind.toHtmlEscaped(), s.ip,
                                        s.proximity["label"].toString(QStringLiteral("unknown"))));
        who->setWordWrap(true); bv->addWidget(who);
        auto *code = new QLabel(Link::codeText(s.code)); code->setAlignment(Qt::AlignCenter);
        { QFont f = code->font(); f.setPointSizeF(f.pointSizeF() * 2.6); f.setBold(true); f.setLetterSpacing(QFont::AbsoluteSpacing, 3); code->setFont(f); }
        bv->addWidget(code);
        bv->addWidget(new QLabel(QStringLiteral("Link only if the phone shows the same code.")));
        auto *btns = new QHBoxLayout;
        auto *link = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok")), QStringLiteral("Link"));
        auto *reject = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), QStringLiteral("Reject"));
        link->setDefault(true);
        const QString sid = s.sid;
        connect(link, &QPushButton::clicked, this, [this, sid] { m_api->linkApprove(sid); });
        connect(reject, &QPushButton::clicked, this, [this, sid] { m_api->linkReject(sid); });
        btns->addStretch(); btns->addWidget(reject); btns->addWidget(link);
        bv->addLayout(btns);
        m_requests->addWidget(box);
        m_requestRows.insert(sid, box);
    }
    QStringList lines = busy + m_done;
    m_status->setText(lines.isEmpty() ? QStringLiteral("Waiting for a phone…") : lines.join(QStringLiteral("<br>")));
}
