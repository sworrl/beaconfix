#include "hubclient.h"
#include "locator.h"
#include "mapdb.h"
#include "securechannel.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSettings>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QUrl>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSharedPointer>
#include <QThread>
#include "estimator.h"

using namespace Bfs3;

static const char *KEY_CONFIG = "hub:config";
static const char *KEY_COUNTER = "hub:counter";
static const QString CURSOR_KEY = QStringLiteral("hub");      // kv sync:hub:pushed / sync:hub:pulled (Locator::syncRound)

HubClient::HubClient(Locator *loc, QObject *parent) : QObject(parent), m_loc(loc)
{
    load();
    m_syncTimer.setInterval(15000);                           // checks; the round itself runs every hub/syncMinutes
    connect(&m_syncTimer, &QTimer::timeout, this, &HubClient::tick);
    m_posTimer.setInterval(qBound(10, QSettings().value("hub/positionSeconds", 30).toInt(), 3600) * 1000);
    connect(&m_posTimer, &QTimer::timeout, this, &HubClient::positions);
    connect(m_loc, &Locator::FixChanged, this, [this] {      // a new fix goes out within a few seconds
        if (enrolled() && m_loc->fix().valid && m_loc->fix().time != m_publishedFix) QTimer::singleShot(3000, this, &HubClient::positions);
    });
    m_jobTimer.setInterval(60000);
    connect(&m_jobTimer, &QTimer::timeout, this, [this] { heartbeat(); jobs(); });
    m_syncTimer.start(); m_posTimer.start(); m_jobTimer.start();
    if (enrolled()) { QTimer::singleShot(5000, this, &HubClient::tick); QTimer::singleShot(8000, this, &HubClient::positions); }
}

void HubClient::load()
{
    MapDb *db = m_loc->mapDb();
    m_rk.clear(); m_deviceId.clear();
    if (!db || !db->isOpen() || db->key().size() != 32) return;
    const QJsonObject o = QJsonDocument::fromJson(db->kv(QString::fromLatin1(KEY_CONFIG)).toUtf8()).object();
    if (o.isEmpty()) return;
    m_url = o["url"].toString(); m_name = o["name"].toString(); m_kind = o["kind"].toString(); m_fingerprint = o["fingerprint"].toString();
    m_spk = unb64u(o["serverPub"].toString().toLatin1());
    const QByteArray dsk = openAtRest(db->key(), "bfs3 device", QByteArray::fromBase64(o["deviceKey"].toString().toLatin1()));
    if (dsk.size() != 32 || m_spk.size() != 32 || fingerprint(m_spk) != m_fingerprint) { m_lastError = QStringLiteral("stored hub keys do not open with this database key"); return; }
    const QByteArray dpk = x25519Public(dsk), shared = x25519(dsk, m_spk);
    if (shared.isEmpty()) return;
    m_deviceId = deviceId(dpk);
    m_rk = rootKey(shared, m_spk, dpk);
    m_counter = m_reserved = db->kv(QString::fromLatin1(KEY_COUNTER)).toULongLong();
}

// Counters are handed out from a reservation persisted 64 ahead: after a crash we start past anything we may have used
quint64 HubClient::nextCounter()
{
    ++m_counter;
    if (m_counter >= m_reserved) {
        m_reserved = m_counter + 64;
        if (MapDb *db = m_loc->mapDb()) { db->setKv(QString::fromLatin1(KEY_COUNTER), QString::number(m_reserved)); db->scheduleFlush(); }
    }
    return m_counter;
}

QNetworkRequest HubClient::baseRequest(const QUrl &url, int timeoutMs) const
{
    QNetworkRequest r(url);
    r.setTransferTimeout(timeoutMs);
    r.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("BeaconFix/%1 (bfs3)").arg(QStringLiteral(BEACONFIX_VERSION)));
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (url.scheme() == QLatin1String("https")) {
        QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
        QString ca = qEnvironmentVariable("BEACONFIX_HUB_CA"); if (ca.isEmpty()) ca = QSettings().value("hub/caFile").toString();
        if (!ca.isEmpty()) { QList<QSslCertificate> roots = cfg.caCertificates(); roots += QSslCertificate::fromPath(ca, QSsl::Pem); cfg.setCaCertificates(roots); }
        if (qEnvironmentVariable("BEACONFIX_HUB_TLS_INSECURE") == QLatin1String("1") || QSettings().value("hub/tlsInsecure", false).toBool()) cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
        r.setSslConfiguration(cfg);
    }
    return r;
}

