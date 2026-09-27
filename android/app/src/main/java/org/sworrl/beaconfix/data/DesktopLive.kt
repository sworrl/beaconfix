package org.sworrl.beaconfix.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import okhttp3.Request
import okhttp3.sse.EventSource
import okhttp3.sse.EventSourceListener
import okhttp3.sse.EventSources
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.EventDto
import org.sworrl.beaconfix.data.api.LocationDto
import org.sworrl.beaconfix.data.api.LinkedDevice
import org.sworrl.beaconfix.data.api.PoiDto
import org.sworrl.beaconfix.data.api.Trip
import org.sworrl.beaconfix.data.db.DesktopEntity
import javax.inject.Inject
import javax.inject.Singleton

/** One paired desktop's live picture: fix, trip, places, events — polled on demand and streamed over SSE while a screen listens. */
data class DesktopView(val desktop: DesktopEntity, val location: LocationDto? = null, val trip: Trip? = null, val pois: List<PoiDto> = emptyList(),
                       val events: List<EventDto> = emptyList(), val error: String = "", val fetched: Long = 0, val streaming: Boolean = false, val devices: List<LinkedDevice> = emptyList())

@Singleton
class DesktopLive @Inject constructor(private val store: DesktopStore, private val refits: org.sworrl.beaconfix.estimate.RefitBus) {
    private val seenRefits = HashSet<Long>()
    private fun noteRefit(e: EventDto) { if (e.type == "ap_refit" && e.lat != null && e.lon != null && seenRefits.add(e.id)) refits.emit(org.sworrl.beaconfix.estimate.RefitEvent(e.bssid, e.ssid, e.lat, e.lon, e.fromLat, e.fromLon, e.acc ?: 50.0, e.prevAcc, e.n, e.vantage, e.rms, e.vantagePoints.map { org.sworrl.beaconfix.estimate.RefitEvent.Vantage(it.lat, it.lon, it.dbm, it.device) }, origin = "desktop")) }
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val _views = MutableStateFlow<Map<String, DesktopView>>(emptyMap())
    val views: StateFlow<Map<String, DesktopView>> = _views
    private var stream: EventSource? = null
    private var streamJob: Job? = null

    private fun put(id: String, f: (DesktopView) -> DesktopView) { val cur = _views.value; cur[id]?.let { _views.value = cur + (id to f(it)) } }

    /** Refresh everything for every paired desktop (cheap: four small GETs each). */
    suspend fun refreshAll(what: Set<String> = setOf("location", "trip", "pois", "events")) {
        val paired = store.paired()
        val keep = _views.value.filterKeys { k -> paired.any { it.id == k } }.toMutableMap()
        for (d in paired) {
            val v = keep[d.id] ?: DesktopView(d)
            keep[d.id] = v.copy(desktop = d)
            _views.value = keep
            val api = store.api(d); val auth = store.auth(d) ?: continue
            try {
                withTimeoutOrNull(12_000) {
                    if ("location" in what) api.location(auth).body()?.let { l -> put(d.id) { it.copy(location = l) } }
                    if ("trip" in what) api.trip(auth).body()?.trip?.let { t -> put(d.id) { it.copy(trip = t) } }
                    if ("pois" in what) api.poisTyped(auth).body()?.pois?.let { p -> put(d.id) { it.copy(pois = p) } }
                    if ("events" in what) api.events(auth, 0).body()?.events?.let { e -> e.takeLast(5).forEach { ev -> noteRefit(ev) }; put(d.id) { it.copy(events = e.takeLast(60)) } }
                    if ("location" in what) runCatching { api.devicesPositions(auth) }.getOrNull()?.takeIf { it.isSuccessful }?.body()?.devices?.let { dv -> put(d.id) { it.copy(devices = dv) } }
                }
                put(d.id) { it.copy(error = "", fetched = System.currentTimeMillis()) }
            } catch (e: Exception) {
                val code = (e as? retrofit2.HttpException)?.code()
                if (code == 401) store.forgetToken(d.id)
                put(d.id) { it.copy(error = e.message ?: "unreachable") }
            }
        }
    }

    /** Server-sent events from the first paired desktop while a screen is visible: `fix` and `beacon` events. */
    fun startStream() {
        if (stream != null) return
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
}
