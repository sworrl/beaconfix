// BeaconFix ranging maths — the Kotlin twin of src/ranging/rangemath.{h,cpp} and src/ranging/anchors.{h,cpp}.
// Pure Kotlin + the JDK (no Android imports). Same operations in the same order as the C++, so both
// reproduce the vectors in docs/RANGING.md §11 to 1e-9 relative. Owned by the ranging worker; do not
// edit here — change the C++ and this file together and regenerate the vectors.
package org.sworrl.beaconfix.ranging

import java.security.MessageDigest
import kotlin.math.abs
import kotlin.math.asin
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

data class Level(val valid: Boolean, val dbm: Double, val sigma: Double, val nEff: Double, val n: Int)
data class CalPoint(val distM: Double, val level: Double, val weight: Double = 1.0)
data class PathLossFit(val p0: Double, val n: Double, val varP0: Double, val varN: Double, val covP0N: Double, val rmsDb: Double, val used: Int)
data class DiffPair(val group: String, val levelA: Double, val sigmaA: Double = RangeMath.DB_PER_NEPER, val levelB: Double, val sigmaB: Double = RangeMath.DB_PER_NEPER)
data class Fingerprint(val ok: Boolean = false, val groups: Int = 0, val gainDb: Double = 0.0, val d2: Double = 0.0, val noise2: Double = 0.0,
                       val excess: Double = 0.0, val deltaM: Double = 0.0, val lowM: Double = 0.0, val highM: Double = 0.0)
data class GeoPair(val group: String, val apE: Double, val apN: Double, val pathloss: Double = RangeMath.N_WIFI, val sigmaPos: Double = 10.0,
                   val delta: Double, val sigmaDelta: Double = 6.1)
class GeoSolve(val ok: Boolean, val used: Int, val iterations: Int, val dE: Double, val dN: Double, val gainDb: Double, val chi2: Double,
               val spreadDeg: Double, val cov: Array<DoubleArray>)
data class Gauss2(val valid: Boolean = false, val muE: Double = 0.0, val muN: Double = 0.0, val sEE: Double = 0.0, val sEN: Double = 0.0, val sNN: Double = 0.0)
data class RelInput(val haveRange: Boolean = false, val u: Double = 0.0, val puu: Double = 0.0, val haveFp: Boolean = false,
                    val fp: Fingerprint = Fingerprint(), val geo: Gauss2 = Gauss2(), val fix: Gauss2 = Gauss2())
data class RelOutput(val valid: Boolean, val distanceM: Double, val lowM: Double, val highM: Double, val sigmaM: Double, val haveBearing: Boolean,
                     val bearingDeg: Double, val bearingSigmaDeg: Double, val resultantLength: Double, val cls: String)
class BleAdvert(val tag: ByteArray, val txPower: Int, val rtt: Boolean, val api: Boolean, val kind: Int, val calibrating: Boolean, val version: Int)
data class Enu(val e: Double, val n: Double, val u: Double)
data class LatLonAlt(val lat: Double, val lon: Double, val alt: Double?)
data class Reprojected(val lat: Double, val lon: Double, val alt: Double?, val headingDeg: Double?, val headingAssumed: Boolean)
data class AnchorRange(val lat: Double, val lon: Double, val distM: Double, val sigmaM: Double, val accM: Double)
data class Trilat(val ok: Boolean, val lat: Double, val lon: Double, val sigma: Double, val rmsM: Double = 0.0, val used: Int = 0, val iterations: Int = 0)

object RangeMath {
    const val LN10 = 2.302585092994046
    const val DB_PER_NEPER = 4.342944819032518
    const val RAYLEIGH_DB_STD = 5.570043140052503
    const val SHADOW_WIFI = 5.0
    const val SHADOW_BLE = 6.0
    const val DEV_GAIN = 1.5
    const val DECORR_M = 3.0
    const val N_WIFI = 2.4
    const val N_BLE = 2.0
    const val SIGMA_NLOS = 1.0
    const val RTT_FLOOR = 0.3
    const val Z84 = 0.9944578832097535
    const val FROZEN_FADE_VAR = RAYLEIGH_DB_STD * RAYLEIGH_DB_STD / 3.0
    const val OFFSET_VAR0 = 36.0 + SHADOW_BLE * SHADOW_BLE + FROZEN_FADE_VAR
    const val BLE_SERVICE_UUID = "28c9f0bf-a089-4a95-b632-5e8ede1b03b6"
    const val KIND_DESKTOP = 0; const val KIND_ANDROID = 1; const val KIND_LAPTOP = 2; const val KIND_PI = 3; const val KIND_GNSS = 4; const val KIND_OTHER = 7

