// SPDX-License-Identifier: Apache-2.0
#include "sightingsview.h"
#include "imagestore.h"
#include "locator.h"
#include "mapdb.h"
#include "platewatch.h"
#include "eyesonflock.h"
#include <QCheckBox>
#include <QMenu>
#include <QMessageBox>
#include <QComboBox>
#include <QSettings>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>

static QString fmtTime(const QString &iso)
{
    const QDateTime t = QDateTime::fromString(iso, Qt::ISODate);
    return t.isValid() ? t.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : iso;
}

static QString kindLabel(const QJsonObject &e)
{
    if (e.value(QLatin1String("kind")).toString() == QLatin1String("plate_search")) return QStringLiteral("Plate search");
    const QString t = e.value(QLatin1String("camera_type")).toString();
    if (t == QLatin1String("alpr")) return QStringLiteral("ALPR pass");
    QString l = PlateEvents::typeLabel(t);
    l[0] = l[0].toUpper();
    return l;
}

static QString facingText(const QJsonValue &f)
{
    if (f.isNull() || f.isUndefined()) return QStringLiteral("unknown");
    return f.toInt() == 1 ? QStringLiteral("faced you") : QStringLiteral("faced away");
}

SightingsView::SightingsView(Locator *loc, QWidget *parent) : QWidget(parent), m_loc(loc)
{
    auto *v = new QVBoxLayout(this);
    auto *note = new QLabel(QStringLiteral(
        "<b>Camera passes</b> come from your own route: near an <b>ALPR</b>, your plate was <i>likely read</i>. Other cameras (traffic webcams, CCTV, Flock PTZ video cameras) are listed but do not read plates. "
        "<b>Plate searches</b> come from Flock audit logs released through public-records requests (via HaveIBeenFlocked): an agency searched for your plate. "
        "Nobody publishes the photos Flock takes; the images here are your dash cam, public photos of the camera, or a public webcam still."));
    note->setWordWrap(true);
    v->addWidget(note);
    auto *bar = new QHBoxLayout;
    m_filter = new QComboBox;
    m_filter->addItem(QStringLiteral("Everything"), QString());
    m_filter->addItem(QStringLiteral("ALPR passes"), QStringLiteral("alpr"));
    m_filter->addItem(QStringLiteral("Other cameras (no plate reading)"), QStringLiteral("other"));
    m_filter->addItem(QStringLiteral("Plate searches"), QStringLiteral("plate_search"));
    bar->addWidget(m_filter);
    m_status = new QLabel;
    m_status->setWordWrap(true);
    bar->addWidget(m_status, 1);
    auto *backfill = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Re-run backfill"));
    backfill->setToolTip(QStringLiteral("Recompute every camera pass from your whole route history (worker thread)"));
    auto *hibf = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-find")), QStringLiteral("Check audit logs now"));
    hibf->setToolTip(QStringLiteral("Ask HaveIBeenFlocked for searches of your plates now (hashed prefixes only; at most every 10 s, 30 a day)"));
    auto *stills = new QCheckBox(QStringLiteral("Webcam stills"));
    stills->setToolTip(QStringLiteral("On a live pass by a public traffic webcam (e.g. WV511), grab the feed's current frame and keep it on this computer only.\n"
                                      "Off by default: WV511's terms forbid storing its images in a retrieval system. Never synced or served to other devices."));
    stills->setChecked(QSettings().value(QStringLiteral("webcamStills")).toBool());
    connect(stills, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("webcamStills"), on); });
    bar->addWidget(stills);
    bar->addWidget(backfill);
    bar->addWidget(hibf);
    v->addLayout(bar);
    m_table = new QTableWidget(0, 9);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Time"), QStringLiteral("Kind"), QStringLiteral("Camera / agency"), QStringLiteral("Distance"),
                                        QStringLiteral("Confidence"), QStringLiteral("Camera facing"), QStringLiteral("Source"),
                                        QStringLiteral("Agency (Eyes on Flock)"), QStringLiteral("What it means")});
    m_table->horizontalHeaderItem(7)->setToolTip(QString::fromLatin1(EyesOnFlock::kAttribution));
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSortingEnabled(false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        auto *item = m_table->itemAt(pos);
        if (!item) return;
        const int row = item->row();
        const QString uid = m_table->item(row, 0)->data(Qt::UserRole).toString();
        MapDb *db = m_loc->mapDb();
        if (!db) return;
        const QJsonObject ev = db->plateEvent(uid, false, false);
        const QString camId = ev.value(QLatin1String("camera_id")).toString();
        QMenu menu(this);
        menu.addAction(QIcon::fromTheme(QStringLiteral("view-list-details")), QStringLiteral("Open event…"), this, [this, uid] { openEvent(uid); });
        if (!camId.isEmpty()) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("security-high")), QStringLiteral("Inspect unseen…"), this, [this, camId] {
                emit inspectCamera(camId);
            });
        }
        double lat = ev.value(QLatin1String("camera_lat")).toDouble();
        double lon = ev.value(QLatin1String("camera_lon")).toDouble();
        if (lat == 0.0 && lon == 0.0) {
            lat = ev.value(QLatin1String("lat")).toDouble();
            lon = ev.value(QLatin1String("lon")).toDouble();
        }
        if (lat != 0.0 || lon != 0.0) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("map-globe")), QStringLiteral("Show on map"), this, [this, lat, lon] {
                emit showOnMap(lat, lon);
            });
        }
        menu.exec(m_table->viewport()->mapToGlobal(pos));
    });
    v->addWidget(m_table, 1);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { openEvent(m_table->item(row, 0)->data(Qt::UserRole).toString()); });
    connect(m_filter, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(backfill, &QPushButton::clicked, this, [this] { if (m_loc->plateWatch()) m_loc->plateWatch()->startBackfill(true); refresh(); });
    connect(hibf, &QPushButton::clicked, this, [this] { if (m_loc->plateWatch()) m_loc->plateWatch()->checkHibfNow(); refresh(); });
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(1500);
    connect(&m_debounce, &QTimer::timeout, this, &SightingsView::refresh);
    connect(m_loc, &Locator::plateEventsChanged, this, [this] { m_debounce.start(); });
    refresh();
}