void HubClient::request(const QByteArray &method, const QString &endpoint, const QByteArray &body, Reply reply, int timeoutMs)
{
    if (!enrolled()) { reply(0, {}, QStringLiteral("not enrolled with a hub")); return; }
    QString base = m_url; while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    const QUrl url(base + QStringLiteral("/api/v3/") + endpoint, QUrl::StrictMode);
    if (!url.isValid()) { reply(0, {}, QStringLiteral("bad endpoint")); return; }
    // The AAD's target is what goes on the wire: Qt's HTTP code sends exactly this encoding
    const QByteArray target = url.toEncoded(QUrl::RemoveScheme | QUrl::RemoveAuthority | QUrl::RemoveFragment | QUrl::RemoveUserInfo | QUrl::FullyEncoded);
    const quint64 c = nextCounter();
    const qint64 ts = QDateTime::currentSecsSinceEpoch();
    const QByteArray nonce = random(12);
    const QByteArray sealed = sealRequest(m_rk, m_deviceId, method, target, c, ts, nonce, body);
    if (nonce.isEmpty() || sealed.size() < 16) { reply(0, {}, QStringLiteral("cannot seal the request")); return; }
    QNetworkRequest r = baseRequest(url, timeoutMs);
    r.setRawHeader("X-BF-Device", m_deviceId.toLatin1());
    r.setRawHeader("X-BF-Counter", QByteArray::number(c));
    r.setRawHeader("X-BF-Time", QByteArray::number(ts));
    r.setRawHeader("X-BF-Nonce", b64u(nonce));
    QNetworkReply *rep;
    if (body.isEmpty()) {                                     // bodiless: the tag travels in X-BF-Seal
        r.setRawHeader("X-BF-Seal", b64u(sealed));
        rep = method == "GET" ? m_nam.get(r) : method == "DELETE" ? m_nam.deleteResource(r) : m_nam.sendCustomRequest(r, method);
    } else {
        r.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/vnd.beaconfix.sealed"));
        rep = m_nam.sendCustomRequest(r, method, sealed);
    }
    const QString did = m_deviceId; const QByteArray rk = m_rk;
    connect(rep, &QNetworkReply::finished, this, [this, rep, reply, c, did, rk] {
        rep->deleteLater();
        const int status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = rep->readAll();
        if (status == 0) { reply(0, {}, rep->errorString()); return; }
        const QByteArray rc = rep->rawHeader("X-BF-Counter"), rn = rep->rawHeader("X-BF-Nonce");
        if (rc.isEmpty()) {                                    // not sealed: the hub refused before knowing us (401, 429, 404 …)
            if (status == 401) m_counter += 256;               // maybe our counter fell behind (a lost reservation): jump past it
            const QString e = QJsonDocument::fromJson(raw).object()["error"].toString();
            reply(status, {}, QStringLiteral("hub: HTTP %1%2").arg(status).arg(e.isEmpty() ? QString() : QStringLiteral(" ") + e));
            return;
        }
        QByteArray plain; bool okN = false;
        const QByteArray nonce = unb64u(rn, &okN);
        if (rc.toULongLong() != c || !okN || !openResponse(rk, did, status, c, nonce, raw, &plain)) { reply(status, {}, QStringLiteral("the response failed authentication (not our hub?)")); return; }
        if (status < 200 || status >= 300) {
            const QString e = QJsonDocument::fromJson(plain).object()["error"].toString();
            reply(status, plain, QStringLiteral("HTTP %1%2").arg(status).arg(e.isEmpty() ? QString() : QStringLiteral(": ") + e));
            return;
        }
        reply(status, plain, QString());
    });
}

