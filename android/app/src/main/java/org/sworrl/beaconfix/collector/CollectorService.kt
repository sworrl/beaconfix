package org.sworrl.beaconfix.collector

import android.app.Notification
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.BeaconFixApp
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.estimate.ScanSample
import javax.inject.Inject
import javax.inject.Singleton

data class CollectorState(
    val running: Boolean = false,
    val survey: Boolean = false,
    val lastScanAt: Long = 0,
    val lastFixAt: Long = 0,
    val lastFixAcc: Double = -1.0,
    val lastFixSource: String = "",
    val apsInScan: Int = 0,
    val recordedTotal: Long = 0,
    val skippedNoFix: Long = 0,
    val throttled: Boolean = false,
    val scan: List<ScanSample> = emptyList(),
    val history: Map<String, List<Int>> = emptyMap(),   // bssid → last RSSI values for sparklines
    val error: String = "",
)

/** Live status shared between the service and the UI. */
@Singleton
class CollectorStatus @Inject constructor() {
    private val _state = MutableStateFlow(CollectorState())
    val state: StateFlow<CollectorState> = _state
    fun update(f: (CollectorState) -> CollectorState) = _state.update(f)
}

/**
 * Foreground service: scan Wi-Fi, take the fused position, record one observation per BSSID per scan.
 * Two cadences: background collection (every ~60 s, honours scan throttling) and survey mode
 * (as fast as the OS allows while the app shows the Survey tab).
 */
@AndroidEntryPoint
class CollectorService : LifecycleService() {
    @Inject lateinit var recorder: ObservationRecorder
    @Inject lateinit var status: CollectorStatus
    @Inject lateinit var scanner: WifiScanner
    @Inject lateinit var prefs: Prefs
    private var loop: Job? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        when (intent?.action) {
            ACTION_STOP -> { stopSelf(); return START_NOT_STICKY }
            ACTION_SURVEY_ON -> status.update { it.copy(survey = true) }
            ACTION_SURVEY_OFF -> status.update { it.copy(survey = false) }
        }
        ServiceCompat.startForeground(this, NOTIF_ID, notification(), if (Build.VERSION.SDK_INT >= 29) ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION else 0)
        if (loop == null) loop = lifecycleScope.launch { run() }
        status.update { it.copy(running = true, throttled = scanner.throttlingOn()) }
        return START_STICKY
    }

    private suspend fun run() {
        while (lifecycleScope.isActive) {
            val survey = status.state.value.survey
            val interval = if (survey) (if (scanner.throttlingOn()) 30_000L else 4_000L) else prefs.collectIntervalSec.first() * 1000L
            try { recorder.scanAndRecord(fresh = true) } catch (e: Exception) { status.update { it.copy(error = e.message ?: e.toString()) } }
            delay(interval)
        }
    }

    private fun notification(): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 1, Intent(this, CollectorService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE)
        return NotificationCompat.Builder(this, BeaconFixApp.CHANNEL_COLLECTOR)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(getString(R.string.notif_collecting))
            .setContentText("Recording Wi-Fi beacons at your position")
            .setOngoing(true).setSilent(true).setContentIntent(open)
            .addAction(0, getString(R.string.notif_stop), stop)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .build()
    }

    override fun onDestroy() { loop?.cancel(); status.update { it.copy(running = false, survey = false) }; super.onDestroy() }

    companion object {
        const val NOTIF_ID = 42
        const val ACTION_STOP = "org.sworrl.beaconfix.STOP"
        const val ACTION_SURVEY_ON = "org.sworrl.beaconfix.SURVEY_ON"
        const val ACTION_SURVEY_OFF = "org.sworrl.beaconfix.SURVEY_OFF"
        fun start(ctx: Context, action: String? = null) = ContextCompat.startForegroundService(ctx, Intent(ctx, CollectorService::class.java).apply { if (action != null) setAction(action) })
        fun stop(ctx: Context) = ctx.startService(Intent(ctx, CollectorService::class.java).setAction(ACTION_STOP))
    }
}
