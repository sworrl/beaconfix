#include "hub.h"
#include "mapdb.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QElapsedTimer>
#include <algorithm>

using namespace Bfs3;

static const int FAIL_MAX = 20;                      // failures per source …
static const qint64 FAIL_SPAN_MS = 10 * 60 * 1000;   // … within 10 min …
static const qint64 BLOCK_MS = 15 * 60 * 1000;       // … refuse it for 15 min
static const QStringList ALL_SCOPES{QStringLiteral("read"), QStringLiteral("sync"), QStringLiteral("control")};

QJsonObject Hub::Device::toJson(bool store) const
{
    QJsonObject o{{"id", id}, {"name", name}, {"kind", kind}, {"scopes", QJsonArray::fromStringList(scopes)},
                  {"created", created.toString(Qt::ISODate)}, {"lastSeen", lastSeen.isValid() ? QJsonValue(lastSeen.toString(Qt::ISODate)) : QJsonValue()},
                  {"lastIp", lastIp}, {"revoked", revoked}, {"counter", double(win.highest())}};
    if (!label.isEmpty()) o["invitedAs"] = label;
    if (!caps.isEmpty()) { o["capabilities"] = caps; o["capabilitiesTime"] = capsTime.toString(Qt::ISODate); }
    if (lastLease.isValid()) o["lastLease"] = lastLease.toString(Qt::ISODate);
    if (jobsDone || jobsFailed) { o["jobsDone"] = jobsDone; o["jobsFailed"] = jobsFailed; }
    if (store) { o["pub"] = QString::fromLatin1(b64u(pub)); o["win"] = win.save(); }
    return o;
}

Hub::Hub(MapDb *db, const QString &stateDir, const QString &url, QObject *parent) : QObject(parent), m_db(db), m_stateDir(stateDir), m_url(url)
{
    m_saveTimer.setSingleShot(true); m_saveTimer.setInterval(60000);
    connect(&m_saveTimer, &QTimer::timeout, this, &Hub::saveDevices);
    m_jobSaveTimer.setSingleShot(true); m_jobSaveTimer.setInterval(30000);
    connect(&m_jobSaveTimer, &QTimer::timeout, this, &Hub::saveJobs);
    m_selfTimer.setInterval(30000);
    connect(&m_selfTimer, &QTimer::timeout, this, &Hub::selfComputeTick);
    m_leaseSecs = qBound(30, qEnvironmentVariableIntValue("BEACONFIX_HUB_LEASE_SECONDS") > 0 ? qEnvironmentVariableIntValue("BEACONFIX_HUB_LEASE_SECONDS") : 600, 3600);
    loadKeys();
    if (!ready()) return;
    loadDevices();
    loadInvites();
    loadWindows();
    loadJobs();
}

Hub::~Hub() { if (m_saveTimer.isActive()) saveDevices(); if (m_jobSaveTimer.isActive()) saveJobs(); }

// S_sk: generated once, sealed with the database key (HKDF label "bfs3 server") in kv
void Hub::loadKeys()
{
    if (!m_db || !m_db->isOpen() || m_db->readOnly() || m_db->key().size() != 32) { m_error = QStringLiteral("the map database is not open for writing"); return; }
    const QByteArray stored = QByteArray::fromBase64(m_db->kv(QStringLiteral("bfs3:server_sk")).toLatin1());
    if (!stored.isEmpty()) {
        m_sk = openAtRest(m_db->key(), "bfs3 server", stored);
        if (m_sk.size() != 32) { m_sk.clear(); m_error = QStringLiteral("the stored server key does not open with this database key"); return; }
    } else {
        m_sk = x25519Generate(nullptr);
        if (m_sk.size() != 32) { m_error = QStringLiteral("cannot generate the server key"); return; }
        m_db->setKv(QStringLiteral("bfs3:server_sk"), QString::fromLatin1(sealAtRest(m_db->key(), "bfs3 server", m_sk).toBase64()));
        m_db->flush();                                   // the key must never exist only in memory
        qInfo("beaconfix: hub server key generated");
    }
    m_pk = x25519Public(m_sk);
}

