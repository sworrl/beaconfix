package org.sworrl.beaconfix.sync

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runInterruptible
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.putJsonArray
import kotlinx.serialization.serializer
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.DesktopView
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.DevicesPositions
import org.sworrl.beaconfix.data.api.EmergencyDto
import org.sworrl.beaconfix.data.api.LocationDto
import org.sworrl.beaconfix.data.api.PoiDto
import org.sworrl.beaconfix.data.api.TrackPoint
import org.sworrl.beaconfix.data.api.TripDto
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.data.db.SnapshotEntity
import org.sworrl.beaconfix.estimate.Geo
import retrofit2.Response
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/**
 * The offline mirror's rules, shared by `data.DesktopLive` (what a screen asks for) and [SyncRepository] (the
 * background sync): which desktop answers count, how cached rows turn back into the API's shapes when a desktop
 * is out of reach, the home-network merge rule, and raw GET/PUT for the two endpoints whose typed models would lose
 * something (the `/track` stop details; an empty home list, which the typed body would leave out).
 *
 * Snapshots hold each endpoint's body as the desktop sent it (kind → body): `location` → `/location`,
 * `trip` → `/trip` (`{"trip":{…},"ts":…}`), `track` → `/track` (raw, `{"track":[stops…]}`), `emergency` →
 * `/emergency`, `devices` → `/devices/positions`, `hello` → `/hello`, `home` → `/home`. Source is `desktop:<id>`.
 */
object Mirror {
    const val LOCATION = "location"
    const val TRIP = "trip"
    const val TRACK = "track"
    const val EMERGENCY = "emergency"
    const val DEVICES = "devices"
    const val HELLO = "hello"
    const val HOME = "home"

    /** The view parts a refresh can fill from the cache before the desktop answers. */
    val PREFILLED = setOf(LOCATION, TRIP, "pois", EMERGENCY, DEVICES)

    // ── fetching ────────────────────────────────────────────────────────────

    /**
     * An endpoint's body, or null when the desktop does not have it (a 404 from an older desktop), refuses it, or
     * answers with something we cannot read. Network failures still throw, so an unreachable desktop fails fast
     * instead of timing out once per endpoint.
     */
    suspend fun <T> bodyOf(call: suspend () -> Response<T>): T? {
        val r = try { call() } catch (e: IllegalArgumentException) { return null }   // kotlinx SerializationException
        return if (r.isSuccessful) r.body() else null
    }

    data class Raw(val code: Int, val body: String) { val ok get() = code in 200..299 && body.isNotBlank() }

    /** GET [path] (e.g. `api/v1/track`) from [d] with the bearer [auth], body kept verbatim. */
    suspend fun rawGet(d: DesktopEntity, auth: String, path: String): Raw =
        call(Request.Builder().url(ApiFactory.baseUrl(d.host, d.port, d.tls) + path).header("Authorization", auth).get().build())

    /** PUT a JSON [json] body to [path]. */
    suspend fun rawPut(d: DesktopEntity, auth: String, path: String, json: String): Raw =
        call(Request.Builder().url(ApiFactory.baseUrl(d.host, d.port, d.tls) + path).header("Authorization", auth)
            .put(json.toRequestBody("application/json".toMediaType())).build())

    private suspend fun call(req: Request): Raw = runInterruptible(Dispatchers.IO) {
        ApiFactory.client.newCall(req).execute().use { r -> Raw(r.code, r.body?.string() ?: "") }
    }

    /** The `/track` stops in a raw body (lenient; unknown keys ignored). */
    fun trackPoints(raw: String): List<TrackPoint> =
        runCatching { ApiFactory.json.decodeFromString(org.sworrl.beaconfix.data.api.TrackDto.serializer(), raw).track }.getOrDefault(emptyList())

    // ── desktop fixes ───────────────────────────────────────────────────────

    private val ISO: DateTimeFormatter = DateTimeFormatter.ISO_LOCAL_DATE_TIME

