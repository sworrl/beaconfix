package org.sworrl.beaconfix.trip

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.estimate.Geo
import java.time.LocalDate
import java.time.ZoneOffset

class StopsTest {
    private val utc = ZoneOffset.UTC
    /** 2026-09-27T08:00:00Z */
    private val t0 = 1_790_496_000_000L
    private val min = 60_000L
    /** degrees of latitude per metre */
    private val dLat = 1.0 / 111_195.0

    private fun fix(minute: Int, northM: Double, acc: Double = 10.0, source: String = "phone-gps", eastM: Double = 0.0) =
        FixEntity(time = t0 + minute * min, lat = 40.0 + northM * dLat, lon = -75.0 + eastM * dLat / Math.cos(Math.toRadians(40.0)), acc = acc, source = source)

    /** 10 min driving north at 60 km/h (1 km/min), 15 min parked (jitter ±30 m), 10 min driving, 5 min parked, 5 min driving. */
    private fun day(): List<FixEntity> {
        val out = ArrayList<FixEntity>()
        var m = 0; var north = 0.0
        repeat(10) { out += fix(m++, north); north += 1000.0 }
        val parkedAt = north
        repeat(15) { i -> out += fix(m++, parkedAt + (if (i % 2 == 0) 20.0 else -20.0), eastM = if (i % 3 == 0) 25.0 else -10.0) }
        north = parkedAt + 1000.0
        repeat(10) { out += fix(m++, north); north += 1000.0 }
        val shortAt = north
        repeat(5) { out += fix(m++, shortAt + 5.0 * it) }       // only 4 minutes: not a stop
        north = shortAt + 1000.0
        repeat(5) { out += fix(m++, north); north += 1000.0 }
        return out
    }

    @Test fun aPhoneStopIsTenMinutesWithin150m() {
        val path = Stops.clean(Stops.points(day()))
        val stops = Stops.detect(path, Stops.PHONE)
        assertEquals(1, stops.size)
        val s = stops.single()
        assertEquals(t0 + 10 * min, s.arrival)
        assertEquals(t0 + 24 * min, s.departure)
        assertEquals(14 * 60L, s.dwellS)
        assertEquals(40.0 + 10_000.0 * dLat, s.lat, 50 * dLat)
        assertEquals(10_000.0, s.legM, 100.0)      // the drive before it
        assertFalse(s.open)
        assertNull(s.elevM)
    }

    @Test fun coarseFixesAndSpeedJumpsAreSkipped() {
        val clean = day().toMutableList()
        // a 5 km-accurate fix and a 50 km spike in the middle of the stop
        clean.add(15, fix(15, 60_000.0).copy(time = t0 + 15 * min + 1000))
        clean.add(17, fix(16, 10_000.0, acc = 5000.0).copy(time = t0 + 16 * min + 1000))
        val path = Stops.clean(Stops.points(clean))
        assertTrue(path.none { it.acc > Stops.MAX_ACC_M })
        assertTrue(path.none { Geo.distanceM(40.0, -75.0, it.lat, it.lon) > 40_000 })
        val stops = Stops.detect(path, Stops.PHONE)
        assertEquals(1, stops.size)
        assertEquals(14 * 60L, stops.single().dwellS)
    }

    @Test fun aLoneBadFirstFixDoesNotPoisonTheRest() {
        val fixes = listOf(fix(0, 500_000.0)) + day().drop(1)
        val path = Stops.clean(Stops.points(fixes))
        assertTrue(path.first().lat < 40.5)
        assertEquals(1, Stops.detect(path, Stops.PHONE).size)
    }

    @Test fun theNewestStopMayStillBeOpen() {
        val parked = (0 until 20).map { fix(it, if (it % 2 == 0) 10.0 else -10.0) }
        val s = Stops.detect(Stops.clean(Stops.points(parked)), Stops.PHONE).single()
        assertTrue(s.open)
        assertEquals(19 * 60L, s.dwellS)
        assertEquals(0.0, s.legM, 0.0)
    }

