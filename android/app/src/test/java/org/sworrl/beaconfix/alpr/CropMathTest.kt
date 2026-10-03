package org.sworrl.beaconfix.alpr

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.alpr.core.BoxF
import org.sworrl.beaconfix.alpr.core.CropMath
import org.sworrl.beaconfix.alpr.core.Detection
import org.sworrl.beaconfix.alpr.core.IntBox

class CropMathTest {
    private fun contains(c: IntBox, b: BoxF) = b.x1 >= c.x1 && b.y1 >= c.y1 && b.x2 <= c.x2 && b.y2 <= c.y2

    @Test fun vehicleCropIsFourByWideSixByHighWithThePlateLow() {
        val plate = BoxF(2000f, 1800f, 2200f, 1900f)          // 200×100 plate mid-frame
        val c = CropMath.vehicleCrop(plate, 4080, 3072)
        assertEquals(800, c.w); assertEquals(600, c.h)
        assertTrue(contains(c, plate))
        // plate centre ~65 % down the crop, horizontally centred
        assertEquals(0.65, (plate.cy - c.y1) / c.h.toDouble(), 0.01)
        assertEquals(0.5, (plate.cx - c.x1) / c.w.toDouble(), 0.01)
    }

    @Test fun smallPlateGetsTheMinimumCrop() {
        val plate = BoxF(1000f, 1500f, 1045f, 1522f)
        val c = CropMath.vehicleCrop(plate, 4080, 3072)
        assertEquals(CropMath.MIN_SIDE, c.w); assertEquals((CropMath.MIN_SIDE * 0.75).toInt(), c.h)
        assertTrue(contains(c, plate))
    }

    @Test fun cropIsClampedAtTheCornersAndKeepsThePlate() {
        for (plate in listOf(BoxF(0f, 0f, 120f, 50f), BoxF(3960f, 3022f, 4080f, 3072f), BoxF(4000f, 10f, 4080f, 40f), BoxF(5f, 3000f, 90f, 3070f))) {
            val c = CropMath.vehicleCrop(plate, 4080, 3072)
            assertTrue("$c in frame", c.x1 >= 0 && c.y1 >= 0 && c.x2 <= 4080 && c.y2 <= 3072)
            assertTrue("$plate in $c", contains(c, plate))
        }
    }

    @Test fun hugePlateCropIsClampedToTheFrame() {
        val plate = BoxF(500f, 800f, 3500f, 1800f)            // phone right behind a bumper
        val c = CropMath.vehicleCrop(plate, 4080, 3072)
        assertTrue(c.w <= 4080 && c.h <= 3072)
        assertTrue(contains(c, plate))
    }

    @Test fun outputSizeCapsTheLongSideAndNeverUpscales() {
        assertEquals(800 to 600, CropMath.outputSize(800, 600))
        assertEquals(2048 to 1536, CropMath.outputSize(4080, 3060))
        assertEquals(1536 to 2048, CropMath.outputSize(3000, 4000))
    }

    @Test fun normalizeInFrameAndInCrop() {
        assertEquals(listOf(0.25, 0.5, 0.5, 1.0), CropMath.normalize(IntBox(1020, 1536, 2040, 3072), 4080, 3072))
        val crop = IntBox(100, 200, 500, 500)
        assertEquals(listOf(0.25, 0.5, 0.75, 0.8333), CropMath.normalize(BoxF(200f, 350f, 400f, 450f), crop))
    }

    @Test fun letterboxMatchesOpenImageModels() {
        // 3000×2413 photo into 640: r = 640/3000, new 640×515, dh = 62.5 → top pad round(62.4) = 62
        val lb = CropMath.letterbox(3000, 2413, 640)
        assertEquals(640f / 3000f, lb.r, 1e-6f); assertEquals(640, lb.newW); assertEquals(515, lb.newH)
        assertEquals(0f, lb.dw, 1e-6f); assertEquals(62.5f, lb.dh, 1e-6f); assertEquals(62, lb.padTop)
        // model box → source: (x - dw) / r
        val b = lb.toSource(BoxF(258f, 296.5f, 370.8f, 342f))
        assertEquals(1209.4f, b.x1, 0.5f); assertEquals(1097.3f, b.y1, 0.5f)
    }

    @Test fun tilesFor12MpAreThreeSquaresAcrossThePlateBand() {
        val t = CropMath.tiles(4080, 3072, cols = 3)
        assertEquals(3, t.size)
        val side = t[0].w
        assertEquals(1458, side)
        assertTrue(t.all { it.w == side && it.h == side && it.x1 >= 0 && it.x2 <= 4080 && it.y1 >= 0 && it.y2 <= 3072 })
        assertEquals(0, t.first().x1); assertEquals(4080, t.last().x2)
        assertTrue("overlap", t[0].x2 > t[1].x1 && t[1].x2 > t[2].x1)
        assertEquals((3072 * 0.6 - side / 2.0).toInt(), t[0].y1, 1)
    }

    @Test fun noTilesForSmallFramesOrWhenOff() {
        assertTrue(CropMath.tiles(960, 720).isEmpty())
        assertTrue(CropMath.tiles(4080, 3072, cols = 0).isEmpty())
        assertEquals(2, CropMath.tiles(1920, 1080, cols = 2).size)
    }

    @Test fun nmsKeepsTheBestAndDropsSeamDuplicates() {
        val a = Detection(BoxF(100f, 100f, 200f, 150f), 0.9f)
        val b = Detection(BoxF(105f, 102f, 205f, 152f), 0.7f)      // same plate, other tile
        val cut = Detection(BoxF(150f, 100f, 200f, 150f), 0.8f)    // half of it, cut at a seam
        val other = Detection(BoxF(800f, 100f, 900f, 150f), 0.6f)
        val k = CropMath.nms(listOf(b, cut, other, a))
        assertEquals(listOf(a, other), k)
    }

    @Test fun readableNeedsTenPixelCharacters() {
        assertTrue(CropMath.readable(BoxF(0f, 0f, 60f, 30f)))
        assertTrue(!CropMath.readable(BoxF(0f, 0f, 30f, 15f)))
    }

    private fun assertEquals(expected: Int, actual: Int, tol: Int) = assertTrue("$expected vs $actual", kotlin.math.abs(expected - actual) <= tol)
}
