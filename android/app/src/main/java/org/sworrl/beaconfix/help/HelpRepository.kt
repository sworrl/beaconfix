package org.sworrl.beaconfix.help

import android.util.Log
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.EmergencyDto
import org.sworrl.beaconfix.data.api.Hello
import org.sworrl.beaconfix.data.api.HelpPlaceDto
import org.sworrl.beaconfix.data.db.SnapshotEntity
import org.sworrl.beaconfix.estimate.Geo
import org.sworrl.beaconfix.poi.PhonePlacesResult
import org.sworrl.beaconfix.poi.PhonePlacesState
import org.sworrl.beaconfix.ui.Emergency
import java.time.Instant
import java.time.LocalDateTime
import java.time.OffsetDateTime
import java.time.ZoneId
import javax.inject.Inject
import javax.inject.Singleton

/**
 * What the Help screen shows beyond [HelpSnapshot]: the origin fix's accuracy, where the places were searched from
 * ([dataLat]/[dataLon], [dataDistM] from the origin; -1 unknown), and the phone search's last note.
 */
data class HelpMeta(
    val originAccM: Double = -1.0,
    val dataLat: Double = 0.0,
    val dataLon: Double = 0.0,
    val dataDistM: Double = -1.0,
    val phoneNote: String = "",
    val phoneBusyUntil: Long = 0L,
    val lastRunAt: Long = 0L,
)

/**
 * Nearest help (children's ER, ER, urgent care, police, fire, pharmacy, vet) from the best origin, desktop or phone.
 *
 * [refresh]: ask the desktops (≤ 6 s, unless DevFlags block them), rebuild from the offline mirror (Room `snapshots`
 * and `pois`, via [HelpInputs]); when no desktop answered and the data is old or far away, let the phone search
 * OpenStreetMap itself ([org.sworrl.beaconfix.poi.PhonePlaces]) and rebuild. Distances, bearings and drive times are
 * recomputed from the chosen origin ([Origin]). A desktop older than 3.8 gets name-based children's ERs
 * ([PedsFallback]). [refresh] never throws: on failure it keeps the previous snapshot and sets
 * [HelpSnapshot.lastError]. [refreshIfStale] only ever talks to a desktop (no Overpass, no Nominatim).
 */
@Singleton
class HelpRepository(private val io: HelpInputs) {
    @Inject constructor(inputs: AndroidHelpInputs) : this(inputs as HelpInputs)

    private val _snapshot = MutableStateFlow(HelpSnapshot())
    val snapshot: StateFlow<HelpSnapshot> = _snapshot.asStateFlow()
    private val _meta = MutableStateFlow(HelpMeta())
    val meta: StateFlow<HelpMeta> = _meta.asStateFlow()
    private val _refreshing = MutableStateFlow(false)
    /** True while a refresh runs (for pull-to-refresh indicators). */
    val refreshing: StateFlow<Boolean> = _refreshing.asStateFlow()
    private val mutex = Mutex()

    suspend fun refresh(force: Boolean = false): HelpSnapshot = run(force = force, phoneSearch = true, netAddress = true)

    suspend fun refreshIfStale(maxAgeMs: Long = 30 * 60_000L) {
        val now = io.now()
        val last = _meta.value.lastRunAt
        if (now - last > maxAgeMs || (_snapshot.value.places.isEmpty() && now - last > 60_000L)) run(force = false, phoneSearch = false, netAddress = false)
    }

