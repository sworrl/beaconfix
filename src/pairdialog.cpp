#include "pairdialog.h"
#include "apiserver.h"
#include "locator.h"
#include "pairing.h"
#include "tilesource.h"
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

// ── MiniMap ───────────────────────────────────────────────────────────────────
static QPointF mercOf(double lat, double lon)
{
    const double la = qBound(-85.0, lat, 85.0) * M_PI / 180.0;
    return {(lon + 180.0) / 360.0, (1.0 - std::log(std::tan(la) + 1.0 / std::cos(la)) / M_PI) / 2.0};
}

MiniMap::MiniMap(TileSource *tiles, QWidget *parent) : QWidget(parent), m_tiles(tiles)
{
    setMinimumSize(360, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void MiniMap::setPoints(bool haveThem, double tLat, double tLon, double tAcc, bool haveUs, double uLat, double uLon, double uAcc)
{
    m_them = haveThem; m_tLat = tLat; m_tLon = tLon; m_tAcc = tAcc;
    m_us = haveUs; m_uLat = uLat; m_uLon = uLon; m_uAcc = uAcc;
    if (m_them && m_us) {
        const QPointF a = mercOf(tLat, tLon), b = mercOf(uLat, uLon);
        m_center = (a + b) / 2;
        const double d = Locator::distanceM(tLat, tLon, uLat, uLon);
        const double span = std::max(d, std::max(tAcc, uAcc) * 2) * 2.4 + 60;    // metres across the view
        const double mpp0 = 156543.03 * std::cos(uLat * M_PI / 180.0);            // m/px at zoom 0
        m_zoom = qBound(3, int(std::floor(std::log2(mpp0 * std::max(200, width()) / span))), 18);
    } else if (m_us || m_them) {
        m_center = mercOf(m_us ? uLat : tLat, m_us ? uLon : tLon);
        m_zoom = 15;
    }
    update();
}

QPointF MiniMap::toScreen(double lat, double lon) const
{
    const double ws = 256.0 * std::pow(2.0, m_zoom);
    const QPointF m = mercOf(lat, lon);
    return {width() / 2.0 + (m.x() - m_center.x()) * ws, height() / 2.0 + (m.y() - m_center.y()) * ws};
}

void MiniMap::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0x0b, 0x10, 0x1a));
    if (!m_us && !m_them) { p.setPen(QColor(0x9f, 0xb0, 0xc8)); p.drawText(rect(), Qt::AlignCenter, QStringLiteral("no position on either side")); return; }
    const double ws = 256.0 * std::pow(2.0, m_zoom), n = std::pow(2.0, m_zoom);
    const double x0 = m_center.x() * ws - width() / 2.0, y0 = m_center.y() * ws - height() / 2.0;
    const int tx0 = int(std::floor(x0 / 256)), ty0 = int(std::floor(y0 / 256)), tx1 = int(std::floor((x0 + width()) / 256)), ty1 = int(std::floor((y0 + height()) / 256));
    for (int ty = ty0; ty <= ty1; ++ty) {
        if (ty < 0 || ty >= n) continue;
        for (int tx = tx0; tx <= tx1; ++tx) {
            const int wx = ((tx % int(n)) + int(n)) % int(n);
            const QString key = QStringLiteral("%1/%2/%3").arg(m_zoom).arg(wx).arg(ty);
            const QPointF at(tx * 256 - x0, ty * 256 - y0);
            const auto it = m_cache.constFind(key);
            if (it != m_cache.constEnd()) { if (!it->isNull()) p.drawImage(at, *it); continue; }
            m_cache.insert(key, QImage());
            if (m_tiles) m_tiles->get(TileSource::Dark, m_zoom, wx, ty, this, [this, key](const QImage &img) { m_cache.insert(key, img); update(); });
        }
    }
    p.setRenderHint(QPainter::Antialiasing);
    const double mpp = 156543.03 * std::cos((m_us ? m_uLat : m_tLat) * M_PI / 180.0) / std::pow(2.0, m_zoom);
    auto marker = [&](double lat, double lon, double acc, const QColor &c, const QString &label) {
        const QPointF s = toScreen(lat, lon);
        if (acc > 0) { QColor f = c; f.setAlphaF(0.12); p.setPen(QPen(c, 1, Qt::DashLine)); p.setBrush(f); p.drawEllipse(s, acc / mpp, acc / mpp); }
        p.setPen(QPen(Qt::white, 1.5)); p.setBrush(c); p.drawEllipse(s, 6, 6);
        p.setPen(Qt::white); QFont f = font(); f.setBold(true); p.setFont(f);
        p.drawText(QRectF(s.x() - 80, s.y() + 8, 160, 16), Qt::AlignCenter, label);
    };
    if (m_us && m_them) {
        const QPointF a = toScreen(m_tLat, m_tLon), b = toScreen(m_uLat, m_uLon);
        p.setPen(QPen(QColor(0xff, 0xd1, 0x66), 1.5, Qt::DashLine)); p.drawLine(a, b);
        const double d = Locator::distanceM(m_tLat, m_tLon, m_uLat, m_uLon);
        p.setPen(QColor(0xff, 0xd1, 0x66)); p.drawText(QRectF((a + b) / 2 - QPointF(60, 20), QSizeF(120, 16)), Qt::AlignCenter, d < 1000 ? QStringLiteral("%1 m").arg(qRound(d)) : QStringLiteral("%1 km").arg(d / 1000, 0, 'f', 1));
    }
    if (m_us) marker(m_uLat, m_uLon, m_uAcc, QColor(0x35, 0xd6, 0xff), QStringLiteral("you"));
    if (m_them) marker(m_tLat, m_tLon, m_tAcc, QColor(0xff, 0x4f, 0xd8), QStringLiteral("the device"));
    p.setPen(QColor(0x9f, 0xb0, 0xc8, 160)); p.setFont(font()); p.drawText(rect().adjusted(6, 0, -6, -4), Qt::AlignBottom | Qt::AlignRight, QStringLiteral("© OpenStreetMap"));
}

