package org.sworrl.beaconfix.alpr

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.alpr.core.BoxF
import org.sworrl.beaconfix.alpr.core.CameraMotion
import org.sworrl.beaconfix.alpr.core.TrackInput
import org.sworrl.beaconfix.alpr.core.Tracker
import kotlin.math.abs

/** Tracker association on synthetic box sequences (frame pixels of a 4080×3072 frame, 1–4 fps). */
class TrackerTest {
    private fun box(cx: Float, cy: Float, w: Float, h: Float = w / 2f) = BoxF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)
    private fun det(cx: Float, cy: Float, w: Float, score: Float = 0.8f, text: String = "") = TrackInput(box(cx, cy, w), score, text)

    @Test fun approachingPlateAtOneFpsIsOneTrack() {
        val t = Tracker()
        // a car being overtaken: the plate moves 300 px/s right and grows 25 %/s; at 1 fps consecutive boxes do not overlap
        val ids = (0 until 6).map { i ->
            val w = 60f * Math.pow(1.25, i.toDouble()).toFloat()
            t.update(i * 1000L, listOf(det(1000f + 300f * i, 1800f, w))).ids[0]
        }
        assertTrue(ids.all { it == ids[0] && it > 0 })
    }

    @Test fun twoPlatesSideBySideKeepTheirIds() {
        val t = Tracker()
        var a = -1; var b = -1
        for (i in 0 until 8) {
            val u = t.update(i * 333L, listOf(det(1000f + 40f * i, 1800f, 80f), det(2600f - 40f * i, 1750f, 90f)))
            if (i == 0) { a = u.ids[0]; b = u.ids[1] } else { assertEquals(a, u.ids[0]); assertEquals(b, u.ids[1]) }
        }
        assertNotEquals(a, b)
    }

    @Test fun lostBufferIsInSecondsNotFrames() {
        val t = Tracker(lostMs = 2_500)
        val a = t.update(0, listOf(det(2000f, 1800f, 80f))).ids[0]
        t.update(1_000, emptyList()); t.update(2_000, emptyList())               // hidden behind a truck for 2 s
        assertEquals(a, t.update(2_400, listOf(det(2050f, 1800f, 84f))).ids[0])
        // gone for 3 s at 4 fps (12 empty frames): a new track, and the old one is reported as ended
        var ended = emptyList<Int>()
        for (k in 1..12) ended = ended + t.update(2_400 + k * 250L, emptyList()).ended.map { it.id }
        assertEquals(listOf(a), ended)
        assertNotEquals(a, t.update(5_500, listOf(det(2050f, 1800f, 84f))).ids[0])
    }

    @Test fun weakDetectionsContinueTracksButNeverStartThem() {
        val t = Tracker()
        val a = t.update(0, listOf(det(2000f, 1800f, 80f, 0.9f))).ids[0]
        assertEquals(a, t.update(250, listOf(det(2010f, 1800f, 80f, 0.42f))).ids[0])  // stage 2: low score, followed track
        assertEquals(-1, t.update(500, listOf(det(2010f, 1800f, 80f, 0.9f), det(400f, 1500f, 60f, 0.42f))).ids[1])
        // …unless it already carries a full read
        assertTrue(t.update(750, listOf(det(2010f, 1800f, 80f), det(400f, 1500f, 60f, 0.42f, "ABC1234"))).ids[1] > 0)
    }

    @Test fun plateTextPicksTheRightCandidate() {
        val t = Tracker()
        val a = t.update(0, listOf(det(2000f, 1800f, 100f, text = "ABC1234"))).ids[0]
        // two plates where the track might be; the one reading the same plate wins even though the other is closer
        val u = t.update(1_000, listOf(det(2060f, 1800f, 100f, text = "XYZ7788"), det(2150f, 1800f, 100f, text = "ABC1Z34")))
        assertEquals(a, u.ids[1]); assertNotEquals(a, u.ids[0])
    }

    @Test fun strongTextMatchReFindsATrackWithoutOverlap() {
        val t = Tracker()
        val a = t.update(0, listOf(det(1000f, 1800f, 100f, text = "ABC1234"))).ids[0]
        assertEquals(a, t.update(1_500, listOf(det(3000f, 1800f, 100f, text = "ABC1234"))).ids[0])
    }

    @Test fun gyroShiftKeepsTracksThroughATurn() {
        // turning 20°/s: everything slides ~1000 px between 1 fps frames, plates do not overlap their old boxes
        val noComp = Tracker(); val comp = Tracker()
        val a = noComp.update(0, listOf(det(1500f, 1800f, 80f))).ids[0]
        val b = comp.update(0, listOf(det(1500f, 1800f, 80f))).ids[0]
        assertNotEquals(a, noComp.update(1_000, listOf(det(2500f, 1800f, 80f))).ids[0])
        assertEquals(b, comp.update(1_000, listOf(det(2500f, 1800f, 80f)), shiftX = 1000f).ids[0])
        // a wrong sign never hurts more than no compensation
        val wrong = Tracker()
        val c = wrong.update(0, listOf(det(1500f, 1800f, 80f))).ids[0]
        assertEquals(c, wrong.update(250, listOf(det(1520f, 1800f, 80f)), shiftX = -1000f).ids[0])
    }

    @Test fun pixelShiftFollowsTheDisplayRotation() {
        val f = 3000f
        // portrait: yaw (about device y) moves the scene sideways; pitch (about x) up/down
        assertEquals(30f, CameraMotion.pixelShift(0f, 0.01f, 0, f).first, 1e-3f)
        assertEquals(30f, CameraMotion.pixelShift(0.01f, 0f, 0, f).second, 1e-3f)
        // landscape (ROTATION_90): the car's yaw axis is the device x axis
        val (dx, dy) = CameraMotion.pixelShift(0.01f, 0f, 1, f)
        assertEquals(30f, dx, 1e-3f); assertEquals(0f, dy, 1e-3f)
        assertEquals(-30f, CameraMotion.pixelShift(0.01f, 0f, 3, f).first, 1e-3f)
        assertTrue(abs(CameraMotion.focalPx(4.38f, 6.17f, 4080) - 2896f) < 2f)
        assertEquals(CameraMotion.DEFAULT_FOCAL_RATIO * 4080, CameraMotion.focalPx(0f, 0f, 4080), 1e-3f)
    }
}