    private suspend fun run(force: Boolean, phoneSearch: Boolean, netAddress: Boolean): HelpSnapshot = mutex.withLock {
        _refreshing.value = true
        val prev = _snapshot.value
        val prevMeta = _meta.value
        try {
            if (prev.fetchedAt == 0L) runCatching { build(io.now(), false, prevMeta) }.getOrNull()?.let { publish(it) }   // show the mirror at once
            var err = ""
            val answered = try { io.refreshDesktops() } catch (e: CancellationException) { throw e } catch (e: Exception) {
                err = "Desktop: " + (e.message ?: e.javaClass.simpleName); false
            }
            var built = build(io.now(), netAddress, prevMeta)
            var phoneNote = prevMeta.phoneNote
            if (!answered && phoneSearch && io.phonePlacesOn()) {
                val fix = Origin.of(io.phoneFix())
                if (fix != null && (force || needsPhoneSearch(built, fix, io.now()))) {
                    // the search can take a minute: show what the mirror has meanwhile
                    if (built.snapshot.places.isNotEmpty()) publish(Built(built.snapshot.copy(lastError = err), built.meta.copy(lastRunAt = prevMeta.lastRunAt)))
                    val r = try { io.phone.refreshAround(fix.lat, fix.lon, force = force, pediatric = true) } catch (e: CancellationException) { throw e } catch (e: Exception) {
                        PhonePlacesResult(false, note = e.message ?: "phone search failed")
                    }
                    Log.i(TAG, "phone search: ok=${r.ok} count=${r.count} skipped=${r.skipped} ${r.note}")
                    phoneNote = r.note
                    if (r.ok) built = build(io.now(), netAddress, prevMeta)
                    else if (!r.skipped && r.note.isNotBlank() && err.isEmpty()) err = r.note
                }
            }
            var snap = built.snapshot.copy(lastError = err)
            if (snap.places.isEmpty() && prev.places.isNotEmpty()) snap = prev.copy(lastError = err.ifEmpty { "No help places in reach right now" }, stale = true)
            val meta = built.meta.copy(phoneNote = phoneNote, phoneBusyUntil = runCatching { io.phone.busyUntil }.getOrDefault(0L), lastRunAt = io.now())
            publish(Built(snap, meta))
            Log.i(TAG, "refresh force=$force desktop=$answered origin=${snap.origin} places=${snap.places.size} source=${snap.source} stale=${snap.stale} peds=${snap.pediatricSupported} ${snap.lastError}")
            snap
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Log.w(TAG, "refresh failed: ${e.message}")
            val kept = prev.copy(lastError = e.message ?: e.javaClass.simpleName)
            _snapshot.value = kept
            _meta.value = prevMeta.copy(lastRunAt = io.now())
            kept
        } finally {
            _refreshing.value = false
            io.touchWidgets()
        }
    }

    private fun publish(b: Built) { _snapshot.value = b.snapshot; _meta.value = b.meta }

    internal data class Built(val snapshot: HelpSnapshot, val meta: HelpMeta)

    private fun needsPhoneSearch(b: Built, fix: Origin.Fix, now: Long): Boolean {
        if (b.snapshot.places.isEmpty() || b.snapshot.fetchedAt <= 0) return true
        if (now - b.snapshot.fetchedAt > DAY_MS) return true
        if (!Origin.valid(b.meta.dataLat, b.meta.dataLon)) return true
        return Geo.distanceM(fix.lat, fix.lon, b.meta.dataLat, b.meta.dataLon) > PHONE_SEARCH_MOVED_M
    }

    /** The desktop's `/emergency` answer as read from its snapshot. */
    private class Emerg(val dto: EmergencyDto, val pediatric: Boolean, val snap: SnapshotEntity, val oLat: Double, val oLon: Double, val oTime: Long)

    private fun readEmergency(s: SnapshotEntity?): Emerg? {
        s ?: return null
        val obj = runCatching { ApiFactory.json.parseToJsonElement(s.json).jsonObject }.getOrNull() ?: return null
        val dto = runCatching { ApiFactory.json.decodeFromJsonElement(EmergencyDto.serializer(), obj) }.getOrNull() ?: return null
        val peds = obj.hasAny("pediatric", "pediatricSearchKm", "pediatricNote", "pediatricTime")
        val o = dto.origin
        val oValid = o != null && Origin.valid(o.lat, o.lon)
        return Emerg(dto, peds, s,
            if (oValid) o!!.lat else s.lat, if (oValid) o!!.lon else s.lon,
            if (oValid) (isoMs(o!!.time) ?: s.fetchedAt) else s.fetchedAt)
    }

    private fun JsonObject.hasAny(vararg k: String) = k.any { containsKey(it) }

