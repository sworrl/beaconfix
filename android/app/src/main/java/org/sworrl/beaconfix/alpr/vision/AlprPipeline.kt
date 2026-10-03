package org.sworrl.beaconfix.alpr.vision

import ai.onnxruntime.OrtEnvironment
import android.content.Context
import android.graphics.Bitmap
import android.location.Location
import android.os.SystemClock
import android.util.Log
import org.sworrl.beaconfix.alpr.core.AlprJson
import org.sworrl.beaconfix.alpr.core.BoxF
import org.sworrl.beaconfix.alpr.core.BurstPlanner
import org.sworrl.beaconfix.alpr.core.CropMath
import org.sworrl.beaconfix.alpr.core.Detection
import org.sworrl.beaconfix.alpr.core.FrameMeta
import org.sworrl.beaconfix.alpr.core.HotlistMatch
import org.sworrl.beaconfix.alpr.core.HotlistMatcher
import org.sworrl.beaconfix.alpr.core.MatchKind
import org.sworrl.beaconfix.alpr.core.OnDevice
import org.sworrl.beaconfix.alpr.core.OnDevicePlate
import org.sworrl.beaconfix.alpr.core.PendingEvent
import org.sworrl.beaconfix.alpr.core.PlateFormats
import org.sworrl.beaconfix.alpr.core.PlateGroups
import org.sworrl.beaconfix.alpr.core.PlateLattice
import org.sworrl.beaconfix.alpr.core.PlateQuality
import org.sworrl.beaconfix.alpr.core.PlateRead
import org.sworrl.beaconfix.alpr.core.SlotDto
import org.sworrl.beaconfix.alpr.core.TrackInput
import org.sworrl.beaconfix.alpr.core.Tracker
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.roundToInt

/** Where and when a frame was taken. [location] is used only when fresh. */
data class FrameContext(val capturedMs: Long, val location: Location?, val regionCode: String?)

/** Knobs for one pass (settings after the thermal step). */
data class PassOptions(
    val tileCols: Int = 3,
    /** Tiles scanned per frame, round robin; 0 = as many as it takes to sweep them all in [tileCols] frames. */
    val tilesPerFrame: Int = 0,
    val minScore: Float = 0.4f,
    /** Full-frame pass on the small (416) detector (heat). */
    val smallFullFrame: Boolean = false,
    /** Re-read a vehicle's new best frame with the accurate recognizer (when it is loaded). */
    val accurateOcr: Boolean = false,
    /** Camera rotation since the last frame, as an image shift in pixels (gyro; 0 = unknown). */
    val shiftX: Float = 0f,
    val shiftY: Float = 0f,
    /** False for the self-test: nothing is kept for upload. */
    val makeEvents: Boolean = true,
)

/** A plate in this frame: the raw read, its vehicle's [track] and fused text, and that vehicle's hotlist matches. */
data class PlateSeen(val det: Detection, val read: PlateRead?, val matches: List<HotlistMatch> = emptyList(), val own: Boolean = false, val track: Int = -1, val fused: String = "")

/** One vehicle, emitted once, from its best frame. */
data class VehicleEvent(val event: PendingEvent, val text: String, val conf: Float, val frames: Int, val accepted: Boolean, val matches: List<HotlistMatch>, val thumb: Bitmap?)

/** A hotlist alert for a vehicle, raised as soon as its fused read supports it (before the vehicle's event). */
data class PlateAlert(val text: String, val conf: Float, val match: HotlistMatch, val thumb: Bitmap?, val atMs: Long)

data class FrameOutcome(
    val plates: List<PlateSeen>, val events: List<VehicleEvent>, val alerts: List<PlateAlert>,
    val detectMs: Int, val ocrMs: Int, val totalMs: Int, val tracks: Int = 0,
)

/**
 * Per frame: detect plates (full frame + tiles), read the ones big enough, follow each plate from frame to frame
 * ([Tracker]), fuse each vehicle's reads ([PlateGroups] / TrackFusion) and match the fused candidate lattice against
 * the hotlist. Each vehicle becomes one event, from its best frame (q = height × sharpness × confidence), when it
 * leaves view; a confirmed hotlist vehicle immediately. Runs on the analysis thread; the frame is only sampled, never
 * stored (a vehicle's best crop is kept as a JPEG in memory until its event is made).
 */
