package org.sworrl.beaconfix.share

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.ZoneOffset

class ShareTextTest {
    @Test fun locationCarriesCoordinatesAccuracyAddressAndLinks() {
        val t = ShareText.location(40.0, -75.0, 12.4, "1 Test St, Testville", null)
        assertTrue(t, t.startsWith("My location: 40.00000, -75.00000 (±12 m)"))
        assertTrue(t, t.contains("\n1 Test St, Testville\n"))
        assertTrue(t, t.contains("geo:40.000000,-75.000000"))
        assertTrue(t, t.contains("mlat=40.000000&mlon=-75.000000"))
        assertTrue(t, t.contains("query=40.000000,-75.000000"))
        assertTrue(t, t.contains("ll=40.000000,-75.000000"))
        assertFalse(t, t.endsWith("\n"))
    }

    @Test fun sixDecimalsInLinksAndNoAccuracyWhenUnknown() {
        val t = ShareText.location(40.1234567, -75.7654321, null, null, "RV")
        assertTrue(t, t.startsWith("RV: 40.12346, -75.76543\n"))
        assertTrue(t, t.contains("geo:40.123457,-75.765432"))
        assertTrue(t, t.contains("mlat=40.123457"))
        assertFalse(t, t.contains("±"))
    }

    @Test fun placeHasNamePhoneAndAddress() {
        val t = ShareText.place("Test Children's Hospital", 40.0, -75.0, "1 Test St", "+1 555 0100")
        assertTrue(t, t.startsWith("Test Children's Hospital\n1 Test St\nPhone: +1 555 0100\n40.00000, -75.00000\n"))
        assertTrue(t, t.contains("geo:40.000000,-75.000000?q=40.000000,-75.000000(Test%20Children%27s%20Hospital)"))
    }

    @Test fun linksAreGeoOsmGoogleApple() {
        val l = ShareText.links(40.0, -75.0, "Camp (north)")
        assertEquals(4, l.size)
        assertEquals("geo:40.000000,-75.000000?q=40.000000,-75.000000(Camp%20%20north)", l[0])
        assertEquals("https://www.openstreetmap.org/?mlat=40.000000&mlon=-75.000000#map=17/40.000000/-75.000000", l[1])
        assertEquals("https://www.google.com/maps/search/?api=1&query=40.000000,-75.000000", l[2])
        assertEquals("https://maps.apple.com/?ll=40.000000,-75.000000&q=Camp%20%20north", l[3])
        assertEquals("https://maps.apple.com/?ll=40.000000,-75.000000", ShareText.links(40.0, -75.0, "")[3])
    }

    @Test fun theRvsPositionIsLabelledAsSuch() {
        val t = ShareText.location(40.0, -75.0, 15.0, null, label = ShareText.RV_POSITION)
        assertTrue(t, t.startsWith("RV position (phone has no recent fix): 40.00000, -75.00000 (±15 m)\n"))
        assertFalse(t, t.contains("My location"))
        assertTrue(t, t.contains("geo:40.000000,-75.000000?q=40.000000,-75.000000(RV%20position%20%20phone%20has%20no%20recent%20fix)"))
    }

    @Test fun anOldFixCarriesItsTime() {
        val now = 1_790_000_000_000L                                   // 2026-09-21 14:13:20 UTC
        val fresh = ShareText.location(40.0, -75.0, 10.0, null, fixAtMs = now - 60_000L, nowMs = now, zone = ZoneOffset.UTC)
        assertFalse(fresh, fresh.contains("Fix taken"))
        val old = ShareText.location(40.0, -75.0, 10.0, "1 Test St", label = ShareText.LAST_KNOWN, fixAtMs = now - 25 * 60_000L, nowMs = now, zone = ZoneOffset.UTC)
        assertTrue(old, old.startsWith("My last known location: 40.00000, -75.00000 (±10 m)\nFix taken 13:48 (25 min ago)\n1 Test St\n"))
        assertEquals("Fix taken 2026-09-20 12:13 (26 h ago)", ShareText.fixAge(now - 26 * 3_600_000L, now, ZoneOffset.UTC))
        assertEquals("Fix taken 2026-09-18 14:13 (3 d ago)", ShareText.fixAge(now - 3 * 86_400_000L, now, ZoneOffset.UTC))
        assertEquals(null, ShareText.fixAge(0L, now, ZoneOffset.UTC))
    }
}
