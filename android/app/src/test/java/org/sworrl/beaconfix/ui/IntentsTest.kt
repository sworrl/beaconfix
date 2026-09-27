package org.sworrl.beaconfix.ui

import org.junit.Assert.assertEquals
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
}
