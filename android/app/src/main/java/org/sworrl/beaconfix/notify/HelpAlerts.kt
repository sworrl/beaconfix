package org.sworrl.beaconfix.notify

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.distinctUntilChangedBy
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.help.HelpKind
import org.sworrl.beaconfix.help.HelpRepository
import org.sworrl.beaconfix.help.HelpSnapshot
import org.sworrl.beaconfix.tile.TileModel
import org.sworrl.beaconfix.ui.Intents
import java.time.Instant
import java.time.ZoneId
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * When to post the nearest-help heads-up (pure; unit-tested). One alert when the help snapshot's origin has moved
 * more than [MOVE_M] from where the last alert (or the first run) was, at most every [MIN_GAP_MS], never in the
 * quiet hours [QUIET_FROM]:00–[QUIET_TO]:00 local time, only with the setting on and a fresh snapshot that has places.
 * The very first evaluation only records the origin.
 */
object HelpAlertRules {
    const val MOVE_M = 25_000.0
    const val MIN_GAP_MS = 3 * 3600_000L
    const val QUIET_FROM = 22
    const val QUIET_TO = 7

    /** Persisted in `Prefs.helpAlertState` as JSON. [at] = when the last alert was posted (0 = never). */
    @Serializable data class State(val lat: Double = 0.0, val lon: Double = 0.0, val at: Long = 0)

    /** [alert] = post one now; [save] = the state to persist (null = leave it). [reason] is for the log. */
    data class Decision(val alert: Boolean, val save: State?, val reason: String)

    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }
    fun parse(s: String): State? = if (s.isBlank()) null else runCatching { json.decodeFromString(State.serializer(), s) }.getOrNull()
    fun encode(s: State): String = json.encodeToString(State.serializer(), s)

    fun decide(s: HelpSnapshot, last: State?, enabled: Boolean, now: Long, zone: ZoneId = ZoneId.systemDefault()): Decision {
        if (!enabled) return Decision(false, null, "disabled")
        if (s.origin == "none" || (s.originLat == 0.0 && s.originLon == 0.0) || s.places.isEmpty()) return Decision(false, null, "no data")
        if (s.stale) return Decision(false, null, "stale")
        if (last == null) return Decision(false, State(s.originLat, s.originLon, 0L), "first run: origin recorded")
        val moved = distanceM(last.lat, last.lon, s.originLat, s.originLon)
        if (moved <= MOVE_M) return Decision(false, null, "moved ${moved.toInt()} m")
        if (last.at > 0 && now - last.at < MIN_GAP_MS) return Decision(false, null, "too soon")
        if (quiet(now, zone)) return Decision(false, null, "quiet hours")
        return Decision(true, State(s.originLat, s.originLon, now), "moved ${(moved / 1000).toInt()} km")
    }

    fun quiet(now: Long, zone: ZoneId): Boolean {
        val h = Instant.ofEpochMilli(now).atZone(zone).hour
        return h >= QUIET_FROM || h < QUIET_TO
    }

    fun distanceM(la1: Double, lo1: Double, la2: Double, lo2: Double): Double {
        val d2r = Math.PI / 180; val dLa = (la2 - la1) * d2r; val dLo = (lo2 - lo1) * d2r
        val a = sin(dLa / 2) * sin(dLa / 2) + cos(la1 * d2r) * cos(la2 * d2r) * sin(dLo / 2) * sin(dLo / 2)
        return 2 * 6371000 * atan2(sqrt(a), sqrt(1 - a))
    }
}

/**
 * The "new area" heads-up: after a move of more than 25 km, one notification on the `alerts` channel naming the
 * nearest children's ER and ER. Tapping opens Help; the one action is Directions. It never runs a search itself —
 * it only watches [HelpRepository.snapshot].
 */
@Singleton
class HelpAlerts @Inject constructor(
    @ApplicationContext private val app: Context,
    private val prefs: Prefs,
    private val help: HelpRepository,
) {
    private var job: Job? = null

    /** The shared `alerts` channel (the Wi-Fi safety alerts post on it too). Idempotent. */
    fun ensureChannel(ctx: Context) {
        val nm = ctx.getSystemService(NotificationManager::class.java) ?: return
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL, ctx.getString(R.string.channel_alerts_name), NotificationManager.IMPORTANCE_DEFAULT)
                .apply { description = ctx.getString(R.string.channel_alerts_desc) }
        )
    }

    /** Watch the help snapshot for the life of [scope]. */
    @Synchronized
    fun start(scope: CoroutineScope) {
        job?.cancel()
        job = scope.launch {
            help.snapshot.filter { it.fetchedAt > 0 }.distinctUntilChangedBy { it.fetchedAt }.collect { s ->
                runCatching { evaluate(s) }.onFailure { Log.w(TAG, "evaluate failed", it) }
            }
        }
    }

    private suspend fun evaluate(s: HelpSnapshot, now: Long = System.currentTimeMillis()) {
        val d = HelpAlertRules.decide(s, HelpAlertRules.parse(prefs.helpAlertState.first()), prefs.helpAlerts.first(), now)
        Log.i(TAG, d.reason)
        if (d.alert && !post(s)) return          // could not post (no permission): keep the old state, try again later
        d.save?.let { prefs.setHelpAlertState(HelpAlertRules.encode(it)) }
    }

    @android.annotation.SuppressLint("MissingPermission")   // checked in canPost()
    private fun post(s: HelpSnapshot): Boolean {
        if (!canPost()) return false
        ensureChannel(app)
        val text = TileModel.alertText(s)
        val open = PendingIntent.getActivity(app, REQ_OPEN, Intent(app, MainActivity::class.java).putExtra("action", "help").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val target = s.first(HelpKind.PEDS_ER) ?: s.first(HelpKind.ER)
        val public = NotificationCompat.Builder(app, CHANNEL).setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(app.getString(R.string.alerts_help_public)).build()
        val b = NotificationCompat.Builder(app, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification).setColor(0xFF35D6FF.toInt())
            .setContentTitle(app.getString(R.string.alerts_help_title))
            .setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(text))
            .setContentIntent(open).setAutoCancel(true)
            .setCategory(NotificationCompat.CATEGORY_RECOMMENDATION)
            .setVisibility(NotificationCompat.VISIBILITY_PRIVATE).setPublicVersion(public)
        if (target != null) {
            val go = PendingIntent.getActivity(app, REQ_GO, Intent(Intent.ACTION_VIEW, Uri.parse(Intents.geoUri(target.lat, target.lon, target.name))).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            b.addAction(0, app.getString(R.string.alerts_help_directions), go)
        }
        NotificationManagerCompat.from(app).notify(ID, b.build())
        return true
    }

    private fun canPost(): Boolean =
        (Build.VERSION.SDK_INT < 33 || ContextCompat.checkSelfPermission(app, android.Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) &&
            NotificationManagerCompat.from(app).areNotificationsEnabled()

    companion object {
        const val CHANNEL = "alerts"
        const val ID = 4310
        private const val REQ_OPEN = 4311
        private const val REQ_GO = 4312
        private const val TAG = "BfHelpAlert"
    }
}
