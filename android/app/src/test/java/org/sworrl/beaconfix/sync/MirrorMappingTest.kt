package org.sworrl.beaconfix.sync

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.DevicesPositions
import org.sworrl.beaconfix.data.api.EmergencyDto
import org.sworrl.beaconfix.data.api.HelpPlaceDto
import org.sworrl.beaconfix.data.api.LinkedDevice
import org.sworrl.beaconfix.data.api.LocationDto
import org.sworrl.beaconfix.data.api.OriginDto
import org.sworrl.beaconfix.data.api.PoiDto
import org.sworrl.beaconfix.data.api.TrackPoint
import org.sworrl.beaconfix.data.api.Trip
import org.sworrl.beaconfix.data.api.TripDto
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.data.db.SnapshotEntity
import org.sworrl.beaconfix.estimate.Geo
import retrofit2.Response
import java.io.IOException
import java.time.ZoneOffset

class MirrorMappingTest {
    private val desk = DesktopEntity(id = "192.0.2.10:47822", host = "192.0.2.10", port = 47822, name = "desktop", paired = true)
    private val src = DesktopCache.desktopSource(desk.id)
    private val utc = ZoneOffset.UTC

    /** Every field set to something other than its default, so a field the mapping forgets cannot hide. */
    private val full = PoiDto(name = "Test Children's Hospital", label = "Pediatric ER", cat = "peds_er", group = "civic", icon = "i", color = "#ff5fa2",
        lat = 40.1, lon = -75.1, d = 0.0, brg = 0.0, address = "1 Test St", detail = "ER on campus: Test General — call ahead", phone = "+1 555 0100",
        hours = "24/7", website = "https://example.org", osm = "https://www.openstreetmap.org/way/9", osmType = "way", osmId = 9L, wheelchair = "yes",
        emergency = true, wifi = true, peds = 2, er = "yes", campusEr = "Test General", scope = "far", driveS = 600, driveM = 9000, driveEst = false)

    // ── places ──────────────────────────────────────────────────────────────

    @Test fun osmLinkBecomesAKeyAndBack() {
        assertEquals("way/329264979", DesktopCache.osmKey("https://www.openstreetmap.org/way/329264979"))
        val e = DesktopCache.toEntity(src, PoiDto(name = "T", cat = "health", lat = 40.0, lon = -75.0, osm = "https://www.openstreetmap.org/relation/8017287"), 0.0, 0.0, 1)
        assertEquals("relation/8017287", e.key)
        val back = Mirror.toDto(e)
        assertEquals("https://www.openstreetmap.org/relation/8017287", back.osm)
        assertEquals("relation", back.osmType); assertEquals(8017287L, back.osmId)
        // a place without an OSM object keeps its position key and gets no link
        val ll = Mirror.toDto(DesktopCache.toEntity(src, PoiDto(name = "T", cat = "fuel", lat = 40.0, lon = -75.0), 0.0, 0.0, 1))
        assertEquals("", ll.osm); assertEquals("", ll.osmType); assertEquals(0L, ll.osmId)
    }

    @Test fun everyPoiDtoFieldIsMappedBothWays() {
        // the fixture must set every field (a field added to PoiDto later fails here until it is mapped)
        val defaults = PoiDto()
        val skip = setOf("d", "brg")      // recomputed from the search origin
        for (f in PoiDto::class.java.declaredFields.filter { !java.lang.reflect.Modifier.isStatic(it.modifiers) }) {
            if (f.name in skip) continue
            f.isAccessible = true
            assertNotEquals("fixture leaves ${f.name} at its default", f.get(defaults), f.get(full))
        }
        val e = DesktopCache.toEntity(src, full, 40.0, -75.0, 1234L)
        val back = Mirror.toDto(e)
        assertEquals(full.copy(d = back.d, brg = back.brg), back)
        // d and brg come back as seen from where the desktop searched
        assertEquals(Geo.distanceM(40.0, -75.0, 40.1, -75.1), back.d, 1e-6)
        assertEquals(Geo.bearingDeg(40.0, -75.0, 40.1, -75.1), back.brg, 1e-6)
        assertEquals(0.0, Mirror.toDto(e.copy(originLat = 0.0, originLon = 0.0)).d, 0.0)
    }

    // ── fetching ────────────────────────────────────────────────────────────

    private fun <T> httpError(code: Int): Response<T> = Response.error(code, "{\"error\":\"x\"}".toResponseBody("application/json".toMediaType()))

    @Test fun a404OnEmergencyIsIgnored() = runBlocking {
        assertNull(Mirror.bodyOf<EmergencyDto> { httpError(404) })
        assertNull(Mirror.bodyOf<EmergencyDto> { httpError(403) })
        assertEquals("911", Mirror.bodyOf { Response.success(EmergencyDto(number = "911")) }?.number)
        // an answer we cannot read (a desktop with a different shape) is skipped, not fatal
        assertNull(Mirror.bodyOf<EmergencyDto> { throw kotlinx.serialization.SerializationException("bad") })
    }

