#pragma once
#include "securechannel.h"
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <functional>

class MapDb;

// The hub's side of BFS3 (docs/SECURE-API.md): the server key S_sk (generated on first start, sealed in the
// encrypted map database's kv), the enrolled devices (kv "bfs3:devices"), one-time invites (kv "bfs3:invites"),
// and per-device replay windows (a sealed file next to the database, rewritten after every accepted request so a
// crash never forgets a counter). ApiServer (hub mode, beaconfix --server) routes /api/v3/* through here.
class Hub : public QObject {
    Q_OBJECT
public:
    struct Device {
        QString id, name, kind, label;                 // label: the name the invite was made for
        QByteArray pub, rk;                            // rk: derived at load, never stored
        QStringList scopes;                            // read | sync | control
        QDateTime created, lastSeen;
        QString lastIp;
        bool revoked = false;
        Bfs3::ReplayWindow win;
        QJsonObject caps;                              // capabilities (nodes/heartbeat, jobs/lease): compute, gpu, wifiScan, rtt, mobile, tensor, version, jobs[]
        QDateTime capsTime, lastLease;
        int jobsDone = 0, jobsFailed = 0;
        QJsonObject toJson(bool store) const;
    };
    // A unit of heavy work the hub does not do itself (docs/HUB.md): keyed by what changed, leased by a node
    struct Job {
        QString id, type, key;                         // type "refit": key = BSSID
        qint64 watermark = 0;                          // the hub's change sequence when the job's input last changed
        qint64 created = 0;
        int attempts = 0;
        QString lease, leasedBy;                       // current lease (empty = queued)
        qint64 leaseExpires = 0, leaseWatermark = 0;
        QStringList triedBy;                           // devices that returned an error for it
    };
    struct Session {                                   // one authenticated request
        QString deviceId, name, kind;
        QStringList scopes;
        QByteArray rk;
        quint64 counter = 0;
    };

    Hub(MapDb *db, const QString &stateDir, const QString &url, QObject *parent = nullptr);
    ~Hub() override;

    bool       ready() const { return m_sk.size() == 32; }
    QString    error() const { return m_error; }
    QByteArray serverPub() const { return m_pk; }
    QString    fingerprint() const { return Bfs3::fingerprint(m_pk); }
    QString    url() const { return m_url; }

    // Admin (loopback CLI): a single-use invite valid 15 min; the device list; revocation (id or name)
    // invitedBy: the enrolled device that asked (POST /api/v3/hub/invites, docs/LINKING.md), "" = the admin CLI
    QJsonObject createInvite(const QString &name, const QString &kind, const QStringList &scopes, int minutes = 15, const QString &invitedBy = QString());
    int         openInvites(const QString &invitedBy) const;   // unused, unexpired invites that device made
    QJsonArray  devicesJson() const;
    bool        revoke(const QString &idOrName);
    QJsonObject statusJson() const;

    // POST /api/v3/enroll (plain JSON inside TLS): 200 {deviceId, fingerprint, proof} or 403 {"error":"enrolment refused"}
    int  enroll(const QJsonObject &body, const QString &ip, QJsonObject *out);
    // A sealed request. headers: lower-case names. sealed: the body, or b64u(X-BF-Seal) for a bodiless request.
    // false = 401 (unknown / revoked device, time, replay, tag) — nothing more is said.
    bool open(const QByteArray &method, const QByteArray &target, const QHash<QByteArray, QByteArray> &headers, const QByteArray &body,
              const QString &ip, Session *s, QByteArray *plain);
    QByteArray sealResponse(const Session &s, int status, const QByteArray &plain, QByteArray *nonce) const;
    QByteArray sealEvent(const Session &s, quint64 seq, const QByteArray &json) const;   // the "data:" value

    // ── Jobs (docs/HUB.md) ──
    // enqueue: (type, key) is unique; a newer watermark updates a queued job (a leased one is redone after its result)
    void enqueue(const QString &type, const QString &key, qint64 watermark);
    QJsonObject heartbeat(const Session &s, const QJsonObject &body);                 // POST nodes/heartbeat
    QJsonObject lease(const Session &s, const QJsonObject &body);                     // POST jobs/lease
    int  submit(const Session &s, const QString &jobId, const QJsonObject &body, QJsonObject *out);   // POST jobs/<id>/result
    QJsonObject jobsJson() const;                                                      // GET jobs
    // apply: store one result (type, key, the result object); false = invalid. selfCompute: the hub's fallback.
    void setResultSink(std::function<bool(const QString &type, const QString &key, const QJsonObject &result)> apply) { m_apply = std::move(apply); }
    void setSelfCompute(std::function<bool(const QString &type, const QString &key)> run, int idleMinutes);
    int  pendingJobs() const;

    // Per-source back-off on authentication / enrolment failures (20 in 10 min → refused for 15 min)
    bool blocked(const QString &ip) const;
    void noteFailure(const QString &ip);

signals:
    void changed();

private:
    void loadKeys();
    void loadDevices();
    void saveDevices();
    void loadInvites();
    void saveInvites();
    void loadWindows();
    void saveWindows();
    QString windowsFile() const;
    void loadJobs();
    void saveJobs();
    void sweepLeases();
    void selfComputeTick();

    MapDb *m_db;
    QString m_stateDir, m_url, m_error;
    QByteArray m_sk, m_pk;
    QMap<QString, Device> m_devices;
    struct InviteRec { QString id, name, kind, by; QStringList scopes; QByteArray secret; qint64 expires = 0; bool used = false; };
    QList<InviteRec> m_invites;
    QHash<QString, QList<qint64>> m_failures;          // ip → failure times (ms)
    QHash<QString, qint64> m_blockedUntil;             // ip → ms
    QTimer m_saveTimer;                                // lastSeen / lastIp: written lazily
    // jobs
    QMap<QString, Job> m_jobs;                         // id → job
    QHash<QString, QString> m_jobIndex;                // type + "\n" + key → id
    QTimer m_jobSaveTimer, m_selfTimer;
    qint64 m_lastLease = 0;                            // ms: any compute node leasing
    int m_selfIdleMin = 0;
    int m_leaseSecs = 600;
    std::function<bool(const QString &, const QString &, const QJsonObject &)> m_apply;
    std::function<bool(const QString &, const QString &)> m_selfRun;
};
