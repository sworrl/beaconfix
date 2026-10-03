// SPDX-License-Identifier: Apache-2.0
#include "routeplanner.h"
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <cmath>

#ifndef BEACONFIX_VERSION
#define BEACONFIX_VERSION "dev"
#endif

using namespace AvoidRoute;

RoutePlanner::RoutePlanner(CameraSource cameras, QObject *parent) : QObject(parent), m_cameras(std::move(cameras)) {}

static QString settingsName(Provider p) { return p == Provider::Ors ? QStringLiteral("routing/orsKey") : QStringLiteral("routing/graphhopperKey"); }
QString RoutePlanner::storedKey(Provider p) { return QSettings().value(settingsName(p)).toString().trimmed(); }
void RoutePlanner::storeKey(Provider p, const QString &key) { QSettings().setValue(settingsName(p), key.trimmed()); }
QString RoutePlanner::defaultProvider()
{
    Provider p;
    return providerFromId(QSettings().value(QStringLiteral("routing/provider")).toString(), &p) ? providerId(p) : QStringLiteral("ors");
}
void RoutePlanner::setDefaultProvider(const QString &id) { Provider p; if (providerFromId(id, &p)) QSettings().setValue(QStringLiteral("routing/provider"), providerId(p)); }

QJsonObject RoutePlanner::status() const
{
    QJsonArray ps;
    bool any = false;
    for (Provider p : {Provider::Ors, Provider::GraphHopper}) {
        const bool has = !keyFor(p).isEmpty();
        any = any || has;
        ps.append(QJsonObject{{"id", providerId(p)}, {"name", providerName(p)}, {"hasKey", has},
                              {"signup", p == Provider::Ors ? "https://openrouteservice.org/dev/#/signup" : "https://graphhopper.com/dashboard/#/register"},
                              {"terms", p == Provider::Ors ? "https://openrouteservice.org/terms-of-service/" : "https://www.graphhopper.com/terms/"}});
    }
    return QJsonObject{{"ready", any}, {"default", m_keys ? QStringLiteral("ors") : defaultProvider()}, {"providers", ps}};
}

void RoutePlanner::send(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &body, Reply reply)
{
    if (m_transport) { m_transport(url, headers, body, std::move(reply)); return; }
    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    const qint64 wait = m_last.isValid() ? qMax<qint64>(0, 1000 - m_last.msecsTo(QDateTime::currentDateTime())) : 0;
    QPointer<RoutePlanner> self(this);
    QTimer::singleShot(int(wait), this, [self, url, headers, body, reply] {
        if (!self) return;
        self->m_last = QDateTime::currentDateTime();
        QNetworkRequest req(url);
        req.setRawHeader("User-Agent", "BeaconFix/" BEACONFIX_VERSION " (+https://github.com/sworrl/beaconfix)");
        for (const auto &h : headers) req.setRawHeader(h.first, h.second);
        req.setTransferTimeout(45000);
        QNetworkReply *r = self->m_nam->post(req, body);
        QObject::connect(r, &QNetworkReply::finished, self, [r, reply] {
            r->deleteLater();
            const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray bytes = r->readAll();
            QString err;
            if (!r->rawHeader("blocked-by").isEmpty()) err = QStringLiteral("blocked by %1 (DNS filter)").arg(QString::fromLatin1(r->rawHeader("blocked-by")));
            else if (r->error() != QNetworkReply::NoError && http == 0) err = r->errorString();
            reply(http, bytes, err);
        });
    });
}