QString HubClient::detectKind()
{
    for (const QString &b : QDir(QStringLiteral("/sys/class/power_supply")).entryList({QStringLiteral("BAT*")}, QDir::Dirs | QDir::System)) { Q_UNUSED(b); return QStringLiteral("laptop"); }
    return QStringLiteral("desktop");
}

// docs/SECURE-API.md "Enrolment": our own X25519 pair, the invite's MAC, then the hub's proof that it holds S_sk
void HubClient::enroll(const QString &inviteText, const QString &nameIn, std::function<void(bool, const QJsonObject &)> done)
{
    MapDb *db = m_loc->mapDb();
    if (!db || !db->isOpen() || db->readOnly() || db->key().size() != 32) { done(false, {{"error", "the map database is not writable"}}); return; }
    Invite inv;
    if (!Invite::decode(inviteText, &inv)) { done(false, {{"error", "not a bfs3: invite"}}); return; }
    if (inv.expires + 60 < QDateTime::currentSecsSinceEpoch()) { done(false, {{"error", "the invite has expired — make a new one on the hub"}}); return; }
    QByteArray dpk; const QByteArray dsk = x25519Generate(&dpk);
    const QByteArray shared = x25519(dsk, inv.serverPub);
    if (dsk.isEmpty() || shared.isEmpty()) { done(false, {{"error", "key generation failed"}}); return; }
    const QString name = nameIn.trimmed().isEmpty() ? Locator::deviceName() : nameIn.trimmed().left(64), kind = detectKind();
    const QString did = deviceId(dpk), pub = QString::fromLatin1(b64u(dpk));
    const qint64 ts = QDateTime::currentSecsSinceEpoch();
    const QByteArray rk = rootKey(shared, inv.serverPub, dpk);
    const QJsonObject body{{"inviteId", inv.id}, {"name", name}, {"kind", kind}, {"pub", pub}, {"ts", double(ts)},
                           {"mac", enrollMac(inv.secret, inv.id, name, kind, pub, ts)}};
    QString base = inv.url; while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    QNetworkRequest r = baseRequest(QUrl(base + QStringLiteral("/api/v3/enroll")), 30000);
    r.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *rep = m_nam.post(r, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(rep, &QNetworkReply::finished, this, [this, rep, done, inv, dsk, did, rk, name, kind, db] {
        rep->deleteLater();
        const int status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        if (status != 200) { done(false, {{"error", status == 0 ? rep->errorString() : QStringLiteral("hub: HTTP %1 %2").arg(status).arg(o["error"].toString())}}); return; }
        const QString fp = fingerprint(inv.serverPub);
        if (o["deviceId"].toString() != did || o["fingerprint"].toString() != fp
            || !constantTimeEqual(o["proof"].toString().toLower().toLatin1(), proof(rk, did).toLatin1())) {
            done(false, {{"error", "the hub's proof does not match the invite's key — NOT enrolled (wrong hub or tampered invite)"}}); return;
        }
        const QJsonObject prev = QJsonDocument::fromJson(db->kv(QString::fromLatin1(KEY_CONFIG)).toUtf8()).object();
        if (prev["fingerprint"].toString() != fp) {             // another hub: its cursors start over
            db->setKv(QStringLiteral("sync:%1:pushed").arg(CURSOR_KEY), QStringLiteral("0"));
            db->setKv(QStringLiteral("sync:%1:pulled").arg(CURSOR_KEY), QStringLiteral("0"));
        }
        const QJsonObject cfg{{"url", inv.url}, {"serverPub", QString::fromLatin1(b64u(inv.serverPub))}, {"fingerprint", fp}, {"deviceId", did},
                              {"name", name}, {"kind", kind}, {"enrolled", QDateTime::currentDateTime().toString(Qt::ISODate)},
                              {"deviceKey", QString::fromLatin1(sealAtRest(db->key(), "bfs3 device", dsk).toBase64())}};
        db->setKv(QString::fromLatin1(KEY_CONFIG), QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Compact)));
        db->setKv(QString::fromLatin1(KEY_COUNTER), QStringLiteral("0"));
        db->flush();                                          // the device key must survive a crash right after this
        load();
        m_failures = 0; m_nextSync = QDateTime(); m_lastError.clear();
        emit changed();
        QTimer::singleShot(500, this, &HubClient::tick);
        QTimer::singleShot(1000, this, &HubClient::positions);
        done(true, {{"ok", true}, {"deviceId", did}, {"fingerprint", fp}, {"url", inv.url}, {"name", name}, {"kind", kind}});
    });
}

