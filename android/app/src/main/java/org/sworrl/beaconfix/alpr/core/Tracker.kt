package org.sworrl.beaconfix.alpr.core

import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min

/** One detection handed to the [Tracker]: its box (frame pixels), detector score and OCR text ("" when none). */
data class TrackInput(val box: BoxF, val score: Float, val text: String = "")

/** A tracked plate (one vehicle). Boxes are in frame pixels, velocities per second. */
class Track internal constructor(val id: Int, var box: BoxF, var lastMs: Long, val firstMs: Long, var text: String, var camAtObs: FloatArray) {
    var hits = 1; internal set
    /** Centre velocity (px/s) and height growth rate (d ln h / s), estimated from observations. */
    var vx = 0f; internal set
    var vy = 0f; internal set
    var vs = 0f; internal set
    /** Missed at least one update since its last observation. */
    var lost = false; internal set

    /** The box expected at [nowMs] under constant velocity and growth (prediction capped at [maxMs] ahead). */
    fun predict(nowMs: Long, maxMs: Long = 1_500): BoxF {
        val dt = min(max(0L, nowMs - lastMs), maxMs) / 1000f
        if (dt <= 0f || hits < 2) return box
        val k = exp((vs * dt).coerceIn(-1f, 1f))
        val w = box.w * k; val h = box.h * k
        val cx = box.cx + vx * dt; val cy = box.cy + vy * dt
        return BoxF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)
    }
}

/** Track id per input (-1 = not tracked: a low-score detection that matched nothing) and the tracks that ended. */
class TrackUpdate(val ids: IntArray, val ended: List<Track>)

/**
 * Plate tracker for 1–4 fps dash-cam frames.
 *
 * Ideas (no code copied): ByteTrack's two-stage association (Zhang et al. 2022, MIT): high-score detections first,
 * then low-score ones only against tracks that are still being followed, and only high-score ones start tracks;
 * C-BIoU's buffered boxes (Yang et al. 2023): both boxes are enlarged before IoU, more the longer the gap, so a plate
 * that moved a plate-width between frames still overlaps; OC-SORT's observation-centric re-update (Cao et al. 2023,
 * MIT): after a gap the velocity is re-estimated from the two real observations around it, not from the drifting
 * prediction. A track is kept for [lostMs] (seconds, not frames) after its last observation. The plate text is a
 * second cue: Jaro-Winkler ≥ 0.85 helps a match, clearly different texts block it, and a strong text match re-finds
 * a lost track even where the boxes no longer overlap. An optional camera rotation shift (gyro) is tried as a
 * second hypothesis beside the plain prediction, so a wrong-signed or missing gyro can never make matching worse.
 */
