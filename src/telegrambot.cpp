#include "telegrambot.h"
#include "locator.h"
#include "mapdb.h"
#include <QJsonArray>
#include <QSettings>
#include <QUrlQuery>
#include <QUrl>
#include <QDateTime>
#include <QRandomGenerator>
#include <cmath>

// Legacy Markdown: text from outside (place names, OSM tags, plates) with a stray _ * ` [ made Telegram
// reject the whole message ("can't parse entities"). md() escapes free text; code() is for inside `…`.
static QString md(QString t)
{
    for (const char *c : {"_", "*", "`", "["}) t.replace(QLatin1String(c), QLatin1String("\\") + QLatin1String(c));
    return t;
}
static QString code(QString t) { return t.remove(QLatin1Char('`')); }
static QString bold(QString t) { return t.remove(QLatin1Char('*')); }

// The pairing code: 8 characters from an alphabet without look-alikes (~40 bits). Compared in constant time
// (for the secret's length), case-insensitively, so the code can be typed off the screen
static QString newPairCode()
{
    static const char abc[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    QString c;
    for (int i = 0; i < 8; ++i) c += QLatin1Char(abc[QRandomGenerator::system()->bounded(int(sizeof abc) - 1)]);
    return c;
}
static bool samePairCode(const QString &given, const QString &code)
{
    const QByteArray a = given.toUpper().toUtf8(), b = code.toUpper().toUtf8();
    if (b.isEmpty()) return false;
    unsigned diff = unsigned(a.size() ^ b.size());
    for (int i = 0; i < b.size(); ++i) diff |= uchar(b[i]) ^ uchar(i < a.size() ? a[i] : 0);
    return diff == 0;
}

// No token = no bot: nothing polls until the owner configures their own (POST /api/v1/telegram). No chat bound =
// only "/start <pairing code>" binds one; every other chat is ignored, before and after
TelegramBot::TelegramBot(Locator *locator, QObject *parent)
    : QObject(parent), m_loc(locator)
{
    QSettings settings;
    m_token = settings.value(QStringLiteral("telegram/token")).toString().trimmed();
    m_chatId = settings.value(QStringLiteral("telegram/chat_id"), 0).toLongLong();
    m_pairCode = settings.value(QStringLiteral("telegram/pairCode")).toString();

    m_pollTimer.setSingleShot(true);
    connect(&m_pollTimer, &QTimer::timeout, this, &TelegramBot::pollUpdates);

    if (m_loc) {
        connect(m_loc, &Locator::cameraPassed, this, &TelegramBot::onCameraPassed);
        connect(m_loc, &Locator::usSyncProgress, this, &TelegramBot::onUsSyncProgress);
    }
    start();
}

void TelegramBot::start()
{
    if (m_token.isEmpty() || m_running) return;
    m_running = true;
    m_backoffMs = 0;
    ensurePairCode();
    if (!m_chatId) qInfo("beaconfix: Telegram bot is not paired: send \"/start %s\" to it from your Telegram account", qPrintable(m_pairCode));
    setupCommands();
    fetchMe();
    schedulePoll(0);
}

void TelegramBot::stop()
{
    m_running = false;
    m_pollTimer.stop();
    if (QNetworkReply *r = m_pollReply.data()) { m_pollReply = nullptr; r->abort(); }   // its finished handler sees it is stale
}

void TelegramBot::setToken(const QString &token)
{
    const QString t = token.trimmed();
    if (t == m_token) { start(); return; }                   // the same token again restarts a bot stopped by a 401
    stop();
    m_token = t;
    m_botUsername.clear();
    m_lastUpdateId = 0;
    QSettings s;
    if (t.isEmpty()) s.remove(QStringLiteral("telegram/token"));
    else s.setValue(QStringLiteral("telegram/token"), t);
    start();
}

void TelegramBot::setChatId(qint64 id)
{
    m_chatId = id;
    QSettings s;
    s.setValue(QStringLiteral("telegram/chat_id"), m_chatId);
    m_pairCode.clear();                                       // used up when bound; unbinding draws a fresh one
    s.remove(QStringLiteral("telegram/pairCode"));
    if (!id && m_running) {
        ensurePairCode();
        qInfo("beaconfix: Telegram bot unpaired: send \"/start %s\" to it to pair again", qPrintable(m_pairCode));
    }
}

void TelegramBot::ensurePairCode()
{
    if (m_chatId || !m_pairCode.isEmpty()) return;
    m_pairCode = newPairCode();
    QSettings().setValue(QStringLiteral("telegram/pairCode"), m_pairCode);
}

void TelegramBot::schedulePoll(int ms)
{
    if (m_running) m_pollTimer.start(ms);
}

void TelegramBot::fetchMe()
{
    QNetworkRequest req(QUrl(QStringLiteral("https://api.telegram.org/bot%1/getMe").arg(m_token)));
    req.setTransferTimeout(20000);
    QNetworkReply *rep = m_nam.get(req);
    const QString tok = m_token;
    connect(rep, &QNetworkReply::finished, this, [this, rep, tok] {
        rep->deleteLater();
        if (rep->error() != QNetworkReply::NoError || tok != m_token) return;
        m_botUsername = QJsonDocument::fromJson(rep->readAll()).object()[QStringLiteral("result")].toObject()[QStringLiteral("username")].toString();
    });
}

void TelegramBot::setupCommands()
{
    if (m_token.isEmpty()) return;

    QUrl url(QStringLiteral("https://api.telegram.org/bot%1/setMyCommands").arg(m_token));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonArray cmds;
    cmds.append(QJsonObject{{"command", "status"}, {"description", "Current fix, vehicle & tracking status"}});
    cmds.append(QJsonObject{{"command", "radar"}, {"description", "Nearest Flock & ALPR surveillance cameras"}});
    cmds.append(QJsonObject{{"command", "encounters"}, {"description", "Recent camera pass history"}});
    cmds.append(QJsonObject{{"command", "audits"}, {"description", "Plate events: ALPR passes and plate searches"}});
    cmds.append(QJsonObject{{"command", "plates"}, {"description", "Registered vehicles and license plates"}});
    cmds.append(QJsonObject{{"command", "sync"}, {"description", "Trigger nationwide US camera sync"}});
    cmds.append(QJsonObject{{"command", "recalculate"}, {"description", "Recalculate route history camera passes"}});
    cmds.append(QJsonObject{{"command", "help"}, {"description", "Show bot commands & help"}});

    QJsonObject body{{"commands", cmds}};
    QNetworkReply *rep = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(rep, &QNetworkReply::finished, rep, &QObject::deleteLater);
}

void TelegramBot::sendMessage(const QString &text, qint64 targetChatId)
{
    const qint64 cid = targetChatId != 0 ? targetChatId : m_chatId;
    if (m_token.isEmpty() || cid == 0) return;

    QUrl url(QStringLiteral("https://api.telegram.org/bot%1/sendMessage").arg(m_token));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonObject body;
    body["chat_id"] = cid;
    body["text"] = text;
    body["parse_mode"] = QStringLiteral("Markdown");
    body["disable_web_page_preview"] = false;

    req.setTransferTimeout(20000);
    QNetworkReply *rep = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(rep, &QNetworkReply::finished, this, [this, rep, req, body] {
        rep->deleteLater();
        // 400 = the Markdown did not parse: send it again as plain text rather than lose the alert
        if (rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 400 || !body.contains(QLatin1String("parse_mode"))) return;
        QJsonObject plain = body; plain.remove(QStringLiteral("parse_mode"));
        QNetworkReply *again = m_nam.post(req, QJsonDocument(plain).toJson(QJsonDocument::Compact));
        connect(again, &QNetworkReply::finished, again, &QObject::deleteLater);
    });
}

void TelegramBot::sendAlert(const QString &title, const QString &body)
{
    const QString msg = QStringLiteral("*%1*\n\n%2").arg(title, body);
    sendMessage(msg);
}


void TelegramBot::pollUpdates()
{
    if (!m_running || m_token.isEmpty() || m_pollReply) return;

    QUrl url(QStringLiteral("https://api.telegram.org/bot%1/getUpdates").arg(m_token));
    QUrlQuery q;
    if (m_lastUpdateId > 0) q.addQueryItem(QStringLiteral("offset"), QString::number(m_lastUpdateId + 1));
    q.addQueryItem(QStringLiteral("timeout"), QStringLiteral("20"));
    q.addQueryItem(QStringLiteral("allowed_updates"), QStringLiteral("[\"message\"]"));
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setTransferTimeout(30000);
    m_pollReply = m_nam.get(req);
    connect(m_pollReply, &QNetworkReply::finished, this, &TelegramBot::handleUpdatesReply);
}

// One long poll in flight; the next is scheduled from its answer. Errors back off (doubling, 5 s to 2 min; 10 min
// for 409 Conflict, i.e. another client polls this token or a webhook is set, which retrying every 2 s never fixes)
void TelegramBot::handleUpdatesReply()
{
    auto *rep = qobject_cast<QNetworkReply *>(sender());
    if (!rep) return;
    rep->deleteLater();
    if (rep != m_pollReply) return;                           // aborted by stop() / setToken()
    m_pollReply = nullptr;

    if (rep->error() != QNetworkReply::NoError) {
        const int http = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http == 401 || http == 404) { qWarning("beaconfix: Telegram rejected the bot token (HTTP %d); polling stopped", http); stop(); return; }
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        int floor = 5000, cap = 120000;
        if (http == 409) {
            floor = 30000; cap = 600000;
            if (!m_backoffMs) qWarning("beaconfix: Telegram getUpdates conflict (HTTP 409: %s); backing off", qPrintable(o[QStringLiteral("description")].toString()));
        } else if (http == 429) {
            floor = qMax(5, o[QStringLiteral("parameters")].toObject()[QStringLiteral("retry_after")].toInt()) * 1000;
        }
        m_backoffMs = qMin(cap, qMax(floor, m_backoffMs * 2));
        schedulePoll(m_backoffMs);
        return;
    }
    m_backoffMs = 0;

    const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
    const QJsonArray results = doc.object()["result"].toArray();
    for (const QJsonValue &v : results) {
        const QJsonObject upd = v.toObject();
        const qint64 uid = upd["update_id"].toInteger();
        if (uid > m_lastUpdateId) m_lastUpdateId = uid;

        if (upd.contains(QLatin1String("message"))) {
            processMessage(upd["message"].toObject());
        }
    }
    schedulePoll(200);
}