    @Test fun anUnreachableDesktopStillFails() {
        val thrown = runCatching { runBlocking { Mirror.bodyOf<EmergencyDto> { throw IOException("unreachable") } } }.exceptionOrNull()
        assertTrue(thrown is IOException)
    }

    @Test fun emergencyIsFiledWhereItApplies() {
        val fix = LocationDto(valid = true, lat = 40.0, lon = -75.0)
        assertEquals(40.5 to -75.5, Mirror.emergencyAt(EmergencyDto(origin = OriginDto(lat = 40.5, lon = -75.5)), fix))
        assertEquals(40.0 to -75.0, Mirror.emergencyAt(EmergencyDto(), fix))
        assertEquals(0.0 to 0.0, Mirror.emergencyAt(EmergencyDto(), LocationDto(valid = false, lat = 1.0, lon = 1.0)))
    }

    // ── desktop fixes ───────────────────────────────────────────────────────

    @Test fun desktopFixesAreStoredOncePerTimestamp() = runBlocking {
        val stored = HashSet<Long>()
        val t1 = Mirror.parseIsoOrNull("2026-09-27T10:00:00", utc)!!
        stored += t1
        val pts = listOf(
            TrackPoint(40.0, -75.0, 10.0, "gps", "2026-09-27T10:00:00", "A"),        // already stored
            TrackPoint(40.1, -75.0, 10.0, "gps", "2026-09-27T11:00:00+00:00", "B"),
            TrackPoint(40.1, -75.0, 10.0, "gps", "2026-09-27T11:00:00", "B again"),   // same time twice in one answer
            TrackPoint(40.2, -75.0, 10.0, "gps", "", "no time"),                      // used to become "now" on every sync
            TrackPoint(40.3, -75.0, 10.0, "gps", "garbage", "bad time"),
        )
        val out = Mirror.newDesktopFixes(pts, { it in stored }, utc)
        assertEquals(listOf("B"), out.map { it.place })
        assertTrue(out.all { it.source == "desktop" })
        // a second sync of the same track adds nothing
        stored += out.map { it.time }
        assertTrue(Mirror.newDesktopFixes(pts, { it in stored }, utc).isEmpty())
        // the desktop's current fix: once, and only when valid and dated
        val l = LocationDto(valid = true, lat = 40.0, lon = -75.0, accuracy = 8.0, time = "2026-09-27T12:00:00")
        assertEquals(40.0, Mirror.newLocationFix(l, { it in stored }, utc)!!.lat, 0.0)
        assertNull(Mirror.newLocationFix(l.copy(time = ""), { false }, utc))
        assertNull(Mirror.newLocationFix(l.copy(valid = false), { false }, utc))
        assertNull(Mirror.newLocationFix(l, { true }, utc))
    }

    @Test fun trackStopsParseLeniently() {
        val raw = """{"track":[{"lat":40.0,"lon":-75.0,"acc":12,"source":"gps","place":"Test Park","time":"2026-09-27T10:00:00","departed":"2026-09-27T12:00:00","dwellSecs":7200,"legKm":3.5,"elev":250.5,"city":"Testville"}],"count":1,"ts":"x"}"""
        val p = Mirror.trackPoints(raw).single()
        assertEquals("Test Park", p.place); assertEquals(12.0, p.acc, 0.0)
        assertTrue(Mirror.trackPoints("{broken").isEmpty())
    }

    // ── home networks ───────────────────────────────────────────────────────

    @Test fun homeDirtyMergeRule() {
        // edited on the phone: pushed where the token has control, left alone where it has not — never pulled over
        assertEquals(Mirror.HomeStep.PUSH, Mirror.homeStep(dirty = true, canControl = true))
        assertEquals(Mirror.HomeStep.KEEP, Mirror.homeStep(dirty = true, canControl = false))
        assertEquals(Mirror.HomeStep.PULL, Mirror.homeStep(dirty = false, canControl = true))
        assertEquals(Mirror.HomeStep.PULL, Mirror.homeStep(dirty = false, canControl = false))
        // a pull adopts the desktop's list, but an empty or missing one never wipes the phone's (as before)
        assertEquals(setOf("Desk*"), Mirror.pulled(setOf("Mine*"), listOf("Desk*")))
        assertEquals(setOf("Mine*"), Mirror.pulled(setOf("Mine*"), emptyList()))
        assertEquals(setOf("Mine*"), Mirror.pulled(setOf("Mine*"), null))
        // the flag clears only if the list was not edited again while the push was on the way
        assertTrue(Mirror.mayClearDirty(setOf("A", "B"), setOf("B", "A")))
        assertFalse(Mirror.mayClearDirty(setOf("A"), setOf("A", "C")))
    }

