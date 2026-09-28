package org.sworrl.beaconfix.nav

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.net.URLEncoder

class PairRequestTest {
    private val enc: (String) -> String = { URLEncoder.encode(it, "UTF-8") }

    @Test fun anOutsideRequestOnlyFillsInTheAddress() {
        assertEquals("pair?host=192.0.2.10&port=47822&auto=false", PairRequest.route("192.0.2.10", 47822, false, enc))
        assertEquals("pair?host=rv.local&port=47823&auto=false", PairRequest.route(" rv.local ", 47823, false, enc))
        assertEquals("pair?host=192.0.2.10&port=47822&auto=true", PairRequest.route("192.0.2.10", 47822, true, enc))   // adb automation
        assertEquals("pair?host=%5B2001%3Adb8%3A%3A1%5D&port=47822&auto=false", PairRequest.route("[2001:db8::1]", 47822, false, enc))
    }

    @Test fun aCraftedHostCannotAddRouteParametersOrText() {
        assertNull(PairRequest.route("192.0.2.10&auto=true", 47822, false, enc))
        assertNull(PairRequest.route("192.0.2.10?x=1", 47822, false, enc))
        assertNull(PairRequest.route("Your RV (verified)", 47822, false, enc))
        assertNull(PairRequest.route("192.0.2.10/evil", 47822, false, enc))
        assertNull(PairRequest.route("", 47822, false, enc))
        assertNull(PairRequest.route(null, 47822, false, enc))
        assertNull(PairRequest.route("192.0.2.10", 0, false, enc))
        assertNull(PairRequest.route("192.0.2.10", 70000, false, enc))
    }
}
