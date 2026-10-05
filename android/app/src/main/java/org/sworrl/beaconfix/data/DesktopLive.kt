package org.sworrl.beaconfix.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import okhttp3.Request
import okhttp3.sse.EventSource
import okhttp3.sse.EventSourceListener
import okhttp3.sse.EventSources
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.DevicesPositions
import org.sworrl.beaconfix.data.api.EmergencyDto
import org.sworrl.beaconfix.data.api.EventDto
import org.sworrl.beaconfix.data.api.Hello
import org.sworrl.beaconfix.data.api.LocationDto
import org.sworrl.beaconfix.data.api.LinkedDevice
import org.sworrl.beaconfix.data.api.PoiDto
import org.sworrl.beaconfix.data.api.Trip
import org.sworrl.beaconfix.data.api.TripDto
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.sync.Mirror
import javax.inject.Inject
import javax.inject.Singleton

/**
 * One paired desktop's live picture: fix, trip, places, events — polled on demand and streamed over SSE while a screen listens.
 * Until the desktop answers (or while it cannot), the parts it answered before come from the offline cache:
 * [cached] names them (location, trip, pois, emergency, devices), [cachedAt] is when they were saved, and [stale] is
 * true while anything shown is not from this run's latest answer. [emergency] is `/emergency` (null on desktops without it).
 */
data class DesktopView(val desktop: DesktopEntity, val location: LocationDto? = null, val trip: Trip? = null, val pois: List<PoiDto> = emptyList(),
                       val events: List<EventDto> = emptyList(), val error: String = "", val fetched: Long = 0, val streaming: Boolean = false, val devices: List<LinkedDevice> = emptyList(),
                       val emergency: EmergencyDto? = null, val cachedAt: Long = 0, val stale: Boolean = false, val cached: Set<String> = emptySet(),
                       val flockCameras: List<org.sworrl.beaconfix.data.api.FlockCameraDto> = emptyList(),
                       val alprSummary: org.sworrl.beaconfix.data.api.AlprSummaryDto? = null)