void Hub::loadDevices()
{
    m_devices.clear();
    for (const QJsonValue &v : QJsonDocument::fromJson(m_db->kv(QStringLiteral("bfs3:devices")).toUtf8()).array()) {
        const QJsonObject o = v.toObject();
        Device d; d.id = o["id"].toString(); d.name = o["name"].toString(); d.kind = o["kind"].toString(); d.label = o["invitedAs"].toString();
        d.pub = unb64u(o["pub"].toString().toLatin1());
        for (const QJsonValue &sv : o["scopes"].toArray()) d.scopes << sv.toString();
        d.created = QDateTime::fromString(o["created"].toString(), Qt::ISODate); d.lastSeen = QDateTime::fromString(o["lastSeen"].toString(), Qt::ISODate);
        d.lastIp = o["lastIp"].toString(); d.revoked = o["revoked"].toBool(); d.win = ReplayWindow::load(o["win"].toString());
        d.caps = o["capabilities"].toObject(); d.capsTime = QDateTime::fromString(o["capabilitiesTime"].toString(), Qt::ISODate);
        d.lastLease = QDateTime::fromString(o["lastLease"].toString(), Qt::ISODate); d.jobsDone = o["jobsDone"].toInt(); d.jobsFailed = o["jobsFailed"].toInt();
        if (d.pub.size() != 32 || deviceId(d.pub) != d.id) continue;
        const QByteArray shared = x25519(m_sk, d.pub);
        if (shared.isEmpty()) continue;
        d.rk = rootKey(shared, m_pk, d.pub);
        m_devices.insert(d.id, d);
    }
}