void HubClient::requestInvite(const QString &name, const QString &kind, std::function<void(const QString &, const QString &)> done, int timeoutMs)
{
    const QByteArray body = QJsonDocument(QJsonObject{{"name", name.left(64)}, {"kind", kind.left(16)}}).toJson(QJsonDocument::Compact);
    request("POST", QStringLiteral("hub/invites"), body, [done](int, const QByteArray &plain, const QString &error) {
        const QString inv = QJsonDocument::fromJson(plain).object()["invite"].toString();
        Invite check;
        if (!error.isEmpty() || !Invite::decode(inv, &check)) { done(QString(), error.isEmpty() ? QStringLiteral("the hub sent no invite") : error); return; }
        done(inv, QString());
    }, timeoutMs);
}

void HubClient::forget()
{
    if (MapDb *db = m_loc->mapDb()) { db->setKv(QString::fromLatin1(KEY_CONFIG), QString()); db->flush(); }
    m_rk.clear(); m_deviceId.clear(); m_url.clear(); m_spk.clear(); m_fingerprint.clear();
    emit changed();
}

void HubClient::syncNow(std::function<void(bool, const QString &)> done)
{
    if (done) m_waiters << done;
    m_nextSync = QDateTime();
    if (!m_busy) tick();
}

// A sync round when due (hub/syncMinutes, default 2), with back-off after failures
void HubClient::tick()
{
    auto finishWaiters = [this](bool ok, const QString &msg) { const auto w = m_waiters; m_waiters.clear(); for (const auto &f : w) f(ok, msg); };
    if (!enrolled()) { finishWaiters(false, QStringLiteral("not enrolled with a hub")); return; }
    if (m_busy) return;
    const QDateTime now = QDateTime::currentDateTime();
    if (m_nextSync.isValid() && now < m_nextSync) return;
    m_busy = true;
    Locator::SyncSend send = [this](const QByteArray &method, const QString &ep, const QByteArray &body, Locator::SyncReply reply) {
        request(method, ep, body, [reply](int status, const QByteArray &plain, const QString &error) {
            reply(status, QJsonDocument::fromJson(plain).object(), error);
        });
    };
    m_loc->syncRound(CURSOR_KEY, QStringLiteral("hub"), send, [this, finishWaiters](bool ok, const QString &msg, int) {
        m_busy = false;
        m_lastSync = QDateTime::currentDateTime(); m_lastOk = ok;
        if (ok) {
            m_failures = 0; m_lastResult = msg; m_lastError.clear();
            m_nextSync = m_lastSync.addSecs(qBound(1, QSettings().value("hub/syncMinutes", 2).toInt(), 24 * 60) * 60);
            emit m_loc->scanUpdated();
            QTimer::singleShot(200, this, [this] { heartbeat(); jobs(); });   // fresh data: the jobs it unlocked
        } else {
            ++m_failures; m_lastError = msg;
            m_nextSync = m_lastSync.addSecs(qMin(900, 30 << qMin(m_failures - 1, 5)));   // 30 s, 1, 2, 4, 8, 15 min
            qInfo("beaconfix: hub sync failed (%d in a row, next in %lld s): %s", m_failures, (long long)m_lastSync.secsTo(m_nextSync), qPrintable(msg));
        }
        emit changed();
        finishWaiters(ok, msg);
    }, m_name);
}

