package org.sworrl.beaconfix.data

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.api.EmergencyDto
import org.sworrl.beaconfix.data.api.PoiDto
import org.sworrl.beaconfix.data.db.PoiDao
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.data.db.SnapshotDao
import org.sworrl.beaconfix.data.db.SnapshotEntity

class DesktopCacheTest {
    /** In-memory PoiDao; `replace` is the interface's own default (the code Room wraps in a transaction). */
    private class FakePois : PoiDao {
        val rows = MutableStateFlow<List<PoiEntity>>(emptyList())
        val calls = ArrayList<String>()
        override fun all(): Flow<List<PoiEntity>> = rows
        override suspend fun allNow() = rows.value
        override suspend fun upsertAll(rows: List<PoiEntity>) {
            calls += "upsert ${rows.size}"
            val m = LinkedHashMap<Pair<String, String>, PoiEntity>(); this.rows.value.forEach { m[it.source to it.key] = it }
            rows.forEach { m[it.source to it.key] = it }; this.rows.value = m.values.toList()
        }
        override suspend fun deleteScope(source: String, scope: String) { calls += "delete $source $scope"; rows.value = rows.value.filterNot { it.source == source && it.scope == scope } }
        override suspend fun deleteSource(source: String) { calls += "deleteSource $source"; rows.value = rows.value.filterNot { it.source == source } }
    }
    private class FakeSnaps : SnapshotDao {
        val m = LinkedHashMap<Pair<String, String>, SnapshotEntity>()
        override suspend fun put(s: SnapshotEntity) { m[s.source to s.kind] = s }
        override suspend fun get(source: String, kind: String) = m[source to kind]
        override fun newest(kind: String): Flow<SnapshotEntity?> = MutableStateFlow(m.values.filter { it.kind == kind }.maxByOrNull { it.fetchedAt })
        override suspend fun newestNow(kind: String) = m.values.filter { it.kind == kind }.maxByOrNull { it.fetchedAt }
    }

    private fun dto(name: String, osm: String = "", scope: String = "near", cat: String = "health", lat: Double = 40.0, lon: Double = -75.0) =
        PoiDto(name = name, cat = cat, lat = lat, lon = lon, osm = osm, scope = scope)
    private fun row(source: String, key: String, fetchedAt: Long, name: String = key) =
        PoiEntity(source = source, key = key, cat = "health", name = name, lat = 40.0, lon = -75.0, fetchedAt = fetchedAt)

    @Test fun osmLinkBecomesAKey() {
        assertEquals("way/329264979", DesktopCache.osmKey("https://www.openstreetmap.org/way/329264979"))
        assertEquals("node/42", DesktopCache.osmKey("https://www.openstreetmap.org/node/42"))
        assertEquals("relation/8017287", DesktopCache.osmKey("https://osm.org/relation/8017287"))
    }

    @Test fun malformedLinkFallsBackToAPositionKey() {
        for (bad in listOf("https://www.openstreetmap.org//0", "https://www.openstreetmap.org/way/0", "https://www.openstreetmap.org/way/abc", "not a link", "", null))
            assertNull(bad, DesktopCache.osmKey(bad))
        assertEquals("ll:40.00000,-75.00000:health", DesktopCache.keyFor(dto("Test Hospital", osm = "https://www.openstreetmap.org//0")))
        assertEquals("ll:40.12346,-75.00001:police", DesktopCache.keyFor(dto("Test", cat = "police", lat = 40.123456, lon = -75.000011)))
    }

    @Test fun osmTypeAndIdWinOverTheLink() {
        val d = PoiDto(name = "Test", cat = "peds_er", osmType = "way", osmId = 1135527599L, osm = "https://www.openstreetmap.org/node/1")
        assertEquals("way/1135527599", DesktopCache.keyFor(d))
        assertEquals("relation/7", DesktopCache.keyFor(PoiDto(osmType = "r", osmId = 7)))
    }

    @Test fun everyFieldIsMapped() {
        val d = PoiDto(name = "Test Children's Hospital", label = "Pediatric ER", cat = "peds_er", group = "civic", icon = "i", color = "#ff5fa2", lat = 40.1, lon = -75.1,
            address = "1 Test St", detail = "ER on campus: Test General — call ahead", phone = "+1 555 0100", hours = "24/7", website = "https://example.org",
            osmType = "way", osmId = 9L, wheelchair = "yes", emergency = true, wifi = true, peds = 2, er = "yes", campusEr = "Test General", scope = "far",
            driveS = 600, driveM = 9000, driveEst = false)
        val e = DesktopCache.toEntity("desktop:192.0.2.1:47822", d, 40.0, -75.0, 1234L)
        assertEquals(PoiEntity(source = "desktop:192.0.2.1:47822", key = "way/9", scope = "far", cat = "peds_er", label = "Pediatric ER", grp = "civic", icon = "i", color = "#ff5fa2",
            name = "Test Children's Hospital", detail = "ER on campus: Test General — call ahead", address = "1 Test St", lat = 40.1, lon = -75.1, phone = "+1 555 0100", hours = "24/7",
            website = "https://example.org", wheelchair = "yes", emergency = true, wifi = true, peds = 2, er = "yes", campus = "Test General", driveS = 600, driveM = 9000,
            driveEst = false, fetchedAt = 1234L, originLat = 40.0, originLon = -75.0), e)
    }