class AlprPipeline(ctx: Context, threads: Int = 4, xnnpack: Boolean = false, accurate: Boolean = false) : AutoCloseable {
    private val env = OrtEnvironment.getEnvironment()
    private val detector = PlateDetector(env, AlprModels.load(ctx, AlprModels.DETECTOR), threads, xnnpack, 640)
    /** The 416 px detector for tiles and bursts; null (→ the 640 one) if it could not be loaded. */
    private val small: PlateDetector? = runCatching { PlateDetector(env, AlprModels.load(ctx, AlprModels.DETECTOR_TILE), threads, xnnpack, 416) }
        .onFailure { Log.w(TAG, "small detector unavailable, tiles use the 640 model", it) }.getOrNull()
    private val ocr = PlateOcr(env, AlprModels.load(ctx, AlprModels.OCR), threads, xnnpack)
    private val accurateOcr: PlateOcr? = if (!accurate) null else runCatching { PlateOcr(env, AlprModels.load(ctx, AlprModels.OCR_ACCURATE), threads, xnnpack) }
        .onFailure { Log.w(TAG, "accurate recognizer unavailable", it) }.getOrNull()
    private val tracker = Tracker()
    private val groups = PlateGroups<Shot>()
    private val planner = BurstPlanner()

    /** Sampling vs model time of the last frame's detector passes (ms). */
    var lastPrepMs = 0L; private set
    var lastRunMs = 0L; private set
    val hasAccurateOcr get() = accurateOcr != null

    private var nextTile = 0

    /** A vehicle's best frame so far: the upload crop, a thumbnail and the frame's metadata. */
    private class Shot(val jpeg: ByteArray, val thumb: Bitmap, val capturedMs: Long, val meta: FrameMeta, val plateBox: List<Double>)

    fun process(frame: FrameRgb, fc: FrameContext, matcher: HotlistMatcher, opt: PassOptions = PassOptions()): FrameOutcome {
        val t0 = SystemClock.elapsedRealtime()
        val dets = ArrayList<Detection>()
        var prep = 0L; var run = 0L
        val full = if (opt.smallFullFrame && small != null) small else detector
        dets += full.detect(frame, BoxF(0f, 0f, frame.width.toFloat(), frame.height.toFloat()), opt.minScore); prep += full.prepMs; run += full.runMs
        val tileModel = small ?: detector
        val all = CropMath.tilesFor(frame.width, frame.height, opt.tileCols, tileModel.size)
        val per = if (opt.tilesPerFrame > 0) opt.tilesPerFrame else ceil(all.size / max(1, opt.tileCols).toFloat()).toInt()
        val pick = if (per >= all.size) all else List(per) { all[(nextTile + it) % all.size] }.also { nextTile = (nextTile + per) % all.size }
        for (t in pick) { dets += tileModel.detect(frame, t.toF(), opt.minScore); prep += tileModel.prepMs; run += tileModel.runMs }
        lastPrepMs = prep; lastRunMs = run
        return analyse(frame, fc, matcher, CropMath.nms(dets), opt, t0)
    }

    /** Windows to re-scan in a frame the rate limit would skip (tracked plates still too small to read). */
    fun burstWindows(nowMs: Long, frameW: Int, frameH: Int): List<BoxF> = planner.plan(nowMs, tracker.tracks, frameW, frameH)

    /** A burst pass: only [windows], on the small detector at full resolution. */
    fun burst(frame: FrameRgb, fc: FrameContext, matcher: HotlistMatcher, windows: List<BoxF>, opt: PassOptions): FrameOutcome {
        val t0 = SystemClock.elapsedRealtime()
        val m = small ?: detector
        val dets = ArrayList<Detection>()
        for (w in windows) dets += m.detect(frame, w, opt.minScore)
        return analyse(frame, fc, matcher, CropMath.nms(dets), opt, t0, windows)
    }