// Our position out (when the fix changed, else every 5 min as a heartbeat), every node's position in
void HubClient::positions()
{
    if (!enrolled() || m_posBusy) return;
    if (m_failures > 0 && m_nextSync.isValid() && QDateTime::currentDateTime() < m_nextSync) return;   // offline: wait for the sync's back-off
    m_posBusy = true;
    auto pull = [this] {
        request("GET", QStringLiteral("devices/positions"), {}, [this](int, const QByteArray &plain, const QString &error) {
            m_posBusy = false;
            if (!error.isEmpty()) { m_lastError = QStringLiteral("positions: ") + error; emit changed(); return; }
            m_loc->mergeRemoteDevices(QJsonDocument::fromJson(plain).object()["devices"].toArray(), QStringLiteral("hub"), {m_name});
        }, 30000);
    };
    const Fix &f = m_loc->fix();
    const QDateTime now = QDateTime::currentDateTime();
    if (f.valid && (f.time != m_publishedFix || !m_lastPublish.isValid() || m_lastPublish.secsTo(now) >= 300)) {
        const QJsonObject b{{"lat", f.lat}, {"lon", f.lon}, {"acc", f.accuracy}, {"time", f.time.toString(Qt::ISODate)}, {"source", f.source},
                            {"place", f.place}, {"kind", m_kind}, {"beacons", m_loc->apCount()}};
        const QDateTime ft = f.time;
        request("POST", QStringLiteral("devices/position"), QJsonDocument(b).toJson(QJsonDocument::Compact), [this, pull, ft, now](int, const QByteArray &, const QString &error) {
            if (error.isEmpty()) { m_publishedFix = ft; m_lastPublish = now; }
            pull();
        }, 30000);
        return;
    }
    pull();
}

// What this node can do, for the hub's registry and its job routing
QJsonObject HubClient::capabilities()
{
    int wifi = 0;
    for (const QString &n : QDir(QStringLiteral("/sys/class/net")).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System))
        if (QFileInfo::exists(QStringLiteral("/sys/class/net/%1/wireless").arg(n)) || QFileInfo::exists(QStringLiteral("/sys/class/net/%1/phy80211").arg(n))) ++wifi;
    QString gpu;
    QFile nv(QStringLiteral("/proc/driver/nvidia/version"));
    if (nv.open(QIODevice::ReadOnly)) gpu = QStringLiteral("nvidia");
    else for (const QString &c : QDir(QStringLiteral("/sys/class/drm")).entryList({QStringLiteral("card?")}, QDir::Dirs | QDir::System)) {
        QFile v(QStringLiteral("/sys/class/drm/%1/device/vendor").arg(c));
        if (!v.open(QIODevice::ReadOnly)) continue;
        const QByteArray id = v.readAll().trimmed();
        gpu = id == "0x10de" ? QStringLiteral("nvidia") : id == "0x1002" ? QStringLiteral("amd") : id == "0x8086" ? QStringLiteral("intel") : QString::fromLatin1(id);
        if (gpu == QLatin1String("nvidia") || gpu == QLatin1String("amd")) break;
    }
    const bool compute = QSettings().value("hub/compute", true).toBool();
    return {{"compute", compute ? QThread::idealThreadCount() : 0}, {"gpu", gpu.isEmpty() ? QJsonValue(false) : QJsonValue(gpu)}, {"wifiScan", wifi},
            {"rtt", false}, {"mobile", false}, {"tensor", false}, {"version", QStringLiteral(BEACONFIX_VERSION)},
            {"jobs", compute ? QJsonArray{QStringLiteral("refit")} : QJsonArray()}, {"role", Locator::hubRole() ? "hub" : qEnvironmentVariableIsSet("BEACONFIX_NODE") ? "node" : "desktop"},
            {"estimator", Estimator::kVersion}};
}

void HubClient::heartbeat()
{
    if (!enrolled() || (m_lastHeartbeat.isValid() && m_lastHeartbeat.secsTo(QDateTime::currentDateTime()) < 600)) return;
    if (m_failures > 0 && m_nextSync.isValid() && QDateTime::currentDateTime() < m_nextSync) return;
    m_lastHeartbeat = QDateTime::currentDateTime();
    request("POST", QStringLiteral("nodes/heartbeat"), QJsonDocument(QJsonObject{{"capabilities", capabilities()}}).toJson(QJsonDocument::Compact),
            [this](int, const QByteArray &, const QString &error) { if (!error.isEmpty()) m_lastHeartbeat = QDateTime(); }, 30000);
}