void TelegramBot::processMessage(const QJsonObject &msg)
{
    const qint64 chatId = msg["chat"].toObject()["id"].toInteger();
    if (chatId == 0) return;

    const QString text = msg["text"].toString().trimmed();
    if (!text.startsWith('/')) return;

    QString cmd = text.section(' ', 0, 0).toLower();
    if (cmd.contains('@')) cmd = cmd.section('@', 0, 0);
    const QString args = text.section(' ', 1).trimmed();

    // Unbound: only "/start <pairing code>" (the code from GET /api/v1/telegram or the log) binds a chat. The first
    // sender owning it let any Telegram user who found the bot read /status (our exact position) and /plates
    if (m_chatId == 0) {
        if (cmd != QLatin1String("/start") || !samePairCode(args, m_pairCode)) return;
        setChatId(chatId);
        qInfo("beaconfix: Telegram bot paired with chat %lld", chatId);
        handleCommand(chatId, cmd, QString());
        return;
    }
    if (chatId != m_chatId) return;

    handleCommand(chatId, cmd, args);
}

void TelegramBot::handleCommand(qint64 chatId, const QString &cmd, const QString &args)
{
    Q_UNUSED(args)
    if (!m_loc) return;

    if (cmd == QLatin1String("/start")) {
        const auto plates = m_loc->licensePlates();
        QString activeStr = QStringLiteral("none registered (add one in the desktop app or the web UI)");
        for (const auto &p : plates) {
            if (p.active) {
                activeStr = QStringLiteral("%1 `%2` (%3)").arg(md(p.state), code(p.plate), md(p.vehicleDesc));
                break;
            }
        }
        const QString resp = QStringLiteral(
            "🛸 *BeaconFix ALPR & Radar Intelligence Bot*\n\n"
            "This chat is registered for real-time camera alerts, ALPR detections, and plate audits.\n\n"
            "🚘 *Active Vehicle:* %1\n\n"
            "*Available Commands:*\n"
            "/status - Current fix, vehicle & tracking status\n"
            "/radar - Nearest Flock & ALPR surveillance cameras\n"
            "/encounters - Recent camera pass history\n"
            "/audits - Plate events: ALPR passes (plate likely read) and searches in released Flock audit logs\n"
            "/plates - Registered vehicles and license plates\n"
            "/sync - Trigger nationwide US camera sync\n"
            "/recalculate - Recalculate route history camera passes\n"
            "/help - Show bot commands"
        ).arg(activeStr);
        sendMessage(resp, chatId);
        return;
    }

    if (cmd == QLatin1String("/help")) {
        const QString resp = QStringLiteral(
            "📖 *BeaconFix Bot Commands:*\n\n"
            "• `/status` - GPS position, speed, active vehicle, tracked cameras\n"
            "• `/radar` - Closest 5 ALPR cameras to your current location\n"
            "• `/encounters` - View recent ALPR camera encounters\n"
            "• `/audits` - Latest plate events: ALPR passes and plate searches from released audit logs\n"
            "• `/plates` - View and manage registered license plates\n"
            "• `/sync` - Download and synchronize cameras across the entire US\n"
            "• `/recalculate` - Recompute passes against all stored route points"
        );
        sendMessage(resp, chatId);
        return;
    }

    if (cmd == QLatin1String("/status")) {
        const double lat = m_loc->latitude();
        const double lon = m_loc->longitude();
        const double acc = m_loc->accuracy();
        const double spd = m_loc->speedKmh();
        const double elev = m_loc->elevation();
        const QString place = m_loc->place();
        const QJsonObject alpr = m_loc->alprSummary();       // counts in SQL: the table is ~140k rows after a sync

        const QString resp = QStringLiteral(
            "📍 *BeaconFix Status Report*\n\n"
            "• *Position:* `%1, %2` (±%3 m)\n"
            "• *Place:* %4\n"
            "• *Speed:* %5 km/h\n"
            "• *Elevation:* %6 m\n"
            "• *Tracked ALPRs in DB:* %7 cameras\n"
            "• *Cameras Encountered:* %8 cameras passed\n"
            "• *Nationwide Sync:* %9\n"
            "• *Web Dashboard:* `http://localhost:47822/web`"
        ).arg(QString::number(lat, 'f', 5))
         .arg(QString::number(lon, 'f', 5))
         .arg(QString::number(acc, 'f', 1))
         .arg(!place.isEmpty() ? md(place) : QStringLiteral("Unknown"))
         .arg(spd >= 0 ? QString::number(spd, 'f', 1) : QStringLiteral("0.0"))
         .arg(elev > -9000 ? QString::number(elev, 'f', 0) : QStringLiteral("N/A"))
         .arg(alpr[QStringLiteral("totalCameras")].toInt())
         .arg(alpr[QStringLiteral("passedCameras")].toInt())
         .arg(m_loc->usSyncActive() ? md(m_loc->usSyncStatus()) : QStringLiteral("Idle"));
        sendMessage(resp, chatId);
        return;
    }

    if (cmd == QLatin1String("/radar") || cmd == QLatin1String("/cameras")) {
        const double lat = m_loc->latitude();
        const double lon = m_loc->longitude();
        if (lat == 0.0 && lon == 0.0) {
            sendMessage(QStringLiteral("⚠️ Current position is unknown."), chatId);
            return;
        }

        auto cams = m_loc->flockCamerasNear(lat, lon, 100, 50);
        if (cams.isEmpty() && m_loc->alprSummary()[QStringLiteral("totalCameras")].toInt() > 0) {
            sendMessage(QStringLiteral("📷 No cameras within 100 km of your position."), chatId);
            return;
        }
        if (cams.isEmpty()) {
            sendMessage(QStringLiteral("⚠️ No cameras in local database. Run /sync to fetch nationwide cameras."), chatId);
            return;
        }

        struct CamDist { FlockCamera cam; double dist; };
        QList<CamDist> sorted;
        for (const auto &c : cams) {
            if (c.lat == 0.0 && c.lon == 0.0) continue;
            sorted.append({c, Locator::distanceM(lat, lon, c.lat, c.lon)});
        }
        std::sort(sorted.begin(), sorted.end(), [](const CamDist &a, const CamDist &b) {
            return a.dist < b.dist;
        });

        QString resp = QStringLiteral("🎯 *Nearest ALPR & Surveillance Cameras:*\n\n");
        const int count = qMin(sorted.size(), 5);
        for (int i = 0; i < count; ++i) {
            const auto &cd = sorted[i];
            const QString alertEmoji = cd.cam.passCount > 0 ? QStringLiteral("🚨") : QStringLiteral("📷");
            const QString passNote = cd.cam.passCount > 0 ? QStringLiteral(" · *%1 passes*").arg(cd.cam.passCount) : QString();
            resp += QStringLiteral("%1 *%2* (%3)%4\n   └ 📏 %5 m away · Dir: %6\n   └ [OSM Pin](https://www.openstreetmap.org/?mlat=%7&mlon=%8)\n\n")
                    .arg(alertEmoji, bold(cd.cam.model), md(cd.cam.operatorName), passNote)
                    .arg(int(cd.dist))
                    .arg(!cd.cam.direction.isEmpty() ? md(cd.cam.direction) : QStringLiteral("Omni"))
                    .arg(QString::number(cd.cam.lat, 'f', 5))
                    .arg(QString::number(cd.cam.lon, 'f', 5));
        }
        sendMessage(resp, chatId);
        return;
    }

    if (cmd == QLatin1String("/encounters")) {
        const auto encs = m_loc->cameraEncounters(QString(), 8);
        if (encs.isEmpty()) {
            sendMessage(QStringLiteral("ℹ️ No camera pass encounters logged yet."), chatId);
            return;
        }
        QString resp = QStringLiteral("🚨 *Recent ALPR Camera Encounters:*\n\n");
        for (const auto &e : encs) {
            resp += QStringLiteral("• *Encounter #%1* · %2\n   └ Vehicle: `%3` (%4)\n   └ Distance: %5 m · %6\n\n")
                    .arg(e.encounterNum)
                    .arg(e.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                    .arg(code(e.plate), md(e.vehicleDesc))
                    .arg(QString::number(e.distanceM, 'f', 1))
                    .arg(md(e.cameraId));
        }
        sendMessage(resp, chatId);
        return;
    }

    if (cmd == QLatin1String("/audits")) {
        // Plate events (docs/SIGHTINGS.md): ALPR passes from your route, plate searches from released Flock audit logs
        const QJsonArray events = m_loc->mapDb() ? m_loc->mapDb()->plateEventsLatest(10) : QJsonArray();
        if (events.isEmpty()) {
            sendMessage(QStringLiteral("ℹ️ No plate events yet. Camera passes come from your route history (the backfill runs by itself); plate searches from released Flock audit logs (HaveIBeenFlocked)."), chatId);
            return;
        }
        QString resp = QStringLiteral("🛡️ *Latest plate events:*\n\n");
        for (const QJsonValue &v : events) {
            const QJsonObject e = v.toObject();
            const QString when = QDateTime::fromString(e["time"].toString(), Qt::ISODate).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
            const QString src = e["source_url"].toString();
            if (e["kind"].toString() == QLatin1String("plate_search")) {
                resp += QStringLiteral("🔎 %1 · *searched* `%2` · %3\n   └ %4\n   └ Conf %5%%6\n\n")
                            .arg(when, code(e["plate"].toString()), md(e["agency"].toString()), md(e["details"].toString()))
                            .arg(e["confidence"].toInt()).arg(src.isEmpty() ? QString() : QStringLiteral(" · [source](%1)").arg(src));
            } else {
                const bool alpr = e["camera_type"].toString() == QLatin1String("alpr");
                resp += QStringLiteral("%1 %2 · %3 · %4 m\n   └ %5\n   └ Conf %6%%7\n\n")
                            .arg(alpr ? QStringLiteral("🚨") : QStringLiteral("📷"), when,
                                 md(QStringList{e["operator"].toString(), e["model"].toString()}.join(QLatin1Char(' ')).simplified()))
                            .arg(qRound(e["distance_m"].toDouble())).arg(md(e["details"].toString())).arg(e["confidence"].toInt())
                            .arg(src.isEmpty() ? QString() : QStringLiteral(" · [camera](%1)").arg(src));
            }
        }
        sendMessage(resp, chatId);
        return;
    }

    if (cmd == QLatin1String("/plates")) {
        const auto plates = m_loc->licensePlates();
        QString resp = QStringLiteral("🚘 *Registered Vehicles & Plates:*\n\n");
        for (const auto &p : plates) {
            resp += QStringLiteral("%1 *%2* `%3`\n   └ %4\n\n")
                    .arg(p.active ? QStringLiteral("⭐ [ACTIVE]") : QStringLiteral("•"))
                    .arg(bold(p.state), code(p.plate), md(p.vehicleDesc));
        }
        sendMessage(resp, chatId);
        return;
    }

    if (cmd == QLatin1String("/sync")) {
        m_loc->syncNationwideUsCameras(true);
        sendMessage(QStringLiteral("🚀 *Initiated ALPR Camera Sync*\n\nFetching the DeFlock ALPR dataset and community reports (falling back to regional OpenStreetMap queries). You will receive an update once complete."), chatId);
        return;
    }

    if (cmd == QLatin1String("/recalculate")) {
        const int count = m_loc->recalculatePasses();
        sendMessage(QStringLiteral("🔄 *Recalculated passes from route history:*\nCorrelated *%1* camera pass encounters.").arg(count), chatId);
        return;
    }

    sendMessage(QStringLiteral("❓ Unknown command. Type /help for a list of commands."), chatId);
}

// A new plate event worth an alert (Locator::cameraPassed carries the plate_events row): honest wording, as the desktop's
void TelegramBot::onCameraPassed(const QString &encounterJson)
{
    const QJsonObject e = QJsonDocument::fromJson(encounterJson.toUtf8()).object();
    const QString when = QDateTime::fromString(e["time"].toString(), Qt::ISODate).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const QString src = e["source_url"].toString();
    QString msg;
    if (e["kind"].toString() == QLatin1String("plate_search")) {
        msg = QStringLiteral("🔎 *Your plate was searched in Flock* · %1\n\n%2\n🕒 %3\n📄 From a released Flock audit log%4")
                  .arg(md(e["agency"].toString()), md(e["details"].toString()), when,
                       src.isEmpty() ? QString() : QStringLiteral(" · [source](%1)").arg(src));
    } else {
        if (e["camera_type"].toString() != QLatin1String("alpr")) return;
        const QJsonValue f = e["facing"];
        const QString facing = f.isNull() || f.isUndefined() ? QStringLiteral("facing unknown") : f.toInt() == 1 ? QStringLiteral("camera faced you") : QStringLiteral("camera faced away");
        msg = QStringLiteral("🚨 *Passed an ALPR camera* · %1 · %2 m\n\nYour plate `%3` was likely read (%4). Confidence %5 %.\n🕒 %6\n🗺️ [Map](https://www.openstreetmap.org/?mlat=%7&mlon=%8#map=18/%7/%8)%9")
                  .arg(md(QStringList{e["operator"].toString(), e["model"].toString()}.join(QLatin1Char(' ')).simplified()))
                  .arg(qRound(e["distance_m"].toDouble()))
                  .arg(code(e["plate"].toString()), facing)
                  .arg(e["confidence"].toInt())
                  .arg(when)
                  .arg(e["camera_lat"].toDouble(), 0, 'f', 5).arg(e["camera_lon"].toDouble(), 0, 'f', 5)
                  .arg(src.isEmpty() ? QString() : QStringLiteral(" · [camera](%1)").arg(src));
    }
    sendMessage(msg);
}

void TelegramBot::onUsSyncProgress(int sector, int totalSectors, int camerasAdded, const QString &status)
{
    if (sector == totalSectors) {
        // Complete
        Q_UNUSED(camerasAdded);
        const QString msg = QStringLiteral(
            "✅ *ALPR CAMERA SYNC COMPLETE*\n\n%1\n\n"
            "Camera passes are recalculated from your route history in the background."
        ).arg(md(status));
        sendMessage(msg);
    }
}
