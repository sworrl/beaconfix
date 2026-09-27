package org.sworrl.beaconfix.widget

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import dagger.hilt.android.AndroidEntryPoint
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.collector.CollectorService
import org.sworrl.beaconfix.data.Prefs
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The "tray presence": one permanent, silent notification that shows what BeaconFix knows and is doing.
 *
 * It is the foreground-service notification of [CollectorService], which runs whenever the status notification is enabled
 * (the default) — collecting or not. Android 14+ lets the user swipe a foreground-service notification away; the delete
 * intent re-posts it at once (the service keeps running), `FLAG_NO_CLEAR` keeps it out of "Clear all", and a
 * BOOT_COMPLETED / MY_PACKAGE_REPLACED receiver plus START_STICKY bring the service back after reboots, updates and
 * process death. Content comes from the same [WidgetState] the widgets use, so every scan, fix, sync and range refreshes it.
 */
@Singleton
class StatusNotifier @Inject constructor(@ApplicationContext private val ctx: Context, private val prefs: Prefs) {
    fun ensureChannel() {
        val nm = ctx.getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, ctx.getString(R.string.notif_channel_status), NotificationManager.IMPORTANCE_LOW).apply { description = ctx.getString(R.string.notif_channel_status_desc); setShowBadge(false) })
    }
    private fun pi(action: String, req: Int): PendingIntent = PendingIntent.getBroadcast(ctx, req, Intent(ctx, NotificationActionReceiver::class.java).setAction(action), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
    private fun open(action: String?, req: Int): PendingIntent = PendingIntent.getActivity(ctx, req, Intent(ctx, MainActivity::class.java).apply { if (action != null) putExtra("action", action) }, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

    /** The last notification we posted, so a dismissal can be undone instantly without rebuilding state. */
    @Volatile private var last: Notification? = null

    /** The nearest-help line for the expanded card ("🧸 Kids ER: … · 38 km — ER: … · 12 km"); set by [WidgetUpdater]. */
    @Volatile var helpLine: String = ""

    /** Build the notification for the current state. [ongoing] = it is the service's foreground notification (the normal case). */
    fun build(st: WidgetState, ongoing: Boolean, collectorState: String, events: List<String>): Notification {
        ensureChannel()
        val worst = when (st.worst) { "critical" -> "☠ insecure nearby"; "weak" -> "⚠ WPA2 only"; "strong" -> "🛡 WPA3"; "ok" -> "◐"; else -> "" }
        val where = st.place.ifEmpty { "no fix yet" } + (if (st.acc >= 0) " · ±${st.acc.toInt()} m" else "") + (if (st.source.isNotEmpty()) " · ${st.source.uppercase()}" else "")
        val home = st.atHome?.let { if (it) " · 🏠 at home" else " · ${st.awayText}" } ?: ""
        val title = "BeaconFix · $collectorState"
        val line1 = "$where$home"
        val line2 = "${st.inRange} beacons in range" + (if (worst.isNotEmpty()) " · $worst" else "") + (if (st.desktopPaired) " · ${st.desktopName}: synced ${agoShort(st.lastSync)}, ${st.unsynced} waiting" else " · ${st.unsynced} waiting to sync")
        val big = StringBuilder(line1).append('\n').append(line2)
        if (st.rangeLine.isNotEmpty()) big.append('\n').append(st.rangeLine)
        if (helpLine.isNotEmpty()) big.append('\n').append(helpLine)
        if (events.isNotEmpty()) { big.append("\n— recent —"); events.take(3).forEach { big.append('\n').append(it) } }
        val b = NotificationCompat.Builder(ctx, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification).setColor(0xFF35D6FF.toInt())
            .setContentTitle(title).setContentText(if (st.rangeLine.isNotEmpty()) st.rangeLine else line1).setSubText(line2.take(60))
            .setStyle(NotificationCompat.BigTextStyle().bigText(big.toString()))
            .setOngoing(true).setOnlyAlertOnce(true).setSilent(true).setShowWhen(false)
            .setCategory(NotificationCompat.CATEGORY_STATUS).setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
            .setPublicVersion(publicVersion())                     // the lock screen sees "BeaconFix · running", never a place
            .setContentIntent(open(null, 0))
            .setDeleteIntent(pi(ACTION_DISMISSED, 5))             // swiped away (Android 14+ allows it) → re-posted immediately
            // Android shows at most three actions: Help first (sync lives in the app, the Sync widget and the worker)
            .addAction(0, "Help", open("help", 4))
            .addAction(0, "Scan now", pi(ACTION_SCAN, 1))
            .addAction(0, if (st.collectorOn) "Pause" else "Resume", pi(if (st.collectorOn) ACTION_PAUSE else ACTION_RESUME, 3))
        if (ongoing) b.setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
        val n = b.build()
        n.flags = n.flags or Notification.FLAG_NO_CLEAR or Notification.FLAG_ONGOING_EVENT
        last = n
        return n
    }

    private fun publicVersion(): Notification = NotificationCompat.Builder(ctx, CHANNEL)
        .setSmallIcon(R.drawable.ic_notification).setColor(0xFF35D6FF.toInt())
        .setContentTitle("BeaconFix · running")
        .setOngoing(true).setSilent(true).setShowWhen(false).setCategory(NotificationCompat.CATEGORY_STATUS)
        .build()

    /** Post the current state (updates the service's foreground notification in place when the service runs). */
    @android.annotation.SuppressLint("MissingPermission")   // guarded by canPost()
    suspend fun post(st: WidgetState, serviceRunning: Boolean, collectorState: String, events: List<String>) {
        val nm = NotificationManagerCompat.from(ctx)
        if (!prefs.statusNotification.first()) { nm.cancel(ID); last = null; return }
        if (!canPost()) return
        nm.notify(ID, build(st, ongoing = serviceRunning, collectorState = collectorState, events = events))
    }
    /** The user swiped the card away: put it straight back (same content), and make sure the service is up. */
    @android.annotation.SuppressLint("MissingPermission")
    suspend fun repost() {
        if (!prefs.statusNotification.first() || !canPost()) return
        last?.let { NotificationManagerCompat.from(ctx).notify(ID, it) }
    }
    fun clear() { last = null; NotificationManagerCompat.from(ctx).cancel(ID) }
    fun canPost(): Boolean = android.os.Build.VERSION.SDK_INT < 33 || ContextCompat.checkSelfPermission(ctx, android.Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED

    companion object {
        const val CHANNEL = "status"; const val ID = 42        // same id as the collector's foreground notification: one card, one owner
        const val ACTION_SCAN = "org.sworrl.beaconfix.notif.SCAN"; const val ACTION_SYNC = "org.sworrl.beaconfix.notif.SYNC"
        const val ACTION_PAUSE = "org.sworrl.beaconfix.notif.PAUSE"; const val ACTION_RESUME = "org.sworrl.beaconfix.notif.RESUME"
        const val ACTION_DISMISSED = "org.sworrl.beaconfix.notif.DISMISSED"
        fun agoShort(ms: Long): String { if (ms <= 0) return "never"; val s = (System.currentTimeMillis() - ms) / 1000; return when { s < 60 -> "just now"; s < 3600 -> "${s / 60} min ago"; s < 86400 -> "${s / 3600} h ago"; else -> "${s / 86400} d ago" } }
    }
}

/** Handles the notification's buttons and its dismissal. */
@AndroidEntryPoint
class NotificationActionReceiver : BroadcastReceiver() {
    @Inject lateinit var prefs: Prefs
    @Inject lateinit var updater: WidgetUpdater
    @Inject lateinit var notifier: StatusNotifier
    @Inject lateinit var scheduler: org.sworrl.beaconfix.sync.SyncScheduler
    @Inject lateinit var recorder: org.sworrl.beaconfix.collector.ObservationRecorder
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    override fun onReceive(context: Context, intent: Intent) {
        val pending = goAsync()
        scope.launch {
            try {
                when (intent.action) {
                    StatusNotifier.ACTION_DISMISSED -> { notifier.repost(); CollectorService.ensure(context, prefs); return@launch }
                    StatusNotifier.ACTION_SCAN -> runCatching { recorder.scanAndRecord(fresh = true) }
                    StatusNotifier.ACTION_SYNC -> scheduler.syncNow()
                    StatusNotifier.ACTION_PAUSE -> prefs.setCollectorOn(false)     // the service watches the pref: the loop stops, the card stays
                    StatusNotifier.ACTION_RESUME -> { prefs.setCollectorOn(true); CollectorService.ensure(context, prefs) }
                }
                runCatching { updater.refresh(renderMap = false) }
            } finally { pending.finish() }
        }
    }
}
