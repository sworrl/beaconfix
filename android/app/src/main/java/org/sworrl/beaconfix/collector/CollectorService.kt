package org.sworrl.beaconfix.collector

import android.app.Notification
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.estimate.ScanSample
import javax.inject.Inject
import javax.inject.Singleton

data class CollectorState(
    /** the presence service is alive (its notification is in the shade) */
    val presence: Boolean = false,
    /** the collection loop is running */
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
 * BeaconFix's presence on the phone: a foreground service that owns the permanent status notification and, while the
 * collector is switched on, scans Wi-Fi and records one observation per BSSID per scan at the phone's position.
 * It runs whenever the status notification is enabled (the default): started by the app, the widgets, the notification's
 * buttons, and after boot / update by [BootReceiver]; START_STICKY brings it back after process death.
 * Two collection cadences: background (every ~60 s, honours scan throttling) and survey mode (as fast as the OS allows
 * while the app shows the Survey tab). Device ranging (Wi-Fi RTT / BLE) also runs under this service while it is up.
 */
@AndroidEntryPoint
class CollectorService : LifecycleService() {
    @Inject lateinit var recorder: ObservationRecorder
    @Inject lateinit var status: CollectorStatus
    @Inject lateinit var scanner: WifiScanner
    @Inject lateinit var prefs: Prefs
    @Inject lateinit var widgets: org.sworrl.beaconfix.widget.WidgetUpdater
    @Inject lateinit var notifier: org.sworrl.beaconfix.widget.StatusNotifier
    @Inject lateinit var ranging: org.sworrl.beaconfix.ranging.RangingRepository
    private var loop: Job? = null

    override fun onCreate() {
        super.onCreate()
        // the pref drives the loop; the notification (and the service) stay either way
        lifecycleScope.launch { prefs.collectorOn.distinctUntilChanged().collect { on -> if (on) startLoop() else stopLoop() } }
        lifecycleScope.launch { prefs.statusNotification.distinctUntilChanged().collect { show -> if (!show && !status.state.value.survey) stopSelf() } }
        lifecycleScope.launch { ranging.presence(true) }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        when (intent?.action) {
            ACTION_STOP -> { stopSelf(); return START_NOT_STICKY }
            ACTION_SURVEY_ON -> status.update { it.copy(survey = true) }
            ACTION_SURVEY_OFF -> { status.update { it.copy(survey = false) }; lifecycleScope.launch { if (!prefs.statusNotification.first()) stopSelf() else if (!prefs.collectorOn.first()) stopLoop() } }
        }
        if (!goForeground()) { stopSelf(); return START_NOT_STICKY }
        status.update { it.copy(presence = true, throttled = scanner.throttlingOn()) }
        if (intent?.action == ACTION_SURVEY_ON) lifecycleScope.launch { if (!prefs.collectorOn.first()) startLoop() }   // a survey scans even with background collection off
        widgets.touch("collector")
        return START_STICKY
    }

    /** Foreground with the types Android 14+ checks: location when we may use it, connectedDevice (Wi-Fi/BLE ranging) always. */
    private fun goForeground(): Boolean {
        if (Build.VERSION.SDK_INT < 29) return runCatching { ServiceCompat.startForeground(this, NOTIF_ID, notification(), 0) }.isSuccess
        val loc = ContextCompat.checkSelfPermission(this, android.Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED ||
            ContextCompat.checkSelfPermission(this, android.Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED
        val types = listOfNotNull(if (loc) ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION or ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE else null, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        for (t in types) {
            try { ServiceCompat.startForeground(this, NOTIF_ID, notification(), t); return true }
            catch (e: Exception) { status.update { it.copy(error = "foreground refused: ${e.message}") } }     // e.g. location from the background without while-in-use access
        }
        return false
    }

    private fun startLoop() { if (loop == null) loop = lifecycleScope.launch { run() } }
    private fun stopLoop() { loop?.cancel(); loop = null; status.update { it.copy(running = false, survey = false) }; refreshNotification() }

    private suspend fun run() {
        status.update { it.copy(running = true, throttled = scanner.throttlingOn()) }
        while (currentCoroutineContext().isActive) {
            val survey = status.state.value.survey
            val interval = if (survey) (if (scanner.throttlingOn()) 30_000L else 4_000L) else prefs.collectIntervalSec.first() * 1000L
            try { recorder.scanAndRecord(fresh = true) } catch (e: Exception) { status.update { it.copy(error = e.message ?: e.toString()) } }
            delay(3000); refreshNotification()
            delay((interval - 3000).coerceAtLeast(1000))
        }
    }

    private var lastState = org.sworrl.beaconfix.widget.WidgetState()
    private fun notification(): Notification = notifier.build(lastState, ongoing = true, collectorState = widgets.collectorText(), events = widgets.recent)
    private fun refreshNotification() { lifecycleScope.launch { runCatching { lastState = widgets.current(); notifier.post(lastState, true, widgets.collectorText(), widgets.recent) } } }

    override fun onDestroy() {
        loop?.cancel(); loop = null
        status.update { it.copy(presence = false, running = false, survey = false) }
        runCatching { ranging.presence(false) }
        widgets.touch("collector")
        super.onDestroy()
    }

    companion object {
        const val NOTIF_ID = 42
        const val ACTION_STOP = "org.sworrl.beaconfix.STOP"
        const val ACTION_SURVEY_ON = "org.sworrl.beaconfix.SURVEY_ON"
        const val ACTION_SURVEY_OFF = "org.sworrl.beaconfix.SURVEY_OFF"
        /** Start the presence (idempotent; may throw ForegroundServiceStartNotAllowedException from the background — callers catch). */
        fun start(ctx: Context, action: String? = null) = ContextCompat.startForegroundService(ctx, Intent(ctx, CollectorService::class.java).apply { if (action != null) setAction(action) })
        /** Bring the presence up unless the user hid the status notification; never throws. */
        suspend fun ensure(ctx: Context, prefs: Prefs) { if (prefs.statusNotification.first()) runCatching { start(ctx) } }
        /** Only when the user hides the status notification: the presence goes away (and with it reliable background collection). */
        fun stop(ctx: Context) { runCatching { ctx.startService(Intent(ctx, CollectorService::class.java).setAction(ACTION_STOP)) } }
    }
}