    private fun analyse(frame: FrameRgb, fc: FrameContext, matcher: HotlistMatcher, merged: List<Detection>, opt: PassOptions, t0: Long, scanned: List<BoxF>? = null): FrameOutcome {
        val t1 = SystemClock.elapsedRealtime()
        val now = fc.capturedMs
        val readable = merged.indices.filter { CropMath.readable(merged[it].box) }
        val res = arrayOfNulls<OcrResult>(merged.size)
        ocr.read(frame, readable.map { merged[it].box }).forEachIndexed { k, r -> res[readable[k]] = r }
        fun gated(r: PlateRead?) = r?.takeIf { PlateFormats.gate(it.text, it.regionProb) }
        val upd = tracker.update(now, merged.mapIndexed { i, d -> TrackInput(d.box, d.score, gated(res[i]?.read)?.text.orEmpty()) }, opt.shiftX, opt.shiftY, scanned)
        for (t in upd.ended) groups.trackEnded(t.id)
        // the accurate recognizer only on frames that beat their vehicle's best (≈ once or twice per vehicle)
        val acc = accurateOcr
        if (opt.accurateOcr && acc != null) {
            val best = readable.filter { upd.ids[it] >= 0 }
                .map { it to PlateQuality.q(merged[it].box.h, res[it]!!.tenengrad, res[it]!!.read.conf) }
                .filter { (i, q) -> q > groups.bestQuality(upd.ids[i]) * 1.1f }
                .sortedByDescending { it.second }.take(2).map { it.first }
            if (best.isNotEmpty()) acc.read(frame, best.map { merged[it].box }).forEachIndexed { k, r -> res[best[k]] = r }
        }
        val t2 = SystemClock.elapsedRealtime()

        val touched = LinkedHashSet<PlateGroups.Group<Shot>>()
        for (i in readable) {
            val id = upd.ids[i]; val r = res[i] ?: continue
            if (id < 0) continue
            val q = PlateQuality.q(merged[i].box.h, r.tenengrad, r.read.conf)
            val (g, best) = groups.observe(id, gated(r.read), q, now, fc.regionCode)
            if (best && opt.makeEvents && !g.emitted) groups.setShot(g, shot(frame, fc, merged[i].box))
            touched += g
        }

        val alerts = ArrayList<PlateAlert>()
        val events = ArrayList<VehicleEvent>()
        val matchesOf = HashMap<PlateGroups.Group<Shot>, List<HotlistMatch>>()
        for (g in touched) {
            val f = g.fused ?: continue
            g.own = g.own || matcher.isOwn(f.text, f.lattice)
            if (g.own) continue
            val ms = matcher.match(f.text, f.lattice)
            matchesOf[g] = ms
            alerts += newAlerts(g, ms, now, atEmit = false)
            if (opt.makeEvents && g.accepted && !g.emitted && ms.any { it.kind == MatchKind.EXACT }) events += listOfNotNull(emit(g, ms))
        }
        if (opt.makeEvents) for (d in groups.due(now)) {
            val g = d.group
            if (g.own) { groups.markEmitted(g); continue }
            val ms = matchesOf[g] ?: g.fused?.let { matcher.match(it.text, it.lattice) }.orEmpty()
            alerts += newAlerts(g, ms, now, atEmit = true)
            events += listOfNotNull(emit(g, ms))
        }

        val seen = merged.mapIndexed { i, d ->
            val r = res[i]?.read
            val g = upd.ids[i].takeIf { it >= 0 }?.let { groups.groupOf(it) }
            val own = g?.own == true || (r != null && r.text.length >= 2 && matcher.isOwn(r.text, PlateLattice.of(r)))
            val ms = when {
                own -> emptyList()
                g?.fused != null -> matchesOf[g] ?: matcher.match(g.text, g.fused!!.lattice)
                r != null && r.text.length >= 2 -> matcher.match(r.text, PlateLattice.of(r))
                else -> emptyList()
            }
            PlateSeen(d, r, ms, own, upd.ids[i], g?.text.orEmpty())
        }
        return FrameOutcome(seen, events, alerts, (t1 - t0).toInt(), (t2 - t1).toInt(), (SystemClock.elapsedRealtime() - t0).toInt(), tracker.tracks.size)
    }

    /**
     * Alerts not yet raised for [g]: an exact match once the vehicle is accepted; an exact text not (yet) confirmed by
     * the accept rules, and a lattice alternative of an accepted vehicle, as "possible"; any remaining possible match
     * when the vehicle is emitted.
     */
    private fun newAlerts(g: PlateGroups.Group<Shot>, ms: List<HotlistMatch>, now: Long, atEmit: Boolean): List<PlateAlert> {
        val f = g.fused ?: return emptyList()
        val out = ArrayList<PlateAlert>()
        for (m in ms) {
            val kind = if (m.kind == MatchKind.EXACT && g.accepted) MatchKind.EXACT else MatchKind.POSSIBLE
            if (kind == MatchKind.POSSIBLE && m.kind == MatchKind.POSSIBLE && !g.accepted && !atEmit) continue
            if (!g.alerted.add("$kind:${m.entry.id}:${m.candidate}")) continue
            out += PlateAlert(f.text, f.conf, m.copy(kind = kind), g.shot?.thumb, now)
        }
        return out
    }

