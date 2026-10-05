package org.sworrl.beaconfix.anchors

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.api.AnchorDto
import org.sworrl.beaconfix.data.db.AnchorEntity
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.estimate.ScanSample
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Anchors (docs/RANGING.md §4): surveyed antennas and places. Local Room table, synced both ways with every paired desktop
 * that has the endpoints (`GET/POST/DELETE /api/v1/anchors`, plus `anchors:[…]` rows through /db/changes); a 404 keeps
 * them local only. Merge rule: newest placedAt (or deletedAt) wins.
 */
@Singleton
class AnchorRepository @Inject constructor(private val db: AppDatabase, private val desktops: DesktopStore) {
    val anchors: Flow<List<AnchorDto>> = db.anchors().all().map { l -> l.map { decode(it.json) } }
    suspend fun allNow(): List<AnchorDto> = db.anchors().allNow().map { decode(it.json) }
    suspend fun get(id: String): AnchorDto? = db.anchors().get(id)?.takeIf { !it.deleted }?.let { decode(it.json) }

    /** Create or update from the phone: stamps placedBy/placedAt, normalises, marks dirty, pushes to desktops that have the endpoint. */
    suspend fun save(a: AnchorDto, touch: Boolean = true): AnchorDto {
        val now = isoNow()
        val norm = normalise(a.copy(id = a.id.ifEmpty { java.util.UUID.randomUUID().toString() }, placedBy = "android", placedAt = if (touch || a.placedAt.isEmpty()) now else a.placedAt))
        if (norm.ref) for (o in db.anchors().allNow()) { val od = decode(o.json); if (od.ref && od.id != norm.id && org.sworrl.beaconfix.estimate.Geo.distanceM(od.lat, od.lon, norm.lat, norm.lon) < 100) db.anchors().upsert(o.copy(json = encode(od.copy(ref = false)), ref = false, dirty = true)) }
        db.anchors().upsert(entity(norm, dirty = true))
        applyToAps()
        push()
        return norm
    }
    suspend fun delete(id: String) {
        val e = db.anchors().get(id) ?: return
        val d = decode(e.json).copy(deleted = true, deletedAt = isoNow())
        db.anchors().upsert(e.copy(json = encode(d), deleted = true, dirty = true))
        for (b in d.bssids) db.aps().get(b)?.let { ap -> if (ap.posSource == "anchor") db.aps().setPosition(b, ap.lat ?: 0.0, ap.lon ?: 0.0, ap.acc ?: 50.0, "observed", ap.refDbm, ap.pathExp, ap.residual) }
        push()
    }

    /** Push dirty rows to every paired desktop that speaks anchors; a 404 marks it as not having the endpoint (until next hello). */
    private val noEndpoint = HashSet<String>()
    suspend fun push() {
        val dirty = db.anchors().dirty()
        if (dirty.isEmpty()) return
        var anyOk = false
        for (d in desktops.paired()) {
            if (d.id in noEndpoint || !desktops.hasScope(d, "control")) continue
            val api = desktops.api(d); val auth = desktops.auth(d) ?: continue
            var ok = true
            for (e in dirty) {
                val r = runCatching { if (e.deleted) api.deleteAnchor(auth, e.id).code() else api.setAnchor(auth, decode(e.json)).code() }.getOrNull() ?: run { ok = false; -1 }
                if (r == 404) { noEndpoint += d.id; ok = false; break }
                if (r !in 200..299 && r != -1) ok = false
            }
            if (ok) anyOk = true
        }
        if (anyOk) db.anchors().clean(dirty.map { it.id })
    }
    fun forgetNoEndpoint(desktopId: String) { noEndpoint -= desktopId }

    /** Merge rows from a desktop (GET /anchors or /db/changes `anchors`); newest placedAt / deletedAt wins, local dirty edits are kept. */
    suspend fun merge(rows: List<JsonObject>): Int {
        var n = 0
        for (o in rows) {
            val a = runCatching { json.decodeFromJsonElement(AnchorDto.serializer(), o) }.getOrNull() ?: continue
            if (a.id.isEmpty()) continue
            val local = db.anchors().get(a.id)
            val theirs = a.deletedAt ?: a.placedAt; val mine = local?.let { decode(it.json) }?.let { it.deletedAt ?: it.placedAt } ?: ""
            if (local != null && (local.dirty || mine >= theirs)) continue
            val deleted = a.deleted == true
            db.anchors().upsert(entity(a, dirty = false).copy(deleted = deleted, seq = a.seq ?: local?.seq ?: 0))
            n++
        }
        return n
    }
    suspend fun mergeDtos(list: List<AnchorDto>): Int = merge(list.map { json.encodeToJsonElement(AnchorDto.serializer(), it).jsonObject })

