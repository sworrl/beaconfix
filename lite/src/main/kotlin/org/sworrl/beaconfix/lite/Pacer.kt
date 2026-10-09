package org.sworrl.beaconfix.lite

/**
 * How long to wait before the next look. A device that keeps hearing the same APs doubles its wait up to [maxMs]
 * (a picture frame on a shelf settles at the maximum within a few hours); one that hears a different place goes
 * back to [baseMs], and one that moved far between two looks (a car, a train) to [minMs].
 */
class Pacer(val minMs: Long = 2 * 60_000L, val baseMs: Long = 15 * 60_000L, val maxMs: Long = 2 * 3600_000L) {
    var intervalMs: Long = baseMs; private set
    private var prev: LiteFix? = null

    fun next(fix: LiteFix?): Long {
        val p = prev
        intervalMs = when {
            fix == null -> baseMs                                                       // nothing heard: try again later
            fix.source == "reuse" -> minOf(maxMs, intervalMs * 2)                       // same place
            p != null && Geo.distanceM(p.lat, p.lon, fix.lat, fix.lon) > 500 + p.accM + fix.accM -> minMs   // travelling
            else -> baseMs
        }
        if (fix != null) prev = fix
        return intervalMs
    }
}
