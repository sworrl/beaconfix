package org.sworrl.beaconfix.help

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
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
}