    private fun emit(g: PlateGroups.Group<Shot>, ms: List<HotlistMatch>): VehicleEvent? {
        val shot = g.shot
        val f = g.fused
        groups.markEmitted(g)
        shot ?: return null
        val hot = ms.any { it.kind == MatchKind.EXACT }
        val plate = OnDevicePlate(shot.plateBox, f?.text.orEmpty(), (f?.conf ?: 0f).toDouble().r3(),
            f?.lattice?.slots.orEmpty().map { s -> s.map { SlotDto(it.c.toString(), it.p.toDouble().r3()) } })
        val meta = shot.meta.copy(reason = if (hot) FrameMeta.REASON_HOTLIST else FrameMeta.REASON_PLATE, onDevice = OnDevice(listOf(plate)))
        return VehicleEvent(PendingEvent(AlprJson.newId(), shot.capturedMs, meta, shot.jpeg), f?.text.orEmpty(), f?.conf ?: 0f,
            g.readableFrames, g.accepted, ms, shot.thumb)
    }

    private fun shot(frame: FrameRgb, fc: FrameContext, box: BoxF): Shot {
        val crop = CropMath.vehicleCrop(box, frame.width, frame.height)
        val (ow, oh) = CropMath.outputSize(crop.w, crop.h)
        val jpeg = frame.jpeg(crop.toF(), ow, oh, 90)
        val loc = fc.location?.takeIf { System.currentTimeMillis() - it.time < 60_000 }
        val meta = FrameMeta(
            capturedAt = AlprJson.rfc3339(fc.capturedMs),
            lat = loc?.latitude, lon = loc?.longitude,
            accuracyM = loc?.takeIf { it.hasAccuracy() }?.accuracy?.toDouble()?.r1(),
            speedMps = loc?.takeIf { it.hasSpeed() }?.speed?.toDouble()?.r1(),
            headingDeg = loc?.takeIf { it.hasBearing() }?.bearing?.toDouble()?.r1(),
            altitudeM = loc?.takeIf { it.hasAltitude() }?.altitude?.r1(),
            regionCode = fc.regionCode,
            cropBox = CropMath.normalize(crop, frame.width, frame.height),
            frameSize = listOf(frame.width, frame.height),
        )
        return Shot(jpeg, thumb(frame, box), fc.capturedMs, meta, CropMath.normalize(box, crop))
    }

    /** Capture stopped: the vehicles still being followed become their events now (their tracks are forgotten). */
    fun flush(matcher: HotlistMatcher): FrameOutcome {
        tracker.clear(); planner.clear()
        val now = System.currentTimeMillis()
        val alerts = ArrayList<PlateAlert>(); val events = ArrayList<VehicleEvent>()
        for (d in groups.flush()) {
            val g = d.group
            if (g.own) continue
            val ms = g.fused?.let { matcher.match(it.text, it.lattice) }.orEmpty()
            alerts += newAlerts(g, ms, now, atEmit = true)
            events += listOfNotNull(emit(g, ms))
        }
        return FrameOutcome(emptyList(), events, alerts, 0, 0, 0, 0)
    }

    /** A small picture of the plate and a bit around it, for the status card and the alert (memory only). */
    fun thumb(frame: FrameRgb, b: BoxF): Bitmap {
        val pad = BoxF(b.x1 - b.w * 0.6f, b.y1 - b.h * 1.2f, b.x2 + b.w * 0.6f, b.y2 + b.h * 1.2f).clamp(frame.width, frame.height)
        val w = 320; val h = max(1, (w * pad.h / max(1f, pad.w)).roundToInt()).coerceAtMost(320)
        return frame.bitmap(pad, w, h)
    }

    override fun close() { detector.close(); small?.close(); ocr.close(); accurateOcr?.close() }

    private fun Double.r1() = Math.round(this * 10.0) / 10.0
    private fun Double.r3() = Math.round(this * 1000.0) / 1000.0

    companion object {
        private const val TAG = "AlprPipeline"

        /** OCR text for display ("" → "unreadable"). */
        fun label(r: PlateRead?) = r?.text?.takeIf { it.isNotEmpty() } ?: "unreadable"
    }
}