@Singleton
class DesktopLive @Inject constructor(
    private val store: DesktopStore, private val refits: org.sworrl.beaconfix.estimate.RefitBus,
    private val cache: DesktopCache, private val db: AppDatabase,
    private val plateEvents: dagger.Lazy<org.sworrl.beaconfix.sightings.PlateEventRepository>,
) {
    private val seenRefits = HashSet<Long>()
    private fun noteRefit(e: EventDto) { if (e.type == "ap_refit" && e.lat != null && e.lon != null && seenRefits.add(e.id)) refits.emit(org.sworrl.beaconfix.estimate.RefitEvent(e.bssid, e.ssid, e.lat, e.lon, e.fromLat, e.fromLon, e.acc ?: 50.0, e.prevAcc, e.n, e.vantage, e.rms, e.vantagePoints.map { org.sworrl.beaconfix.estimate.RefitEvent.Vantage(it.lat, it.lon, it.dbm, it.device) }, origin = "desktop")) }
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val _views = MutableStateFlow<Map<String, DesktopView>>(emptyMap())
    val views: StateFlow<Map<String, DesktopView>> = _views
    private var stream: EventSource? = null
    private var streamJob: Job? = null

    private fun put(id: String, f: (DesktopView) -> DesktopView) = _views.update { cur -> cur[id]?.let { cur + (id to f(it)) } ?: cur }
    /** A live answer for [part]: apply it and drop [part] from what the view shows from the cache. */
    private fun live(id: String, part: String, f: (DesktopView) -> DesktopView) = put(id) { f(it).let { v -> if (part in v.cached) v.copy(cached = v.cached - part) else v } }

    /**
     * Refresh what [what] names for every paired desktop — any of location, trip, pois, events, emergency, track,
     * devices, hello (devices also come with location). Every answer is mirrored into the offline cache
     * (`DesktopCache`: snapshots per kind, places per desktop), and a desktop's view starts from that cache the first
     * time it is built, so screens show the last known picture while the desktop is out of reach. A 404 (an older
     * desktop without that endpoint) or an unreadable answer skips that part only; `DevFlags` blocks the network.
     * Cameras: only the newest "flock" request's answer is applied (an older one finishing later is dropped), and a
     * cameras-only refresh leaves the view's error / fetched / stale alone (they describe the location and the rest).
     * Returns whether some desktop answered with cameras (false when "flock" is not asked for).
     */
    suspend fun refreshAll(what: Set<String> = DEFAULT, flockNear: Pair<Double, Double>? = null, flockKey: String = "map"): Boolean {
        val paired = store.paired()
        val fresh = HashMap<String, DesktopView>()
        for (d in paired) if (_views.value[d.id] == null) fresh[d.id] = prefill(d)
        _views.update { cur -> paired.associate { d -> d.id to ((cur[d.id] ?: fresh[d.id] ?: DesktopView(d)).copy(desktop = d)) } }
        val counter = flockGens.getOrPut(flockKey) { java.util.concurrent.atomic.AtomicLong() }
        val gen = if ("flock" in what) counter.incrementAndGet() else 0L
        var cams = false
        for (d in paired) if (refreshOne(d, what, flockNear, gen, counter)) cams = true
        return cams
    }
    /** per caller (the map, the car screen): bumped by each refresh that asks for cameras, and an answer is applied
     *  only while its number is still that caller's newest — one caller's requests never cancel another's */
    private val flockGens = java.util.concurrent.ConcurrentHashMap<String, java.util.concurrent.atomic.AtomicLong>()

    private suspend fun prefill(d: DesktopEntity): DesktopView = runCatching {
        val src = DesktopCache.desktopSource(d.id)
        val snaps = Mirror.PREFILLED.mapNotNull { k -> cache.snapshotFrom(src, k)?.let { k to it } }.toMap()
        Mirror.prefill(d, snaps, db.pois().allNow())
    }.getOrElse { DesktopView(d) }

    /** true when "flock" was asked for and [d]'s camera answer was applied. */
    private suspend fun refreshOne(d: DesktopEntity, what: Set<String>, flockNear: Pair<Double, Double>? = null, gen: Long = 0L,
                                   counter: java.util.concurrent.atomic.AtomicLong? = null): Boolean {
        // the view's freshness describes the location, trip and the rest: a cameras-only refresh never settles it
        val settles = what.any { it != "flock" }
        if (DevFlags.desktopBlocked()) { if (settles) put(d.id) { Mirror.settle(it, false, BLOCKED, System.currentTimeMillis()) }; return false }
        val api = store.api(d); val auth = store.auth(d) ?: return false
        val src = DesktopCache.desktopSource(d.id)
        var cams = false
        try {
            val done = withTimeoutOrNull(timeoutFor(what)) {
                // where the desktop is: positions its snapshots and its places' origin
                var fix: LocationDto? = _views.value[d.id]?.location?.takeIf { it.valid }
                suspend fun <T> snap(kind: String, body: T, s: kotlinx.serialization.KSerializer<T>, at: Pair<Double, Double>? = null) {
                    val (la, lo) = at ?: fix?.let { it.lat to it.lon } ?: (0.0 to 0.0)
                    runCatching { cache.putSnapshot(src, kind, ApiFactory.json.encodeToString(s, body), la, lo) }
                }
                if ("hello" in what) Mirror.bodyOf { api.hello() }?.let { h -> snap(Mirror.HELLO, h, Hello.serializer()) }
                if ("location" in what) Mirror.bodyOf { api.location(auth) }?.let { l ->
                    live(d.id, Mirror.LOCATION) { it.copy(location = l) }
                    if (l.valid) fix = l
                    snap(Mirror.LOCATION, l, LocationDto.serializer())
                }
                if ("trip" in what) Mirror.bodyOf { api.trip(auth) }?.let { t -> live(d.id, Mirror.TRIP) { it.copy(trip = t.trip) }; snap(Mirror.TRIP, t, TripDto.serializer()) }
                if ("pois" in what) Mirror.bodyOf { api.poisTyped(auth) }?.let { p ->
                    live(d.id, "pois") { it.copy(pois = p.pois) }
                    val o = p.origin?.takeIf { it.lat != 0.0 || it.lon != 0.0 }?.let { it.lat to it.lon } ?: fix?.let { it.lat to it.lon }
                    runCatching { cache.saveDesktopPois(d.id, p.pois, o?.first ?: 0.0, o?.second ?: 0.0) }
                }
                if ("events" in what) Mirror.bodyOf { api.events(auth, 0) }?.events?.let { e -> e.takeLast(5).forEach { ev -> noteRefit(ev) }; put(d.id) { it.copy(events = e.takeLast(60)) } }
                if ("emergency" in what) Mirror.bodyOf { api.emergency(auth) }?.let { e ->
                    live(d.id, Mirror.EMERGENCY) { it.copy(emergency = e) }
                    snap(Mirror.EMERGENCY, e, EmergencyDto.serializer(), Mirror.emergencyAt(e, fix))
                }
                if ("track" in what) Mirror.rawGet(d, auth, "api/v1/track").takeIf { it.ok }?.let { r ->
                    val (la, lo) = fix?.let { it.lat to it.lon } ?: (0.0 to 0.0)
                    runCatching { cache.putSnapshot(src, Mirror.TRACK, r.body, la, lo) }
                }
                if ("location" in what || "devices" in what) Mirror.bodyOf { api.devicesPositions(auth) }?.let { dv ->
                    live(d.id, Mirror.DEVICES) { it.copy(devices = dv.devices) }
                    snap(Mirror.DEVICES, dv, DevicesPositions.serializer())
                }
                if ("flock" in what) {
                    // an area, never the whole nationwide set (~137k cameras, ~30 MB): around [flockNear], else the desktop's own fix
                    Mirror.bodyOf { api.flockCameras(auth, flockNear?.first, flockNear?.second, flockNear?.let { FLOCK_KM }, FLOCK_LIMIT) }?.let { fc ->
                        if (gen == (counter?.get() ?: gen)) { live(d.id, "flock") { it.copy(flockCameras = fc.cameras) }; cams = true }
                    }
                    if (gen == (counter?.get() ?: gen)) Mirror.bodyOf { api.flockSummary(auth) }?.let { sum ->
                        live(d.id, "alprSummary") { it.copy(alprSummary = sum) }
                    }
                    // plate events (docs/SIGHTINGS.md §5): the desktop's feed into Room, alerted per §6 (at most once a minute)
                    plateEvents.get().quickSync(d)
                }
                true
            }
            if (settles) put(d.id) { Mirror.settle(it, done == true, if (done == true) "" else "timed out", System.currentTimeMillis()) }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            val code = (e as? retrofit2.HttpException)?.code()
            if (code == 401) store.forgetToken(d.id)
            if (settles) put(d.id) { Mirror.settle(it, false, e.message ?: "unreachable", System.currentTimeMillis()) }
        }
        return cams
    }

    suspend fun fetchRouteHeatmap(): List<org.sworrl.beaconfix.data.api.RoutePointDto> { 
        val d = store.paired().firstOrNull() ?: return emptyList() 
        val auth = store.auth(d) ?: return emptyList() 
        val api = org.sworrl.beaconfix.data.api.ApiFactory.create(d.host, d.port, d.tls) 
        return runCatching { api.routeHeatmap(auth).body()?.points ?: emptyList() }.getOrDefault(emptyList()) 
    }

    suspend fun syncNationwideUs(): Boolean {
        val d = store.paired().firstOrNull() ?: return false
        val auth = store.auth(d) ?: return false
        val api = ApiFactory.create(d.host, d.port, d.tls)
        val res = runCatching { api.syncNationwideUs(auth) }.getOrNull()
        return res?.isSuccessful == true
    }

    /** Server-sent events from the first paired desktop while a screen is visible: `fix` and `beacon` events. */
    fun startStream() {
        if (stream != null || DevFlags.desktopBlocked()) return
        streamJob = scope.launch {
            val d = store.paired().firstOrNull() ?: return@launch
            val auth = store.auth(d) ?: return@launch
            val req = Request.Builder().url(ApiFactory.baseUrl(d.host, d.port, d.tls) + "api/v1/stream").header("Authorization", auth).header("Accept", "text/event-stream").build()
            val client = ApiFactory.client.newBuilder().readTimeout(0, java.util.concurrent.TimeUnit.MILLISECONDS).build()
            stream = EventSources.createFactory(client).newEventSource(req, object : EventSourceListener() {
                override fun onOpen(eventSource: EventSource, response: okhttp3.Response) { put(d.id) { it.copy(streaming = true) } }
                override fun onEvent(eventSource: EventSource, id: String?, type: String?, data: String) {
                    when (type) {
                        "beacon" -> runCatching { ApiFactory.json.decodeFromString(EventDto.serializer(), data) }.getOrNull()?.let { e -> noteRefit(e); put(d.id) { v -> if (v.events.any { it.id == e.id }) v else v.copy(events = (v.events + e).takeLast(80)) } }
                        "fix" -> runCatching { ApiFactory.json.decodeFromString(LocationDto.serializer(), data) }.getOrNull()?.let { l -> put(d.id) { it.copy(location = l) } }
                    }
                }
                override fun onClosed(eventSource: EventSource) { put(d.id) { it.copy(streaming = false) }; stream = null }
                override fun onFailure(eventSource: EventSource, t: Throwable?, response: okhttp3.Response?) { put(d.id) { it.copy(streaming = false) }; stream = null }
            })
        }
    }
    fun stopStream() { stream?.cancel(); stream = null; streamJob?.cancel(); streamJob = null; _views.value = _views.value.mapValues { it.value.copy(streaming = false) } }

    companion object {
        /** What a screen refreshes by default. */
        val DEFAULT = setOf("location", "trip", "pois", "events", "emergency", "flock")
        /** The camera area asked for around a position (km), and the most cameras one answer may carry. */
        const val FLOCK_KM = 50.0
        const val FLOCK_LIMIT = 2000
        const val BLOCKED = "offline (simulated)"
        /** 12 s for the usual handful of small GETs, a little more when more is asked for (Starlink can be slow). */
        fun timeoutFor(what: Set<String>): Long = (12_000L + 2_000L * (what.size - 5).coerceAtLeast(0)).coerceAtMost(20_000L)
    }
}
