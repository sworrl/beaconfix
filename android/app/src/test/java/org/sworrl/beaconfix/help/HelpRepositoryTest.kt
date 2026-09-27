package org.sworrl.beaconfix.help

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.data.db.SnapshotEntity
import org.sworrl.beaconfix.estimate.Geo
import org.sworrl.beaconfix.poi.PhonePlaces
import org.sworrl.beaconfix.poi.PhonePlacesResult

class HelpRepositoryTest {
    companion object {
        const val NOW = 1_790_000_000_000L
        const val DESK = "desktop:192.0.2.10:47822"
        fun res(name: String): String = HelpRepositoryTest::class.java.classLoader!!.getResource(name)!!.readText()
    }

    private class Fake : HelpInputs {
        var t = NOW
        override fun now() = t
        var desktopAnswers = true
        var desktopThrows: Exception? = null
        var desktopCalls = 0
        override suspend fun refreshDesktops(): Boolean { desktopCalls++; desktopThrows?.let { throw it }; return desktopAnswers }
        var emergencyJson: String? = null
        var emergencyAt = NOW - 60_000L
        override suspend fun emergency() = emergencyJson?.let { SnapshotEntity(DESK, "emergency", it, 40.0, -75.0, emergencyAt) }
        var helloJson: String? = null
        override suspend fun hello() = helloJson?.let { SnapshotEntity(DESK, "hello", it, fetchedAt = NOW) }
        override suspend fun phoneSearch(): SnapshotEntity? = null
        var rows: List<PoiEntity> = emptyList()
        var placesThrows: Exception? = null
        override suspend fun places(): List<PoiEntity> { placesThrows?.let { throw it }; return rows }
        var phoneFixV: FixEntity? = null
        override suspend fun phoneFix() = phoneFixV
        var desk: FixEntity? = FixEntity(time = NOW - 60_000L, lat = 40.0, lon = -75.0, acc = 10.0, source = "desktop")
        override suspend fun desktopFix() = desk
        var phonePlacesEnabled = true
        override suspend fun phonePlacesOn() = phonePlacesEnabled
        override suspend fun pedsRadiusKm() = 150
        override suspend fun countryCode() = "US"
        val addressCalls = ArrayList<Boolean>()
        override suspend fun address(lat: Double, lon: Double, allowNetwork: Boolean): AddressLine { addressCalls += allowNetwork; return AddressLine("1 Test St", "Testville", "Test County", "PA") }
        val searches = ArrayList<Pair<Double, Double>>()
        var onSearch: () -> Unit = {}
        override val phone = object : PhonePlaces {
            override suspend fun refreshAround(lat: Double, lon: Double, force: Boolean, pediatric: Boolean): PhonePlacesResult { searches += lat to lon; onSearch(); return PhonePlacesResult(true, 1) }
            override val busyUntil = 0L
        }
        var touched = 0
        override fun touchWidgets() { touched++ }
    }

    private fun row(key: String, cat: String, name: String, lat: Double, lon: Double, emergency: Boolean = false, peds: Int = 0, source: String = DESK,
                    fetchedAt: Long = NOW - 60_000L, oLat: Double = 40.0, oLon: Double = -75.0, scope: String = "near") =
        PoiEntity(source = source, key = key, scope = scope, cat = cat, name = name, lat = lat, lon = lon, emergency = emergency, er = if (emergency) "yes" else "",
            peds = peds, fetchedAt = fetchedAt, originLat = oLat, originLon = oLon)

    private val general = row("way/9100000003", "health", "Test General Hospital", 40.020, -75.010, emergency = true)
    private val childrens = row("way/9100000020", "health", "Test Children's Hospital", 40.060, -75.000)
    private val police = row("node/9100000001", "police", "Test Township Police", 40.010, -75.000)

    @Test fun desktop37FallsBackToNames() = runBlocking {
        val io = Fake().apply { emergencyJson = res("emergency_v37.json"); rows = listOf(general, childrens, police) }
        val s = HelpRepository(io).refresh()
        assertFalse(s.pediatricSupported)
        val peds = s.first(HelpKind.PEDS_ER)!!
        assertEquals("Test Children's Hospital", peds.name)
        assertEquals(2, peds.tier)
        assertEquals("Test General Hospital", s.first(HelpKind.ER)!!.name)
        assertEquals("911", s.number); assertEquals("US", s.countryCode); assertEquals("1-800-222-1222", s.poisonControl)
        assertEquals(Origin.RV, s.origin)
        assertEquals(DESK, s.source)
        assertTrue(s.pedsNote.contains("name"))
        // the 3.7 emergency answer alone (no places list) still yields help
        val only = HelpRepository(Fake().apply { emergencyJson = res("emergency_v37.json") }).refresh()
        assertEquals("Test General Hospital", only.first(HelpKind.ER)!!.name)
        assertEquals("Test Walk-In Clinic", only.first(HelpKind.URGENT)!!.name)
        assertTrue(only.first(HelpKind.URGENT)!!.notEr)
        assertEquals(1, io.touched)
    }

