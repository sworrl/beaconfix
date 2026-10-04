package org.sworrl.beaconfix.alpr

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.location.Geocoder
import android.location.Location
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import android.util.Size
import android.view.OrientationEventListener
import android.view.Surface
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.resolutionselector.AspectRatioStrategy
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.alpr.core.Geofence
import org.sworrl.beaconfix.alpr.core.HitBody
import org.sworrl.beaconfix.alpr.core.MatchKind
import org.sworrl.beaconfix.alpr.core.AlprJson
import org.sworrl.beaconfix.alpr.core.CameraMotion
import org.sworrl.beaconfix.alpr.core.ThermalPolicy
import org.sworrl.beaconfix.alpr.vision.AlprPipeline
import org.sworrl.beaconfix.alpr.vision.FrameContext
import org.sworrl.beaconfix.alpr.vision.FrameOutcome
import org.sworrl.beaconfix.alpr.vision.GyroMotion
import org.sworrl.beaconfix.alpr.vision.PassOptions
import org.sworrl.beaconfix.alpr.vision.ShortShutter
import org.sworrl.beaconfix.alpr.vision.YuvFrame
import org.sworrl.beaconfix.collector.LocationSource
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import javax.inject.Inject

/**
 * The dash cam: a camera-type foreground service that runs CameraX ImageAnalysis on the main back camera, reads plates
 * on the device, alerts on hotlist matches and hands plate events to [AlprUplink]. It keeps running with the screen
 * off. It captures only when every gate is open: camera permission, a location fix outside the legal geofence (no
 * private ALPR in ME / NH / AR), on the charger (unless the setting allows battery), and not overheating. Video is
 * never recorded; frames touch storage only as the one or two stills of a camera pass (docs/SIGHTINGS.md §3.3).
 */
@AndroidEntryPoint
class DashCamService : LifecycleService() {
    @Inject lateinit var settings: AlprSettings
    @Inject lateinit var status: AlprStatus
    @Inject lateinit var link: FalconLink
    @Inject lateinit var hotlist: HotlistStore
    @Inject lateinit var uplink: AlprUplink
    @Inject lateinit var alerts: AlprAlerts
    @Inject lateinit var locations: LocationSource
    @Inject lateinit var livePasses: org.sworrl.beaconfix.sightings.LivePassTracker
    @Inject lateinit var passFrames: org.sworrl.beaconfix.sightings.DashFrameBuffer
    @Inject lateinit var prefs: org.sworrl.beaconfix.data.Prefs
    @Volatile private var privateInspection = false

    private val main = Handler(Looper.getMainLooper())
    private lateinit var executor: ExecutorService
    private var provider: ProcessCameraProvider? = null
    private var analysis: ImageAnalysis? = null
    private var boundRes = ""
    private var binding = false
    private var pipeline: AlprPipeline? = null          // analysis thread only
    private var pipelineKey = ""
    private val planes = org.sworrl.beaconfix.alpr.vision.PlaneBuffers()   // analysis thread only
    private var wake: PowerManager.WakeLock? = null
    private var orientation: OrientationEventListener? = null
    private var thermalListener: PowerManager.OnThermalStatusChangedListener? = null

    @Volatile private var capturing = false
    @Volatile private var lastLoc: Location? = null
    @Volatile private var geoState: String? = null
    @Volatile private var geoAt: Location? = null
    private var geoBusy = false
    @Volatile private var charging = false
    @Volatile private var thermal = 0
    /** PowerManager.getThermalHeadroom(10 s), polled every few seconds (API 30+); NaN = unknown. */
    @Volatile private var headroom = Float.NaN
    private val shutter = ShortShutter()
    private var gyro: GyroMotion? = null
    private var lastBurstAt = 0L
    @Volatile private var rotation = Surface.ROTATION_90
    private var lastFrameAt = 0L
    private val frameTimes = ArrayDeque<Long>()
    private var lastNotifAt = 0L
    private var lastNotifText = ""