    /** A desktop timestamp (`2026-09-27T10:00:00`, any offset suffix ignored, local time as the desktop writes it); null when unreadable. */
    fun parseIsoOrNull(s: String?, zone: ZoneId = ZoneId.systemDefault()): Long? {
        if (s.isNullOrBlank() || s.length < 19) return null
        return runCatching { LocalDateTime.parse(s.take(19), ISO).atZone(zone).toInstant().toEpochMilli() }.getOrNull()
    }

    /**
     * The desktop fixes still missing from the phone: one per timestamp, never one already stored ([exists]), never
     * one without a readable time (those used to be stamped "now" and piled up on every sync).
     */
    suspend fun newDesktopFixes(points: List<TrackPoint>, exists: suspend (Long) -> Boolean, zone: ZoneId = ZoneId.systemDefault()): List<FixEntity> {
        val seen = HashSet<Long>(); val out = ArrayList<FixEntity>()
        for (p in points) {
            val t = parseIsoOrNull(p.time, zone) ?: continue
            if (!seen.add(t) || exists(t)) continue
            out += FixEntity(time = t, lat = p.lat, lon = p.lon, acc = p.acc, source = "desktop", provider = p.source, place = p.place)
        }
        return out
    }

    /** The desktop's current fix as a fix row, or null when it has none or it is already stored. */
    suspend fun newLocationFix(l: LocationDto?, exists: suspend (Long) -> Boolean, zone: ZoneId = ZoneId.systemDefault()): FixEntity? {
        if (l == null || !l.valid) return null
        val t = parseIsoOrNull(l.time, zone) ?: return null
        if (exists(t)) return null
        return FixEntity(time = t, lat = l.lat, lon = l.lon, acc = l.accuracy, source = "desktop", provider = l.provider, place = l.place)
    }

    // ── home networks ───────────────────────────────────────────────────────

    enum class HomeStep { PUSH, KEEP, PULL }

    /**
     * The home-network merge rule. A list edited on the phone ([dirty]) is pushed to a desktop that granted control,
     * and is never overwritten by a pull before it has been pushed (a desktop without control leaves both sides as
     * they are). Otherwise the desktop's list is pulled.
     */
    fun homeStep(dirty: Boolean, canControl: Boolean): HomeStep = when {
        dirty && canControl -> HomeStep.PUSH
        dirty -> HomeStep.KEEP
        else -> HomeStep.PULL
    }

    /** What the phone's list becomes after a pull: the desktop's when it has one, otherwise unchanged (as before 1.4). */
    fun pulled(local: Set<String>, remote: List<String>?): Set<String> = if (remote.isNullOrEmpty()) local else remote.toSet()

    /** A push may clear the dirty flag only when the list was not edited again while it was on the way. */
    fun mayClearDirty(pushed: Set<String>, nowLocal: Set<String>): Boolean = pushed == nowLocal

    /** The `PUT /home` body; always carries `patterns`, even empty (the typed model would drop an empty list). */
    fun homeBody(patterns: Collection<String>): String = buildJsonObject { putJsonArray("patterns") { patterns.sorted().forEach { add(it) } } }.toString()

    // ── snapshots → views ───────────────────────────────────────────────────

    /** Where an emergency answer applies: its own origin (3.8), else the desktop's fix, else nowhere (0,0). */
    fun emergencyAt(e: EmergencyDto, fix: LocationDto?): Pair<Double, Double> =
        e.origin?.takeIf { it.lat != 0.0 || it.lon != 0.0 }?.let { it.lat to it.lon }
            ?: fix?.takeIf { it.valid }?.let { it.lat to it.lon }
            ?: (0.0 to 0.0)

    inline fun <reified T> decode(s: SnapshotEntity?): T? =
        s?.let { runCatching { ApiFactory.json.decodeFromString(serializer<T>(), it.json) }.getOrNull() }

    private val OSM_KEY = Regex("""^(node|way|relation)/(\d+)$""")