void Hub::saveDevices()
{
    QJsonArray arr; for (const Device &d : m_devices) arr.append(d.toJson(true));
    m_db->setKv(QStringLiteral("bfs3:devices"), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

void Hub::loadInvites()
{
    m_invites.clear();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const QJsonValue &v : QJsonDocument::fromJson(m_db->kv(QStringLiteral("bfs3:invites")).toUtf8()).array()) {
        const QJsonObject o = v.toObject();
        InviteRec r; r.id = o["id"].toString(); r.name = o["name"].toString(); r.kind = o["kind"].toString(); r.by = o["by"].toString(); r.expires = qint64(o["expires"].toDouble()); r.used = o["used"].toBool();
        for (const QJsonValue &sv : o["scopes"].toArray()) r.scopes << sv.toString();
        r.secret = openAtRest(m_db->key(), "bfs3 invite", QByteArray::fromBase64(o["secret"].toString().toLatin1()));
        if (r.secret.size() == 32 && r.expires > now - 86400) m_invites << r;        // used / expired ones kept a day (for --devices)
    }
}

void Hub::saveInvites()
{
    QJsonArray arr;
    for (const InviteRec &r : m_invites)
        arr.append(QJsonObject{{"id", r.id}, {"name", r.name}, {"kind", r.kind}, {"by", r.by}, {"scopes", QJsonArray::fromStringList(r.scopes)}, {"expires", double(r.expires)}, {"used", r.used},
                               {"secret", QString::fromLatin1(sealAtRest(m_db->key(), "bfs3 invite", r.secret).toBase64())}});
    m_db->setKv(QStringLiteral("bfs3:invites"), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

// The replay windows: a small sealed file rewritten (atomically) after every accepted request. The database copy
// (in bfs3:devices) is written lazily; on load the higher of the two wins.
QString Hub::windowsFile() const { return m_stateDir + QStringLiteral("/bfs3-counters"); }

void Hub::loadWindows()
{
    QFile f(windowsFile());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray plain = openAtRest(m_db->key(), "bfs3 counters", f.readAll());
    const QJsonObject o = QJsonDocument::fromJson(plain).object();
    for (auto it = o.begin(); it != o.end(); ++it) {
        auto d = m_devices.find(it.key());
        if (d == m_devices.end()) continue;
        const ReplayWindow w = ReplayWindow::load(it.value().toString());
        if (w.highest() >= d->win.highest()) d->win = w;
    }
}

void Hub::saveWindows()
{
    QJsonObject o; for (const Device &d : m_devices) o[d.id] = d.win.save();
    const QByteArray blob = sealAtRest(m_db->key(), "bfs3 counters", QJsonDocument(o).toJson(QJsonDocument::Compact));
    QDir().mkpath(m_stateDir);
    QSaveFile f(windowsFile());
    if (blob.isEmpty() || !f.open(QIODevice::WriteOnly)) { qWarning("beaconfix: hub: cannot write %s", qPrintable(windowsFile())); return; }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(blob);
    if (!f.commit()) qWarning("beaconfix: hub: cannot commit %s", qPrintable(windowsFile()));
}

// ── Admin ─────────────────────────────────────────────────────────────────────
QJsonObject Hub::createInvite(const QString &name, const QString &kind, const QStringList &scopes, int minutes, const QString &invitedBy)
{
    if (!ready()) return {{"error", m_error}};
    InviteRec r; r.by = invitedBy;
    r.id = QString::fromLatin1(random(8).toHex()); r.secret = random(32);
    r.name = name.trimmed().left(64); r.kind = kind.trimmed().toLower().left(16);
    for (const QString &s : scopes) if (ALL_SCOPES.contains(s.trimmed().toLower()) && !r.scopes.contains(s.trimmed().toLower())) r.scopes << s.trimmed().toLower();
    if (r.scopes.isEmpty()) r.scopes = ALL_SCOPES;
    if (!r.scopes.contains(QStringLiteral("read"))) r.scopes.prepend(QStringLiteral("read"));
    r.expires = QDateTime::currentSecsSinceEpoch() + qBound(1, minutes, 15) * 60;
    if (r.id.size() != 16 || r.secret.size() != 32) return {{"error", "rng"}};
    for (int i = m_invites.size() - 1; i >= 0; --i)                // used / expired ones are kept a day (for --status), no longer
        if (m_invites[i].expires < QDateTime::currentSecsSinceEpoch() - 86400) m_invites.removeAt(i);
    m_invites << r;
    saveInvites(); m_db->flush();
    Invite inv; inv.url = m_url; inv.serverPub = m_pk; inv.id = r.id; inv.secret = r.secret; inv.expires = r.expires;
    emit changed();
    return {{"invite", inv.encode()}, {"inviteId", r.id}, {"name", r.name}, {"kind", r.kind}, {"scopes", QJsonArray::fromStringList(r.scopes)},
            {"expires", QDateTime::fromSecsSinceEpoch(r.expires).toString(Qt::ISODate)}, {"url", m_url}, {"fingerprint", fingerprint()}};
}

int Hub::openInvites(const QString &by) const
{
    const qint64 now = QDateTime::currentSecsSinceEpoch(); int n = 0;
    for (const InviteRec &r : m_invites) if (r.by == by && !r.used && r.expires >= now) ++n;
    return n;
}

QJsonArray Hub::devicesJson() const
{
    QJsonArray arr; for (const Device &d : m_devices) arr.append(d.toJson(false));
    return arr;
}

QJsonObject Hub::statusJson() const
{
    QJsonArray inv;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const InviteRec &r : m_invites)
        inv.append(QJsonObject{{"id", r.id}, {"name", r.name}, {"kind", r.kind}, {"by", r.by.isEmpty() ? QStringLiteral("admin") : r.by}, {"status", r.used ? "used" : r.expires < now ? "expired" : "open"},
                               {"expires", QDateTime::fromSecsSinceEpoch(r.expires).toString(Qt::ISODate)}});
    QJsonArray blocked;
    const qint64 ms = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_blockedUntil.begin(); it != m_blockedUntil.end(); ++it) if (it.value() > ms) blocked.append(it.key());
    return {{"ready", ready()}, {"error", m_error}, {"url", m_url}, {"fingerprint", fingerprint()}, {"serverPub", QString::fromLatin1(b64u(m_pk))},
            {"devices", devicesJson()}, {"invites", inv}, {"blocked", blocked}, {"jobs", jobsJson()}};
}

bool Hub::revoke(const QString &idOrName)
{
    bool any = false;
    for (Device &d : m_devices)
        if (!d.revoked && (d.id == idOrName || d.name.compare(idOrName, Qt::CaseInsensitive) == 0)) { d.revoked = true; any = true; }
    if (any) { saveDevices(); m_db->flush(); emit changed(); }
    return any;
}

// ── Enrolment ─────────────────────────────────────────────────────────────────
int Hub::enroll(const QJsonObject &b, const QString &ip, QJsonObject *out)
{
    auto refuse = [&] { noteFailure(ip); *out = QJsonObject{{"error", "enrolment refused"}}; return 403; };
    if (!ready()) return refuse();
    const QString inviteId = b["inviteId"].toString(), name = b["name"].toString(), kind = b["kind"].toString(), pubText = b["pub"].toString(), mac = b["mac"].toString();
    const qint64 ts = b["ts"].isDouble() ? qint64(b["ts"].toDouble()) : b["ts"].toString().toLongLong();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    bool ok = false; const QByteArray pub = unb64u(pubText.toLatin1(), &ok);
    if (!ok || pub.size() != 32 || name.trimmed().isEmpty() || name.size() > 64 || kind.size() > 16 || qAbs(now - ts) > kWindowSecs) return refuse();
    InviteRec *inv = nullptr;
    for (InviteRec &r : m_invites) if (r.id == inviteId) inv = &r;
    if (!inv || inv->used || inv->expires < now) return refuse();
    if (!constantTimeEqual(enrollMac(inv->secret, inviteId, name, kind, pubText, ts).toLatin1(), mac.toLower().toLatin1())) return refuse();
    const QByteArray shared = x25519(m_sk, pub);
    if (shared.isEmpty()) return refuse();
    inv->used = true;
    saveInvites();
    Device d; d.id = deviceId(pub); d.pub = pub; d.name = name.trimmed(); d.kind = kind.trimmed().isEmpty() ? inv->kind : kind.trimmed().toLower();
    d.label = inv->name; d.scopes = inv->scopes.isEmpty() ? ALL_SCOPES : inv->scopes; d.created = QDateTime::currentDateTime(); d.lastIp = ip;
    d.rk = rootKey(shared, m_pk, pub);
    if (m_devices.contains(d.id)) d.win = m_devices[d.id].win;   // the same key enrolled again: its counters stay
    m_devices.insert(d.id, d);
    saveDevices(); saveWindows(); m_db->flush();
    qInfo("beaconfix: hub: enrolled %s (%s, %s) from %s", qPrintable(d.id), qPrintable(d.name), qPrintable(d.kind), qPrintable(ip));
    emit changed();
    *out = QJsonObject{{"deviceId", d.id}, {"fingerprint", fingerprint()}, {"proof", proof(d.rk, d.id)}};
    return 200;
}

// ── Requests ──────────────────────────────────────────────────────────────────
bool Hub::open(const QByteArray &method, const QByteArray &target, const QHash<QByteArray, QByteArray> &h, const QByteArray &body,
               const QString &ip, Session *s, QByteArray *plain)
{
    if (!ready()) return false;
    const QString did = QString::fromLatin1(h.value("x-bf-device").trimmed());
    auto it = m_devices.find(did);
    if (it == m_devices.end() || it->revoked) return false;
    bool okC = false, okT = false, okN = false, okS = true;
    const quint64 c = h.value("x-bf-counter").trimmed().toULongLong(&okC);
    const qint64 ts = h.value("x-bf-time").trimmed().toLongLong(&okT);
    const QByteArray nonce = unb64u(h.value("x-bf-nonce"), &okN);
    const QByteArray sealed = body.isEmpty() ? unb64u(h.value("x-bf-seal"), &okS) : body;   // a bodiless request carries its tag in X-BF-Seal
    if (!okC || !okT || !okN || !okS || nonce.size() != 12 || sealed.size() < 16) return false;
    if (qAbs(QDateTime::currentSecsSinceEpoch() - ts) > kWindowSecs) return false;
    if (!it->win.fresh(c)) return false;
    if (!openRequest(it->rk, did, method, target, c, ts, nonce, sealed, plain)) return false;
    it->win.accept(c);                                   // only after the tag: a forger cannot burn counters
    saveWindows();
    it->lastSeen = QDateTime::currentDateTime(); it->lastIp = ip;
    if (!m_saveTimer.isActive()) m_saveTimer.start();
    s->deviceId = did; s->name = it->name; s->kind = it->kind; s->scopes = it->scopes; s->rk = it->rk; s->counter = c;
    return true;
}

QByteArray Hub::sealResponse(const Session &s, int status, const QByteArray &plain, QByteArray *nonce) const
{
    *nonce = random(12);
    return Bfs3::sealResponse(s.rk, s.deviceId, status, s.counter, *nonce, plain);
}

QByteArray Hub::sealEvent(const Session &s, quint64 seq, const QByteArray &json) const { return Bfs3::sealEvent(s.rk, s.deviceId, s.counter, seq, random(12), json); }

bool Hub::blocked(const QString &ip) const { return m_blockedUntil.value(ip, 0) > QDateTime::currentMSecsSinceEpoch(); }

void Hub::noteFailure(const QString &ip)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<qint64> &l = m_failures[ip];
    while (!l.isEmpty() && l.first() < now - FAIL_SPAN_MS) l.removeFirst();
    l.append(now);
    if (l.size() >= FAIL_MAX) { m_blockedUntil[ip] = now + BLOCK_MS; l.clear(); qWarning("beaconfix: hub: %s blocked for 15 min after %d failures", qPrintable(ip), FAIL_MAX); }
    if (m_failures.size() > 4096) m_failures.clear();    // a spray of sources: forget, keep the blocks
}

