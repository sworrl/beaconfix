package org.sworrl.beaconfix.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.help.HelpKind
import org.sworrl.beaconfix.help.HelpPlace
import org.sworrl.beaconfix.help.HelpSnapshot
import org.sworrl.beaconfix.ui.vm.HelpBadge
import org.sworrl.beaconfix.ui.vm.HelpRows

/** The snapshot → Help screen mapping: tier labels, "Not an ER" on urgent care, and the ER row never a pediatric pick. */
class HelpViewModelTest {
    private fun p(kind: String, name: String, tier: Int = 0, campus: String = "", er: String = "", notEr: Boolean = false, driveS: Int = 900) =
        HelpPlace(kind = kind, name = name, lat = 40.0, lon = -75.0, tier = tier, campus = campus, er = er, notEr = notEr, distM = 12_000.0, driveS = driveS)

    @Test fun tierLabels() {
        assertEquals(HelpBadge.ChildrensEr, HelpRows.badge(p(HelpKind.PEDS_ER, "Test Children's Hospital", tier = 1, er = "yes")))
        assertEquals(HelpBadge.ChildrensCampus("Test General Hospital"), HelpRows.badge(p(HelpKind.PEDS_ER, "Test Children's", tier = 2, campus = "Test General Hospital")))
        assertEquals(HelpBadge.ChildrensUnconfirmed, HelpRows.badge(p(HelpKind.PEDS_CLOSER, "Test Children's", tier = 2)))
        assertEquals(HelpBadge.PedsDept, HelpRows.badge(p(HelpKind.PEDS_CLOSER, "Test Valley Medical Center", tier = 3, er = "yes")))
        assertEquals(HelpBadge.Er, HelpRows.badge(p(HelpKind.ER, "Test General Hospital", er = "yes")))
        assertEquals(HelpBadge.ErUnconfirmed, HelpRows.badge(p(HelpKind.ER, "Test General Hospital")))
        assertEquals(HelpBadge.None, HelpRows.badge(p(HelpKind.POLICE, "Test Police")))
        // every label is a string resource (text, not colour alone)
        assertEquals(R.string.help_badge_campus, HelpBadge.ChildrensCampus("x").res)
        assertEquals(listOf("x"), HelpBadge.ChildrensCampus("x").args)
        assertEquals(R.string.help_badge_unconfirmed, HelpBadge.ChildrensUnconfirmed.res)
        assertEquals(R.string.help_badge_not_er, HelpBadge.NotEr.res)
    }

    @Test fun urgentRowsSayNotAnEr() {
        val ui = HelpRows.map(HelpSnapshot(places = listOf(
            p(HelpKind.PEDS_URGENT, "Test Kids Express Care", tier = 4, notEr = true),
            p(HelpKind.URGENT, "Test Walk-In Clinic"),
        )))
        assertEquals(2, ui.urgent.size)
        assertTrue(ui.urgent.all { it.badge == HelpBadge.NotEr && it.notEr })
        assertEquals(HelpKind.PEDS_URGENT, ui.urgent.first().kind)
    }

    @Test fun theErRowIsNeverAPedsEr() {
        val kids = p(HelpKind.PEDS_ER, "Test Children's Hospital", tier = 1, er = "yes")
        val onlyKids = HelpRows.map(HelpSnapshot(places = listOf(kids)))
        assertEquals("Test Children's Hospital", onlyKids.peds!!.place.name)
        assertNull(onlyKids.er)
        // even a malformed snapshot that files a children's ER under "er" does not show it as the general ER
        assertNull(HelpRows.map(HelpSnapshot(places = listOf(kids.copy(kind = HelpKind.ER)))).er)
        val both = HelpRows.map(HelpSnapshot(places = listOf(kids, p(HelpKind.ER, "Test General Hospital", er = "yes"))))
        assertEquals(HelpKind.ER, both.er!!.kind)
        assertNotEquals(HelpKind.PEDS_ER, both.er!!.kind)
        assertEquals("Test General Hospital", both.er!!.place.name)
        // a general ER with a pediatrics department may be both
        val dept = p(HelpKind.ER, "Test Valley Medical Center", tier = 3, er = "yes")
        assertEquals(HelpBadge.PedsDept, HelpRows.map(HelpSnapshot(places = listOf(dept))).er!!.badge)
    }

    @Test fun sectionsAndEta() {
        val ui = HelpRows.map(HelpSnapshot(number = "911", poisonControl = "1-800-222-1222", places = listOf(
            p(HelpKind.PEDS_ER, "A", tier = 1), p(HelpKind.PEDS_CLOSER, "B", tier = 2), p(HelpKind.ER, "C", er = "yes"),
            p(HelpKind.POLICE, "D"), p(HelpKind.FIRE, "E"), p(HelpKind.PHARMACY, "F"), p(HelpKind.VET, "G", driveS = -1),
        )))
        assertEquals("911", ui.number); assertEquals("1-800-222-1222", ui.poisonControl)
        assertEquals("B", ui.closer!!.place.name)
        assertEquals(listOf("D", "E"), ui.safety.map { it.place.name })
        assertEquals(listOf("F", "G"), ui.more.map { it.place.name })
        assertEquals("~15 min (est.)", ui.peds!!.eta)
        assertEquals("", ui.more.last().eta)
        assertEquals("🧸", ui.peds!!.icon)
        assertTrue(HelpRows.map(HelpSnapshot()).isEmpty)
    }
}
