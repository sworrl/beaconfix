package org.sworrl.beaconfix.alpr.vision

import android.graphics.Bitmap
import androidx.camera.core.ImageProxy
import org.sworrl.beaconfix.alpr.core.BoxF
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer

/**
 * An upright camera frame the pipeline samples from, in memory only. Nothing is ever converted whole: the detector,
 * the recognizer and the upload crop each sample just the region and size they need.
 */
interface FrameRgb {
    val width: Int
    val height: Int

    /** Region [src] (upright frame pixels) resampled to [dstW]×[dstH] ARGB into [out] (box-filtered when shrinking, bilinear otherwise). */
    fun sampleArgb(src: BoxF, dstW: Int, dstH: Int, out: IntArray)

    /** JPEG of region [src] at [dstW]×[dstH]. */
    fun jpeg(src: BoxF, dstW: Int, dstH: Int, quality: Int): ByteArray {
        val px = IntArray(dstW * dstH)
        sampleArgb(src, dstW, dstH, px)
        val bmp = Bitmap.createBitmap(px, dstW, dstH, Bitmap.Config.ARGB_8888)
        return ByteArrayOutputStream(dstW * dstH / 4).also { bmp.compress(Bitmap.CompressFormat.JPEG, quality, it); bmp.recycle() }.toByteArray()
    }

    fun bitmap(src: BoxF, dstW: Int, dstH: Int): Bitmap {
        val px = IntArray(dstW * dstH)
        sampleArgb(src, dstW, dstH, px)
        return Bitmap.createBitmap(px, dstW, dstH, Bitmap.Config.ARGB_8888)
    }
}

/** Reusable plane copies for [YuvFrame] (analysis thread only; overwritten every frame, never persisted). */
class PlaneBuffers {
    var y = ByteArray(0); var u = ByteArray(0); var v = ByteArray(0)
    fun fill(dst: ByteArray, src: ByteBuffer): ByteArray {
        val n = src.remaining()
        val d = if (dst.size >= n) dst else ByteArray(n)
        src.get(d, 0, n); src.rewind()
        return d
    }
}

/**
 * A YUV_420_888 [ImageProxy] seen upright: [rotation] is CameraX's `rotationDegrees` (clockwise rotation that makes the
 * buffer upright). The planes are copied into [buf] (bulk copies; reused across frames) so sampling is plain array
 * indexing. Every buffer index is separable into a per-column and a per-row offset for all four rotations, so a
 * resampled pixel costs a few array reads.
 */
class YuvFrame(img: ImageProxy, private val rotation: Int, buf: PlaneBuffers) : FrameRgb {
    private val bw = img.width
    private val bh = img.height
    private val y: ByteArray
    private val u: ByteArray
    private val v: ByteArray
    private val yRow = img.planes[0].rowStride
    private val yPix = img.planes[0].pixelStride
    private val uvRow = img.planes[1].rowStride
    private val uvPix = img.planes[1].pixelStride

    init {
        buf.y = buf.fill(buf.y, img.planes[0].buffer); buf.u = buf.fill(buf.u, img.planes[1].buffer); buf.v = buf.fill(buf.v, img.planes[2].buffer)
        y = buf.y; u = buf.u; v = buf.v
    }

    override val width = if (rotation % 180 == 0) bw else bh
    override val height = if (rotation % 180 == 0) bh else bw

    /** Buffer offset contributed by upright column [ux] (luma or chroma). */
    private fun colOff(ux: Int, chroma: Boolean): Int {
        val row = if (chroma) uvRow else yRow; val pix = if (chroma) uvPix else yPix; val sh = if (chroma) 1 else 0
        return when (rotation) {
            90 -> ((bh - 1 - ux) shr sh) * row
            180 -> ((bw - 1 - ux) shr sh) * pix
            270 -> (ux shr sh) * row
            else -> (ux shr sh) * pix
        }
    }