// ── Jobs (docs/HUB.md) ────────────────────────────────────────────────────────
// The hub stores and orchestrates; the nodes compute. A job is (type, key) at a watermark (the hub's change sequence
// when its input last changed). A node leases jobs whose watermark it has already pulled (its cursor), computes them on
// its own mirror, and submits; the hub validates and stores the result, which then reaches every node via db/changes.
// A lease expires after BEACONFIX_HUB_LEASE_SECONDS (600) → queued again. A result for a job whose input changed
// meanwhile is stored (it is still the best there is) and the job stays queued at the new watermark.
static const QStringList JOB_TYPES{QStringLiteral("refit")};

void Hub::loadJobs()
{
    for (const QJsonValue &v : QJsonDocument::fromJson(m_db->kv(QStringLiteral("bfs3:jobs")).toUtf8()).array()) {
        const QJsonObject o = v.toObject();
        Job j; j.id = o["id"].toString(); j.type = o["type"].toString(); j.key = o["key"].toString(); j.watermark = qint64(o["watermark"].toDouble());
        j.created = qint64(o["created"].toDouble()); j.attempts = o["attempts"].toInt();
        if (j.id.isEmpty() || !JOB_TYPES.contains(j.type) || j.key.isEmpty() || m_jobIndex.contains(j.type + QLatin1Char('\n') + j.key)) continue;
        m_jobs.insert(j.id, j); m_jobIndex.insert(j.type + QLatin1Char('\n') + j.key, j.id);
    }
    if (!m_jobs.isEmpty()) qInfo("beaconfix: hub: %d job(s) queued", int(m_jobs.size()));
}

