#include "notify.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>

static const char *NOTIF = "org.freedesktop.Notifications";
static const char *NOTIF_PATH = "/org/freedesktop/Notifications";

Notifier::Notifier(QObject *parent) : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    m_connected = bus.connect(QString::fromLatin1(NOTIF), QString::fromLatin1(NOTIF_PATH), QString::fromLatin1(NOTIF), QStringLiteral("ActionInvoked"), this, SLOT(onActionInvoked(uint,QString)));
    bus.connect(QString::fromLatin1(NOTIF), QString::fromLatin1(NOTIF_PATH), QString::fromLatin1(NOTIF), QStringLiteral("NotificationClosed"), this, SLOT(onClosed(uint,uint)));
}

bool Notifier::available() const
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    return bus.isConnected() && bus.interface() && bus.interface()->isServiceRegistered(QString::fromLatin1(NOTIF));
}

void Notifier::send(const QString &summary, const QString &body, const QString &icon, const QStringList &actions, Action onAction, int timeoutMs)
{
    // No QDBusInterface here: its constructor introspects (and may try to D-Bus-activate a daemon),
    // which blocks the caller for seconds on a bus without a notification service.
    if (!available()) { emit fallback(summary, body); return; }
    QVariantMap hints; hints[QStringLiteral("desktop-entry")] = QStringLiteral("beaconfix");
    if (!actions.isEmpty()) hints[QStringLiteral("resident")] = false;
    QDBusMessage m = QDBusMessage::createMethodCall(QString::fromLatin1(NOTIF), QString::fromLatin1(NOTIF_PATH), QString::fromLatin1(NOTIF), QStringLiteral("Notify"));
    m.setArguments({QStringLiteral("BeaconFix"), uint(0), icon.isEmpty() ? QStringLiteral("beaconfix") : icon, summary, body, actions, hints, timeoutMs});
    QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(m);
    if (!onAction) return;
    auto *w = new QDBusPendingCallWatcher(call, this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, onAction] {
        w->deleteLater();
        QDBusPendingReply<uint> r = *w;
        if (r.isValid() && r.value() != 0) m_handlers.insert(r.value(), onAction);
    });
}

void Notifier::onActionInvoked(uint id, const QString &key)
{
    const auto it = m_handlers.find(id);
    if (it == m_handlers.end()) return;
    const Action a = it.value();
    m_handlers.erase(it);
    if (a) a(key);
}

void Notifier::onClosed(uint id, uint)
{
    m_handlers.remove(id);
}