    fun fsplDb(dM: Double, fMHz: Double): Double = 20.0 * log10(dM) + 20.0 * log10(fMHz) - 27.55
    fun priorP0Ble(txPower: Int): Double = if (txPower == 127) -59.0 else txPower.toDouble() - 41.0
    fun priorP0Wifi(fMHz: Double): Double = -40.0 - 20.0 * log10(fMHz / 2437.0)
    fun modelLevel(p0: Double, n: Double, dM: Double): Double = p0 - 10.0 * n * log10(max(dM, 0.05))
    fun modelDistance(p0: Double, n: Double, level: Double): Double = 10.0.pow((p0 - level) / (10.0 * n))

    fun quantile7(v: List<Double>, p: Double): Double {
        if (v.isEmpty()) return Double.NaN
        val s = v.sorted()
        val n = s.size
        if (n == 1) return s[0]
        val h = (n - 1) * p
        val lo = floor(h).toInt()
        val hi = min(lo + 1, n - 1)
        return s[lo] + (h - lo) * (s[hi] - s[lo])
    }
    fun median(v: List<Double>): Double = quantile7(v, 0.5)
    fun chi2Quantile(m: Double, z: Double): Double {
        val a = 2.0 / (9.0 * m)
        val b = 1.0 - a + z * sqrt(a)
        return if (b <= 0) 0.0 else m * b * b * b
    }

    fun levelFromSamples(rssi: List<Double>, channels: Int, spanS: Double, moving: Boolean): Level {
        val n = rssi.size
        if (n == 0) return Level(false, 0.0, 0.0, 0.0, 0)
        val x = DoubleArray(n) { 10.0.pow(rssi[it] / 10.0) }
        var t = Double.POSITIVE_INFINITY
        var c = 1.0
        if (n >= 8) { t = quantile7(x.toList(), 0.95); c = 0.95 }
        var sum = 0.0
        for (i in 0 until n) sum += min(x[i], t)
        val dbm = 10.0 * log10((sum / n) / c)
        val tau = if (moving) 0.2 else 30.0
        val nEff = min(n.toDouble(), max(1, channels).toDouble() * (1.0 + max(0.0, spanS) / tau))
        return Level(true, dbm, DB_PER_NEPER / sqrt(nEff), nEff, n)
    }

    fun fitPathLoss(pts: List<CalPoint>, p0Prior: Double, nPrior: Double, varP0: Double = 64.0, varN: Double = 0.25, noiseDb: Double = 4.0): PathLossFit {
        var a00 = 1.0 / varP0; var a01 = 0.0; var a11 = 1.0 / varN
        var b0 = p0Prior / varP0; var b1 = nPrior / varN
        val wn = 1.0 / (noiseDb * noiseDb)
        for (p in pts) {
            val x = log10(max(p.distM, 0.05))
            val h1 = -10.0 * x; val w = p.weight * wn
            a00 += w; a01 += w * h1; a11 += w * h1 * h1
            b0 += w * p.level; b1 += w * h1 * p.level
        }
        val det = a00 * a11 - a01 * a01
        val varP = a11 / det; val varNn = a00 / det; val cov = -a01 / det
        val p0 = varP * b0 + cov * b1
        var n = cov * b0 + varNn * b1
        n = max(1.5, min(4.5, n))
        var sw = 0.0; var sr = 0.0
        for (p in pts) { val r = p.level - modelLevel(p0, n, p.distM); sw += p.weight; sr += p.weight * r * r }
        return PathLossFit(p0, n, varP, varNn, cov, if (sw > 0) sqrt(sr / sw) else 0.0, pts.size)
    }

