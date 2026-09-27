package org.sworrl.beaconfix.notify

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.help.HelpKind
import org.sworrl.beaconfix.help.HelpPlace
import org.sworrl.beaconfix.help.HelpSnapshot
import org.sworrl.beaconfix.notify.HelpAlertRules.State
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.ZoneOffset

class HelpAlertRulesTest {
    private val zone: ZoneId = ZoneOffset.UTC
    private fun at(hour: Int, minute: Int = 0) = LocalDateTime.of(2026, 9, 27, hour, minute).toInstant(ZoneOffset.UTC).toEpochMilli()
    private val noon = at(12)
    // 0.1° of latitude ≈ 11.1 km
    private fun snap(lat: Double, stale: Boolean = false, places: Boolean = true) = HelpSnapshot(
        places = if (places) listOf(HelpPlace(HelpKind.ER, "Test General Hospital", lat + 0.05, -75.0, distM = 5_000.0)) else emptyList(),
        origin = "phone", originLat = lat, originLon = -75.0, fetchedAt = noon, stale = stale)
    private val home = State(40.0, -75.0, 0L)

    @Test fun firstRunOnlyRecordsTheOrigin() {
        val d = HelpAlertRules.decide(snap(40.0), null, enabled = true, now = noon, zone = zone)
        assertFalse(d.alert)
        assertEquals(State(40.0, -75.0, 0L), d.save)
    }

    @Test fun twentyFiveKmThreshold() {
        val near = HelpAlertRules.decide(snap(40.2), home, true, noon, zone)          // ≈ 22 km
        assertFalse(near.alert); assertNull(near.save)
        val far = HelpAlertRules.decide(snap(40.25), home, true, noon, zone)          // ≈ 28 km
        assertTrue(far.alert)
        assertEquals(State(40.25, -75.0, noon), far.save)
    }

    @Test fun atMostEveryThreeHours() {
        val last = State(40.0, -75.0, noon - 2 * 3600_000L)
        assertFalse(HelpAlertRules.decide(snap(41.0), last, true, noon, zone).alert)
        val later = State(40.0, -75.0, noon - 3 * 3600_000L - 1)
        assertTrue(HelpAlertRules.decide(snap(41.0), later, true, noon, zone).alert)
    }

    @Test fun quietHours() {
        assertFalse(HelpAlertRules.decide(snap(41.0), home, true, at(22, 0), zone).alert)
        assertFalse(HelpAlertRules.decide(snap(41.0), home, true, at(3, 30), zone).alert)
        assertFalse(HelpAlertRules.decide(snap(41.0), home, true, at(6, 59), zone).alert)
        assertTrue(HelpAlertRules.decide(snap(41.0), home, true, at(7, 0), zone).alert)
        assertTrue(HelpAlertRules.decide(snap(41.0), home, true, at(21, 59), zone).alert)
        // a suppressed alert leaves the state alone, so it fires once the quiet hours end
        assertNull(HelpAlertRules.decide(snap(41.0), home, true, at(23), zone).save)
    }

    @Test fun disabledFlag() {
        val d = HelpAlertRules.decide(snap(41.0), home, enabled = false, now = noon, zone = zone)
        assertFalse(d.alert); assertNull(d.save)
        assertNull(HelpAlertRules.decide(snap(41.0), null, enabled = false, now = noon, zone = zone).save)
    }

    @Test fun staleOrEmptySnapshotsNeverAlert() {
        assertFalse(HelpAlertRules.decide(snap(41.0, stale = true), home, true, noon, zone).alert)
        assertFalse(HelpAlertRules.decide(snap(41.0, places = false), home, true, noon, zone).alert)
        assertFalse(HelpAlertRules.decide(snap(41.0).copy(origin = "none"), home, true, noon, zone).alert)
    }

    @Test fun stateRoundTrip() {
        val s = State(40.0, -75.0, noon)
        assertEquals(s, HelpAlertRules.parse(HelpAlertRules.encode(s)))
        assertNull(HelpAlertRules.parse(""))
        assertNull(HelpAlertRules.parse("not json"))
        assertNotNull(HelpAlertRules.parse("""{"lat":40.0,"lon":-75.0}"""))
    }
}
