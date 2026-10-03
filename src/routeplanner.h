// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "avoidroute.h"
#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <functional>

class QNetworkAccessManager;

// Camera-avoidance routing (docs/SIGHTINGS.md §8): the provider abstraction over OpenRouteService and GraphHopper. Both
// need the user's own free API key (Settings → Routing); without one nothing is sent and the answer says why. The
// network (Transport), the cameras (CameraSource) and the keys (KeySource) are injectable, so the unit test runs the
// whole flow against canned answers.
class RoutePlanner : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(int http, const QByteArray &body, const QString &networkError)>;
    using Transport = std::function<void(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &body, Reply reply)>;
    using CameraSource = std::function<QList<AvoidRoute::Cam>(double latMin, double latMax, double lonMin, double lonMax)>;
    using KeySource = std::function<QString(AvoidRoute::Provider)>;
    using Done = std::function<void(int httpCode, const QJsonObject &result)>;

    explicit RoutePlanner(CameraSource cameras, QObject *parent = nullptr);
    void setTransport(Transport t) { m_transport = std::move(t); }
    void setKeySource(KeySource k) { m_keys = std::move(k); }

    // QSettings: routing/orsKey, routing/graphhopperKey, routing/provider (ors | graphhopper)
    static QString storedKey(AvoidRoute::Provider p);
    static void    storeKey(AvoidRoute::Provider p, const QString &key);
    static QString defaultProvider();
    static void    setDefaultProvider(const QString &id);
    QJsonObject status() const;                    // {ready, default, providers:[{id, name, hasKey, signup, terms}]}

    // from → to, avoiding the ALPR cones along the way. provider "" = the default one that has a key.
    // done(200, {provider, route, distanceM, durationS, avoided, passes, attribution}) or (412 no key | 400 | 502, {error, …})
    void route(AvoidRoute::LatLon from, AvoidRoute::LatLon to, const QString &provider, Done done);

    // §9: Inspect unseen. from → vantage, vantage → to (default: to = from).
    // Computes avoid regions, finds vantages, routes both legs, verifies 5 m exposure.
    // If no routing key is configured, still returns vantages and avoid regions (200), noting that routing requires a key.
    void inspect(const AvoidRoute::Cam &target, AvoidRoute::LatLon from, std::optional<AvoidRoute::LatLon> to,
                 const QString &profile, double minM, double maxM, const QString &provider,
                 const QList<RoadSnap::Way> &ways, const QJsonArray &photos, Done done);

private:
    QString keyFor(AvoidRoute::Provider p) const { return m_keys ? m_keys(p) : storedKey(p); }
    void send(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &body, Reply reply);
    CameraSource m_cameras;
    Transport m_transport;
    KeySource m_keys;
    QNetworkAccessManager *m_nam = nullptr;
    QDateTime m_last;                              // ≥ 1 s between two provider requests
};