    fun fingerprintExcessModel(deltaM: Double): Double {
        val g = 10.0 * N_WIFI / (LN10 * 15.0 * sqrt(2.0))
        return 2.0 * SHADOW_WIFI * SHADOW_WIFI * (1.0 - exp(-deltaM / DECORR_M)) + g * g * deltaM * deltaM
    }
    fun fingerprintInvert(target: Double): Double {
        if (!(target > 0)) return 0.0
        if (fingerprintExcessModel(200.0) < target) return 200.0
        var lo = 0.0; var hi = 200.0
        repeat(60) { val mid = 0.5 * (lo + hi); if (fingerprintExcessModel(mid) < target) lo = mid else hi = mid }
        return 0.5 * (lo + hi)
    }
    fun fingerprintDistance(pairs: List<DiffPair>): Fingerprint {
        val agg = java.util.TreeMap<String, DoubleArray>()
        for (p in pairs) {
            val w = 1.0 / (p.sigmaA * p.sigmaA + p.sigmaB * p.sigmaB)
            val a = agg.getOrPut(p.group) { DoubleArray(2) }
            a[0] += w; a[1] += w * (p.levelB - p.levelA)
        }
        val m = agg.size
        if (m < 3) return Fingerprint(groups = m)
        val d = ArrayList<Double>(); val v = ArrayList<Double>()
        for (a in agg.values) { d.add(a[1] / a[0]); v.add(1.0 / a[0]) }
        val gain = median(d)
        val e = DoubleArray(m) { d[it] - gain }
        val ae = List(m) { abs(e[it]) }
        val d2 = if (m >= 5) { val mad = 1.482602218505602 * median(ae); mad * mad } else { var s = 0.0; for (i in 0 until m) s += e[i] * e[i]; s / m }
        var nz = 0.0
        for (i in 0 until m) nz += v[i] + DEV_GAIN * DEV_GAIN
        val noise2 = nz / m
        val excess = max(0.0, d2 - noise2)
        val d2lo = d2 * m / chi2Quantile(m.toDouble(), Z84)
        val q16 = chi2Quantile(m.toDouble(), -Z84)
        val d2hi = if (q16 > 0) d2 * m / q16 else 1e12
        return Fingerprint(true, m, gain, d2, noise2, excess, fingerprintInvert(excess),
            fingerprintInvert(max(0.0, d2lo - noise2)), fingerprintInvert(max(0.0, d2hi - noise2)))
    }
    fun fingerprintLogLik(fp: Fingerprint, r: Double): Double {
        val s = fingerprintExcessModel(r) + fp.noise2
        val m = fp.groups.toDouble()
        return -(m / 2.0) * ln(s) - m * fp.d2 / (2.0 * s)
    }

