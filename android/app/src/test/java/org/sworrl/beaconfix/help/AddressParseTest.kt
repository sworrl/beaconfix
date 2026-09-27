package org.sworrl.beaconfix.help

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class AddressParseTest {
    @Test fun nominatimReverse() {
        val body = """{"place_id":1,"display_name":"1, Test Street, Testville","address":{"house_number":"1","road":"Test Street","town":"Testville","county":"Test County","state":"Pennsylvania","postcode":"00000","country_code":"us"}}"""
        assertEquals(AddressLine("1 Test Street", "Testville", "Test County", "Pennsylvania"), AddressResolver.parseNominatim(body))
    }

    @Test fun ruralRoadWithoutANumber() {
        val body = """{"address":{"road":"Test Pike","hamlet":"Test Hollow","county":"Test County","state":"West Virginia"}}"""
        val a = AddressResolver.parseNominatim(body)!!
        assertEquals("Test Pike", a.line); assertEquals("Test Hollow", a.locality)
        assertEquals("Test Pike, Test Hollow, Test County, West Virginia", a.oneLine())
    }

    @Test fun errorsAndEmptyAnswers() {
        assertNull(AddressResolver.parseNominatim("""{"error":"Unable to geocode"}"""))
        assertNull(AddressResolver.parseNominatim("""{"address":{}}"""))
        assertNull(AddressResolver.parseNominatim("not json"))
    }

    @Test fun aSavedAddressStandsInOnlyNearWhereItWasResolved() {
        // resolved at the synthetic origin; ~111 m per 0.001° of latitude
        assertTrue(AddressResolver.savedUsable(40.0, -75.0, 40.0, -75.0))
        assertTrue(AddressResolver.savedUsable(40.0, -75.0, 40.004, -75.0))       // ~445 m
        assertFalse(AddressResolver.savedUsable(40.0, -75.0, 40.006, -75.0))      // ~667 m: another street
        assertFalse(AddressResolver.savedUsable(40.0, -75.0, 40.5, -75.0))        // the last campground
        assertFalse(AddressResolver.savedUsable(0.0, 0.0, 0.0, 0.0))              // a snapshot saved without a spot
    }
}