    @Test fun stopsGroupByLocalDay() {
        val a = Stop(40.0, -75.0, t0, t0 + 3_600_000, 3600, 1000.0)
        val b = Stop(40.1, -75.0, t0 + 10 * 3_600_000, null, -1, 2000.0)
        val c = Stop(40.2, -75.0, t0 + 20 * 3_600_000, null, -1, 500.0)      // 2026-09-28T04:00Z
        val days = Stops.byDay(listOf(c, a, b), utc)
        assertEquals(listOf(LocalDate.of(2026, 9, 27), LocalDate.of(2026, 9, 28)), days.map { it.date })
        assertEquals(listOf(a, b), days[0].stops)
        assertEquals(3000.0, days[0].distanceM, 0.0)
        assertEquals(listOf(c), days[1].stops)
    }

    private val track = """{"track":[
        {"lat":40.0,"lon":-75.0,"acc":12,"source":"gps","place":"Test Park","time":"2026-09-27T08:00:00","departed":"2026-09-27T10:00:00","dwellSecs":7200,"legKm":0,"elev":250.5,"city":"Testville"},
        {"lat":40.5,"lon":-75.0,"acc":5000,"source":"ip","place":"Far away","time":"2026-09-27T11:00:00"},
        {"lat":40.3,"lon":-75.0,"acc":15,"source":"gps","place":"","time":"2026-09-27T12:30:00","departed":"2026-09-27T13:00:00","legKm":35.2,"city":"Testburg","region":"TS"},
        {"lat":40.4,"lon":-75.0,"acc":15,"source":"gps","place":"No time","time":""},
        {"lat":40.35,"lon":-75.0,"acc":8,"source":"gps","place":"Camp","time":"2026-09-27T15:00:00+00:00","elev":301}
    ],"count":5,"ts":"2026-09-27T15:05:00"}"""

    @Test fun theDesktopTrackIsAlreadyAListOfStops() {
        val stops = Stops.fromTrack(track, utc)
        assertEquals(listOf("Test Park", "Testburg, TS", "Camp"), stops.map { it.place })
        val (park, burg, camp) = stops
        assertEquals(t0, park.arrival); assertEquals(t0 + 2 * 3_600_000, park.departure); assertEquals(7200L, park.dwellS)
        assertEquals(250.5, park.elevM!!, 0.0); assertEquals(0.0, park.legM, 0.0); assertFalse(park.open)
        assertEquals(1800L, burg.dwellS); assertEquals(35_200.0, burg.legM, 1e-6); assertNull(burg.elevM)
        assertTrue(camp.open); assertEquals(-1L, camp.dwellS); assertNull(camp.departure)
        assertEquals(Geo.distanceM(40.3, -75.0, 40.35, -75.0), camp.legM, 1e-6)     // no legKm: straight line from the previous stop
        assertTrue(stops.all { it.source == Stops.TRACK })
        assertTrue(Stops.fromTrack("{broken", utc).isEmpty())
        assertTrue(Stops.fromTrack(null, utc).isEmpty())
    }

    @Test fun theJournalPrefersTrackThenDesktopThenPhone() {
        val phone = day()
        val desk = day().map { it.copy(source = "desktop", lon = it.lon + 0.5) }
        val j1 = Stops.journal(track, desk, phone, utc)
        assertEquals(Stops.TRACK, j1.source); assertEquals(3, j1.stops.size)
        assertEquals(listOf(250.5, null, 301.0), j1.path.map { it.ele })
        val j2 = Stops.journal("""{"track":[]}""", desk, phone, utc)
        assertEquals(Stops.DESKTOP, j2.source); assertEquals(1, j2.stops.size); assertEquals(-74.5, j2.stops.single().lon, 0.01)
        val j3 = Stops.journal(null, emptyList(), phone, utc)
        assertEquals(Stops.PHONE, j3.source); assertEquals(1, j3.stops.size)
        // positions but no stop anywhere: the source is still named and its path kept for the GPX export
        val moving = (0 until 5).map { fix(it, it * 1000.0) }
        val j4 = Stops.journal(null, emptyList(), moving, utc)
        assertEquals(Stops.PHONE, j4.source); assertTrue(j4.isEmpty); assertEquals(5, j4.path.size)
        assertEquals(Journal(), Stops.journal(null, emptyList(), emptyList(), utc))
    }

    @Test fun thePhonesDay() {
        val now = t0 + 60 * min
        val d = Stops.phoneDay(day() + fix(-24 * 60, 0.0), now, utc)     // yesterday's fix does not count
        assertEquals(1, d.stops)
        assertEquals(45, d.fixes)
        assertTrue("distance ${d.distanceM}", d.distanceM in 25_000.0..27_000.0)
    }
}
