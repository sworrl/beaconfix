package org.sworrl.beaconfix.wifi

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.first
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.Prefs
import java.security.MessageDigest
import javax.inject.Inject
import javax.inject.Singleton

/**
 * At most one alert per network name per [windowMs], on an injected [clock]. The names are stored hashed, so the
 * throttle file never holds the SSIDs of the places this phone has been.
 */
class WifiAlertThrottle(private val clock: () -> Long, private val store: Store = MemoryStore(), private val windowMs: Long = DAY_MS) {
    interface Store { fun get(key: String): Long?; fun put(key: String, at: Long); fun keys(): Set<String>; fun remove(key: String) }

    class MemoryStore : Store {
        private val m = HashMap<String, Long>()
        override fun get(key: String) = m[key]
        override fun put(key: String, at: Long) { m[key] = at }
        override fun keys(): Set<String> = m.keys.toSet()
        override fun remove(key: String) { m.remove(key) }
    }

    /** True (and remembered) when [name] may alert now; false within [windowMs] of its last alert. */
    @Synchronized
    fun tryAcquire(name: String): Boolean {
        val now = clock()
        val key = keyOf(name)
        val last = store.get(key)
        if (last != null && now >= last && now - last < windowMs) return false
        for (k in store.keys()) { val t = store.get(k) ?: continue; if (now - t >= windowMs || now < t) store.remove(k) }   // keep the file small
        store.put(key, now)
        return true
    }

    companion object {
        const val DAY_MS = 24 * 3_600_000L
        fun keyOf(name: String): String =
            MessageDigest.getInstance("SHA-256").digest(name.toByteArray()).take(12).joinToString("") { "%02x".format(it) }
    }
}

/**
 * The "open Wi-Fi" heads-up: when the phone joins an open, WEP or TKIP network that is not a home network, one
 * notification per network name per 24 h, only with Settings → "Open Wi-Fi alerts" (`wifiAlerts`) on. Posted on the
 * `alerts` channel (A5's HelpAlerts creates it; [ensureChannel] makes it too, only when it is missing). The lock
 * screen shows only a generic line, never the network name.
 */
@Singleton
class WifiAlerts internal constructor(private val ctx: Context, private val prefs: Prefs, private val throttle: WifiAlertThrottle) {
    @Inject constructor(@ApplicationContext ctx: Context, prefs: Prefs) :
        this(ctx, prefs, WifiAlertThrottle(System::currentTimeMillis, PrefsStore(ctx.getSharedPreferences("wifi_alerts", Context.MODE_PRIVATE))))

    /** Called by [CurrentNetworkMonitor] whenever the connected network changes. Never throws. */
    suspend fun onConnected(w: CurrentWifi) {
        val text = try {
            decide(prefs.wifiAlerts.first(), w, prefs.homePatterns.first(), throttle)
        } catch (e: Exception) { Log.w(TAG, "wifi alert check failed: $e"); null } ?: return
        post(text, WifiGrade.grade(w.security).label)
    }

    private fun post(text: String, label: String) {
        ensureChannel(ctx)
        val nm = NotificationManagerCompat.from(ctx)
        if (!nm.areNotificationsEnabled()) return
        if (Build.VERSION.SDK_INT >= 33 && ContextCompat.checkSelfPermission(ctx, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return
        val open = PendingIntent.getActivity(ctx, NOTIFICATION_ID, Intent(ctx, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val public = NotificationCompat.Builder(ctx, CHANNEL).setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(ctx.getString(R.string.wifi_alert_public)).build()
        val n = NotificationCompat.Builder(ctx, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification).setColor(0xFFFF4D4D.toInt())
            .setContentTitle(ctx.getString(R.string.wifi_alert_title))
            .setSubText(label)
            .setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(text))
            .setContentIntent(open).setAutoCancel(true).setOnlyAlertOnce(true)
            .setCategory(NotificationCompat.CATEGORY_RECOMMENDATION)
            .setVisibility(NotificationCompat.VISIBILITY_PRIVATE).setPublicVersion(public)
            .build()
        try { nm.notify(NOTIFICATION_ID, n) } catch (e: SecurityException) { Log.w(TAG, "wifi alert not posted: $e") }
    }

    private class PrefsStore(private val sp: SharedPreferences) : WifiAlertThrottle.Store {
        override fun get(key: String): Long? = if (sp.contains(key)) sp.getLong(key, 0L) else null
        override fun put(key: String, at: Long) { sp.edit().putLong(key, at).apply() }
        override fun keys(): Set<String> = sp.all.keys.toSet()
        override fun remove(key: String) { sp.edit().remove(key).apply() }
    }

    companion object {
        const val TAG = "BfWifi"
        const val CHANNEL = "alerts"
        const val NOTIFICATION_ID = 4301

        /** The `alerts` channel, created only when missing (so A5's name and settings are never overwritten). */
        fun ensureChannel(ctx: Context) {
            val nm = ctx.getSystemService(NotificationManager::class.java) ?: return
            if (nm.getNotificationChannel(CHANNEL) != null) return
            nm.createNotificationChannel(NotificationChannel(CHANNEL, ctx.getString(R.string.wifi_alerts_channel_name), NotificationManager.IMPORTANCE_DEFAULT)
                .apply { description = ctx.getString(R.string.wifi_alerts_channel_desc) })
        }

        /**
         * The alert text for [w], or null: off, a network without a known name (it could be a home network we can't
         * recognise), a home network, one that isn't open/WEP/TKIP, or already alerted within 24 h.
         */
        fun decide(enabled: Boolean, w: CurrentWifi, homePatterns: Set<String>, throttle: WifiAlertThrottle): String? {
            if (!enabled || w.ssid.isEmpty()) return null
            val v = WifiGrade.grade(w.security, WifiGrade.isHome(w.ssid, w.bssid, homePatterns))
            if (v.level != WifiGrade.Level.BAD) return null
            val text = WifiGrade.alertText(w.ssid, w.security) ?: return null
            return if (throttle.tryAcquire(w.ssid)) text else null
        }
    }
}