void Hub::saveJobs()
{
    QJsonArray arr;
    for (const Job &j : m_jobs) arr.append(QJsonObject{{"id", j.id}, {"type", j.type}, {"key", j.key}, {"watermark", double(j.watermark)}, {"created", double(j.created)}, {"attempts", j.attempts}});
    m_db->setKv(QStringLiteral("bfs3:jobs"), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

int Hub::pendingJobs() const { return int(m_jobs.size()); }

void Hub::enqueue(const QString &type, const QString &key, qint64 watermark)
{
    const QString ik = type + QLatin1Char('\n') + key;
    if (const auto it = m_jobIndex.constFind(ik); it != m_jobIndex.constEnd()) {
        Job &j = m_jobs[*it];
        j.watermark = qMax(j.watermark, watermark);          // a leased job keeps its lease; its result will be older than this → redone
    } else {
        Job j; j.id = QString::fromLatin1(random(8).toHex()); j.type = type; j.key = key; j.watermark = watermark; j.created = QDateTime::currentSecsSinceEpoch();
        m_jobs.insert(j.id, j); m_jobIndex.insert(ik, j.id);
    }
    if (!m_jobSaveTimer.isActive()) m_jobSaveTimer.start();
}

void Hub::sweepLeases()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (Job &j : m_jobs) if (!j.lease.isEmpty() && j.leaseExpires < now) { j.lease.clear(); j.leasedBy.clear(); ++j.attempts; }
}

QJsonObject Hub::heartbeat(const Session &s, const QJsonObject &b)
{
    auto it = m_devices.find(s.deviceId);
    if (it != m_devices.end() && b["capabilities"].isObject()) {
        it->caps = b["capabilities"].toObject(); it->capsTime = QDateTime::currentDateTime();
        if (!m_saveTimer.isActive()) m_saveTimer.start();
    }
    return {{"ok", true}, {"pending", pendingJobs()}, {"time", double(QDateTime::currentSecsSinceEpoch())}, {"leaseSeconds", m_leaseSecs}};
}

QJsonObject Hub::lease(const Session &s, const QJsonObject &b)
{
    sweepLeases();
    auto dev = m_devices.find(s.deviceId);
    if (dev != m_devices.end()) {
        if (b["capabilities"].isObject()) { dev->caps = b["capabilities"].toObject(); dev->capsTime = QDateTime::currentDateTime(); }
        dev->lastLease = QDateTime::currentDateTime();
        if (!m_saveTimer.isActive()) m_saveTimer.start();
    }
    QStringList types; for (const QJsonValue &v : b["types"].toArray()) if (JOB_TYPES.contains(v.toString())) types << v.toString();
    if (b["types"].isUndefined()) types = JOB_TYPES;
    const int max = qBound(0, b["max"].isDouble() ? b["max"].toInt() : 50, 500);
    const qint64 cursor = qint64(b["cursor"].toDouble(-1));  // what the node has pulled: only jobs whose input it already holds
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!types.isEmpty() && max > 0) m_lastLease = QDateTime::currentMSecsSinceEpoch();
    QList<Job *> pick;
    for (Job &j : m_jobs) {                                  // oldest first (QMap order is by id: collect, then sort)
        if (!j.lease.isEmpty() || !types.contains(j.type) || (cursor >= 0 && j.watermark > cursor) || j.triedBy.contains(s.deviceId)) continue;
        pick << &j;
    }
    std::sort(pick.begin(), pick.end(), [](const Job *a, const Job *c) { return a->created != c->created ? a->created < c->created : a->id < c->id; });
    QJsonArray out;
    for (Job *j : pick) {
        if (out.size() >= max) break;
        j->lease = QString::fromLatin1(random(8).toHex()); j->leasedBy = s.deviceId; j->leaseExpires = now + m_leaseSecs; j->leaseWatermark = j->watermark;
        out.append(QJsonObject{{"id", j->id}, {"lease", j->lease}, {"type", j->type}, {"key", j->key}, {"watermark", double(j->watermark)},
                               {"expires", double(j->leaseExpires)}});
    }
    int waiting = 0; for (const Job &j : m_jobs) if (j.lease.isEmpty()) ++waiting;
    return {{"jobs", out}, {"leaseSeconds", m_leaseSecs}, {"pending", waiting}, {"time", double(now)}};
}