    @Test fun homeBodyAlwaysCarriesPatterns() {
        val empty = Json.parseToJsonElement(Mirror.homeBody(emptySet())).jsonObject
        assertEquals(0, empty["patterns"]!!.jsonArray.size)       // the desktop answers 400 without the key
        val two = Json.parseToJsonElement(Mirror.homeBody(setOf("b*", "a*"))).jsonObject
        assertEquals(listOf("a*", "b*"), two["patterns"]!!.jsonArray.map { it.jsonPrimitive.content })
    }

    // ── snapshots → views ───────────────────────────────────────────────────

    private fun snap(kind: String, json: String, at: Long) = SnapshotEntity(src, kind, json, fetchedAt = at)
    private fun enc(v: Any): String = when (v) {
        is LocationDto -> ApiFactory.json.encodeToString(LocationDto.serializer(), v)
        is TripDto -> ApiFactory.json.encodeToString(TripDto.serializer(), v)
        is EmergencyDto -> ApiFactory.json.encodeToString(EmergencyDto.serializer(), v)
        is DevicesPositions -> ApiFactory.json.encodeToString(DevicesPositions.serializer(), v)
        else -> error("unexpected")
    }

    @Test fun prefillBuildsAStaleViewFromTheCache() {
        val snaps = mapOf(
            Mirror.LOCATION to snap(Mirror.LOCATION, enc(LocationDto(valid = true, lat = 40.0, lon = -75.0, place = "Testville")), 100),
            Mirror.TRIP to snap(Mirror.TRIP, enc(TripDto(Trip(rank = "Scout", stopsToday = 2))), 300),
            Mirror.EMERGENCY to snap(Mirror.EMERGENCY, enc(EmergencyDto(number = "911", hospital = HelpPlaceDto(name = "Test General"))), 200),
            Mirror.DEVICES to snap(Mirror.DEVICES, enc(DevicesPositions(listOf(LinkedDevice(device = "pi", kind = "pi")))), 150),
        )
        val near = DesktopCache.toEntity(src, PoiDto(name = "Near", cat = "fuel", lat = 40.001, lon = -75.0), 40.0, -75.0, 250)
        val far = DesktopCache.toEntity(src, PoiDto(name = "Far", cat = "peds_er", lat = 40.5, lon = -75.0, scope = "far"), 40.0, -75.0, 250)
        val other = DesktopCache.toEntity("desktop:other", PoiDto(name = "Other", cat = "fuel", lat = 40.0, lon = -75.0), 40.0, -75.0, 999)
        val v = Mirror.prefill(desk, snaps, listOf(far, other, near))
        assertTrue(v.stale); assertEquals(300L, v.cachedAt)
        assertEquals(setOf("location", "trip", "pois", "emergency", "devices"), v.cached)
        assertEquals("Testville", v.location?.place); assertEquals("Scout", v.trip?.rank); assertEquals(2, v.trip?.stopsToday)
        assertEquals("Test General", v.emergency?.hospital?.name); assertEquals("pi", v.devices.single().device)
        assertEquals(listOf("Near", "Far"), v.pois.map { it.name })       // this desktop's rows only, nearest first
        assertEquals(0L, v.fetched); assertEquals("", v.error)
    }

    @Test fun nothingCachedIsNotStale() {
        val v = Mirror.prefill(desk, mapOf(Mirror.TRIP to snap(Mirror.TRIP, "{broken", 5)), emptyList())
        assertFalse(v.stale); assertEquals(0L, v.cachedAt); assertTrue(v.cached.isEmpty()); assertNull(v.trip)
    }

    @Test fun settleKeepsTheViewStaleUntilEverythingIsLive() {
        val pre = Mirror.prefill(desk, mapOf(Mirror.TRIP to snap(Mirror.TRIP, enc(TripDto(Trip(rank = "Scout"))), 300)), emptyList())
        // the desktop answered, but not for the trip: still stale, still "as of" the saved time
        val partial = Mirror.settle(pre, ok = true, error = "", now = 1000)
        assertTrue(partial.stale); assertEquals(300L, partial.cachedAt); assertEquals(1000L, partial.fetched)
        // everything live
        val live = Mirror.settle(pre.copy(cached = emptySet()), ok = true, error = "", now = 1000)
        assertFalse(live.stale); assertEquals(1000L, live.cachedAt)
        // then the desktop goes away: stale "as of" the last live answer
        val gone = Mirror.settle(live, ok = false, error = "unreachable", now = 2000)
        assertTrue(gone.stale); assertEquals(1000L, gone.cachedAt); assertEquals("unreachable", gone.error)
        // never answered and nothing cached: an error, nothing stale to show
        val none = Mirror.settle(Mirror.prefill(desk, emptyMap(), emptyList()), ok = false, error = "unreachable", now = 2000)
        assertFalse(none.stale)
    }

}