    /** Buffer offset contributed by upright row [uy]. */
    private fun rowOff(uy: Int, chroma: Boolean): Int {
        val row = if (chroma) uvRow else yRow; val pix = if (chroma) uvPix else yPix; val sh = if (chroma) 1 else 0
        return when (rotation) {
            90 -> (uy shr sh) * pix
            180 -> ((bh - 1 - uy) shr sh) * row
            270 -> ((bw - 1 - uy) shr sh) * pix
            else -> (uy shr sh) * row
        }
    }

    override fun sampleArgb(src: BoxF, dstW: Int, dstH: Int, out: IntArray) {
        val ax = Taps.axis(src.x1, src.w / dstW, dstW, width, false); val ay = Taps.axis(src.y1, src.h / dstH, dstH, height, false)
        val cx = Taps.axis(src.x1, src.w / dstW, dstW, width, true); val cy = Taps.axis(src.y1, src.h / dstH, dstH, height, true)
        val c0 = IntArray(dstW) { colOff(ax.p0[it], false) }; val c1 = IntArray(dstW) { colOff(ax.p1[it], false) }
        val cc0 = IntArray(dstW) { colOff(cx.p0[it], true) }; val cc1 = IntArray(dstW) { colOff(cx.p1[it], true) }
        val cl = minOf(u.size, v.size)
        var k = 0
        for (j in 0 until dstH) {
            val r0 = rowOff(ay.p0[j], false); val r1 = rowOff(ay.p1[j], false); val wy = ay.w[j]
            val rc0 = rowOff(cy.p0[j], true); val rc1 = rowOff(cy.p1[j], true); val wcy = cy.w[j]
            for (i in 0 until dstW) {
                val wx = ax.w[i]
                val top = (y[r0 + c0[i]].toInt() and 0xFF) * (256 - wx) + (y[r0 + c1[i]].toInt() and 0xFF) * wx
                val bot = (y[r1 + c0[i]].toInt() and 0xFF) * (256 - wx) + (y[r1 + c1[i]].toInt() and 0xFF) * wx
                val lum = (top * (256 - wy) + bot * wy + 32768) shr 16
                val wcx = cx.w[i]
                val i00 = rc0 + cc0[i]; val i01 = rc0 + cc1[i]; val i10 = rc1 + cc0[i]; val i11 = rc1 + cc1[i]
                var cu = 0; var cv = 0
                if (i00 in 0 until cl && i01 in 0 until cl && i10 in 0 until cl && i11 in 0 until cl) {
                    cu = (Taps.mix(u[i00].toInt() and 0xFF, u[i01].toInt() and 0xFF, u[i10].toInt() and 0xFF, u[i11].toInt() and 0xFF, wcx, wcy)) - 128
                    cv = (Taps.mix(v[i00].toInt() and 0xFF, v[i01].toInt() and 0xFF, v[i10].toInt() and 0xFF, v[i11].toInt() and 0xFF, wcx, wcy)) - 128
                }
                // BT.601 full range (camera YUV is JFIF)
                var r = lum + ((91881 * cv) shr 16); var g = lum - ((22554 * cu + 46802 * cv) shr 16); var bb = lum + ((116130 * cu) shr 16)
                if (r < 0) r = 0 else if (r > 255) r = 255
                if (g < 0) g = 0 else if (g > 255) g = 255
                if (bb < 0) bb = 0 else if (bb > 255) bb = 255
                out[k++] = (0xFF shl 24) or (r shl 16) or (g shl 8) or bb
            }
        }
    }

    /** Mean luma (0..255) over a coarse grid of the whole frame (for the exposure loop). */
    fun meanLuma(): Int {
        var sum = 0L; var n = 0
        for (j in 0 until 24) { val ro = rowOff(((j + 0.5f) * height / 24).toInt(), false); for (i in 0 until 32) { sum += y[ro + colOff(((i + 0.5f) * width / 32).toInt(), false)].toInt() and 0xFF; n++ } }
        return (sum / n).toInt()
    }
}

/**
 * The two source taps per output column (or row) and the weight of the second, in 1/256: shrinking by more than 1.5×
 * averages two points a quarter-step either side of the centre (a cheap box filter); otherwise bilinear interpolation
 * between the two nearest pixel centres, like the recognizer's training resize (OpenCV INTER_LINEAR). Nearest
 * neighbour, used before, made enlarged plates blocky: on synthetic 40 px plates it read 21 % of them right against
 * 83 % with bilinear.
 */
