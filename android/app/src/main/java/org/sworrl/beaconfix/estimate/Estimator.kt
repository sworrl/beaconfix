package org.sworrl.beaconfix.estimate

import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sqrt

/** One RSSI sample of an access point taken at a known position. */
data class Sample(val lat: Double, val lon: Double, val acc: Double, val dbm: Int)

/** The fitted position (and radio model) of an access point. */
data class ApFit(val lat: Double, val lon: Double, val acc: Double, val refDbm: Double, val pathExp: Double, val residual: Double, val n: Int, val vantage: Int)

/** An access point with a known position, heard right now. */
data class KnownAp(val lat: Double, val lon: Double, val acc: Double, val dbm: Int, val refDbm: Double?, val pathExp: Double?)

data class PhoneFix(val lat: Double, val lon: Double, val acc: Double, val used: Int, val method: String)

/**
 * Robust weighted least squares on the log-distance path-loss model
 *   dbm_i = P0 - 10·n·log10(d_i),  d_i = |x_ap - x_i|
 * with unknowns (x, y, P0, n). Gauss–Newton with Huber weights (outliers: reflections,
 * a phone in a pocket, a sample taken while the AP was moving), plus the sample's own
 * position error folded into the weight. Nothing here needs phase or channel state:
 * with RSSI only, this is what the data supports. Result accuracy is the RMS of the
 * position residual mapped back to metres, floored by the samples' own accuracy.
 */
object Estimator {
    private const val MIN_SAMPLES = 3
    private const val MIN_SPREAD_M = 8.0

    fun fitAp(samples: List<Sample>): ApFit? {
        val s = samples.filter { it.acc in 0.0..250.0 && it.dbm in -100..-10 }
        if (s.size < MIN_SAMPLES) return null
        // distinct vantage points: cluster samples closer than max(acc, 5 m) together
        val vantage = countVantage(s)
        if (vantage < 2) return null
        val origin = weightedCentroid(s)
        val local = Geo.Local(origin[0], origin[1])
        val xs = DoubleArray(s.size); val ys = DoubleArray(s.size)
        for (i in s.indices) { val p = local.toXY(s[i].lat, s[i].lon); xs[i] = p[0]; ys[i] = p[1] }
        val spread = spreadM(xs, ys)
        if (spread < MIN_SPREAD_M) {
            // all samples from one spot: we can only say "near here", radius from the strongest sample
            val best = s.maxBy { it.dbm }
            val d = distanceFor(best.dbm, -40.0, 2.5)
            return ApFit(best.lat, best.lon, max(best.acc, d), -40.0, 2.5, d, s.size, vantage)
        }
        // initial guess: signal-weighted centroid (linear power weights), P0=-40, n=2.5
        var x = 0.0; var y = 0.0; var wsum = 0.0
        for (i in s.indices) { val w = 10.0.pow(s[i].dbm / 20.0); x += w * xs[i]; y += w * ys[i]; wsum += w }
        x /= wsum; y /= wsum
        var p0 = -40.0; var n = 2.5
        var weights = DoubleArray(s.size) { 1.0 / (1.0 + (s[it].acc / 10.0).pow(2)) }
        var lastCost = Double.MAX_VALUE
        repeat(30) { iter ->
            // Build normal equations J^T W J Δ = J^T W r for unknowns (x, y, p0, n)
            val a = Array(4) { DoubleArray(4) }; val b = DoubleArray(4)
            val res = DoubleArray(s.size)
            for (i in s.indices) {
                val dx = x - xs[i]; val dy = y - ys[i]
                val d = max(1.0, sqrt(dx * dx + dy * dy))
                val model = p0 - 10 * n * log10(d)
                val r = s[i].dbm - model
                res[i] = r
                // partials of model w.r.t. x, y, p0, n
                val k = -10 * n / (ln(10.0) * d * d)
                val j = doubleArrayOf(k * dx, k * dy, 1.0, -10 * log10(d))
                val w = weights[i]
                for (p in 0 until 4) { b[p] += w * j[p] * r; for (q in 0 until 4) a[p][q] += w * j[p] * j[q] }
            }
            // Levenberg damping keeps n and p0 from running away on small sets
            val lambda = 1e-3 * (1 + iter)
            for (p in 0 until 4) a[p][p] += lambda * max(1e-6, a[p][p])
            val delta = solve4(a, b) ?: return@repeat
            x += delta[0].coerceIn(-200.0, 200.0); y += delta[1].coerceIn(-200.0, 200.0)
            p0 = (p0 + delta[2]).coerceIn(-70.0, -10.0); n = (n + delta[3]).coerceIn(1.6, 4.5)
            // Huber re-weighting on dB residuals (scale 6 dB) times position confidence
            var cost = 0.0
            for (i in s.indices) {
                val r = abs(res[i]); val huber = if (r <= 6.0) 1.0 else 6.0 / r
                weights[i] = huber / (1.0 + (s[i].acc / 10.0).pow(2)); cost += weights[i] * res[i] * res[i]
            }
            if (abs(lastCost - cost) < 1e-3 * max(1.0, lastCost)) return@repeat
            lastCost = cost
        }
        // Uncertainty: propagate the dB residual into metres along the model, plus the samples' own error
        var rms = 0.0; var wsum2 = 0.0
        for (i in s.indices) {
            val dx = x - xs[i]; val dy = y - ys[i]; val d = max(1.0, sqrt(dx * dx + dy * dy))
            val r = s[i].dbm - (p0 - 10 * n * log10(d))
            val dm = d * (10.0.pow(abs(r) / (10 * n)) - 1)   // metres of distance error that residual implies
            rms += weights[i] * dm * dm; wsum2 += weights[i]
        }
        val residM = if (wsum2 > 0) sqrt(rms / wsum2) else 50.0
        val medAcc = s.map { it.acc }.sorted()[s.size / 2]
        val acc = max(8.0, sqrt(residM * residM + medAcc * medAcc) / sqrt(min(vantage, 12).toDouble() / 2))
        val ll = local.toLatLon(x, y)
        return ApFit(ll[0], ll[1], acc.coerceAtMost(1500.0), p0, n, residM, s.size, vantage)
    }

