// Cross-language test vectors for RangeMath.kt: the SAME inputs as tests/ranging_math_test.cpp, and the
// expected values its `--vectors` run printed (docs/RANGING.md §11). Regenerate both together.
package org.sworrl.beaconfix.ranging

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.sqrt
import kotlin.math.cos
import kotlin.math.ln

class RangeMathTest {
    private val expected: Map<String, List<Any>> = mapOf(
        "anchor_calibration" to listOf<Any>(-37.295802015063686, 2.4287654109893175, 0.7143219926188046),
        "ble" to listOf<Any>("898131b88e0457b0", 7.0, "898131b88e0457b0f907", -7.0, 1.0, true, true, 0.0),
        "ble_window_neg" to listOf<Any>("cf6fff450eef869b"),
        "chi2_6_+" to listOf<Any>(9.229109314951645),
        "chi2_6_-" to listOf<Any>(2.7560888499578042),
        "classify" to listOf<Any>("unknown", "adjacent", "room", "far", "near", "unknown"),
        "enu" to listOf<Any>(1.7054479876969342, 2.0037599996868494, 1.2000000000000028),
        "filter" to listOf<Any>(0.07689093392987602, -2.346415535374766, -3.216278090901163, 0.031994050882637924, 0.6099683097169435, 15.463545696660228, 11.340825811962223, 16.87439220342253, 1.1936882912146383, 0.49163323972359524, 0.7907189095276757, 1.8020205656066342, -0.051605713228050784, -0.12249847699217876, -0.1944921230282392, -0.8251850909005902),
        "filter_drift" to listOf<Any>(0.6298835297924379, 2.2340875206714412, 0.18792407716665965, 3.578842109814038, 82.34179352734864, 91.34179352734864, 0.253602, 0.4333269413222595),
        "filter_rtt_robust" to listOf<Any>(1.3733860610084303, 0.00012094918043716869, 23.62577485657404),
        "fingerprint" to listOf<Any>(-3.450000000000003, 2.6597122993644633, 13.321666666666667, 0.0, 0.0, 0.0, 0.0, 6.0, -10.255926479189677, -13.444994423861454),
        "fingerprint_far" to listOf<Any>(4.0, 54.952733457943324, 44.702733457943324, 4.666438293955714, 1.9159647199595962, 17.307883242697173),
        "fit" to listOf<Any>(-39.853867193347156, 2.271109132634109, 6.269943059867058, 0.09800952168791119, 0.5619772550236338, 0.4124708581136841),
        "fspl_0.61m_5180" to listOf<Any>(42.44319189512001),
        "fspl_1m_2437" to listOf<Any>(40.18711058369449),
        "geo" to listOf<Any>(1.1999999997307187, -0.7999999997493872, -2.9999999999804126, 4.0, 242.26182037161277, 18.70447781205538, 20.18114356805259, 5.554126396376508, 4.885697637654346e-21),
        "level1" to listOf<Any>(-50.42527605252986, 1.9422239675774466, 5.0),
        "level2" to listOf<Any>(-59.92358370267848, 2.507400360344115, 3.0),
        "median" to listOf<Any>(4.0),
        "model_distance" to listOf<Any>(0.6095368972401694),
        "model_level" to listOf<Any>(-45.70659670021534),
        "prior_ble_-7" to listOf<Any>(-48.0),
        "prior_ble_127" to listOf<Any>(-59.0),
        "prior_wifi_5180" to listOf<Any>(-46.549484611210175),
        "q7_0.632" to listOf<Any>(5.0),
        "q7_0.95" to listOf<Any>(7.5),
        "rel_fp" to listOf<Any>(7.186558590674563, 1.7593465916496707, 17.065026528572087, "near"),
        "rel_range" to listOf<Any>(0.61, 0.48511734778731724, 0.7670309084950191, 0.14095678035385092, 0.0, 5.4745342114606975e-17, "adjacent"),
        "rel_range_fix" to listOf<Any>(45.09074549867884, 31.860582597534947, 60.591585411392664, 36.86989764584402, 26.80350483842413, 0.8963513400468486, "far"),
        "reproject_known" to listOf<Any>(40.0029712540424, -75.06801759070221, 120.4, false),
        "reproject_unknown" to listOf<Any>(40.002986525332375, -75.0679624731686, 120.4, true),
        "rls" to listOf<Any>(-49.265931839945814, 1.9142909075068142, 5.806407120458949, 0.3608712634841288, 0.14915744844330686),
        "rotate" to listOf<Any>(4.0, -2.9999999999999996),
        "rv_gnss" to listOf<Any>(40.0029820337765, -75.06816449924145, 1.8920887928424501, true, 40.003, -75.0681, 5.314132102234569),
        "trilat" to listOf<Any>(40.00291088304175, -75.06803931836865, 0.4455765733186116, 1.3221886849534632, 14.0),
    )

