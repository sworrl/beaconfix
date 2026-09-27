// BeaconFix device ranging service — see rangingservice.h and docs/RANGING.md §5, §7, §8.
#include "rangingservice.h"
#include "anchors.h"
#include "blelink.h"
#include "../apiserver.h"
#include "../identity.h"
#include "../locator.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <cmath>

using namespace RangeMath;

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
static QString isoMs(qint64 ms) { return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).toString(Qt::ISODateWithMs) : QString(); }
static QJsonValue num(double v, bool ok = true) { return ok && std::isfinite(v) ? QJsonValue(v) : QJsonValue(); }
static const char *kindName(int k)
{
    switch (k) { case KindDesktop: return "desktop"; case KindAndroid: return "android"; case KindLaptop: return "laptop"; case KindPi: return "pi"; case KindGnss: return "gnss"; default: return "device"; }
}
// Virtual APs of one radio differ only in the low bits of the last octet: one fading group (§5.4a)
static QString radioGroup(const QString &bssid)
{
    const QString b = Anchors::normalizeBssid(bssid);
    if (b.size() != 17) return bssid.toUpper();
    bool ok = false; const int last = b.mid(15, 2).toInt(&ok, 16);
    return b.left(15) + QStringLiteral("%1").arg(ok ? (last & 0xF0) : 0, 2, 16, QLatin1Char('0')).toUpper();
}

// Windows: first flush after 5 s so a new peer shows up quickly, then 10 s (§3.2: N_eff grows slowly anyway)
static constexpr double kFirstWindowS = 5, kWindowS = 10;
static constexpr qint64 kFreshMs = 120 * 1000;                // a method counts while its data is < 2 min old
static constexpr qint64 kSessionMs = 60 * 1000;               // a peer posting within this long = active ranging session

RangingService::RangingService(Locator *loc, QObject *parent) : QObject(parent), m_loc(loc)
{
    m_startMs = nowMs();
    load();
    m_tick.setInterval(2000);
    connect(&m_tick, &QTimer::timeout, this, [this] {
        const qint64 now = nowMs();
        bool session = false;
        for (Peer &p : m_peers) {
            bool changed = false;
            if (p.calUntilMs && now >= p.calUntilMs) { finishCalibration(p); changed = true; }
            const int before = p.nBleUp + p.nBleDown;
            flushUp(p, now, false); flushDown(p, now, false);
            if (p.nBleUp + p.nBleDown != before) changed = true;
            if (changed) { recompute(p); emit estimateChanged(p.device); }
            if (p.updatedMs && now - p.updatedMs < kSessionMs) session = true;
        }
        m_raw.erase(std::remove_if(m_raw.begin(), m_raw.end(), [now](const RawBle &r) { return now - r.t > 120000; }), m_raw.end());
        if (m_ble) {                                            // continuous scan + 200 ms adverts only while someone ranges
            m_ble->setScanDuty(session ? 1 : 10, session ? 1 : 30);
            m_ble->setIntervalMs(session ? 200 : 1000);
        }
        updateAdvertFlags();
        // Counters and calibrations survive a tray restart (a kill skips the destructor): write at most once a minute
        if (m_dirty && now - m_savedMs > 60000) { save(); m_savedMs = now; m_dirty = false; }
    });
}

RangingService::~RangingService() { save(); }

void RangingService::start()
{
    m_tick.start();
    if (m_ble) return;
    m_ble = new BleLink(this);
    Identity *idn = m_loc->identity();
    m_ble->setIdentity(idn && idn->exists() ? idn->id() : QString());
    m_ble->setTxPower(kOurBleTxDbm);
    m_ble->setIntervalMs(1000);
    m_ble->setScanDuty(10, 30);
    updateAdvertFlags();
    connect(m_ble, &BleLink::sample, this, &RangingService::onBleSample);
    connect(m_ble, &BleLink::txPowerChanged, this, &RangingService::applyOurTx);
    m_ble->start();
    applyOurTx(m_ble->txPower());
}

// The reference of every down link (a peer hearing us) is our TX power: −59 dBm was the "unknown" prior
// while our advert said 127, ~30 dB below what a 7–10 dBm advert gives at 1 m. When BlueZ cannot set it (byte 8
// stays 127) the level the peer reports for our advert (its TX-power AD fallback) stands in (downTxOf).
void RangingService::applyOurTx(int tx)
{
    m_ourTx = tx;
    for (Peer &p : m_peers) if (!p.calibrated) p.rlsDown = Rls2(priorP0Ble(downTxOf(p)), kNBle);
}

// The Pi agent has no identity of its own. It used to advertise OUR tag (kind pi), so phones counted it as this
// desktop and we dropped it as our own advert. It now advertises this derived id (agent/beaconfix-agent
// beacon_id(), docs/RANGING.md §9.1): SHA-256("beaconfix-agent-beacon-v1|<desktop id>|<device name>"), 26 hex chars.
QString RangingService::agentBeaconId(const QString &desktopId, const QString &device)
{
    return QString::fromLatin1(QCryptographicHash::hash(QStringLiteral("beaconfix-agent-beacon-v1|%1|%2").arg(desktopId, device).toUtf8(),
                                                        QCryptographicHash::Sha256).toHex().left(26));
}

void RangingService::updateAdvertFlags()
{
    if (!m_ble) return;
    bool cal = false; const qint64 now = nowMs();
    for (const Peer &p : m_peers) if (p.calUntilMs > now) cal = true;
    m_ble->setFlags(responderInfo().value(QStringLiteral("enabled")).toBool(), m_loc->apiListening(), KindDesktop, cal);
    Identity *idn = m_loc->identity();
    m_ble->setIdentity(idn && idn->exists() ? idn->id() : QString());
}

