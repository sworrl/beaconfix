package org.sworrl.beaconfix.help

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class OriginTest {
    private val now = 1_790_000_000_000L
    private val rv = Origin.Fix(40.0, -75.0, 10.0, now - 3_600_000L)

    @Test fun aFreshPreciseFixWins() {
        val o = Origin.choose(Origin.Fix(40.45, -75.0, 20.0, now - 60_000L), rv, now)
        assertEquals(Origin.PHONE, o.kind); assertEquals(40.45, o.lat, 0.0); assertEquals("from you", Origin.label(o.kind))
    }

    @Test fun oldOrVagueFixesFallBackToTheRv() {
        assertEquals(Origin.RV, Origin.choose(Origin.Fix(40.45, -75.0, 20.0, now - 16 * 60_000L), rv, now).kind)
        assertEquals(Origin.RV, Origin.choose(Origin.Fix(40.45, -75.0, 1500.0, now - 60_000L), rv, now).kind)
        assertEquals("from the RV", Origin.label(Origin.RV))
    }

    @Test fun withoutAnRvAnyPhoneFixWillDo() {
        assertEquals(Origin.PHONE, Origin.choose(Origin.Fix(40.45, -75.0, 5000.0, now - 86_400_000L), null, now).kind)
        val none = Origin.choose(null, null, now)
        assertEquals(Origin.NONE, none.kind); assertFalse(none.known); assertEquals("", Origin.label(none.kind))
        assertEquals(Origin.NONE, Origin.choose(Origin.Fix(0.0, 0.0), Origin.Fix(0.0, 0.0), now).kind)
    }
}
