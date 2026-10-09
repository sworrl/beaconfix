package org.sworrl.beaconfix.lite

import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Where we are from APs whose positions are known: BeaconFix's `Estimator::selfLocate` (src/estimator.cpp), ported.
 *
 * Each AP's level gives a distance through a log-distance path-loss model; the position is the robust (Huber)
 * least-squares fit of those ranges, solved by Levenberg–Marquardt from the signal-weighted centroid. With 4+ APs an
 * integrity check (RAIM-like: a chi-squared test on the normalised range residuals) drops the AP that disagrees most
 * and solves again, up to three times: an AP that moved since it was mapped is the usual culprit, and it would
 * otherwise drag the fix by its whole move. The radius comes from the fit's covariance, scaled up when the ranges
 * disagree more than the noise model expects, so a bad geometry or a bad AP shows up as a wider circle rather than a
 * confident wrong answer.
 *
 * Pure arithmetic, no allocation beyond a few arrays: about a millisecond for 20 APs on a phone-class CPU.
 */
object SelfLocate {
    /** A transmitter with a known position: [accM] its own position's 1-sigma, [weight] how much to trust it (0.05–1). */
    class Known(val lat: Double, val lon: Double, val accM: Double, val dbm: Int, val freqMhz: Int = 0, val weight: Double = 1.0)

    class Result(
        val valid: Boolean, val lat: Double = 0.0, val lon: Double = 0.0, val accM: Double = 0.0, val r95M: Double = 0.0,
        val rmsM: Double = 0.0, val used: Int = 0, val rejected: Int = 0,
        /** Indexes into the input of the APs the integrity check dropped. */
        val excluded: List<Int> = emptyList(),
        val integrity: String = "unverified",
    )

    /** Shadowing (dB): how much a level wanders from the model at a fixed distance. */
    const val SIGMA_DB = 6.0
    /** Path-loss exponent: 2 in free space, ~2.4 through a house or two. */
    const val PATH_LOSS = 2.4
    private const val LN10 = 2.302585092994046
    private const val MAX_ITER = 40

    /** Level at 1 m. 5 and 6 GHz lose ~6 and ~8 dB more than 2.4 GHz over the same metre (free-space loss). */
    fun p0(freqMhz: Int): Double = when {
        freqMhz >= 5925 -> -48.0
        freqMhz >= 4900 -> -46.0
        else -> -40.0
    }

    fun distanceFor(dbm: Int, freqMhz: Int): Double = 10.0.pow((p0(freqMhz) - dbm) / (10.0 * PATH_LOSS)).coerceIn(1.0, 1500.0)