void SightingsView::refresh()
{
    MapDb *db = m_loc->mapDb();
    if (!db || !db->isOpen()) { m_status->setText(QStringLiteral("Map database unavailable")); return; }
    const QString f = m_filter->currentData().toString();
    const QJsonArray all = db->plateEventsLatest(2000, f == QLatin1String("plate_search") ? f : (f.isEmpty() ? QString() : QStringLiteral("camera_pass")));
    m_table->setRowCount(0);
    PlateWatch *pw = m_loc->plateWatch();
    QHash<QString, QJsonObject> factsCache;                       // per camera / agency: the join runs once per refresh
    for (const QJsonValue &v : all) {
        const QJsonObject e = v.toObject();
        const bool isAlpr = e.value(QLatin1String("camera_type")).toString() == QLatin1String("alpr");
        if ((f == QLatin1String("alpr") && !isAlpr) || (f == QLatin1String("other") && isAlpr)) continue;
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const bool pass = e.value(QLatin1String("kind")).toString() == QLatin1String("camera_pass");
        const QString who = pass ? QStringList{e.value(QLatin1String("operator")).toString(), e.value(QLatin1String("model")).toString()}.join(QLatin1Char(' ')).simplified()
                                 : e.value(QLatin1String("agency")).toString();
        const QString fkey = pass ? e.value(QLatin1String("camera_id")).toString() : QStringLiteral("agency:") + e.value(QLatin1String("agency")).toString();
        if (pw && !factsCache.contains(fkey)) factsCache.insert(fkey, pw->agencyFacts(e));
        const QJsonObject facts = factsCache.value(fkey);
        QString conf = QStringLiteral("%1 %").arg(e.value(QLatin1String("confidence")).toInt());
        const QJsonObject snap = e.value(QLatin1String("metrics")).toObject().value(QLatin1String("snap")).toObject();
        const QString verdict = snap.value(QLatin1String("verdict")).toString();
        if (pass && (verdict == QLatin1String("parallel_road") || verdict == QLatin1String("different_layer") || verdict == QLatin1String("opposite_direction")))
            conf += QStringLiteral(" · other road");
        const QStringList cells{fmtTime(e.value(QLatin1String("time")).toString()), kindLabel(e), who,
                                pass ? QStringLiteral("%1 m").arg(qRound(e.value(QLatin1String("distance_m")).toDouble())) : QString(),
                                conf,
                                pass ? facingText(e.value(QLatin1String("facing"))) : QString(),
                                e.value(QLatin1String("source")).toString() + (e.value(QLatin1String("leaky")).toInt() == 1 && pass ? QStringLiteral(" · leaky agency") : QString()),
                                facts.isEmpty() ? QString() : facts.value(QLatin1String("agency")).toString() + QStringLiteral(": ") + EyesOnFlock::summaryLine(facts),
                                e.value(QLatin1String("details")).toString()};
        for (int c = 0; c < cells.size(); ++c) {
            auto *it = new QTableWidgetItem(cells[c]);
            if (c == 0) it->setData(Qt::UserRole, e.value(QLatin1String("uid")).toString());
            if (c == 1) it->setIcon(QIcon::fromTheme(pass ? (e.value(QLatin1String("camera_type")).toString() == QLatin1String("alpr") ? QStringLiteral("camera-web") : QStringLiteral("camera-photo"))
                                                          : QStringLiteral("system-search")));
            m_table->setItem(row, c, it);
        }
    }
    m_table->resizeColumnsToContents();
    if (m_table->columnWidth(7) > 420) m_table->setColumnWidth(7, 420);
    if (m_table->columnWidth(8) > 520) m_table->setColumnWidth(8, 520);
    const QJsonObject c = db->plateEventCounts();
    QString st = QStringLiteral("%1 ALPR passes · %2 other-camera passes · %3 plate searches · %4 images")
                     .arg(c.value(QLatin1String("alprPass")).toInt()).arg(c.value(QLatin1String("cameraOnlyPass")).toInt())
                     .arg(c.value(QLatin1String("plateSearch")).toInt()).arg(c.value(QLatin1String("media")).toInt());
    if (PlateWatch *pw = m_loc->plateWatch()) {
        const QJsonObject s = pw->status();
        const QJsonObject bf = s.value(QLatin1String("backfill")).toObject(), hb = s.value(QLatin1String("hibf")).toObject();
        if (bf.value(QLatin1String("running")).toBool()) st += QStringLiteral(" · backfill running (%1 day(s) left)").arg(bf.value(QLatin1String("chunksLeft")).toInt());
        else if (!bf.value(QLatin1String("finished")).toString().isEmpty()) st += QStringLiteral(" · backfill through %1").arg(fmtTime(bf.value(QLatin1String("through")).toString()));
        if (!hb.value(QLatin1String("lastCheck")).toString().isEmpty())
            st += QStringLiteral(" · audit logs checked %1, next %2 (%3)").arg(fmtTime(hb.value(QLatin1String("lastCheck")).toString()), fmtTime(hb.value(QLatin1String("nextCheck")).toString()),
                                                                               hb.value(QLatin1String("mode")).toString());
        else if (!hb.value(QLatin1String("lastError")).toString().isEmpty()) st += QStringLiteral(" · audit-log check: %1").arg(hb.value(QLatin1String("lastError")).toString());
        const QJsonObject eof = pw->eyesOnFlockState();
        if (eof.value(QLatin1String("status")).toString() == QLatin1String("ok"))
            st += QStringLiteral(" · Eyes on Flock: %1 agency portals (%2)").arg(eof.value(QLatin1String("portals")).toInt()).arg(fmtTime(eof.value(QLatin1String("fetched")).toString()).left(10));
        else if (!eof.value(QLatin1String("error")).toString().isEmpty())
            st += QStringLiteral(" · Eyes on Flock unavailable (%1): agency facts %2").arg(eof.value(QLatin1String("error")).toString(),
                                                                                          eof.value(QLatin1String("fetched")).toString().isEmpty() ? QStringLiteral("not shown") : QStringLiteral("from the last fetch"));
    }
    m_status->setText(st);
}

