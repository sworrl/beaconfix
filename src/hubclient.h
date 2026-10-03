#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <functional>

class Locator;

// This node's link to the hub (docs/SECURE-API.md, BFS3): enrolment from a "bfs3:" invite, then every request
// sealed with this device's root key, every response checked against it. The keys and the hub's identity live in
// the encrypted map database (kv "hub:config"; D_sk additionally sealed with the database key), the counter is
// reserved ahead in blocks (kv "hub:counter") so a crash never reuses one.
//
// Work, on timers: Locator::syncRound() with the hub (push our own samples / stops / anchors, pull everything every
// node gave it, incl. the hub's fits) every hub/syncMinutes (2); our live position out and every node's position in
// (devices/position, devices/positions → linked devices on the map and in the widget) every hub/positionSeconds (30).
// Offline: nothing is lost (cursors only move on success), retries back off from 30 s to 15 min.
// Compute (docs/HUB.md): after each sync and every minute the node leases jobs the hub queued (refits of BSSIDs whose
// samples changed), runs them on its own mirror in time-boxed slices, and submits the results; hub/compute=false
// opts out. Capabilities go to the hub with every lease and a heartbeat every 10 min.
//
// Settings (QSettings hub/…, or the environment for tests): caFile / BEACONFIX_HUB_CA (an extra CA for a private
// hub certificate), tlsInsecure / BEACONFIX_HUB_TLS_INSECURE=1 (skip TLS verification — BFS3 still authenticates
// the hub: a response that does not open with our root key is refused).
class HubClient : public QObject {
    Q_OBJECT
public:
    explicit HubClient(Locator *loc, QObject *parent = nullptr);

    bool        enrolled() const { return m_rk.size() == 32 && !m_deviceId.isEmpty(); }
    QString     name() const { return m_name; }
    QJsonObject statusJson() const;

    using Reply = std::function<void(int status, const QByteArray &plain, const QString &error)>;
    // One sealed request: endpoint after /api/v3/ (with its query), plain body (empty for GET). error is set for a
    // transport failure, a non-2xx status (the hub's "error" text) or a response that fails authentication.
    void request(const QByteArray &method, const QString &endpoint, const QByteArray &body, Reply reply, int timeoutMs = 180000);

    void enroll(const QString &invite, const QString &name, std::function<void(bool ok, const QJsonObject &result)> done);
    // POST /api/v3/hub/invites (docs/LINKING.md): a fresh single-use invite for a device this PC links; "" + error when
    // not enrolled / the hub is unreachable (WireGuard off) — the phone is linked on the LAN anyway
    void requestInvite(const QString &name, const QString &kind, std::function<void(const QString &invite, const QString &error)> done, int timeoutMs = 8000);
    static QJsonObject capabilities();                       // compute (cores), gpu, wifiScan (interfaces), rtt, mobile, tensor, version, jobs[], role
    void forget();
    void syncNow(std::function<void(bool ok, const QString &message)> done = {});

signals:
    void changed();

private:
    void load();
    void tick();
    void positions();
    void jobs();
    void heartbeat();
    quint64 nextCounter();
    QNetworkRequest baseRequest(const QUrl &url, int timeoutMs) const;
    static QString detectKind();

    Locator *m_loc;
    QNetworkAccessManager m_nam;
    QString m_url, m_deviceId, m_name, m_kind, m_fingerprint;
    QByteArray m_spk, m_rk;
    quint64 m_counter = 0, m_reserved = 0;
    QTimer m_syncTimer, m_posTimer, m_jobTimer;
    bool m_jobBusy = false;
    int m_jobsDone = 0, m_jobsFailed = 0;
    QDateTime m_lastHeartbeat, m_lastJobs;
    bool m_busy = false, m_posBusy = false, m_lastOk = false;
    int m_failures = 0;
    QDateTime m_lastSync, m_nextSync, m_lastPublish, m_publishedFix;
    QString m_lastResult, m_lastError;
    QList<std::function<void(bool, const QString &)>> m_waiters;
};
