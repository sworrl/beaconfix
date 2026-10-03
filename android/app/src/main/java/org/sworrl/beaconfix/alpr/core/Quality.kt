package org.sworrl.beaconfix.alpr.core

import kotlin.math.max
import kotlin.math.min

/**
 * How good one frame of a plate is, to pick a vehicle's best frame and weight its reads:
 * `q = plate height factor × Tenengrad sharpness of the 128×64 recognizer crop × mean slot confidence`.
 */
object PlateQuality {
    /** Plates this tall (frame pixels) or more fill the recognizer's 64 px input without upscaling. */
    const val FULL_HEIGHT = 64f
    /**
     * Tenengrad value at which sharpness is 0.5. Measured on synthetic 128×64 plates: ≈125 000 crisp, ≈56 000 after a
     * 4 px blur at 600 px plate width, ≈10 000 after a 10 px blur.
     */
    const val TENENGRAD_HALF = 30_000f

    /** Mean squared Sobel gradient magnitude of the luma of [argb] ([w]×[h]), border excluded. */
    fun tenengrad(argb: IntArray, w: Int, h: Int): Float {
        if (w < 3 || h < 3) return 0f
        val y = IntArray(w * h)
        for (i in 0 until w * h) { val c = argb[i]; y[i] = (((c shr 16) and 0xFF) * 77 + ((c shr 8) and 0xFF) * 150 + (c and 0xFF) * 29) shr 8 }
        var sum = 0.0
        for (j in 1 until h - 1) {
            val o = j * w
            for (i in 1 until w - 1) {
                val a = y[o - w + i - 1]; val b = y[o - w + i]; val c = y[o - w + i + 1]
                val d = y[o + i - 1]; val f = y[o + i + 1]
                val g = y[o + w + i - 1]; val hh = y[o + w + i]; val k = y[o + w + i + 1]
                val gx = (c + 2 * f + k) - (a + 2 * d + g)
                val gy = (g + 2 * hh + k) - (a + 2 * b + c)
                sum += (gx * gx + gy * gy).toDouble()
            }
        }
        return (sum / ((w - 2) * (h - 2))).toFloat()
    }

    fun sharpness(tenengrad: Float) = if (tenengrad <= 0f) 0f else tenengrad / (tenengrad + TENENGRAD_HALF)

    fun heightFactor(plateH: Float) = min(1f, max(0f, plateH) / FULL_HEIGHT)

    fun q(plateH: Float, tenengrad: Float, conf: Float) = heightFactor(plateH) * sharpness(tenengrad) * max(conf, 0.05f)
}

/**
 * Heat steps for the dash cam, from `PowerManager.getThermalHeadroom` (1.0 = the device starts throttling) and the
 * thermal status. Each step gives back work before Android throttles us: first the extras (accurate OCR, burst
 * re-scans, a column of tiles), then the frame rate and the full-frame model, then everything but a small full-frame
 * pass. Severe status still pauses capture (DashCamService).
 */
object ThermalPolicy {
    data class Step(
        val level: Int,
        /** Multiplier on the frame interval. */
        val intervalFactor: Int,
        val tileCols: Int,
        /** Run the full-frame pass on the small (416) detector. */
        val smallFullFrame: Boolean,
        val accurateOcr: Boolean,
        val burst: Boolean,
    )

    /** 0 normal, 1 headroom ≥ 0.85 (or status light), 2 ≥ 0.95 (or moderate), 3 ≥ 1.0. NaN / null headroom = unknown. */
    fun level(headroom: Float?, status: Int): Int {
        val h = when {
            headroom == null || headroom.isNaN() -> 0
            headroom >= 1.0f -> 3
            headroom >= 0.95f -> 2
            headroom >= 0.85f -> 1
            else -> 0
        }
        val s = when { status >= 3 -> 3; status == 2 -> 2; status == 1 -> 1; else -> 0 }
        return max(h, s)
    }

    fun step(level: Int, tileCols: Int, accurate: Boolean): Step = when (level) {
        0 -> Step(0, 1, tileCols, false, accurate, true)
        1 -> Step(1, 1, if (tileCols > 2) tileCols - 1 else tileCols, false, false, false)
        2 -> Step(2, 2, min(tileCols, 2), true, false, false)
        else -> Step(3, 4, 0, true, false, false)
    }
}
