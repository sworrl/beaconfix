package org.sworrl.beaconfix.alpr

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.alpr.core.PlateFormats
import org.sworrl.beaconfix.alpr.core.PlateFormats.Fit
import org.sworrl.beaconfix.alpr.core.PlateLattice
import org.sworrl.beaconfix.alpr.core.SlotChar

class PlateFormatsTest {
    private fun lat(vararg slots: List<Pair<Char, Float>>) = PlateLattice(slots.map { s -> s.map { SlotChar(it.first, it.second) } })
    private fun one(text: String, p: Float = 0.9f) = text.map { listOf(it to p) }.toTypedArray()

    @Test fun patterns() {
        assertTrue(PlateFormats.matches("ABC1234", "LLLDDDD"))
        assertFalse(PlateFormats.matches("A8C1234", "LLLDDDD"))
        assertFalse(PlateFormats.matches("ABC123", "LLLDDDD"))
        assertEquals(Fit.STATE, PlateFormats.fit("7ABC123", "CA"))
        assertEquals(Fit.US, PlateFormats.fit("7ABC123", "TX"))          // a California car in Texas
        assertEquals(Fit.NONE, PlateFormats.fit("GOBEARS", "TX"))         // vanity
        assertEquals(Fit.US, PlateFormats.fit("ABC1234", null))
        assertTrue(PlateFormats.valid("AB1C2D"))                          // Missouri
        assertFalse(PlateFormats.valid("AB12C3D"))
    }

    @Test fun gates() {
        assertFalse(PlateFormats.gate("AB12"))                            // too short
        assertTrue(PlateFormats.gate("AB123"))
        assertTrue(PlateFormats.gate("ABC1234", 0.01f))                   // a US format passes whatever the region head says
        assertFalse(PlateFormats.gate("AB12C3D", 0.05f))                  // 7+ characters, no format, not North American
        assertTrue(PlateFormats.gate("AB12C3D", 0.8f))
        assertTrue(PlateFormats.gate("AB12C3D"))                          // no region head: 7 is allowed
        assertFalse(PlateFormats.gate("AB12C3DE"))                        // …8 without a region head is not
        assertFalse(PlateFormats.gate("ABC1234567", 0.99f))               // longer than any US plate
    }

    @Test fun statePriorResolvesAmbiguousSlots() {
        // slot 2: 8 (0.55) or B (0.4); in Texas LLLDDDD wins
        val l = lat(*one("AB"), listOf('8' to 0.55f, 'B' to 0.4f), *one("1234"))
        assertEquals("AB81234", l.best())
        val tx = PlateFormats.choose(l, "TX")
        assertEquals("ABB1234", tx.text); assertEquals(Fit.STATE, tx.fit)
        // no state known: AB81234 fits Illinois' LLDDDDD as well, so both are US formats and the lattice decides
        assertEquals("AB81234", PlateFormats.choose(l, null).text)
    }

    @Test fun priorNeverInventsCharacters() {
        // the only letter on offer is far less likely than the digit: 0.98 vs 0.005, so the read stays as it is
        val l = lat(*one("AB"), listOf('8' to 0.98f, 'B' to 0.005f), *one("1234"))
        assertEquals("AB81234", PlateFormats.choose(l, "TX").text)
        // a format needing a character no slot offers is not considered
        val v = lat(*one("GOBEARS"))
        assertEquals("GOBEARS", PlateFormats.choose(v, "TX").text)
        assertEquals(Fit.NONE, PlateFormats.choose(v, "TX").fit)
    }

    @Test fun stateFormatBeatsAnotherStatesFormat() {
        // 1 or I first: CA DLLLDDD wants a digit there; in California that wins, elsewhere both are US formats
        val l = lat(listOf('I' to 0.5f, '1' to 0.45f), *one("ABC123"))
        assertEquals("1ABC123", PlateFormats.choose(l, "CA").text)
    }
}
