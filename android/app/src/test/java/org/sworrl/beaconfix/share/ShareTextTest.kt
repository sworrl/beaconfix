package org.sworrl.beaconfix.share

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

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
}