    fun solve(known: List<Known>): Result {
        val n = known.size
        if (n < 2) return Result(false)
        val fr = Frame(known[0].lat, known[0].lon)
        val sx = DoubleArray(n); val sy = DoubleArray(n); val sw = DoubleArray(n); val sd = DoubleArray(n)
        val sigD = DoubleArray(n); val acc = DoubleArray(n); val on = BooleanArray(n) { true }
        for (i in 0 until n) {
            val k = known[i]
            val d = distanceFor(k.dbm, k.freqMhz)
            sx[i] = fr.x(k.lon); sy[i] = fr.y(k.lat); sw[i] = max(0.05, k.weight); sd[i] = d
            sigD[i] = d * LN10 * SIGMA_DB / (10.0 * PATH_LOSS)
            acc[i] = max(1.0, k.accM)
        }
        fun sig2(i: Int) = sigD[i] * sigD[i] + acc[i] * acc[i]

        // Start from the signal-weighted centroid (loud and precisely placed APs count most) — or, with 4+ APs, the
        // median: one loud AP mapped kilometres away (it moved house) drags a centroid so far that the solve
        // settles next to it and the integrity check can no longer tell which AP is wrong. The median doesn't move.
        val cw = DoubleArray(n) { 10.0.pow(known[it].dbm / 20.0) / max(10.0, acc[it]) }
        var x0: Double; var y0: Double
        // (unweighted: the loud moved AP can carry more than half of all the signal weight on its own)
        if (n >= 4) { val one = DoubleArray(n) { 1.0 }; x0 = weightedMedian(sx, one); y0 = weightedMedian(sy, one) }
        else {
            var w = 0.0; x0 = 0.0; y0 = 0.0
            for (i in 0 until n) { w += cw[i]; x0 += cw[i] * sx[i]; y0 += cw[i] * sy[i] }
            x0 /= w; y0 /= w
        }
        var x = x0; var y = y0
        val excluded = ArrayList<Int>()

        // An AP mapped far beyond the rest of the group is out before the solve. Huber bounds an outlier relative to its
        // OWN range sigma, and a loud AP (your router, heard at -45) claims to be a metre away with a sigma of metres:
        // mapped 2 km off, it would still outvote ten honest APs. "Far" = five times the group's median spread from
        // the (robust) start, and never under 300 m.
        if (n >= 4) {
            val dist = DoubleArray(n) { hypot(sx[it] - x0, sy[it] - y0) }
            val med = dist.sorted()[n / 2]
            for (i in 0 until n) if (dist[i] > max(300.0, 5 * med) + acc[i] && n - excluded.size > 3) { on[i] = false; excluded += i }
        }

        fun cost(cx: Double, cy: Double): Double {
            var c = 0.0
            for (i in 0 until n) {
                if (!on[i]) continue
                val r = max(1.0, hypot(cx - sx[i], cy - sy[i]))
                val v = sig2(i); val res = r - sd[i]; val h = huber(res / sqrt(v), 2.0)
                c += sw[i] * h * res * res / v
            }
            return c
        }

        fun solveActive() {
            var active = 0
            for (i in 0 until n) if (on[i]) active++
            if (active < 3) return
            var px = x0; var py = y0; var lambda = 1e-2
            for (it in 0 until MAX_ITER) {
                var a00 = 0.0; var a01 = 0.0; var a11 = 0.0; var b0 = 0.0; var b1 = 0.0
                for (i in 0 until n) {
                    if (!on[i]) continue
                    val dx = px - sx[i]; val dy = py - sy[i]; val r = max(1.0, hypot(dx, dy))
                    val jx = dx / r; val jy = dy / r; val v = sig2(i)
                    val res = r - sd[i]; val h = huber(res / sqrt(v), 2.0); val w = sw[i] * h / v
                    a00 += w * jx * jx; a01 += w * jx * jy; a11 += w * jy * jy
                    b0 -= w * jx * res; b1 -= w * jy * res
                }
                val c = cost(px, py)
                a00 *= 1 + lambda; a11 *= 1 + lambda
                val det = a00 * a11 - a01 * a01
                if (abs(det) < 1e-18) break
                val dx = (b0 * a11 - b1 * a01) / det
                val dy = (a00 * b1 - a01 * b0) / det
                val nx = px + dx; val ny = py + dy
                if (cost(nx, ny) <= c) {
                    px = nx; py = ny; lambda = max(1e-6, lambda / 3)
                    if (hypot(dx, dy) < 0.2) break
                } else {
                    lambda *= 8
                    if (lambda > 1e6) break
                }
            }
            x = px; y = py
        }

        if (n >= 3) solveActive()

        // integrity: drop the AP that disagrees most while the residuals fail the chi-squared test
        var integrity = if (n >= 4) (if (excluded.isEmpty()) "ok" else "repaired") else "unverified"
        if (n >= 4) {
            for (round in 0 until 3) {
                var chi = 0.0; var act = 0; var worst = -1; var worstZ = 0.0
                for (i in 0 until n) {
                    if (!on[i]) continue
                    val r = max(1.0, hypot(x - sx[i], y - sy[i]))
                    val z = (r - sd[i]) / sqrt(sig2(i))
                    chi += z * z; act++
                    if (abs(z) > worstZ) { worstZ = abs(z); worst = i }
                }
                if (act < 4 || chi <= chi2Quantile99(act - 2)) {
                    if (round > 0) integrity = if (act >= 4) "repaired" else "unverified"
                    break
                }
                if (round == 2 || act - 1 < 3) { integrity = "failed"; break }
                on[worst] = false; excluded += worst
                solveActive()
            }
        }

        var c00 = 0.0; var c01 = 0.0; var c11 = 0.0; var wsum = 0.0; var wres = 0.0; var rejected = 0; var used = 0
        for (i in 0 until n) {
            if (!on[i]) continue
            val dx = x - sx[i]; val dy = y - sy[i]; val r = max(1.0, hypot(dx, dy))
            val jx = dx / r; val jy = dy / r; val v = sig2(i)
            val res = r - sd[i]; val h = huber(res / sqrt(v), 2.0); val w = sw[i] * h / v
            if (h < 0.5) rejected++
            used++
            c00 += w * jx * jx; c01 += w * jx * jy; c11 += w * jy * jy
            wsum += w; wres += w * res * res
        }
        val chiNorm = wres / max(1, used - 2)                 // reduced chi-squared of the normalised ranges
        val spread = sqrt(wres / max(1e-12, wsum))           // weighted RMS range residual (m)
        var a: Double
        var r95: Double
        val det = c00 * c11 - c01 * c01
        if (used >= 3 && abs(det) >= 1e-12) {
            val scale = max(1.0, chiNorm)
            val ixx = c11 / det * scale; val ixy = -c01 / det * scale; val iyy = c00 / det * scale
            val tr = ixx + iyy; val dt = ixx * iyy - ixy * ixy
            val disc = sqrt(max(0.0, tr * tr / 4 - dt))
            val major = sqrt(max(0.0, tr / 2 + disc)); val minor = sqrt(max(0.0, tr / 2 - disc))
            a = major; r95 = radiusFor(major, minor, 0.95)
        } else {
            a = 0.0
            for (i in 0 until n) if (on[i]) a = max(a, sqrt(sigD[i] * sigD[i] + acc[i] * acc[i]))
            r95 = 2.45 * a
        }
        val accOut = max(15.0, max(a, spread * 0.7))
        return Result(
            valid = true, lat = fr.lat(y), lon = fr.lon(x), accM = accOut, r95M = max(accOut * 2.45, r95), rmsM = spread,
            used = used - rejected, rejected = rejected + excluded.size, excluded = excluded, integrity = integrity,
        )
    }

