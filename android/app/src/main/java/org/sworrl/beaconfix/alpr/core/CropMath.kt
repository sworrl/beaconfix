package org.sworrl.beaconfix.alpr.core

import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/** An axis-aligned box in pixels (float) of some image; x2/y2 exclusive. */
data class BoxF(val x1: Float, val y1: Float, val x2: Float, val y2: Float) {
    val w get() = x2 - x1
    val h get() = y2 - y1
    val cx get() = (x1 + x2) / 2f
    val cy get() = (y1 + y2) / 2f
    val area get() = max(0f, w) * max(0f, h)
    fun iou(o: BoxF): Float {
        val ix = max(0f, min(x2, o.x2) - max(x1, o.x1)); val iy = max(0f, min(y2, o.y2) - max(y1, o.y1))
        val inter = ix * iy; val u = area + o.area - inter
        return if (u <= 0f) 0f else inter / u
    }
    fun clamp(w: Int, h: Int) = BoxF(x1.coerceIn(0f, w.toFloat()), y1.coerceIn(0f, h.toFloat()), x2.coerceIn(0f, w.toFloat()), y2.coerceIn(0f, h.toFloat()))
    fun offset(dx: Float, dy: Float) = BoxF(x1 + dx, y1 + dy, x2 + dx, y2 + dy)
    fun inside(o: IntBox) = x1 >= o.x1 && y1 >= o.y1 && x2 <= o.x2 && y2 <= o.y2
}

/** An integer pixel rectangle; x2/y2 exclusive. */
data class IntBox(val x1: Int, val y1: Int, val x2: Int, val y2: Int) {
    val w get() = x2 - x1
    val h get() = y2 - y1
    fun toF() = BoxF(x1.toFloat(), y1.toFloat(), x2.toFloat(), y2.toFloat())
}

/** One plate detection in full-frame pixels. */
data class Detection(val box: BoxF, val score: Float)

/** Letterbox parameters (YOLO preprocessing): scale [r], padding [dw]/[dh] in model pixels. */
data class Letterbox(val r: Float, val dw: Float, val dh: Float, val newW: Int, val newH: Int, val size: Int) {
    /** A box in model input pixels → source pixels (relative to the source region's origin). */
    fun toSource(b: BoxF) = BoxF((b.x1 - dw) / r, (b.y1 - dh) / r, (b.x2 - dw) / r, (b.y2 - dh) / r)
    /** Left/top padding as drawn (matches open-image-models' rounding). */
    val padLeft get() = (dw - 0.1f).roundToInt()
    val padTop get() = (dh - 0.1f).roundToInt()
}

/**
 * The geometry of the dash-cam pipeline, pure so it is unit-tested: letterboxing for the detector, the tiles a large
 * frame is cut into (a 12 MP frame squeezed into 640 px would shrink a distant plate to a few pixels), NMS across tiles,
 * and the crop around the vehicle that is uploaded to FalconEyez.
 */
object CropMath {
    /** Vehicle crop ≈ 4× the plate width and 6× the plate height (the brief), with the plate in the lower part. */
    const val WIDTH_FACTOR = 4f
    const val HEIGHT_FACTOR = 6f
    /** Where the plate centre sits in the crop, from the top (a plate is low on a vehicle: most of the car is above it). */
    const val PLATE_V_POS = 0.65f
    /** A crop smaller than this (short side) gives the server too little to work with; grown to at least this. */
    const val MIN_SIDE = 384
    /** Long side of the uploaded JPEG. */
    const val MAX_LONG = 2048
    /** OCR is pointless below this: plate characters are ~45 % of the plate height, so 22 px ≈ 10 px characters. */
    const val MIN_PLATE_W = 40f
    const val MIN_PLATE_H = 22f

    fun letterbox(srcW: Int, srcH: Int, size: Int): Letterbox {
        val r = min(size.toFloat() / srcW, size.toFloat() / srcH)
        val nw = (srcW * r).roundToInt(); val nh = (srcH * r).roundToInt()
        return Letterbox(r, (size - nw) / 2f, (size - nh) / 2f, nw, nh, size)
    }

    fun readable(b: BoxF) = b.w >= MIN_PLATE_W && b.h >= MIN_PLATE_H

    /**
     * The crop around the vehicle that carries [plate], clamped to the frame ([frameW]×[frameH]); it always contains
     * the plate (sizes are never below the plate's own, and clamping shifts before it clips).
     */
    fun vehicleCrop(plate: BoxF, frameW: Int, frameH: Int): IntBox {
        var cw = max(plate.w * WIDTH_FACTOR, MIN_SIDE.toFloat())
        var ch = max(plate.h * HEIGHT_FACTOR, MIN_SIDE * 0.75f)
        cw = min(cw, frameW.toFloat()); ch = min(ch, frameH.toFloat())
        var x1 = plate.cx - cw / 2f
        var y1 = plate.cy - ch * PLATE_V_POS
        // the plate must stay inside even when the crop had to be shrunk to the frame
        x1 = min(x1, plate.x1); x1 = max(x1, plate.x2 - cw)
        y1 = min(y1, plate.y1); y1 = max(y1, plate.y2 - ch)
        x1 = x1.coerceIn(0f, frameW - cw); y1 = y1.coerceIn(0f, frameH - ch)
        val iw = ceil(cw).toInt().coerceAtMost(frameW); val ih = ceil(ch).toInt().coerceAtMost(frameH)
        val ix1 = x1.toInt().coerceIn(0, frameW - iw); val iy1 = y1.toInt().coerceIn(0, frameH - ih)
        return IntBox(ix1, iy1, ix1 + iw, iy1 + ih)
    }