    /** Where is the phone, from APs whose positions we know? Weighted centroid then two Gauss–Newton refinements. */
    fun locatePhone(aps: List<KnownAp>): PhoneFix? {
        val k = aps.filter { it.acc in 0.0..300.0 && it.dbm > -95 }
        if (k.isEmpty()) return null
        if (k.size == 1) { val a = k[0]; val d = distanceFor(a.dbm, a.refDbm ?: -40.0, a.pathExp ?: 2.5); return PhoneFix(a.lat, a.lon, max(a.acc, d) + 20, 1, "single-ap") }
        val local = Geo.Local(k[0].lat, k[0].lon)
        var x = 0.0; var y = 0.0; var ws = 0.0
        val pts = k.map { local.toXY(it.lat, it.lon) }
        for (i in k.indices) { val w = 10.0.pow(k[i].dbm / 20.0) / (1 + (k[i].acc / 20.0).pow(2)); x += w * pts[i][0]; y += w * pts[i][1]; ws += w }
        x /= ws; y /= ws
        if (k.size >= 3) repeat(8) {
            val a = Array(2) { DoubleArray(2) }; val b = DoubleArray(2)
            for (i in k.indices) {
                val dx = x - pts[i][0]; val dy = y - pts[i][1]; val d = max(1.0, sqrt(dx * dx + dy * dy))
                val target = distanceFor(k[i].dbm, k[i].refDbm ?: -40.0, k[i].pathExp ?: 2.5)
                val r = target - d
                val w = 1.0 / (1 + (k[i].acc / 20.0).pow(2) + (target / 60.0).pow(2))
                val jx = dx / d; val jy = dy / d
                a[0][0] += w * jx * jx; a[0][1] += w * jx * jy; a[1][0] += w * jy * jx; a[1][1] += w * jy * jy
                b[0] += w * jx * r; b[1] += w * jy * r
            }
            val det = a[0][0] * a[1][1] - a[0][1] * a[1][0]
            if (abs(det) < 1e-9) return@repeat
            val ddx = (b[0] * a[1][1] - a[0][1] * b[1]) / det; val ddy = (a[0][0] * b[1] - a[1][0] * b[0]) / det
            x += ddx.coerceIn(-100.0, 100.0); y += ddy.coerceIn(-100.0, 100.0)
        }
        var spread = 0.0
        for (i in k.indices) { val dx = x - pts[i][0]; val dy = y - pts[i][1]; spread += dx * dx + dy * dy }
        spread = sqrt(spread / k.size)
        val acc = max(15.0, min(spread, 400.0) / sqrt(k.size.toDouble()) + k.map { it.acc }.sorted()[k.size / 2] / 2)
        val ll = local.toLatLon(x, y)
        return PhoneFix(ll[0], ll[1], acc, k.size, if (k.size >= 3) "wls" else "centroid")
    }

    fun distanceFor(dbm: Int, refDbm: Double, pathExp: Double): Double = 10.0.pow((refDbm - dbm) / (10 * pathExp)).coerceIn(1.0, 2000.0)

    private fun weightedCentroid(s: List<Sample>): DoubleArray {
        var lat = 0.0; var lon = 0.0; var ws = 0.0
        for (x in s) { val w = 10.0.pow(x.dbm / 20.0); lat += w * x.lat; lon += w * x.lon; ws += w }
        return doubleArrayOf(lat / ws, lon / ws)
    }
    private fun spreadM(xs: DoubleArray, ys: DoubleArray): Double {
        val mx = xs.average(); val my = ys.average(); var s = 0.0
        for (i in xs.indices) s += (xs[i] - mx).pow(2) + (ys[i] - my).pow(2)
        return sqrt(s / xs.size)
    }
    fun countVantage(s: List<Sample>): Int {
        val centres = ArrayList<Sample>()
        for (x in s) {
            val near = centres.any { c -> Geo.distanceM(c.lat, c.lon, x.lat, x.lon) < max(5.0, max(c.acc, x.acc)) }
            if (!near) centres.add(x)
        }
        return centres.size
    }
    private fun solve4(a: Array<DoubleArray>, b: DoubleArray): DoubleArray? {
        val m = Array(4) { i -> DoubleArray(5) { j -> if (j < 4) a[i][j] else b[i] } }
        for (c in 0 until 4) {
            var piv = c
            for (r in c + 1 until 4) if (abs(m[r][c]) > abs(m[piv][c])) piv = r
            if (abs(m[piv][c]) < 1e-12) return null
            val t = m[c]; m[c] = m[piv]; m[piv] = t
            for (r in 0 until 4) if (r != c) { val f = m[r][c] / m[c][c]; for (k in c until 5) m[r][k] -= f * m[c][k] }
        }
        return DoubleArray(4) { m[it][4] / m[it][it] }
    }
    @Suppress("unused") private fun sigmoid(x: Double) = 1 / (1 + exp(-x))
}