int Hub::submit(const Session &s, const QString &jobId, const QJsonObject &b, QJsonObject *out)
{
    auto it = m_jobs.find(jobId);
    if (it == m_jobs.end() || it->lease.isEmpty() || it->leasedBy != s.deviceId || !constantTimeEqual(it->lease.toLatin1(), b["lease"].toString().toLatin1())
        || it->leaseExpires < QDateTime::currentSecsSinceEpoch()) {
        *out = QJsonObject{{"error", "lease expired or superseded"}, {"id", jobId}};
        return 409;                                          // late / duplicate: ignored, the node drops it
    }
    Job &j = *it;
    auto dev = m_devices.find(s.deviceId);
    if (!b["error"].toString().isEmpty() || !b["result"].isObject()) {   // the node could not do it (e.g. no samples there): someone else may
        j.lease.clear(); j.leasedBy.clear(); ++j.attempts;
        if (!j.triedBy.contains(s.deviceId)) j.triedBy << s.deviceId;
        if (dev != m_devices.end()) ++dev->jobsFailed;
        const bool drop = j.attempts >= 5;
        if (drop) { m_jobIndex.remove(j.type + QLatin1Char('\n') + j.key); m_jobs.erase(it); }
        if (!m_jobSaveTimer.isActive()) m_jobSaveTimer.start();
        *out = QJsonObject{{"accepted", false}, {"dropped", drop}, {"id", jobId}};
        return 200;
    }
    if (!m_apply || !m_apply(j.type, j.key, b["result"].toObject())) {
        j.lease.clear(); j.leasedBy.clear(); ++j.attempts;
        if (dev != m_devices.end()) ++dev->jobsFailed;
        *out = QJsonObject{{"error", "invalid result"}, {"id", jobId}};
        return 400;
    }
    if (dev != m_devices.end()) { ++dev->jobsDone; if (!m_saveTimer.isActive()) m_saveTimer.start(); }
    const bool stale = j.watermark > j.leaseWatermark;       // its input changed while it was out: stored, and redone
    if (stale) { j.lease.clear(); j.leasedBy.clear(); j.triedBy.clear(); }
    else { m_jobIndex.remove(j.type + QLatin1Char('\n') + j.key); m_jobs.erase(it); }
    if (!m_jobSaveTimer.isActive()) m_jobSaveTimer.start();
    *out = QJsonObject{{"accepted", true}, {"requeued", stale}, {"id", jobId}};
    return 200;
}