    private fun weightedMedian(v: DoubleArray, w: DoubleArray): Double {
        val idx = v.indices.sortedBy { v[it] }
        val half = w.sum() / 2
        var acc = 0.0
        for (i in idx) { acc += w[i]; if (acc >= half) return v[i] }
        return v[idx.last()]
    }

    private fun huber(r: Double, k: Double): Double { val a = abs(r); return if (a <= k) 1.0 else k / a }

    /** Wilson–Hilferty. */
    fun chi2Quantile99(dofIn: Int): Double {
        val k = max(1, dofIn).toDouble()
        val z = 2.3263478740408408
        val t = 1 - 2 / (9 * k) + z * sqrt(2 / (9 * k))
        return k * t * t * t
    }

    fun normCdf(z: Double): Double {
        val x = -z / sqrt(2.0)
        val t = 1.0 / (1.0 + 0.5 * abs(x))
        val r = t * exp(-x * x - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 + t * (-0.18628806 + t * (0.27886807 +
            t * (-1.13520398 + t * (1.48851587 + t * (-0.82215223 + t * 0.17087277)))))))))
        val erfc = if (x >= 0) r else 2.0 - r
        return 0.5 * erfc
    }

    /** P(|X| < r) for X ~ N(0, diag(s1², s2²)): Simpson over the circle (64 intervals is plenty for a radius). */
    fun probWithin(s1: Double, s2: Double, r: Double): Double {
        if (r <= 0) return 0.0
        val sa = max(1e-9, max(s1, s2)); val sb = max(1e-9, min(s1, s2))
        val nI = 64
        val h = Math.PI / nI
        var sum = 0.0
        for (i in 0..nI) {
            val t = -Math.PI / 2 + i * h; val xx = r * sin(t); val ct = cos(t)
            val fv = exp(-0.5 * (xx / sa) * (xx / sa)) / (sa * sqrt(2 * Math.PI)) * (2 * normCdf(r * ct / sb) - 1) * r * ct
            sum += if (i == 0 || i == nI) fv else if (i % 2 == 1) 4 * fv else 2 * fv
        }
        return (sum * h / 3).coerceIn(0.0, 1.0)
    }

    fun radiusFor(s1: Double, s2: Double, p: Double): Double {
        var lo = 0.0; var hi = 5.0 * max(1e-6, max(s1, s2))
        repeat(40) { val mid = 0.5 * (lo + hi); if (probWithin(s1, s2, mid) < p) lo = mid else hi = mid }
        return 0.5 * (lo + hi)
    }
}