    @Test fun desktop38PediatricAnswer() = runBlocking {
        val io = Fake().apply { emergencyJson = res("emergency_peds.json"); rows = listOf(general, police) }
        val s = HelpRepository(io).refresh()
        assertTrue(s.pediatricSupported)
        assertEquals(150, s.pedsSearchKm)
        val peds = s.first(HelpKind.PEDS_ER)!!
        assertEquals("Test Children's Hospital", peds.name); assertEquals(1, peds.tier); assertEquals("way/9100000010", peds.osmKey)
        assertEquals("Test Valley Medical Center", s.first(HelpKind.PEDS_CLOSER)!!.name)
        val urg = s.first(HelpKind.PEDS_URGENT)!!
        assertTrue(urg.notEr); assertEquals(4, urg.tier)
        assertEquals("Test General Hospital", s.first(HelpKind.ER)!!.name)
        assertFalse(s.stale)
        assertEquals("", s.pedsNote)
        assertEquals(AddressLine("1 Test St", "Testville", "Test County", "PA"), s.address)
    }

    @Test fun anUnfinishedPediatricSearchIsNotNoneMapped() = runBlocking {
        // the desktop's pediatric search never completed (Overpass busy): no pick, no search time
        val root = kotlinx.serialization.json.Json.parseToJsonElement(res("emergency_peds.json")).let { it as kotlinx.serialization.json.JsonObject }.toMutableMap()
        root["pediatric"] = kotlinx.serialization.json.JsonNull; root["pediatricCloser"] = kotlinx.serialization.json.JsonNull
        root["pediatricUrgent"] = kotlinx.serialization.json.JsonNull
        root["pediatricNote"] = kotlinx.serialization.json.JsonPrimitive("Overpass busy — will retry")
        root["pediatricTime"] = kotlinx.serialization.json.JsonPrimitive("")
        val s = HelpRepository(Fake().apply { emergencyJson = kotlinx.serialization.json.JsonObject(root).toString(); rows = listOf(general, police) }).refresh()
        assertNull(s.first(HelpKind.PEDS_ER))
        assertEquals("Test General Hospital", s.first(HelpKind.ER)!!.name)
        assertFalse(s.pedsNote, s.pedsNote.startsWith("No pediatric ER mapped"))
        assertTrue(s.pedsNote, s.pedsNote.startsWith("Children's ER search didn't finish — go to the nearest ER"))
        assertTrue(s.pedsNote, s.pedsNote.contains("Overpass busy"))
        // a search that finished and found nothing may say so
        root["pediatricNote"] = kotlinx.serialization.json.JsonPrimitive("No pediatric ER mapped within 150 km — go to the nearest ER")
        root["pediatricTime"] = kotlinx.serialization.json.JsonPrimitive("2026-09-27T09:00:00")
        val done = HelpRepository(Fake().apply { emergencyJson = kotlinx.serialization.json.JsonObject(root).toString(); rows = listOf(general, police) }).refresh()
        assertEquals("No pediatric ER mapped within 150 km — go to the nearest ER", done.pedsNote)
    }

    @Test fun failuresKeepThePreviousAnswer() = runBlocking {
        val io = Fake().apply { emergencyJson = res("emergency_peds.json"); rows = listOf(general, police) }
        val repo = HelpRepository(io)
        val first = repo.refresh()
        assertTrue(first.places.isNotEmpty())

        io.desktopThrows = RuntimeException("connection refused"); io.t += 60_000L
        val second = repo.refresh()
        assertEquals(first.places, second.places); assertEquals(first.fetchedAt, second.fetchedAt)
        assertTrue(second.lastError, second.lastError.contains("connection refused"))

        io.placesThrows = IllegalStateException("database closed")
        val third = repo.refresh()
        assertEquals(first.places, third.places); assertEquals(first.fetchedAt, third.fetchedAt)
        assertEquals(first.places, repo.snapshot.value.places)
        assertTrue(third.lastError.contains("database closed"))
    }