    private val battery = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) { updateCharging(i) }
    }

    override fun onCreate() {
        super.onCreate()
        alerts.ensureChannels()
        executor = Executors.newSingleThreadExecutor { r -> Thread(r, "alpr-analysis").apply { priority = Thread.NORM_PRIORITY } }
        if (!goForeground()) { stopSelf(); return }
        status.set { it.copy(running = true, paused = "Starting…", modelError = "") }
        ContextCompat.registerReceiver(this, battery, IntentFilter(Intent.ACTION_BATTERY_CHANGED), ContextCompat.RECEIVER_NOT_EXPORTED)?.let { updateCharging(it) }
        val pm = getSystemService(PowerManager::class.java)
        if (Build.VERSION.SDK_INT >= 29) {
            thermal = pm.currentThermalStatus
            thermalListener = PowerManager.OnThermalStatusChangedListener { s -> thermal = s; reconcile() }.also { pm.addThermalStatusListener(ContextCompat.getMainExecutor(this), it) }
        }
        wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "beaconfix:alpr").apply { setReferenceCounted(false) }
        gyro = GyroMotion(this)
        lifecycleScope.launch {
            prefs.privateInspectionActive.collect { active ->
                privateInspection = active
                reconcile()
            }
        }
        if (Build.VERSION.SDK_INT >= 30) lifecycleScope.launch {
            // headroom is rate-limited by the system (≥ 1 s between calls); every 5 s is plenty for a slow quantity
            while (true) {
                if (capturing) {
                    val h = runCatching { pm.getThermalHeadroom(10) }.getOrDefault(Float.NaN)
                    headroom = h
                    status.set { it.copy(headroom = h) }
                }
                delay(5_000)
            }
        }
        orientation = object : OrientationEventListener(this) {
            override fun onOrientationChanged(o: Int) {
                if (o == ORIENTATION_UNKNOWN) return
                val r = when (o) { in 45..134 -> Surface.ROTATION_270; in 135..224 -> Surface.ROTATION_180; in 225..314 -> Surface.ROTATION_90; else -> Surface.ROTATION_0 }
                if (r != rotation) { rotation = r; analysis?.targetRotation = r }
            }
        }.also { if (it.canDetectOrientation()) it.enable() }
        uplink.onRevoked = { main.post { settings.update { it.copy(enabled = false) }; stopSelf() } }
        uplink.start()
        activeInstance = this
        // location: speed / heading for every event, and the legal geofence
        lifecycleScope.launch {
            if (hasLocation()) {
                val l = runCatching { locations.current(5_000) }.getOrNull()
                if (l != null) onLocation(l)
            }
            if (lastLoc == null) reconcile()
            if (hasLocation()) locations.updates(1_000, 0f).catch { Log.w(TAG, "location", it) }.collect { onLocation(it) }
        }
        // hotlist: refresh when due (15 min) while running
        lifecycleScope.launch {
            while (true) { runCatching { hotlist.refresh() }; reconcile(); delay(60_000) }
        }
        lifecycleScope.launch { settings.config.drop(1).collect { reconcile() } }
        lifecycleScope.launch { status.state.collect { maybeUpdateNotification(it) } }
        reconcile()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        if (intent?.action == ACTION_STOP) { settings.update { it.copy(enabled = false) }; stopSelf(); return START_NOT_STICKY }
        return START_STICKY
    }

    override fun onDestroy() {
        activeInstance = null
        capturing = false
        unbind()
        runCatching { unregisterReceiver(battery) }
        orientation?.disable()
        if (Build.VERSION.SDK_INT >= 29) thermalListener?.let { runCatching { getSystemService(PowerManager::class.java).removeThermalStatusListener(it) } }
        if (::executor.isInitialized) { executor.execute { pipeline?.close(); pipeline = null }; executor.shutdown() }
        uplink.onRevoked = null
        uplink.spillAll()
        status.set { it.copy(running = false, paused = null, fps = 0f) }
        super.onDestroy()
    }

    private fun hasPerm(p: String) = ContextCompat.checkSelfPermission(this, p) == PackageManager.PERMISSION_GRANTED
    private fun hasLocation() = hasPerm(Manifest.permission.ACCESS_FINE_LOCATION) || hasPerm(Manifest.permission.ACCESS_COARSE_LOCATION)

    private fun goForeground(): Boolean {
        if (!hasPerm(Manifest.permission.CAMERA)) { alerts.notice("ALPR dash cam", "Camera permission is needed. Open BeaconFix → Detector → Use camera as ALPR."); return false }
        var types = 0
        if (Build.VERSION.SDK_INT >= 30) types = ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or (if (hasLocation()) ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION else 0)
        return try {
            ServiceCompat.startForeground(this, AlprAlerts.NOTIF_FOREGROUND, notification("Starting…"), types); true
        } catch (e: Exception) {
            // Android 14+: a camera service can only start while BeaconFix is in the foreground
            Log.w(TAG, "startForeground", e); alerts.notice("ALPR dash cam", "Couldn't start the camera service: open BeaconFix and switch it on there."); false
        }
    }

    private fun notification(text: String): Notification {
        val stop = PendingIntent.getService(this, 7320, Intent(this, DashCamService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        return NotificationCompat.Builder(this, AlprAlerts.CH_DASHCAM).setSmallIcon(R.drawable.alpr_ic_camera).setColor(0xFF35D6FF.toInt())
            .setContentTitle("ALPR dash cam").setContentText(text).setOngoing(true).setOnlyAlertOnce(true).setSilent(true)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .setContentIntent(alerts.openScreen()).addAction(0, "Stop", stop).build()
    }

    private fun maybeUpdateNotification(s: AlprState) {
        val text = s.paused ?: "Reading plates · ${"%.1f".format(s.fps)} fps · ${s.platesRead} read · ${s.memPending + s.spillCount} to send"
        val now = SystemClock.elapsedRealtime()
        if (text == lastNotifText || (now - lastNotifAt < 5_000 && s.paused == null)) return
        lastNotifText = text; lastNotifAt = now
        runCatching { androidx.core.app.NotificationManagerCompat.from(this).notify(AlprAlerts.NOTIF_FOREGROUND, notification(text)) }
    }

    private fun updateCharging(i: Intent) {
        val plugged = i.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0)
        val now = plugged != 0
        if (now != charging || status.state.value.charging == null) { charging = now; status.set { it.copy(charging = now) }; reconcile() }
    }

    private fun onLocation(l: Location) {
        lastLoc = l
        livePasses.onLocation(l)          // camera passes (docs/SIGHTINGS.md §2); with frames while capturing (§3.3)
        val g = geoAt
        if (!geoBusy && (g == null || g.distanceTo(l) > 3_000 || l.time - g.time > 5 * 60_000)) {
            geoBusy = true
            lifecycleScope.launch {
                val code = withContext(Dispatchers.IO) { geocodeState(l) }
                geoBusy = false; geoAt = l; geoState = code
                if (code != null) status.set { it.copy(region = code) }
                reconcile()
            }
        } else reconcile()
    }

    @Suppress("DEPRECATION")
    private fun geocodeState(l: Location): String? = runCatching {
        if (!Geocoder.isPresent()) return null
        val a = Geocoder(this, Locale.US).getFromLocation(l.latitude, l.longitude, 1)?.firstOrNull() ?: return null
        Geofence.stateCode(a.adminArea, a.countryCode)
    }.getOrNull()

    /** The geocoder's state, if it describes where we are now (within 3 km / 5 min). */
    private fun freshState(): String? {
        val g = geoAt ?: return null; val l = lastLoc ?: return null
        return geoState?.takeIf { g.distanceTo(l) < 3_000 && l.time - g.time < 5 * 60_000 }
    }

    private fun thermalName(t: Int) = when (t) { 0 -> "normal"; 1 -> "light"; 2 -> "moderate"; 3 -> "severe"; 4 -> "critical"; 5 -> "emergency"; 6 -> "shutdown"; else -> "?" }

    /** Opens or closes the capture gate; binds / unbinds the camera accordingly. Main thread. */
    private fun reconcile() {
        if (Looper.myLooper() != Looper.getMainLooper()) { main.post { reconcile() }; return }
        val cfg = settings.value
        val loc = lastLoc
        val blocked = hotlist.hotlist.value.blockedRegions.toSet()
        val geo = loc?.let { Geofence.check(blocked, it.latitude, it.longitude, freshState()) }
        val reason = when {
            privateInspection -> "Paused: Private camera inspection active"
            uiActive -> "Paused: Live viewfinder active on screen"
            !hasPerm(Manifest.permission.CAMERA) -> "No camera permission"
            status.state.value.modelError.isNotEmpty() -> "Models unavailable: ${status.state.value.modelError}"
            Build.VERSION.SDK_INT >= 29 && thermal >= PowerManager.THERMAL_STATUS_SEVERE -> "Paused: phone too hot (${thermalName(thermal)})"
            cfg.chargingOnly && !charging -> "Paused: waiting for the charger (Charging only is on)"
            !hasLocation() -> "Paused: location permission needed (legal geofence)"
            geo != null && geo.blocked -> "Paused: private ALPR is not allowed in ${stateName(geo.region)} (${geo.region})"
            else -> null
        }
        status.set { it.copy(paused = reason, thermal = thermalName(thermal), region = freshState() ?: geo?.region ?: it.region) }
        if (reason == null) bind() else unbind()
    }

    private fun stateName(code: String?) = Geofence.STATE_CODES.entries.firstOrNull { it.value == code }?.key ?: code.orEmpty()

    private fun resolution(cfg: AlprConfig): Pair<Size, AspectRatioStrategy> = when (cfg.resolution) {
        "4k" -> Size(3840, 2160) to AspectRatioStrategy.RATIO_16_9_FALLBACK_AUTO_STRATEGY
        "1080p" -> Size(1920, 1080) to AspectRatioStrategy.RATIO_16_9_FALLBACK_AUTO_STRATEGY
        else -> Size(4080, 3072) to AspectRatioStrategy.RATIO_4_3_FALLBACK_AUTO_STRATEGY
    }

    @SuppressLint("WakelockTimeout")
    private fun bind() {
        val cfg = settings.value
        capturing = true
        if ((analysis != null && boundRes == cfg.resolution) || binding) return
        binding = true
        val future = ProcessCameraProvider.getInstance(this)
        future.addListener({
            binding = false
            if (!capturing) return@addListener
            val p = runCatching { future.get() }.getOrNull() ?: return@addListener
            provider = p
            val (size, aspect) = resolution(cfg)
            val sel = ResolutionSelector.Builder().setAspectRatioStrategy(aspect)
                .setResolutionStrategy(ResolutionStrategy(size, ResolutionStrategy.FALLBACK_RULE_CLOSEST_LOWER_THEN_HIGHER)).build()
            val ab = ImageAnalysis.Builder().setResolutionSelector(sel).setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888).setTargetRotation(rotation)
            shutter.attach(ab)
            val a = ab.build()
            a.setAnalyzer(executor) { img -> analyze(img) }
            try {
                p.unbindAll()
                val cam = p.bindToLifecycle(this, CameraSelector.DEFAULT_BACK_CAMERA, a)
                analysis = a; boundRes = cfg.resolution
                runCatching { shutter.bind(cam) }.onFailure { Log.w(TAG, "shutter", it) }
                gyro?.start()
                passFrames.running = true
                wake?.acquire()
                Log.i(TAG, "camera bound, analysis ${a.resolutionInfo?.resolution}")
            } catch (e: Exception) {
                Log.w(TAG, "bind", e); status.set { it.copy(paused = "Camera unavailable: ${e.message}") }
            }
        }, ContextCompat.getMainExecutor(this))
    }

    private fun unbind() {
        // vehicles still in view become their events now (the analysis thread owns the pipeline)
        if (analysis != null && ::executor.isInitialized && !executor.isShutdown) executor.execute {
            val p = pipeline ?: return@execute
            runCatching { handle(p.flush(hotlist.matcher), status.state.value.cameraSize, SystemClock.elapsedRealtime(), burst = true, step = null) }
                .onFailure { Log.w(TAG, "flush", it) }
        }
        capturing = false
        if (::passFrames.isInitialized) passFrames.running = false
        analysis?.clearAnalyzer()
        shutter.unbind()
        gyro?.stop()
        runCatching { provider?.unbindAll() }
        analysis = null; boundRes = ""
        if (wake?.isHeld == true) wake?.release()
        status.set { it.copy(fps = 0f) }
    }

    /** Analysis thread. The frame is sampled, then released; nothing of it outlives this call except a vehicle's crop. */
    private fun analyze(img: androidx.camera.core.ImageProxy) {
        try {
            if (!capturing) return
            val cfg = settings.value
            val step = ThermalPolicy.step(ThermalPolicy.level(headroom, if (Build.VERSION.SDK_INT >= 29) thermal else 0), cfg.tileCols, cfg.accurateOcr)
            val interval = 1000L / cfg.targetFps.coerceIn(1, 4) * step.intervalFactor
            val now = SystemClock.elapsedRealtime()
            val p = pipeline(cfg) ?: return
            val wall = System.currentTimeMillis()
            if (now - lastFrameAt < interval) {
                // a frame the rate limit skips: re-scan just around plates that are still too small to read
                if (!step.burst || now - lastBurstAt < BURST_GAP_MS) return
                val rot = img.imageInfo.rotationDegrees
                val fw = if (rot % 180 == 0) img.width else img.height; val fh = if (rot % 180 == 0) img.height else img.width
                val windows = p.burstWindows(wall, fw, fh)
                if (windows.isEmpty()) return
                lastBurstAt = now
                val frame = YuvFrame(img, rot, planes)
                val out = p.burst(frame, FrameContext(wall, lastLoc, freshState()), hotlist.matcher, windows, options(step, img))
                handle(out, "${frame.width}×${frame.height}", now, burst = true, step = step)
                return
            }
            lastFrameAt = now
            val frame = YuvFrame(img, img.imageInfo.rotationDegrees, planes)
            shutter.enabled = cfg.shortShutter
            shutter.speedMps = lastLoc?.takeIf { it.hasSpeed() }?.speed ?: 0f
            shutter.onLuma(frame.meanLuma())
            val out = p.process(frame, FrameContext(wall, lastLoc, freshState()), hotlist.matcher, options(step, img))
            handle(out, "${frame.width}×${frame.height}", now, step = step)
            // a camera is near: keep this analysed frame for the pass's images (memory only, ~3 s; docs/SIGHTINGS.md §3.3)
            if (passFrames.wants(System.currentTimeMillis())) {
                val k = minOf(1f, org.sworrl.beaconfix.sightings.DashFrameBuffer.MAX_SIDE.toFloat() / maxOf(frame.width, frame.height))
                val w = maxOf(1, (frame.width * k).toInt()); val h = maxOf(1, (frame.height * k).toInt())
                passFrames.offer(System.currentTimeMillis()) { frame.bitmap(org.sworrl.beaconfix.alpr.core.BoxF(0f, 0f, frame.width.toFloat(), frame.height.toFloat()), w, h) }
            }
        } catch (t: Throwable) {
            Log.w(TAG, "analyze", t)
        } finally {
            img.close()
        }
    }

    /** The pipeline for the current model settings (rebuilt when they change). Analysis thread. */
    private fun pipeline(cfg: AlprConfig): AlprPipeline? {
        val key = "${cfg.xnnpack}/${cfg.accurateOcr}"
        if (pipeline == null || pipelineKey != key) {
            pipeline?.close(); pipeline = null
            pipeline = try { AlprPipeline(this, 4, cfg.xnnpack, cfg.accurateOcr) } catch (t: Throwable) {
                Log.e(TAG, "models", t); status.set { it.copy(modelError = t.message ?: t.javaClass.simpleName) }; reconcile(); return null
            }
            pipelineKey = key
        }
        return pipeline
    }

    /** This pass's options: the heat step, and the camera's rotation since the last pass as an image shift (gyro). */
    private fun options(step: ThermalPolicy.Step, img: androidx.camera.core.ImageProxy): PassOptions {
        val turn = gyro?.take() ?: floatArrayOf(0f, 0f)
        val f = CameraMotion.focalPx(shutter.focalMm, shutter.sensorWidthMm, maxOf(img.width, img.height))
        val (dx, dy) = CameraMotion.pixelShift(turn[0], turn[1], rotation, f)
        return PassOptions(tileCols = step.tileCols, smallFullFrame = step.smallFullFrame, accurateOcr = step.accurateOcr, shiftX = dx, shiftY = dy)
    }

    private fun handle(out: FrameOutcome, size: String, now: Long, burst: Boolean = false, step: ThermalPolicy.Step?) {
        if (!burst) { frameTimes.addLast(now); while (frameTimes.isNotEmpty() && now - frameTimes.first() > 5_000) frameTimes.removeFirst() }
        val fps = if (frameTimes.size < 2) 0f else (frameTimes.size - 1) * 1000f / (frameTimes.last() - frameTimes.first()).coerceAtLeast(1)
        val read = out.plates.count { (it.read?.text?.length ?: 0) >= 2 }
        status.set {
            it.copy(cameraSize = size, fps = fps,
                latencyMs = if (burst) it.latencyMs else out.totalMs, detectMs = if (burst) it.detectMs else out.detectMs, ocrMs = if (burst) it.ocrMs else out.ocrMs,
                framesAnalysed = it.framesAnalysed + (if (burst) 0 else 1), burstFrames = it.burstFrames + (if (burst) 1 else 0),
                platesSeen = it.platesSeen + out.plates.size, platesRead = it.platesRead + read,
                events = it.events + out.events.size, tracks = out.tracks, thermalLevel = step?.level ?: it.thermalLevel, shutter = shutter.describe)
        }
        val paired = link.pairing.value.let { it.paired && !it.revoked }
        val loc = lastLoc
        val place = loc?.let { "%.5f, %.5f".format(Locale.US, it.latitude, it.longitude) }.orEmpty()
        // alerts come as soon as a vehicle's fused read supports them, before its event
        for (a in out.alerts) {
            val m = a.match
            if (m.kind == MatchKind.EXACT) {
                alerts.hotlistHit(m, a.thumb, place)
                if (paired) uplink.submitHit(HitBody(m.entry.id, m.read, a.conf.toDouble(), AlprJson.rfc3339(a.atMs), loc?.latitude, loc?.longitude))
            } else if (settings.value.notifyPossible) alerts.possibleMatch(m, a.thumb)
        }
        val nowWall = System.currentTimeMillis()
        for (p in out.plates) {
            val r = p.read
            if (r != null && r.text.length >= 4 && r.conf >= 0.60f) {
                val kind = when {
                    p.matches.any { it.kind == MatchKind.EXACT } -> "hotlist"
                    p.matches.isNotEmpty() -> "possible"
                    else -> ""
                }
                val readText = p.fused.ifEmpty { r.text }
                status.addRead(RecentRead(readText, r.conf, nowWall, loc?.latitude, loc?.longitude, kind, null))
            }
        }
        for (ve in out.events) {
            val kind = when {
                ve.matches.any { it.kind == MatchKind.EXACT } -> "hotlist"
                ve.matches.isNotEmpty() -> "possible"
                else -> ""
            }
            status.addRead(RecentRead(ve.text.ifEmpty { "?" }, ve.conf, ve.event.createdMs, ve.event.meta.lat, ve.event.meta.lon, kind, ve.thumb))
            // not paired: reads stay on the phone and nothing is queued
            if (paired) uplink.submit(ve.event)
        }
    }

    companion object {
        private const val TAG = "AlprDashCam"
        const val ACTION_STOP = "org.sworrl.beaconfix.alpr.STOP"
        /** At most one burst pass this often (the analysis thread stays partly idle). */
        private const val BURST_GAP_MS = 120L

        @Volatile var uiActive: Boolean = false
            set(value) {
                field = value
                activeInstance?.reconcile()
            }
        @Volatile private var activeInstance: DashCamService? = null

        /** Start from a foreground context (an activity): Android 14+ refuses a camera service started from the background. */
        fun start(ctx: Context) = ContextCompat.startForegroundService(ctx, Intent(ctx, DashCamService::class.java))

        fun stop(ctx: Context) { ctx.stopService(Intent(ctx, DashCamService::class.java)) }
    }
}