    /** Help places named in the emergency answer, as candidates (they fill gaps when the places list is missing). */
    private fun emergencyCandidates(e: Emerg): List<NearestHelp.Candidate> {
        val d = e.dto
        fun c(p: HelpPlaceDto?, cat: String, peds: Int = 0): NearestHelp.Candidate? {
            p ?: return null
            if (!Origin.valid(p.lat, p.lon)) return null
            val realCat = if (cat == "peds_er" && peds == 3) "health" else cat
            return NearestHelp.Candidate(
                key = DesktopCache.osmKey(p.osm) ?: DesktopCache.llKey(p.lat, p.lon, realCat), cat = realCat, name = p.name, lat = p.lat, lon = p.lon,
                detail = if (p.urgentCare) "urgent care" else "",
                phone = p.phone, address = p.address, hours = p.hours, website = p.website, peds = peds, er = p.er, campus = p.campusEr,
                emergency = p.er == "yes", driveS = p.driveS, driveEst = p.driveEst, fromLat = e.oLat, fromLon = e.oLon,
                source = e.snap.source, fetchedAt = e.snap.fetchedAt, originLat = e.oLat, originLon = e.oLon,
            )
        }
        return listOfNotNull(
            c(d.pediatric, "peds_er", d.pediatric?.tier?.takeIf { it > 0 } ?: 2),
            c(d.pediatricCloser, "peds_er", d.pediatricCloser?.tier?.takeIf { it > 0 } ?: 2),
            c(d.pediatricUrgent, "peds_urgent", 4),
            c(d.hospital, "health"), c(d.urgent, "urgent"), c(d.police, "police"), c(d.fire, "fire"), c(d.pharmacy, "pharmacy"), c(d.vet, "vet"),
        )
    }

    /** Rebuild the answer from the offline mirror. Pure apart from reading [io]. */
    internal suspend fun build(now: Long, netAddress: Boolean, prevMeta: HelpMeta = HelpMeta()): Built {
        val e = readEmergency(io.emergency())
        val rows = io.places().filter { it.cat in NearestHelp.HELP_CATS }
        val fromRows = rows.map { NearestHelp.fromPoi(it) }
        val fromE = e?.let { emergencyCandidates(it) }.orEmpty().filter { ec ->
            fromRows.none { r -> r.key == ec.key || (PedsFallback.norm(r.name) == PedsFallback.norm(ec.name) && Geo.distanceM(r.lat, r.lon, ec.lat, ec.lon) < SAME_PLACE_M) }
        }
        var cands = fromRows + fromE
        val desktopData = cands.any { it.source.startsWith("desktop:") } || e != null
        val helloPeds = runCatching { ApiFactory.json.decodeFromString(Hello.serializer(), io.hello()?.json ?: "") }.getOrNull()?.features?.contains("pediatric") == true
        val desktopPeds = (e?.pediatric == true) || helloPeds || rows.any { DesktopCache.isDesktop(it) && (it.peds > 0 || it.cat.startsWith("peds_")) }
        if (!desktopPeds) cands = cands.map { if (it.source.startsWith("desktop:")) PedsFallback.apply(it) else it }
        val phoneData = cands.any { it.source == DesktopCache.PHONE }
        val supported = if (desktopData) desktopPeds else phoneData

        // where to measure from
        val rvFromE = e?.takeIf { Origin.valid(it.oLat, it.oLon) }?.let { Origin.Fix(it.oLat, it.oLon, it.dto.origin?.acc ?: -1.0, it.oTime) }
        val rvFix = listOfNotNull(rvFromE, Origin.of(io.desktopFix())).maxByOrNull { it.time }
        var o = Origin.choose(Origin.of(io.phoneFix()), rvFix, now)
        if (!o.known) cands.filter { Origin.valid(it.originLat, it.originLon) }.maxByOrNull { it.fetchedAt }?.let { c ->
            o = Origin.Chosen(if (c.source == DesktopCache.PHONE) Origin.PHONE else Origin.RV, c.originLat, c.originLon, -1.0, c.fetchedAt)
        }

        val at = LocalDateTime.ofInstant(Instant.ofEpochMilli(now), ZoneId.systemDefault())
        val picks = if (o.known) NearestHelp.pick(cands, o.lat, o.lon, at) else emptyList()

        // provenance: the newest data among what is shown
        val basis = picks.map { it.from }.ifEmpty { cands }
        val newest = basis.maxByOrNull { it.fetchedAt }
        val fetchedAt = newest?.fetchedAt ?: 0L
        val dataValid = newest != null && Origin.valid(newest.originLat, newest.originLon)
        val dataDist = if (dataValid && o.known) Geo.distanceM(o.lat, o.lon, newest!!.originLat, newest.originLon) else -1.0

        val phoneSnap = io.phoneSearch()
        val phoneFar = rows.any { it.source == DesktopCache.PHONE && it.scope == DesktopCache.FAR }
        val pedsKm = when {
            (e?.dto?.pediatricSearchKm ?: 0) > 0 -> e!!.dto.pediatricSearchKm
            phoneFar -> io.pedsRadiusKm()
            else -> 0
        }
        val staleM = if (pedsKm > 0) pedsKm * 1000.0 / 2 else DEFAULT_STALE_M
        val stale = fetchedAt > 0 && (now - fetchedAt > DAY_MS || dataDist > staleM)

        val pedsPick = picks.firstOrNull { it.place.kind == HelpKind.PEDS_ER }?.place
        val pedsNote = pedsNote(e, pedsPick, pedsKm, supported, desktopData, phoneSnap)

        val cc = e?.dto?.countryCode?.takeIf { it.isNotBlank() } ?: runCatching { io.countryCode() }.getOrDefault("")
        val number = e?.dto?.number?.takeIf { it.isNotBlank() } ?: Emergency.number(cc.ifBlank { null })
        val address = if (o.known) runCatching { io.address(o.lat, o.lon, netAddress) }.getOrNull() else null

        val snap = HelpSnapshot(
            number = number, countryCode = cc, places = picks.map { it.place }, pedsNote = pedsNote, pedsSearchKm = pedsKm,
            pediatricSupported = supported, origin = o.kind, originLat = o.lat, originLon = o.lon, originAgeMs = o.ageMs(now),
            source = newest?.source.orEmpty(), fetchedAt = fetchedAt, stale = stale, lastError = "",
            poisonControl = NearestHelp.poisonControl(cc), address = address,
            originAt = if (o.known && o.time > 0) o.time else 0L, computedAt = now,
        )
        val meta = prevMeta.copy(originAccM = o.accM, dataLat = if (dataValid) newest!!.originLat else 0.0, dataLon = if (dataValid) newest!!.originLon else 0.0, dataDistM = dataDist)
        return Built(snap, meta)
    }

