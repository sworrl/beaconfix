package org.sworrl.beaconfix.poi

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.db.PoiDao
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.data.db.SnapshotDao
import org.sworrl.beaconfix.data.db.SnapshotEntity

class PhonePlacesPolicyTest {
    private class FakePois : PoiDao {
        val rows = MutableStateFlow<List<PoiEntity>>(emptyList())
        override fun all(): Flow<List<PoiEntity>> = rows
        override suspend fun allNow() = rows.value
        override suspend fun upsertAll(rows: List<PoiEntity>) {
            val m = LinkedHashMap<Pair<String, String>, PoiEntity>(); this.rows.value.forEach { m[it.source to it.key] = it }
            rows.forEach { m[it.source to it.key] = it }; this.rows.value = m.values.toList()
        }
        override suspend fun deleteScope(source: String, scope: String) { rows.value = rows.value.filterNot { it.source == source && it.scope == scope } }
        override suspend fun deleteSource(source: String) { rows.value = rows.value.filterNot { it.source == source } }
    }
    private class FakeSnaps : SnapshotDao {
        val m = LinkedHashMap<Pair<String, String>, SnapshotEntity>()
        override suspend fun put(s: SnapshotEntity) { m[s.source to s.kind] = s }
        override suspend fun get(source: String, kind: String) = m[source to kind]
        override fun newest(kind: String): Flow<SnapshotEntity?> = MutableStateFlow(m.values.filter { it.kind == kind }.maxByOrNull { it.fetchedAt })
        override suspend fun newestNow(kind: String) = m.values.filter { it.kind == kind }.maxByOrNull { it.fetchedAt }
    }

    private val sample: List<OsmElement> by lazy { OverpassClient.parse(200, javaClass.classLoader!!.getResource("overpass_sample.json")!!.readText()).elements }

    /** A fake Overpass: counts requests, takes [durationMs] of fake time, answers with [answer]. */
    private inner class Fake(var answer: (String) -> OverpassResult = { OverpassResult(true, sample) }) : OverpassSource {
        val requests = ArrayList<Pair<String, Long>>()        // kind, start time
        var durationMs = 2_000L
        override suspend fun query(query: String, timeoutS: Int): OverpassResult {
            requests += (if (query.contains("emergency:paediatric")) "peds" else "help") to now
            now += durationMs
            return answer(query)
        }
    }

    private var now = 1_800_000_000_000L
    private val pois = FakePois()
    private val snaps = FakeSnaps()
    private val cache = DesktopCache(pois, snaps)
    private var cond = PhonePlacesPolicy.Conditions()
    private val fake = Fake()
    private val pauses = ArrayList<Long>()
    private val places = OverpassPhonePlaces(fake, cache, { _, _ -> cond }, { now }, { ms -> pauses += ms; now += ms })

    // the fixture's origin, so the sample's places are at their real distances
    private val lat = 39.6350
    private val lon = -79.9550
    private fun north(km: Double) = lat + km / 111.32

    private val fail = { _: String -> OverpassResult(false, error = "HTTP 429 (too many requests)", http = 429) }

    @Test fun noRequestWithinTenMinutesOfAFailure() = runBlocking {
        fake.answer = fail
        val first = places.refreshAround(lat, lon)
        assertEquals(1, fake.requests.size)                  // the help request failed, so the pediatric one never went out
        assertFalse(first.ok); assertEquals(PhonePlacesPolicy.NOTE_BUSY, first.note)
        assertEquals(now + 0, places.busyUntil - PhonePlacesPolicy.BACKOFF_MS)

        fake.answer = { OverpassResult(true, sample) }
        now += 9 * 60_000L
        val again = places.refreshAround(lat, lon)
        val forced = places.refreshAround(lat, lon, force = true)   // force skips freshness, never the back-off
        assertEquals(1, fake.requests.size)
        assertTrue(again.skipped); assertTrue(forced.skipped); assertEquals(PhonePlacesPolicy.NOTE_BUSY, forced.note)

        now += 2 * 60_000L
        val later = places.refreshAround(lat, lon)
        assertTrue(later.ok)
        assertEquals(listOf("help", "help", "peds"), fake.requests.map { it.first })
        assertEquals(0L, places.busyUntil)
    }