    private fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it.toInt() and 0xFF) }

    private fun computed(): Map<String, List<Any>> {
        val v = LinkedHashMap<String, List<Any>>()
        v["fspl_1m_2437"] = listOf(RangeMath.fsplDb(1.0, 2437.0))
        v["fspl_0.61m_5180"] = listOf(RangeMath.fsplDb(0.61, 5180.0))
        v["prior_ble_-7"] = listOf(RangeMath.priorP0Ble(-7))
        v["prior_ble_127"] = listOf(RangeMath.priorP0Ble(127))
        v["prior_wifi_5180"] = listOf(RangeMath.priorP0Wifi(5180.0))
        v["model_level"] = listOf(RangeMath.modelLevel(-50.0, 2.0, 0.61))
        v["model_distance"] = listOf(RangeMath.modelDistance(-50.0, 2.0, -45.7))
        val q = listOf(3.0, 1.0, 4.0, 1.0, 5.0, 9.0, 2.0, 6.0, 5.0, 3.0, 5.0)
        v["q7_0.95"] = listOf(RangeMath.quantile7(q, 0.95))
        v["q7_0.632"] = listOf(RangeMath.quantile7(q, 0.632))
        v["median"] = listOf(RangeMath.median(q))
        v["chi2_6_+"] = listOf(RangeMath.chi2Quantile(6.0, RangeMath.Z84))
        v["chi2_6_-"] = listOf(RangeMath.chi2Quantile(6.0, -RangeMath.Z84))
        val l1 = RangeMath.levelFromSamples(listOf(-51.0, -49.0, -55.0, -62.0, -50.0, -48.0, -53.0, -47.0, -58.0, -50.0, -52.0, -49.0), 3, 20.0, false)
        val l2 = RangeMath.levelFromSamples(listOf(-60.0, -61.0, -59.0), 1, 2.0, true)
        v["level1"] = listOf(l1.dbm, l1.sigma, l1.nEff)
        v["level2"] = listOf(l2.dbm, l2.sigma, l2.nEff)

        val f = RangeFilter(2)
        f.predict(1.0, false)
        val z1 = f.updateRtt(1.2, 0.6, 0.0)
        val z2 = f.updateRssi(0, -52.0, 2.0, -48.0, 2.0)
        val z3 = f.updateRssi(1, -55.0, 2.5, -50.0, 2.0)
        f.predict(5.0, false)
        val z4 = f.updateRtt(0.9, 0.5, 0.3)
        v["filter"] = listOf(f.x[0], f.x[1], f.x[2], f.p[0][0], f.p[0][1], f.p[1][1], f.p[1][2], f.p[2][2],
            f.distanceM, f.sigmaM, f.lowM, f.highM, z1, z2, z3, z4)
        val f2 = RangeFilter(2)
        f2.updateRtt(25.0, 0.4, 0.0)
        f2.updateRtt(4.0, 0.4, 0.0)
        v["filter_rtt_robust"] = listOf(f2.x[0], f2.p[0][0], f2.distanceM)
        val f3 = RangeFilter(2)
        f3.updateRssi(0, -60.0, 2.0, -48.0, 2.0)
        f3.predict(3600.0, false)
        val z5 = f3.updateRssi(0, -58.0, 2.0, -48.0, 2.0)
        f3.predict(2.0, true)
        v["filter_drift"] = listOf(f3.x[0], f3.x[1], f3.p[0][0], f3.p[0][1], f3.p[1][1], f3.p[2][2], f3.p[3][3], z5)

        val r = Rls2(-59.0, 2.0)
        r.update(log10(0.61), -44.3, 16.0)
        r.update(log10(2.0), -54.1, 16.0)
        r.update(log10(5.0), -62.0, 16.0)
        v["rls"] = listOf(r.p0, r.n, r.s[0][0], r.s[0][1], r.s[1][1])

        val pf = RangeMath.fitPathLoss(listOf(CalPoint(1.0, -40.5), CalPoint(2.0, -47.0), CalPoint(4.0, -53.2), CalPoint(8.0, -60.1), CalPoint(16.0, -66.8)), -40.0, 2.4)
        v["fit"] = listOf(pf.p0, pf.n, pf.varP0, pf.varN, pf.covP0N, pf.rmsDb)

        val dp = listOf(DiffPair("a", -50.0, 2.0, -53.1, 2.2), DiffPair("b", -61.0, 2.5, -64.9, 2.5), DiffPair("c", -70.0, 3.0, -72.2, 3.0),
            DiffPair("d", -45.0, 1.8, -49.8, 1.9), DiffPair("e", -66.0, 2.8, -68.5, 2.9), DiffPair("f", -58.0, 2.1, -62.7, 2.0),
            DiffPair("a", -51.0, 2.0, -53.9, 2.2))
        val fp = RangeMath.fingerprintDistance(dp)
        v["fingerprint"] = listOf(fp.gainDb, fp.d2, fp.noise2, fp.excess, fp.deltaM, fp.lowM, fp.highM, fp.groups.toDouble(),
            RangeMath.fingerprintLogLik(fp, 1.0), RangeMath.fingerprintLogLik(fp, 10.0))
        val dp2 = listOf(DiffPair("a", -50.0, 2.0, -41.0, 2.0), DiffPair("b", -61.0, 2.0, -70.0, 2.0), DiffPair("c", -70.0, 2.0, -66.0, 2.0),
            DiffPair("d", -45.0, 2.0, -52.0, 2.0), DiffPair("e", -66.0, 2.0, -59.0, 2.0))
        val fq = RangeMath.fingerprintDistance(dp2)
        v["fingerprint_far"] = listOf(fq.gainDb, fq.d2, fq.excess, fq.deltaM, fq.lowM, fq.highM)

        val ge = doubleArrayOf(8.0, -12.0, 3.0, -6.0, 20.0); val gn = doubleArrayOf(5.0, 7.0, -15.0, -9.0, -2.0)
        val gp = (0 until 5).map { i ->
            val dA = kotlin.math.hypot(ge[i], gn[i]); val dB = kotlin.math.hypot(1.2 - ge[i], -0.8 - gn[i])
            GeoPair("g$i", ge[i], gn[i], 2.4, 2.0, -24.0 * log10(dB / dA) - 3.0, 1.5)
        }
        val gs = RangeMath.solveDifferential(gp)
        v["geo"] = listOf(gs.dE, gs.dN, gs.gainDb, gs.iterations.toDouble(), gs.spreadDeg, gs.cov[0][0], gs.cov[1][1], gs.cov[2][2], gs.chi2)

        val oa = RangeMath.relativePosterior(RelInput(haveRange = true, u = log10(0.61), puu = 0.01))
        v["rel_range"] = listOf(oa.distanceM, oa.lowM, oa.highM, oa.sigmaM, if (oa.haveBearing) 1.0 else 0.0, oa.resultantLength, oa.cls)
        val ob = RangeMath.relativePosterior(RelInput(haveRange = true, u = log10(50.0), puu = 0.04, fix = Gauss2(true, 30.0, 40.0, 400.0, 0.0, 400.0)))
        v["rel_range_fix"] = listOf(ob.distanceM, ob.lowM, ob.highM, ob.bearingDeg, ob.bearingSigmaDeg, ob.resultantLength, ob.cls)
        val oc = RangeMath.relativePosterior(RelInput(haveFp = true, fp = fp))
        v["rel_fp"] = listOf(oc.distanceM, oc.lowM, oc.highM, oc.cls)
        v["classify"] = listOf(RangeMath.classify(false, 0.0, 1.0), RangeMath.classify(true, 0.4, 1.9), RangeMath.classify(true, 1.0, 5.0),
            RangeMath.classify(true, 31.0, 80.0), RangeMath.classify(true, 8.0, 25.0), RangeMath.classify(true, 10.0, 45.0))

        val tag = RangeMath.bleTag("6htgz65xb7yfs53dmhdanfmk7c", 1790478720L)
        val flags = RangeMath.bleFlags(true, true, RangeMath.KIND_ANDROID, false)
        val sd = RangeMath.bleServiceData(tag, -7, flags)
        val ad = RangeMath.parseServiceData(sd)!!
        v["ble"] = listOf(hex(tag), flags.toDouble(), hex(sd), ad.txPower.toDouble(), ad.kind.toDouble(), ad.rtt, ad.api, ad.version.toDouble())
        v["ble_window_neg"] = listOf(hex(RangeMath.bleTag("x", -1L)))

        val e = AnchorMath.enu(40.002937, -75.06806, 120.0, 40.002955, -75.068040, 121.2)
        v["enu"] = listOf(e.e, e.n, e.u)
        val rt = AnchorMath.rotate(3.0, 4.0, 90.0)
        v["rotate"] = listOf(rt.first, rt.second)
        val p1 = AnchorMath.reproject(3.2, -1.5, 0.4, 212.0, 40.0030, -75.0680, 120.0, 302.0)
        val p2 = AnchorMath.reproject(3.2, -1.5, 0.4, 212.0, 40.0030, -75.0680, 120.0, null)
        v["reproject_known"] = listOf(p1.lat, p1.lon, p1.alt!!, p1.headingAssumed)
        v["reproject_unknown"] = listOf(p2.lat, p2.lon, p2.alt!!, p2.headingAssumed)
        val tl = AnchorMath.trilaterate(listOf(AnchorRange(40.00290, -75.06810, 4.1, 0.5, 0.2), AnchorRange(40.00298, -75.06800, 6.9, 0.5, 0.2),
            AnchorRange(40.00284, -75.06799, 7.7, 0.5, 0.2)), 40.0031, -75.0679, 40.0)
        v["trilat"] = listOf(tl.lat, tl.lon, tl.sigma, tl.rmsM, tl.iterations.toDouble())
        val g1 = AnchorMath.rvGnss(40.00300, -75.06810, 1.8, Enu(-2.0, 5.5, 1.8), Enu(0.0, 0.0, 0.0), 0.3, 0.5, 212.0, 302.0)
        val g2 = AnchorMath.rvGnss(40.00300, -75.06810, 1.8, null, null, 0.3, 0.5)
        v["rv_gnss"] = listOf(g1.lat, g1.lon, g1.sigma, g1.ok, g2.lat, g2.lon, g2.sigma)
        val af = RangeMath.fitPathLoss(listOf(CalPoint(2.1, -43.8), CalPoint(5.4, -55.2), CalPoint(9.8, -61.9), CalPoint(15.2, -66.0)),
            RangeMath.priorP0Wifi(2437.0), RangeMath.N_WIFI)
        v["anchor_calibration"] = listOf(af.p0, af.n, af.rmsDb)
        return v
    }

    @Test fun vectorsMatchTheCppImplementation() {
        val got = computed()
        val failures = ArrayList<String>()
        for ((key, exp) in expected) {
            val g = got[key]
            if (g == null) { failures += "$key: not computed"; continue }
            if (g.size != exp.size) { failures += "$key: size ${g.size} != ${exp.size}"; continue }
            for (i in exp.indices) {
                val e = exp[i]; val a = g[i]
                val ok = if (e is Number && a is Number) {
                    val ed = e.toDouble(); val ad = a.toDouble()
                    abs(ad - ed) <= 1e-9 * max(1.0, abs(ed))
                } else e == a
                if (!ok) failures += "$key[$i]: got $a, expected $e"
            }
        }
        assertTrue("vector mismatches:\n" + failures.joinToString("\n"), failures.isEmpty())
        assertEquals(expected.keys, got.keys)
    }

    // The same SplitMix64 + Box–Muller generator as the C++ harness.
    private class Rng(var s: Long) {
        fun next(): Long { s += -7046029254386353131L; var z = s; z = (z xor (z ushr 30)) * -4658895280553007687L; z = (z xor (z ushr 27)) * -7723592293110705685L; return z xor (z ushr 31) }
        fun uniform(): Double = (next() ushr 11).toDouble() * (1.0 / 9007199254740992.0)
        fun rayleighDb(): Double { var u = uniform(); if (u < 1e-300) u = 1e-300; return 10.0 * log10(-ln(u)) }
    }

    @Test fun winsorisedLinearMeanIsUnbiasedUnderRayleigh() {
        val g = Rng(99L)
        val s = List(4000) { -60.0 + g.rayleighDb() }
        val l = RangeMath.levelFromSamples(s, 3, 3600.0, true)
        assertEquals(-60.0, l.dbm, 0.3)
        assertEquals(-62.51, s.average(), 0.3)            // the mean of dB is 2.5 dB low
    }

    @Test fun rttRangesPinTheDistanceAndCalibrateTheBleOffset() {
        val f = RangeFilter(2)
        repeat(30) { f.predict(1.0, false); f.updateRtt(0.61 + 0.55, 0.3, 0.55); f.updateRssi(0, -47.0, 2.0, -59.0, 2.0) }
        assertEquals(0.61, f.distanceM, 0.05)
        assertEquals("adjacent", RangeMath.relativePosterior(RelInput(haveRange = true, u = f.u, puu = f.puu)).cls)
        assertTrue("the BLE link offset absorbed the wrong prior", abs(f.offset(0) - (-47.0 - (-59.0 - 20.0 * log10(0.61)))) < 1.0)
    }
}
