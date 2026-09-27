#include "anchordialog.h"
#include "locator.h"
#include "ranging/rangingservice.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

static const char *const kKindLabels[][2] = {
    {"this-computer", "This computer's antenna (the RTT responder / BLE advertiser)"},
    {"wifi-ap", "A Wi-Fi access point (router, mesh node)"},
    {"rtt-responder", "Another Wi-Fi RTT responder"},
    {"ble", "A BLE beacon"},
    {"gnss", "A GNSS receiver (e.g. the Pi with the GPS HAT)"},
    {"custom", "Something else / a reference point"},
};

AnchorDialog::AnchorDialog(Locator *loc, double lat, double lon, const QJsonObject &existing, QWidget *parent)
    : QDialog(parent), m_loc(loc), m_base(existing), m_lat(lat), m_lon(lon)
{
    setWindowTitle(existing.isEmpty() ? QStringLiteral("Place an anchor") : QStringLiteral("Edit anchor"));
    auto *form = new QFormLayout;
    m_name = new QLineEdit(existing["name"].toString());
    m_name->setPlaceholderText(QStringLiteral("Wi-Fi antenna, living-room router…"));
    form->addRow(QStringLiteral("Name"), m_name);
    m_kind = new QComboBox;
    for (const auto &k : kKindLabels) m_kind->addItem(QString::fromUtf8(k[1]), QString::fromLatin1(k[0]));
    const int ki = m_kind->findData(existing["kind"].toString(QStringLiteral("this-computer")));
    m_kind->setCurrentIndex(ki >= 0 ? ki : 0);
    form->addRow(QStringLiteral("What is there"), m_kind);
    auto *pos = new QHBoxLayout;
    m_lat_e = new QLineEdit(QString::number(existing.contains("lat") ? existing["lat"].toDouble() : lat, 'f', 7));
    m_lon_e = new QLineEdit(QString::number(existing.contains("lon") ? existing["lon"].toDouble() : lon, 'f', 7));
    pos->addWidget(m_lat_e); pos->addWidget(m_lon_e);
    form->addRow(QStringLiteral("Latitude, longitude"), pos);
    m_acc = new QDoubleSpinBox; m_acc->setRange(0.05, 500); m_acc->setDecimals(2); m_acc->setSuffix(QStringLiteral(" m"));
    m_acc->setValue(existing["accM"].toDouble(1.0));
    m_acc->setToolTip(QStringLiteral("How well you know where it is (1 σ). A careful map pick at full zoom is about 1 m."));
    form->addRow(QStringLiteral("Placement accuracy"), m_acc);
    auto *h = new QHBoxLayout;
    m_hasHeight = new QCheckBox(QStringLiteral("known")); m_height = new QDoubleSpinBox; m_height->setRange(-10, 300); m_height->setDecimals(2); m_height->setSuffix(QStringLiteral(" m"));
    m_hasHeight->setChecked(existing["heightM"].isDouble()); m_height->setValue(existing["heightM"].toDouble(1.0)); m_height->setEnabled(m_hasHeight->isChecked());
    connect(m_hasHeight, &QCheckBox::toggled, m_height, &QWidget::setEnabled);
    h->addWidget(m_hasHeight); h->addWidget(m_height, 1);
    form->addRow(QStringLiteral("Height above the floor"), h);
    QStringList b; for (const QJsonValue &v : existing["bssids"].toArray()) b << v.toString();
    m_bssids = new QLineEdit(b.join(QStringLiteral(", ")));
    m_bssids->setPlaceholderText(QStringLiteral("AA:BB:CC:DD:EE:FF, … (every BSSID this transmitter uses)"));
    form->addRow(QStringLiteral("BSSIDs"), m_bssids);
    m_heard = new QComboBox;
    m_heard->addItem(QStringLiteral("Add one I hear now…"), QString());
    QList<AccessPoint> aps = loc->accessPoints();
    std::sort(aps.begin(), aps.end(), [](const AccessPoint &x, const AccessPoint &y) { return x.dbm > y.dbm; });
    for (const AccessPoint &ap : aps.mid(0, 40))
        m_heard->addItem(QStringLiteral("%1  %2  %3 dBm  %4 MHz").arg(ap.ssid.isEmpty() ? QStringLiteral("(hidden)") : ap.ssid, ap.bssid.toUpper()).arg(ap.dbm).arg(ap.frequency), ap.bssid.toUpper());
    connect(m_heard, &QComboBox::activated, this, [this](int i) {
        const QString v = m_heard->itemData(i).toString();
        if (v.isEmpty()) return;
        QStringList l = m_bssids->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (QString &x : l) x = x.trimmed();
        if (!l.contains(v)) l << v;
        m_bssids->setText(l.join(QStringLiteral(", ")));
        m_heard->setCurrentIndex(0);
    });
    form->addRow(QString(), m_heard);
    m_rv = new QCheckBox(QStringLiteral("Moves with the RV (re-projected after every move)"));
    m_rv->setChecked(existing.isEmpty() ? true : existing["rv"].toBool());
    form->addRow(QString(), m_rv);
    m_ref = new QCheckBox(QStringLiteral("RV reference point (the others follow it)"));
    m_ref->setChecked(existing["ref"].toBool());
    form->addRow(QString(), m_ref);
    m_hint = new QLabel; m_hint->setWordWrap(true); m_hint->setStyleSheet(QStringLiteral("color: palette(mid);"));
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Ok)->setText(existing.isEmpty() ? QStringLiteral("Place") : QStringLiteral("Save"));
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *lay = new QVBoxLayout(this);
    lay->addLayout(form); lay->addWidget(m_hint); lay->addWidget(box);
    connect(m_kind, &QComboBox::currentIndexChanged, this, &AnchorDialog::kindChanged);
    kindChanged();
    resize(560, sizeHint().height());
}

