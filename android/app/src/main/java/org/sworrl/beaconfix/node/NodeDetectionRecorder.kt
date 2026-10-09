package org.sworrl.beaconfix.node

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONObject
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.NodeDetectionEntity

/**
 * Keeps what a BLE-linked node detects (probes, beacons, deauth alerts, BLE trackers) in `node_detections`, so it
 * outlives the session and goes to the hub with the next sync (HubSync). Lines the node stored on flash while this
 * phone was away carry `"sf":<seq>`; once they're saved the node gets `sf ack <seq>` and drops them.
 *
 * Live lines are thinned the way the node thins them while storing (one beacon per AP a minute, one probe per client
 * and network every 30 s) so a day of following doesn't fill the phone.
 */
class NodeDetectionRecorder(
    private val db: AppDatabase,
    scope: CoroutineScope,
    private val sendCommand: (String) -> Unit,
) {
    data class Stats(val savedThisSession: Int = 0, val fromFlash: Int = 0, val lastNode: String = "", val lastAtMs: Long = 0)

    private val _stats = MutableStateFlow(Stats())
    val stats: StateFlow<Stats> = _stats.asStateFlow()

    private val lines = Channel<Pair<String, String>>(8192, BufferOverflow.DROP_OLDEST)
    private val lastLive = HashMap<String, Long>()

    init {
        scope.launch {
            while (true) {
                val batch = mutableListOf(lines.receive())
                withTimeoutOrNull(1000) { while (batch.size < 300) batch += lines.receive() }
                runCatching { save(batch) }
            }
        }
    }

    /** One JSON line from the node [node] holds the BLE link to */
    fun offer(line: String, node: String) {
        if (line.contains("\"sf\":") || line.contains("\"type\":\"probe\"") || line.contains("\"type\":\"beacon\"") ||
            line.contains("\"type\":\"alert\"") || line.contains("\"type\":\"ble_tracker\"") ||
            line.contains("\"type\":\"sf_end\"") || line.contains("\"type\":\"mesh_telemetry\"")) {
            lines.trySend(line to node)
        }
    }

    private suspend fun save(batch: List<Pair<String, String>>) {
        val rows = mutableListOf<NodeDetectionEntity>()
        val seqs = mutableListOf<Long>()
        var end: JSONObject? = null
        var fromFlash = 0
        var lastNode = ""
        val now = System.currentTimeMillis()
        for ((line, linkNode) in batch) {
            val o = runCatching { JSONObject(line) }.getOrNull() ?: continue
            when (o.optString("type")) {
                "sf_end" -> { end = o; continue }
                "mesh_telemetry" -> {
                    val inner = o.optJSONObject("data") ?: continue
                    val origin = o.optString("origin").takeIf { it.isNotBlank() && it != "Unknown" } ?: continue
                    toRow(inner, origin, now)?.let { rows += it; lastNode = origin }
                    continue
                }
            }
            val seq = o.optLong("sf", 0)
            if (seq > 0) { seqs += seq; fromFlash++ }
            val node = o.optString("node").ifBlank { linkNode }.ifBlank { "node" }
            toRow(o, node, now)?.let { rows += it; lastNode = node }
        }
        val located = rows.map { r ->
            when {
                r.lat != null -> r
                // Stored on the node's flash = made while the phone was out of reach, so not where the phone was:
                // the node stays where it was last seen with a fix
                r.stored -> db.nodeDetections().lastPositioned(r.node, r.timeMs, 6 * 3600_000L)
                    ?.let { p -> r.copy(lat = p.lat, lon = p.lon, acc = p.acc, locSource = "node_last") } ?: r
                // Live over BLE = within range of this phone
                else -> db.fixes().phoneNear(r.timeMs, 120_000)
                    ?.let { f -> r.copy(lat = f.lat, lon = f.lon, acc = f.acc, locSource = "phone") } ?: r
            }
        }
        val saved = if (located.isEmpty()) 0 else db.nodeDetections().insertAll(located).count { it > 0 }
        // Only once they're on disk (a line that's already here counts): the node deletes what's acknowledged
        received += seqs
        end?.let { finishDump(it) }
        if (saved > 0 || fromFlash > 0) {
            _stats.value = _stats.value.let {
                it.copy(savedThisSession = it.savedThisSession + saved, fromFlash = it.fromFlash + fromFlash,
                        lastNode = lastNode.ifBlank { it.lastNode }, lastAtMs = now)
            }
        }
    }

    private val received = HashSet<Long>()
    private var resendAsks = 0

    // The node sent first..last, consecutive. Acknowledge the unbroken run from first; if lines went missing on the
    // way, ask for the rest again (a few times, then leave it on the node for the next link).
    private fun finishDump(end: JSONObject) {
        val first = end.optLong("first")
        val last = end.optLong("last")
        if (first <= 0 || last < first) { received.clear(); return }
        var upTo = first - 1
        while (upTo < last && (upTo + 1) in received) upTo++
        if (upTo >= first) sendCommand("sf ack $upTo")
        if (upTo < last && resendAsks < 3) {
            resendAsks++
            sendCommand("sf dump")
        } else {
            resendAsks = 0
        }
        received.clear()
    }

    private fun toRow(o: JSONObject, node: String, now: Long): NodeDetectionEntity? {
        val kind = o.optString("type")
        val seq = o.optLong("sf", 0)
        val mac: String
        val ssid: String
        var detail = ""
        when (kind) {
            "probe" -> { mac = o.optString("mac"); ssid = o.optString("ssid") }
            "beacon" -> { mac = o.optString("bssid"); ssid = o.optString("ssid") }
            "alert" -> { mac = o.optString("sa"); ssid = ""; detail = "${o.optString("event")} → ${o.optString("da")} (reason ${o.optInt("reason")})" }
            "ble_tracker" -> { mac = o.optString("mac"); ssid = o.optString("name"); detail = o.optString("kind") }
            else -> return null
        }
        if (mac.isBlank()) return null
        // ts_us is epoch once the node has taken the phone's time; before that it counts from boot
        val tsUs = o.optLong("ts_us", 0)
        val timeMs = if (tsUs > 1_600_000_000_000_000L) tsUs / 1000 else now
        if (seq == 0L) {
            val every = when (kind) { "beacon" -> 60_000L; "probe", "ble_tracker" -> 30_000L; else -> 0L }
            if (every > 0) {
                val key = "$node|$kind|$mac|$ssid"
                val last = lastLive[key]
                if (last != null && now - last < every) return null
                lastLive[key] = now
                if (lastLive.size > 20_000) lastLive.entries.removeIf { now - it.value > 120_000 }
            }
        }
        val hasFix = o.has("lat") && o.has("lon") && (o.optDouble("lat") != 0.0 || o.optDouble("lon") != 0.0)
        return NodeDetectionEntity(
            uid = if (seq > 0) "$node:sf:$seq" else "$node:$kind:$mac:$timeMs",
            node = node,
            kind = kind,
            mac = mac,
            ssid = ssid,
            rssi = o.optInt("rssi"),
            ch = o.optInt("ch"),
            detail = detail,
            timeMs = timeMs,
            lat = if (hasFix) o.optDouble("lat") else null,
            lon = if (hasFix) o.optDouble("lon") else null,
            acc = if (hasFix) o.optDouble("acc", 10.0) else null,
            locSource = if (hasFix) "node" else "",
            stored = seq > 0,
        )
    }
}
