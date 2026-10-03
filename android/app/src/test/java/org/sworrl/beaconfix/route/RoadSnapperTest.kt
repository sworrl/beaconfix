package org.sworrl.beaconfix.route

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.collector.MotionMode
import org.sworrl.beaconfix.data.db.FixEntity

class RoadSnapperTest {

    // offline: unit tests never reach the OSRM server (vehicular stretches fall back to the local smoothing)
    private val snapper = RoadSnapper(fetch = { OsrmReply(0, null) }, now = System::currentTimeMillis, pause = {})

    @Test
    fun vehicularSpeedIdentifiesInVehicleMode() = runBlocking {
        val now = 1700000000000L
        // Travel 500 meters in 20 seconds = 25 m/s = 90 km/h (highway driving)
        val fixes = listOf(
            FixEntity(id = 1, time = now, lat = 39.7500, lon = -104.9900, acc = 5.0, source = "phone-gps"),
            FixEntity(id = 2, time = now + 10_000, lat = 39.7520, lon = -104.9900, acc = 5.0, source = "phone-gps"),
            FixEntity(id = 3, time = now + 20_000, lat = 39.7540, lon = -104.9900, acc = 5.0, source = "phone-gps")
        )

        val segments = snapper.snapTrack(fixes)
        assertEquals(1, segments.size)
        assertEquals(MotionMode.IN_VEHICLE, segments[0].mode)
        assertTrue(segments[0].points.size >= 2)
    }

    @Test
    fun walkingSpeedPreservesOnFootModeWithoutForcedCenterline() = runBlocking {
        val now = 1700000000000L
        // Walking 10 meters in 8 seconds = 1.25 m/s = 4.5 km/h
        val fixes = listOf(
            FixEntity(id = 1, time = now, lat = 39.75000, lon = -104.99000, acc = 3.0, source = "phone-gps"),
            FixEntity(id = 2, time = now + 8_000, lat = 39.75009, lon = -104.99000, acc = 3.0, source = "phone-gps"),
            FixEntity(id = 3, time = now + 16_000, lat = 39.75018, lon = -104.99000, acc = 3.0, source = "phone-gps")
        )

        val segments = snapper.snapTrack(fixes)
        assertEquals(1, segments.size)
        assertEquals(MotionMode.ON_FOOT, segments[0].mode)
        assertEquals(3, segments[0].points.size)
        // Foot travel preserves the exact lat/lon without snapping
        assertEquals(39.75000, segments[0].points[0].lat, 0.00001)
        assertEquals(39.75018, segments[0].points[2].lat, 0.00001)
    }

    @Test
    fun stationaryDwellingCollapsesToCentroidWithoutSpiderweb() = runBlocking {
        val now = 1700000000000L
        // Stationary jitter: 5 fixes drifting within 4 meters over 3 minutes
        val fixes = listOf(
            FixEntity(id = 1, time = now, lat = 39.750000, lon = -104.990000, acc = 3.0, source = "phone-stationary"),
            FixEntity(id = 2, time = now + 40_000, lat = 39.750020, lon = -104.990010, acc = 4.0, source = "phone-stationary"),
            FixEntity(id = 3, time = now + 80_000, lat = 39.750010, lon = -104.989990, acc = 3.5, source = "phone-stationary"),
            FixEntity(id = 4, time = now + 120_000, lat = 39.749990, lon = -104.990005, acc = 5.0, source = "phone-stationary"),
            FixEntity(id = 5, time = now + 160_000, lat = 39.750005, lon = -104.990002, acc = 3.0, source = "phone-stationary")
        )

        val segments = snapper.snapTrack(fixes)
        assertEquals(1, segments.size)
        assertEquals(MotionMode.STATIONARY, segments[0].mode)
        assertEquals(1, segments[0].points.size)
        assertEquals(39.75000, segments[0].points[0].lat, 0.00005)
        assertEquals(-104.99000, segments[0].points[0].lon, 0.00005)
    }

    @Test
    fun isolatedMultipathSpikeJumpFiltered() = runBlocking {
        val now = 1700000000000L
        // Point 2 jumps 60m out and returns to original line at point 3 (within 10m of point 1)
        val fixes = listOf(
            FixEntity(id = 1, time = now, lat = 39.75000, lon = -104.99000, acc = 4.0, source = "phone-foot"),
            FixEntity(id = 2, time = now + 6_000, lat = 39.75060, lon = -104.99000, acc = 12.0, source = "phone-foot"), // 66m jump
            FixEntity(id = 3, time = now + 12_000, lat = 39.75008, lon = -104.99000, acc = 4.0, source = "phone-foot"), // 9m from pt 1
            FixEntity(id = 4, time = now + 18_000, lat = 39.75016, lon = -104.99000, acc = 4.0, source = "phone-foot")
        )

        val segments = snapper.snapTrack(fixes)
        assertEquals(1, segments.size)
        assertEquals(MotionMode.ON_FOOT, segments[0].mode)
        // Point 2 spike jump was discarded
        assertEquals(3, segments[0].points.size)
        assertEquals(39.75000, segments[0].points[0].lat, 0.00001)
        assertEquals(39.75008, segments[0].points[1].lat, 0.00001)
        assertEquals(39.75016, segments[0].points[2].lat, 0.00001)
    }
}
