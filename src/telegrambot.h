#pragma once

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>

class Locator;

class TelegramBot : public QObject {
    Q_OBJECT
public:
    explicit TelegramBot(Locator *locator, QObject *parent = nullptr);

    QString token() const { return m_token; }
    void setToken(const QString &token);

    qint64 chatId() const { return m_chatId; }
    void setChatId(qint64 id);                            // 0 unbinds: a fresh pairing code is drawn

    bool isConfigured() const { return !m_token.isEmpty(); }
    bool isPolling() const { return m_running; }
    // Unbound: the one-time code the owner sends as "/start <code>" (empty once a chat is bound)
    QString pairCode() const { return m_chatId ? QString() : m_pairCode; }
    QString botUsername() const { return m_botUsername; }  // from getMe; empty until Telegram answers

    void start();                                         // no-op without a token
    void stop();

public slots:
    void sendMessage(const QString &text, qint64 targetChatId = 0);
    void sendAlert(const QString &title, const QString &body);
    void pollUpdates();

private slots:
    void handleUpdatesReply();
    void onCameraPassed(const QString &encounterJson);
    void onUsSyncProgress(int sector, int totalSectors, int camerasAdded, const QString &status);

private:
    void setupCommands();
    void fetchMe();                                       // getMe: the bot's @username for the pairing hint
    void ensurePairCode();
    void schedulePoll(int ms);
    void processMessage(const QJsonObject &msg);
    void handleCommand(qint64 chatId, const QString &cmd, const QString &args);

    Locator *m_loc = nullptr;
    QNetworkAccessManager m_nam;
    QString m_token;
    qint64 m_chatId = 0;
    qint64 m_lastUpdateId = 0;
    QString m_pairCode, m_botUsername;
    bool m_running = false;
    int m_backoffMs = 0;                                  // errors / 409 / 429: grows to 10 min, reset by a good poll
    QTimer m_pollTimer;                                   // single shot: the next getUpdates (one in flight at a time)
    QPointer<QNetworkReply> m_pollReply;
};
