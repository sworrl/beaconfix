package org.sworrl.beaconfix.estimate

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.asin
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.hypot
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

// Position estimation from signal-strength samples, with a graded confidence (docs/GRADING.md).
//
// Model (per place k): y_k = P0 + g_dev + δ_dev − 10·n·log10(d_k) + ε_k, d_k = √(|p − q_k|² + h²). g_dev is the hearing
// device's calibrated offset (Context.deviceOffset, this device = 0); δ_dev its deviation for THIS AP (prior
// N(0, devOffsetSd²)): the device with the most places is the reference (δ = 0), the next MAXD get a fitted δ, any further
// one stays pinned. Places never mix devices. After the robust LM, the solution's mirror across the places' principal
// axis is refitted too; outside the hull an unresolved mirror widens the covariance as a two-point mixture.
//
// This file is a line-by-line port of the desktop engine (src/estimator.h + src/estimator.cpp): same model, same
// order of operations, same constants, iteration counts, tie-breaking and bootstrap RNG. Both are checked against
// tests/fixtures/estimator_golden.json (EstimatorGoldenTest here, estimator_golden --check on the desktop).
// Change both or neither.

/** One signal-strength sample of an AP. */
data class Obs(
    var lat: Double = 0.0, var lon: Double = 0.0,   // where the observer was
    var acc: Double = 30.0,                          // its fix accuracy (m, 68 % radius)
    var dbm: Int = -100,
    var t: Long = 0,                                 // seconds since the epoch (0 = unknown)
    var weight: Double = 1.0,                        // extra factor: 0.3 for "travels with you" statuses etc.
    var device: String = "",                         // who heard it ("" = this device)
    var rangeM: Double = -1.0, var rangeSd: Double = 0.0,   // optional Wi-Fi RTT range to the AP and its 1-σ (m)
)

/** Where we scanned without hearing the AP (docs/GRADING.md §1.7). */
data class Miss(var lat: Double = 0.0, var lon: Double = 0.0, var count: Int = 1)

/** A position somebody else claims (WiGLE / Apple / BeaconDB / the AP's own LCI): only compared, never fitted. */
data class External(var has: Boolean = false, var lat: Double = 0.0, var lon: Double = 0.0, var acc: Double = 50.0, var source: String = "")

data class Fit(
    var valid: Boolean = false,              // a position is claimed (kind fix or region)
    var kind: String = "none",               // fix | region | mobile | none
    var lat: Double = 0.0, var lon: Double = 0.0,
    var acc: Double = 0.0,                   // circular 1-σ (m): semi-major axis of the 1-σ ellipse (compat)
    var semiMajor: Double = 0.0, var semiMinor: Double = 0.0, var orientDeg: Double = 0.0,
    var cxx: Double = 0.0, var cxy: Double = 0.0, var cyy: Double = 0.0,   // position covariance (m², x = east, y = north)
    var r95: Double = 0.0, var cep50: Double = 0.0,
    var pWithin25: Double = 0.0,
    var rms: Double = 0.0,
    var p0: Double = -40.0,
    var pathloss: Double = 2.4,
    var fittedN: Boolean = false,
    var n: Int = 0,
    var vantage: Int = 0,
    var rejected: Int = 0,
    var quality: String = "none",            // compat: good | fair | poor | none
    var updated: Long = 0,                   // seconds since the epoch
    // ── grading metrics (docs/GRADING.md §2) ──
    var score: Double = 0.0,
    var grade: String = "",                  // A–F · R (region only) · M (mobile) · "" (none)
    var pendingGrade: String = "",
    var rssDop: Double = 0.0,
    var crlbR95: Double = 0.0,
    var rbar: Double = 1.0,
    var maxGapDeg: Double = 360.0,
    var inHull: Boolean = false,
    var linRatio: Double = 0.0,
    var chi2nu: Double = 0.0,
    var sigmaDb: Double = 0.0,
    var outlierFrac: Double = 0.0,
    var ess: Double = 0.0,
    var sessions: Int = 0, var devices: Int = 0,
    var spearman: Double = 0.0,
    var p0RangeCorr: Double = 0.0,
    var dminRatio: Double = 0.0,
    var ambiguous: Boolean = false,
    var altLat: Double = 0.0, var altLon: Double = 0.0,
    var modes: Int = 0,
    var driftD2: Double = 0.0,
    var extD2: Double = -1.0,
    var jackMax: Double = 0.0,
    var nisEwma: Double = 0.0,
    var fadingDb: Double = 0.0,
    var moved: Boolean = false,
    var newest: Long = 0,
    var suggestLat: Double = 0.0, var suggestLon: Double = 0.0, var suggestGain: Double = 0.0,
    var cP: Double = 0.0, var cG: Double = 0.0, var cE: Double = 0.0, var cF: Double = 0.0, var cFfit: Double = 0.0,
    var cS: Double = 0.0, var cT: Double = 0.0, var cX: Double = -1.0,
    var groupRef: String = "",
    var groupSize: Int = 0,
)

data class Options(
    var defaultN: Double = 2.4,
    var nSd: Double = 0.5,
    var p0Mean: Double = -40.0, var p0Sd: Double = 8.0,
    var sigmaDb: Double = 6.0,
    var outlierPrior: Double = 0.1,
    var outlierSpanDb: Double = 80.0,
    var heightM: Double = 3.0,
    var clusterMinM: Double = 5.0,       // place radius: max(clusterMinM, clusterAccK × the better fix accuracy of the pair)
    var clusterAccK: Double = 1.5,
    var maxClusters: Int = 120,
    var ageTauDays: Double = 180.0,
    var maxAcc: Double = 300.0,
    var geomAcc: Double = 100.0,
    var rhoIn: Double = 0.6,
    var shadowCorrM: Double = 8.0,       // Gudmundson decorrelation distance (measured: docs/GRADING.md §1.5)
    var sigmaPriorDof: Double = 6.0,     // ν0 of the scaled-inverse-χ² prior on σ² (centred: E[σ²] = σ0² without data)
    var postWindow: Double = 4.0,        // R95 from the posterior over ±this many σ of the major axis (0 = Laplace only)
    var devOffsetSd: Double = 10.0,      // σ (dB) of a device's per-AP deviation from its calibrated offset
    var rangePriorM: Double = 150.0,
    var missFloor: Double = 0.2,
    var sensitivity: Double = -92.0,
    var gridN: Int = 48,
    var minHalfWidth: Double = 120.0, var maxHalfWidth: Double = 600.0,
    var bootstrap: Int = 24,
    var jackMaxK: Int = 60,
    var maxIter: Int = 40,
    var fixMaxR95: Double = 150.0,
    var maxR95: Double = 1500.0,
    var kappa: Double = 1.0,
    var seed: Long = 0x5EEDBEAC0F1L,
    var minSamples: Int = 1,
)

data class Context(
    var deviceOffset: Map<String, Double> = emptyMap(),   // dB the device hears louder than this one
    var misses: List<Miss> = emptyList(),
    var external: External = External(),
    var hasPrev: Boolean = false,
    var prev: Fit = Fit(),
    var mobile: Boolean = false,
    var noEpochSplit: Boolean = false,
)

/** A known transmitter for self-location. */
data class Known(
    var lat: Double = 0.0, var lon: Double = 0.0, var acc: Double = 25.0, var dbm: Int = -100,
    var p0: Double = -40.0, var pathloss: Double = 2.4, var haveModel: Boolean = false, var bssid: String = "",
    var cxx: Double = 0.0, var cxy: Double = 0.0, var cyy: Double = 0.0,   // its position covariance (0 = use acc)
    var weight: Double = 1.0,                                               // by grade
)

data class SelfFix(
    var valid: Boolean = false, var lat: Double = 0.0, var lon: Double = 0.0, var acc: Double = 0.0, var rms: Double = 0.0,
    var used: Int = 0, var rejected: Int = 0,
    var excluded: Int = 0,
    var integrity: String = "",            // ok | repaired | failed | unverified
    var r95: Double = 0.0,
)

/** Local metric frame around (lat0, lon0) (the desktop's Estimator::Frame). */
class Frame(val lat0: Double = 0.0, val lon0: Double = 0.0) {
    val mx: Double = 111320.0 * cos(lat0 * PI / 180.0)
    val my: Double = 111320.0
    fun x(lon: Double) = (lon - lon0) * mx
    fun y(lat: Double) = (lat - lat0) * my
    fun lat(y: Double) = lat0 + y / my
    fun lon(x: Double) = lon0 + x / mx
}

object Estimator {
    /** Stored as the "estimatorVersion" preference: estimates older than this are recomputed. */
    const val VERSION = 4

    private const val LN10 = 2.302585092994046
    private const val ACC68 = 1.515
    private const val CHI2_2_999 = 13.815510557964274
    private const val MAXD = 4                 // per-AP device deviations δ fitted (more devices: pinned at δ = 0)
    private const val MAXP = 4 + MAXD          // parameters: x, y, P0, n, δ…

    // std::max / std::min / std::clamp semantics (not Math.max: those differ on NaN and signed zero)
    private fun cmax(a: Double, b: Double): Double = if (a < b) b else a
    private fun cmin(a: Double, b: Double): Double = if (b < a) b else a
    private fun cclamp(v: Double, lo: Double, hi: Double): Double = if (v < lo) lo else if (hi < v) hi else v
    /** std::lround: half away from zero. */
    fun lround(x: Double): Long = if (x < 0) -floor(-x + 0.5).toLong() else floor(x + 0.5).toLong()