    /** Smallest box containing both crops (for plates of one vehicle / adjacent vehicles in one upload). */
    fun union(a: IntBox, b: IntBox) = IntBox(min(a.x1, b.x1), min(a.y1, b.y1), max(a.x2, b.x2), max(a.y2, b.y2))

    /** Output size for a [cropW]×[cropH] crop: never upscaled, long side ≤ [maxLong]. */
    fun outputSize(cropW: Int, cropH: Int, maxLong: Int = MAX_LONG): Pair<Int, Int> {
        val long = max(cropW, cropH)
        if (long <= maxLong) return cropW to cropH
        val s = maxLong.toFloat() / long
        return max(1, (cropW * s).roundToInt()) to max(1, (cropH * s).roundToInt())
    }

    /** [b] (frame pixels) normalized to 0..1 within [within] (frame pixels), rounded to 4 decimals. */
    fun normalize(b: BoxF, within: IntBox): List<Double> = listOf(
        ((b.x1 - within.x1) / within.w).toDouble(), ((b.y1 - within.y1) / within.h).toDouble(),
        ((b.x2 - within.x1) / within.w).toDouble(), ((b.y2 - within.y1) / within.h).toDouble(),
    ).map { r4(it.coerceIn(0.0, 1.0)) }

    fun normalize(b: IntBox, frameW: Int, frameH: Int): List<Double> =
        listOf(b.x1.toDouble() / frameW, b.y1.toDouble() / frameH, b.x2.toDouble() / frameW, b.y2.toDouble() / frameH).map { r4(it.coerceIn(0.0, 1.0)) }

    private fun r4(v: Double) = Math.round(v * 10_000.0) / 10_000.0

    /**
     * One row of [cols] square detector tiles across the frame, centred at [centerY] (fraction of the height: on a dash
     * the plates of traffic ahead sit just below the middle), overlapping by [overlap] of a tile so a plate on a seam
     * is whole in one of them. For a 4080×3072 frame and 3 columns: three 1458 px tiles, each seen at 0.44 scale (the
     * full-frame pass sees it at 0.16). Empty when the full-frame pass alone already sees the frame at ≥ 2/3 scale.
     */
    fun tiles(frameW: Int, frameH: Int, cols: Int = 3, centerY: Float = 0.6f, overlap: Float = 0.1f, modelSize: Int = 640): List<IntBox> {
        if (cols <= 0 || max(frameW, frameH) <= modelSize * 1.5f) return emptyList()
        val side = min(frameH, ceil(frameW / (cols - (cols - 1) * overlap)).toInt()).coerceAtMost(frameW)
        val y = ((frameH * centerY) - side / 2f).roundToInt().coerceIn(0, frameH - side)
        return (0 until cols).map { c ->
            val x = if (cols == 1) (frameW - side) / 2 else ((frameW - side).toFloat() * c / (cols - 1)).roundToInt()
            IntBox(x, y, x + side, y + side)
        }
    }

    /**
     * Tiles for a detector of [modelSize] px that see the frame at the same scale as [cols] tiles would on the 640 px
     * model: a smaller model gets proportionally smaller tiles, and as many columns as it takes to cover the width
     * with at least [overlap]. (4080×3072, 3 columns, 416 model: five 948 px tiles at the same 0.44 scale.)
     */
    fun tilesFor(frameW: Int, frameH: Int, cols: Int, modelSize: Int, centerY: Float = 0.6f, overlap: Float = 0.1f): List<IntBox> {
        if (modelSize >= 640) return tiles(frameW, frameH, cols, centerY, overlap)
        if (cols <= 0 || max(frameW, frameH) <= 640 * 1.5f) return emptyList()
        val side640 = min(frameH, ceil(frameW / (cols - (cols - 1) * overlap)).toInt()).coerceAtMost(frameW)
        val side = (side640 * modelSize / 640f).roundToInt().coerceAtLeast(modelSize).coerceAtMost(min(frameW, frameH))
        val n = max(1, ceil((frameW.toFloat() / side - overlap) / (1 - overlap)).toInt())
        val y = ((frameH * centerY) - side / 2f).roundToInt().coerceIn(0, frameH - side)
        return (0 until n).map { c ->
            val x = if (n == 1) (frameW - side) / 2 else ((frameW - side).toFloat() * c / (n - 1)).roundToInt()
            IntBox(x, y, x + side, y + side)
        }
    }

    /** Greedy non-maximum suppression (highest score first). */
    fun nms(dets: List<Detection>, iou: Float = 0.45f): List<Detection> {
        val sorted = dets.sortedByDescending { it.score }
        val keep = ArrayList<Detection>()
        for (d in sorted) if (keep.none { it.box.iou(d.box) > iou || contains(it.box, d.box) }) keep += d
        return keep
    }

    /** [a] (nearly) contains [b]: a tile seam can give one whole and one cut-off box of the same plate. */
    private fun contains(a: BoxF, b: BoxF): Boolean {
        val ix = max(0f, min(a.x2, b.x2) - max(a.x1, b.x1)); val iy = max(0f, min(a.y2, b.y2) - max(a.y1, b.y1))
        return b.area > 0 && ix * iy / b.area > 0.8f
    }
}