// ── PairDialog ────────────────────────────────────────────────────────────────
PairDialog::PairDialog(ApiServer *api, Locator *loc, TileSource *tiles, const QString &pendingId, QWidget *parent)
    : QDialog(parent), m_api(api), m_loc(loc), m_tiles(tiles), m_id(pendingId)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Pair a device — BeaconFix"));
    setMinimumWidth(640);
    auto *v = new QVBoxLayout(this);
    m_title = new QLabel; { QFont f = m_title->font(); f.setPointSizeF(f.pointSizeF() * 1.4); f.setBold(true); m_title->setFont(f); } v->addWidget(m_title);
    m_meta = new QLabel; m_meta->setWordWrap(true); m_meta->setTextInteractionFlags(Qt::TextSelectableByMouse); v->addWidget(m_meta);
    auto *row = new QHBoxLayout;
    m_verdict = new QLabel; m_verdict->setMinimumWidth(170); m_verdict->setAlignment(Qt::AlignCenter);
    m_beacons = new QLabel; m_beacons->setWordWrap(true);
    row->addWidget(m_verdict); row->addWidget(m_beacons, 1);
    v->addLayout(row);
    m_map = new MiniMap(tiles); v->addWidget(m_map, 1);
    m_hint = new QLabel; m_hint->setWordWrap(true); v->addWidget(m_hint);
    m_triples = new QGridLayout; m_triples->setSpacing(10); v->addLayout(m_triples);
    m_result = new QLabel; m_result->setWordWrap(true); m_result->setAlignment(Qt::AlignCenter); v->addWidget(m_result);
    auto *bottom = new QHBoxLayout;
    m_codeBtn = new QPushButton(QStringLiteral("Use the code instead…")); m_code = new QLabel; m_code->hide();
    m_confirm = new QLineEdit; m_confirm->setPlaceholderText(QStringLiteral("type  pair  to confirm")); m_confirm->setMaximumWidth(180); m_confirm->hide();
    m_anywayBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-warning")), QStringLiteral("Pair anyway (not recommended)")); m_anywayBtn->hide();
    m_denyBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), QStringLiteral("Deny"));
    m_closeBtn = new QPushButton(QStringLiteral("Close")); m_closeBtn->hide();
    m_controlBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("security-medium")), QStringLiteral("Allow control too")); m_controlBtn->hide();
    m_controlBtn->setToolTip(QStringLiteral("Adds the control scope to the token this request produced (needed for sync / db writes). The device keeps its token."));
    bottom->addWidget(m_codeBtn); bottom->addWidget(m_code); bottom->addStretch(); bottom->addWidget(m_confirm); bottom->addWidget(m_anywayBtn); bottom->addWidget(m_controlBtn); bottom->addWidget(m_denyBtn); bottom->addWidget(m_closeBtn);
    v->addLayout(bottom);
    connect(m_denyBtn, &QPushButton::clicked, this, &PairDialog::deny);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(m_controlBtn, &QPushButton::clicked, this, [this] {
        if (m_api->grantControl(m_id)) { m_result->setText(QStringLiteral("<b style='color:#2ecc71'>Control granted.</b> The device's token now carries read + control.")); m_controlBtn->hide(); }
        else m_result->setText(QStringLiteral("<b style='color:#e74c3c'>Could not upgrade this token</b> (request expired or the token was revoked)."));
        refresh();
    });
    connect(m_anywayBtn, &QPushButton::clicked, this, &PairDialog::pairAnyway);
    connect(m_codeBtn, &QPushButton::clicked, this, [this] {
        m_code->setText(QStringLiteral("Code on the device: <b style='font-size:18pt'>%1</b> — approve it in Devices if the codes match").arg(m_detail["code"].toString()));
        m_code->show(); m_codeBtn->hide();
    });
    connect(m_api, &ApiServer::changed, this, &PairDialog::refresh);
    refresh();
}

