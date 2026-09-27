package org.sworrl.beaconfix.ui

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class UnitsTest {
    @After fun reset() { Units.imperial = false }

    @Test fun imperialThroughCommonHelpers() {
        Units.imperial = true
        assertEquals("1.0 mi", metres(1609.344))
        assertEquals("490 ft", metres(150.0))
        assertEquals("36 mi", km(58.0))
        assertEquals("49 ft", metres(15.0))
        assertEquals("0.1 mi", metres(170.0))
        assertEquals("12 mi", metres(20_000.0))
        assertEquals("3.1 mi", km(5.0))
    }

    @Test fun metricIsUnchanged() {
        Units.imperial = false
        assertEquals("999 m", metres(999.0))
        assertEquals("150 m", metres(150.0))
        assertEquals("1.5 km", metres(1500.0))
        assertEquals("58.0 km", metres(58_000.0))
        assertEquals("5.0 km", km(5.0))
        assertEquals("58 km", km(58.0))
    }

    @Test fun speedElevationRadius() {
        assertEquals("88 km/h", Units.speed(24.5, imperial = false))
        assertEquals("55 mph", Units.speed(24.5, imperial = true))
        assertEquals("300 m", Units.elev(300.0, imperial = false))
        assertEquals("984 ft", Units.elev(300.0, imperial = true))
        assertEquals("150 km", Units.wholeKm(150, imperial = false))
        assertEquals("93 mi", Units.wholeKm(150, imperial = true))
    }

    @Test fun autoRule() {
        assertTrue(Units.resolve("auto", null, "US"))
        assertTrue(Units.resolve("auto", null, "lr"))
        assertTrue(Units.resolve("auto", null, "MM"))
        assertFalse(Units.resolve("auto", null, "DE"))
        assertFalse(Units.resolve("auto", null, null))
        assertTrue(Units.resolve("auto", "imperial", "DE"))
        assertFalse(Units.resolve("auto", "metric", "CA"))
        assertTrue(Units.resolve("imperial", "metric", "DE"))
        assertFalse(Units.resolve("metric", "imperial", "US"))
    }

    @Test fun desktopUnitsFromTripSnapshots() {
        assertEquals("imperial", Units.desktopUnitsOf("""{"rank":"x","locale":{"country":"United States","countryCode":"US","units":"imperial"}}"""))
        assertEquals("metric", Units.desktopUnitsOf("""{"trip":{"locale":{"units":"Metric"}},"ts":""}"""))
        assertNull(Units.desktopUnitsOf("""{"trip":{}}"""))
        assertNull(Units.desktopUnitsOf("not json"))
        assertNull(Units.desktopUnitsOf("""{"locale":{"units":""}}"""))
    }
}