internal class Taps(val p0: IntArray, val p1: IntArray, val w: IntArray) {
    companion object {
        /**
         * Taps along one axis: output pixel i covers source [start + i·step, start + (i+1)·step). [chroma]: taps on the
         * half-resolution chroma grid, returned as luma coordinates (2·c) for the offset functions.
         */
        fun axis(start: Float, step: Float, n: Int, size: Int, chroma: Boolean): Taps {
            val p0 = IntArray(n); val p1 = IntArray(n); val w = IntArray(n)
            val max = size - 1
            val shrink = step > 1.5f
            for (i in 0 until n) {
                val f = start + (i + 0.5f) * step
                if (chroma) {
                    if (shrink) { val c = (f.toInt().coerceIn(0, max)) and 1.inv(); p0[i] = c; p1[i] = c; w[i] = 0; continue }
                    val g = f / 2f - 0.5f; val c0 = kotlin.math.floor(g).toInt(); val wt = ((g - c0) * 256f).toInt().coerceIn(0, 256)
                    val cmax = max / 2
                    p0[i] = c0.coerceIn(0, cmax) * 2; p1[i] = (c0 + 1).coerceIn(0, cmax) * 2; w[i] = wt
                } else if (shrink) {
                    p0[i] = (f - step / 4f).toInt().coerceIn(0, max); p1[i] = (f + step / 4f).toInt().coerceIn(0, max); w[i] = 128
                } else {
                    val g = f - 0.5f; val x0 = kotlin.math.floor(g).toInt(); val wt = ((g - x0) * 256f).toInt().coerceIn(0, 256)
                    p0[i] = x0.coerceIn(0, max); p1[i] = (x0 + 1).coerceIn(0, max); w[i] = wt
                }
            }
            return Taps(p0, p1, w)
        }

        /** Bilinear mix of a 2×2 neighbourhood with weights [wx], [wy] in 1/256. */
        fun mix(a: Int, b: Int, c: Int, d: Int, wx: Int, wy: Int): Int {
            val top = a * (256 - wx) + b * wx; val bot = c * (256 - wx) + d * wx
            return (top * (256 - wy) + bot * wy + 32768) shr 16
        }
    }
}

/** An ARGB image already in memory (the self-test's photo). */
class ArgbFrame(override val width: Int, override val height: Int, private val px: IntArray) : FrameRgb {
    override fun sampleArgb(src: BoxF, dstW: Int, dstH: Int, out: IntArray) {
        val ax = Taps.axis(src.x1, src.w / dstW, dstW, width, false); val ay = Taps.axis(src.y1, src.h / dstH, dstH, height, false)
        var k = 0
        for (j in 0 until dstH) {
            val r0 = ay.p0[j] * width; val r1 = ay.p1[j] * width; val wy = ay.w[j]
            for (i in 0 until dstW) {
                val a = px[r0 + ax.p0[i]]; val b = px[r0 + ax.p1[i]]; val c = px[r1 + ax.p0[i]]; val d = px[r1 + ax.p1[i]]; val wx = ax.w[i]
                val r = Taps.mix((a shr 16) and 0xFF, (b shr 16) and 0xFF, (c shr 16) and 0xFF, (d shr 16) and 0xFF, wx, wy)
                val g = Taps.mix((a shr 8) and 0xFF, (b shr 8) and 0xFF, (c shr 8) and 0xFF, (d shr 8) and 0xFF, wx, wy)
                val bl = Taps.mix(a and 0xFF, b and 0xFF, c and 0xFF, d and 0xFF, wx, wy)
                out[k++] = (0xFF shl 24) or (r shl 16) or (g shl 8) or bl
            }
        }
    }

    companion object {
        fun of(bmp: Bitmap): ArgbFrame {
            val px = IntArray(bmp.width * bmp.height)
            bmp.getPixels(px, 0, bmp.width, 0, 0, bmp.width, bmp.height)
            return ArgbFrame(bmp.width, bmp.height, px)
        }
    }
}
