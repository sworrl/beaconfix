#pragma once
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

// Desktop notifications with buttons (org.freedesktop.Notifications). `actions` is a flat
// list of key, label pairs; the callback gets the key of the button that was pressed —
// "default" when the notification body itself was clicked. Falls back to the signal
// when no notification daemon answers (the tray shows a balloon then).
class Notifier : public QObject {
    Q_OBJECT
public:
    using Action = std::function<void(const QString &key)>;
    explicit Notifier(QObject *parent = nullptr);
    void send(const QString &summary, const QString &body, const QString &icon, const QStringList &actions = {}, Action onAction = nullptr, int timeoutMs = 8000);
    bool available() const;

signals:
    void fallback(const QString &summary, const QString &body);

private slots:
    void onActionInvoked(uint id, const QString &key);
    void onClosed(uint id, uint reason);

private:
    QHash<uint, Action> m_handlers;
    bool m_connected = false;
};
