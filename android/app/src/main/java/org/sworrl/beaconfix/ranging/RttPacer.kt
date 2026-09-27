package org.sworrl.beaconfix.ranging

import kotlin.math.abs
import kotlin.math.max

/**
 * Session pacing of Wi-Fi RTT bursts (docs/RANGING.md §7 "Pacing"). While a session runs (app open or collector on) the
 * loop ticks every 2.5 s, and each tick used to fire an 8-exchange burst, for as long as the session lasted. Now: a burst
 * every tick while something changes; once the fused distance has held for [stableMs] with neither the phone nor the
 * desktop moving, one burst every [slowMs]. Back to a burst every tick at once when either side moves, when the level of
 * the desktop's BLE advert moves by more than [BLE_JUMP_DB], when the distance leaves its band, or when the user opens a
 * ranging view ([boost]).
 *
 * Pure (the clock is passed in) and thread-safe ([boost] comes from the UI thread, the rest from the ranging loop).
 */
class RttPacer(private val stableMs: Long = STABLE_MS, private val slowMs: Long = SLOW_MS) {
    private var stableSince = -1L                 // -1: nothing stable yet (no estimate, or something just changed)
    private var refDistance = Double.NaN          // the distance when the stable period began
    private var refBle = Double.NaN               // the BLE level when the stable period began (the first one seen)
    private var lastBurst = Long.MIN_VALUE / 2
    private val ble = ArrayDeque<Pair<Long, Int>>()   // the desktop's advert as heard over the last BLE_WINDOW_MS
    /** Why the pacer last (re)started its stable period ("" until something has). */
    var lastReason: String = ""; private set

    /** One burst every [slowMs] instead of one per tick. */
    val slow: Boolean @Synchronized get() = slowAt(lastNow)
    private var lastNow = 0L
    private fun slowAt(now: Long) = stableSince >= 0 && now - stableSince >= stableMs

    /**
     * One tick's evidence: the fused distance shown to the user (null: none yet), whether the phone and the desktop are
     * moving, and the RSSI of the desktop's advert heard this tick.
     */
    @Synchronized
    fun observe(now: Long, distanceM: Double?, phoneMoving: Boolean, desktopMoving: Boolean, bleRssi: List<Int> = emptyList()) {
        lastNow = now
        for (r in bleRssi) ble.addLast(now to r)
        while (ble.isNotEmpty() && now - ble.first().first > BLE_WINDOW_MS) ble.removeFirst()
        val level = if (ble.size >= BLE_MIN_SAMPLES) RangeMath.levelFromSamples(ble.map { it.second.toDouble() }, 3, (now - ble.first().first) / 1000.0, false).takeIf { it.valid }?.dbm else null
        val why = when {
            phoneMoving -> "the phone is moving"
            desktopMoving -> "the desktop is moving"
            distanceM == null -> "no estimate yet"
            !refDistance.isNaN() && abs(distanceM - refDistance) > max(STABLE_ABS_M, STABLE_REL * refDistance) -> "the distance changed"
            level != null && !refBle.isNaN() && abs(level - refBle) > BLE_JUMP_DB -> "the BLE level changed by more than ${BLE_JUMP_DB.toInt()} dB"
            else -> null
        }
        if (why != null) {
            lastReason = why
            if (distanceM == null) { stableSince = -1; refDistance = Double.NaN } else { stableSince = now; refDistance = distanceM }
            refBle = level ?: Double.NaN
            return
        }
        if (stableSince < 0) { stableSince = now; refDistance = distanceM!! }
        if (refBle.isNaN() && level != null) refBle = level
    }

    /** The user opened a ranging view: a burst now and one every tick for [stableMs]; then slow again if nothing changed. */
    @Synchronized
    fun boost(now: Long) {
        lastNow = now
        if (stableSince >= 0) stableSince = now
        lastBurst = Long.MIN_VALUE / 2
        lastReason = "a ranging view opened"
    }

    /** Whether this tick should fire a burst. */
    @Synchronized
    fun burstDue(now: Long): Boolean { lastNow = now; return !slowAt(now) || now - lastBurst >= slowMs }

    /** A burst went on the air at [now]. */
    @Synchronized
    fun burst(now: Long) { lastBurst = now }

    companion object {
        /** How long the distance must hold, with nothing moving, before bursts slow down. */
        const val STABLE_MS = 120_000L
        /** One burst this often while slow. */
        const val SLOW_MS = 30_000L
        /** The distance "holds" while it stays within max(STABLE_ABS_M, STABLE_REL × the reference). */
        const val STABLE_ABS_M = 0.5
        const val STABLE_REL = 0.2
        /** A change of the desktop's BLE level (over [BLE_WINDOW_MS]) larger than this means something moved. */
        const val BLE_JUMP_DB = 6.0
        const val BLE_WINDOW_MS = 10_000L
        const val BLE_MIN_SAMPLES = 5
    }
}
