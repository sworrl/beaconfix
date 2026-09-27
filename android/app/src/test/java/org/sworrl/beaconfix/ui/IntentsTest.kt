package org.sworrl.beaconfix.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class IntentsTest {
    @Test fun dialsTheFirstNumberOnly() {
        assertEquals("112", Intents.dialable("112 / 911"))
        assertEquals("911", Intents.dialable("911"))
        assertEquals("911", Intents.dialable("tel:911"))
        assertEquals("112", Intents.dialable("112 or 911"))
        assertEquals("+15550100", Intents.dialable("+1 555-0100;+1 555-0199"))
    }

    @Test fun keepsOnlyDialableCharacters() {
        assertEquals("+13045550100", Intents.dialable("+1 (304) 555-0100"))
        assertEquals("+15550100", Intents.dialable("+1 555 0100 ext. 12"))
        assertEquals("", Intents.dialable(""))
        assertEquals("", Intents.dialable("no phone"))
    }

    @Test fun geoUriWithLabel() {
        assertEquals("geo:0,0?q=40.000000,-75.000000", Intents.geoUri(40.0, -75.0))
        assertEquals("geo:0,0?q=40.000000,-75.000000(Test%20ER%20%20north)", Intents.geoUri(40.0, -75.0, "Test ER (north)"))
    }

    @Test fun websitesAreHttpOnly() {
        assertEquals("https://example.org/", Intents.webUrl("https://example.org/"))
        assertEquals("http://example.org", Intents.webUrl(" http://example.org "))
        assertEquals("https://example.org", Intents.webUrl("example.org"))
        assertEquals("https://example.org:8080/x", Intents.webUrl("example.org:8080/x"))
        // an OSM website tag must not re-enter BeaconFix or launch anything else
        assertNull(Intents.webUrl("beaconfix://link/abc"))
        assertNull(Intents.webUrl("beaconfix://pair?host=192.0.2.10"))
        assertNull(Intents.webUrl("intent://x#Intent;scheme=beaconfix;end"))
        assertNull(Intents.webUrl("javascript:alert(1)"))
        assertNull(Intents.webUrl("file:///sdcard/x"))
        assertNull(Intents.webUrl("https:example.org"))
        assertNull(Intents.webUrl(""))
    }

    @Test fun anExternalLinkPayloadIsDescribedForConfirmation() {
        val offer = org.sworrl.beaconfix.identity.IdentityOps.encodeOffer(
            org.sworrl.beaconfix.identity.LinkOffer(id = "abcdefghijklmnopqrstuvwxyz", name = "Test Laptop", pub = "AAAA", ts = "2026-09-27T00:00:00Z"))
        val d = org.sworrl.beaconfix.ui.vm.describeLinkPayload(offer)
        assertTrue(d, d.startsWith("Link with Test Laptop (id "))
        assertTrue(d, d.contains("sign in as you"))
        assertTrue(org.sworrl.beaconfix.ui.vm.describeLinkPayload("BFLNK1:###").contains("not one BeaconFix understands"))
    }
}
