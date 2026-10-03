package org.sworrl.beaconfix.net

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.sse.EventSource
import org.sworrl.beaconfix.data.api.DevicePositionBody
import org.sworrl.beaconfix.data.api.LinkedDevice
import org.sworrl.beaconfix.estimate.Geo
import java.util.concurrent.atomic.AtomicBoolean
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Every node's live position from the hub (`GET /devices/positions` over BFS3) while a screen shows them: polled
 * every 15 s, and at once when the hub's sealed event stream says a device moved or came online (the poll slows to
 * once a minute while the stream is up). This phone's own entry is left out — the map draws it itself.
 */
@Singleton
class HubLive @Inject constructor(private val hub: HubClient, private val store: HubStore) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val _devices = MutableStateFlow<List<LinkedDevice>>(emptyList())
    val devices: StateFlow<List<LinkedDevice>> = _devices
    private val _streaming = MutableStateFlow(false)
    val streaming: StateFlow<Boolean> = _streaming
    private val kick = Channel<Unit>(Channel.CONFLATED)
    private var users = 0
    private var job: Job? = null
    @Volatile private var stream: EventSource? = null

    @Synchronized fun start() {
        users++
        if (job == null) job = scope.launch { loop() }
    }
    @Synchronized fun stop() {
        users = (users - 1).coerceAtLeast(0)
        if (users == 0) { job?.cancel(); job = null; stream?.cancel(); stream = null; _streaming.value = false }
    }

    private suspend fun loop() {
        var streamRetryAt = 0L
        while (currentCoroutineContext().isActive) {
            if (!hub.enrolled()) { _devices.value = emptyList(); delay(POLL_MS); continue }
            refresh()
            if (stream == null && System.currentTimeMillis() >= streamRetryAt) {
                streamRetryAt = System.currentTimeMillis() + 60_000
                stream = runCatching { hub.openStream(onEvent = { e -> if (wakes(e["type"]?.jsonPrimitive?.contentOrNull)) kick.trySend(Unit) },
                    onOpen = { _streaming.value = true }, onClosed = { _streaming.value = false; stream = null }) }.getOrNull()
            }
            withTimeoutOrNull(if (_streaming.value) STREAM_POLL_MS else POLL_MS) { kick.receive(); delay(1500) }    // a burst of events → one refresh
        }
    }

    /** Fetch the positions now; failures are noted on the hub status (and keep the last list). */
    suspend fun refresh() {
        val api = hub.api() ?: return
        try {
            val r = api.devicesPositions(HubClient.AUTH)
            if (r.isSuccessful) {
                val me = store.config()?.name
                _devices.value = (r.body()?.devices ?: emptyList()).filter { it.device != me }
                store.noteContact()
            } else store.noteError(HubErrors.ofStatus(r.code(), hub.clockSkewS), false)
        } catch (e: CancellationException) { throw e } catch (e: Exception) {
            store.noteError(HubErrors.describe(e), HubErrors.isUnreachable(e))
        }
    }

    companion object {
        const val POLL_MS = 15_000L
        const val STREAM_POLL_MS = 60_000L
        /** Stream events that move or (un)list a device on the map. */
        fun wakes(type: String?) = type == null || type == "fix" || type.startsWith("device")
        /**
         * One list for the map from the LAN desktops' answers and the hub's: one entry per device name, the freshest
         * (smallest age) winning; entries without an age count as old.
         */
        fun merge(lan: List<LinkedDevice>, hub: List<LinkedDevice>): List<LinkedDevice> =
            (lan + hub).groupBy { it.device }.map { (_, l) -> l.minBy { it.ageS ?: Double.MAX_VALUE } }
    }
}

/**
 * Publishes this phone's live position to the hub (`POST /devices/position` over BFS3), fed with every fix the
 * collector sees: while moving at most every [Cadence.MOVING_MS] (5 s), while stationary every
 * [Cadence.STATIONARY_MS] (3 min); after a failure (hub unreachable) not again for [Cadence.BACKOFF_MS]. A position
 * that cannot be sent is dropped — it is live data; the fixes themselves reach the hub through the sync.
 */
@Singleton
class HubPresence @Inject constructor(private val hub: HubClient, private val store: HubStore, private val node: HubNode) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val busy = AtomicBoolean(false)
    @Volatile private var lastAt = 0L
    @Volatile private var lastLat = 0.0
    @Volatile private var lastLon = 0.0
    @Volatile private var failedAt = 0L

    /** A fix: [source] "gps" / "wifi"; [speedMps] when the provider knows it. Cheap; safe from any thread. */
    fun offer(lat: Double, lon: Double, acc: Double, timeMs: Long, source: String, speedMps: Float? = null, beacons: Int = 0) {
        if (store.cached == null || (lat == 0.0 && lon == 0.0) || !(acc in 0.0..5000.0)) return
        node.maybeHeartbeat()                    // the node registry's 10-minute heartbeat rides on the fix feed
        val now = System.currentTimeMillis()
        val moved = if (lastAt == 0L) Double.MAX_VALUE else Geo.distanceM(lastLat, lastLon, lat, lon)
        if (!Cadence.due(now, lastAt, failedAt, Cadence.moving(speedMps, moved, acc))) return
        if (!busy.compareAndSet(false, true)) return
        scope.launch {
            try {
                val api = hub.api() ?: return@launch
                val r = api.devicePosition(HubClient.AUTH, DevicePositionBody(lat, lon, acc, org.sworrl.beaconfix.sync.SyncRepository.iso(timeMs), beacons, source))
                if (r.isSuccessful) { lastAt = now; lastLat = lat; lastLon = lon; failedAt = 0; store.notePublish() }
                else { failedAt = now; store.noteError(HubErrors.ofStatus(r.code(), hub.clockSkewS), false) }
            } catch (e: CancellationException) { throw e } catch (e: Exception) {
                failedAt = now; store.noteError(HubErrors.describe(e), HubErrors.isUnreachable(e))
            } finally { busy.set(false) }
        }
    }

    object Cadence {
        const val MOVING_MS = 5_000L
        const val STATIONARY_MS = 180_000L
        const val BACKOFF_MS = 60_000L
        /** Moving: the provider says > 1 m/s, or the fix is further from the last published one than max(15 m, its accuracy). */
        fun moving(speedMps: Float?, movedM: Double, accM: Double): Boolean = (speedMps != null && speedMps > 1f) || movedM > maxOf(15.0, accM)
        fun due(nowMs: Long, lastAtMs: Long, failedAtMs: Long, moving: Boolean): Boolean {
            if (failedAtMs > 0 && nowMs - failedAtMs < BACKOFF_MS) return false
            if (lastAtMs == 0L) return true
            return nowMs - lastAtMs >= if (moving) MOVING_MS else STATIONARY_MS
        }
    }
}