void RoutePlanner::route(LatLon from, LatLon to, const QString &providerIn, Done done)
{
    auto bad = [](double lat, double lon) { return !std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180 || (lat == 0 && lon == 0); };
    if (bad(from.lat, from.lon) || bad(to.lat, to.lon)) { done(400, QJsonObject{{"error", "from and to need a valid lat / lon"}}); return; }
    if (PlateEvents::distanceM(from.lat, from.lon, to.lat, to.lon) > 1000000.0) { done(400, QJsonObject{{"error", "from and to are more than 1000 km apart"}}); return; }
    Provider p = Provider::Ors;
    if (!providerIn.isEmpty()) {
        if (!providerFromId(providerIn, &p)) { done(400, QJsonObject{{"error", "provider: ors | graphhopper"}}); return; }
        if (keyFor(p).isEmpty()) {
            done(412, QJsonObject{{"error", QStringLiteral("no %1 API key: routing around ALPR cameras needs your own free key — set it in Settings → Routing").arg(providerName(p))},
                                  {"needsKey", true}, {"routing", status()}});
            return;
        }
    } else {
        Provider def = Provider::Ors;
        providerFromId(m_keys ? QStringLiteral("ors") : defaultProvider(), &def);
        const Provider other = def == Provider::Ors ? Provider::GraphHopper : Provider::Ors;
        if (!keyFor(def).isEmpty()) p = def;
        else if (!keyFor(other).isEmpty()) p = other;
        else {
            done(412, QJsonObject{{"error", "no routing API key: routing around ALPR cameras needs a free OpenRouteService or GraphHopper key — set one in Settings → Routing"},
                                  {"needsKey", true}, {"routing", status()}});
            return;
        }
    }
    const double corr = corridorM(from, to) + 200.0;
    const double dLat = corr / 111194.93, midLat = (from.lat + to.lat) / 2.0;
    const double dLon = corr / (111194.93 * std::max(0.05, std::cos(midLat * M_PI / 180.0)));
    const QList<Cam> cams = m_cameras ? m_cameras(std::min(from.lat, to.lat) - dLat, std::max(from.lat, to.lat) + dLat,
                                                  std::min(from.lon, to.lon) - dLon, std::max(from.lon, to.lon) + dLon)
                                      : QList<Cam>();
    const Plan pl = plan(cams, from, to, p);
    const QString key = keyFor(p);
    send(requestUrl(p, key), requestHeaders(p, key), requestBody(p, from, to, pl.areas), [pl, cams, p, done](int http, const QByteArray &body, const QString &netErr) {
        if (!netErr.isEmpty()) { done(502, QJsonObject{{"error", QStringLiteral("%1 unreachable: %2").arg(providerName(p), netErr)}, {"provider", providerId(p)}}); return; }
        const Route r = parseResponse(p, http, body);
        if (!r.ok) {
            QJsonObject o{{"error", r.error}, {"provider", providerId(p)}, {"http", http},
                          {"avoided", QJsonObject{{"areas", int(pl.areas.size())}, {"capped", pl.capped}, {"corridorCameras", int(pl.corridor.size())}}}};
            done(http == 401 || http == 403 ? 412 : 502, o);
            return;
        }
        done(200, toJson(r, pl, camerasPassed(r.points, cams), p));
    });
}