void SightingsView::openEvent(const QString &uid)
{
    MapDb *db = m_loc->mapDb();
    if (!db || uid.isEmpty()) return;
    const QJsonObject ev = db->plateEvent(uid, true, true);
    if (ev.isEmpty()) return;
    auto *d = new PlateEventDialog(m_loc, ev, this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    connect(d, &PlateEventDialog::showOnMap, this, &SightingsView::showOnMap);
    connect(d, &PlateEventDialog::inspectCamera, this, &SightingsView::inspectCamera);
    d->show();
    d->raise();
    d->activateWindow();
}

// ── the event dialog ──────────────────────────────────────────────────────────
static QString valueText(const QJsonValue &v, int decimals = 2)
{
    if (v.isNull() || v.isUndefined()) return QStringLiteral("—");
    if (v.isBool()) return v.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
    if (v.isDouble()) { const double d = v.toDouble(); return d == std::floor(d) && std::fabs(d) < 1e12 ? QString::number(qint64(d)) : QString::number(d, 'f', decimals); }
    if (v.isArray()) {
        QStringList parts;
        for (const QJsonValue &x : v.toArray()) parts << valueText(x);
        return parts.join(QStringLiteral(", "));
    }
    if (v.isObject()) return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    return v.toString();
}

PlateEventDialog::PlateEventDialog(Locator *loc, const QJsonObject &ev, QWidget *parent) : QDialog(parent)
{
    const bool pass = ev.value(QLatin1String("kind")).toString() == QLatin1String("camera_pass");
    setWindowTitle(QStringLiteral("%1 · %2").arg(kindLabel(ev), fmtTime(ev.value(QLatin1String("time")).toString())));
    resize(860, 720);
    auto *v = new QVBoxLayout(this);
    auto *head = new QLabel(QStringLiteral("<h3>%1</h3><p>%2</p>").arg(kindLabel(ev).toHtmlEscaped(), ev.value(QLatin1String("details")).toString().toHtmlEscaped()));
    head->setWordWrap(true);
    head->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(head);
    if (pass) {
        const QString honest = ev.value(QLatin1String("camera_type")).toString() == QLatin1String("alpr")
            ? QStringLiteral("Inferred from your route: you passed this plate reader, so it probably read the plate of the vehicle you were in. The plate shown is your active plate, "
                             "not a per-trip record. The confidence is 100 × P(read): your track inside the camera's cone and range (the fix error integrated) × capture × read.")
            : QStringLiteral("This is a %1: it does not read plates. Recorded so the pass is not lost, never alerted.").arg(PlateEvents::typeLabel(ev.value(QLatin1String("camera_type")).toString()));
        auto *h = new QLabel(honest); h->setWordWrap(true); v->addWidget(h);
    } else {
        auto *h = new QLabel(QStringLiteral("From a Flock audit log released through a public-records request, indexed by HaveIBeenFlocked. A search is not a stop; "
                                            "the logs arrive months after the searches.%1")
                                 .arg(ev.value(QLatin1String("confidence")).toInt() < 100 ? QStringLiteral(" This row has no full plate hash: it may be another plate with the same hash prefix.") : QString()));
        h->setWordWrap(true); v->addWidget(h);
    }
    PlateWatch *pw = loc->plateWatch();
    // §4.6: the agency's own transparency-portal numbers (Eyes on Flock, CC BY-SA 4.0)
    if (pw) {
        const QJsonObject f = pw->agencyFacts(ev);
        QString text;
        if (!f.isEmpty()) {
            text = QStringLiteral("<b>%1</b> (Eyes on Flock): %2.").arg(f.value(QLatin1String("agency")).toString().toHtmlEscaped(), EyesOnFlock::summaryLine(f).toHtmlEscaped());
            if (f.value(QLatin1String("receivedFrom")).isDouble()) text += QStringLiteral(" Receives from %1 agencies.").arg(f.value(QLatin1String("receivedFrom")).toInt());
            if (!f.value(QLatin1String("prohibitedUses")).toString().isEmpty())
                text += QStringLiteral(" Prohibited uses it lists: %1").arg(f.value(QLatin1String("prohibitedUses")).toString().toHtmlEscaped());
            text += QStringLiteral(" <a href=\"%1\">Transparency portal</a> · <small>%2 (<a href=\"%3\">eyesonflock.com</a>)</small>")
                        .arg(f.value(QLatin1String("portal")).toString().toHtmlEscaped(), QString::fromLatin1(EyesOnFlock::kAttribution).toHtmlEscaped(),
                             QString::fromLatin1(EyesOnFlock::kSite));
        } else {
            const QJsonObject st = pw->eyesOnFlockState();
            if (st.value(QLatin1String("status")).toString() != QLatin1String("ok") && !st.value(QLatin1String("error")).toString().isEmpty())
                text = QStringLiteral("Agency facts unavailable: Eyes on Flock could not be fetched (%1).").arg(st.value(QLatin1String("error")).toString().toHtmlEscaped());
        }
        if (!text.isEmpty()) {
            auto *l = new QLabel(text); l->setWordWrap(true); l->setTextFormat(Qt::RichText); l->setOpenExternalLinks(true);
            v->addWidget(l);
        }
    }
    // §2.6 / §2.7: was it the road the camera reads, and is the camera there at all
    if (pass && pw) {
        const QJsonObject m = ev.value(QLatin1String("metrics")).toObject();
        const QJsonObject snap = m.value(QLatin1String("snap")).toObject();
        static const QHash<QString, QString> says{{QStringLiteral("on_watched_way"), QStringLiteral("you were on the road it watches")},
                                                  {QStringLiteral("near_axis"), QStringLiteral("you were on a lane of the road it watches")},
                                                  {QStringLiteral("parallel_road"), QStringLiteral("you were on a parallel road, not the one it watches")},
                                                  {QStringLiteral("different_layer"), QStringLiteral("you were on an over- or underpass, not the road it watches")},
                                                  {QStringLiteral("opposite_direction"), QStringLiteral("you were on the other carriageway (against the oneway it reads)")},
                                                  {QStringLiteral("ambiguous"), QStringLiteral("the fixes cannot tell which road you were on")}};
        QString snapText;
        if (snap.value(QLatin1String("status")).toString() == QLatin1String("ok"))
            snapText = QStringLiteral("Road check: %1 (P(read) × %2, OSM way %3 watched, way %4 matched).")
                           .arg(says.value(snap.value(QLatin1String("verdict")).toString(), snap.value(QLatin1String("verdict")).toString()))
                           .arg(snap.value(QLatin1String("factor")).toDouble(), 0, 'f', 2)
                           .arg(qint64(snap.value(QLatin1String("watchedWay")).toDouble())).arg(qint64(snap.value(QLatin1String("matchedWay")).toDouble()));
        else if (!snap.isEmpty()) snapText = QStringLiteral("Road check: not possible (%1).").arg(snap.value(QLatin1String("status")).toString());
        else snapText = QStringLiteral("Road check: pending (the roads around this camera are fetched from OpenStreetMap).");
        auto *sl = new QLabel(snapText); sl->setWordWrap(true); v->addWidget(sl);
        const QString camId = ev.value(QLatin1String("camera_id")).toString();
        auto *trustRow = new QHBoxLayout;
        auto *tl = new QLabel; tl->setWordWrap(true);
        auto showTrust = [tl, pw, camId] {
            const QJsonObject c = pw->cameraInfo(camId);
            const QJsonObject d = c.value(QLatin1String("trustDetail")).toObject();
            QStringList terms;
            for (const QJsonValue &t : d.value(QLatin1String("terms")).toArray())
                terms << QStringLiteral("%1 %2%3").arg(t.toObject().value(QLatin1String("note")).toString())
                             .arg(t.toObject().value(QLatin1String("value")).toDouble() >= 0 ? QStringLiteral("+") : QString())
                             .arg(t.toObject().value(QLatin1String("value")).toDouble(), 0, 'f', 2);
            tl->setText(c.isEmpty() ? QStringLiteral("Camera trust: the camera is no longer in the map.")
                                    : QStringLiteral("Camera trust %1 % (P(read) × %2): %3. Suggested weights, not measured.")
                                          .arg(qRound(100 * c.value(QLatin1String("trust")).toDouble())).arg(c.value(QLatin1String("trust")).toDouble(), 0, 'f', 2)
                                          .arg(terms.join(QStringLiteral("; "))));
        };
        showTrust();
        trustRow->addWidget(tl, 1);
        auto *there = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), QStringLiteral("It's there"));
        there->setToolTip(QStringLiteral("You saw this camera: its trust goes up (log-odds +1) and its passes are rescored"));
        auto *gone = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), QStringLiteral("Not there"));
        gone->setToolTip(QStringLiteral("The camera is not where the map says: its trust goes down (log-odds −1.5) and its passes are rescored"));
        auto *clear = new QPushButton(QStringLiteral("Clear"));
        clear->setToolTip(QStringLiteral("Forget your verdict for this camera"));
        for (auto *b : {there, gone, clear}) trustRow->addWidget(b);
        const auto set = [this, pw, camId, showTrust](const QString &verdict) {
            const QJsonObject r = pw->setCameraVerdict(camId, verdict);
            if (r.contains(QLatin1String("error"))) QMessageBox::warning(this, QStringLiteral("Camera verdict"), r.value(QLatin1String("error")).toString());
            showTrust();
        };
        connect(there, &QPushButton::clicked, this, [set] { set(QStringLiteral("present")); });
        connect(gone, &QPushButton::clicked, this, [set] { set(QStringLiteral("absent")); });
        connect(clear, &QPushButton::clicked, this, [set] { set(QStringLiteral("clear")); });
        v->addLayout(trustRow);
    }
    auto *tabs = new QTabWidget;
    v->addWidget(tabs, 1);
    // metrics
    {
        auto *w = new QWidget; auto *form = new QFormLayout(w);
        static const char *cols[] = {"time", "plate", "camera_type", "operator", "agency", "model", "distance_m", "speed_kmh", "heading_deg", "approach_bearing_deg",
                                     "camera_dir_deg", "facing", "confidence", "leaky", "acc", "lat", "lon", "camera_id", "camera_lat", "camera_lon", "source", "source_name", "device", "uid"};
        for (const char *c : cols) {
            const QJsonValue val = ev.value(QLatin1String(c));
            if (val.isUndefined() || (!pass && QByteArray(c).startsWith("camera"))) continue;
            const QString key = QString::fromLatin1(c);
            QString t = key == QLatin1String("facing") ? facingText(val) : valueText(val, key.endsWith(QLatin1String("lat")) || key.endsWith(QLatin1String("lon")) ? 6 : 2);
            auto *l = new QLabel(t); l->setTextInteractionFlags(Qt::TextSelectableByMouse); l->setWordWrap(true);
            form->addRow(QString::fromLatin1(c) + QLatin1Char(':'), l);
        }
        const QJsonObject m = ev.value(QLatin1String("metrics")).toObject();
        for (auto it = m.begin(); it != m.end(); ++it) {
            auto *l = new QLabel(valueText(it.value())); l->setTextInteractionFlags(Qt::TextSelectableByMouse); l->setWordWrap(true);
            form->addRow(QStringLiteral("metrics.%1:").arg(it.key()), l);
        }
        auto *sa = new QScrollArea; sa->setWidget(w); sa->setWidgetResizable(true);
        tabs->addTab(sa, QIcon::fromTheme(QStringLiteral("view-list-details")), QStringLiteral("Metrics"));
    }
    // images
    {
        auto *w = new QWidget; auto *grid = new QVBoxLayout(w);
        const QJsonArray media = ev.value(QLatin1String("media")).toArray();
        if (media.isEmpty()) {
            auto *l = new QLabel(pass ? QStringLiteral("No images yet. Camera photos come from OpenStreetMap image tags, Wikimedia Commons or Panoramax; "
                                                       "a dash-cam frame from the phone; a webcam still only on a live pass by a camera with a public feed.")
                                      : QStringLiteral("Plate searches have no images."));
            l->setWordWrap(true); grid->addWidget(l);
        }
        MapDb *db = loc->mapDb();
        for (const QJsonValue &mv : media) {
            const QJsonObject m = mv.toObject();
            MapDb::MediaRow meta;
            const QByteArray data = db ? db->mediaData(m.value(QLatin1String("uid")).toString(), &meta) : QByteArray();
            QImage img = ImageStore::decode(data, meta.mime);          // JPEG XL through Qt's jxl plugin (kimageformats), else djxl
            auto *box = new QGroupBox(QStringLiteral("%1 · %2×%3 · %4 KB stored (%5)").arg(m.value(QLatin1String("kind")).toString()).arg(meta.width).arg(meta.height)
                                          .arg(qRound(double(data.size()) / 1024.0)).arg(meta.mime + (meta.jpegReconstructible ? QStringLiteral(", the original JPEG bit for bit") : QString())));
            auto *bl = new QVBoxLayout(box);
            auto *pic = new QLabel;
            if (!img.isNull()) pic->setPixmap(QPixmap::fromImage(img.width() > 780 ? img.scaledToWidth(780, Qt::SmoothTransformation) : img));
            else pic->setText(QStringLiteral("(cannot decode %1 here)").arg(meta.mime));
            bl->addWidget(pic);
            QString cap = meta.attribution;
            if (!meta.license.isEmpty()) cap += QStringLiteral(" · %1").arg(meta.license);
            if (!meta.capturedAt.isEmpty()) cap += QStringLiteral(" · captured %1").arg(fmtTime(meta.capturedAt));
            if (!meta.originalUrl.isEmpty()) cap += QStringLiteral(" · <a href=\"%1\">original</a>").arg(meta.originalUrl.toHtmlEscaped());
            auto *c = new QLabel(cap); c->setWordWrap(true); c->setOpenExternalLinks(true); c->setTextFormat(Qt::RichText);
            bl->addWidget(c);
            grid->addWidget(box);
        }
        grid->addStretch(1);
        auto *sa = new QScrollArea; sa->setWidget(w); sa->setWidgetResizable(true);
        tabs->addTab(sa, QIcon::fromTheme(QStringLiteral("image-x-generic")), QStringLiteral("Images (%1)").arg(media.size()));
    }
    // raw
    {
        auto *t = new QPlainTextEdit; t->setReadOnly(true);
        QJsonObject full = ev;
        t->setPlainText(QString::fromUtf8(QJsonDocument(ev.value(QLatin1String("raw")).toObject()).toJson(QJsonDocument::Indented))
                        + QStringLiteral("\n── the event ──\n") + QString::fromUtf8(QJsonDocument(full).toJson(QJsonDocument::Indented)));
        t->setFont(QFont(QStringLiteral("monospace")));
        tabs->addTab(t, QIcon::fromTheme(QStringLiteral("text-x-generic")), QStringLiteral("Raw record"));
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    const QString url = ev.value(QLatin1String("source_url")).toString();
    if (!url.isEmpty()) {
        auto *src = buttons->addButton(QStringLiteral("View source"), QDialogButtonBox::ActionRole);
        src->setIcon(QIcon::fromTheme(QStringLiteral("internet-web-browser")));
        src->setToolTip(url);
        connect(src, &QPushButton::clicked, this, [url] { QDesktopServices::openUrl(QUrl(url)); });
    }
    const double lat = pass ? ev.value(QLatin1String("lat")).toDouble() : 0, lon = pass ? ev.value(QLatin1String("lon")).toDouble() : 0;
    if (lat != 0.0 || lon != 0.0) {
        auto *map = buttons->addButton(QStringLiteral("Show on map"), QDialogButtonBox::ActionRole);
        map->setIcon(QIcon::fromTheme(QStringLiteral("map-globe")));
        connect(map, &QPushButton::clicked, this, [this, lat, lon] { emit showOnMap(lat, lon); });
    }
    const QString camId = pass ? ev.value(QLatin1String("camera_id")).toString() : QString();
    if (!camId.isEmpty()) {
        auto *insp = buttons->addButton(QStringLiteral("Inspect unseen…"), QDialogButtonBox::ActionRole);
        insp->setIcon(QIcon::fromTheme(QStringLiteral("security-high")));
        connect(insp, &QPushButton::clicked, this, [this, camId] {
            emit inspectCamera(camId);
            close();
        });
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    v->addWidget(buttons);
}