    @Test fun failuresInARowBackOffLonger() = runBlocking {
        assertEquals(listOf(10L, 20L, 40L, 80L, 160L, 240L, 240L), (1..7).map { PhonePlacesPolicy.backoffMs(it) / 60_000L })
        fake.answer = fail
        places.refreshAround(lat, lon)
        assertEquals(10 * 60_000L, places.busyUntil - now)
        now = places.busyUntil + 1
        places.refreshAround(lat, lon)
        assertEquals(20 * 60_000L, places.busyUntil - now)
        now = places.busyUntil + 1
        places.refreshAround(lat, lon)
        assertEquals(40 * 60_000L, places.busyUntil - now)
        assertEquals(3, fake.requests.size)                  // one (failed) help request per attempt, none while backing off
        // a success resets the count: the next failure backs off 10 minutes again
        fake.answer = { OverpassResult(true, sample) }
        now = places.busyUntil + 1
        assertTrue(places.refreshAround(lat, lon).ok)
        fake.answer = fail
        now += 25 * 3_600_000L
        places.refreshAround(lat, lon)
        assertEquals(10 * 60_000L, places.busyUntil - now)
    }

    @Test fun freshDataIsSkippedAndMovesTriggerTheRightSearches() = runBlocking {
        val r = places.refreshAround(lat, lon)
        assertTrue(r.ok); assertEquals(12, r.count)            // 6 help rows + 6 pediatric rows
        assertEquals(listOf("help", "peds"), fake.requests.map { it.first })

        now += 3_600_000L
        val same = places.refreshAround(north(1.0), lon)       // 1 km, 1 h later
        assertTrue(same.skipped); assertEquals(PhonePlacesPolicy.NOTE_FRESH, same.note)
        assertEquals(2, fake.requests.size)

        now += 60_000L
        places.refreshAround(north(6.0), lon)                  // > 5 km: help only
        assertEquals(listOf("help", "peds", "help"), fake.requests.map { it.first })

        now += 60_000L
        places.refreshAround(north(50.0), lon)                 // > 40 km: both
        assertEquals(listOf("help", "peds", "help", "help", "peds"), fake.requests.map { it.first })

        now += 25 * 3_600_000L                                 // a day later, same place: help again, pediatric still fresh
        places.refreshAround(north(50.0), lon)
        assertEquals("help", fake.requests.last().first); assertEquals(6, fake.requests.size)

        places.refreshAround(north(50.0), lon, pediatric = true, force = true)
        assertEquals(listOf("help", "peds"), fake.requests.takeLast(2).map { it.first })
    }

    @Test fun pediatricWaitsFiveSecondsAfterTheHelpRequest() = runBlocking {
        fake.durationMs = 1_500L
        places.refreshAround(lat, lon)
        val (help, peds) = fake.requests
        assertTrue("gap ${peds.second - (help.second + fake.durationMs)} ms", peds.second - (help.second + fake.durationMs) >= PhonePlacesPolicy.GAP_MS)
        // a forced search right after also keeps the gap to the previous request
        places.refreshAround(lat, lon, force = true)
        assertTrue(fake.requests[2].second - (peds.second + fake.durationMs) >= PhonePlacesPolicy.GAP_MS)
    }

    @Test fun gatesStopEveryRequest() = runBlocking {
        for (c in listOf(
            PhonePlacesPolicy.Conditions(enabled = false),
            PhonePlacesPolicy.Conditions(metered = true, allowMetered = false),
            PhonePlacesPolicy.Conditions(simOffline = true),
            PhonePlacesPolicy.Conditions(online = false),
            PhonePlacesPolicy.Conditions(accuracyM = 6_000.0),
        )) {
            cond = c
            val r = places.refreshAround(lat, lon, force = true)
            assertTrue("$c", r.skipped); assertFalse("$c", r.ok); assertTrue("$c", r.note.isNotEmpty())
        }
        assertTrue(fake.requests.isEmpty())
        cond = PhonePlacesPolicy.Conditions(metered = true, allowMetered = true, accuracyM = 4_000.0)
        assertTrue(places.refreshAround(lat, lon).ok)
        assertEquals(2, fake.requests.size)
    }

    private fun desktopRow(key: String, scope: String, ageMs: Long, oLat: Double) =
        PoiEntity(source = "desktop:192.0.2.10:47822", key = key, scope = scope, cat = "health", lat = 40.0, lon = -75.0,
            fetchedAt = now - ageMs, originLat = oLat, originLon = lon)

    @Test fun freshDesktopDataNearbyMakesTheSearchUnnecessary() = runBlocking {
        pois.rows.value = listOf(desktopRow("way/1", "near", 3_600_000L, north(10.0)))
        places.refreshAround(lat, lon)
        assertEquals(listOf("peds"), fake.requests.map { it.first })           // the 3.7-style desktop answer has no wide search

        fake.requests.clear(); pois.rows.value = listOf(desktopRow("way/1", "near", 3_600_000L, north(10.0)), desktopRow("way/2", "far", 3_600_000L, north(10.0)))
        snaps.m.clear()
        val r = places.refreshAround(lat, lon)
        assertTrue(r.skipped); assertTrue(r.note, r.note.contains("desktop"))
        assertTrue(fake.requests.isEmpty())

        // old or far-away desktop data does not count
        pois.rows.value = listOf(desktopRow("way/1", "far", 25 * 3_600_000L, north(10.0)), desktopRow("way/2", "far", 3_600_000L, north(30.0)))
        now += 6_000L
        places.refreshAround(lat, lon)
        assertEquals(listOf("help", "peds"), fake.requests.map { it.first })
    }