QJsonObject Hub::jobsJson() const
{
    int queued = 0, leased = 0; QJsonObject byType;
    for (const Job &j : m_jobs) { (j.lease.isEmpty() ? queued : leased)++; byType[j.type] = byType[j.type].toInt() + 1; }
    return {{"queued", queued}, {"leased", leased}, {"byType", byType}, {"leaseSeconds", m_leaseSecs}, {"selfComputeIdleMinutes", m_selfIdleMin},
            {"lastLease", m_lastLease ? QJsonValue(QDateTime::fromMSecsSinceEpoch(m_lastLease).toString(Qt::ISODate)) : QJsonValue()}};
}

// The fallback: when no node has leased for idleMinutes and work is waiting, the hub computes (time-boxed batches)
void Hub::setSelfCompute(std::function<bool(const QString &, const QString &)> run, int idleMinutes)
{
    m_selfRun = std::move(run); m_selfIdleMin = qMax(0, idleMinutes);
    if (m_selfIdleMin > 0 && m_selfRun) m_selfTimer.start(); else m_selfTimer.stop();
    if (m_lastLease == 0) m_lastLease = QDateTime::currentMSecsSinceEpoch();   // give the nodes a chance after a start
}

void Hub::selfComputeTick()
{
    if (!m_selfRun || m_selfIdleMin <= 0 || m_jobs.isEmpty()) return;
    const qint64 ms = QDateTime::currentMSecsSinceEpoch();
    if (ms - m_lastLease < qint64(m_selfIdleMin) * 60000) return;
    sweepLeases();
    QElapsedTimer clock; clock.start();
    int n = 0;
    for (auto it = m_jobs.begin(); it != m_jobs.end() && clock.elapsed() < 200;) {
        if (!it->lease.isEmpty()) { ++it; continue; }
        m_selfRun(it->type, it->key);
        m_jobIndex.remove(it->type + QLatin1Char('\n') + it->key);
        it = m_jobs.erase(it); ++n;
    }
    if (n) { qInfo("beaconfix: hub: no compute node for %d min — computed %d job(s) here", m_selfIdleMin, n); if (!m_jobSaveTimer.isActive()) m_jobSaveTimer.start(); }
    if (!m_jobs.isEmpty()) QTimer::singleShot(100, this, &Hub::selfComputeTick);   // drain in slices, the event loop stays responsive
}
