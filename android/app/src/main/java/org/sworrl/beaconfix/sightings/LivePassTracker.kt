// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.location.Location
import android.os.Build
import android.util.Log
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonPrimitive
import okhttp3.Request
import org.sworrl.beaconfix.data.DesktopLive
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.DevFlags
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.api.ApiFactory
import java.io.ByteArrayOutputStream
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.abs

/**
 * The dash cam's last ~3 s of analysed frames (docs/SIGHTINGS.md §3.3), kept only while a camera is near ([armed]),
 * plus the frames pinned for a pass in progress: the one nearest the closest approach and the one nearest the moment
 * the camera saw the front plate. Pinning happens as the fixes arrive, so the frames are still in the 3 s ring.
 * Memory only; nothing is written until a pass completes.
 */
@Singleton
class DashFrameBuffer @Inject constructor() {
    class Frame(val timeMs: Long, val bitmap: Bitmap)

    /** The dash cam is capturing (DashCamService sets it). */
    @Volatile var running = false
        set(v) { field = v; if (!v) clear() }
    /** A camera is close: frames are worth keeping. */
    @Volatile var armed = false
    private val ring = ArrayDeque<Frame>()
    private val pinned = HashMap<String, Frame>()
    @Volatile private var lastOffer = 0L

    /** Whether the analysis thread should hand over this frame (armed, and ≥ 250 ms since the last one). */
    fun wants(now: Long): Boolean = running && armed && now - lastOffer >= MIN_SPACING_MS

    fun offer(timeMs: Long, make: () -> Bitmap) {
        lastOffer = timeMs
        val f = Frame(timeMs, make())
        synchronized(this) {
            ring.addLast(f)
            while (ring.isNotEmpty() && (timeMs - ring.first().timeMs > KEEP_MS || ring.size > MAX_FRAMES)) ring.removeFirst()
        }
    }

    /** Keep, under [key], the ring frame nearest [targetMs] unless the frame already kept is nearer. */
    fun pin(key: String, targetMs: Long) = synchronized(this) {
        val cand = ring.minByOrNull { abs(it.timeMs - targetMs) } ?: return@synchronized
        val cur = pinned[key]
        if (cur == null || abs(cand.timeMs - targetMs) < abs(cur.timeMs - targetMs)) pinned[key] = cand
    }

    /** The frames pinned for [cameraId] (role → frame), removed from the buffer. */
    fun take(cameraId: String): Map<String, Frame> = synchronized(this) {
        val keys = pinned.keys.filter { it.startsWith("$cameraId|") }
        keys.associate { it.substringAfterLast('|') to pinned.remove(it)!! }
    }

    fun dropOlderThan(ms: Long) = synchronized(this) { pinned.entries.removeAll { it.value.timeMs < ms } }

    fun clear() = synchronized(this) { ring.clear(); pinned.clear() }

    companion object {
        const val KEEP_MS = 3_000L
        const val MAX_FRAMES = 12
        const val MIN_SPACING_MS = 250L
        /** The buffered frame's long side (px): the analysed frame, sampled down to this when larger. */
        const val MAX_SIDE = 1280
    }
}

/**
 * Phone-live pass detection (docs/SIGHTINGS.md §2.1–2.4): this phone's fixes (the collector every few seconds, the dash
 * cam every second) go through [LiveTracker] — the desktop's live algorithm — against the cameras the desktop serves
 * around us. A finished pass is stored as `phone_live`, or as `dashcam` with frames when the dash cam was running;
 * ALPR passes alert (§6), every other camera type is recorded silently. With the opt-in setting on, a `webcam` camera's
 * public still is fetched as we pass and kept on the phone only.
 */