// ── info ─────────────────────────────────────────────────────────────────────
QJsonObject RangingService::responderInfo()
{
    QFile f(QStringLiteral("/etc/beaconfix/rtt-responder.json"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    // "enabled" in the file means configured; report whether the AP is actually up right now
    const QString iface = o.value(QStringLiteral("iface")).toString(QStringLiteral("bfrtt0"));
    QFile oper(QStringLiteral("/sys/class/net/%1/operstate").arg(iface));
    const bool up = oper.open(QIODevice::ReadOnly) && !oper.readAll().trimmed().startsWith("down");
    o.insert(QStringLiteral("enabled"), o.value(QStringLiteral("enabled")).toBool() && up);
    return o;
}

QJsonObject RangingService::info() const
{
    QJsonObject rtt = responderInfo();
    const QList<BfAnchor> anchors = m_loc->anchors();
    const BfAnchor *self = nullptr;
    for (const BfAnchor &a : anchors) if (a.kind == QLatin1String("this-computer") && (!self || a.placedAt > self->placedAt)) self = &a;
    QJsonObject o;
    if (!rtt.isEmpty()) {
        QJsonObject r{{"bssid", rtt["bssid"]}, {"freqMHz", rtt["freqMHz"]}, {"centerFreq0MHz", rtt["centerFreq0MHz"]}, {"bandwidthMHz", rtt["bandwidthMHz"]},
                      {"channel", rtt["channel"]}, {"preamble", rtt["preamble"]}, {"enabled", rtt["enabled"]}, {"txPowerDbm", rtt["txPowerDbm"]}};
        for (const BfAnchor &a : anchors) if (a.bssids.contains(rtt["bssid"].toString().toUpper())) r["anchorId"] = a.id;
        if (!r.contains("anchorId") && self) r["anchorId"] = self->id;
        o["rtt"] = r;
    } else {
        o["rtt"] = QJsonValue();
    }
    o["ble"] = QJsonObject{{"serviceUuid", QString::fromLatin1(kBleServiceUuid)}, {"txPower", m_ble ? m_ble->txPower() : 127},
                           {"txPowerConfirmed", m_ble && m_ble->txPowerConfirmed()}, {"enabled", m_ble && m_ble->advertising()},
                           {"scanning", m_ble && m_ble->scanning()}, {"intervalMs", m_ble ? m_ble->intervalMs() : 1000},
                           {"error", m_ble ? QJsonValue(m_ble->lastError()) : QJsonValue()}};
    o["anchor"] = self ? QJsonValue(self->toJson(true)) : QJsonValue();
    return o;
}

// ── peers ────────────────────────────────────────────────────────────────────
RangingService::Peer &RangingService::peer(const QString &device, const QString &kind)
{
    auto it = m_peers.find(device);
    if (it == m_peers.end()) { Peer p; p.device = device; p.rlsDown = Rls2(priorP0Ble(m_ourTx), kNBle); it = m_peers.insert(device, p); }
    if (!kind.isEmpty() && kind != QLatin1String("device")) it->kind = kind;
    return it.value();
}

void RangingService::advance(Peer &p, qint64 tMs)
{
    if (!p.started) { p.started = true; p.lastPredictMs = tMs; return; }
    if (tMs <= p.lastPredictMs) return;
    p.f.predict((tMs - p.lastPredictMs) / 1000.0, p.moving);
    p.lastPredictMs = tMs;
}

void RangingService::flushDown(Peer &p, qint64 nowMs_, bool force)
{
    if (p.winDown.empty()) return;
    const double span = (nowMs_ - p.winDownStartMs) / 1000.0;
    if (!force && span < (p.firstDown ? kFirstWindowS : kWindowS)) return;
    const Level L = levelFromSamples(p.winDown, 3, std::max(1.0, span), p.moving);
    if (L.valid) {
        advance(p, nowMs_);
        // RTT-supervised calibration (§8 "Automatic"): a range pinned by RTT is a known distance for the BLE link —
        // but only once the RTT pair offset itself is calibrated: an uncalibrated pair read ~11 m at 0.6 m and
        // taught the BLE model that bias.
        if (rttCalibrated(p) && std::sqrt(p.f.puu()) < 0.05 && nowMs_ - p.lastRttMs < 30000) p.rlsDown.update(p.f.u(), L.dbm, L.sigma * L.sigma);
        p.f.updateRssi(0, L.dbm, L.sigma, p.rlsDown.p0, p.rlsDown.n);
        p.nBleDown += L.n;
    }
    p.winDown.clear(); p.winDownStartMs = 0; p.firstDown = false;
}

void RangingService::flushUp(Peer &p, qint64 nowMs_, bool force)
{
    if (p.winUp.empty()) return;
    const double span = (nowMs_ - p.winUpStartMs) / 1000.0;
    if (!force && span < (p.firstUp ? kFirstWindowS : kWindowS)) return;
    // Only what BlueZ reported is a measurement; BleLink's held repeats of an unchanged RSSI weight the level by time
    // but add no information (a window of repeats alone is no update at all).
    const Level L = freshLevel(p.winUp, p.winUpFresh, std::max(1.0, span), p.moving);
    if (L.valid) {
        advance(p, nowMs_);
        if (rttCalibrated(p) && std::sqrt(p.f.puu()) < 0.05 && nowMs_ - p.lastRttMs < 30000) p.rlsUp.update(p.f.u(), L.dbm, L.sigma * L.sigma);
        p.f.updateRssi(1, L.dbm, L.sigma, p.rlsUp.p0, p.rlsUp.n);
        p.nBleUp += p.winUpFresh;
    }
    p.winUp.clear(); p.winUpFresh = 0; p.winUpStartMs = 0; p.firstUp = false;
}

RangeMath::Level RangingService::freshLevel(const std::vector<double> &rssi, int fresh, double spanS, bool moving)
{
    Level L = levelFromSamples(rssi, 3, spanS, moving);
    if (!L.valid || fresh >= L.n) return L;
    if (fresh <= 0) { L.valid = false; return L; }
    const double tau = moving ? 0.2 : 30.0;                     // the §3.2 coherence model, with N = the fresh samples
    L.nEff = std::min(double(fresh), 3.0 * (1.0 + std::max(0.0, spanS) / tau));
    L.sigma = kDbPerNeper / std::sqrt(L.nEff);
    return L;
}

void RangingService::wifiDiff(Peer &p, const QJsonArray &theirWifi, qint64 now)
{
    if (theirWifi.isEmpty()) return;
    QHash<QString, int> theirs;
    for (const QJsonValue &v : theirWifi) {
        const QJsonObject w = v.toObject();
        const QString b = Anchors::normalizeBssid(w.value(QStringLiteral("bssid")).toString());
        if (!b.isEmpty()) theirs.insert(b, w.value(QStringLiteral("rssi")).toInt(w.value(QStringLiteral("dbm")).toInt(-100)));
    }
    std::vector<DiffPair> pairs;
    std::vector<GeoPair> geo;
    const Fix &me = m_loc->fix();
    for (const AccessPoint &ap : m_loc->accessPoints()) {
        const QString b = Anchors::normalizeBssid(ap.bssid);
        const auto it = theirs.constFind(b);
        if (it == theirs.constEnd() || it.value() <= -100) continue;
        DiffPair d; d.group = radioGroup(b); d.levelA = ap.dbm; d.levelB = it.value();
        pairs.push_back(d);
        if (!me.valid || !me.precise()) continue;
        const ApEstimate e = m_loc->estimateFor(ap);
        if ((e.kind == ApEstimate::Trilat || e.kind == ApEstimate::Wigle || e.kind == ApEstimate::Peer || e.kind == ApEstimate::Anchor) && e.radiusM > 0 && e.radiusM <= 40) {
            const Anchors::Enu en = Anchors::enu(me.lat, me.lon, std::nan(""), e.lat, e.lon, std::nan(""));
            GeoPair g; g.group = d.group; g.apE = en.e; g.apN = en.n; g.pathloss = m_loc->environmentN(ap.frequency); g.sigmaPos = e.radiusM;
            g.delta = double(it.value() - ap.dbm);
            geo.push_back(g);
        }
    }
    const Fingerprint fp = fingerprintDistance(pairs);
    if (fp.ok) { p.fp = fp; p.fpMs = now; p.nWifiDiff = fp.groups; }
    const GeoSolve g = solveDifferential(geo);
    if (g.ok) { p.geo = g; p.geoMs = now; }
}

QJsonObject RangingService::report(const QString &device, const QString &kind, const QJsonObject &b)
{
    const qint64 now = nowMs();
    Peer &p = peer(device, kind);
    p.moving = b.value(QStringLiteral("moving")).toBool(false);
    // Why its RTT list is (not) empty — "doze" = Android has RTT off until the phone is unlocked, charged or moved
    if (b.value(QStringLiteral("rttState")).isString()) { p.rttState = b.value(QStringLiteral("rttState")).toString().left(40); p.rttStateMs = now; }
    m_dirty = true;
    if (p.calUntilMs && now >= p.calUntilMs) finishCalibration(p);
    const bool calibrating = p.calUntilMs > now;
    const QString responder = responderInfo().value(QStringLiteral("bssid")).toString().toUpper();
    // RTT bursts to our responder, oldest first
    QList<QJsonObject> rtt;
    for (const QJsonValue &v : b.value(QStringLiteral("rtt")).toArray()) rtt << v.toObject();
    std::sort(rtt.begin(), rtt.end(), [](const QJsonObject &x, const QJsonObject &y) { return x["time"].toDouble() < y["time"].toDouble(); });
    for (const QJsonObject &s : rtt) {
        if (!responder.isEmpty() && Anchors::normalizeBssid(s["bssid"].toString()) != responder) continue;   // another responder: a ring around that anchor (future)
        if (!s["distMm"].isDouble()) continue;
        const qint64 t = qint64(s["time"].toDouble(double(now)));
        const double dist = s["distMm"].toDouble() / 1000.0;
        const int n = std::max(1, s["n"].toInt(1));
        const double sigma = std::max(kRttFloor, s["stdMm"].toDouble(1000) / 1000.0 / std::sqrt(double(n)));
        if (calibrating) { p.calRtt.push_back(dist); p.calRttSigma.push_back(sigma); }
        else { advance(p, std::min(t, now)); p.f.updateRtt(dist, sigma, p.rttOffset); }
        ++p.nRtt; p.lastRttMs = std::max(p.lastRttMs, std::min(t, now));
    }
    // What it heard of OUR advert (link 0)
    for (const QJsonValue &v : b.value(QStringLiteral("ble")).toArray()) {
        const QJsonObject s = v.toObject();
        if (!s["rssi"].isDouble()) continue;
        const double r = s["rssi"].toDouble();
        // The TX power the peer read for our advert: stands in for ours while our byte 8 says 127 (see applyOurTx)
        const int tx = s["txPower"].isDouble() ? s["txPower"].toInt() : 127;
        if (tx != 127 && tx >= -127 && tx <= 20 && tx != p.downTx) {
            p.downTx = tx;
            if (m_ourTx == 127 && !p.calibrated) p.rlsDown = Rls2(priorP0Ble(tx), kNBle);
        }
        if (calibrating) { p.calDown.push_back(r); continue; }
        if (p.winDown.empty()) p.winDownStartMs = std::min(now, qint64(s["time"].toDouble(double(now))));
        p.winDown.push_back(r);
        p.lastBleDownMs = now;
    }
    if (!calibrating) flushDown(p, now, false);
    wifiDiff(p, b.value(QStringLiteral("wifi")).toArray(), now);
    // Its fix vs ours: a (weak) Gaussian on the relative position
    const QJsonObject fx = b.value(QStringLiteral("fix")).toObject();
    const Fix &me = m_loc->fix();
    if (fx["lat"].isDouble() && fx["lon"].isDouble() && me.valid && me.precise()) {
        const Anchors::Enu en = Anchors::enu(me.lat, me.lon, std::nan(""), fx["lat"].toDouble(), fx["lon"].toDouble(), std::nan(""));
        const double acc = std::max(3.0, fx["acc"].toDouble(50)), myAcc = std::max(1.0, me.accuracy);
        p.fix.valid = true; p.fix.muE = en.e; p.fix.muN = en.n; p.fix.sEE = p.fix.sNN = acc * acc + myAcc * myAcc; p.fix.sEN = 0;
        p.fixMs = now;
    }
    p.updatedMs = now;
    recompute(p);
    emit estimateChanged(p.device);
    return toJson(p);
}

// ── BLE from the scanner (link 1: we hear the peer) ──────────────────────────
QString RangingService::resolveTag(const QByteArray &tag, int kind, qint64 now, bool *own)
{
    const qint64 w = now / 1000 / 900;
    if (w != m_tagWindow || m_tags.isEmpty()) {
        m_tags.clear(); m_bound.clear(); m_tagWindow = w;
        QHash<QString, TagOwner> ids;                           // identity id → owner
        Identity *idn = m_loc->identity();
        const QString ownId = idn && idn->exists() ? idn->id() : QString();
        if (ApiServer *api = m_loc->apiServer())
            for (const ApiServer::Device &d : api->devices()) {
                if (d.revoked) continue;
                if (!d.identity.isEmpty()) {
                    const QString k = m_loc->kindForDevice(d.name, d.kind);
                    ids.insert(d.identity, TagOwner{d.name, false, k == QLatin1String("desktop") || k == QLatin1String("laptop")});
                } else if (!ownId.isEmpty()) {
                    ids.insert(agentBeaconId(ownId, d.name), TagOwner{d.name, false, false});   // a Pi agent's beacon
                }
            }
        if (idn) {
            for (const PendingLink &pl : idn->pending()) if (!ids.contains(pl.id)) ids.insert(pl.id, TagOwner{pl.deviceName.isEmpty() ? pl.name : pl.deviceName, false, false});
            for (const QString &id : idn->linkedIds()) if (!ids.contains(id)) ids.insert(id, TagOwner{QStringLiteral("identity ") + Identity::groupId(id), false, false});
        }
        // Our own adverts (another device sharing our identity is told apart by name elsewhere)
        if (!ownId.isEmpty()) ids.insert(ownId, TagOwner{QString(), true, true});
        for (auto it = ids.constBegin(); it != ids.constEnd(); ++it)
            for (int dw = -1; dw <= 1; ++dw) m_tags.insert(bleTag(it.key(), (w + dw) * 900), it.value());
    }
    const auto it = m_tags.constFind(tag);
    if (it != m_tags.constEnd()) {
        // A desktop's identity tag belongs only to that desktop's own advert. Before 3.8 the Pi agent advertised its
        // desktop's identity with kind pi: that advert is neither us nor that desktop, so it is resolved like an unknown one.
        if (!it->desktop || kind == KindDesktop || kind == KindLaptop) {
            if (it->self) *own = true;
            return it->device;
        }
    }
    return m_bound.value(tag + char(kind & 7));
}

void RangingService::onBleSample(const QByteArray &tag, int rssi, int txPower, int kind, int flags, qint64 timeMs, const QString &address, bool held)
{
    Q_UNUSED(flags); Q_UNUSED(address);
    const qint64 now = nowMs();
    if (!held) m_raw.push_back({tag, rssi, timeMs, txPower});  // pairing proximity counts reported adverts only
    bool own = false;
    QString device = resolveTag(tag, kind, now, &own);
    if (own) return;                                            // our own advert
    if (device.isEmpty()) {
        // Session binding: an unknown tag of the same kind as the one peer that is ranging with us right now
        const Peer *only = nullptr; int n = 0;
        for (const Peer &p : m_peers) if (now - p.updatedMs < 30000 && p.kind == QLatin1String(kindName(kind))) { only = &p; ++n; }
        if (n != 1) return;
        device = only->device;
        m_bound.insert(tag + char(kind & 7), device);
    }
    Peer &p = peer(device, QString::fromLatin1(kindName(kind)));
    if (!p.calibrated && (!p.haveUpPrior || (txPower != p.upTx && txPower != 127))) { p.rlsUp = Rls2(priorP0Ble(txPower), kNBle); p.haveUpPrior = true; }
    if (txPower != 127) p.upTx = txPower;
    m_dirty = true;
    if (p.calUntilMs > now) { p.calUp.push_back(rssi); if (!held) ++p.calUpFresh; return; }
    if (p.winUp.empty()) p.winUpStartMs = now;
    p.winUp.push_back(rssi);
    if (!held) ++p.winUpFresh;
    p.lastBleUpMs = now;                                        // held repeats too: BleLink sends them only while it hears the device
}

// ── calibration (§8) ─────────────────────────────────────────────────────────
// The RTT ranges of a calibration window, reduced robustly: the median of the densest cluster [x, x + width].
// A plain median was not enough: in the 0.6 m calibration of 2026-09-27 the Pixel reported 8 bursts at
// 14.4–14.7 m and 7 at 196–408 m (bursts whose timestamps the responder got wrong), so the median sat on the
// cluster's edge, and one more bad burst would have made the offset ~200 m. Time-of-flight errors are late,
// so among equally dense clusters the nearest wins. Returns the cluster size (0 = no samples).
int RangingService::rttCluster(std::vector<double> v, double width, double *center, double *lo, double *hi)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t bi = 0, bj = 0;                                      // best window [bi, bj)
    for (size_t i = 0, j = 0; i < v.size(); ++i) {
        j = std::max(j, i);
        while (j < v.size() && v[j] - v[i] <= width) ++j;
        if (j - i > bj - bi) { bi = i; bj = j; }
    }
    if (center) *center = median(std::vector<double>(v.begin() + qsizetype(bi), v.begin() + qsizetype(bj)));
    if (lo) *lo = v[bi];                                        // the earliest densest window: every value in [lo, hi] is in it
    if (hi) *hi = v[bj - 1];
    return int(bj - bi);
}

double RangingService::rttInlierSigma(const std::vector<double> &dist, const std::vector<double> &sigma, double lo, double hi)
{
    double s2 = 0, m = 0, m2 = 0; int n = 0;
    for (size_t i = 0; i < dist.size() && i < sigma.size(); ++i) {
        if (dist[i] < lo || dist[i] > hi) continue;             // an outlier's σ says nothing about the cluster
        s2 += sigma[i] * sigma[i]; m += dist[i]; m2 += dist[i] * dist[i]; ++n;
    }
    if (n == 0) return kRttFloor;
    const double mean = m / n;
    const double scatter = n >= 3 ? std::sqrt(std::max(0.0, (m2 - n * mean * mean) / (n - 1))) : 0.0;
    return std::max({kRttFloor, std::sqrt(s2 / n), scatter});
}

// Three or more bursts that do not agree make the whole window suspect (the phone moved, heavy multipath): then nothing
// changes — no RTT offset, no BLE model, not "calibrated" — rather than pinning BLE to a distance RTT disputes.
QString RangingService::calibrationFailure(int bursts, int agreed, bool bleUsable)
{
    const bool rttOk = rttClusterOk(bursts, agreed);
    if (bursts >= 3 && !rttOk) return QStringLiteral("only %1 of %2 RTT bursts agreed within %3 m").arg(agreed).arg(bursts).arg(kCalClusterM, 0, 'f', 0);
    if (!rttOk && !bleUsable) return QStringLiteral("no usable RTT bursts and too few BLE samples in the window");
    return QString();
}

QJsonObject RangingService::calibrate(const QString &device, double distanceM, int durationS)
{
    if (!(distanceM > 0.05 && distanceM < 100)) return QJsonObject{{"error", "distanceM must be between 0.05 and 100"}};
    Peer &p = peer(device, QString());
    const qint64 now = nowMs();
    p.calStartMs = now;
    p.calUntilMs = now + qint64(std::clamp(durationS, 5, 120)) * 1000;
    p.calTarget = distanceM;
    p.calRtt.clear(); p.calRttSigma.clear(); p.calDown.clear(); p.calUp.clear(); p.calUpFresh = 0;
    updateAdvertFlags();
    return QJsonObject{{"device", device}, {"calibrating", true}, {"distanceM", distanceM}, {"until", isoMs(p.calUntilMs)},
                       {"hint", "keep both devices at that distance; nudge the phone a few centimetres now and then so the fade averages out"}};
}

void RangingService::finishCalibration(Peer &p)
{
    const double D = p.calTarget;
    const double span = std::max(5.0, (p.calUntilMs - p.calStartMs) / 1000.0);
    p.calUntilMs = 0;
    double center = 0, lo = 0, hi = 0;
    const int nBursts = int(p.calRtt.size());
    const int nRtt = rttCluster(p.calRtt, kCalClusterM, &center, &lo, &hi);
    const bool rttOk = rttClusterOk(nBursts, nRtt);
    const Level la = p.calDown.size() >= 3 ? levelFromSamples(p.calDown, 3, span, false) : Level();
    const Level lb = p.calUp.size() >= 3 ? freshLevel(p.calUp, p.calUpFresh, span, false) : Level();
    const QString at = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QString fail = calibrationFailure(nBursts, nRtt, la.valid || lb.valid);
    QString text;
    if (!fail.isEmpty()) {
        text = QStringLiteral("Ranging calibration with %1 at %2 m failed: %3; nothing was changed").arg(p.device).arg(D, 0, 'f', 2).arg(fail);
    } else {
        if (rttOk) {
            p.rttOffset = center - D;
            // The median of the N cluster bursts: σ ≈ 1.2533·σ_b/√N, σ_b from the inliers only, plus 5 cm for the tape measure
            const double sc = 1.2533 * rttInlierSigma(p.calRtt, p.calRttSigma, lo, hi) / std::sqrt(double(nRtt));
            p.rttOffsetVar = sc * sc + 0.05 * 0.05;
            p.f.resetRttOffset(0, p.rttOffsetVar);
            p.rttCal = true;
        }
        // A calibration replaces what the links learnt before: start each link that has calibration samples from its
        // TX-power prior (a link without samples keeps its previous calibration).
        if (la.valid) { p.rlsDown = Rls2(priorP0Ble(downTxOf(p)), kNBle); p.rlsDown.update(std::log10(D), la.dbm, la.sigma * la.sigma); p.f.resetOffset(0, 0, std::max(la.sigma * la.sigma, kFrozenFadeVar) + kFrozenFadeVar); }
        if (lb.valid) { p.rlsUp = Rls2(priorP0Ble(p.upTx), kNBle); p.rlsUp.update(std::log10(D), lb.dbm, lb.sigma * lb.sigma); p.f.resetOffset(1, 0, std::max(lb.sigma * lb.sigma, kFrozenFadeVar) + kFrozenFadeVar); }
        p.f.updateLogRange(std::log10(D), 0.01);               // they were D apart when the window closed
        p.calibrated = true; p.calibratedAt = at; p.calDistM = D;
        text = QStringLiteral("Ranging calibrated with %1 at %2 m (%3; %4 + %5 BLE samples)").arg(p.device).arg(D, 0, 'f', 2)
            .arg(rttOk ? QStringLiteral("%1 of %2 RTT bursts agreed, offset %3 m ± %4").arg(nRtt).arg(nBursts).arg(p.rttOffset, 0, 'f', 2).arg(std::sqrt(p.rttOffsetVar), 0, 'f', 2)
                       : QStringLiteral("%1 RTT burst%2: offset not changed").arg(nBursts).arg(nBursts == 1 ? "" : "s"))
            .arg(p.calDown.size()).arg(p.calUpFresh);
    }
    p.lastCal = QJsonObject{{"at", at}, {"ok", fail.isEmpty()}, {"distanceM", D}, {"rttBursts", nBursts}, {"rttAgreed", nRtt}, {"text", text}};
    m_loc->logEvent([&] {
        BeaconEvent ev; ev.type = QStringLiteral("device"); ev.text = text;
        ev.extra = QJsonObject{{"device", p.device}, {"calibrated", fail.isEmpty()}, {"distanceM", D}, {"rttOffsetM", p.rttOffset},
                               {"rttBursts", nBursts}, {"rttAgreed", nRtt}};
        return ev; }());
    p.calRtt.clear(); p.calRttSigma.clear(); p.calDown.clear(); p.calUp.clear(); p.calUpFresh = 0;
    recompute(p);
    save();
    updateAdvertFlags();
}

// ── posterior ────────────────────────────────────────────────────────────────
void RangingService::recompute(Peer &p)
{
    const qint64 now = nowMs();
    RelInput in;
    in.haveRange = p.started && (p.nRtt + p.nBleDown + p.nBleUp) > 0;
    in.u = p.f.u(); in.puu = p.f.puu();
    if (p.fp.ok && now - p.fpMs < kFreshMs) { in.haveFp = true; in.fp = p.fp; }
    if (p.geo.ok && now - p.geoMs < kFreshMs) { in.geo.valid = true; in.geo.muE = p.geo.dE; in.geo.muN = p.geo.dN; in.geo.sEE = p.geo.cov[0][0]; in.geo.sEN = p.geo.cov[0][1]; in.geo.sNN = p.geo.cov[1][1]; }
    if (p.fix.valid && now - p.fixMs < 5 * 60 * 1000) in.fix = p.fix;
    p.out = relativePosterior(in);
    p.method.clear();
    if (p.lastRttMs && now - p.lastRttMs < kFreshMs) p.method << QStringLiteral("rtt");
    if ((p.lastBleUpMs && now - p.lastBleUpMs < kFreshMs) || (p.lastBleDownMs && now - p.lastBleDownMs < kFreshMs)) p.method << QStringLiteral("ble");
    if (in.haveFp) p.method << QStringLiteral("wifi-diff");
    if (in.geo.valid) p.method << QStringLiteral("wifi-geo");
    if (in.fix.valid) p.method << QStringLiteral("fix");
}

QJsonObject RangingService::toJson(const Peer &p) const
{
    const RelOutput &o = p.out;
    QJsonObject j{{"device", p.device}, {"kind", p.kind.isEmpty() ? QStringLiteral("device") : p.kind},
                  {"distanceM", num(o.distanceM, o.valid)}, {"sigmaM", num(o.sigmaM, o.valid)}, {"lowM", num(o.lowM, o.valid)}, {"highM", num(o.highM, o.valid)},
                  {"method", QJsonArray::fromStringList(p.method)},
                  {"bearingDeg", num(o.bearingDeg, o.valid && o.haveBearing)}, {"bearingSigmaDeg", num(o.bearingSigmaDeg, o.valid && o.haveBearing)}, {"dz", QJsonValue()},
                  {"class", o.valid ? o.cls : QStringLiteral("unknown")}, {"updated", isoMs(std::max({p.updatedMs, p.lastBleUpMs, p.lastRttMs}))},
                  // counts since this tray started ("since"); "total" adds earlier runs (ranging.json)
                  {"samples", QJsonObject{{"rtt", p.nRtt}, {"ble", p.nBleDown + p.nBleUp}, {"bleDown", p.nBleDown}, {"bleUp", p.nBleUp}, {"wifiDiff", p.nWifiDiff},
                                          {"since", isoMs(m_startMs)},
                                          {"total", QJsonObject{{"rtt", double(p.prevRtt + p.nRtt)}, {"bleDown", double(p.prevBleDown + p.nBleDown)}, {"bleUp", double(p.prevBleUp + p.nBleUp)}}}}},
                  {"lastRtt", std::max(p.lastRttMs, p.prevLastRttMs) > 0 ? QJsonValue(isoMs(std::max(p.lastRttMs, p.prevLastRttMs))) : QJsonValue()},
                  // the peer's own word on its RTT: ok | doze | wifi-off | location-off | idle | no-response | … (docs/API.md)
                  {"rttState", p.rttState.isEmpty() ? QJsonValue() : QJsonValue(p.rttState)}, {"rttStateAt", p.rttStateMs ? QJsonValue(isoMs(p.rttStateMs)) : QJsonValue()},
                  {"calib", QJsonObject{{"rttOffsetM", p.rttOffset}, {"rttOffsetSigmaM", std::sqrt(std::max(0.0, p.rttOffsetVar))}, {"bleP0", p.rlsDown.p0}, {"bleN", p.rlsDown.n},
                                        {"bleP0Up", p.rlsUp.p0}, {"bleNUp", p.rlsUp.n}, {"calibrated", p.calibrated},
                                        {"calibratedAt", p.calibratedAt.isEmpty() ? QJsonValue() : QJsonValue(p.calibratedAt)}, {"distanceM", p.calibrated ? QJsonValue(p.calDistM) : QJsonValue()},
                                        {"rttCalibrated", rttCalibrated(p)}, {"last", p.lastCal.isEmpty() ? QJsonValue() : QJsonValue(p.lastCal)}}},
                  {"calibrating", p.calUntilMs > nowMs()}};
    // Where that puts it on the map: our position ⊕ (bearing, distance), only when the bearing is real
    const Fix &me = m_loc->fix();
    if (o.valid && o.haveBearing && me.valid) {
        double lat = 0, lon = 0, alt = 0;
        const double br = o.bearingDeg * M_PI / 180.0;
        Anchors::fromEnu(me.lat, me.lon, 0, Anchors::Enu{o.distanceM * std::sin(br), o.distanceM * std::cos(br), 0}, &lat, &lon, &alt);
        j["lat"] = lat; j["lon"] = lon;
    }
    return j;
}

QJsonObject RangingService::estimateJson(const QString &device) const
{
    const auto it = m_peers.constFind(device);
    if (it == m_peers.constEnd() || !(it->started || it->fp.ok || it->fix.valid)) return {};
    return toJson(it.value());
}

QString RangingService::kindOf(const QString &device) const
{
    const auto it = m_peers.constFind(device);
    return it == m_peers.constEnd() ? QString() : it->kind;
}

QJsonObject RangingService::list() const
{
    QJsonArray devs; qint64 newest = 0;
    QList<const Peer *> l; for (const Peer &p : m_peers) if (p.started || p.fp.ok || p.fix.valid) l << &p;
    std::sort(l.begin(), l.end(), [](const Peer *a, const Peer *b) { return a->updatedMs > b->updatedMs; });
    for (const Peer *p : l) { devs.append(toJson(*p)); newest = std::max({newest, p->updatedMs, p->lastBleUpMs}); }
    const QJsonObject inf = info();
    return QJsonObject{{"updated", newest ? QJsonValue(isoMs(newest)) : QJsonValue()}, {"anchor", inf["anchor"]}, {"devices", devs}};
}

QJsonObject RangingService::pairingProximity(const QJsonObject &theirs, const QString &identityId) const
{
    const qint64 now = nowMs();
    RelInput in;
    QStringList method;
    // BLE: its advert, found by the tag its identity produces
    if (!identityId.isEmpty()) {
        const qint64 w = now / 1000 / 900;
        QSet<QByteArray> tags; for (int dw = -1; dw <= 1; ++dw) tags.insert(bleTag(identityId, (w + dw) * 900));
        std::vector<double> r; qint64 t0 = 0; int tx = 127;
        for (const RawBle &s : m_raw) if (tags.contains(s.tag) && now - s.t < 60000) { r.push_back(s.rssi); if (!t0) t0 = s.t; tx = s.tx; }
        if (r.size() >= 3) {
            const Level L = levelFromSamples(r, 3, std::max(1.0, (now - t0) / 1000.0), false);
            if (L.valid) {
                RangeFilter f(2);
                f.updateRssi(1, L.dbm, L.sigma, priorP0Ble(tx), kNBle);
                in.haveRange = true; in.u = f.u(); in.puu = f.puu();
                method << QStringLiteral("ble");
            }
        }
    }
    // Shared beacons → fingerprint distance
    std::vector<DiffPair> pairs;
    QHash<QString, int> mine;
    for (const AccessPoint &ap : m_loc->accessPoints()) mine.insert(Anchors::normalizeBssid(ap.bssid), ap.dbm);
    for (const QJsonValue &v : theirs.value(QStringLiteral("beacons")).toArray()) {
        const QJsonObject b = v.toObject();
        const QString mac = Anchors::normalizeBssid(b["bssid"].toString());
        const auto it = mine.constFind(mac);
        if (mac.isEmpty() || it == mine.constEnd()) continue;
        DiffPair d; d.group = radioGroup(mac); d.levelA = it.value(); d.levelB = b["dbm"].toInt(-100);
        if (d.levelB > -100) pairs.push_back(d);
    }
    const Fingerprint fp = fingerprintDistance(pairs);
    if (fp.ok) { in.haveFp = true; in.fp = fp; method << QStringLiteral("wifi-diff"); }
    const Fix &me = m_loc->fix();
    if (theirs["lat"].isDouble() && theirs["lon"].isDouble() && me.valid && me.precise() && theirs["source"].toString() != QLatin1String("ip")) {
        const Anchors::Enu en = Anchors::enu(me.lat, me.lon, std::nan(""), theirs["lat"].toDouble(), theirs["lon"].toDouble(), std::nan(""));
        const double acc = std::max(3.0, theirs["acc"].toDouble(50));
        in.fix.valid = true; in.fix.muE = en.e; in.fix.muN = en.n; in.fix.sEE = in.fix.sNN = acc * acc + std::max(1.0, me.accuracy) * std::max(1.0, me.accuracy);
        method << QStringLiteral("fix");
    }
    const RelOutput o = relativePosterior(in);
    return QJsonObject{{"class", o.valid ? o.cls : QStringLiteral("unknown")}, {"distanceM", num(o.distanceM, o.valid)}, {"lowM", num(o.lowM, o.valid)},
                       {"highM", num(o.highM, o.valid)}, {"sigmaM", num(o.sigmaM, o.valid)}, {"method", QJsonArray::fromStringList(method)},
                       {"sharedGroups", fp.groups}, {"evidence", o.valid && !method.isEmpty()}};
}

// ── persistence: calibrations survive restarts; the filter state does not need to ──
QString RangingService::statePath() { return Locator::stateDir() + QStringLiteral("/ranging.json"); }

void RangingService::save() const
{
    QJsonObject peers;
    for (const Peer &p : m_peers) {
        const qint64 tRtt = p.prevRtt + p.nRtt, tDown = p.prevBleDown + p.nBleDown, tUp = p.prevBleUp + p.nBleUp;
        if (!p.calibrated && !p.haveUpPrior && !tRtt && !tDown && !tUp) continue;
        auto rls = [](const Rls2 &r) { return QJsonObject{{"p0", r.p0}, {"n", r.n}, {"S", QJsonArray{r.S[0][0], r.S[0][1], r.S[1][1]}}}; };
        peers[p.device] = QJsonObject{{"kind", p.kind}, {"rttOffset", p.rttOffset}, {"rttOffsetVar", p.rttOffsetVar}, {"calibrated", p.calibrated},
                                      {"rttCalibrated", p.rttCal}, {"lastCal", p.lastCal},
                                      {"calibratedAt", p.calibratedAt}, {"calDistM", p.calDistM}, {"rlsDown", rls(p.rlsDown)}, {"rlsUp", rls(p.rlsUp)},
                                      {"offsetVar", QJsonArray{p.f.offsetVar(0), p.f.offsetVar(1)}},
                                      {"totals", QJsonObject{{"rtt", double(tRtt)}, {"bleDown", double(tDown)}, {"bleUp", double(tUp)},
                                                             {"lastRttMs", double(std::max(p.lastRttMs, p.prevLastRttMs))}}}};
    }
    QDir().mkpath(Locator::stateDir());
    QSaveFile f(statePath());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(QJsonObject{{"version", 1}, {"peers", peers}}).toJson(QJsonDocument::Indented));
    f.commit();
}

