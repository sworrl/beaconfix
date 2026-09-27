package org.sworrl.beaconfix.nav

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import org.sworrl.beaconfix.nav.IncomingLink.Parsed
import org.sworrl.beaconfix.share.ShareText

/** Synthetic places only (the repo's placeholder origin 40.0, -75.0 and nearby). */
class IncomingLinkTest {
    private fun at(text: String) = IncomingLink.parse(text)

    @Test fun plainPair() {
        assertEquals(Parsed(40.0, -75.0), at("40.0, -75.0"))
        assertEquals(Parsed(40.12345, -75.54321), at("Meet us here: 40.12345,-75.54321 by the gate"))
        assertEquals(Parsed(40.1, -75.2), at("40.1°, -75.2°"))
    }

    @Test fun plainNumbersWithoutDecimalsAreNotAPosition() {
        assertNull(at("Room 12, 34"))
        assertNull(at("call 911, then 112"))
    }

    @Test fun geoUris() {
        assertEquals(Parsed(40.1, -75.2), at("geo:40.1,-75.2"))
        assertEquals(Parsed(40.1, -75.2), at("geo:40.1,-75.2;u=35?z=15"))
        assertEquals(Parsed(40.1, -75.2, "Test Camp"), at("geo:0,0?q=40.1,-75.2(Test%20Camp)"))
        assertEquals(Parsed(40.1, -75.2, "Test Camp"), at("geo:40.1,-75.2?q=Test+Camp"))
    }

    @Test fun geoWithOnlyAnAddressIsNotAPosition() {
        assertNull(at("geo:0,0?q=1+Test+Street"))
    }

    @Test fun googleQueryAndAt() {
        assertEquals(Parsed(40.0352, -75.0559), at("https://www.google.com/maps/search/?api=1&query=40.035200,-75.055900"))
        assertEquals(Parsed(40.0352, -75.0559), at("https://www.google.com/maps/search/?api=1&query=40.035200%2C-75.055900"))
        assertEquals(Parsed(40.1, -75.2), at("https://maps.google.com/?q=40.1,-75.2"))
        assertEquals(Parsed(40.1, -75.2), at("https://www.google.com/maps/@40.1,-75.2,15z"))
    }

    @Test fun googlePlacePrefersThePinOverTheView() {
        val link = "https://www.google.com/maps/place/Test+Park/@40.1,-75.2,17z/data=!3m1!4b1!4m6!3m5!1s0x0:0x0!8m2!3d40.1001!4d-75.2002"
        assertEquals(Parsed(40.1001, -75.2002, "Test Park"), at("Look at this $link"))
    }

    @Test fun openStreetMap() {
        assertEquals(Parsed(40.0, -75.0), at("https://www.openstreetmap.org/?mlat=40.000000&mlon=-75.000000#map=17/40.000000/-75.000000"))
        assertEquals(Parsed(40.1, -75.2), at("https://www.openstreetmap.org/#map=15/40.1/-75.2"))
    }

    @Test fun appleMaps() {
        assertEquals(Parsed(40.0, -75.0, "Test Spot"), at("https://maps.apple.com/?ll=40.000000,-75.000000&q=Test%20Spot"))
        assertEquals(Parsed(40.1, -75.2, "Test Place"), at("https://maps.apple.com/place?coordinate=40.1,-75.2&name=Test%20Place"))
    }

    @Test fun beaconfixLink() {
        assertEquals(Parsed(40.1, -75.2, "Camp site"), at("beaconfix://map?lat=40.1&lon=-75.2&label=Camp%20site"))
        assertNull(at("beaconfix://pair?host=192.0.2.10&port=47822"))
    }

    @Test fun outOfRangeIsRejected() {
        assertNull(at("geo:91.0,-75.0"))
        assertNull(at("geo:40.0,-181.0"))
        assertNull(at("beaconfix://map?lat=-90.5&lon=10"))
        assertNull(at("95.5, 10.5"))
        assertNull(at("geo:0,0"))
    }

    @Test fun garbageAndShortLinks() {
        assertNull(at(""))
        assertNull(at("see you at the lake"))
        assertNull(at("https://maps.app.goo.gl/abcdefg"))
        assertNull(at("https://example.com/page?id=5"))
    }

    @Test fun ourOwnShareTextRoundTrips() {
        val p = at(ShareText.location(40.0, -75.0, 12.0, "1 Test Street", label = "RV spot"))
        assertNotNull(p)
        assertEquals(40.0, p!!.lat, 1e-6); assertEquals(-75.0, p.lon, 1e-6); assertEquals("RV spot", p.label)
        val q = at(ShareText.place("Test Children's Hospital", 40.2, -75.3, "", "+1 555 0100"))!!
        assertEquals(40.2, q.lat, 1e-6); assertEquals(-75.3, q.lon, 1e-6)
    }

    @Test fun trailingPunctuationIsNotPartOfTheLink() {
        assertEquals(Parsed(40.1, -75.2), at("Here: https://maps.google.com/?q=40.1,-75.2."))
        assertEquals(Parsed(40.1, -75.2, "Test Camp"), at("(geo:0,0?q=40.1,-75.2(Test%20Camp))"))
    }
}