    @Test fun emptyRowsAreANoOp() = runBlocking {
        val pois = FakePois(); val cache = DesktopCache(pois, FakeSnaps())
        pois.rows.value = listOf(row("desktop:a", "way/1", 1))
        cache.saveDesktopPois("a", emptyList(), 40.0, -75.0, 5)
        cache.savePhonePois("near", emptyList())
        assertTrue(pois.calls.toString(), pois.calls.isEmpty())
        assertEquals(1, cache.poisNow().size)
    }

    @Test fun desktopSaveReplacesOnlyItsOwnScope() = runBlocking {
        val pois = FakePois(); val cache = DesktopCache(pois, FakeSnaps())
        pois.rows.value = listOf(row("desktop:a", "way/1", 1).copy(scope = "far"), row("desktop:a", "way/2", 1), row("desktop:b", "way/3", 1), row("phone", "way/4", 1))
        cache.saveDesktopPois("a", listOf(dto("New", osm = "https://www.openstreetmap.org/way/5")), 40.0, -75.0, 9)
        val keys = pois.rows.value.map { it.source + " " + it.key }.toSet()
        assertEquals(setOf("desktop:a way/1", "desktop:a way/5", "desktop:b way/3", "phone way/4"), keys)
        // near + far in one answer: far is written first so a place in both ends up near
        pois.calls.clear()
        cache.saveDesktopPois("a", listOf(dto("N", osm = "https://www.openstreetmap.org/way/6"), dto("F", osm = "https://www.openstreetmap.org/way/6", scope = "far")), 40.0, -75.0, 10)
        assertEquals(listOf("delete desktop:a far", "upsert 1", "delete desktop:a near", "upsert 1"), pois.calls)
        assertEquals("near", pois.rows.value.single { it.key == "way/6" }.scope)
    }

    @Test fun phoneSaveUsesThePhoneSource() = runBlocking {
        val pois = FakePois(); val cache = DesktopCache(pois, FakeSnaps())
        cache.savePhonePois("far", listOf(row("whatever", "way/7", 3)))
        assertEquals(listOf("phone" to "far"), pois.rows.value.map { it.source to it.scope })
    }

    @Test fun dedupePrefersTheNewestThenTheDesktop() = runBlocking {
        val pois = FakePois(); val cache = DesktopCache(pois, FakeSnaps())
        pois.rows.value = listOf(
            row("desktop:a", "way/1", 100, "old desktop"), row("phone", "way/1", 200, "newer phone"),
            row("phone", "way/2", 300, "phone tie"), row("desktop:b", "way/2", 300, "desktop tie"),
            row("desktop:a", "way/3", 50, "only"),
        )
        val out = cache.pois().first().associate { it.key to it.name }
        assertEquals(mapOf("way/1" to "newer phone", "way/2" to "desktop tie", "way/3" to "only"), out)
        assertEquals(out, cache.poisNow().associate { it.key to it.name })
    }

    @Test fun snapshotsRoundTrip() = runBlocking {
        val cache = DesktopCache(FakePois(), FakeSnaps())
        cache.putSnapshot("desktop:a", "emergency", """{"number":"911","countryCode":"US","hospital":{"name":"Test General","lat":40.0,"lon":-75.0,"d":1200},"future":1}""", now = 5)
        cache.putSnapshot("phone", "emergency", "{broken", now = 7)
        assertEquals("phone", cache.snapshotNow("emergency")?.source)
        assertNull(cache.decode<EmergencyDto>(cache.snapshotNow("emergency")))
        val e = cache.decode<EmergencyDto>(cache.snapshotFrom("desktop:a", "emergency"))
        assertEquals("911", e?.number); assertEquals(1200.0, e?.hospital?.d ?: 0.0, 0.0); assertNull(e?.pediatric)
        assertNull(cache.decode<EmergencyDto>(null))
        assertEquals("911", cache.decode<EmergencyDto>(SnapshotEntity("x", "emergency", cache.encode(EmergencyDto(number = "911")), fetchedAt = 1))?.number)
    }
}