void RangingService::load()
{
    QFile f(statePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject peers = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("peers")).toObject();
    for (auto it = peers.begin(); it != peers.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        Peer &p = peer(it.key(), o["kind"].toString());
        auto rls = [](const QJsonObject &j, Rls2 &r) {
            if (j.isEmpty()) return;
            r.p0 = j["p0"].toDouble(r.p0); r.n = j["n"].toDouble(r.n);
            const QJsonArray S = j["S"].toArray();
            if (S.size() == 3) { r.S[0][0] = S[0].toDouble(); r.S[0][1] = r.S[1][0] = S[1].toDouble(); r.S[1][1] = S[2].toDouble(); }
        };
        const QJsonObject tot = o["totals"].toObject();
        p.prevRtt = qint64(tot["rtt"].toDouble()); p.prevBleDown = qint64(tot["bleDown"].toDouble()); p.prevBleUp = qint64(tot["bleUp"].toDouble());
        p.prevLastRttMs = qint64(tot["lastRttMs"].toDouble());
        p.calibrated = o["calibrated"].toBool(); p.calibratedAt = o["calibratedAt"].toString(); p.calDistM = o["calDistM"].toDouble();
        p.lastCal = o["lastCal"].toObject();
        // Only a calibration is worth restoring. What an uncalibrated peer's links "learnt" came from the automatic
        // RTT supervision, which trusted an uncalibrated pair (~11 m for 0.6 m): back to the priors.
        if (!p.calibrated) continue;
        rls(o["rlsDown"].toObject(), p.rlsDown); rls(o["rlsUp"].toObject(), p.rlsUp);
        p.haveUpPrior = true;
        p.rttOffset = o["rttOffset"].toDouble(); p.rttOffsetVar = o["rttOffsetVar"].toDouble(kUncalRttOffsetVar);
        // Files before 3.8 have no flag: an RTT-measured offset is the one whose variance is not a prior (0.25, then 4)
        p.rttCal = o.contains(QStringLiteral("rttCalibrated")) ? o["rttCalibrated"].toBool() : p.rttOffsetVar < 0.2499;
        p.f.resetRttOffset(0, p.rttOffsetVar);
        const QJsonArray ov = o["offsetVar"].toArray();
        for (int k = 0; k < 2 && k < ov.size(); ++k) p.f.resetOffset(k, 0, std::max(ov[k].toDouble(kOffsetVar0), 2.0 * kFrozenFadeVar));
    }
}