    // ── public helpers ───────────────────────────────────────────────────────
    fun distanceM(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val r = 6371000.0; val d2r = PI / 180.0
        val dLat = (lat2 - lat1) * d2r; val dLon = (lon2 - lon1) * d2r
        val a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * d2r) * cos(lat2 * d2r) * sin(dLon / 2) * sin(dLon / 2)
        return 2 * r * asin(sqrt(a))
    }
    fun modelDbm(p0: Double, n: Double, d: Double): Double = p0 - 10.0 * n * log10(cmax(1.0, d))
    fun modelDistance(p0: Double, n: Double, dbm: Int): Double = 10.0.pow((p0 - dbm) / (10.0 * n))
    fun huberWeight(r: Double, k: Double): Double { val a = abs(r); return if (a <= k) 1.0 else k / a }

    // ── internals ────────────────────────────────────────────────────────────
    /** One place (cluster of samples). */
    private data class Cl(
        var dk: Int = 0,                  // device key (index into the sorted device names): places never mix devices
        var dev: Int = -1,                // index of its δ, −1 = the reference device (or pinned)
        var x: Double = 0.0, var y: Double = 0.0,
        var level: Double = 0.0,
        var W: Double = 1.0,
        var a: Double = 10.0,
        var sh2: Double = 36.0,
        var m: Int = 0,
        var fading: Double = 0.0,
        var sx: Double = 0.0, var sy: Double = 0.0,
        var sa: Double = 0.0,             // seed fix accuracy (clustering)
    )
    private class Rg(val x: Double, val y: Double, val r: Double, val sd2: Double)
    private class Theta(val x: Double = 0.0, val y: Double = 0.0, val p0: Double = -40.0, val n: Double = 2.4, val dv: DoubleArray = DoubleArray(MAXD)) {
        fun copy(x: Double = this.x, y: Double = this.y): Theta = Theta(x, y, p0, n, dv.copyOf())
    }
    private class Sm(val x: Double, val y: Double, val level: Double, val w0: Double, val acc: Double, @Suppress("unused") val idx: Int, val dk: Int)

    private fun median(v: DoubleArray): Double {
        if (v.isEmpty()) return 0.0
        val s = v.copyOf(); s.sort()
        val h = s.size / 2
        return if (s.size % 2 != 0) s[h] else 0.5 * (s[h - 1] + s[h])
    }
    private fun median(v: List<Double>): Double = median(v.toDoubleArray())

    private fun clamp01(v: Double): Double = if (v < 0) 0.0 else if (v > 1) 1.0 else v

    /** The radius within which two samples are one place (docs/GRADING.md §1.1). */
    private fun placeRadius(accA: Double, accB: Double, minM: Double, accK: Double): Double = cmax(minM, accK * cmin(accA, accB))

    /** 2×2 symmetric eigen-decomposition → [λ1, λ2, e1x, e1y] with λ1 ≥ λ2. */
    private fun eig2(cxx: Double, cxy: Double, cyy: Double): DoubleArray {
        val tr = cxx + cyy; val det = cxx * cyy - cxy * cxy
        val disc = sqrt(cmax(0.0, tr * tr / 4 - det))
        val l1 = tr / 2 + disc; val l2 = tr / 2 - disc
        val ex: Double; val ey: Double
        if (abs(cxy) > 1e-12) {
            val vx = l1 - cyy; val vy = cxy; val nn = sqrt(vx * vx + vy * vy)
            ex = vx / nn; ey = vy / nn
        } else if (cxx >= cyy) { ex = 1.0; ey = 0.0 }
        else { ex = 0.0; ey = 1.0 }
        return doubleArrayOf(l1, l2, ex, ey)
    }

    /** C ← the larger of C and D along C's principal axes. */
    private fun eigMax(C: DoubleArray, D: DoubleArray) {
        val e = eig2(C[0], C[1], C[2])
        var l1 = e[0]; var l2 = e[1]; val ex = e[2]; val ey = e[3]
        val fx = -ey; val fy = ex
        val d1 = ex * ex * D[0] + 2 * ex * ey * D[1] + ey * ey * D[2]
        val d2 = fx * fx * D[0] + 2 * fx * fy * D[1] + fy * fy * D[2]
        l1 = cmax(l1, d1); l2 = cmax(l2, d2)
        C[0] = l1 * ex * ex + l2 * fx * fx
        C[1] = l1 * ex * ey + l2 * fx * fy
        C[2] = l1 * ey * ey + l2 * fy * fy
    }

    private fun mat4() = Array(4) { DoubleArray(4) }
    private fun matP() = Array(MAXP) { DoubleArray(MAXP) }

    /** Solve A·x = b (k ≤ MAXP) by Gaussian elimination with partial pivoting (the same steps as [solve]). */
    private fun solveN(A: Array<DoubleArray>, b: DoubleArray, k: Int, out: DoubleArray): Boolean {
        val M = Array(MAXP) { DoubleArray(MAXP + 1) }
        for (i in 0 until k) { for (j in 0 until k) M[i][j] = A[i][j]; M[i][k] = b[i] }
        for (c in 0 until k) {
            var piv = c
            for (r in c + 1 until k) if (abs(M[r][c]) > abs(M[piv][c])) piv = r
            if (abs(M[piv][c]) < 1e-12) return false
            if (piv != c) for (j in 0..k) { val tmp = M[c][j]; M[c][j] = M[piv][j]; M[piv][j] = tmp }
            for (r in 0 until k) {
                if (r == c) continue
                val f = M[r][c] / M[c][c]
                for (j in c..k) M[r][j] -= f * M[c][j]
            }
        }
        for (i in 0 until k) out[i] = M[i][k] / M[i][i]
        return true
    }

    private fun invertN(A: Array<DoubleArray>, k: Int, out: Array<DoubleArray>): Boolean {
        for (col in 0 until k) {
            val e = DoubleArray(MAXP); e[col] = 1.0; val o = DoubleArray(MAXP)
            if (!solveN(A, e, k, o)) return false
            for (i in 0 until k) out[i][col] = o[i]
        }
        return true
    }

    private class At(val d: Double, val rho: Double, val ell: Double, val s2: Double, val r: Double, val J: DoubleArray)

    private fun evalAt(c: Cl, t: Theta, h: Double): At {
        val dx = t.x - c.x; val dy = t.y - c.y
        val rho = sqrt(dx * dx + dy * dy)
        val d = cmax(1.0, sqrt(rho * rho + h * h))
        val ell = -10.0 * log10(d)
        val b = 10.0 * t.n / LN10
        val grad = b * rho / (d * d)
        val s2 = c.sh2 + grad * grad * c.a * c.a
        val r = c.level - (t.p0 + (if (c.dev >= 0) t.dv[c.dev] else 0.0) + t.n * ell)
        val J = DoubleArray(MAXP)
        J[0] = b * dx / (d * d); J[1] = b * dy / (d * d); J[2] = -1.0; J[3] = -ell
        for (i in 0 until MAXD) J[4 + i] = if (c.dev == i) -1.0 else 0.0
        return At(d, rho, ell, s2, r, J)
    }

    private class Solver(val cl: List<Cl>, val rg: List<Rg>, val o: Options, val nd: Int = 0) {
        var mult: DoubleArray = DoubleArray(cl.size) { 1.0 }
        var tau = 1.0
        var robust = false
        val np = 4 + nd                        // parameters in use: 4 + the fitted device deviations

        fun copy(): Solver = Solver(cl, rg, o, nd).also { it.mult = mult.copyOf(); it.tau = tau; it.robust = robust }

        fun outlierDensity(): Double = o.outlierPrior * cmin(1.0, o.sigmaDb * tau / o.outlierSpanDb)
        fun rhoFn(z: Double): Double {
            if (!robust) return 0.5 * z * z
            val c = outlierDensity(); val k = (1 - o.outlierPrior) / sqrt(2 * PI)
            return -ln(k * exp(-0.5 * z * z) + c) + ln(k + c)
        }
        fun uFn(z: Double): Double {
            if (!robust) return 1.0
            val c = outlierDensity(); val g = (1 - o.outlierPrior) / sqrt(2 * PI) * exp(-0.5 * z * z)
            return g / (g + c)
        }

        fun cost(t: Theta): Double {
            var c = 0.0
            for (k in cl.indices) {
                if (mult[k] <= 0.0) continue
                val a = evalAt(cl[k], t, o.heightM)
                c += mult[k] * cl[k].W * rhoFn(a.r / (sqrt(a.s2) * tau))
            }
            for (g in rg) {
                val dx = t.x - g.x; val dy = t.y - g.y; val d = sqrt(dx * dx + dy * dy + o.heightM * o.heightM)
                c += rhoFn((g.r - d) / (sqrt(g.sd2) * tau))
            }
            val zp = (t.p0 - o.p0Mean) / o.p0Sd; val zn = (t.n - o.defaultN) / o.nSd
            c = c + 0.5 * zp * zp + 0.5 * zn * zn
            for (i in 0 until np - 4) { val zd = t.dv[i] / o.devOffsetSd; c += 0.5 * zd * zd }
            return c
        }

        /** Normal equations at t: A = Σψ JᵀJ + prior, g = Σψ J r + prior gradient. */
        fun normal(t: Theta, tauUse: Double, A: Array<DoubleArray>, g: DoubleArray) {
            for (i in 0 until MAXP) { g[i] = 0.0; for (j in 0 until MAXP) A[i][j] = 0.0 }
            for (k in cl.indices) {
                if (mult[k] <= 0.0) continue
                val a = evalAt(cl[k], t, o.heightM)
                val z = a.r / (sqrt(a.s2) * tauUse)
                val psi = mult[k] * cl[k].W * uFn(z) / (a.s2 * tauUse * tauUse)
                for (i in 0 until np) { g[i] += psi * a.J[i] * a.r; for (j in 0 until np) A[i][j] += psi * a.J[i] * a.J[j] }
            }
            for (gg in rg) {
                val dx = t.x - gg.x; val dy = t.y - gg.y; val d = sqrt(dx * dx + dy * dy + o.heightM * o.heightM)
                val r = gg.r - d; val z = r / (sqrt(gg.sd2) * tauUse)
                val psi = uFn(z) / (gg.sd2 * tauUse * tauUse)
                val J0 = -dx / d; val J1 = -dy / d
                A[0][0] += psi * J0 * J0; A[0][1] += psi * J0 * J1; A[1][0] += psi * J0 * J1; A[1][1] += psi * J1 * J1
                g[0] += psi * J0 * r; g[1] += psi * J1 * r
            }
            A[2][2] += 1.0 / (o.p0Sd * o.p0Sd); g[2] += (t.p0 - o.p0Mean) / (o.p0Sd * o.p0Sd)
            A[3][3] += 1.0 / (o.nSd * o.nSd); g[3] += (t.n - o.defaultN) / (o.nSd * o.nSd)
            for (i in 0 until np - 4) { A[4 + i][4 + i] += 1.0 / (o.devOffsetSd * o.devOffsetSd); g[4 + i] += t.dv[i] / (o.devOffsetSd * o.devOffsetSd) }
        }

        fun run(t0: Theta, iters: Int, costOut: DoubleArray?): Theta {
            var t = t0
            var lambda = 1e-2
            for (it in 0 until iters) {
                val A = matP(); val g = DoubleArray(MAXP)
                normal(t, tau, A, g)
                val c0 = cost(t)
                for (i in 0 until np) A[i][i] *= (1.0 + lambda)
                val rhs = DoubleArray(MAXP); val dx = DoubleArray(MAXP)
                for (i in 0 until np) rhs[i] = -g[i]
                if (!solveN(A, rhs, np, dx)) break
                val step = sqrt(dx[0] * dx[0] + dx[1] * dx[1])
                if (step > 300.0) { dx[0] *= 300.0 / step; dx[1] *= 300.0 / step }
                val nt = Theta(t.x + dx[0], t.y + dx[1], cclamp(t.p0 + dx[2], -90.0, 10.0), cclamp(t.n + dx[3], 1.5, 4.5))
                for (i in 0 until np - 4) nt.dv[i] = cclamp(t.dv[i] + dx[4 + i], -30.0, 30.0)
                val c1 = cost(nt)
                if (c1 <= c0) {
                    val small = sqrt(dx[0] * dx[0] + dx[1] * dx[1]) < 0.05 && c0 - c1 < 1e-7 * cmax(1.0, c0)
                    t = nt; lambda = cmax(1e-6, lambda / 3)
                    if (small) break
                } else {
                    lambda *= 8
                    if (lambda > 1e6) break
                }
            }
            if (costOut != null) costOut[0] = cost(t)
            return t
        }
    }

    /**
     * Closed-form marginal log-likelihood of a position with (P0, n) — and the [nd] device deviations δ — integrated out
     * (Gaussian priors), plus ranges and misses. [b] (if given) receives the posterior mean of (P0, n) at that position,
     * [dvOut] (if given) that of the δs.
     */
    private fun gridLogL(cl: List<Cl>, rg: List<Rg>, misses: List<Miss>, mxs: DoubleArray, mys: DoubleArray,
                         px: Double, py: Double, o: Options, nd: Int, b: DoubleArray?, dvOut: DoubleArray? = null): Double {
        val bn = 10.0 * o.defaultN / LN10
        val m0: Double; val m1: Double; var ll: Double
        if (nd == 0) {
            var S00 = 0.0; var S01 = 0.0; var S11 = 0.0; var T0 = 0.0; var T1 = 0.0; var Syy = 0.0; var logdet = 0.0
            for (c in cl) {
                val dx = px - c.x; val dy = py - c.y; val rho = sqrt(dx * dx + dy * dy)
                val d = cmax(1.0, sqrt(rho * rho + o.heightM * o.heightM)); val ell = -10.0 * log10(d)
                val grad = bn * rho / (d * d); val s2 = c.sh2 + grad * grad * c.a * c.a
                val w = c.W / s2
                S00 += w; S01 += w * ell; S11 += w * ell * ell; T0 += w * c.level; T1 += w * ell * c.level; Syy += w * c.level * c.level
                logdet += ln(s2 / c.W)
            }
            val iP = 1.0 / (o.p0Sd * o.p0Sd); val iN = 1.0 / (o.nSd * o.nSd)
            val L00 = S00 + iP; val L01 = S01; val L11 = S11 + iN
            val e0 = T0 + o.p0Mean * iP; val e1 = T1 + o.defaultN * iN
            val det = L00 * L11 - L01 * L01
            m0 = (L11 * e0 - L01 * e1) / det; m1 = (L00 * e1 - L01 * e0) / det
            val quad = Syy + o.p0Mean * o.p0Mean * iP + o.defaultN * o.defaultN * iN - (e0 * m0 + e1 * m1)
            ll = -0.5 * quad - 0.5 * ln(det) - 0.5 * logdet
        } else {
            // β = (P0, n, δ_0 … δ_nd−1), the design row of a place (1, ℓ, one-hot of its device): L = XᵀWX + Λ0, by Cholesky
            val q = 2 + nd
            val S = Array(2 + MAXD) { DoubleArray(2 + MAXD) }; val T = DoubleArray(2 + MAXD); var Syy = 0.0; var logdet = 0.0
            for (c in cl) {
                val dx = px - c.x; val dy = py - c.y; val rho = sqrt(dx * dx + dy * dy)
                val d = cmax(1.0, sqrt(rho * rho + o.heightM * o.heightM)); val ell = -10.0 * log10(d)
                val grad = bn * rho / (d * d); val s2 = c.sh2 + grad * grad * c.a * c.a
                val w = c.W / s2
                val xr = DoubleArray(2 + MAXD); xr[0] = 1.0; xr[1] = ell
                if (c.dev >= 0) xr[2 + c.dev] = 1.0
                for (i in 0 until q) { T[i] += w * xr[i] * c.level; for (j in 0 until q) S[i][j] += w * xr[i] * xr[j] }
                Syy += w * c.level * c.level
                logdet += ln(s2 / c.W)
            }
            val lam = DoubleArray(2 + MAXD); val mu = DoubleArray(2 + MAXD); val e = DoubleArray(2 + MAXD)
            lam[0] = 1.0 / (o.p0Sd * o.p0Sd); mu[0] = o.p0Mean
            lam[1] = 1.0 / (o.nSd * o.nSd); mu[1] = o.defaultN
            for (i in 2 until q) { lam[i] = 1.0 / (o.devOffsetSd * o.devOffsetSd); mu[i] = 0.0 }
            for (i in 0 until q) { S[i][i] += lam[i]; e[i] = T[i] + lam[i] * mu[i] }
            val C = Array(2 + MAXD) { DoubleArray(2 + MAXD) }; var logdetL = 0.0
            for (i in 0 until q)
                for (j in 0..i) {
                    var s = S[i][j]
                    for (k in 0 until j) s -= C[i][k] * C[j][k]
                    if (i == j) { if (s <= 0) return -1e300; C[i][i] = sqrt(s); logdetL += 2.0 * ln(C[i][i]) }
                    else C[i][j] = s / C[j][j]
                }
            val y = DoubleArray(2 + MAXD); val m = DoubleArray(2 + MAXD)
            for (i in 0 until q) { var s = e[i]; for (k in 0 until i) s -= C[i][k] * y[k]; y[i] = s / C[i][i] }
            for (i in q - 1 downTo 0) { var s = y[i]; for (k in i + 1 until q) s -= C[k][i] * m[k]; m[i] = s / C[i][i] }
            var quad = Syy
            for (i in 0 until q) quad += lam[i] * mu[i] * mu[i]
            for (i in 0 until q) quad -= e[i] * m[i]
            m0 = m[0]; m1 = m[1]
            if (dvOut != null) for (i in 0 until nd) dvOut[i] = m[2 + i]
            ll = -0.5 * quad - 0.5 * logdetL - 0.5 * logdet
        }
        for (g in rg) {
            val dx = px - g.x; val dy = py - g.y; val d = sqrt(dx * dx + dy * dy + o.heightM * o.heightM)
            ll += -0.5 * (g.r - d) * (g.r - d) / g.sd2
        }
        for (j in misses.indices) {
            val dx = px - mxs[j]; val dy = py - mys[j]; val d = cmax(1.0, sqrt(dx * dx + dy * dy + o.heightM * o.heightM))
            val mu = m0 + m1 * (-10.0 * log10(d))
            val pDet = (1.0 - o.missFloor) * normCdf((mu - o.sensitivity) / o.sigmaDb)
            ll += minOf(misses[j].count, 3) * ln(cmax(1e-12, 1.0 - pDet))
        }
        if (b != null) { b[0] = m0; b[1] = m1 }
        return ll
    }

    /** Wilson–Hilferty. */
    private fun chi2Quantile99(dof0: Int): Double {
        val dof = if (dof0 < 1) 1 else dof0
        val k = dof.toDouble(); val z = 2.3263478740408408; val t = 1 - 2 / (9 * k) + z * sqrt(2 / (9 * k))
        return k * t * t * t
    }

    private fun ranks(v: List<Double>): DoubleArray {
        val n = v.size
        val idx = (0 until n).sortedWith(Comparator { a, b -> if (v[a] < v[b]) -1 else if (v[b] < v[a]) 1 else a.compareTo(b) })
        val out = DoubleArray(n)
        var i = 0
        while (i < n) {
            var j = i
            while (j + 1 < n && v[idx[j + 1]] == v[idx[i]]) ++j
            val r = 0.5 * (i + j) + 1
            for (q in i..j) out[idx[q]] = r
            i = j + 1
        }
        return out
    }

    private fun pearson(a: DoubleArray, b: DoubleArray): Double {
        val n = a.size
        var ma = 0.0; var mb = 0.0; for (i in 0 until n) { ma += a[i]; mb += b[i] }
        ma /= n; mb /= n
        var sab = 0.0; var saa = 0.0; var sbb = 0.0
        for (i in 0 until n) { sab += (a[i] - ma) * (b[i] - mb); saa += (a[i] - ma) * (a[i] - ma); sbb += (b[i] - mb) * (b[i] - mb) }
        return if (saa > 0 && sbb > 0) sab / sqrt(saa * sbb) else 0.0
    }

    private fun pointInHull(cl: List<Cl>, px: Double, py: Double): Boolean {
        val n = cl.size
        if (n < 3) return false
        val idx = (0 until n).sortedWith(Comparator { a, b ->
            val less = cl[a].x < cl[b].x || (cl[a].x == cl[b].x && (cl[a].y < cl[b].y || (cl[a].y == cl[b].y && a < b)))
            val greater = cl[b].x < cl[a].x || (cl[b].x == cl[a].x && (cl[b].y < cl[a].y || (cl[b].y == cl[a].y && b < a)))
            if (less) -1 else if (greater) 1 else 0
        })
        fun cross(o: Int, a: Int, b: Int) = (cl[a].x - cl[o].x) * (cl[b].y - cl[o].y) - (cl[a].y - cl[o].y) * (cl[b].x - cl[o].x)
        val h = IntArray(2 * n)
        var k = 0
        for (i in 0 until n) { while (k >= 2 && cross(h[k - 2], h[k - 1], idx[i]) <= 0) --k; h[k++] = idx[i] }
        run {
            val t = k + 1
            var i = n - 2
            while (i >= 0) { while (k >= t && cross(h[k - 2], h[k - 1], idx[i]) <= 0) --k; h[k++] = idx[i]; --i }
        }
        val m = k - 1
        if (m < 3) return false
        var area = 0.0
        for (i in 0 until m) { val a = cl[h[i]]; val b = cl[h[i + 1]]; area += a.x * b.y - b.x * a.y }
        if (abs(area) < 1.0) return false                  // collinear: no inside
        for (i in 0 until m) {
            val a = cl[h[i]]; val b = cl[h[i + 1]]
            if ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x) < -1e-9) return false
        }
        return true
    }

    private fun setEllipse(f: Fit, C: DoubleArray) {
        f.cxx = C[0]; f.cxy = C[1]; f.cyy = C[2]
        val e = ellipse(C[0], C[1], C[2])
        f.semiMajor = e[0]; f.semiMinor = e[1]; f.orientDeg = e[2]
        f.acc = cmax(8.0, f.semiMajor)
        f.r95 = radiusFor(f.semiMajor, f.semiMinor, 0.95)
        f.cep50 = radiusFor(f.semiMajor, f.semiMinor, 0.5)
        f.pWithin25 = probWithin(f.semiMajor, f.semiMinor, 25.0)
    }

    private fun freshness(newest: Long, now: Long): Double {
        if (now <= 0 || newest <= 0) return 1.0
        val days = cmax(0.0, (now - newest).toDouble() / 86400.0)
        return cmax(0.3, 0.5.pow(days / 180.0))
    }

    private fun precisionComp(r95: Double): Double = clamp01(ln(300.0 / cmax(1e-6, r95)) / ln(30.0))

    private fun nisFactor(nis: Double): Double = if (nis > 0) exp(-cmax(0.0, nis - 3.0) / 3.0) else 1.0

    // ── helpers ──────────────────────────────────────────────────────────────
    /** Solve A·x = b (k ≤ 4) by Gaussian elimination with partial pivoting. */
    fun solve(A: Array<DoubleArray>, b: DoubleArray, k: Int, out: DoubleArray): Boolean {
        val M = Array(4) { DoubleArray(5) }
        for (i in 0 until k) { for (j in 0 until k) M[i][j] = A[i][j]; M[i][k] = b[i] }
        for (c in 0 until k) {
            var piv = c
            for (r in c + 1 until k) if (abs(M[r][c]) > abs(M[piv][c])) piv = r
            if (abs(M[piv][c]) < 1e-12) return false
            if (piv != c) for (j in 0..k) { val tmp = M[c][j]; M[c][j] = M[piv][j]; M[piv][j] = tmp }
            for (r in 0 until k) {
                if (r == c) continue
                val f = M[r][c] / M[c][c]
                for (j in c..k) M[r][j] -= f * M[c][j]
            }
        }
        for (i in 0 until k) out[i] = M[i][k] / M[i][i]
        return true
    }

    /** 2×2 symmetric inverse [[a b][b d]] → out = [o0, o1, o2]. */
    fun invert2(a: Double, b: Double, d: Double, out: DoubleArray): Boolean {
        val det = a * d - b * b
        if (abs(det) < 1e-12) return false
        out[0] = d / det; out[1] = -b / det; out[2] = a / det
        return true
    }

    /** 1-σ ellipse of a 2×2 covariance → [major, minor, orientDeg] (orientDeg exactly as the desktop computes it). */
    fun ellipse(cxx: Double, cxy: Double, cyy: Double): DoubleArray {
        val tr = cxx + cyy; val det = cxx * cyy - cxy * cxy
        val disc = sqrt(cmax(0.0, tr * tr / 4 - det))
        val l1 = cmax(0.0, tr / 2 + disc); val l2 = cmax(0.0, tr / 2 - disc)
        // the major axis (cxy, λ1 − cxx) as a bearing: degrees clockwise from north, in [0, 180)
        var b = 90.0 - atan2(l1 - cxx, cxy) * 180.0 / PI
        while (b < 0) b += 180.0
        while (b >= 180.0) b -= 180.0
        var orient = b
        // axis-aligned or (numerically) circular: no real orientation, and the eigenvector is last-bit noise
        if (abs(cxy) < 1e-12) orient = if (cxx >= cyy) 90.0 else 0.0
        if (disc <= 1e-6 * abs(tr)) orient = 0.0                  // circular: bearing 0 by convention
        return doubleArrayOf(sqrt(l1), sqrt(l2), orient)
    }

    /** erfc with fractional error < 1.2e-7 (Numerical Recipes erfcc): identical to the desktop. */
    fun normCdf(z: Double): Double {
        val x = -z / sqrt(2.0)
        val t = 1.0 / (1.0 + 0.5 * abs(x))
        val r = t * exp(-x * x - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 + t * (-0.18628806 + t * (0.27886807 +
                t * (-1.13520398 + t * (1.48851587 + t * (-0.82215223 + t * 0.17087277)))))))))
        val erfc = if (x >= 0) r else 2.0 - r
        return 0.5 * erfc
    }

    /** P(|X| < r) for X ~ N(0, diag(s1², s2²)). */
    fun probWithin(s1: Double, s2: Double, r: Double): Double {
        if (r <= 0) return 0.0
        val sa = cmax(1e-9, cmax(s1, s2)); val sb = cmax(1e-9, cmin(s1, s2))
        val N = 256
        val h = PI / N
        var sum = 0.0
        for (i in 0..N) {
            val t = -PI / 2 + i * h; val x = r * sin(t); val ct = cos(t)
            val fv = exp(-0.5 * (x / sa) * (x / sa)) / (sa * sqrt(2 * PI)) * (2 * normCdf(r * ct / sb) - 1) * r * ct
            sum += if (i == 0 || i == N) fv else (if (i % 2 != 0) 4 * fv else 2 * fv)
        }
        return cclamp(sum * h / 3, 0.0, 1.0)
    }

    /** The radius holding probability p. */
    fun radiusFor(s1: Double, s2: Double, p: Double): Double {
        var lo = 0.0; var hi = 5.0 * cmax(1e-6, cmax(s1, s2))
        for (i in 0 until 50) { val mid = 0.5 * (lo + hi); if (probWithin(s1, s2, mid) < p) lo = mid else hi = mid }
        return 0.5 * (lo + hi)
    }

    fun letterFor(score: Double): String =
        if (score >= 85) "A" else if (score >= 70) "B" else if (score >= 55) "C" else if (score >= 40) "D" else if (score >= 20) "E" else "F"

    private fun lowerBound(l: String): Double = when (l) { "A" -> 85.0; "B" -> 70.0; "C" -> 55.0; "D" -> 40.0; "E" -> 20.0; else -> 0.0 }

    fun qualityFor(g: String): String = when (g) {
        "A", "B" -> "good"
        "C", "D" -> "fair"
        "E", "F", "R" -> "poor"
        else -> "none"
    }

    /** Distinct places, as the fitter clusters them. */
    fun vantageCount(obs: List<Obs>, clusterMinM: Double = 5.0, clusterAccK: Double = 1.5): Int {
        if (obs.isEmpty()) return 0
        val fr = Frame(obs[0].lat, obs[0].lon)
        val seeds = ArrayList<DoubleArray>()
        for (o in obs) {
            val x = fr.x(o.lon); val y = fr.y(o.lat)
            var found = false
            for (s in seeds) if (hypot(x - s[0], y - s[1]) < placeRadius(o.acc, s[2], clusterMinM, clusterAccK)) { found = true; break }
            if (!found) seeds.add(doubleArrayOf(x, y, o.acc))
        }
        return seeds.size
    }

    // ── grading ──────────────────────────────────────────────────────────────
    /** Score and letter from the metrics already in f (with hysteresis against prev). */
    fun grade(f: Fit, prev: Fit?) {
        if (f.kind == "mobile") { f.score = 0.0; f.grade = "M"; f.pendingGrade = ""; f.quality = "none"; return }
        if (!f.valid) { f.score = 0.0; f.grade = ""; f.pendingGrade = ""; f.quality = "none"; return }
        if (f.kind == "region") {
            f.score = cmin(39.0, 100.0 * f.cP)
            f.grade = "R"; f.pendingGrade = ""; f.quality = "poor"
            return
        }
        val comps = doubleArrayOf(f.cP, f.cG, f.cE, f.cF, f.cS, f.cT, f.cX)
        val w = doubleArrayOf(0.30, 0.20, 0.15, 0.15, 0.10, 0.05, 0.05)
        var sw = 0.0; var sl = 0.0
        for (i in 0 until 7) {
            if (i == 6 && f.cX < 0) continue
            sw += w[i]; sl += w[i] * ln(cmax(1e-3, comps[i]))
        }
        var score = 100.0 * exp(sl / sw)
        if (f.ambiguous || f.modes >= 2) score = cmin(score, 49.0)
        if (!f.inHull && f.linRatio < 0.02) score = cmin(score, 59.0)
        f.score = score
        val raw = letterFor(score)
        var letter = raw; f.pendingGrade = ""
        if (prev != null && prev.valid && prev.kind == "fix" && prev.grade.isNotEmpty() && prev.grade != "R" && prev.grade != "M" && raw != prev.grade) {
            val better = lowerBound(raw) > lowerBound(prev.grade)
            val clear = if (better) score >= lowerBound(raw) + 3 else score <= lowerBound(prev.grade) - 3
            if (!clear && prev.pendingGrade != raw) { letter = prev.grade; f.pendingGrade = raw }
        }
        f.grade = letter
        f.quality = qualityFor(letter)
    }

    // ── the AP fitter ────────────────────────────────────────────────────────
    fun fitAp(samples: List<Obs>, now: Long = 0, opt: Options = Options(), ctx: Context = Context()): Fit {
        val f = Fit(); f.n = samples.size; f.pathloss = opt.defaultN; f.p0 = opt.p0Mean; f.updated = now
        if (ctx.mobile) {
            f.kind = "mobile"
            if (samples.isNotEmpty()) { f.lat = samples.last().lat; f.lon = samples.last().lon }
            grade(f, null)
            return f
        }
        // Usable samples: precise enough; coarse ones only when nothing better exists
        val use = ArrayList<Obs>()
        for (o in samples) if (o.acc > 0 && o.acc <= opt.geomAcc) use.add(o)
        if (use.isEmpty()) for (o in samples) if (o.acc > 0 && o.acc <= opt.maxAcc) use.add(o)
        f.n = use.size
        if (use.isEmpty()) { grade(f, null); return f }

        // Did the AP move? Fit the last 30 days against everything older (docs/GRADING.md §3.8)
        if (!ctx.noEpochSplit) {
            var tmin = 0L; var tmax = 0L
            for (o in use) if (o.t > 0) { tmin = if (tmin == 0L) o.t else minOf(tmin, o.t); tmax = maxOf(tmax, o.t) }
            if (tmax - tmin > 60L * 86400L) {
                val recent = ArrayList<Obs>(); val old = ArrayList<Obs>()
                for (o in use) if (o.t > 0 && o.t >= tmax - 30L * 86400L) recent.add(o) else old.add(o)
                if (recent.size >= 3 && old.size >= 3) {
                    val q = opt.copy(bootstrap = 0, jackMaxK = 0)
                    val c2 = Context(deviceOffset = ctx.deviceOffset, noEpochSplit = true)
                    val a = fitAp(old, now, q, c2); val b = fitAp(recent, now, q, c2)
                    if (a.kind == "fix" && b.kind == "fix") {
                        val fa = Frame(a.lat, a.lon)
                        val dx = fa.x(b.lon); val dy = fa.y(b.lat)
                        val inv = DoubleArray(3)
                        if (invert2(a.cxx + b.cxx, a.cxy + b.cxy, a.cyy + b.cyy, inv)) {
                            val d2 = dx * dx * inv[0] + 2 * dx * dy * inv[1] + dy * dy * inv[2]
                            if (d2 > CHI2_2_999) {
                                val c3 = ctx.copy(noEpochSplit = true)
                                val r = fitAp(recent, now, opt, c3)
                                r.moved = true
                                return r
                            }
                        }
                    }
                }
            }
        }

        // ── samples → places ──
        val fr = Frame(use[0].lat, use[0].lon)
        val sm = ArrayList<Sm>()
        val sessions = HashSet<String>(); val devices = HashSet<String>()
        val devNames = ArrayList<String>()                     // sorted: device keys are deterministic (C++ and Kotlin agree)
        for (o in use) if (!devNames.contains(o.device)) devNames.add(o.device)
        devNames.sort()
        for (i in use.indices) {
            val o = use[i]
            var w0 = 1.0
            if (now > 0 && o.t > 0) w0 = cmax(0.15, exp(-cmax(0.0, (now - o.t).toDouble() / 86400.0) / opt.ageTauDays))
            w0 *= cmax(0.05, o.weight)
            sm.add(Sm(fr.x(o.lon), fr.y(o.lat), o.dbm.toDouble() - (ctx.deviceOffset[o.device] ?: 0.0), w0, o.acc, i, devNames.indexOf(o.device)))
            sessions.add(o.device + "|" + (if (o.t > 0) o.t / 86400L else -1L).toString())
            devices.add(o.device)
            f.newest = maxOf(f.newest, o.t)
        }
        f.sessions = sessions.size; f.devices = devices.size
        val accs = DoubleArray(sm.size) { sm[it].acc }
        // Places: a sample joins the first seed within max(clusterMinM, clusterAccK × the better of the two fixes): a
        // smoothed walk keeps its geometry, poor indoor fixes still merge (was one radius, max(15 m, median accuracy)).
        // A place holds one device: devices hear the same AP differently (the δ below), so their levels are not pooled.
        var members = ArrayList<ArrayList<Int>>()
        var cl = ArrayList<Cl>()
        var grow = 1.0
        while (true) {
            members = ArrayList(); cl = ArrayList()
            for (i in sm.indices) {
                var found = -1
                for (k in cl.indices)
                    if (cl[k].dk == sm[i].dk && hypot(sm[i].x - cl[k].sx, sm[i].y - cl[k].sy) < grow * placeRadius(sm[i].acc, cl[k].sa, opt.clusterMinM, opt.clusterAccK)) { found = k; break }
                if (found < 0) { val c = Cl(); c.dk = sm[i].dk; c.sx = sm[i].x; c.sy = sm[i].y; c.sa = sm[i].acc; cl.add(c); members.add(arrayListOf(i)) }
                else members[found].add(i)
            }
            if (cl.size <= opt.maxClusters) break
            grow *= 1.5
        }
        val K = cl.size
        // Per-AP device deviations: the device with the most places is the reference (δ = 0, carries P0); the next MAXD by
        // places (ties: name order) each get a δ ~ N(0, devOffsetSd²); any further device stays pinned at its calibrated offset
        var nd = 0
        if (devNames.size >= 2) {
            val cnt = IntArray(devNames.size)
            for (c in cl) ++cnt[c.dk]
            val byCnt = (0 until devNames.size).sortedWith(Comparator { a, b -> if (cnt[a] > cnt[b] || (cnt[a] == cnt[b] && a < b)) -1 else if (cnt[b] > cnt[a] || (cnt[b] == cnt[a] && b < a)) 1 else 0 })
            val devIdx = IntArray(devNames.size) { -1 }
            var r = 1
            while (r < byCnt.size && nd < MAXD) { if (cnt[byCnt[r]] > 0) devIdx[byCnt[r]] = nd++; ++r }
            for (c in cl) c.dev = devIdx[c.dk]
        }
        for (k in 0 until K) {
            var sw = 0.0; var sx = 0.0; var sy = 0.0; val lv = ArrayList<Double>(); val ac = ArrayList<Double>()
            for (i in members[k]) { sw += sm[i].w0; sx += sm[i].w0 * sm[i].x; sy += sm[i].w0 * sm[i].y; lv.add(sm[i].level); ac.add(sm[i].acc) }
            val c = cl[k]
            c.x = sx / sw; c.y = sy / sw; c.m = members[k].size; c.W = sw / c.m
            c.level = median(lv); c.a = median(ac) / ACC68
            c.sh2 = opt.sigmaDb * opt.sigmaDb * (opt.rhoIn + (1 - opt.rhoIn) / c.m)
            if (c.m >= 2) {
                var mu = 0.0; for (v in lv) mu += v; mu /= c.m
                var ss = 0.0; for (v in lv) ss += (v - mu) * (v - mu)
                c.fading = sqrt(ss / (c.m - 1))
            }
        }
        run {   // age/status weights are relative: they shift influence between places, not the total information
            var mw = 0.0; for (c in cl) mw += c.W; mw /= K
            if (mw > 0) for (c in cl) c.W /= mw
        }
        f.vantage = K
        run { var s = 0.0; var q = 0; for (c in cl) if (c.m >= 2) { s += c.fading; ++q }; f.fadingDb = if (q != 0) s / q else 0.0 }
        val rg = ArrayList<Rg>()
        for (o in use) if (o.rangeM > 0) { val a = o.acc / ACC68; rg.add(Rg(fr.x(o.lon), fr.y(o.lat), o.rangeM, cmax(0.25, o.rangeSd * o.rangeSd) + a * a)) }

        // A Wi-Fi AP heard at places more than 5 km apart travels (Ichnaea's rule)
        var maxPair = 0.0
        for (a in 0 until K) for (b in a + 1 until K) maxPair = cmax(maxPair, hypot(cl[a].x - cl[b].x, cl[a].y - cl[b].y))
        if (maxPair > 5000) {
            f.kind = "mobile"; f.lat = use.last().lat; f.lon = use.last().lon
            grade(f, null)
            return f
        }

        // ── grid posterior ──
        var cx0 = 0.0; var cy0 = 0.0; var cw = 0.0; var maxLevel = -200.0
        for (c in cl) { val w = c.W * 10.0.pow(c.level / 40.0); cw += w; cx0 += w * c.x; cy0 += w * c.y; maxLevel = cmax(maxLevel, c.level) }
        cx0 /= cw; cy0 /= cw
        val cx = cx0; val cy = cy0
        val wcx = cx; val wcy = cy
        var reach = 0.0; for (c in cl) reach = cmax(reach, hypot(c.x - cx, c.y - cy))
        val dPlaus = cclamp(modelDistance(opt.p0Mean + opt.p0Sd, cmax(1.6, opt.defaultN - opt.nSd), lround(maxLevel).toInt()), 50.0, 400.0)
        val W = cclamp(reach + dPlaus, opt.minHalfWidth, opt.maxHalfWidth)
        val G = opt.gridN
        val cs = 2 * W / G
        val mxs = DoubleArray(ctx.misses.size) { fr.x(ctx.misses[it].lon) }
        val mys = DoubleArray(ctx.misses.size) { fr.y(ctx.misses[it].lat) }
        val ll = DoubleArray(G * G); val post = DoubleArray(G * G)
        var order = IntArray(G * G)
        var llMax = -1e300; var psum = 0.0; var pmx = 0.0; var pmy = 0.0; var hpdR95 = 0.0
        val gC = DoubleArray(3)
        fun runGrid(cls: List<Cl>) {
            for (j in 0 until G)
                for (i in 0 until G) {
                    val px = cx - W + (i + 0.5) * cs; val py = cy - W + (j + 0.5) * cs
                    // range prior: an AP is rarely far beyond the nearest place it was heard from
                    var near = 1e300; for (c in cls) near = cmin(near, hypot(px - c.x, py - c.y))
                    ll[j * G + i] = gridLogL(cls, rg, ctx.misses, mxs, mys, px, py, opt, nd, null) - near / opt.rangePriorM
                }
            llMax = -1e300; for (v in ll) llMax = cmax(llMax, v)
            psum = 0.0; pmx = 0.0; pmy = 0.0
            for (j in 0 until G) for (i in 0 until G) {
                val p = exp(ll[j * G + i] - llMax); post[j * G + i] = p; psum += p
                pmx += p * (cx - W + (i + 0.5) * cs); pmy += p * (cy - W + (j + 0.5) * cs)
            }
            pmx /= psum; pmy /= psum
            gC[0] = 0.0; gC[1] = 0.0; gC[2] = 0.0
            for (j in 0 until G) for (i in 0 until G) {
                val p = post[j * G + i] / psum; val dx = cx - W + (i + 0.5) * cs - pmx; val dy = cy - W + (j + 0.5) * cs - pmy
                gC[0] += p * dx * dx; gC[1] += p * dx * dy; gC[2] += p * dy * dy
            }
            gC[0] += cs * cs / 12; gC[2] += cs * cs / 12
            order = (0 until G * G).sortedWith(Comparator { a, b -> if (ll[a] > ll[b]) -1 else if (ll[b] > ll[a]) 1 else a.compareTo(b) }).toIntArray()
            val hpd = BooleanArray(G * G); var acc95 = 0.0; var nHpd = 0
            for (idx in order) { if (acc95 >= 0.95 * psum) break; hpd[idx] = true; acc95 += post[idx]; ++nHpd }
            hpdR95 = sqrt(nHpd * cs * cs / PI)
            // significant modes: 8-connected components of the HPD region holding ≥ 5 % of the mass
            val comp = IntArray(G * G) { -1 }; var modes = 0
            for (idx in order) {
                if (!hpd[idx] || comp[idx] >= 0) continue
                val stack = ArrayList<Int>(); stack.add(idx); comp[idx] = idx; var mass = 0.0
                while (stack.isNotEmpty()) {
                    val c = stack.removeAt(stack.size - 1); mass += post[c]
                    val ci = c % G; val cj = c / G
                    for (dj in -1..1) for (di in -1..1) {
                        val ni = ci + di; val nj = cj + dj
                        if (ni < 0 || nj < 0 || ni >= G || nj >= G) continue
                        val nidx = nj * G + ni
                        if (hpd[nidx] && comp[nidx] < 0) { comp[nidx] = idx; stack.add(nidx) }
                    }
                }
                if (mass >= 0.05 * psum) ++modes
            }
            f.modes = modes
        }
        runGrid(cl)
        // Seeds: up to three grid maxima (non-maximum suppression over 3 cells), best first
        val seeds = ArrayList<Int>()
        for (idx in order) {
            if (ll[idx] < llMax - 8 || seeds.size >= 3) break
            var near = false
            for (s in seeds) if (abs(s % G - idx % G) <= 3 && abs(s / G - idx / G) <= 3) { near = true; break }
            if (!near) seeds.add(idx)
        }

        // Place geometry (independent of the solution)
        var gsw = 0.0; var gmx0 = 0.0; var gmy0 = 0.0
        for (c in cl) { gsw += c.W; gmx0 += c.W * c.x; gmy0 += c.W * c.y }
        gmx0 /= gsw; gmy0 /= gsw
        val gmx = gmx0; val gmy = gmy0
        var sxx = 0.0; var sxy = 0.0; var syy = 0.0
        for (c in cl) { sxx += c.W * (c.x - gmx) * (c.x - gmx); sxy += c.W * (c.x - gmx) * (c.y - gmy); syy += c.W * (c.y - gmy) * (c.y - gmy) }
        val eg = eig2(sxx / gsw, sxy / gsw, syy / gsw)
        val mu1 = eg[0]; val mu2 = eg[1]; val e1x = eg[2]; val e1y = eg[3]
        f.linRatio = if (mu1 > 1e-9) cmax(0.0, mu2) / mu1 else 0.0
        run { var s1 = 0.0; var s2 = 0.0; for (c in cl) { val w = c.W / c.sh2; s1 += w; s2 += w * w }; f.ess = if (s2 > 0) s1 * s1 / s2 else 0.0 }

        var best = Theta(); var haveLm = false; var tauEff = 1.0
        var altGap = -1.0                                      // cost (≈ nats) of the alternative solution above the best
        val sv = Solver(cl, rg, opt, nd)
        val bb = DoubleArray(2)
        if (K >= 3) {
            // Stage A (robust, nominal scale) from every seed and the mirror of the best; Stage B (robust at the
            // pooled scale τ) from each result
            val starts = ArrayList<Theta>()
            val pts = ArrayList<DoubleArray>()
            for (s in seeds) pts.add(doubleArrayOf(cx - W + (s % G + 0.5) * cs, cy - W + (s / G + 0.5) * cs))
            run {   // and the classic centroids, which a few loud outliers cannot all capture: signal-weighted, plain, loudest quarter
                var ux = 0.0; var uy = 0.0; for (c in cl) { ux += c.x; uy += c.y }
                val loud = (0 until K).sortedWith(Comparator { a, b -> if (cl[a].level > cl[b].level) -1 else if (cl[b].level > cl[a].level) 1 else a.compareTo(b) })
                val q = max(3, K / 4); var lx = 0.0; var ly = 0.0
                for (i in 0 until q) { lx += cl[loud[i]].x; ly += cl[loud[i]].y }
                pts.add(doubleArrayOf(wcx, wcy)); pts.add(doubleArrayOf(ux / K, uy / K)); pts.add(doubleArrayOf(lx / q, ly / q))
            }
            for (pt in pts) {
                var dup = false; for (t in starts) if (hypot(t.x - pt[0], t.y - pt[1]) < 10) { dup = true; break }
                if (dup) continue
                val dv0 = DoubleArray(MAXD)
                gridLogL(cl, rg, ctx.misses, mxs, mys, pt[0], pt[1], opt, nd, bb, dv0)
                val t = Theta(pt[0], pt[1], cclamp(bb[0], -90.0, 10.0), cclamp(bb[1], 1.5, 4.5))
                for (i in 0 until nd) t.dv[i] = cclamp(dv0[i], -30.0, 30.0)
                starts.add(t)
            }
            if (starts.isNotEmpty()) {
                val b = starts[0]
                val vx = b.x - gmx; val vy = b.y - gmy; val along = vx * e1x + vy * e1y
                val m = b.copy(x = gmx + 2 * along * e1x - vx, y = gmy + 2 * along * e1y - vy)
                if (hypot(m.x - b.x, m.y - b.y) > 10) starts.add(m)
            }
            sv.robust = true                                   // Stage A: robust at the nominal scale
            val stA = ArrayList<Theta>(); val costA = ArrayList<Double>()
            val co = DoubleArray(1)
            for (t0 in starts) { stA.add(sv.run(t0, 10, co)); costA.add(co[0]) }
            var ia = 0; for (i in 1 until stA.size) if (costA[i] < costA[ia]) ia = i
            run {   // robust scale: MAD of the standardised residuals, pooled with a prior of 1 (4 pseudo-observations)
                val z = DoubleArray(K) { val a = evalAt(cl[it], stA[ia], opt.heightM); a.r / sqrt(a.s2) }
                val med = median(z); val dev = DoubleArray(K) { abs(z[it] - med) }
                val mad = 1.4826 * median(dev)
                sv.tau = cmax(0.5, sqrt((4.0 + K * mad * mad) / (4.0 + K)))
            }
            val stB = ArrayList<Theta>(); val costB = ArrayList<Double>()
            for (t0 in stA) { stB.add(sv.run(t0, opt.maxIter, co)); costB.add(co[0]) }
            var ib = 0; for (i in 1 until stB.size) if (costB[i] < costB[ib]) ib = i
            run {   // the mirror of the SOLUTION across the places' principal axis: from places along a line the AP and its reflection
                // explain the levels equally well, and no seed need have started on the other side (docs/GRADING.md §1.4)
                val b = stB[ib]
                val vx = b.x - gmx; val vy = b.y - gmy; val along = vx * e1x + vy * e1y
                val m = b.copy(x = gmx + 2 * along * e1x - vx, y = gmy + 2 * along * e1y - vy)
                if (m.x.isFinite() && m.y.isFinite() && hypot(m.x - b.x, m.y - b.y) > 10) {
                    stB.add(sv.run(m, opt.maxIter, co)); costB.add(co[0])
                    if (costB[costB.size - 1] < costB[ib]) ib = stB.size - 1
                }
            }
            best = stB[ib]; haveLm = best.x.isFinite() && best.y.isFinite() && costB[ib].isFinite()
            var ialt = -1
            for (i in stB.indices)
                if (i != ib && hypot(stB[i].x - best.x, stB[i].y - best.y) > 10 && (ialt < 0 || costB[i] < costB[ialt])) ialt = i
            f.sigmaDb = sv.tau * opt.sigmaDb
            if (haveLm) {   // the posterior again with each place down-weighted as the robust fit did
                val rw = cl.map { it.copy() }
                for (k in 0 until K) { val a = evalAt(cl[k], best, opt.heightM); val z = a.r / (sqrt(a.s2) * sv.tau); rw[k].W = cmax(1e-9, cl[k].W * sv.uFn(z)) }
                runGrid(rw)
            }
            if (haveLm && ialt >= 0) { f.altLat = fr.lat(stB[ialt].y); f.altLon = fr.lon(stB[ialt].x); altGap = costB[ialt] - costB[ib]; f.ambiguous = altGap < 2.0 }
        }

        // ── the answer: LM solution (≥ 3 places) or the grid posterior ──
        var sol: Theta
        val C = DoubleArray(3)
        val crlbC = DoubleArray(3)
        val Finv = matP(); var haveFinv = false
        val np = 4 + nd
        if (haveLm) {
            sol = best
            // Gudmundson: N_eff = 1ᵀR⁻¹1, R_ij = exp(−|q_i − q_j|/d_c): the effectively independent places
            var neff = K.toDouble()
            run {
                val L = DoubleArray(K * K)
                var ok = true
                var i = 0
                while (i < K && ok) {
                    for (j in 0..i) {
                        var s = exp(-hypot(cl[i].x - cl[j].x, cl[i].y - cl[j].y) / opt.shadowCorrM) + (if (i == j) 1e-9 else 0.0)
                        for (q in 0 until j) s -= L[i * K + q] * L[j * K + q]
                        if (i == j) { if (s <= 0) { ok = false; break }; L[i * K + i] = sqrt(s) }
                        else L[i * K + j] = s / L[j * K + j]
                    }
                    ++i
                }
                if (ok) {
                    val z = DoubleArray(K)
                    for (ii in 0 until K) { var s = 1.0; for (q in 0 until ii) s -= L[ii * K + q] * z[q]; z[ii] = s / L[ii * K + ii] }
                    neff = 0.0; for (v in z) neff += v * v
                    neff = cclamp(neff, 1.0, K.toDouble())
                }
            }
            // The noise scale τ² = σ²/σ0²: scaled-inverse-χ² posterior mean, prior ν0 centred on σ0, data = the robust-weighted
            // residual SS over the effectively independent places: E[σ²]/σ0² = (ν0 − 2 + SS·N_eff/K)/(ν0 − 2 + ν_d)
            run {
                val H = matP(); val A1 = matP(); val I1 = matP(); var ss = 0.0; var kin = 0.0
                for (k in 0 until K) {
                    val a = evalAt(cl[k], sol, opt.heightM)
                    val z = a.r / sqrt(a.s2); val w = cl[k].W * sv.uFn(z / sv.tau)
                    ss += w * z * z; kin += w
                    for (i in 0 until np) for (j in 0 until np) H[i][j] += w / a.s2 * a.J[i] * a.J[j]
                }
                for (i in 0 until np) for (j in 0 until np) A1[i][j] = H[i][j]
                A1[2][2] += 1.0 / (opt.p0Sd * opt.p0Sd); A1[3][3] += 1.0 / (opt.nSd * opt.nSd)
                for (i in 4 until np) A1[i][i] += 1.0 / (opt.devOffsetSd * opt.devOffsetSd)
                var peff = np.toDouble()
                if (invertN(A1, np, I1)) { peff = 0.0; for (i in 0 until np) for (j in 0 until np) peff += I1[i][j] * H[j][i] }
                val nud = cmax(0.0, kin - peff) * neff / K; val v0 = cmax(0.0, opt.sigmaPriorDof - 2)
                val tau2 = (v0 + (if (nud > 0) ss * neff / K else 0.0)) / cmax(1e-9, v0 + nud)
                tauEff = sqrt(cmax(0.01, tau2))
            }
            // Laplace at that scale as a sandwich A⁻¹·B·A⁻¹, B adding the places' shared shadowing τ²σ0²ρ_in·R_ij (was × K/N_eff)
            val A = matP(); val g = DoubleArray(MAXP)
            sv.normal(sol, tauEff, A, g)
            if (invertN(A, np, Finv)) {
                haveFinv = true
                val Jw = DoubleArray(MAXP * K)
                for (k in 0 until K) {
                    val a = evalAt(cl[k], sol, opt.heightM)
                    val psi = cl[k].W * sv.uFn(a.r / (sqrt(a.s2) * tauEff)) / (a.s2 * tauEff * tauEff)
                    for (i in 0 until np) Jw[MAXP * k + i] = psi * a.J[i]
                }
                val s2c = tauEff * tauEff * opt.sigmaDb * opt.sigmaDb * opt.rhoIn
                val B = matP(); for (i in 0 until np) for (j in 0 until np) B[i][j] = A[i][j]
                for (k in 0 until K)
                    for (l in 0 until K) {
                        if (l == k) continue
                        val c = s2c * exp(-hypot(cl[k].x - cl[l].x, cl[k].y - cl[l].y) / opt.shadowCorrM)
                        for (i in 0 until np) for (j in 0 until np) B[i][j] += c * Jw[MAXP * k + i] * Jw[MAXP * l + j]
                    }
                val FB = matP()
                for (i in 0 until np) for (j in 0 until np) { var s = 0.0; for (q in 0 until np) s += Finv[i][q] * B[q][j]; FB[i][j] = s }
                val S = Array(2) { DoubleArray(2) }
                for (i in 0 until 2) for (j in 0 until 2) { var s = 0.0; for (q in 0 until np) s += FB[i][q] * Finv[q][j]; S[i][j] = s }
                C[0] = S[0][0]; C[1] = 0.5 * (S[0][1] + S[1][0]); C[2] = S[1][1]
                // R95 from the local posterior: the marginal likelihood at the fitted scale, tempered by sandwich / Laplace,
                // on a 41 × 41 grid over ±postWindow σ of the major axis; widens C (shape kept) when larger
                if (opt.postWindow > 0) {
                    val sc = cl.map { it.copy() }
                    for (k in 0 until K) {
                        val a = evalAt(cl[k], sol, opt.heightM)
                        sc[k].W = cmax(1e-9, cl[k].W * sv.uFn(a.r / (sqrt(a.s2) * sv.tau)))
                        sc[k].sh2 *= tauEff * tauEff; sc[k].a *= tauEff
                    }
                    val e = eig2(C[0], C[1], C[2])
                    val temper = cmax(1.0, (C[0] + C[2]) / (Finv[0][0] + Finv[1][1]))
                    val ext = cmax(10.0, opt.postWindow * sqrt(cmax(0.0, e[0])))
                    val N = 41
                    val h = 2 * ext / (N - 1)
                    val lv = DoubleArray(N * N); val rr = DoubleArray(N * N)
                    var lmax = -1e300
                    for (j in 0 until N)
                        for (i in 0 until N) {
                            val px = sol.x - ext + i * h; val py = sol.y - ext + j * h
                            lv[j * N + i] = gridLogL(sc, rg, ctx.misses, mxs, mys, px, py, opt, nd, null) / temper
                            lmax = cmax(lmax, lv[j * N + i])
                            rr[j * N + i] = hypot(px - sol.x, py - sol.y)
                        }
                    val idx = (0 until N * N).sortedWith(Comparator { p, q -> if (rr[p] < rr[q]) -1 else if (rr[q] < rr[p]) 1 else p.compareTo(q) })
                    var ps = 0.0; for (v in lv) ps += exp(v - lmax)
                    var acc = 0.0; var r95p = 0.0
                    for (i in idx) { acc += exp(lv[i] - lmax); if (acc >= 0.95 * ps) { r95p = rr[i]; break } }
                    val e0 = ellipse(C[0], C[1], C[2])
                    val r95c = radiusFor(e0[0], e0[1], 0.95)
                    if (r95c > 0 && r95p > r95c) { val q = (r95p / r95c) * (r95p / r95c); C[0] *= q; C[1] *= q; C[2] *= q }
                }
            } else { C[0] = gC[0]; C[1] = gC[1]; C[2] = gC[2] }
            // correlated fix error: shared within a session (the per-place errors-in-variables term counts it too, on purpose)
            run { val am = median(accs) / ACC68; val fl = am * am / max(1, f.sessions); C[0] += fl; C[2] += fl }
            // the Cramér–Rao bound with the nominal σ0: the geometry figure crlbR95, no longer a floor
            run {
                val s0 = Solver(cl, rg, opt, nd)
                val A0 = matP(); val g0 = DoubleArray(MAXP); val I0 = matP()
                s0.normal(sol, 1.0, A0, g0)
                if (invertN(A0, np, I0)) { crlbC[0] = I0[0][0]; crlbC[1] = I0[0][1]; crlbC[2] = I0[1][1] }
            }
            // leave-one-place-out jackknife
            if (K >= 4 && K <= opt.jackMaxK) {
                val px = DoubleArray(K); val py = DoubleArray(K)
                val sj = sv.copy()
                for (k in 0 until K) {
                    sj.mult.fill(1.0); sj.mult[k] = 0.0
                    val t = sj.run(sol, 12, null)
                    px[k] = t.x; py[k] = t.y
                    f.jackMax = cmax(f.jackMax, hypot(t.x - sol.x, t.y - sol.y))
                }
                var mxj = 0.0; var myj = 0.0; for (k in 0 until K) { mxj += px[k]; myj += py[k] }; mxj /= K; myj /= K
                val J = DoubleArray(3)
                for (k in 0 until K) { J[0] += (px[k] - mxj) * (px[k] - mxj); J[1] += (px[k] - mxj) * (py[k] - myj); J[2] += (py[k] - myj) * (py[k] - myj) }
                for (q in 0 until 3) J[q] *= (K - 1).toDouble() / K
                eigMax(C, J)
            }
            // cluster bootstrap
            if (K >= 6 && opt.bootstrap > 1) {
                var st: ULong = opt.seed.toULong() xor (K.toULong() * 0x9E3779B97F4A7C15uL)
                if (st == 0uL) st = 1uL
                fun next(): ULong { st = st xor (st shr 12); st = st xor (st shl 25); st = st xor (st shr 27); return st * 2685821657736338717uL }
                val px = ArrayList<Double>(); val py = ArrayList<Double>()
                val sb = sv.copy()
                for (b in 0 until opt.bootstrap) {
                    sb.mult.fill(0.0)
                    for (q in 0 until K) sb.mult[((next() shr 11) % K.toULong()).toInt()] += 1.0
                    val t = sb.run(sol, 12, null)
                    px.add(t.x); py.add(t.y)
                }
                val B = px.size
                var mxb = 0.0; var myb = 0.0; for (b in 0 until B) { mxb += px[b]; myb += py[b] }; mxb /= B; myb /= B
                val Bc = DoubleArray(3)
                for (b in 0 until B) { Bc[0] += (px[b] - mxb) * (px[b] - mxb); Bc[1] += (px[b] - mxb) * (py[b] - myb); Bc[2] += (py[b] - myb) * (py[b] - myb) }
                for (q in 0 until 3) Bc[q] /= (B - 1)
                eigMax(C, Bc)
            }
        } else {
            gridLogL(cl, rg, ctx.misses, mxs, mys, pmx, pmy, opt, nd, bb)
            sol = Theta(pmx, pmy, cclamp(bb[0], -90.0, 10.0), cclamp(bb[1], 1.5, 4.5))
            C[0] = gC[0]; C[1] = gC[1]; C[2] = gC[2]
        }
        val k2 = opt.kappa * opt.kappa
        C[0] *= k2; C[1] *= k2; C[2] *= k2
        setEllipse(f, C)
        f.lat = fr.lat(sol.y); f.lon = fr.lon(sol.x); f.p0 = sol.p0; f.pathloss = sol.n; f.fittedN = haveLm
        if (crlbC[0] > 0) { val e = ellipse(crlbC[0], crlbC[1], crlbC[2]); f.crlbR95 = radiusFor(e[0], e[1], 0.95) }

        // ── residual statistics ──
        run {
            var sw = 0.0; var swr = 0.0; var chi = 0.0; var kin = 0; var rej = 0
            val lv = ArrayList<Double>(); val ld = ArrayList<Double>()
            for (k in 0 until K) {
                val a = evalAt(cl[k], sol, opt.heightM)
                val z = a.r / (sqrt(a.s2) * (if (haveLm) sv.tau else 1.0))
                if (if (haveLm) sv.uFn(z) < 0.5 else abs(z) > 3.0) { rej += cl[k].m; continue }
                ++kin; sw += cl[k].W; swr += cl[k].W * a.r * a.r; chi += cl[k].W * a.r * a.r / a.s2
                lv.add(cl[k].level); ld.add(ln(a.d))
            }
            f.rejected = rej
            f.outlierFrac = if (f.n > 0) rej.toDouble() / f.n else 0.0
            f.rms = if (sw > 0) sqrt(swr / sw) else 0.0
            f.chi2nu = chi / max(1, kin - 2)
            if (!haveLm) f.sigmaDb = f.rms
            if (lv.size >= 4) f.spearman = pearson(ranks(lv), ranks(ld))
        }
        // bearings AP → places
        run {
            var sc = 0.0; var ss = 0.0; var sw = 0.0; val th = DoubleArray(K)
            for (i in 0 until K) { val c = cl[i]; val a = atan2(c.y - sol.y, c.x - sol.x); th[i] = a; sc += c.W * cos(a); ss += c.W * sin(a); sw += c.W }
            f.rbar = if (sw > 0) sqrt(sc * sc + ss * ss) / sw else 1.0
            th.sort()
            var gap = 0.0
            if (th.size >= 2) { for (i in 1 until th.size) gap = cmax(gap, th[i] - th[i - 1]); gap = cmax(gap, th[0] + 2 * PI - th[th.size - 1]) }
            else gap = 2 * PI
            f.maxGapDeg = gap * 180.0 / PI
            f.inHull = pointInHull(cl, sol.x, sol.y)
            val ds = DoubleArray(K); var dmin = 1e300
            for (i in 0 until K) { val c = cl[i]; val d = hypot(c.x - sol.x, c.y - sol.y); ds[i] = d; dmin = cmin(dmin, d) }
            val dmed = median(ds); f.dminRatio = if (dmed > 0) dmin / dmed else 0.0
        }
        // geometry-only dilution of precision (P0 profiled out) and the P0–range correlation
        if (K >= 3) {
            var vbx = 0.0; var vby = 0.0; var sw = 0.0; val vx = DoubleArray(K); val vy = DoubleArray(K)
            for (k in 0 until K) { val c = cl[k]; val dx = sol.x - c.x; val dy = sol.y - c.y; val d2 = dx * dx + dy * dy + opt.heightM * opt.heightM; vx[k] = dx / d2; vy[k] = dy / d2 }
            for (k in 0 until K) { vbx += cl[k].W * vx[k]; vby += cl[k].W * vy[k]; sw += cl[k].W }
            vbx /= sw; vby /= sw
            var M0 = 0.0; var M1 = 0.0; var M2 = 0.0
            for (k in 0 until K) { val ax = vx[k] - vbx; val ay = vy[k] - vby; M0 += cl[k].W * ax * ax; M1 += cl[k].W * ax * ay; M2 += cl[k].W * ay * ay }
            val inv = DoubleArray(3)
            f.rssDop = if (invert2(M0, M1, M2, inv) && inv[0] + inv[2] > 0) sqrt(inv[0] + inv[2]) else 1e6
            if (haveFinv) {
                var ex = sol.x - gmx; var ey = sol.y - gmy; val nn = hypot(ex, ey)
                if (nn < 1) { ex = e1x; ey = e1y } else { ex /= nn; ey /= nn }
                val vr = ex * ex * Finv[0][0] + 2 * ex * ey * Finv[0][1] + ey * ey * Finv[1][1]
                val cpr = ex * Finv[0][2] + ey * Finv[1][2]
                f.p0RangeCorr = if (vr > 0 && Finv[2][2] > 0) abs(cpr) / sqrt(vr * Finv[2][2]) else 0.0
            }
        }
        if (f.ambiguous) {
            val dx = fr.x(f.altLon) - sol.x; val dy = fr.y(f.altLat) - sol.y; val dd = hypot(dx, dy)
            val s2 = if (dd > 0) (dx * dx * f.cxx + 2 * dx * dy * f.cxy + dy * dy * f.cyy) / (dd * dd) else 0.0
            // inside the places' hull a far alternative is a second mode (counted by the grid), not a mirror
            if (f.inHull) f.ambiguous = false
            else {
                // Either solution may be the AP: the error about the reported one is the mixture's second moment,
                // C + p·d·dᵀ with p = P(alternative) ≈ 1/(1 + e^gap), so R95 reaches the ghost as often as it is the AP.
                // Widened even when the ghost lies within 2σ (within 2σ is not within R95); only the ghost marker needs more.
                val p = 1.0 / (1.0 + exp(altGap))
                C[0] += p * dx * dx; C[1] += p * dx * dy; C[2] += p * dy * dy
                setEllipse(f, C)
                if (dd <= 2 * sqrt(cmax(0.0, s2))) f.ambiguous = false   // too close to show apart
            }
        }

        // ── kind ──
        val isFix = haveLm && f.r95 <= opt.fixMaxR95 && f.crlbR95 > 0 && f.crlbR95 <= opt.fixMaxR95
        if (isFix) f.kind = "fix"
        else {
            // a region: centred on the posterior mean (the best guess under this much uncertainty)
            f.kind = "region"
            if (haveLm) {
                gridLogL(cl, rg, ctx.misses, mxs, mys, pmx, pmy, opt, nd, bb)
                sol = Theta(pmx, pmy, cclamp(bb[0], -90.0, 10.0), cclamp(bb[1], 1.5, 4.5))
                eigMax(C, gC)
                setEllipse(f, C)
                f.lat = fr.lat(sol.y); f.lon = fr.lon(sol.x); f.p0 = sol.p0; f.pathloss = sol.n
            }
            f.r95 = cmax(f.r95, hpdR95)
            f.ambiguous = false
        }
        if (f.r95 > opt.maxR95 || !f.lat.isFinite() || !f.lon.isFinite()) { f.kind = "none"; f.valid = false; grade(f, null); return f }
        f.valid = true
        // A static AP gets quieter with distance; a companion does not (docs/GRADING.md §3.8)
        if (isFix && K >= 5 && maxPair > 300 && f.spearman > -0.1) {
            f.kind = "mobile"; f.valid = false
            grade(f, null)
            return f
        }

        // ── drift, external agreement ──
        if (ctx.hasPrev && ctx.prev.valid && (ctx.prev.kind == "fix" || ctx.prev.kind == "region")) {
            val dx = fr.x(ctx.prev.lon) - sol.x; val dy = fr.y(ctx.prev.lat) - sol.y
            var pc0 = ctx.prev.cxx; var pc1 = ctx.prev.cxy; var pc2 = ctx.prev.cyy
            if (pc0 <= 0 || pc2 <= 0) { pc0 = ctx.prev.acc * ctx.prev.acc; pc2 = pc0; pc1 = 0.0 }
            val inv = DoubleArray(3)
            if (invert2(f.cxx + pc0, f.cxy + pc1, f.cyy + pc2, inv)) {
                val d2 = dx * dx * inv[0] + 2 * dx * dy * inv[1] + dy * dy * inv[2]
                f.driftD2 = 0.5 * ctx.prev.driftD2 + 0.5 * d2
            }
            f.nisEwma = 0.0
        }
        if (ctx.external.has) {
            val dx = fr.x(ctx.external.lon) - sol.x; val dy = fr.y(ctx.external.lat) - sol.y
            val se = cmax(ctx.external.acc, 50.0) / ACC68; val s2 = se * se
            val inv = DoubleArray(3)
            if (invert2(f.cxx + s2, f.cxy, f.cyy + s2, inv)) f.extD2 = dx * dx * inv[0] + 2 * dx * dy * inv[1] + dy * dy * inv[2]
        }

        // ── "sample here next": the spot whose sample adds the most information ──
        run {
            val s0 = Solver(cl, rg, opt, nd)
            val A0 = matP(); val g0 = DoubleArray(MAXP); val I0 = matP()
            s0.normal(sol, 1.0, A0, g0)
            if (invertN(A0, 4 + nd, I0) && (f.kind == "region" || f.r95 > 25)) {
                val b = 10.0 * sol.n / LN10; val aFix = 10.0 / ACC68
                var bestGain = 0.0; var bx = 0.0; var by = 0.0
                for (ri in 0 until 3) for (bi in 0 until 16) {
                    val rad = 30.0 * (ri + 1); val ang = bi * 2 * PI / 16
                    val qx = sol.x + rad * cos(ang); val qy = sol.y + rad * sin(ang)
                    val dx = sol.x - qx; val dy = sol.y - qy; val rho = hypot(dx, dy); val d = sqrt(rho * rho + opt.heightM * opt.heightM)
                    val J = doubleArrayOf(b * dx / (d * d), b * dy / (d * d), -1.0, 10.0 * log10(d))
                    var q = 0.0; for (i in 0 until 4) for (j in 0 until 4) q += J[i] * I0[i][j] * J[j]
                    val grad = b * rho / (d * d); val s2 = opt.sigmaDb * opt.sigmaDb + grad * grad * aFix * aFix
                    val mu = sol.p0 - 10.0 * sol.n * log10(d)
                    val gain = ln(1 + q / s2) * normCdf((mu - opt.sensitivity) / opt.sigmaDb)
                    if (gain > bestGain + 1e-12) { bestGain = gain; bx = qx; by = qy }
                }
                if (bestGain > 0.01) { f.suggestLat = fr.lat(by); f.suggestLon = fr.lon(bx); f.suggestGain = bestGain }
            }
        }

        // ── score ──
        f.cP = precisionComp(f.r95)
        f.cG = (0.6 * (1 - f.rbar) + 0.4 * cmin(1.0, 4 * f.linRatio)) * (if (f.inHull) 1.0 else 0.7)
        f.cE = 1 - exp(-f.ess / 4)
        f.cFfit = exp(-cmax(0.0, f.chi2nu - 1.5) / 2) * (1 - clamp01((f.outlierFrac - 0.10) / 0.40))
        f.cF = f.cFfit * nisFactor(f.nisEwma)
        f.cS = exp(-f.driftD2 / 6) * exp(-(f.jackMax / cmax(1.0, f.semiMajor)).pow(2) / 8) * (if (f.ambiguous) 0.4 else 1.0)
        f.cT = freshness(f.newest, now)
        f.cX = if (f.extD2 >= 0) cmax(0.3, exp(-f.extD2 / 8)) else -1.0
        grade(f, if (ctx.hasPrev) ctx.prev else null)
        return f
    }

    // ── incremental update (between batched refits) ──────────────────────────
    /**
     * One new sample says "the AP is d metres from here" (d from the AP's own P0/n): a 2-D Kalman step with H = the unit
     * vector observer → fit, and the full covariance kept. [deviceOffsetDb]: how much louder o.device hears than this
     * device (Context.deviceOffset), removed from o.dbm as [fitAp] does.
     */
    fun update(prev: Fit, o: Obs, opt: Options = Options(), deviceOffsetDb: Double = 0.0): Fit {
        if (!prev.valid || (prev.kind != "fix" && prev.kind != "region")) return prev
        val f = prev.copy()
        val fr = Frame(prev.lat, prev.lon)
        val ox = fr.x(o.lon); val oy = fr.y(o.lat)
        val r = cmax(1.0, hypot(ox, oy))
        val d3 = 10.0.pow((prev.p0 - (o.dbm.toDouble() - deviceOffsetDb)) / (10.0 * prev.pathloss))   // modelDistance of the level as this device would hear it
        val dm = sqrt(cmax(1.0, d3 * d3 - opt.heightM * opt.heightM))
        val ux = -ox / r; val uy = -oy / r
        val sigD = d3 * LN10 * opt.sigmaDb / (10.0 * prev.pathloss)
        val a = o.acc / ACC68
        val R = sigD * sigD + a * a
        var P0 = prev.cxx; var P1 = prev.cxy; var P2 = prev.cyy
        if (P0 <= 0 || P2 <= 0) { P0 = prev.acc * prev.acc; P2 = P0; P1 = 0.0 }
        val Pux = P0 * ux + P1 * uy; val Puy = P1 * ux + P2 * uy
        val S = ux * Pux + uy * Puy + R
        val innov = dm - r
        val nis = innov * innov / S
        f.nisEwma = if (prev.nisEwma > 0) 0.8 * prev.nisEwma + 0.2 * nis else nis
        val C = doubleArrayOf(P0, P1, P2)
        if (nis <= 9.0) {
            val Kx = Pux / S; val Ky = Puy / S
            f.lat = fr.lat(Ky * innov); f.lon = fr.lon(Kx * innov)
            C[0] = P0 - Pux * Pux / S; C[1] = P1 - Pux * Puy / S; C[2] = P2 - Puy * Puy / S
        }
        setEllipse(f, C)
        if (f.kind == "region") f.r95 = cmax(f.r95, radiusFor(f.semiMajor, f.semiMinor, 0.95))
        f.n = prev.n + 1
        f.updated = maxOf(prev.updated, o.t)
        f.newest = maxOf(prev.newest, o.t)
        f.cP = precisionComp(f.r95)
        f.cF = f.cFfit * nisFactor(f.nisEwma)
        f.cT = freshness(f.newest, if (o.t > 0) o.t else prev.updated)
        grade(f, prev)
        return f
    }

    // ── self-location: where are WE, from beacons with known positions ───────
    private class S(val x: Double, val y: Double, val w: Double, val d: Double, val sigD: Double,
                    val c0: Double, val c1: Double, val c2: Double, val acc: Double, var on: Boolean)

    private fun sig2(q: S, ux: Double, uy: Double): Double {
        val apVar = if (q.c0 > 0 && q.c2 > 0) ux * ux * q.c0 + 2 * ux * uy * q.c1 + uy * uy * q.c2 else q.acc * q.acc
        return q.sigD * q.sigD + apVar
    }

    /** Self-location from beacons with known positions (with an integrity check). */
    fun selfLocate(known: List<Known>, opt: Options = Options()): SelfFix {
        val out = SelfFix()
        if (known.size < 2) return out
        val fr = Frame(known[0].lat, known[0].lon)
        val s = ArrayList<S>()
        for (k in known) {
            val p0 = if (k.haveModel) k.p0 else -40.0; val n = if (k.haveModel) k.pathloss else opt.defaultN
            val d = cclamp(modelDistance(p0, n, k.dbm), 1.0, 1500.0)
            val sigD = d * LN10 * opt.sigmaDb / (10.0 * n)
            s.add(S(fr.x(k.lon), fr.y(k.lat), cmax(0.05, k.weight), d, sigD, k.cxx, k.cxy, k.cyy, k.acc, true))
        }
        var x = 0.0; var y = 0.0
        run {
            var sw = 0.0
            for (i in s.indices) { val cw = 10.0.pow(known[i].dbm / 20.0) / cmax(10.0, s[i].acc); sw += cw; x += cw * s[i].x; y += cw * s[i].y }
            x /= sw; y /= sw
        }
        val x0 = x; val y0 = y
        fun costAt(cx: Double, cy: Double): Double {
            var c = 0.0
            for (q in s) {
                if (!q.on) continue
                val dx = cx - q.x; val dy = cy - q.y; val r = cmax(1.0, hypot(dx, dy))
                val v = sig2(q, dx / r, dy / r); val res = r - q.d; val h = huberWeight(res / sqrt(v), 2.0)
                c += q.w * h * res * res / v
            }
            return c
        }
        /** Returns null when fewer than 3 are active (the caller keeps its position). */
        fun solveActive(): DoubleArray? {
            var X = x0; var Y = y0; var lambda = 1e-2
            var active = 0; for (q in s) if (q.on) ++active
            if (active < 3) return null
            for (it in 0 until opt.maxIter) {
                val A = mat4(); val b = DoubleArray(4)
                for (q in s) {
                    if (!q.on) continue
                    val dx = X - q.x; val dy = Y - q.y; val r = cmax(1.0, hypot(dx, dy))
                    val jx = dx / r; val jy = dy / r; val v = sig2(q, jx, jy)
                    val res = r - q.d; val h = huberWeight(res / sqrt(v), 2.0); val w = q.w * h / v
                    A[0][0] += w * jx * jx; A[0][1] += w * jx * jy; A[1][0] += w * jx * jy; A[1][1] += w * jy * jy
                    b[0] -= w * jx * res; b[1] -= w * jy * res
                }
                val cost = costAt(X, Y)
                A[0][0] *= 1 + lambda; A[1][1] *= 1 + lambda
                val dx = DoubleArray(4); if (!solve(A, b, 2, dx)) break
                val nx = X + dx[0]; val ny = Y + dx[1]; val nc = costAt(nx, ny)
                if (nc <= cost) { X = nx; Y = ny; lambda = cmax(1e-6, lambda / 3); if (hypot(dx[0], dx[1]) < 0.2) break }
                else { lambda *= 8; if (lambda > 1e6) break }
            }
            return doubleArrayOf(X, Y)
        }
        if (known.size >= 3) solveActive()?.let { x = it[0]; y = it[1] }
        // Integrity (RAIM-like): a χ² test on the normalised range residuals; exclude the worst, retry
        var integrity = if (known.size >= 4) "ok" else "unverified"
        if (known.size >= 4) {
            for (round in 0 until 3) {
                var chi = 0.0; var act = 0; var worst = -1; var worstZ = 0.0
                for (i in s.indices) {
                    if (!s[i].on) continue
                    val dx = x - s[i].x; val dy = y - s[i].y; val r = cmax(1.0, hypot(dx, dy))
                    val z = (r - s[i].d) / sqrt(sig2(s[i], dx / r, dy / r))
                    chi += z * z; ++act
                    if (abs(z) > worstZ) { worstZ = abs(z); worst = i }
                }
                if (act < 4 || chi <= chi2Quantile99(act - 2)) { if (round > 0) integrity = if (act >= 4) "repaired" else "unverified"; break }
                if (round == 2 || act - 1 < 3) { integrity = "failed"; break }
                s[worst].on = false; ++out.excluded
                solveActive()?.let { x = it[0]; y = it[1] }
            }
        }
        val A = DoubleArray(3); var wsum = 0.0; var wres = 0.0; var rejected = 0; var used = 0
        for (q in s) {
            if (!q.on) continue
            val dx = x - q.x; val dy = y - q.y; val r = cmax(1.0, hypot(dx, dy))
            val jx = dx / r; val jy = dy / r; val v = sig2(q, jx, jy)
            val res = r - q.d; val h = huberWeight(res / sqrt(v), 2.0); val w = q.w * h / v
            if (h < 0.5) ++rejected
            ++used
            A[0] += w * jx * jx; A[1] += w * jx * jy; A[2] += w * jy * jy
            wsum += w; wres += w * res * res
        }
        val chiNorm = wres / max(1, used - 2)                 // reduced χ² of the normalised ranges
        val spread = sqrt(wres / cmax(1e-12, wsum))           // weighted RMS range residual (m)
        var acc: Double; var r95: Double
        val inv = DoubleArray(3)
        if (used >= 3 && invert2(A[0], A[1], A[2], inv)) {
            val scale = cmax(1.0, chiNorm)
            val e = ellipse(inv[0] * scale, inv[1] * scale, inv[2] * scale)
            acc = e[0]; r95 = radiusFor(e[0], e[1], 0.95)
        } else { acc = 0.0; for (q in s) if (q.on) acc = cmax(acc, sqrt(q.sigD * q.sigD + q.acc * q.acc)); r95 = 2.45 * acc }
        out.valid = true; out.lat = fr.lat(y); out.lon = fr.lon(x)
        out.acc = cmax(15.0, cmax(acc, spread * 0.7))
        out.r95 = cmax(out.acc * 2.45, r95)
        out.rms = spread
        out.used = used - rejected; out.rejected = rejected + out.excluded
        out.integrity = integrity
        return out
    }
}