    /**
     * A cached place back in the `/pois` shape, for screens that still read [DesktopView.pois]. `d`/`brg` are
     * recomputed from where the desktop searched ([PoiEntity.originLat]/[PoiEntity.originLon]); the OSM link from the key.
     */
    fun toDto(e: PoiEntity): PoiDto {
        val m = OSM_KEY.find(e.key)
        val hasOrigin = e.originLat != 0.0 || e.originLon != 0.0
        return PoiDto(
            name = e.name, label = e.label, cat = e.cat, group = e.grp, icon = e.icon, color = e.color,
            lat = e.lat, lon = e.lon,
            d = if (hasOrigin) Geo.distanceM(e.originLat, e.originLon, e.lat, e.lon) else 0.0,
            brg = if (hasOrigin) Geo.bearingDeg(e.originLat, e.originLon, e.lat, e.lon) else 0.0,
            address = e.address, detail = e.detail, phone = e.phone, hours = e.hours, website = e.website,
            osm = if (m != null) "https://www.openstreetmap.org/${e.key}" else "",
            osmType = m?.groupValues?.get(1) ?: "", osmId = m?.groupValues?.get(2)?.toLongOrNull() ?: 0L,
            wheelchair = e.wheelchair, emergency = e.emergency, wifi = e.wifi,
            peds = e.peds, er = e.er, campusEr = e.campus, scope = e.scope,
            driveS = e.driveS, driveM = e.driveM, driveEst = e.driveEst,
        )
    }

    /**
     * A desktop's view as last saved, for when it has not answered yet in this run (or cannot): location, trip,
     * places, emergency and devices from its snapshots and cached rows. [DesktopView.cached] lists what came from the
     * cache, [DesktopView.cachedAt] is when the newest of it was saved, and [DesktopView.stale] is set when anything was.
     */
    fun prefill(d: DesktopEntity, snaps: Map<String, SnapshotEntity>, rows: List<PoiEntity>, now: Long = System.currentTimeMillis()): DesktopView {
        val src = DesktopCache.desktopSource(d.id)
        val mine = rows.filter { it.source == src }
        // the fix's age as the desktop reported it, plus how long ago that answer was saved
        val loc = decode<LocationDto>(snaps[LOCATION])?.let { l ->
            val saved = snaps[LOCATION]?.fetchedAt ?: now
            l.copy(ageS = l.ageS?.let { it + (now - saved).coerceAtLeast(0) / 1000.0 })
        }
        val trip = decode<TripDto>(snaps[TRIP])?.trip
        val em = decode<EmergencyDto>(snaps[EMERGENCY])
        val dev = decode<DevicesPositions>(snaps[DEVICES])?.devices
        val cached = buildSet {
            if (loc != null) add(LOCATION); if (trip != null) add(TRIP); if (mine.isNotEmpty()) add("pois")
            if (em != null) add(EMERGENCY); if (dev != null) add(DEVICES)
        }
        val at = (listOf(LOCATION, TRIP, EMERGENCY, DEVICES).filter { it in cached }.mapNotNull { snaps[it]?.fetchedAt } + mine.map { it.fetchedAt }).maxOrNull() ?: 0L
        return DesktopView(
            desktop = d, location = loc, trip = trip, pois = mine.map(::toDto).sortedBy { it.d },
            devices = dev ?: emptyList(), emergency = em,
            cached = cached, cachedAt = if (cached.isEmpty()) 0L else at, stale = cached.isNotEmpty(),
        )
    }

    /** After a refresh: [ok] = the desktop answered. Whatever is still from the cache (or everything, on a failure) keeps the view stale. */
    fun settle(v: DesktopView, ok: Boolean, error: String, now: Long): DesktopView = when {
        ok && v.cached.isEmpty() -> v.copy(error = "", fetched = now, stale = false, cachedAt = now)
        ok -> v.copy(error = "", fetched = now, stale = true)
        else -> v.copy(error = error, stale = v.cached.isNotEmpty() || v.fetched > 0 || v.cachedAt > 0, cachedAt = if (v.cached.isEmpty() && v.fetched > 0) v.fetched else v.cachedAt)
    }
}