class Tracker(
    val lostMs: Long = 2_500,
    val highScore: Float = 0.5f,
    /** Unmatched detections at or above this start a track; below, only if they carry a read of ≥ 5 characters. */
    val newTrackScore: Float = 0.5f,
    /**
     * Buffer (fraction of the box size added on every side) at no gap, growth per second of gap, and its cap. At 1 fps a
     * plate of a car being passed moves 3–5 plate widths between frames, so the buffer has to grow much faster than
     * C-BIoU's 0.3 at video rates; the third stage doubles it once more.
     */
    val buffer: Float = 0.3f,
    val bufferPerSec: Float = 1.2f,
    val maxBuffer: Float = 2.5f,
    val minGeo: Float = 0.05f,
) {
    private val live = ArrayList<Track>()
    private var nextId = 1
    /** Accumulated camera shift (px) applied to the scene since the tracker started. */
    private val cam = FloatArray(2)

    val tracks: List<Track> get() = live

    /**
     * Associates [dets] (one frame at [nowMs]) with the live tracks. [shiftX]/[shiftY]: how far the scene moved in the
     * image since the previous update because the camera rotated (from the gyro; 0 when unknown). [scanned]: when only
     * parts of the frame were searched (a burst pass), tracks expected elsewhere are not counted as missed.
     */
    fun update(nowMs: Long, dets: List<TrackInput>, shiftX: Float = 0f, shiftY: Float = 0f, scanned: List<BoxF>? = null): TrackUpdate {
        cam[0] += shiftX; cam[1] += shiftY
        val ended = live.filter { nowMs - it.lastMs > lostMs }
        live.removeAll(ended.toSet())
        val ids = IntArray(dets.size) { -1 }
        val freeT = live.toMutableSet()
        val freeD = dets.indices.toMutableSet()

        fun stage(dIdx: Collection<Int>, tracks: Collection<Track>, bufferScale: Float) {
            val pairs = ArrayList<Triple<Float, Int, Track>>()
            for (d in dIdx) for (t in tracks) { val s = pairScore(t, dets[d], nowMs, bufferScale); if (s > 0f) pairs += Triple(s, d, t) }
            pairs.sortByDescending { it.first }
            for ((_, d, t) in pairs) {
                if (d !in freeD || t !in freeT) continue
                freeD -= d; freeT -= t; ids[d] = t.id
                observe(t, dets[d], nowMs)
            }
        }
        val high = dets.indices.filter { dets[it].score >= highScore }
        val low = dets.indices.filter { dets[it].score < highScore }
        stage(high, freeT.toList(), 1f)                                           // 1: confident detections vs every track
        stage(low.filter { it in freeD }, freeT.filter { !it.lost }, 1f)          // 2: weak detections vs followed tracks
        stage(high.filter { it in freeD }, freeT.toList(), 2f)                    // 3: C-BIoU cascade, larger buffer
        for (t in freeT) {
            val p = t.predict(nowMs)
            if (scanned == null || scanned.any { p.cx in it.x1..it.x2 && p.cy in it.y1..it.y2 }) t.lost = true
        }
        for (d in freeD.sorted()) {
            val x = dets[d]
            if (x.score < newTrackScore && x.text.length < 5) continue
            val t = Track(nextId++, x.box, nowMs, nowMs, x.text.takeIf { it.length >= 4 }.orEmpty(), cam.copyOf())
            live += t; ids[d] = t.id
        }
        return TrackUpdate(ids, ended)
    }

    /** Ends every track (e.g. the camera stopped). */
    fun clear(): List<Track> = live.toList().also { live.clear() }

    private fun observe(t: Track, d: TrackInput, nowMs: Long) {
        val dt = (nowMs - t.lastMs) / 1000f
        if (dt > 0f) {
            // OC-SORT re-update: after a gap the observations alone define the velocity; else smooth it
            val nvx = (d.box.cx - t.box.cx) / dt; val nvy = (d.box.cy - t.box.cy) / dt
            val nvs = (ln(max(1f, d.box.h) / max(1f, t.box.h)) / dt).coerceIn(-2f, 2f)
            val a = if (t.lost || t.hits < 2) 1f else 0.6f
            t.vx = a * nvx + (1 - a) * t.vx; t.vy = a * nvy + (1 - a) * t.vy; t.vs = a * nvs + (1 - a) * t.vs
        }
        t.box = d.box; t.lastMs = nowMs; t.hits++; t.lost = false; t.camAtObs = cam.copyOf()
        if (d.text.length >= 4) t.text = d.text
    }

    /** > 0 when [d] may continue [t]; higher is better. */
    private fun pairScore(t: Track, d: TrackInput, nowMs: Long, bufferScale: Float): Float {
        val dtSec = max(0L, nowMs - t.lastMs) / 1000f
        val b = min(maxBuffer, (buffer + bufferPerSec * dtSec) * bufferScale)
        val pred = t.predict(nowMs)
        val sx = cam[0] - t.camAtObs[0]; val sy = cam[1] - t.camAtObs[1]
        val hyps = if (sx != 0f || sy != 0f) listOf(pred, t.box, pred.offset(sx, sy), t.box.offset(sx, sy)) else listOf(pred, t.box)
        var geo = 0f
        for (h in hyps) {
            val ratio = d.box.h / max(1f, h.h)
            if (ratio < 0.4f || ratio > 2.5f) continue
            geo = max(geo, expand(h, b).iou(expand(d.box, b)))
        }
        val jw = if (t.text.length >= 4 && d.text.length >= 4) PlateSimilarity.jaroWinkler(t.text, d.text) else -1.0
        val textTerm = when { jw < 0 -> 0f; jw >= 0.85 -> 0.3f; jw < 0.6 -> -0.4f; else -> 0f }
        if (geo >= minGeo) return max(0f, geo + textTerm)
        // re-identification by text alone: the plate is where the boxes no longer overlap (camera turned, long gap)
        if (jw >= 0.9 && t.text.length >= 5 && d.text.length >= 5) return 0.01f + 0.04f * jw.toFloat()
        return 0f
    }

    companion object {
        /** [b] grown by [k] of its size on every side (C-BIoU's buffer). */
        fun expand(b: BoxF, k: Float): BoxF { val dx = b.w * k; val dy = b.h * k; return BoxF(b.x1 - dx, b.y1 - dy, b.x2 + dx, b.y2 + dy) }
    }
}

/** Camera rotation → image shift, for the tracker's motion compensation. */
object CameraMotion {
    /**
     * Pixel shift (dx right, dy down) of the scene in the upright frame when the phone turned by [ax]/[ay] radians
     * about its own x / y axes (Android sensor axes; counter-clockwise positive) while the frame is upright for display
     * rotation [rotation] (Surface.ROTATION_0..270 as 0..3). [fPx]: focal length in frame pixels.
     * Turning left (counter-clockwise about the image's up axis) moves the scene right; tilting up moves it down.
     */
    fun pixelShift(ax: Float, ay: Float, rotation: Int, fPx: Float): Pair<Float, Float> {
        // image-up and image-right axes in device coordinates per display rotation; dx = f·(ω·up), dy = f·(ω·right)
        val (up, right) = when (rotation and 3) {
            1 -> floatArrayOf(1f, 0f) to floatArrayOf(0f, -1f)      // ROTATION_90: top of the phone points left
            2 -> floatArrayOf(0f, -1f) to floatArrayOf(-1f, 0f)
            3 -> floatArrayOf(-1f, 0f) to floatArrayOf(0f, 1f)      // ROTATION_270: top points right
            else -> floatArrayOf(0f, 1f) to floatArrayOf(1f, 0f)
        }
        val dx = fPx * (ax * up[0] + ay * up[1])
        val dy = fPx * (ax * right[0] + ay * right[1])
        return dx to dy
    }

    /**
     * Focal length in frame pixels from the lens focal length and the sensor's physical width (both mm; the sensor's
     * long side) for a buffer whose long side is [bufferLongPx] (pixels are square, so it holds for both axes).
     */
    fun focalPx(focalMm: Float, sensorWidthMm: Float, bufferLongPx: Int): Float =
        if (focalMm > 0f && sensorWidthMm > 0f) focalMm / sensorWidthMm * bufferLongPx else DEFAULT_FOCAL_RATIO * bufferLongPx

    /** ≈ 70° horizontal field of view: 1 / (2·tan 35°). */
    const val DEFAULT_FOCAL_RATIO = 0.714f
}