    /** Pin every anchored BSSID's AP row to the anchor (position, accuracy, posSource "anchor"); rows of deleted anchors go back to "observed" refits. */
    suspend fun applyToAps() {
        for (a in allNow()) for (b in a.bssids) db.aps().get(b)?.let { ap -> if (ap.lat != a.lat || ap.lon != a.lon || ap.posSource != "anchor") db.aps().setPosition(b, a.lat, a.lon, a.accM, "anchor", ap.refDbm, ap.pathExp, ap.residual) }
    }

    /** BSSIDs that are pinned by an anchor (never refitted; enter self-location as known transmitters). */
    suspend fun pinned(): Map<String, AnchorDto> { val m = HashMap<String, AnchorDto>(); for (a in allNow()) for (b in a.bssids) m[b.uppercase()] = a; return m }

    companion object {
        val json = Json { ignoreUnknownKeys = true; isLenient = true; explicitNulls = false; coerceInputValues = true }
        fun decode(s: String): AnchorDto = json.decodeFromString(AnchorDto.serializer(), s)
        fun encode(a: AnchorDto): String = json.encodeToString(AnchorDto.serializer(), a)
        fun entity(a: AnchorDto, dirty: Boolean) = AnchorEntity(a.id, encode(a), a.name, a.kind, a.lat, a.lon, a.rv, a.ref, a.deleted == true, a.placedAt, a.seq ?: 0, dirty)
        fun isoNow(): String = DateTimeFormatter.ISO_INSTANT.format(Instant.now().atOffset(ZoneOffset.UTC).withNano(0))
        val KINDS = listOf("fixed-point" to "Fixed known point (anchor)", "esp32-node" to "ESP32 monitor node", "this-computer" to "This computer", "wifi-ap" to "Wi-Fi access point", "ble" to "Bluetooth beacon", "rtt-responder" to "Wi-Fi RTT responder", "custom" to "Custom")
        fun normalise(a: AnchorDto): AnchorDto = a.copy(
            kind = a.kind.ifEmpty { "custom" }, name = a.name.trim().take(64).ifEmpty { KINDS.firstOrNull { it.first == a.kind.ifEmpty { "custom" } }?.second ?: "Anchor" },
            bssids = a.bssids.map { it.uppercase().trim() }.filter { it.length == 17 }.distinct(), accM = a.accM.coerceIn(0.05, 500.0),
            source = a.source.ifEmpty { "map-pick" }, placedBy = a.placedBy.ifEmpty { "android" })

        /**
         * Group scan results by physical transmitter for the BSSID chooser: one box exposes several radios/SSIDs whose BSSIDs
         * differ only in the first octet (locally administered bits) and the last one, e.g. all 0?:11:22:33:44:5? of one router.
         */
        fun groups(scan: List<ScanSample>): List<BssidGroup> {
            val by = LinkedHashMap<String, MutableList<ScanSample>>()
            for (s in scan.sortedByDescending { it.dbm }) { val p = s.bssid.uppercase().split(':'); if (p.size != 6) continue; by.getOrPut(p.subList(1, 5).joinToString(":")) { ArrayList() } += s }
            return by.values.map { l -> BssidGroup(l.map { it.ssid }.filter { it.isNotEmpty() }.distinct(), l.map { it.bssid.uppercase() }.distinct(), l.maxOf { it.dbm }, l.map { org.sworrl.beaconfix.collector.ObservationRecorder.bandOf(it.freq) }.distinct().sorted()) }
        }
    }
}

data class BssidGroup(val ssids: List<String>, val bssids: List<String>, val dbm: Int, val bands: List<String>) {
    val title: String get() = ssids.firstOrNull() ?: "(hidden)"
    val subtitle: String get() = (if (ssids.size > 1) ssids.drop(1).joinToString(", ") + " · " else "") + "${bssids.size} radio${if (bssids.size == 1) "" else "s"} · ${bands.joinToString("/")} GHz · $dbm dBm"
}