    private fun pedsNote(e: Emerg?, pick: HelpPlace?, pedsKm: Int, supported: Boolean, desktopData: Boolean, phoneSnap: SnapshotEntity?): String {
        val phoneState = phoneSnap?.let { s -> runCatching { ApiFactory.json.decodeFromString(PhonePlacesState.serializer(), s.json) }.getOrNull() }
        val base = e?.dto?.pediatricNote?.takeIf { it.isNotBlank() } ?: phoneState?.note.orEmpty()
        if (pick == null && pedsKm > 0) {
            // "none mapped" only after a search that finished: a pediatric search time from the desktop, or the phone's own
            // completed pediatric search. pedsKm is just the configured radius, not proof that anything was searched.
            val searched = !e?.dto?.pediatricTime.isNullOrBlank() || phoneState?.peds != null
            return if (searched) "No pediatric ER mapped within $pedsKm km — go to the nearest ER"
            else "Children's ER search didn't finish — go to the nearest ER" + (base.takeIf { it.isNotBlank() && !it.startsWith("No pediatric ER") }?.let { " ($it)" } ?: "; will retry")
        }
        if (desktopData && !supported) return if (pick != null) "Children's hospital found by name only (the RV desktop is older than 3.8) — call ahead" else ""
        // the desktop's pick-specific notes only hold when our pick agrees
        if (base.startsWith("ER not confirmed") && pick?.tier != 2) return ""
        if (base.startsWith("No pediatric ER mapped") && pick != null) return ""
        return base
    }

    companion object {
        private const val TAG = "BfHelp"
        const val DAY_MS = 24 * 3600_000L
        const val PHONE_SEARCH_MOVED_M = 5_000.0
        const val DEFAULT_STALE_M = 25_000.0
        private const val SAME_PLACE_M = 300.0

        /** ISO 8601 with or without an offset (a desktop's local time), to epoch ms; null when unreadable. */
        fun isoMs(s: String?): Long? {
            val t = s?.trim()?.takeIf { it.isNotEmpty() } ?: return null
            return runCatching { OffsetDateTime.parse(t).toInstant().toEpochMilli() }.getOrNull()
                ?: runCatching { Instant.parse(t).toEpochMilli() }.getOrNull()
                ?: runCatching { LocalDateTime.parse(t).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli() }.getOrNull()
        }
    }
}
