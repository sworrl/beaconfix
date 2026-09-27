package org.sworrl.beaconfix.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.core.app.NotificationManagerCompat
import androidx.lifecycle.compose.LifecycleResumeEffect
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.ui.theme.Gold
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Red
import org.sworrl.beaconfix.ui.theme.Slate

/** What the phone allows BeaconFix, read fresh on every resume (see [HealthProbe]). */
data class HealthInput(
    val sdk: Int,
    val fine: Boolean,
    val coarse: Boolean,
    val background: Boolean,
    val notificationPermission: Boolean,
    val notificationsEnabled: Boolean,
    val nearbyWifi: Boolean,
    val batteryUnrestricted: Boolean,
    val powerSaver: Boolean,
)

/** How an item gets fixed. Background location is only ever requested after foreground location is granted. */
enum class HealthFix { NONE, REQUEST_FOREGROUND, REQUEST_BACKGROUND, REQUEST_NOTIFICATIONS, OPEN_NOTIFICATION_SETTINGS, REQUEST_NEARBY, OPEN_BATTERY_SETTINGS, OPEN_POWER_SAVER }

/**
 * One line of the health card. [problem] = it breaks collection or alerts (shown on the compact Home card);
 * the rest are advice (the full card in Settings only).
 */
data class HealthItem(val key: String, val ok: Boolean, val problem: Boolean, val fix: HealthFix)

/** The system-health checklist (pure; unit-tested). */
data class HealthState(val items: List<HealthItem>) {
    /** What the compact card shows. */
    val problems: List<HealthItem> get() = items.filter { !it.ok && it.problem }
    /** The compact card is hidden when this is true. */
    val allOk: Boolean get() = problems.isEmpty()
    fun item(key: String): HealthItem? = items.firstOrNull { it.key == key }

    companion object {
        const val PRECISE = "precise"
        const val BACKGROUND = "background"
        const val NOTIFICATIONS = "notifications"
        const val NEARBY = "nearby"
        const val BATTERY = "battery"
        const val POWER_SAVER = "power_saver"

        fun from(i: HealthInput): HealthState = HealthState(buildList {
            val anyLocation = i.fine || i.coarse
            add(HealthItem(PRECISE, i.fine, problem = true, fix = if (i.fine) HealthFix.NONE else HealthFix.REQUEST_FOREGROUND))
            if (i.sdk >= 29) {
                // "Allow all the time": asked for only once foreground location is granted; nagging before that is noise
                add(HealthItem(BACKGROUND, i.background, problem = anyLocation, fix = if (i.background || !anyLocation) HealthFix.NONE else HealthFix.REQUEST_BACKGROUND))
            }
            val notifOk = (i.sdk < 33 || i.notificationPermission) && i.notificationsEnabled
            add(HealthItem(NOTIFICATIONS, notifOk, problem = true, fix = when {
                notifOk -> HealthFix.NONE
                i.sdk >= 33 && !i.notificationPermission -> HealthFix.REQUEST_NOTIFICATIONS
                else -> HealthFix.OPEN_NOTIFICATION_SETTINGS
            }))
            if (i.sdk >= 33) add(HealthItem(NEARBY, i.nearbyWifi, problem = true, fix = if (i.nearbyWifi) HealthFix.NONE else HealthFix.REQUEST_NEARBY))
            add(HealthItem(BATTERY, i.batteryUnrestricted, problem = false, fix = if (i.batteryUnrestricted) HealthFix.NONE else HealthFix.OPEN_BATTERY_SETTINGS))
            add(HealthItem(POWER_SAVER, !i.powerSaver, problem = false, fix = if (i.powerSaver) HealthFix.OPEN_POWER_SAVER else HealthFix.NONE))
        })
    }
}

/** Reads [HealthInput] from the system. */
object HealthProbe {
    fun read(ctx: Context): HealthInput {
        val pm = ctx.getSystemService(PowerManager::class.java)
        return HealthInput(
            sdk = Build.VERSION.SDK_INT,
            fine = Permissions.hasForeground(ctx),
            coarse = Permissions.hasCoarse(ctx),
            background = Permissions.hasBackground(ctx),
            notificationPermission = Permissions.hasNotificationPermission(ctx),
            notificationsEnabled = NotificationManagerCompat.from(ctx).areNotificationsEnabled(),
            nearbyWifi = Permissions.hasNearbyWifi(ctx),
            batteryUnrestricted = pm?.isIgnoringBatteryOptimizations(ctx.packageName) ?: true,
            powerSaver = pm?.isPowerSaveMode ?: false,
        )
    }
}

/**
 * Permissions and battery settings at a glance. [compact] = the Home version: only the problems, and nothing at all
 * when everything is fine. The full version (Settings) also shows the battery advice. Rechecked on every resume, so a
 * grant made in system settings shows up as soon as you come back.
 */
