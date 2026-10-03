package org.sworrl.beaconfix.alpr.core

import kotlin.math.max
import kotlin.math.min

/**
 * Short-shutter exposure for plate reading. Auto exposure stretches the shutter to tens of ms when light drops, and a
 * plate passing at road speed then smears over several characters. This keeps the shutter at about 1/1000 s by day and
 * at most 1/250 s at night (1/500 s above [fastSpeedMps]), and raises ISO to keep the brightness auto exposure wanted:
 * sensor noise hurts the recognizer far less than motion blur.
 *
 * The idea of plates-tracker's `ExposureController` (mssdvd/plates-tracker-public, MIT): stay on AUTO while AE's own
 * shutter is short enough; switch to MANUAL (debounced) when it would exceed the cap, seeding ISO from AE's
 * exposure × ISO product; then close the loop on the frame's mean luma, and hand back to AUTO when the scene is bright
 * at the lowest ISO. Reimplemented here, with the day/night/speed caps added. Pure logic (no camera calls), so tested.
 */
class ShutterPolicy(
    val isoMin: Int = 100,
    val isoMax: Int = 3200,
    val expMinNs: Long = 100_000,
    val expMaxNs: Long = 100_000_000,
    val dayNs: Long = 1_000_000,
    val nightCapNs: Long = 4_000_000,
    val fastNightCapNs: Long = 2_000_000,
    val fastSpeedMps: Float = 15f,
    /** Up to this ISO the shutter stays at [dayNs]; beyond it the shutter lengthens toward the night cap first. */
    val isoComfort: Int = 800,
    val debounce: Int = 3,
    val lumaLow: Int = 90,
    val lumaHigh: Int = 170,
) {
    sealed class Command {
        object Auto : Command()
        data class Manual(val exposureNs: Long, val iso: Int) : Command()
    }

    var manual = false; private set
    /** Brightness wanted, as exposure (ns) × ISO. */
    var product = 0.0; private set
    var current: Command = Command.Auto; private set
    private var over = 0

    /** An AE result (exposure time, ISO) while on AUTO; returns a command when it is time to switch. */
    @Synchronized fun onAutoResult(exposureNs: Long, iso: Int, speedMps: Float): Command? {
        if (manual || exposureNs <= 0 || iso <= 0) return null
        if (exposureNs <= dayNs) { over = 0; return null }
        if (++over < debounce) return null
        over = 0
        product = exposureNs.toDouble() * iso
        manual = true
        return manualCommand(speedMps)
    }

    /** Mean luma (0..255) of an analysed frame; while MANUAL, nudges the brightness or hands back to AUTO. */
    @Synchronized fun onLuma(luma: Int, speedMps: Float): Command? {
        if (!manual) return null
        val m = current as? Command.Manual
        return when {
            luma > lumaHigh && m != null && m.iso <= isoMin && m.exposureNs <= dayNs -> { reset(); Command.Auto }
            luma > lumaHigh -> { product *= 0.75; manualCommand(speedMps) }
            luma < lumaLow -> { product *= 1.33; manualCommand(speedMps) }
            else -> {                                                    // in band: only a speed change moves the cap
                val prev = current
                manualCommand(speedMps).takeIf { it != prev }
            }
        }
    }

    @Synchronized fun reset() { manual = false; over = 0; current = Command.Auto }

    /** Shutter and ISO for [product] at [speedMps]. */
    fun settings(product: Double, speedMps: Float): Command.Manual {
        val cap = if (speedMps >= fastSpeedMps) fastNightCapNs else nightCapNs
        val t = if (product / dayNs <= isoComfort) dayNs else min(cap.toDouble(), max(dayNs.toDouble(), product / isoComfort)).toLong()
        val tc = t.coerceIn(expMinNs, max(expMinNs, min(expMaxNs, cap)))
        val iso = (product / tc).toInt().coerceIn(isoMin, isoMax)
        return Command.Manual(tc, iso)
    }

    private fun manualCommand(speedMps: Float): Command.Manual {
        // never ask for more than the sensor can give: keep the product within its range
        product = product.coerceIn(isoMin.toDouble() * expMinNs, isoMax.toDouble() * max(expMinNs, min(expMaxNs, nightCapNs)))
        return settings(product, speedMps).also { current = it }
    }
}

/**
 * Burst re-scans for far plates (the idea of plates-tracker's `BurstPlanner`, MIT, done forward instead of from a
 * video ring): when a tracked plate is still too small to read, the frames the frame-rate limit would skip are used
 * for a cheap pass over just a window around where that plate is expected, for [windowMs]; a track gets at most one
 * burst per [minGapMs]. An approaching car's plate then gets read in the few hundred ms it is readable, instead of
 * between two regular frames.
 */
class BurstPlanner(
    val windowMs: Long = 1_200,
    val minGapMs: Long = 1_500,
    val maxRois: Int = 2,
    /** Side of the window around a plate, frame pixels (the small detector's input: no downscaling). */
    val roiSide: Int = 416,
    /** Tracks seen more recently than this qualify. */
    val freshMs: Long = 700,
    /** Smallest plate width (frame px) worth a burst (below this even a full-resolution pass will not find it). */
    val minPlateW: Float = 12f,
) {
    private class Burst(val until: Long)
    private val bursts = HashMap<Int, Burst>()
    private val lastStart = HashMap<Int, Long>()

    /** Windows (frame pixels, clamped to [frameW]×[frameH]) to scan now for [tracks]; empty when no burst is due. */
    fun plan(nowMs: Long, tracks: List<Track>, frameW: Int, frameH: Int, readable: (BoxF) -> Boolean = { CropMath.readable(it) }): List<BoxF> {
        val alive = tracks.associateBy { it.id }
        bursts.keys.retainAll(alive.keys); lastStart.entries.removeAll { nowMs - it.value > 60_000 }
        for (t in tracks) {
            if (t.id in bursts || nowMs - t.lastMs > freshMs || t.box.w < minPlateW || readable(t.box)) continue
            if (nowMs - (lastStart[t.id] ?: Long.MIN_VALUE / 2) < minGapMs) continue
            bursts[t.id] = Burst(nowMs + windowMs); lastStart[t.id] = nowMs
        }
        bursts.entries.removeAll { (id, b) -> nowMs > b.until || alive[id]?.let { readable(it.box) } == true }
        return bursts.keys.mapNotNull { alive[it] }
            .map { it.predict(nowMs) }
            .sortedByDescending { it.w }
            .take(maxRois)
            .map { p ->
                val side = max(roiSide.toFloat(), p.w * 6f).coerceAtMost(min(frameW, frameH).toFloat())
                val x1 = (p.cx - side / 2).coerceIn(0f, max(0f, frameW - side)); val y1 = (p.cy - side / 2).coerceIn(0f, max(0f, frameH - side))
                BoxF(x1, y1, x1 + side, y1 + side)
            }
    }

    fun active() = bursts.isNotEmpty()

    fun clear() { bursts.clear(); lastStart.clear() }
}
