package org.sworrl.beaconfix.alpr

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.media.AudioAttributes
import android.media.RingtoneManager
import android.net.Uri
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.alpr.core.HotlistMatch
import javax.inject.Inject
import javax.inject.Singleton

/** Notifications of the dash cam: the foreground notice, hotlist matches, possible matches, re-pair. */
@Singleton
class AlprAlerts @Inject constructor(@ApplicationContext private val ctx: Context) {
    private val nm = NotificationManagerCompat.from(ctx)
    private val lastPosted = HashMap<String, Long>()

    fun ensureChannels() {
        val m = ctx.getSystemService(NotificationManager::class.java)
        m.createNotificationChannel(NotificationChannel(CH_DASHCAM, ctx.getString(R.string.alpr_channel_dashcam), NotificationManager.IMPORTANCE_LOW))
        m.createNotificationChannel(NotificationChannel(CH_HOTLIST, ctx.getString(R.string.alpr_channel_hotlist), NotificationManager.IMPORTANCE_HIGH).apply {
            description = ctx.getString(R.string.alpr_channel_hotlist_desc)
            enableVibration(true); vibrationPattern = longArrayOf(0, 600, 250, 600, 250, 900)
            setSound(RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM) ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION),
                AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION_EVENT).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
            setBypassDnd(false)
        })
        m.createNotificationChannel(NotificationChannel(CH_POSSIBLE, ctx.getString(R.string.alpr_channel_possible), NotificationManager.IMPORTANCE_DEFAULT))
    }

    fun openScreen(): PendingIntent = PendingIntent.getActivity(ctx, 7301,
        Intent(ctx, MainActivity::class.java).putExtra("action", "alpr").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

    private fun dial911(): PendingIntent = PendingIntent.getActivity(ctx, 7302, Intent(Intent.ACTION_DIAL, Uri.parse("tel:911")).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), PendingIntent.FLAG_IMMUTABLE)

    /** Once per entry per [REPEAT_MS]. */
    @Synchronized private fun due(key: String, now: Long): Boolean {
        val prev = lastPosted[key]
        if (prev != null && now - prev < REPEAT_MS) return false
        lastPosted[key] = now; return true
    }

    fun hotlistHit(m: HotlistMatch, crop: Bitmap?, place: String) {
        val now = System.currentTimeMillis()
        if (!due("hit:" + m.entry.id + m.read, now)) return
        val what = listOf(m.entry.alertType, m.entry.title).filter { it.isNotBlank() }.joinToString(" · ").ifBlank { "Hotlist" }
        val title = "HOTLIST MATCH: ${m.read} — $what"
        val text = "Call 911 with location, time and direction. Do not approach or follow."
        val big = buildString {
            append(text)
            if (m.entry.vehicleDesc.isNotBlank()) append("\nVehicle: ").append(m.entry.vehicleDesc)
            if (m.entry.plateState.isNotBlank()) append("\nPlate state: ").append(m.entry.plateState)
            if (m.entry.summary.isNotBlank()) append("\n").append(m.entry.summary)
            if (place.isNotBlank()) append("\nSeen at: ").append(place)
        }
        val b = NotificationCompat.Builder(ctx, CH_HOTLIST).setSmallIcon(R.drawable.alpr_ic_camera).setColor(0xFFFF4D4D.toInt())
            .setContentTitle(title).setContentText(text).setPriority(NotificationCompat.PRIORITY_MAX).setCategory(NotificationCompat.CATEGORY_ALARM)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC).setAutoCancel(false).setContentIntent(openScreen())
            .addAction(0, "Call 911", dial911())
        if (crop != null) b.setLargeIcon(crop).setStyle(NotificationCompat.BigPictureStyle().bigPicture(crop).setSummaryText(big))
        else b.setStyle(NotificationCompat.BigTextStyle().bigText(big))
        post(("hit" + m.entry.id).hashCode(), b)
    }

    fun possibleMatch(m: HotlistMatch, crop: Bitmap?) {
        val now = System.currentTimeMillis()
        if (!due("possible:" + m.entry.id + m.read, now)) return
        val what = listOf(m.entry.alertType, m.entry.title).filter { it.isNotBlank() }.joinToString(" · ")
        // either the read equals the listed plate but is not confirmed yet, or the listed plate is an alternative the
        // recognizer gave for this plate (its per-character candidates)
        val why = if (m.candidate == m.read) "Read once or with low confidence, not confirmed over several frames."
            else "The recognizer's alternatives for this plate include it (about ${pct(m.ratio)} as likely as the read)."
        val b = NotificationCompat.Builder(ctx, CH_POSSIBLE).setSmallIcon(R.drawable.alpr_ic_camera).setColor(0xFFFF9F43.toInt())
            .setContentTitle("Possible hotlist match: ${m.read} ≈ ${m.entry.plate}")
            .setContentText("$what — $why Check before acting; do not approach or follow.")
            .setStyle(NotificationCompat.BigTextStyle().bigText("$what\nRead ${m.read}, listed ${m.entry.plate}${if (m.entry.vehicleDesc.isNotBlank()) "\nVehicle: " + m.entry.vehicleDesc else ""}\n$why Check before acting; do not approach or follow."))
            .setContentIntent(openScreen()).setAutoCancel(true)
        if (crop != null) b.setLargeIcon(crop)
        post(("possible" + m.entry.id).hashCode(), b)
    }

    private fun pct(r: Double) = if (r >= 1.0) "equally" else "${(r * 100).toInt().coerceAtLeast(1)} %"

    fun rePair() {
        val b = NotificationCompat.Builder(ctx, CH_POSSIBLE).setSmallIcon(R.drawable.alpr_ic_camera)
            .setContentTitle("FalconEyez: pairing revoked")
            .setContentText("The dash cam stopped. Pair again from BeaconFix → Detector → Use camera as ALPR.")
            .setContentIntent(openScreen()).setAutoCancel(true)
        post(NOTIF_REPAIR, b)
    }

    fun notice(title: String, text: String) {
        post(NOTIF_NOTICE, NotificationCompat.Builder(ctx, CH_POSSIBLE).setSmallIcon(R.drawable.alpr_ic_camera).setContentTitle(title).setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(text)).setContentIntent(openScreen()).setAutoCancel(true))
    }

    private fun post(id: Int, b: NotificationCompat.Builder) {
        runCatching { nm.notify(id, b.build()) }   // SecurityException without POST_NOTIFICATIONS
    }

    companion object {
        const val CH_DASHCAM = "alpr_dashcam"
        const val CH_HOTLIST = "alpr_hotlist"
        const val CH_POSSIBLE = "alpr_possible"
        const val NOTIF_FOREGROUND = 7310
        const val NOTIF_REPAIR = 7311
        const val NOTIF_NOTICE = 7312
        const val REPEAT_MS = 10 * 60_000L
    }
}