@Singleton
class LivePassTracker @Inject constructor(
    @ApplicationContext private val ctx: Context,
    private val desktopLive: DesktopLive,
    private val desktops: DesktopStore,
    private val repo: PlateEventRepository,
    private val frames: DashFrameBuffer,
    private val prefs: Prefs,
    private val inspectionManager: InspectionManager,
    private val espNodeManager: org.sworrl.beaconfix.node.EspNodeManager,
    private val cameraCache: FlockCameraCache,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val lock = Mutex()
    private val tracker = LiveTracker()
    private var fetched: List<PassCamera> = emptyList()
    private var cachedCameras: List<PassCamera> = emptyList()
    private var fetchedAt = 0L; private var fetchedLat = 0.0; private var fetchedLon = 0.0
    @Volatile private var fetching = false
    private val stillFetched = HashSet<String>()
    private val webcamStill = HashMap<String, LocalImage>()
    private var prev: RouteFix? = null
    private var lastDrivingSaved = 0L

    init {
        scope.launch {
            cachedCameras = cameraCache.getPassCameras()
        }
        scope.launch {
            desktopLive.views.collect { views ->
                val fc = views.values.flatMap { it.flockCameras }.filter { !it.stale }
                if (fc.isNotEmpty()) {
                    cameraCache.updateCameras(fc)
                    cachedCameras = cameraCache.getPassCameras()
                }
            }
        }
    }

    /** A fix from the collector or the dash cam. Cheap: the work runs on a background dispatcher. */
    fun onLocation(loc: Location) {
        if (loc.latitude == 0.0 && loc.longitude == 0.0) return
        if (loc.hasAccuracy() && loc.accuracy > MAX_ACC_M) return
        val fix = RouteFix(loc.time.takeIf { it > 0 } ?: System.currentTimeMillis(), loc.latitude, loc.longitude, if (loc.hasAccuracy()) loc.accuracy.toDouble() else null,
            if (loc.hasSpeed()) loc.speed.toDouble() else null, if (loc.hasBearing()) loc.bearing.toDouble() else null, repo.deviceName, "gps")
        scope.launch { runCatching { lock.withLock { handle(fix) } }.onFailure { Log.w(TAG, "pass", it) } }
    }

    private suspend fun handle(fix: RouteFix) {
        if (prefs.privateInspectionActive.first()) {
            inspectionManager.onLocation(fix.lat, fix.lon)
            return
        }
        val p0 = prev
        if (p0 != null && fix.timeMs <= p0.timeMs) return
        prev = fix
        noteDriving(p0, fix)

        // Stream high-precision GPS and travel/motion kinematics to connected Heltec / ESP32 node
        espNodeManager.sendGpsFix(
            lat = fix.lat,
            lon = fix.lon,
            accM = (fix.acc ?: 5.0).toFloat(),
            speedMps = (fix.speedMps ?: -1.0).toFloat(),
            headingDeg = (fix.bearingDeg ?: -1.0).toFloat(),
            altM = 0.0f
        )

        ensureCameras(fix)
        // the desktop's neighbourhood: ±0.004° lat, ±0.005° lon
        val nearby = cameras().filter { abs(it.lat - fix.lat) <= 0.004 && abs(it.lon - fix.lon) <= 0.005 }
        val done = tracker.addFix(fix, nearby) + tracker.tick(fix.timeMs)
        val active = tracker.activeCameras()
        frames.armed = frames.running && nearby.any { PassDetector.distanceM(it.lat, it.lon, fix.lat, fix.lon) <= ARM_M }
        frames.dropOlderThan(fix.timeMs - 15 * 60_000L)
        for (a in active) {
            // the frames of a pass in progress: nearest the closest approach so far, and the latest moment in the camera's cone before it
            if (frames.running) {
                frames.pin("${a.cam.id}|closest", a.bestMs)
                val dirs = PassDetector.parseDirections(PassDetector.directionText(a.cam))
                if (dirs.isNotEmpty() && fix.timeMs <= a.bestMs + 500 && PassDetector.inCone(a.cam, PassDetector.coneFor(a.cam), dirs, fix.lat, fix.lon)) frames.pin("${a.cam.id}|front", fix.timeMs)
            }
            if (a.cam.type == "webcam" && a.cam.webcam != null && stillFetched.add(a.cam.id) && prefs.webcamStills.first()) {
                val cam = a.cam
                scope.launch { fetchWebcam(cam)?.let { img -> lock.withLock { webcamStill[cam.id] = img } } }
            }
            // Send approaching proximity alert to node when within 150m of ALPR camera
            for (c in nearby) {
                if (c.type == "alpr") {
                    val dist = PassDetector.distanceM(c.lat, c.lon, fix.lat, fix.lon)
                    if (dist <= 150.0) {
                        val op = c.operator.ifBlank { c.manufacturer.ifBlank { "Flock Safety" } }
                        val mdl = c.model.ifBlank { if (op.contains("Flock", true)) "Falcon" else "ALPR" }
                        espNodeManager.sendAlprProximity(op, mdl, dist.toFloat())
                        break
                    }
                }
            }
        }
        for (p in done) { stillFetched.remove(p.camera.id); emit(p) }
    }

    private suspend fun emit(p: Pass) {
        val pinned = frames.take(p.camera.id)
        val images = ArrayList<LocalImage>()
        withContext(Dispatchers.Default) {
            pinned["closest"]?.let { f -> encode(f, "closest approach")?.let { images += it } }
            pinned["front"]?.takeIf { it.timeMs != pinned["closest"]?.timeMs }?.let { f -> encode(f, "front plate")?.let { images += it } }
        }
        webcamStill.remove(p.camera.id)?.let { images += it }
        val dashcam = frames.running || images.any { it.kind == "dashcam" }
        val leaky = Hibf.leakyMatch(p.camera.operator, PlateEvents.cameraState(p.camera), repo.leakyAgencies())
        val extra = buildMap {
            put("liveSource", JsonPrimitive(if (dashcam) "dashcam" else "collector"))
            if (images.isNotEmpty()) put("frames", JsonPrimitive(images.count { it.kind == "dashcam" }))
        }
        val row = PlateEvents.fromPass(p, if (dashcam) PlateEvents.SRC_DASHCAM else PlateEvents.SRC_PHONE_LIVE, repo.activePlate().orEmpty(),
            plateInferred = true, device = repo.deviceName, leaky = leaky != null, leakyMatch = leaky, now = System.currentTimeMillis(), extraMetrics = extra)
        repo.recordLocal(row, images)
        if (p.cameraType == "alpr") {
            HibfWatcher.nudge(ctx) // a Flock / leaky pass shortens the watch interval

            // Send ALPR alert to Heltec node -> triggers OLED splash screen & alert LED
            val op = p.camera.operator.ifBlank { p.camera.manufacturer.ifBlank { "Flock Safety" } }
            val mdl = p.camera.model.ifBlank { if (op.contains("Flock", true)) "Falcon" else "ALPR" }
            val dist = p.distanceM.toFloat()
            val conf = p.confidence.coerceIn(1, 100)
            val facingInt = if (p.facing == true) 1 else if (p.facing == false) -1 else 0
            val spd = (p.speedKmh ?: ((prev?.speedMps ?: 0.0) * 3.6)).toFloat()
            espNodeManager.sendAlprAlert(
                operator = op,
                model = mdl,
                distanceM = dist,
                confidence = conf,
                facing = facingInt,
                speedKmh = spd,
                lat = p.camera.lat,
                lon = p.camera.lon
            )
        }
    }

    /** §3.3: WebP lossless at the analysed resolution (Android 11+); PNG — also lossless — on older phones. */
    private fun encode(f: DashFrameBuffer.Frame, note: String): LocalImage? = runCatching {
        val out = ByteArrayOutputStream(f.bitmap.width * f.bitmap.height)
        val mime = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            f.bitmap.compress(Bitmap.CompressFormat.WEBP_LOSSLESS, 100, out); "image/webp"
        } else { f.bitmap.compress(Bitmap.CompressFormat.PNG, 100, out); "image/png" }
        LocalImage(out.toByteArray(), mime, f.bitmap.width, f.bitmap.height, f.timeMs, "dashcam", note)
    }.getOrNull()

    /**
     * A public webcam's current still (§2.0, opt-in): accepted only when the URL answers with an image. Feeds that are
     * only an HLS stream (WV511) have no still the phone can take; the desktop grabs a frame with ffmpeg instead.
     */
    private suspend fun fetchWebcam(cam: PassCamera): LocalImage? = withContext(Dispatchers.IO) {
        if (DevFlags.simOffline) return@withContext null
        val url = cam.webcam ?: return@withContext null
        runCatching {
            val client = ApiFactory.client.newBuilder().callTimeout(10, TimeUnit.SECONDS).build()
            client.newCall(Request.Builder().url(url).header("User-Agent", "BeaconFix-Android/${org.sworrl.beaconfix.BuildConfig.VERSION_NAME}").build()).execute().use { r ->
                val type = r.header("Content-Type").orEmpty().substringBefore(';').trim().lowercase()
                if (!r.isSuccessful || !type.startsWith("image/")) return@use null
                val bytes = r.body?.bytes() ?: return@use null
                if (bytes.size > 15 * 1024 * 1024) return@use null
                val o = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeByteArray(bytes, 0, bytes.size, o)
                LocalImage(bytes, type, o.outWidth, o.outHeight, System.currentTimeMillis(), "webcam", "Public webcam · ${cam.operator.ifEmpty { url }} (kept on this phone)")
            }
        }.getOrNull()
    }

    private suspend fun noteDriving(prev: RouteFix?, fix: RouteFix) {
        val v = fix.speedMps ?: prev?.let { p ->
            val dt = (fix.timeMs - p.timeMs) / 1000.0
            if (dt in 1.0..120.0) PassDetector.distanceM(p.lat, p.lon, fix.lat, fix.lon) / dt else null
        } ?: return
        if (v * 3.6 > DRIVING_KMH && fix.timeMs - lastDrivingSaved > 60_000L) { lastDrivingSaved = fix.timeMs; prefs.setLastDrivingAt(fix.timeMs) }
    }

    /** The desktop's cameras around us (refreshed every 5 km / 30 min) plus whatever the open screens loaded and persistent cache. */
    private fun cameras(): List<PassCamera> {
        // a stale camera (its source no longer confirms it) keeps its past passes but gets no new ones
        val shown = desktopLive.views.value.values.flatMap { it.flockCameras }.filter { !it.stale }.map { PlateEvents.camera(it) }
        return (fetched + shown + cachedCameras).distinctBy { it.id }
    }

    private fun ensureCameras(fix: RouteFix) {
        val stale = fetchedAt == 0L || fix.timeMs - fetchedAt > 30 * 60_000L || PassDetector.distanceM(fetchedLat, fetchedLon, fix.lat, fix.lon) > 5_000
        if (!stale || fetching || DevFlags.desktopBlocked()) return
        fetching = true
        scope.launch {
            var ok = false
            try {
                for (d in desktops.paired()) {
                    val auth = desktops.auth(d) ?: continue
                    val r = runCatching { desktops.api(d).flockCameras(auth, fix.lat, fix.lon, FETCH_KM, 3000) }.getOrNull() ?: continue
                    val list = r.body()?.cameras ?: continue
                    lock.withLock { fetched = list.filter { !it.stale }.map { PlateEvents.camera(it) }; fetchedAt = fix.timeMs; fetchedLat = fix.lat; fetchedLon = fix.lon }
                    cameraCache.updateCameras(list)
                    cachedCameras = cameraCache.getPassCameras()
                    ok = true
                    break
                }
            } finally {
                // no desktop answered: keep the cameras we have and ask again in ~5 min
                if (!ok) lock.withLock { fetchedAt = fix.timeMs - 25 * 60_000L; fetchedLat = fix.lat; fetchedLon = fix.lon }
                fetching = false
            }
        }
    }

    companion object {
        private const val TAG = "LivePass"
        const val ARM_M = 200.0
        const val MAX_ACC_M = 500f
        const val FETCH_KM = 15.0
        const val DRIVING_KMH = 20.0
    }
}