    @Test fun aFreshPhoneFixFarFromTheRvIsTheOrigin() = runBlocking {
        val io = Fake().apply {
            emergencyJson = res("emergency_peds.json"); rows = listOf(general, police)
            phoneFixV = FixEntity(time = NOW - 60_000L, lat = 40.45, lon = -75.0, acc = 15.0, source = "phone-gps")
        }
        val s = HelpRepository(io).refresh()
        assertEquals(Origin.PHONE, s.origin)
        assertEquals(40.45, s.originLat, 0.0)
        val er = s.first(HelpKind.ER)!!
        assertEquals(Geo.distanceM(40.45, -75.0, er.lat, er.lon), er.distM, 1.0)
        assertEquals("from up here the nearest confirmed ER is the one with the pediatrics dept.", "Test Valley Medical Center", er.name)
        assertTrue(er.distM > 30_000)
        assertEquals(DriveEstimate.seconds(er.distM), er.driveS)
        // the nearest children's ER from up here is the one 17 km north of the RV, not the "closer" one next to it
        assertEquals("Test Children's Hospital", s.first(HelpKind.PEDS_ER)!!.name)
        assertNull(s.first(HelpKind.PEDS_CLOSER))
    }

    @Test fun phoneSearchOnlyWhenNoDesktopAnswersAndOnlyFromRefresh() = runBlocking {
        val old = NOW - 3 * 86_400_000L
        val io = Fake().apply {
            desktopAnswers = false; emergencyJson = res("emergency_v37.json"); emergencyAt = old
            rows = listOf(general.copy(fetchedAt = old))
            phoneFixV = FixEntity(time = NOW - 30_000L, lat = 40.001, lon = -75.001, acc = 8.0, source = "phone-gps")
        }
        val repo = HelpRepository(io)
        repo.refreshIfStale()
        assertTrue("refreshIfStale never searches from the phone", io.searches.isEmpty())
        assertTrue(repo.snapshot.value.stale)
        assertFalse("background refreshes never use the network geocoders", io.addressCalls.any { it })

        io.onSearch = { io.rows = io.rows + row("way/9100000030", "peds_er", "Test Kids Hospital", 40.2, -75.0, emergency = true, peds = 1, source = "phone", fetchedAt = io.t, oLat = 40.001, oLon = -75.001, scope = "far") }
        io.t += 120_000L
        val s = repo.refresh()
        assertEquals(1, io.searches.size)
        assertEquals("Test Kids Hospital", s.first(HelpKind.PEDS_ER)!!.name)
        assertEquals(150, s.pedsSearchKm)

        // a desktop that answers means no phone search
        io.desktopAnswers = true
        repo.refresh(force = true)
        assertEquals(1, io.searches.size)
    }

    @Test fun phoneSearchRespectsTheSwitch() = runBlocking {
        val io = Fake().apply { desktopAnswers = false; phonePlacesEnabled = false; phoneFixV = FixEntity(time = NOW, lat = 40.0, lon = -75.0, acc = 5.0, source = "phone-gps") }
        HelpRepository(io).refresh(force = true)
        assertTrue(io.searches.isEmpty())
    }

    @Test fun nothingKnownIsAnEmptyAnswerNotACrash() = runBlocking {
        val io = Fake().apply { desk = null; desktopAnswers = false; phonePlacesEnabled = false }
        val s = HelpRepository(io).refresh()
        assertTrue(s.places.isEmpty()); assertEquals(Origin.NONE, s.origin); assertEquals("", s.lastError)
        assertEquals("911", s.number)
        assertNotNull(s.poisonControl)
    }

    @Test fun helloFeatureMarksSupportEvenWithAnOldEmergencySnapshot() = runBlocking {
        val io = Fake().apply { emergencyJson = res("emergency_v37.json"); helloJson = """{"name":"desktop","features":["sync","pediatric"]}"""; rows = listOf(general, childrens) }
        val s = HelpRepository(io).refresh()
        assertTrue(s.pediatricSupported)
        assertNull("no name-based guess on a 3.8 desktop", s.first(HelpKind.PEDS_ER))
    }

    @Test fun isoTimes() {
        assertEquals(0L, HelpRepository.isoMs("1970-01-01T00:00:00Z"))
        assertEquals(3_600_000L, HelpRepository.isoMs("1970-01-01T02:00:00+01:00"))
        assertNotNull(HelpRepository.isoMs("2026-09-27T09:00:00"))
        assertNull(HelpRepository.isoMs("")); assertNull(HelpRepository.isoMs("yesterday"))
    }
}
