package org.sworrl.beaconfix.tile

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.help.HelpKind
import org.sworrl.beaconfix.help.HelpPlace
import org.sworrl.beaconfix.help.HelpSnapshot

/** Synthetic places around the repo's placeholder origin (40.0, -75.0). */
class TileModelTest {
    private val km: (Double) -> String = { m -> if (m >= 1000) String.format(java.util.Locale.US, "%.1f km", m / 1000) else "${m.toInt()} m" }
    private fun snap(vararg p: HelpPlace, stale: Boolean = false, at: Long = 1_000L) =
        HelpSnapshot(number = "911", countryCode = "US", places = p.toList(), origin = "phone", originLat = 40.0, originLon = -75.0, fetchedAt = at, stale = stale)
    private val er = HelpPlace(HelpKind.ER, "Test General Hospital", 40.1, -75.0, distM = 12_000.0, driveS = 900)
    private val tier1 = HelpPlace(HelpKind.PEDS_ER, "Test Children's Hospital", 40.3, -75.0, tier = 1, er = "yes", distM = 38_000.0, driveS = 2_700)
    private val tier2 = HelpPlace(HelpKind.PEDS_ER, "Test Children's Medical Center", 40.3, -75.0, tier = 2, campus = "Test General Hospital", distM = 38_000.0, driveS = 2_700)

    @Test fun noData() {
        val t = TileModel.help(HelpSnapshot())
        assertEquals("No data yet", t.subtitle)
        assertFalse(t.active)
        assertEquals("", TileModel.headline(HelpSnapshot()))
        assertEquals("", TileModel.notificationLine(HelpSnapshot()))
    }

    @Test fun fetchedButNothingFound() {
        val t = TileModel.help(snap())
        assertEquals("Nothing found", t.subtitle)
        assertFalse(t.active)
    }

    @Test fun erOnly() {
        val s = snap(er)
        val t = TileModel.help(s, km)
        assertEquals("ER 12.0 km", t.subtitle)
        assertTrue(t.active)
        assertTrue(t.description.contains("Test General Hospital"))
        assertEquals("ER 12.0 km · ~15 min", TileModel.headline(s, km))
    }

    @Test fun tier1() {
        val s = snap(tier1, er)
        val t = TileModel.help(s, km)
        assertEquals("Kids ER 38.0 km · ~45 min", t.subtitle)
        assertTrue(t.active)
        assertEquals("🧸 38.0 km · ~45 min", TileModel.headline(s, km))
        assertEquals("Children's ER", TileModel.tierText(tier1))
        // the general ER is never dropped behind the children's one
        val line = TileModel.notificationLine(s, km)
        assertTrue(line, line.contains("Kids ER: Test Children's Hospital"))
        assertTrue(line, line.contains("ER: Test General Hospital · 12.0 km"))
    }

    @Test fun tier2SaysCallAhead() {
        val s = snap(tier2, er)
        val t = TileModel.help(s, km)
        assertEquals("Kids hospital 38.0 km · call ahead", t.subtitle)
        assertEquals("🧸 38.0 km · call ahead", TileModel.headline(s, km))
        assertEquals("Children's hospital — ER on campus: Test General Hospital — call ahead", TileModel.tierText(tier2))
        assertEquals("Children's hospital — ER not confirmed, call ahead", TileModel.tierText(tier2.copy(campus = "")))
        assertTrue(TileModel.alertText(s, km).contains("Kids hospital (call ahead) Test Children's Medical Center"))
    }

    @Test fun staleSaysSaved() {
        val s = snap(tier1, er, stale = true)
        assertEquals("Kids ER 38.0 km · saved", TileModel.help(s, km).subtitle)
        assertEquals("ER 12.0 km · saved", TileModel.help(snap(er, stale = true), km).subtitle)
        assertTrue(TileModel.help(s, km).description.endsWith("(saved)"))
        assertTrue(TileModel.notificationLine(s, km).endsWith("(saved)"))
    }

    @Test fun oldAnswersCountAsSaved() {
        val s = snap(tier1, at = 1_000L)
        assertFalse(TileModel.aged(s, 1_000L + 23 * 3600_000L).stale)
        assertTrue(TileModel.aged(s, 1_000L + 25 * 3600_000L).stale)
        assertFalse(TileModel.aged(HelpSnapshot(), 10 * 24 * 3600_000L).stale)
    }

    @Test fun theNewestComputationWinsNotTheNewestData() {
        // saved at 10:00 from the old origin; at 11:00 the phone moved and recomputed from the same (older) data
        val saved = snap(tier1, er, at = 5_000L).copy(computedAt = 10_000L)
        val moved = snap(tier1.copy(distM = 2_000.0), er.copy(distM = 30_000.0), at = 4_000L).copy(computedAt = 11_000L, originLat = 40.2)
        assertEquals(moved, TileModel.newest(moved, saved))
        // the live snapshot before its first computation (process just started) never replaces the saved copy
        assertEquals(saved, TileModel.newest(HelpSnapshot(), saved))
        assertEquals(saved, TileModel.newest(null, saved))
        // an older computation loses; a copy saved before computedAt existed (0) loses to any computed answer
        assertEquals(saved, TileModel.newest(moved.copy(computedAt = 9_000L), saved))
        assertEquals(moved, TileModel.newest(moved, saved.copy(computedAt = 0L)))
        // a newer answer with nothing in reach keeps the saved places; with nothing saved either it is shown
        val empty = HelpSnapshot(number = "911", computedAt = 12_000L, fetchedAt = 0L)
        assertEquals(saved, TileModel.newest(empty, saved))
        assertEquals(empty, TileModel.newest(empty, HelpSnapshot()))
    }

    @Test fun urgentCareIsNeverAnEr() {
        val urg = HelpPlace(HelpKind.PEDS_URGENT, "Test Kids Express Care", 40.05, -75.0, tier = 4, notEr = true, distM = 5_000.0)
        assertEquals("Not an ER", TileModel.tierText(urg))
        assertEquals("Kids urgent care (not an ER)", TileModel.longKind(urg))
        assertTrue(TileModel.alertText(snap(urg), km).contains("not an ER"))
    }

    @Test fun etaRounding() {
        assertEquals("", TileModel.eta(-1))
        assertEquals("~5 min", TileModel.eta(0))
        assertEquals("~10 min", TileModel.eta(10 * 60 - 20))
        assertEquals("~45 min", TileModel.eta(2_700))
        assertEquals("~1 h 50 min", TileModel.eta(110 * 60 + 60))
        assertEquals("~2 h", TileModel.eta(119 * 60))
    }

    @Test fun collector() {
        assertEquals(TileModel.CollectorTile("On", true), TileModel.collector(true))
        assertEquals(TileModel.CollectorTile("Paused", false), TileModel.collector(false))
    }
}
