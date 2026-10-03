package org.sworrl.beaconfix.alpr

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.alpr.core.PlateText

class PlateTextTest {
    private fun probs(text: String, p: Float = 0.9f, alt: Char? = null): FloatArray {
        val n = PlateText.ALPHABET.length
        val a = FloatArray(PlateText.MAX_SLOTS * n)
        val padded = text.padEnd(PlateText.MAX_SLOTS, PlateText.PAD)
        for (s in 0 until PlateText.MAX_SLOTS) {
            val c = padded[s]
            val rest = (1f - p) / (n - 1)
            for (i in 0 until n) a[s * n + i] = rest
            a[s * n + PlateText.ALPHABET.indexOf(c)] = p
            if (alt != null && c != PlateText.PAD) { a[s * n + PlateText.ALPHABET.indexOf(alt)] = (1f - p) * 0.8f }
        }
        return a
    }

    @Test fun decodesTextDropsPadsAndAveragesConfidence() {
        val r = PlateText.decode(probs("ABC1234", 0.9f))
        assertEquals("ABC1234", r.text)
        assertEquals(0.9f, r.conf, 0.001f)
        assertEquals(7, r.slots.size)
        assertEquals('A', r.slots[0][0].c)
        assertTrue(r.slots[0][0].p >= r.slots[0].last().p)
    }

    @Test fun keepsAlternativesPerSlot() {
        val r = PlateText.decode(probs("S0", 0.6f, alt = '5'))
        assertEquals("S0", r.text)
        assertEquals('5', r.slots[0][1].c)
    }

    @Test fun normalizeKeepsLettersAndDigits() {
        assertEquals("ABC1234", PlateText.normalize("abc-12 34"))
    }
}
