package org.sworrl.beaconfix.ranging

import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToLong
import kotlin.math.sqrt

/**
 * Wi-Fi RTT (802.11mc FTM / 802.11az NTB) to ordinary access points during collection: the pure part (no Android), so
 * the burst combination, the offset correction and the selection / cooldown are unit-tested (ApRttTest).
 * The radio side is [ApRttRanger]; each answered AP's range is stored on that scan's observation (rangeM / rangeSd)
 * and feeds the estimator's range likelihood (estimate.Estimator `Obs.rangeM`, desktop `Obs::rangeM`).
 */

/** One successful RangingResult (STATUS_SUCCESS, ≥ 2 successful measurements) of one AP. */
data class ApBurst(val distMm: Int, val stdMm: Int, val nOk: Int, val rssi: Int = 0, val az: Boolean = false)

/** The combined range of one AP at one stop: metres, 1-σ metres (offset-corrected, rounded to cm). */
data class ApRange(val bssid: String, val rangeM: Double, val rangeSd: Double, val bursts: Int, val time: Long, val az: Boolean = false) {
    /** "RTT 7.3 m ± 0.9" */
    val text: String get() = String.format(java.util.Locale.US, "RTT %.1f m ± %.1f", rangeM, rangeSd)
}

/** A responder heard in the scan: [dbm] orders the work (strongest ≈ nearest first). */
data class RttCandidate(val bssid: String, val dbm: Int, val az: Boolean = false)

object ApRttMath {
    /** Smallest 1-σ we report (RangeMath.RTT_FLOOR, docs/RANGING.md §3.3). */
    const val FLOOR_M = RangeMath.RTT_FLOOR
    /** Fewer successful measurements than this in a burst: the burst is dropped. */
    const val MIN_OK = 2
    /**
     * Largest |offset| taken as the phone's own. The calibration measures a *pair* offset (phone + the desktop's
     * responder, docs/RANGING.md §3.3/§8); the Pixel 10 Pro XL ↔ AX210 soft-AP pair reads ~14 m, which is the soft-AP's
     * delay, not the phone's, and would put every other AP 14 m closer. Beyond this the offset is not reused.
     */
    const val MAX_PHONE_OFFSET_M = 3.0
    /** Median of k samples has σ ≈ 1.2533·σ/√k. */
    private const val MEDIAN_EFF = 1.2533

    /** The calibrated offset to subtract, or 0 when there is none, it is stale, or it is implausible for the phone alone. */
    fun usableOffset(offsetM: Double?, stale: Boolean = false): Double =
        if (offsetM == null || stale || !offsetM.isFinite() || abs(offsetM) > MAX_PHONE_OFFSET_M) 0.0 else offsetM

    fun median(v: List<Double>): Double { val s = v.sorted(); val n = s.size; return if (n % 2 == 1) s[n / 2] else (s[n / 2 - 1] + s[n / 2]) / 2 }

    /**
     * Bursts of one AP → one range. Distance: the median of the bursts (time-of-flight outliers are late and the median
     * ignores them), minus [offsetM]. 1-σ: the larger of the bursts' own σ (distanceStdDevMm/√n, RMS) and their spread
     * (1.4826·MAD with ≥ 3 bursts, |d1−d2|/√2 with 2), times 1.2533/√k for the median, the offset's σ in quadrature,
     * floored at [FLOOR_M]. A corrected range below 0.1 m (the offset is wrong for this AP) is clamped and the σ widened
     * to cover the clamp. Null when no burst qualifies.
     */
    fun combine(bssid: String, bursts: List<ApBurst>, time: Long, offsetM: Double = 0.0, offsetSd: Double = 0.0): ApRange? {
        val ok = bursts.filter { it.nOk >= MIN_OK && it.distMm > -10_000 }
        if (ok.isEmpty()) return null
        val d = ok.map { it.distMm / 1000.0 }
        val k = d.size
        val med = median(d)
        val own = sqrt(ok.sumOf { val s = max(FLOOR_M, it.stdMm / 1000.0 / sqrt(it.nOk.toDouble())); s * s } / k)
        val spread = when {
            k >= 3 -> 1.4826 * median(d.map { abs(it - med) })
            k == 2 -> abs(d[0] - d[1]) / sqrt(2.0)
            else -> 0.0
        }
        val sb = max(own, spread)
        var sd = if (k >= 2) MEDIAN_EFF * sb / sqrt(k.toDouble()) else sb
        sd = sqrt(sd * sd + offsetSd * offsetSd)
        var r = med - offsetM
        if (r < 0.1) { sd = max(sd, 0.1 - r); r = 0.1 }
        return ApRange(bssid, round2(r), round2(max(FLOOR_M, sd)), k, time, ok.any { it.az })
    }

