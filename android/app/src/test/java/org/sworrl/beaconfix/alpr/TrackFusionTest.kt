package org.sworrl.beaconfix.alpr

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.alpr.core.AcceptRules
import org.sworrl.beaconfix.alpr.core.PlateGroups
import org.sworrl.beaconfix.alpr.core.PlateObs
import org.sworrl.beaconfix.alpr.core.PlateRead
import org.sworrl.beaconfix.alpr.core.PlateSimilarity
import org.sworrl.beaconfix.alpr.core.SlotChar
import org.sworrl.beaconfix.alpr.core.TrackFusion

class TrackFusionTest {
    /** A read of [text] at [p] per slot, with runner-ups [alts] (slot → char, prob). */
    private fun read(text: String, p: Float = 0.9f, alts: Map<Int, Pair<Char, Float>> = emptyMap()): PlateRead {
        val slots = text.mapIndexed { i, c ->
            val a = alts[i]
            if (a == null) listOf(SlotChar(c, p)) else listOf(SlotChar(c, p), SlotChar(a.first, a.second)).sortedByDescending { it.p }
        }
        val chosen = slots.map { it[0] }
        return PlateRead(chosen.map { it.c }.joinToString(""), chosen.map { it.p }.average().toFloat(), slots)
    }
    private fun obs(r: PlateRead, q: Float = 0.5f, t: Long = 0) = PlateObs(r, q, t)

    @Test fun jaroWinkler() {
        assertEquals(1.0, PlateSimilarity.jaroWinkler("ABC1234", "ABC1234"), 1e-9)
        assertTrue(PlateSimilarity.jaroWinkler("ABC1234", "A8C1234") >= 0.85)
        assertTrue(PlateSimilarity.jaroWinkler("ABC1234", "BC1234") >= 0.85)       // a fragment
        assertTrue(PlateSimilarity.jaroWinkler("ABC1234", "XYZ7788") < 0.5)
        assertEquals(0.0, PlateSimilarity.jaroWinkler("", "ABC"), 1e-9)
    }

    @Test fun perCharacterVotingOverridesOneBadFrame() {
        // frames 1 and 3 read B (with 8 as runner-up); frame 2 read 8 confidently but blurred (low quality)
        val f = TrackFusion.fuse(listOf(
            obs(read("ABC1234", 0.8f, mapOf(1 to ('8' to 0.15f))), q = 0.6f),
            obs(read("A8C1234", 0.7f, mapOf(1 to ('B' to 0.25f))), q = 0.2f),
            obs(read("ABC1234", 0.75f, mapOf(1 to ('8' to 0.2f))), q = 0.5f),
        ), null)!!
        assertEquals("ABC1234", f.text)
        assertEquals(3, f.frames); assertEquals(2, f.exactReads)
        val slot = f.lattice.slots[1]
        assertEquals('B', slot[0].c); assertEquals('8', slot[1].c)
        assertEquals(1f, slot.sumOf { it.p.toDouble() }.toFloat(), 0.1f)
    }

    @Test fun qualityWeightsDecideATie() {
        val sharp = obs(read("A8C1234", 0.7f, mapOf(1 to ('B' to 0.25f))), q = 0.9f)
        val blurred = obs(read("ABC1234", 0.7f, mapOf(1 to ('8' to 0.25f))), q = 0.1f)
        assertEquals('8', TrackFusion.vote(listOf(sharp, blurred))!!.slots[1][0].c)
    }

    @Test fun groupVoteDropsAnOddRead() {
        // a frame that caught the neighbouring car's plate is outvoted as a group, not mixed in per character
        val f = TrackFusion.fuse(listOf(obs(read("ABC1234")), obs(read("ABC1234")), obs(read("XYZ7788"), q = 0.9f)), null)!!
        assertEquals("ABC1234", f.text)
        assertEquals(2, f.frames)
        assertEquals(2, TrackFusion.groups(listOf(obs(read("ABC1234")), obs(read("A8C1234")), obs(read("XYZ7788")))).size)
    }

    @Test fun formatPriorAppliesAfterFusion() {
        // Texas: LLLDDDD. The lattice slightly prefers 8, but B fits the state's format
        val f = TrackFusion.fuse(listOf(obs(read("A8C1234", 0.6f, mapOf(1 to ('B' to 0.35f))))), "TX")!!
        assertEquals("A8C1234", f.raw); assertEquals("ABC1234", f.text)
        assertEquals(0, f.exactReads)                    // corrected: no read said exactly this
    }

