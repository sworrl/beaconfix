package org.sworrl.beaconfix.ui

import org.junit.Assert.assertEquals
import org.junit.Test

/** What the red Call buttons may dial from a desktop's answer (Emergency.accept). */
class EmergencyTest {
    @Test fun aRealDesktopsNumbersAreKept() {
        assertEquals("911", Emergency.accept("911", "US"))
        assertEquals("999 / 112", Emergency.accept("999 / 112", "GB"))
        assertEquals("110 police / 119 fire+ambulance", Emergency.accept("110 police / 119 fire+ambulance", "JP"))
        assertEquals("112 / 117 police / 144 ambulance / 118 fire", Emergency.accept("112 / 117 police / 144 ambulance / 118 fire", "CH"))
        assertEquals("10111 police / 10177 ambulance", Emergency.accept("10111 police / 10177 ambulance", "ZA"))
        assertEquals("112", Emergency.accept("112", "FR"))                 // not in the tables: any known emergency code
        assertEquals(" 911 ".trim(), Emergency.accept(" 911 ", "us"))
    }

    @Test fun anythingElseFallsBackToThePhonesOwnNumber() {
        assertEquals("911", Emergency.accept("+1 555 0100", "US"))        // an ordinary phone number
        assertEquals("911", Emergency.accept("555 0100", "US"))
        assertEquals("911", Emergency.accept("5550100", "US"))
        assertEquals("911", Emergency.accept("999", "US"))                // a real code, but not this country's
        assertEquals("911", Emergency.accept("911 / 555", "US"))
        assertEquals("911", Emergency.accept("tel:911", "US"))
        assertEquals("911", Emergency.accept("", "US"))
        assertEquals("911", Emergency.accept(null, "US"))
        assertEquals("999", Emergency.accept("0800 123", "GB"))
        assertEquals("112", Emergency.accept("12345", "FR"))              // 5 digits, but no emergency code
        assertEquals("112 / 911", Emergency.accept("+44 20 7946 0000", null))
    }
}