    @Test fun aFailureKeepsEverythingCached() = runBlocking {
        assertTrue(places.refreshAround(lat, lon).ok)
        val before = pois.rows.value
        val snapBefore = snaps.m.values.single()
        fake.answer = fail
        now += 2 * 86_400_000L
        val r = places.refreshAround(lat, lon)
        assertFalse(r.ok); assertEquals(PhonePlacesPolicy.NOTE_BUSY, r.note)
        assertEquals(before, pois.rows.value)
        val snap = snaps.m.values.single()
        assertEquals(snapBefore.fetchedAt, snap.fetchedAt)                    // the footer's age stays the data's age
        assertEquals(PhonePlacesPolicy.NOTE_BUSY, cache.decode<PhonePlacesState>(snap)!!.note)
    }

    @Test fun rowsAndSnapshotAreStored() = runBlocking {
        val t0 = now
        assertTrue(places.refreshAround(lat, lon).ok)
        val rows = pois.rows.value
        assertTrue(rows.all { it.source == DesktopCache.PHONE })
        assertEquals(rows.size, rows.map { it.key }.toSet().size)             // one row per place
        // in both answers (WVU, Ruby, the kids' urgent care, the ER with a pediatrics dept.): kept once, in the far scope
        for (k in listOf("way/1135527599", "way/463483594", "node/9000000006", "node/9000000005")) assertEquals(k, "far", rows.single { it.key == k }.scope)
        assertEquals(setOf("node/9000000101", "node/9000000102"), rows.filter { it.scope == "near" }.map { it.key }.toSet())
        assertEquals(setOf("way/329264979", "node/9000000010"), rows.filter { it.scope == "far" && it.cat == "peds_er" && it.key != "way/1135527599" }.map { it.key }.toSet())

        val snap = snaps.m[DesktopCache.PHONE to OverpassPhonePlaces.KIND]
        assertNotNull(snap)
        val st = cache.decode<PhonePlacesState>(snap)!!
        assertEquals(lat, st.origin!!.lat, 1e-9); assertEquals("phone", st.origin!!.source); assertTrue(st.time.isNotEmpty()); assertEquals("", st.note)
        assertEquals(6, st.help!!.count); assertEquals(6, st.peds!!.count); assertEquals(150, st.peds!!.radiusKm)
        assertTrue(st.help!!.at >= t0 && st.peds!!.at >= st.help!!.at)
        assertEquals(st.peds!!.at, snap!!.fetchedAt)

        // a second help search that finds the same places does not pull them out of the far scope
        now += 60_000L
        places.refreshAround(north(6.0), lon)
        assertEquals("far", pois.rows.value.single { it.key == "way/1135527599" }.scope)
    }

    @Test fun aNewRadiusRerunsThePediatricSearch() = runBlocking {
        places.refreshAround(lat, lon)
        now += 60_000L
        cond = cond.copy(pedsRadiusKm = 300)
        places.refreshAround(lat, lon)
        assertEquals(listOf("help", "peds", "peds"), fake.requests.map { it.first })
        now += 60_000L
        assertTrue(places.refreshAround(lat, lon, pediatric = false).skipped)
        assertEquals(3, fake.requests.size)
    }

    @Test fun planIsPure() {
        val c = PhonePlacesPolicy.Conditions()
        val none = PhonePlacesPolicy.DesktopCoverage()
        assertEquals(PhonePlacesPolicy.Plan(true, true, ""), PhonePlacesPolicy.plan(now, lat, lon, false, true, c, null, 0, none))
        assertEquals(PhonePlacesPolicy.Plan(true, false, ""), PhonePlacesPolicy.plan(now, lat, lon, false, false, c, null, 0, none))
        val st = PhonePlacesState(help = SearchMark(lat, lon, now - 1000), peds = SearchMark(lat, lon, now - 1000, radiusKm = 150))
        assertEquals(PhonePlacesPolicy.NOTE_FRESH, PhonePlacesPolicy.plan(now, lat, lon, false, true, c, st, 0, none).note)
        assertEquals(PhonePlacesPolicy.Plan(true, true, ""), PhonePlacesPolicy.plan(now, lat, lon, true, true, c, st, 0, none))
        assertEquals(PhonePlacesPolicy.NOTE_BUSY, PhonePlacesPolicy.plan(now, lat, lon, true, true, c, st, now + 1, none).note)
        // a clock that went backwards does not make old data look fresh forever
        assertTrue(PhonePlacesPolicy.helpDue(now, lat, lon, SearchMark(lat, lon, now + 3_600_000L)))
    }
}