    @Test fun acceptRules() {
        fun fused(vararg r: PlateRead) = TrackFusion.fuse(r.map { obs(it) }, null)!!
        assertTrue(AcceptRules.accepted(fused(read("ABC1234", 0.92f))))                               // 1 read ≥ 0.90, valid format
        assertFalse(AcceptRules.accepted(fused(read("AB12C3D", 0.95f))))                              // …but not a US format
        assertFalse(AcceptRules.accepted(fused(read("ABC1234", 0.85f))))
        assertTrue(AcceptRules.accepted(fused(read("ABC1234", 0.8f), read("ABC1234", 0.76f))))       // 2 exact ≥ 0.75
        assertFalse(AcceptRules.accepted(fused(read("ABC1234", 0.72f), read("ABC1234", 0.72f))))
        assertTrue(AcceptRules.accepted(fused(read("ABC1234", 0.72f), read("A8C1234", 0.6f), read("ABC1Z34", 0.6f))))  // 3 frames ≥ 0.70
        assertFalse(AcceptRules.accepted(fused(read("ABC1234", 0.65f), read("A8C1234", 0.6f), read("ABC1Z34", 0.6f))))
    }

    @Test fun oneEventPerVehicleFromItsBestFrame() {
        val g = PlateGroups<String>()
        val (a, best1) = g.observe(1, read("ABC1234", 0.8f), 0.3f, 0, "TX"); assertTrue(best1); g.setShot(a, "frame0")
        val (_, best2) = g.observe(1, read("ABC1234", 0.85f), 0.6f, 500, "TX"); assertTrue(best2); g.setShot(a, "frame1")
        val (_, best3) = g.observe(1, read("ABC1234", 0.8f), 0.4f, 1_000, "TX"); assertFalse(best3)
        assertTrue(a.accepted)
        assertTrue(g.due(1_500).isEmpty())                       // still in view
        g.trackEnded(1)
        val d = g.due(4_000).single()
        assertEquals(PlateGroups.Reason.TRACK_END, d.reason); assertEquals("frame1", d.group.shot)
        g.markEmitted(d.group)
        assertNull(d.group.shot)
        // the same car reappears 20 s later on a new track: same vehicle (accepted groups live 60 s), no new event
        val (b, _) = g.observe(2, read("ABC1234", 0.9f), 0.7f, 24_000, "TX")
        assertTrue(b === a)
        g.trackEnded(2)
        assertTrue(g.due(30_000).isEmpty())
        assertTrue(g.due(90_000).isEmpty()); assertTrue(g.groups.isEmpty())
    }

    @Test fun unacceptedVehicleIsEmittedWhenItExpires() {
        val g = PlateGroups<String>()
        val (a, _) = g.observe(1, read("ABC1234", 0.6f), 0.3f, 0, null); g.setShot(a, "s")
        g.observe(1, null, 0.2f, 300, null)                                                 // a read that failed the gates
        assertFalse(a.accepted)
        g.trackEnded(1)
        assertTrue(g.due(5_000).isEmpty())                                                  // may still come back
        assertEquals(PlateGroups.Reason.EXPIRED, g.due(10_500).single().reason)
        // a single poor sighting is not worth an upload
        val h = PlateGroups<String>()
        h.observe(7, read("ABC1234", 0.3f), 0.1f, 0, null); h.trackEnded(7)
        assertTrue(h.due(11_000).isEmpty())
    }

    @Test fun aNewTrackJoinsTheGroupOfTheSamePlate() {
        val g = PlateGroups<String>()
        val (a, _) = g.observe(1, read("ABC1234", 0.6f), 0.3f, 0, null)
        g.trackEnded(1)
        // the plate is first too small to read on the new track, then read: the groups merge
        val (n, _) = g.observe(2, null, 0.05f, 2_000, null)
        assertTrue(n !== a)
        val (m, _) = g.observe(2, read("ABC1234", 0.7f), 0.4f, 2_500, null)
        assertTrue(m === a)
        assertEquals(1, g.groups.size)
        assertEquals(setOf(1, 2), a.tracks)
        assertEquals(2, a.fused!!.frames)
    }

    @Test fun holdEmitsACarFollowedForLong() {
        val g = PlateGroups<String>(holdMs = 20_000)
        val (a, _) = g.observe(1, read("ABC1234", 0.95f), 0.5f, 0, null); g.setShot(a, "s")
        assertTrue(a.accepted)
        assertTrue(g.due(10_000).isEmpty())
        assertEquals(PlateGroups.Reason.HOLD, g.due(20_000).single().reason)
    }

    @Test fun flushEmitsWhatIsStillInView() {
        val g = PlateGroups<String>()
        g.observe(1, read("ABC1234", 0.95f), 0.5f, 0, null)
        g.observe(2, read("XYZ7788", 0.3f), 0.1f, 0, null)
        assertNotNull(g.flush().singleOrNull())
        assertTrue(g.groups.isEmpty())
    }
}
