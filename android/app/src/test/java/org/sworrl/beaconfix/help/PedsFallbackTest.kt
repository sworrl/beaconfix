package org.sworrl.beaconfix.help

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class PedsFallbackTest {
    private fun t(name: String, emergency: Boolean = false, er: String = "", cat: String = "health", peds: Int = 0) = PedsFallback.tier(cat, name, emergency, er, peds)

    @Test fun childrensHospitalsBecomeTier2OrTier1WithAnEr() {
        assertEquals(2, t("Test Children's Hospital"))
        assertEquals(2, t("Test Childrens Medical Center"))
        assertEquals(2, t("Test Pediatric Hospital"))
        assertEquals(2, t("Test Paediatric Unit"))
        assertEquals(1, t("Test Children’s Hospital", emergency = true))
        assertEquals(1, t("Test Kids Hospital", er = "yes"))
    }

    @Test fun trapsStayWhatTheyAre() {
        assertEquals(0, t("Test Children's Behavioral Health"))
        assertEquals(0, t("Test Shriners Children's"))
        assertEquals(0, t("Test Children's Rehab"))
        assertEquals(0, t("Test Children's Home"))
        assertEquals(0, t("Test Valley Children's Hospital", er = "no"))
        assertEquals(0, t("Test General Hospital", emergency = true))
        assertEquals(0, t("Test Childress Regional"))
        assertEquals(0, t("Test Children's Walk-In", cat = "urgent"))
        assertEquals(0, t("Test Children's Hospital", peds = 1))
    }

    @Test fun applyRewritesTheCategory() {
        val c = NearestHelp.Candidate(key = "way/1", cat = "health", name = "Test Children's Hospital", lat = 40.0, lon = -75.0, source = "desktop:a")
        val out = PedsFallback.apply(c)
        assertEquals("peds_er", out.cat); assertEquals(2, out.peds); assertTrue(out.guessed)
        val general = c.copy(name = "Test General Hospital")
        assertEquals(general, PedsFallback.apply(general))
    }
}