void AnchorDialog::kindChanged()
{
    const QString k = m_kind->currentData().toString();
    if (k == QLatin1String("this-computer")) {
        const QString resp = RangingService::responderInfo().value(QStringLiteral("bssid")).toString().toUpper();
        if (!resp.isEmpty() && !m_bssids->text().contains(resp, Qt::CaseInsensitive))
            m_bssids->setText(m_bssids->text().trimmed().isEmpty() ? resp : m_bssids->text() + QStringLiteral(", ") + resp);
        m_hint->setText(QStringLiteral("While BeaconFix is within 100 m of this spot, the spot IS its position (±accuracy), and it is where the "
                                       "phone's RTT and BLE ranges are measured from. Put it where the Wi-Fi/Bluetooth antenna is."));
        if (m_base.isEmpty()) m_ref->setChecked(true);
    } else if (k == QLatin1String("wifi-ap")) {
        m_hint->setText(QStringLiteral("Its BSSIDs are pinned here: never re-estimated, used as known transmitters for self-location, and "
                                       "BeaconFix learns this place's radio environment (path-loss P0 / n) from how loud it is at a known distance."));
    } else if (k == QLatin1String("gnss")) {
        m_hint->setText(QStringLiteral("A GNSS receiver in the RV: with it placed, the Pi's averaged fix gives this computer's position to a few metres (the rv-gnss tier)."));
    } else {
        m_hint->setText(QString());
    }
}

QJsonObject AnchorDialog::anchor() const
{
    QJsonObject o = m_base;
    o["name"] = m_name->text().trimmed();
    o["kind"] = m_kind->currentData().toString();
    const double lat = m_lat_e->text().toDouble(), lon = m_lon_e->text().toDouble();
    const bool moved = !m_base.contains("lat") || std::fabs(m_base["lat"].toDouble() - lat) > 1e-9 || std::fabs(m_base["lon"].toDouble() - lon) > 1e-9;
    o["lat"] = lat; o["lon"] = lon;
    o["accM"] = m_acc->value();
    if (m_hasHeight->isChecked()) o["heightM"] = m_height->value(); else o.remove(QStringLiteral("heightM"));
    QJsonArray b;
    for (const QString &x : m_bssids->text().split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts)) b.append(x.trimmed());
    o["bssids"] = b;
    o["rv"] = m_rv->isChecked();
    o["ref"] = m_ref->isChecked();
    if (moved || !o["rv"].toBool()) o.remove(QStringLiteral("rvOffset"));   // re-measured against the RV reference on save
    o.remove(QStringLiteral("seq")); o.remove(QStringLiteral("headingAssumed"));
    o["placedBy"] = QStringLiteral("desktop");
    o["placedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    o["source"] = QStringLiteral("map-pick");
    return o;
}