@Composable
fun SystemHealthCard(compact: Boolean) {
    val ctx = LocalContext.current
    var tick by remember { mutableIntStateOf(0) }
    LifecycleResumeEffect(Unit) { tick++; onPauseOrDispose { } }
    val state = remember(tick) { HealthState.from(HealthProbe.read(ctx)) }
    val many = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { tick++ }
    val one = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { tick++ }
    val background = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        // Android 11+ may refuse without showing anything (asked too often): take the user to the app's settings instead
        if (!granted && !Permissions.hasBackground(ctx)) openSettings(ctx, appDetails(ctx))
        tick++
    }
    val shown = if (compact) state.problems else state.items
    if (compact && shown.isEmpty()) return
    InfoCard(stringResource(if (compact) R.string.a10_health_title_compact else R.string.a10_health_title)) {
        if (!compact && state.allOk) Text(stringResource(R.string.a10_health_all_ok), color = Green, style = MaterialTheme.typography.bodySmall)
        for (item in shown) {
            val (title, detail, button) = texts(item)
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.Top) {
                val glyph = when { item.ok -> "✓"; item.problem -> "!"; else -> "i" }
                Text(glyph, color = when { item.ok -> Green; item.problem -> Red; else -> Gold }, style = MaterialTheme.typography.titleMedium, modifier = Modifier.clearAndSetSemantics { })
                Column(Modifier.weight(1f).semantics(mergeDescendants = true) { }) {
                    Text(title, style = MaterialTheme.typography.bodyMedium)
                    if (!item.ok) Text(detail, color = Slate, style = MaterialTheme.typography.bodySmall)
                }
            }
            if (!item.ok && item.fix != HealthFix.NONE && button.isNotEmpty()) {
                OutlinedButton(onClick = {
                    when (item.fix) {
                        HealthFix.REQUEST_FOREGROUND -> many.launch(arrayOf(android.Manifest.permission.ACCESS_FINE_LOCATION, android.Manifest.permission.ACCESS_COARSE_LOCATION))
                        HealthFix.REQUEST_BACKGROUND -> Permissions.background().firstOrNull()?.let { background.launch(it) }
                        HealthFix.REQUEST_NOTIFICATIONS -> if (Build.VERSION.SDK_INT >= 33) one.launch(android.Manifest.permission.POST_NOTIFICATIONS)
                        HealthFix.REQUEST_NEARBY -> if (Build.VERSION.SDK_INT >= 33) one.launch(android.Manifest.permission.NEARBY_WIFI_DEVICES)
                        HealthFix.OPEN_NOTIFICATION_SETTINGS -> openSettings(ctx, Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).putExtra(Settings.EXTRA_APP_PACKAGE, ctx.packageName))
                        HealthFix.OPEN_BATTERY_SETTINGS -> openSettings(ctx, Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
                        HealthFix.OPEN_POWER_SAVER -> openSettings(ctx, Intent(Settings.ACTION_BATTERY_SAVER_SETTINGS))
                        HealthFix.NONE -> Unit
                    }
                }) { Text(button) }
            }
        }
    }
}

@Composable
private fun texts(item: HealthItem): Triple<String, String, String> = when (item.key) {
    HealthState.PRECISE -> Triple(stringResource(if (item.ok) R.string.a10_health_precise_ok else R.string.a10_health_precise), stringResource(R.string.a10_health_precise_detail), stringResource(R.string.a10_health_precise_button))
    HealthState.BACKGROUND -> Triple(stringResource(if (item.ok) R.string.a10_health_background_ok else R.string.a10_health_background),
        stringResource(if (item.fix == HealthFix.NONE) R.string.a10_health_background_first else R.string.a10_health_background_detail), stringResource(R.string.a10_health_background_button))
    HealthState.NOTIFICATIONS -> Triple(stringResource(if (item.ok) R.string.a10_health_notifications_ok else R.string.a10_health_notifications), stringResource(R.string.a10_health_notifications_detail), stringResource(R.string.a10_health_notifications_button))
    HealthState.NEARBY -> Triple(stringResource(if (item.ok) R.string.a10_health_nearby_ok else R.string.a10_health_nearby), stringResource(R.string.a10_health_nearby_detail), stringResource(R.string.a10_health_nearby_button))
    HealthState.BATTERY -> Triple(stringResource(if (item.ok) R.string.a10_health_battery_ok else R.string.a10_health_battery), stringResource(R.string.a10_health_battery_detail), stringResource(R.string.a10_health_battery_button))
    HealthState.POWER_SAVER -> Triple(stringResource(if (item.ok) R.string.a10_health_saver_ok else R.string.a10_health_saver), stringResource(R.string.a10_health_saver_detail), stringResource(R.string.a10_health_saver_button))
    else -> Triple(item.key, "", "")
}

private fun appDetails(ctx: Context) = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", ctx.packageName, null))

private fun openSettings(ctx: Context, intent: Intent) {
    if (ctx !is android.app.Activity) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    runCatching { ctx.startActivity(intent) }.onFailure { runCatching { ctx.startActivity(appDetails(ctx).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) } }
}
