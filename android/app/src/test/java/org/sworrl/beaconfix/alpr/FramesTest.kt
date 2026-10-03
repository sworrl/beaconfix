package org.sworrl.beaconfix.alpr

import androidx.camera.core.ImageInfo
import androidx.camera.core.ImageProxy
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.alpr.core.BoxF
import org.sworrl.beaconfix.alpr.core.PlateQuality
import org.sworrl.beaconfix.alpr.vision.ArgbFrame
import org.sworrl.beaconfix.alpr.vision.PlaneBuffers
import org.sworrl.beaconfix.alpr.vision.YuvFrame
import java.lang.reflect.Proxy
import java.nio.ByteBuffer
import kotlin.math.abs

/** Resampling: bilinear when enlarging (the recognizer's training resize), box-averaged when shrinking. */
class FramesTest {
    private fun gray(v: Int) = (0xFF shl 24) or (v shl 16) or (v shl 8) or v
    private fun lum(c: Int) = (c shr 8) and 0xFF

    /** A horizontal ramp: column x has value k·x. */
    private fun ramp(w: Int, h: Int, k: Int = 10) = ArgbFrame(w, h, IntArray(w * h) { gray(k * (it % w)) })

    @Test fun enlargingInterpolatesInsteadOfRepeating() {
        val f = ramp(8, 2)
        val out = IntArray(32 * 2)
        f.sampleArgb(BoxF(0f, 0f, 8f, 2f), 32, 2, out)
        val row = (0 until 32).map { lum(out[it]) }
        // 4× enlargement: centres at x = 0.125·(i+0.5) - 0.5 source pixels; values rise smoothly, ~2.5 per output pixel
        for (i in 3 until 28) assertEquals("x=$i $row", 2.5, (row[i + 1] - row[i]).toDouble(), 1.01)
        assertEquals(0, row[0]); assertEquals(70, row[31])                     // clamped at the edges
        // nearest neighbour would give runs of four equal values
        assertTrue(row.zipWithNext().count { it.first == it.second } < 10)
    }

    @Test fun shrinkingAveragesTwoTaps() {
        val f = ramp(64, 4, 3)
        val out = IntArray(8 * 1)
        f.sampleArgb(BoxF(0f, 0f, 64f, 4f), 8, 1, out)
        // output i covers source [8i, 8i+8): taps at 8i+2 and 8i+6 → mean 3·(8i+4)
        for (i in 0 until 8) assertEquals(3 * (8 * i + 4), lum(out[i]))
    }

    @Test fun identityIsExact() {
        val f = ArgbFrame(3, 2, intArrayOf(gray(1), gray(50), gray(200), gray(7), gray(9), gray(255)))
        val out = IntArray(6)
        f.sampleArgb(BoxF(0f, 0f, 3f, 2f), 3, 2, out)
        assertEquals(listOf(1, 50, 200, 7, 9, 255), out.map { lum(it) })
    }

    /** A YUV_420_888 ImageProxy over [y] ([w]×[h]) with flat chroma (grey), via dynamic proxies. */
    private fun yuv(w: Int, h: Int, y: ByteArray, rotation: Int): Pair<ImageProxy, Int> {
        val u = ByteArray((w / 2) * (h / 2)) { 128.toByte() }
        fun plane(buf: ByteArray, row: Int) = Proxy.newProxyInstance(javaClass.classLoader, arrayOf(ImageProxy.PlaneProxy::class.java)) { _, m, _ ->
            when (m.name) { "getBuffer" -> ByteBuffer.wrap(buf); "getRowStride" -> row; "getPixelStride" -> 1; else -> null }
        } as ImageProxy.PlaneProxy
        val planes = arrayOf(plane(y, w), plane(u, w / 2), plane(u.copyOf(), w / 2))
        val info = Proxy.newProxyInstance(javaClass.classLoader, arrayOf(ImageInfo::class.java)) { _, m, _ ->
            when (m.name) { "getRotationDegrees" -> rotation; "getTimestamp" -> 0L; else -> null }
        } as ImageInfo
        val img = Proxy.newProxyInstance(javaClass.classLoader, arrayOf(ImageProxy::class.java)) { _, m, _ ->
            when (m.name) { "getWidth" -> w; "getHeight" -> h; "getPlanes" -> planes; "getImageInfo" -> info; "getFormat" -> 35; else -> null }
        } as ImageProxy
        return img to rotation
    }

    @Test fun yuvFrameBilinearAndRotation() {
        val w = 8; val h = 4
        val y = ByteArray(w * h) { (10 * (it % w)).toByte() }                  // the same ramp in the luma plane
        val (img, rot) = yuv(w, h, y, 0)
        val f = YuvFrame(img, rot, PlaneBuffers())
        val out = IntArray(32 * 4)
        f.sampleArgb(BoxF(0f, 0f, 8f, 4f), 32, 4, out)
        val row = (0 until 32).map { lum(out[it]) }
        for (i in 3 until 28) assertTrue("x=$i $row", abs((row[i + 1] - row[i]) - 2.5) <= 1.01)
        // rotated 90°: the upright frame is 4×8 and the ramp runs down it (buffer column x → upright row x)
        val (img90, r90) = yuv(w, h, y, 90)
        val g = YuvFrame(img90, r90, PlaneBuffers())
        assertEquals(4, g.width); assertEquals(8, g.height)
        val col = IntArray(1 * 8)
        g.sampleArgb(BoxF(1f, 0f, 2f, 8f), 1, 8, col)
        assertEquals((0 until 8).map { 10 * it }, col.map { lum(it) })
        assertEquals(35, f.meanLuma())
    }

    @Test fun tenengradPrefersSharpCrops() {
        val w = 128; val h = 64
        val sharp = IntArray(w * h) { if ((it % w) / 8 % 2 == 0) gray(30) else gray(220) }
        val soft = IntArray(w * h) { val x = it % w; val t = (x % 32) / 32.0; gray((125 + 95 * kotlin.math.sin(2 * Math.PI * t)).toInt()) }
        val ts = PlateQuality.tenengrad(sharp, w, h); val tb = PlateQuality.tenengrad(soft, w, h)
        assertTrue("$ts vs $tb", ts > 3 * tb)
        assertEquals(0f, PlateQuality.tenengrad(IntArray(w * h) { gray(100) }, w, h), 0f)
        assertTrue(PlateQuality.q(64f, ts, 0.9f) > PlateQuality.q(32f, ts, 0.9f))
        assertTrue(PlateQuality.q(64f, ts, 0.9f) > PlateQuality.q(64f, tb, 0.9f))
    }
}
