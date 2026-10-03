// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.db.PlateEventEntity
import org.sworrl.beaconfix.detector.DetectionType
import org.sworrl.beaconfix.detector.DetectorAlertManager
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Alerts for plate events (docs/SIGHTINGS.md §6), worded for what the data shows: a pass of an ALPR camera means the
 * plate was *likely* read; a plate search comes from a released audit log. Traffic-camera passes never alert, and a
 * batch of old events (a backfill, a first sync) is one summary, not a flood.
 */
@Singleton
class SightingAlerts @Inject constructor(@ApplicationContext private val ctx: Context, private val detector: DetectorAlertManager) {
    private val nm = ctx.getSystemService(NotificationManager::class.java)

    fun ensureChannels() {
        nm.createNotificationChannel(NotificationChannel(CH_EVENTS, "ALPR passes and plate searches", NotificationManager.IMPORTANCE_HIGH).apply {
            description = "A pass of a plate-reading camera, or a search for your plate found in a released Flock audit log"
        })
        nm.createNotificationChannel(NotificationChannel(CH_SUMMARY, "Sightings summaries", NotificationManager.IMPORTANCE_LOW).apply {
            description = "One line for older events that arrived together (route backfill, first sync)"
        })
    }

    private fun allowed() = Build.VERSION.SDK_INT < 33 || ContextCompat.checkSelfPermission(ctx, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED

    private fun details(uid: String): PendingIntent = PendingIntent.getActivity(ctx, uid.hashCode(),
        Intent(Intent.ACTION_VIEW, Uri.parse(Sightings.deepLink(uid)), ctx, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

    private fun source(url: String, req: Int): PendingIntent = PendingIntent.getActivity(ctx, req,
        Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

    /** Whether [e] gets its own alert at all (§6): ALPR passes and plate searches only. */
    fun alertable(e: PlateEventEntity): Boolean = Sightings.alertable(e)

    /** One event's alert. Returns whether it was posted. */
    fun alert(e: PlateEventEntity, sound: Boolean = true): Boolean {
        val text = Sightings.alertText(e) ?: return false
        if (!allowed()) return false
        val b = NotificationCompat.Builder(ctx, CH_EVENTS).setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(text.first).setContentText(text.second).setStyle(NotificationCompat.BigTextStyle().bigText(text.second))
            .setPriority(NotificationCompat.PRIORITY_HIGH).setAutoCancel(true).setContentIntent(details(e.uid))
            .addAction(0, "Details", details(e.uid))
        e.sourceUrl?.takeIf { it.startsWith("http://") || it.startsWith("https://") }?.let { b.addAction(0, "Source", source(it, e.uid.hashCode() xor 0x5ce)) }
        nm.notify(TAG, e.uid.hashCode(), b.build())
        if (sound && e.kind == PlateEvents.CAMERA_PASS) runCatching { detector.triggerAlert(DetectionType.ALPR_FLOCK) }
        return true
    }

    /** The one notification for a batch of older events (§2.5, §6). */
    fun summary(passes: Int, searches: Int, sinceMs: Long) {
        if (passes + searches == 0 || !allowed()) return
        val title = Sightings.summaryTitle(passes, searches, sinceMs)
        val open = PendingIntent.getActivity(ctx, 0x51c47, Intent(Intent.ACTION_VIEW, Uri.parse("beaconfix://sightings"), ctx, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val n = NotificationCompat.Builder(ctx, CH_SUMMARY).setSmallIcon(R.drawable.ic_notification).setContentTitle(title)
            .setContentText("From route history and released audit logs. Open Sightings for the list.").setAutoCancel(true).setContentIntent(open).build()
        nm.notify(TAG, SUMMARY_ID, n)
    }

    companion object {
        const val CH_EVENTS = "sightings"
        const val CH_SUMMARY = "sightings_summary"
        private const val TAG = "sighting"
        private const val SUMMARY_ID = 0x51c4
    }
}