// Lease → compute in ~100 ms slices on the event loop (refits touch the Locator's records; Estimator::fitAp itself is
// pure) → submit all results in one request → again while the hub hands out full batches
void HubClient::jobs()
{
    if (!enrolled() || m_jobBusy || !QSettings().value("hub/compute", true).toBool() || m_busy) return;
    if (m_failures > 0 && m_nextSync.isValid() && QDateTime::currentDateTime() < m_nextSync) return;
    MapDb *db = m_loc->mapDb();
    if (!db) return;
    m_jobBusy = true;
    const int max = qBound(1, QSettings().value("hub/jobBatch", 100).toInt(), 500);
    const QJsonObject body{{"types", QJsonArray{QStringLiteral("refit")}}, {"max", max}, {"cursor", double(db->kv(QStringLiteral("sync:%1:pulled").arg(CURSOR_KEY)).toLongLong())},
                           {"capabilities", capabilities()}};
    request("POST", QStringLiteral("jobs/lease"), QJsonDocument(body).toJson(QJsonDocument::Compact), [this, max](int, const QByteArray &plain, const QString &error) {
        if (!error.isEmpty()) { m_jobBusy = false; m_lastError = QStringLiteral("jobs: ") + error; emit changed(); return; }
        const QJsonArray leased = QJsonDocument::fromJson(plain).object()["jobs"].toArray();
        m_lastJobs = QDateTime::currentDateTime();
        if (leased.isEmpty()) { m_jobBusy = false; return; }
        auto results = QSharedPointer<QJsonArray>::create();
        auto idx = QSharedPointer<int>::create(0);
        auto slice = QSharedPointer<std::function<void()>>::create();
        *slice = [this, leased, results, idx, slice, max] {
            QElapsedTimer clock; clock.start();
            while (*idx < leased.size() && clock.elapsed() < 100) {
                const QJsonObject j = leased[(*idx)++].toObject();
                QJsonObject r{{"id", j["id"].toString()}, {"lease", j["lease"].toString()}};
                if (j["type"].toString() == QLatin1String("refit")) {
                    const QJsonObject res = m_loc->computeRefit(j["key"].toString());
                    if (res.isEmpty()) r["error"] = QStringLiteral("no samples of %1 here").arg(j["key"].toString()); else r["result"] = res;
                } else r["error"] = QStringLiteral("unsupported job type");
                results->append(r);
            }
            if (*idx < leased.size()) { QTimer::singleShot(0, this, [slice] { (*slice)(); }); return; }
            request("POST", QStringLiteral("jobs/results"), QJsonDocument(QJsonObject{{"results", *results}}).toJson(QJsonDocument::Compact),
                    [this, n = int(leased.size()), max, slice](int, const QByteArray &plain, const QString &error) {
                *slice = nullptr;                              // break the self-reference
                m_jobBusy = false;
                if (!error.isEmpty()) { m_lastError = QStringLiteral("job results: ") + error; emit changed(); return; }
                const int accepted = QJsonDocument::fromJson(plain).object()["accepted"].toInt();
                m_jobsDone += accepted; m_jobsFailed += n - accepted;
                emit changed();
                if (n >= max) QTimer::singleShot(50, this, &HubClient::jobs);   // a full batch: there is probably more
            });
        };
        (*slice)();
    });
}

QJsonObject HubClient::statusJson() const
{
    auto iso = [](const QDateTime &t) { return t.isValid() ? QJsonValue(t.toString(Qt::ISODate)) : QJsonValue(); };
    return {{"enrolled", enrolled()}, {"url", m_url}, {"fingerprint", m_fingerprint}, {"deviceId", m_deviceId}, {"name", m_name}, {"kind", m_kind},
            {"counter", double(m_counter)}, {"busy", m_busy}, {"lastSync", iso(m_lastSync)}, {"lastOk", m_lastOk}, {"lastResult", m_lastResult},
            {"lastError", m_lastError}, {"nextSync", iso(m_nextSync)}, {"failures", m_failures}, {"lastPositionSent", iso(m_lastPublish)},
            {"jobsDone", m_jobsDone}, {"jobsFailed", m_jobsFailed}, {"lastJobs", iso(m_lastJobs)}, {"compute", QSettings().value("hub/compute", true).toBool()}};
}