QString PairDialog::verdictColor(const QString &v) const
{
    return v == QLatin1String("adjacent") ? QStringLiteral("#2ecc71") : v == QLatin1String("room") ? QStringLiteral("#27ae60") : v == QLatin1String("near") ? QStringLiteral("#f5a623") : v == QLatin1String("far") ? QStringLiteral("#e74c3c") : QStringLiteral("#7f8c8d");
}

void PairDialog::refresh()
{
    m_detail = m_api->pendingDetail(m_id);
    if (m_detail.isEmpty()) {
        if (!m_done) { m_result->setText(QStringLiteral("This request has expired or was withdrawn.")); m_done = true; }
        for (QPushButton *b : m_tripleBtns) b->setEnabled(false);
        m_denyBtn->hide(); m_anywayBtn->hide(); m_confirm->hide(); m_closeBtn->show();
        return;
    }
    const QString name = m_detail["name"].toString(), kind = m_detail["kind"].toString(), ip = m_detail["ip"].toString(), status = m_detail["status"].toString();
    const QJsonObject idn = m_detail["identity"].toObject();
    const Pairing::Proximity px = Pairing::Proximity::fromJson(m_detail["proximity"].toObject());
    m_title->setText(QStringLiteral("Pair %1?").arg(name.toHtmlEscaped()));
    QString meta = QStringLiteral("%1 · %2 · asks for %3").arg(kind.isEmpty() ? QStringLiteral("device") : kind, ip, m_detail["scopes"].toString());
    if (!idn["id"].toString().isEmpty()) meta += QStringLiteral(" · identity <b>%1</b> (%2)%3").arg(idn["name"].toString().toHtmlEscaped(), idn["id"].toString().left(8), idn["known"].toBool() ? QStringLiteral(" — yours") : QString());
    if (m_detail["knownDevice"].toBool()) meta += QStringLiteral(" · <b>one of your known devices</b>");
    m_meta->setText(meta);
    m_verdict->setText(QStringLiteral("<span style='color:white;background:%1;padding:6px 10px;border-radius:6px'><b>%2</b></span>").arg(verdictColor(px.verdict), Pairing::verdictLabel(px.verdict)));
    m_verdict->setTextFormat(Qt::RichText);
    QString bt;
    if (px.theirs == 0 && px.ours == 0) bt = QStringLiteral("Neither side reported any Wi-Fi beacons.");
    else {
        bt = QStringLiteral("Hears <b>%1 of the %2</b> beacons you hear (it reports %3).").arg(px.shared).arg(px.ours).arg(px.theirs);
        if (!px.strongestShared.isEmpty()) bt += QStringLiteral(" Strongest shared: %1.").arg(px.strongestShared.join(QStringLiteral(", ")).toHtmlEscaped());
        if (px.rssiDelta >= 0) bt += QStringLiteral(" After removing the %1 dB radio offset, the shared levels differ by %2 dB (median).").arg(qRound(std::fabs(px.gainOffsetDb))).arg(qRound(px.rssiDelta));
    }
    if (px.distanceM >= 0) bt += QStringLiteral(" Fixes are %1 apart.").arg(px.distanceM < 1000 ? QStringLiteral("%1 m").arg(qRound(px.distanceM)) : QStringLiteral("%1 km").arg(px.distanceM / 1000, 0, 'f', 1));
    m_beacons->setText(bt);
    const QJsonObject tp = m_detail["theirPosition"].toObject();
    const Fix &us = m_loc->fix();
    m_map->setPoints(tp["lat"].isDouble(), tp["lat"].toDouble(), tp["lon"].toDouble(), tp["acc"].toDouble(), us.valid, us.lat, us.lon, us.accuracy);

    // Picture triples
    while (QLayoutItem *it = m_triples->takeAt(0)) { delete it->widget(); delete it; }
    m_tripleBtns.clear();
    const QJsonArray triples = m_detail["triples"].toArray();
    const bool waiting = status == QLatin1String("pending");
    const QString policy = m_api->pairPolicy();
    const bool allowed = Pairing::verdictAllowed(px.verdict, policy);
    for (int i = 0; i < triples.size(); ++i) {
        QString glyphs, names;
        for (const QJsonValue &v : triples[i].toArray()) { glyphs += v.toObject()["glyph"].toString() + QStringLiteral("  "); names += v.toObject()["name"].toString() + QStringLiteral(" "); }
        auto *b = new QPushButton(glyphs.trimmed());
        { QFont f = b->font(); f.setPointSizeF(f.pointSizeF() * 2.2); b->setFont(f); }
        b->setMinimumHeight(64); b->setToolTip(names.trimmed());
        b->setEnabled(waiting && allowed);
        connect(b, &QPushButton::clicked, this, [this, i] { pick(i); });
        m_triples->addWidget(b, 0, i);
        m_tripleBtns << b;
    }
    if (waiting) {
        if (allowed) m_hint->setText(QStringLiteral("<b>Tap the three pictures the device is showing.</b> A wrong pick denies the request — that is the point: only someone who sees both screens can pair."));
        else m_hint->setText(QStringLiteral("<span style='color:%1'><b>%2.</b></span> The pairing policy (Devices tab) only allows devices that are adjacent or near: %3")
                                 .arg(verdictColor(px.verdict), Pairing::verdictLabel(px.verdict),
                                      px.verdict == QLatin1String("far") ? QStringLiteral("this one hears different beacons / sits far from you.") : QStringLiteral("this one sent no usable beacons or position, so its distance is unknown.")));
        m_anywayBtn->setVisible(!allowed); m_confirm->setVisible(!allowed);
        m_denyBtn->show(); m_closeBtn->hide();
        if (policy == QLatin1String("warn") && (px.verdict == QLatin1String("far") || px.verdict == QLatin1String("unknown")))
            m_hint->setText(m_hint->text() + QStringLiteral("<br><span style='color:%1'>Warning: %2 — make sure this is your device.</span>").arg(verdictColor(px.verdict), Pairing::verdictLabel(px.verdict)));
    } else if (status == QLatin1String("approved") && m_detail["autoApproved"].toBool()) {
        // Known device next to us: approved without pictures. Say so (the phone jumps straight to "paired" and
        // never shows its pictures) and offer the control scope, which known-device auto-approval grants only when configured.
        for (QPushButton *b : m_tripleBtns) b->hide();
        const bool ctl = m_detail["scopes"].toString().contains(QLatin1String("control"));
        const bool linked = idn["known"].toBool();
        m_hint->setText(QStringLiteral("<b style='color:#2ecc71'>Auto-approved (%1).</b> %2 is one of your known devices and is %3, so no pictures were needed.%4")
                            .arg(ctl ? QStringLiteral("read + control") : QStringLiteral("read access"), name.toHtmlEscaped(), Pairing::verdictLabel(px.verdict).toLower(),
                                 ctl ? QString() : linked ? QStringLiteral("<br>Its identity is linked to yours: signing in with the identity already yields a control token, so nothing more is needed for sync.")
                                                          : QStringLiteral("<br>Read access cannot push samples (/db/sync needs control). Click <b>Allow control too</b> to upgrade the token it just received, or link its identity to yours (Identity tab) and let it sign in with that.")));
        m_result->clear();
        m_controlBtn->setVisible(!ctl); m_denyBtn->hide(); m_anywayBtn->hide(); m_confirm->hide(); m_codeBtn->hide(); m_closeBtn->show();
        m_done = true;                                       // no auto-close: the user may want to click "Allow control too"
    } else {
        for (QPushButton *b : m_tripleBtns) b->setEnabled(false);
        m_hint->clear();
        m_result->setText(status == QLatin1String("approved") ? QStringLiteral("<b style='color:#2ecc71'>Approved.</b> The device is fetching its token.")
                          : status == QLatin1String("denied") ? QStringLiteral("<b style='color:#e74c3c'>Denied.</b>") + (m_detail["wrongPick"].toBool() ? QStringLiteral(" Wrong pictures — someone else may have been pairing.") : QString())
                          : QStringLiteral("Request %1.").arg(status));
        m_denyBtn->hide(); m_anywayBtn->hide(); m_confirm->hide(); m_closeBtn->show();
        m_done = true;
        QTimer::singleShot(6000, this, &QDialog::close);
    }
}

void PairDialog::pick(int tripleIndex)
{
    const bool ok = m_api->approveByPick(m_id, tripleIndex);
    m_result->setText(ok ? QStringLiteral("<b style='color:#2ecc71'>Matched — approved.</b>") : QStringLiteral("<b style='color:#e74c3c'>Wrong pictures — request denied.</b>"));
    refresh();
}

void PairDialog::deny() { m_api->deny(m_id); refresh(); }

void PairDialog::pairAnyway()
{
    if (m_confirm->text().trimmed().compare(QStringLiteral("pair"), Qt::CaseInsensitive) != 0) { QMessageBox::warning(this, QStringLiteral("Pair anyway"), QStringLiteral("Type  pair  in the box to confirm you understand this device is not next to you.")); return; }
    const QJsonArray triples = m_detail["triples"].toArray();
    // Override = still requires the picture match, only the proximity gate is lifted
    for (QPushButton *b : m_tripleBtns) b->setEnabled(true);
    m_hint->setText(QStringLiteral("<b>Proximity gate lifted for this request.</b> Now tap the three pictures the device is showing."));
    m_anywayBtn->hide(); m_confirm->hide();
    m_api->liftProximity(m_id);
}
