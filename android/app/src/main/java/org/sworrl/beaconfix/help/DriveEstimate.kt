package org.sworrl.beaconfix.help

import kotlin.math.max
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/**
 * A rough drive time without a routing service (pure): the straight line × [DETOUR] at [SPEED_KMH], rounded to five
 * minutes with a five-minute minimum. The same rule as the desktop's `PoiClassify::driveEstimate`.
 */
object DriveEstimate {
    const val DETOUR = 1.4
    const val SPEED_KMH = 70.0
    private const val STEP_S = 300

    /** Estimated road metres for a straight-line distance. */
    fun roadM(straightM: Double): Int = (max(0.0, straightM) * DETOUR).roundToInt()

    /** Estimated seconds for a straight-line distance; always a multiple of 5 minutes, at least 5 minutes. */
    fun seconds(straightM: Double): Int {
        val secs = max(0.0, straightM) * DETOUR / (SPEED_KMH / 3.6)
        return max(STEP_S, (secs / STEP_S).roundToLong().toInt() * STEP_S)
    }

    /** "~1 h 50 min (est.)", "~2 h (est.)", "~10 min (est.)"; without "(est.)" when [est] is false (a routed time). "" for none. */
    fun text(s: Int, est: Boolean = true): String {
        if (s <= 0) return ""
        val min = max(5, ((s / 60.0) / 5.0).roundToInt() * 5)
        val h = min / 60; val m = min % 60
        val body = when { h == 0 -> "~$m min"; m == 0 -> "~$h h"; else -> "~$h h $m min" }
        return if (est) "$body (est.)" else body
    }
}
