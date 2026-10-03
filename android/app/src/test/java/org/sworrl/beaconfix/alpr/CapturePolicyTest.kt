package org.sworrl.beaconfix.alpr

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.alpr.core.BoxF
import org.sworrl.beaconfix.alpr.core.BurstPlanner
import org.sworrl.beaconfix.alpr.core.CropMath
import org.sworrl.beaconfix.alpr.core.ShutterPolicy
import org.sworrl.beaconfix.alpr.core.ThermalPolicy
import org.sworrl.beaconfix.alpr.core.TrackInput
import org.sworrl.beaconfix.alpr.core.Tracker

class CapturePolicyTest {
    @Test fun thermalSteps() {
        assertEquals(0, ThermalPolicy.level(0.5f, 0))
        assertEquals(1, ThermalPolicy.level(0.85f, 0))
        assertEquals(2, ThermalPolicy.level(0.96f, 0))
        assertEquals(3, ThermalPolicy.level(1.02f, 0))
        assertEquals(0, ThermalPolicy.level(Float.NaN, 0))
        assertEquals(2, ThermalPolicy.level(0.5f, 2))                     // moderate status, as before: half rate, ≤ 2 tile columns
        val s0 = ThermalPolicy.step(0, 3, true); assertEquals(3, s0.tileCols); assertTrue(s0.accurateOcr && s0.burst)
        val s1 = ThermalPolicy.step(1, 3, true); assertEquals(2, s1.tileCols); assertTrue(!s1.accurateOcr && !s1.burst && s1.intervalFactor == 1)
        val s2 = ThermalPolicy.step(2, 4, true); assertEquals(2, s2.tileCols); assertEquals(2, s2.intervalFactor); assertTrue(s2.smallFullFrame)
        val s3 = ThermalPolicy.step(3, 4, true); assertEquals(0, s3.tileCols); assertEquals(4, s3.intervalFactor)
    }

    @Test fun shortShutterKeepsBrightness() {
        val p = ShutterPolicy(isoMin = 50, isoMax = 3200)
        // bright day: AE already below 1 ms → stays auto
        repeat(5) { assertNull(p.onAutoResult(400_000, 50, 25f)) }
        // overcast: AE 1/250 s at ISO 100, three frames in a row → 1/1000 s at ISO 400
        assertNull(p.onAutoResult(4_000_000, 100, 10f)); assertNull(p.onAutoResult(4_000_000, 100, 10f))
        val m = p.onAutoResult(4_000_000, 100, 10f) as ShutterPolicy.Command.Manual
        assertEquals(1_000_000, m.exposureNs); assertEquals(400, m.iso)
        // dusk: the shutter lengthens toward the night cap before ISO goes past its comfort level
        val dusk = p.settings(33_000_000.0 * 200, 5f)
        assertEquals(4_000_000, dusk.exposureNs); assertEquals(1650, dusk.iso)
        // at highway speed the night cap is 1/500 s
        assertEquals(2_000_000, p.settings(33_000_000.0 * 200, 30f).exposureNs)
        // too dark: ISO at its maximum
        assertEquals(3200, p.settings(66_000_000.0 * 1600, 5f).iso)
    }

    @Test fun shortShutterLumaLoopAndHandBack() {
        val p = ShutterPolicy(isoMin = 50, isoMax = 3200)
        repeat(3) { p.onAutoResult(2_000_000, 100, 10f) }
        val dark = p.onLuma(40, 10f) as ShutterPolicy.Command.Manual
        assertTrue(dark.iso > 200)
        assertNull(p.onLuma(120, 10f))                                    // in the band: nothing to do
        var c: ShutterPolicy.Command? = null
        repeat(20) { c = p.onLuma(240, 10f) ?: c }
        assertEquals(ShutterPolicy.Command.Auto, c)                        // bright at the ISO floor → back to AE
        assertTrue(!p.manual)
    }

    @Test fun burstOnlyForSmallFreshTracks() {
        val t = Tracker()
        t.update(0, listOf(TrackInput(BoxF(2000f, 1800f, 2030f, 1812f), 0.8f)))       // 30 px wide: too small to read
        t.update(0, listOf(TrackInput(BoxF(2000f, 1800f, 2030f, 1812f), 0.8f), TrackInput(BoxF(500f, 1500f, 600f, 1540f), 0.9f)))
        val b = BurstPlanner()
        val w = b.plan(100, t.tracks, 4080, 3072)
        assertEquals(1, w.size)                                            // the readable plate gets none
        assertEquals(416f, w[0].w, 0.01f); assertTrue(w[0].cx in 2000f..2030f)
        assertEquals(1, b.plan(600, t.tracks, 4080, 3072).size)            // still within the window
        assertTrue(b.plan(1_400, t.tracks, 4080, 3072).isEmpty())          // window over, track not fresh any more
        t.update(1_500, listOf(TrackInput(BoxF(2000f, 1800f, 2030f, 1812f), 0.8f)))
        assertTrue(b.plan(1_510, t.tracks, 4080, 3072).isEmpty())          // within minGap of the last burst
        assertEquals(1, b.plan(1_600, t.tracks, 4080, 3072).size)
    }

    @Test fun smallerTilesForTheSmallModelKeepTheScale() {
        val big = CropMath.tiles(4080, 3072, 3)
        val small = CropMath.tilesFor(4080, 3072, 3, 416)
        assertEquals(3, big.size); assertEquals(5, small.size)
        // same model-pixels-per-frame-pixel: 640/1458 ≈ 416/948
        assertEquals(640f / big[0].w, 416f / small[0].w, 0.01f)
        assertEquals(0, small.first().x1); assertEquals(4080, small.last().x2)
        for (k in 1 until small.size) assertTrue(small[k].x1 < small[k - 1].x2)   // overlapping
        assertEquals(big, CropMath.tilesFor(4080, 3072, 3, 640))
        assertTrue(CropMath.tilesFor(800, 600, 3, 416).isEmpty())
    }
}