void RoutePlanner::inspect(const AvoidRoute::Cam &target, AvoidRoute::LatLon from, std::optional<AvoidRoute::LatLon> toIn,
                           const QString &profile, double minM, double maxM, const QString &providerIn,
                           const QList<RoadSnap::Way> &ways, const QJsonArray &photos, Done done)
{
    const LatLon to = toIn.value_or(from);
    auto bad = [](double lat, double lon) { return !std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180 || (lat == 0 && lon == 0); };
    if (bad(target.lat, target.lon)) { done(400, QJsonObject{{"error", "invalid camera coordinates"}}); return; }
    if (bad(from.lat, from.lon) || bad(to.lat, to.lon)) { done(400, QJsonObject{{"error", "from and to need a valid lat / lon"}}); return; }

    const double corr = 1500.0;
    const double minLat = std::min({from.lat, to.lat, target.lat});
    const double maxLat = std::max({from.lat, to.lat, target.lat});
    const double minLon = std::min({from.lon, to.lon, target.lon});
    const double maxLon = std::max({from.lon, to.lon, target.lon});
    const double dLat = corr / 111194.93;
    const double midLat = (minLat + maxLat) / 2.0;
    const double dLon = corr / (111194.93 * std::max(0.05, std::cos(midLat * M_PI / 180.0)));

    QList<Cam> allCams = m_cameras ? m_cameras(minLat - dLat, maxLat + dLat, minLon - dLon, maxLon + dLon) : QList<Cam>();
    bool foundTarget = false;
    for (const Cam &c : allCams) {
        if (c.id == target.id) { foundTarget = true; break; }
    }
    if (!foundTarget) allCams.prepend(target);

    const QList<Area> avoidAreas = inspectAvoidRegions(allCams);
    const QList<Vantage> vantages = findVantages(target, ways, avoidAreas, profile, minM, maxM);

    const QString limitsText = QStringLiteral("Only mapped cameras with their mapped directions are avoided. "
        "Unmapped cameras, PTZ / 360° domes, private CCTV, police-car ALPRs and phone/cell tracking are not. "
        "Fields of view come from datasheets (§2.3) and mapped directions can be wrong.");

    InspectPlan plan;
    plan.camera = target;
    plan.vantages = vantages;
    plan.avoidRegions = avoidAreas;

    if (vantages.isEmpty()) {
        plan.safe = false;
        plan.note = QStringLiteral("No safe vantage points found outside camera avoid regions within %1–%2 m").arg(qRound(minM)).arg(qRound(maxM));
        done(200, inspectToJson(plan, limitsText, photos));
        return;
    }

    Provider p = Provider::Ors;
    if (!providerIn.isEmpty()) {
        if (!providerFromId(providerIn, &p)) { done(400, QJsonObject{{"error", "provider: ors | graphhopper"}}); return; }
    } else {
        Provider def = Provider::Ors;
        providerFromId(m_keys ? QStringLiteral("ors") : defaultProvider(), &def);
        const Provider other = def == Provider::Ors ? Provider::GraphHopper : Provider::Ors;
        if (!keyFor(def).isEmpty()) p = def;
        else if (!keyFor(other).isEmpty()) p = other;
        else p = def;
    }
    plan.provider = p;
    plan.limits = limits(p);

    const QString key = keyFor(p);
    if (key.isEmpty()) {
        plan.safe = true;
        plan.note = QStringLiteral("Vantage points and camera blind spots computed. Add an OpenRouteService or GraphHopper key in Settings → Routing to calculate turn-by-turn navigation legs.");
        done(200, inspectToJson(plan, limitsText, photos));
        return;
    }

    const Vantage v = vantages.first();
    const QList<Area> cappedAreas = avoidAreas.mid(0, std::min<int>(avoidAreas.size(), plan.limits.maxAreas));

    QPointer<RoutePlanner> self(this);
    const QUrl u1 = requestUrl(p, key, profile);
    const auto h1 = requestHeaders(p, key);
    const QByteArray b1 = requestBody(p, from, v.pt, cappedAreas, profile);

    send(u1, h1, b1, [self, plan, allCams, p, key, profile, from, to, v, cappedAreas, limitsText, photos, done](int http1, const QByteArray &body1, const QString &netErr1) mutable {
        if (!netErr1.isEmpty()) {
            plan.safe = false;
            plan.note = QStringLiteral("%1 unreachable for approach leg: %2").arg(providerName(p), netErr1);
            done(200, inspectToJson(plan, limitsText, photos));
            return;
        }
        plan.toVantage = parseResponse(p, http1, body1);

        const QUrl u2 = requestUrl(p, key, profile);
        const auto h2 = requestHeaders(p, key);
        const QByteArray b2 = requestBody(p, v.pt, to, cappedAreas, profile);

        if (!self) return;
        self->send(u2, h2, b2, [plan, allCams, limitsText, photos, done](int http2, const QByteArray &body2, const QString &netErr2) mutable {
            if (!netErr2.isEmpty()) {
                plan.safe = false;
                plan.note = QStringLiteral("Departure leg unreachable: %1").arg(netErr2);
                done(200, inspectToJson(plan, limitsText, photos));
                return;
            }
            plan.away = parseResponse(plan.provider, http2, body2);

            QList<LatLon> allPts = plan.toVantage.points;
            allPts += plan.away.points;
            plan.exposures = checkExposures(allPts, allCams);
            plan.safe = plan.exposures.isEmpty() && plan.toVantage.ok && plan.away.ok;
            if (!plan.safe && !plan.exposures.isEmpty()) {
                plan.note = QStringLiteral("Route enters field of view of %1 camera(s)").arg(plan.exposures.size());
            }
            done(200, inspectToJson(plan, limitsText, photos));
        });
    });
}