    fun round2(x: Double): Double = (x * 100).roundToLong() / 100.0
}

/**
 * Which heard responders to range this scan, and when not to. Strongest first, at most [maxPerScan]; an AP ranged in
 * the last [cooldownMs] within [samePlaceM] of here is skipped (nothing new to learn at the same spot); an AP that
 * did not answer [failsBeforeBackoff] times in a row waits [cooldownMs]·2^k (≤ [maxBackoffMs]) wherever we are.
 * Pure: the clock and the position are passed in. Not thread-safe (the collector loop is the only caller).
 */
class ApRttPlanner(
    val maxPerScan: Int = MAX_PER_SCAN, val cooldownMs: Long = COOLDOWN_MS, val samePlaceM: Double = SAME_PLACE_M,
    val failsBeforeBackoff: Int = 2, val maxBackoffMs: Long = MAX_BACKOFF_MS,
) {
    private class Rec(var t: Long, var lat: Double?, var lon: Double?, var fails: Int)
    private val recs = HashMap<String, Rec>()

    fun select(cands: List<RttCandidate>, now: Long, lat: Double?, lon: Double?): List<RttCandidate> =
        cands.distinctBy { it.bssid }.sortedByDescending { it.dbm }.filter { due(it.bssid, now, lat, lon) }.take(maxPerScan)

    fun due(bssid: String, now: Long, lat: Double?, lon: Double?): Boolean {
        val r = recs[bssid] ?: return true
        val age = now - r.t
        if (r.fails >= failsBeforeBackoff) return age >= min(maxBackoffMs, cooldownMs shl min(r.fails - failsBeforeBackoff + 1, 8))
        if (age >= cooldownMs) return true
        // within the cooldown: only a new place is worth a burst (unknown positions count as the same place)
        return lat != null && lon != null && r.lat != null && r.lon != null && distM(lat, lon, r.lat!!, r.lon!!) > samePlaceM
    }

    /** [bssid] was ranged at [now]; [answered] = it returned a usable range. */
    fun mark(bssid: String, now: Long, lat: Double?, lon: Double?, answered: Boolean) {
        val r = recs.getOrPut(bssid) { Rec(now, lat, lon, 0) }
        r.t = now; r.lat = lat; r.lon = lon; r.fails = if (answered) 0 else r.fails + 1
        if (recs.size > MAX_RECS) recs.entries.removeAll { now - it.value.t > maxBackoffMs }
    }

    fun fails(bssid: String): Int = recs[bssid]?.fails ?: 0

    companion object {
        const val MAX_PER_SCAN = 8
        const val COOLDOWN_MS = 30_000L
        const val SAME_PLACE_M = 25.0
        const val MAX_BACKOFF_MS = 8 * 60_000L
        const val MAX_RECS = 512
        /** Requests of at most [maxPeers] APs each (RangingRequest.getMaxPeers()). */
        fun <T> batches(items: List<T>, maxPeers: Int): List<List<T>> = if (items.isEmpty()) emptyList() else items.chunked(max(1, maxPeers))
        /** Equirectangular: plenty for "the same spot" (tens of metres). */
        fun distM(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
            val k = Math.PI / 180 * 6_371_000.0
            val dy = (lat2 - lat1) * k; val dx = (lon2 - lon1) * k * cos((lat1 + lat2) / 2 * Math.PI / 180)
            return sqrt(dx * dx + dy * dy)
        }
    }
}
