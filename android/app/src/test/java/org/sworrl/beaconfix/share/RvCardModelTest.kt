package org.sworrl.beaconfix.share

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.api.DevicesPositions
import org.sworrl.beaconfix.data.api.LinkedDevice
import org.sworrl.beaconfix.data.api.LocationDto
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.help.AddressLine
import org.sworrl.beaconfix.help.HelpSnapshot

/** Synthetic positions around the placeholder origin 40.0, -75.0. */
class RvCardModelTest {
    private val now = 1_800_000_000_000L
    private fun phone(lat: Double = 40.0, lon: Double = -75.0, t: Long = now - 60_000) = FixEntity(time = t, lat = lat, lon = lon, acc = 10.0, source = "phone-gps")
    private fun desk(lat: Double, lon: Double, t: Long) = FixEntity(time = t, lat = lat, lon = lon, acc = 30.0, source = "desktop")
    private fun dev(name: String, kind: String, lat: Double, lon: Double, ageS: Double?, time: String = "") =
        LinkedDevice(device = name, kind = kind, lat = lat, lon = lon, acc = 15.0, time = time, ageS = ageS)

    @Test fun noDataMeansUnknown() {
        val s = RvCardModel.state(emptyList(), null, 0, phone(), "Test phone", now)
        assertNull(s.rv); assertNull(s.distM)
    }

    @Test fun distanceBearingAndAgeFromThePhone() {
        // RV ~1.2 km to the north-east (bearing 45°)
        val rv = RvCardModel.fromFix(desk(40.0 + 0.00764, -75.0 + 0.00997, now - 5 * 60_000))
        val s = RvCardModel.state(listOf(rv), null, 0, phone(), "Test phone", now)
        assertNotNull(s.rv)
        assertEquals(1200.0, s.distM!!, 60.0)
        assertEquals(45.0, s.brgDeg!!, 1.0)
        assertEquals(5 * 60_000L, s.ageMs)
        assertEquals(RvCardModel.Age(5, 'm'), RvCardModel.age(s.ageMs!!))
    }

    @Test fun theNewestSourceWins() {
        val oldFix = RvCardModel.fromFix(desk(40.1, -75.1, now - 3_600_000))
        val loc = RvCardModel.fromLocation(LocationDto(valid = true, lat = 40.2, lon = -75.2, accuracy = 20.0, ageS = 30.0), fetchedAt = now - 600_000)
        val pi = RvCardModel.fromDevices(DevicesPositions(listOf(dev("pi", "pi", 40.3, -75.3, ageS = 10.0), dev("Test phone 2", "android", 40.9, -75.9, 1.0))), fetchedAt = now - 120_000)
        assertEquals(1, pi.size)                               // only desktop / Pi entries are the RV
        val s = RvCardModel.state(listOf(oldFix, loc) + pi, null, 0, phone(), "Test phone", now)
        assertEquals(40.3, s.rv!!.lat, 1e-9)
        assertEquals(130_000L, s.ageMs)                        // saved 2 min ago, 10 s old then
        // without the Pi snapshot the desktop's saved /location beats an hour-old synced fix
        assertEquals(40.2, RvCardModel.state(listOf(oldFix, loc), null, 0, phone(), "", now).rv!!.lat, 1e-9)
    }

    @Test fun emptyAndInvalidPositionsAreIgnored() {
        assertNull(RvCardModel.fromFix(desk(0.0, 0.0, now)))
        assertNull(RvCardModel.fromLocation(LocationDto(valid = false, lat = 40.0, lon = -75.0), now))
        assertNull(RvCardModel.fromLocation(LocationDto(valid = true, lat = 40.0, lon = -75.0), now))    // no time, no age
        assertTrue(RvCardModel.fromDevices(DevicesPositions(listOf(dev("desktop", "desktop", 0.0, 0.0, 5.0))), now).isEmpty())
    }

    @Test fun isoTimesWithAndWithoutOffset() {
        assertEquals(1_700_000_000_000L, RvCardModel.timeOf("2023-11-14T22:13:20Z", null, 0))
        assertNotNull(RvCardModel.timeOf("2023-11-14T22:13:20", null, 0))
        assertNull(RvCardModel.timeOf("", null, 0))
        assertEquals(now - 5_000, RvCardModel.timeOf("garbage", 5.0, now))
    }

    @Test fun otherDevicesExcludeThisPhoneAndTheRv() {
        val devices = DevicesPositions(listOf(
            dev("Test phone", "android", 40.0001, -75.0, 5.0),            // this phone, as the desktop knows it
            dev("Test tablet", "android", 40.01, -75.0, 120.0),
            dev("laptop", "laptop", 40.001, -75.0, 30.0),
            dev("desktop", "desktop", 40.02, -75.0, 30.0),
        ))
        val s = RvCardModel.state(RvCardModel.fromDevices(devices, now), devices, now, phone(), "Test phone", now)
        assertEquals(listOf("laptop", "Test tablet"), s.devices.map { it.name })   // nearest first
        assertEquals(111.0, s.devices[0].distM!!, 2.0)
        assertEquals(40.02, s.rv!!.lat, 1e-9)
    }

    @Test fun withoutAPhoneFixThereIsNoDistance() {
        val rv = RvCardModel.fromFix(desk(40.01, -75.0, now - 60_000))
        val s = RvCardModel.state(listOf(rv), null, 0, desk(40.0, -75.0, now), "", now)   // a desktop fix is not "this phone"
        assertNotNull(s.rv); assertNull(s.distM); assertNull(s.brgDeg)
    }

    @Test fun ageBuckets() {
        assertNull(RvCardModel.age(30_000))
        assertEquals(RvCardModel.Age(2, 'm'), RvCardModel.age(150_000))
        assertEquals(RvCardModel.Age(3, 'h'), RvCardModel.age(3 * 3_600_000L + 5))
        assertEquals(RvCardModel.Age(4, 'd'), RvCardModel.age(4 * 86_400_000L))
    }

    @Test fun addressOnlyWhenHelpResolvedItForThisSpot() {
        val snap = HelpSnapshot(origin = "phone", originLat = 40.0, originLon = -75.0, address = AddressLine(line = "1 Test Street", locality = "Testville", state = "PA"))
        assertEquals("1 Test Street, Testville, PA", ShareViewModel.addressNear(snap, 40.001, -75.0))
        assertNull(ShareViewModel.addressNear(snap, 40.01, -75.0))                  // 1.1 km away
        assertNull(ShareViewModel.addressNear(snap.copy(origin = "none"), 40.0, -75.0))
        assertNull(ShareViewModel.addressNear(snap.copy(address = null), 40.0, -75.0))
    }
}