    private fun inv3(a: Array<DoubleArray>): Array<DoubleArray>? {
        val c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1]
        val c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2]
        val c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0]
        val det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02
        if (!(abs(det) > 1e-300)) return null
        return arrayOf(
            doubleArrayOf(c00 / det, (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det, (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det),
            doubleArrayOf(c01 / det, (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det, (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det),
            doubleArrayOf(c02 / det, (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det, (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det))
    }

    fun solveDifferential(pairs: List<GeoPair>): GeoSolve {
        val use = pairs.filter { sqrt(it.apE * it.apE + it.apN * it.apN) >= 1.0 }
        val m = use.size
        val br = use.map { var b = atan2(it.apE, it.apN) * 180.0 / Math.PI; if (b < 0) b += 360.0; b }.sorted()
        var maxGap = 360.0
        if (m >= 2) { maxGap = br[0] + 360.0 - br[m - 1]; for (i in 1 until m) maxGap = max(maxGap, br[i] - br[i - 1]) }
        val spread = if (m >= 2) 360.0 - maxGap else 0.0
        val zero = arrayOf(DoubleArray(3), DoubleArray(3), DoubleArray(3))
        if (m < 3 || spread < 60.0) return GeoSolve(false, m, 0, 0.0, 0.0, 0.0, 0.0, spread, zero)
        val th = doubleArrayOf(0.0, 0.0, median(use.map { it.delta }))
        val nm = arrayOf(DoubleArray(3), DoubleArray(3), DoubleArray(3))
        val gv = DoubleArray(3)
        var chi2 = 0.0
        fun accumulate() {
            for (i in 0 until 3) { gv[i] = 0.0; for (j in 0 until 3) nm[i][j] = 0.0 }
            chi2 = 0.0
            val dist = sqrt(th[0] * th[0] + th[1] * th[1])
            for (p in use) {
                val dx = th[0] - p.apE; val dy = th[1] - p.apN
                val dB = max(0.5, sqrt(dx * dx + dy * dy))
                val dA = sqrt(p.apE * p.apE + p.apN * p.apN)
                val pred = -10.0 * p.pathloss * log10(dB / dA) + th[2]
                val r = p.delta - pred
                val k = -10.0 * p.pathloss / LN10
                val jv = doubleArrayOf(k * dx / (dB * dB), k * dy / (dB * dB), 1.0)
                val grad = 10.0 * p.pathloss / (LN10 * dB)
                val variance = p.sigmaDelta * p.sigmaDelta + DEV_GAIN * DEV_GAIN +
                    2.0 * SHADOW_WIFI * SHADOW_WIFI * (1.0 - exp(-dist / DECORR_M)) + (grad * p.sigmaPos) * (grad * p.sigmaPos)
                val w = 1.0 / variance
                for (i in 0 until 3) { gv[i] += w * jv[i] * r; for (j in 0 until 3) nm[i][j] += w * jv[i] * jv[j] }
                chi2 += w * r * r
            }
        }
        var iterations = 0
        for (it in 0 until 20) {
            accumulate()
            val dmp = Array(3) { i -> DoubleArray(3) { j -> nm[i][j] + (if (i == j) 1e-3 * nm[i][i] else 0.0) } }
            val di = inv3(dmp) ?: return GeoSolve(false, m, iterations, 0.0, 0.0, 0.0, 0.0, spread, zero)
            val step = DoubleArray(3) { i -> di[i][0] * gv[0] + di[i][1] * gv[1] + di[i][2] * gv[2] }
            for (i in 0 until 3) th[i] += step[i]
            iterations = it + 1
            if (abs(step[0]) < 1e-6 && abs(step[1]) < 1e-6 && abs(step[2]) < 1e-6) break
        }
        accumulate()
        val ni = inv3(Array(3) { i -> nm[i].copyOf() }) ?: return GeoSolve(false, m, iterations, 0.0, 0.0, 0.0, 0.0, spread, zero)
        val scale = if (m > 3) max(1.0, chi2 / (m - 3)) else 1.0
        val cov = Array(3) { i -> DoubleArray(3) { j -> ni[i][j] * scale } }
        return GeoSolve(true, m, iterations, th[0], th[1], th[2], chi2, spread, cov)
    }

    fun classify(evidence: Boolean, lowM: Double, highM: Double): String = when {
        !evidence -> "unknown"
        highM <= 2.0 -> "adjacent"
        highM <= 6.0 -> "room"
        lowM > 30.0 -> "far"
        highM <= 30.0 -> "near"
        else -> "unknown"
    }

    private fun gaussLog(g: Gauss2, e: Double, n: Double): Double {
        val det = g.sEE * g.sNN - g.sEN * g.sEN
        if (!(det > 0)) return 0.0
        val de = e - g.muE; val dn = n - g.muN
        val q = (g.sNN * de * de - 2.0 * g.sEN * de * dn + g.sEE * dn * dn) / det
        return -0.5 * q
    }

    fun relativePosterior(input: RelInput): RelOutput {
        val fpOk = input.haveFp && input.fp.ok
        val evidence = input.haveRange || fpOk || input.fix.valid || input.geo.valid
        if (!evidence) return RelOutput(false, 0.0, 0.0, 0.0, 0.0, false, 0.0, 0.0, 0.0, "unknown")
        val nr = 64; val nb = 180
        var uLo: Double; var uHi: Double
        if (input.haveRange) {
            val sd = sqrt(input.puu)
            uLo = input.u - 4.0 * sd; uHi = input.u + 4.0 * sd
        } else {
            var rmax = 200.0
            if (input.fix.valid) rmax = max(rmax, sqrt(input.fix.muE * input.fix.muE + input.fix.muN * input.fix.muN) + 4.0 * sqrt(max(input.fix.sEE, input.fix.sNN)))
            if (input.geo.valid) rmax = max(rmax, sqrt(input.geo.muE * input.geo.muE + input.geo.muN * input.geo.muN) + 4.0 * sqrt(max(input.geo.sEE, input.geo.sNN)))
            uLo = log10(0.05); uHi = log10(rmax)
        }
        uLo = max(uLo, -2.0); uHi = min(uHi, log10(20000.0))
        if (!(uHi > uLo)) uHi = uLo + 1e-3
        val du = (uHi - uLo) / (nr - 1)
        val logw = DoubleArray(nr * nb)
        var maxLog = Double.NEGATIVE_INFINITY
        for (i in 0 until nr) {
            val ui = uLo + i * du; val r = 10.0.pow(ui)
            var base = if (input.haveRange) -0.5 * (ui - input.u) * (ui - input.u) / input.puu else 2.0 * ln(r)
            if (fpOk) base += fingerprintLogLik(input.fp, r)
            for (k in 0 until nb) {
                val th = 2.0 * k * Math.PI / 180.0
                val e = r * sin(th); val n = r * cos(th)
                var lw = base
                if (input.geo.valid) lw += gaussLog(input.geo, e, n)
                if (input.fix.valid) lw += gaussLog(input.fix, e, n)
                logw[i * nb + k] = lw
                if (lw > maxLog) maxLog = lw
            }
        }
        val wr = DoubleArray(nr)
        var wSum = 0.0; var c = 0.0; var s = 0.0
        for (i in 0 until nr) {
            var wi = 0.0
            for (k in 0 until nb) {
                val w = exp(logw[i * nb + k] - maxLog)
                val th = 2.0 * k * Math.PI / 180.0
                wi += w; c += w * cos(th); s += w * sin(th)
            }
            wr[i] = wi; wSum += wi
        }
        fun uq(p: Double): Double {
            val target = p * wSum
            var cum = 0.0
            for (i in 0 until nr) {
                if (cum + wr[i] >= target && wr[i] > 0) {
                    val frac = (target - cum) / wr[i]
                    return (uLo + i * du - 0.5 * du) + frac * du
                }
                cum += wr[i]
            }
            return uHi + 0.5 * du
        }
        val dist = 10.0.pow(uq(0.5)); val low = 10.0.pow(uq(0.16)); val high = 10.0.pow(uq(0.84))
        val rl = sqrt(c * c + s * s) / wSum
        var haveBearing = false; var bearing = 0.0; var bearingSigma = 0.0
        if (rl >= 0.5) {
            var b = atan2(s, c) * 180.0 / Math.PI
            if (b < 0) b += 360.0
            haveBearing = true; bearing = b; bearingSigma = sqrt(-2.0 * ln(rl)) * 180.0 / Math.PI
        }
        return RelOutput(true, dist, low, high, 0.5 * (high - low), haveBearing, bearing, bearingSigma, rl, classify(true, low, high))
    }

    // ── §9.1 BLE advertisement ───────────────────────────────────────────────
    fun bleTag(identityId: String, unixSeconds: Long): ByteArray {
        val w = Math.floorDiv(unixSeconds, 900L)
        val msg = "beaconfix-ble-v1|$identityId|$w".toByteArray(Charsets.UTF_8)
        return MessageDigest.getInstance("SHA-256").digest(msg).copyOf(8)
    }
    fun bleFlags(rtt: Boolean, api: Boolean, kind: Int, calibrating: Boolean): Int =
        (if (rtt) 1 else 0) or (if (api) 2 else 0) or ((kind and 7) shl 2) or (if (calibrating) 0x20 else 0)
    fun bleServiceData(tag: ByteArray, txPower: Int, flags: Int): ByteArray {
        val d = ByteArray(10)
        for (i in 0 until min(8, tag.size)) d[i] = tag[i]
        d[8] = max(-128, min(127, txPower)).toByte()
        d[9] = (flags and 0xFF).toByte()
        return d
    }
    /** null when too short or of an unknown version. */
    fun parseServiceData(data: ByteArray): BleAdvert? {
        if (data.size < 10) return null
        val f = data[9].toInt() and 0xFF
        val version = (f shr 6) and 3
        if (version != 0) return null
        return BleAdvert(data.copyOf(8), data[8].toInt(), f and 1 != 0, f and 2 != 0, (f shr 2) and 7, f and 0x20 != 0, version)
    }
}

/** §5.2: x = [u = log10 d, b_1 … b_M, e_c]. e_c is a Schmidt "consider" state (never updated, only its variance counts). */
class RangeFilter(offsets: Int = 2, u0: Double = 1.0, puu0: Double = 1.0, offsetVar0: Double = RangeMath.OFFSET_VAR0, rttOffsetVar0: Double = 0.25) {
    val nx: Int = 2 + max(0, min(offsets, K_MAX - 2))
    val x = DoubleArray(K_MAX)
    val p = Array(K_MAX) { DoubleArray(K_MAX) }
    var offsetVarCap: Double = offsetVar0

    init {
        x[0] = u0
        p[0][0] = puu0
        for (k in 1 until nx - 1) p[k][k] = offsetVar0
        p[nx - 1][nx - 1] = rttOffsetVar0
    }

    val u: Double get() = x[0]
    val puu: Double get() = p[0][0]
    fun offset(k: Int): Double = x[1 + k]
    fun offsetVar(k: Int): Double = p[1 + k][1 + k]
    val rttOffsetError: Double get() = x[nx - 1]
    val rttOffsetVar: Double get() = p[nx - 1][nx - 1]
    val distanceM: Double get() = 10.0.pow(x[0])
    val sigmaM: Double get() = RangeMath.LN10 * 10.0.pow(x[0]) * sqrt(p[0][0])
    val lowM: Double get() = 10.0.pow(x[0] - sqrt(p[0][0]))
    val highM: Double get() = 10.0.pow(x[0] + sqrt(p[0][0]))

    fun predict(dtS: Double, moving: Boolean) {
        if (!(dtS > 0)) return
        p[0][0] += (if (moving) 0.0025 else 4e-6) * dtS
        for (k in 1 until nx - 1) p[k][k] = if (moving) min(p[k][k] + RangeMath.FROZEN_FADE_VAR, max(p[k][k], offsetVarCap)) else p[k][k] + 1e-4 * dtS
        p[nx - 1][nx - 1] += 1e-6 * dtS
    }

    private fun joseph(k: DoubleArray, h: DoubleArray, r: Double) {
        val a = Array(nx) { i -> DoubleArray(nx) { j -> (if (i == j) 1.0 else 0.0) - k[i] * h[j] } }
        val t = Array(nx) { DoubleArray(nx) }
        for (i in 0 until nx) for (j in 0 until nx) { var s = 0.0; for (m in 0 until nx) s += a[i][m] * p[m][j]; t[i][j] = s }
        val q = Array(nx) { DoubleArray(nx) }
        for (i in 0 until nx) for (j in 0 until nx) { var s = 0.0; for (m in 0 until nx) s += t[i][m] * a[j][m]; q[i][j] = s + k[i] * r * k[j] }
        for (i in 0 until nx) for (j in 0 until nx) p[i][j] = 0.5 * (q[i][j] + q[j][i])
    }

    private fun scalarUpdate(h: DoubleArray, y: Double, r: Double, huberK: Double): Double {
        val pht = DoubleArray(nx)
        for (i in 0 until nx) { var s = 0.0; for (j in 0 until nx) s += p[i][j] * h[j]; pht[i] = s }
        var hpht = 0.0
        for (i in 0 until nx) hpht += h[i] * pht[i]
        var sv = hpht + r
        val z = y / sqrt(sv)
        var reff = r
        if (abs(z) > huberK) { reff = r * (abs(z) / huberK); sv = hpht + reff }
        val k = DoubleArray(nx) { pht[it] / sv }
        k[nx - 1] = 0.0
        for (i in 0 until nx) x[i] += k[i] * y
        joseph(k, h, reff)
        x[0] = max(-2.0, min(4.0, x[0]))
        return z
    }

    fun updateRssi(k: Int, level: Double, sigmaL: Double, p0: Double, n: Double): Double {
        if (k < 0 || 1 + k >= nx - 1) return 0.0
        val h = DoubleArray(nx)
        h[0] = -10.0 * n
        h[1 + k] = 1.0
        val pred = p0 - 10.0 * n * x[0] + x[1 + k]
        return scalarUpdate(h, level - pred, sigmaL * sigmaL, 2.5)
    }

    fun updateRtt(rangeM: Double, sigmaM: Double, offsetM: Double): Double {
        val s = max(sigmaM, RangeMath.RTT_FLOOR)
        val c = nx - 1
        val x0 = x.copyOf(); val xi = x.copyOf()
        val p0 = Array(nx) { p[it].copyOf() }
        val h = DoubleArray(nx); val k = DoubleArray(nx)
        var r = s * s; var z = 0.0
        for (it in 0 until 20) {
            val uPrev = xi[0]; val cPrev = xi[c]
            val d = 10.0.pow(xi[0])
            val resid = rangeM - (d + offsetM + xi[c])
            h[0] = RangeMath.LN10 * d
            h[c] = 1.0
            r = s * s
            val y = resid - h[0] * (x0[0] - xi[0]) - h[c] * (x0[c] - xi[c])
            val pht = DoubleArray(nx) { i -> p0[i][0] * h[0] + p0[i][c] * h[c] }
            val hpht = h[0] * pht[0] + h[c] * pht[c]
            var sv = hpht + r
            z = resid / sqrt(sv)
            if (z > 2.0) {
                r = (s * s + RangeMath.SIGMA_NLOS * RangeMath.SIGMA_NLOS) * (z / 2.0) * (z / 2.0)
                sv = hpht + r
            } else if (z < -3.0) {
                r = r * (-z / 3.0)
                sv = hpht + r
            }
            for (i in 0 until nx) k[i] = pht[i] / sv
            k[c] = 0.0
            for (i in 0 until nx) xi[i] = x0[i] + k[i] * y
            xi[0] = max(-2.0, min(4.0, xi[0]))
            if (abs(xi[0] - uPrev) < 1e-10 && abs(xi[c] - cPrev) < 1e-10) break
        }
        for (i in 0 until nx) { x[i] = xi[i]; for (j in 0 until nx) p[i][j] = p0[i][j] }
        joseph(k, h, r)
        x[0] = max(-2.0, min(4.0, x[0]))
        return z
    }

    fun updateLogRange(uM: Double, sigmaU: Double): Double {
        val h = DoubleArray(nx); h[0] = 1.0
        return scalarUpdate(h, uM - x[0], sigmaU * sigmaU, 2.5)
    }

    private fun resetState(i: Int, value: Double, variance: Double) {
        x[i] = value
        for (j in 0 until nx) { p[i][j] = 0.0; p[j][i] = 0.0 }
        p[i][i] = variance
    }
    fun resetOffset(k: Int, value: Double, variance: Double) { if (k >= 0 && 1 + k < nx - 1) resetState(1 + k, value, variance) }
    fun resetRttOffset(value: Double, variance: Double) = resetState(nx - 1, value, variance)

    companion object { const val K_MAX = 5 }
}

/** §5.3 recursive least squares for [P0, n]. */
class Rls2(p0Prior: Double = -59.0, nPrior: Double = 2.0, varP0: Double = 64.0, varN: Double = 0.25, private val lambda: Double = 0.999) {
    var p0: Double = p0Prior
    var n: Double = nPrior
    val s = arrayOf(doubleArrayOf(varP0, 0.0), doubleArrayOf(0.0, varN))

    fun update(log10d: Double, level: Double, R: Double) {
        val h0 = 1.0; val h1 = -10.0 * log10d
        val sh0 = s[0][0] * h0 + s[0][1] * h1
        val sh1 = s[1][0] * h0 + s[1][1] * h1
        val sv = h0 * sh0 + h1 * sh1 + R
        val k0 = sh0 / sv; val k1 = sh1 / sv
        val e = level - (p0 * h0 + n * h1)
        p0 += k0 * e
        n += k1 * e
        val a = (s[0][0] - k0 * sh0) / lambda
        val b = (s[0][1] - k0 * sh1) / lambda
        val c = (s[1][0] - k1 * sh0) / lambda
        val d = (s[1][1] - k1 * sh1) / lambda
        s[0][0] = a; s[1][1] = d; s[0][1] = 0.5 * (b + c); s[1][0] = s[0][1]
        n = max(1.5, min(4.5, n))
    }
}

/** §4 anchor maths: local tangent plane, RV-frame re-projection, trilateration, the rv-gnss tier. */
object AnchorMath {
    private const val M_PER_DEG = 111320.0

    fun enu(refLat: Double, refLon: Double, refAlt: Double?, lat: Double, lon: Double, alt: Double?): Enu =
        Enu((lon - refLon) * M_PER_DEG * cos(refLat * Math.PI / 180.0), (lat - refLat) * M_PER_DEG,
            if (alt == null || refAlt == null) 0.0 else alt - refAlt)

    fun fromEnu(refLat: Double, refLon: Double, refAlt: Double?, e: Double, n: Double, u: Double): LatLonAlt =
        LatLonAlt(refLat + n / M_PER_DEG, refLon + e / (M_PER_DEG * cos(refLat * Math.PI / 180.0)), refAlt?.let { it + u })

    /** Clockwise (compass sense) by [deg]. */
    fun rotate(e: Double, n: Double, deg: Double): Pair<Double, Double> {
        val t = deg * Math.PI / 180.0
        return Pair(e * cos(t) + n * sin(t), -e * sin(t) + n * cos(t))
    }

    fun distanceM(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val r = 6371000.0; val d2r = Math.PI / 180.0
        val dLat = (lat2 - lat1) * d2r; val dLon = (lon2 - lon1) * d2r
        val a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * d2r) * cos(lat2 * d2r) * sin(dLon / 2) * sin(dLon / 2)
        return 2 * r * asin(min(1.0, sqrt(a)))
    }

    /** New absolute position of an RV anchor with offset (e, n, u) from the reference, placed at [headingPlacedDeg]. */
    fun reproject(e: Double, n: Double, u: Double, headingPlacedDeg: Double?, newRefLat: Double, newRefLon: Double,
                  newRefAlt: Double?, newHeadingDeg: Double?): Reprojected {
        val rotateIt = newHeadingDeg != null && headingPlacedDeg != null
        var e2 = e; var n2 = n
        if (rotateIt) { val r = rotate(e, n, newHeadingDeg!! - headingPlacedDeg!!); e2 = r.first; n2 = r.second }
        val p = fromEnu(newRefLat, newRefLon, newRefAlt, e2, n2, u)
        return Reprojected(p.lat, p.lon, p.alt, if (rotateIt) newHeadingDeg else headingPlacedDeg, !rotateIt)
    }

    fun shouldReproject(oldLat: Double, oldLon: Double, stopLat: Double, stopLon: Double): Boolean = distanceM(oldLat, oldLon, stopLat, stopLon) > 250.0

    /** MAP Gauss–Newton position from ranges to anchors, with a Gaussian prior (the device's own fix). */
    fun trilaterate(ranges: List<AnchorRange>, priorLat: Double, priorLon: Double, priorSigmaM: Double): Trilat {
        val m = ranges.size
        if (m == 0) return Trilat(false, 0.0, 0.0, 0.0)
        val ea = DoubleArray(m); val na = DoubleArray(m); val wa = DoubleArray(m)
        for (i in 0 until m) {
            val q = enu(priorLat, priorLon, null, ranges[i].lat, ranges[i].lon, null)
            ea[i] = q.e; na[i] = q.n
            val s2 = ranges[i].sigmaM * ranges[i].sigmaM + ranges[i].accM * ranges[i].accM
            wa[i] = 1.0 / max(1e-6, s2)
        }
        val w0 = 1.0 / (priorSigmaM * priorSigmaM)
        var pe = 0.0; var pn = 0.0
        var a00 = 0.0; var a01 = 0.0; var a11 = 0.0; var g0 = 0.0; var g1 = 0.0
        fun build() {
            a00 = w0; a01 = 0.0; a11 = w0
            g0 = -w0 * pe; g1 = -w0 * pn
            for (i in 0 until m) {
                val dx = pe - ea[i]; val dy = pn - na[i]
                val d = sqrt(dx * dx + dy * dy)
                val j0: Double; val j1: Double
                if (d < 0.01) { j0 = 0.0; j1 = 1.0 } else { j0 = dx / d; j1 = dy / d }
                val r = ranges[i].distM - max(d, 0.01)
                a00 += wa[i] * j0 * j0; a01 += wa[i] * j0 * j1; a11 += wa[i] * j1 * j1
                g0 += wa[i] * j0 * r; g1 += wa[i] * j1 * r
            }
        }
        var iterations = 0
        for (it in 0 until 30) {
            build()
            val det = a00 * a11 - a01 * a01
            if (!(abs(det) > 1e-300)) break
            val s0 = (a11 * g0 - a01 * g1) / det; val s1 = (-a01 * g0 + a00 * g1) / det
            pe += s0; pn += s1
            iterations = it + 1
            if (abs(s0) < 1e-6 && abs(s1) < 1e-6) break
        }
        build()
        val det = a00 * a11 - a01 * a01
        if (!(abs(det) > 1e-300)) return Trilat(false, 0.0, 0.0, 0.0, used = m, iterations = iterations)
        val sigma = sqrt(0.5 * (a11 / det + a00 / det))
        var ss = 0.0
        for (i in 0 until m) { val r = ranges[i].distM - sqrt((pe - ea[i]) * (pe - ea[i]) + (pn - na[i]) * (pn - na[i])); ss += r * r }
        val p = fromEnu(priorLat, priorLon, null, pe, pn, 0.0)
        return Trilat(true, p.lat, p.lon, sigma, sqrt(ss / m), m, iterations)
    }

    /** §4.3.7: the desktop's position from a Pi's averaged GNSS fix. ok = the fix is good enough (σ ≤ 5 m). */
    fun rvGnss(gnssLat: Double, gnssLon: Double, gnssSigma: Double, gnssOffset: Enu?, thisOffset: Enu?, anchorAccGnss: Double, anchorAccThis: Double,
               headingPlacedDeg: Double? = null, headingDeg: Double? = null): Trilat {
        if (gnssOffset != null && thisOffset != null) {
            var de = thisOffset.e - gnssOffset.e
            var dn = thisOffset.n - gnssOffset.n
            val dz = thisOffset.u - gnssOffset.u
            if (headingDeg != null && headingPlacedDeg != null) { val r = rotate(de, dn, headingDeg - headingPlacedDeg); de = r.first; dn = r.second }
            val p = fromEnu(gnssLat, gnssLon, null, de, dn, dz)
            return Trilat(gnssSigma <= 5.0, p.lat, p.lon, sqrt(gnssSigma * gnssSigma + anchorAccGnss * anchorAccGnss + anchorAccThis * anchorAccThis))
        }
        return Trilat(gnssSigma <= 5.0, gnssLat, gnssLon, sqrt(gnssSigma * gnssSigma + 25.0))
    }

}
