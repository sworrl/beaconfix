package org.sworrl.beaconfix

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.estimate.Context
import org.sworrl.beaconfix.estimate.EstimateRepository
import org.sworrl.beaconfix.estimate.Estimator
import org.sworrl.beaconfix.estimate.Fit
import org.sworrl.beaconfix.estimate.Known
import org.sworrl.beaconfix.estimate.Miss
import org.sworrl.beaconfix.estimate.Obs
import org.sworrl.beaconfix.estimate.Options
import java.util.Random
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Synthetic-geometry checks of the estimator, ported from the desktop's tests/estimator_test.cpp (the random draws differ
 * — java.util.Random, not mt19937 — so these are the behavioural checks; EstimatorGoldenTest pins the exact numbers).
 * Synthetic origin only (40.0 / −75.0 area).
 */
class EstimatorTest {
    private val apLat = 40.0030; private val apLon = -75.0680; private val p0 = -38.0; private val n = 2.6
    private val t0 = 1790000000L
    private val rng = Random(42)

    private fun mLat(north: Double) = north / 111320.0
    private fun mLon(east: Double) = east / (111320.0 * cos(apLat * PI / 180))

    private fun sample(lat: Double, lon: Double, acc: Double, dbNoise: Double, t: Long = t0, jitter: Double = 0.6, dev: String = "", offset: Double = 0.0): Obs {
        val o = Obs(lat = lat, lon = lon, acc = acc, t = t, device = dev)
        val d = sqrt(Estimator.distanceM(lat, lon, apLat, apLon).pow(2) + 9.0)
        o.dbm = Estimator.lround(Estimator.modelDbm(p0, n, d) + offset + rng.nextGaussian() * dbNoise).toInt()
        val s = acc / 1.515 * jitter
        o.lat += mLat(rng.nextGaussian() * s); o.lon += mLon(rng.nextGaussian() * s)
        return o
    }
    private fun at(east: Double, north: Double, acc: Double, db: Double, t: Long = t0, dev: String = "", offset: Double = 0.0) =
        sample(apLat + mLat(north), apLon + mLon(east), acc, db, t, 0.6, dev, offset)
    private fun err(f: Fit) = Estimator.distanceM(f.lat, f.lon, apLat, apLon)

    private fun ring(count: Int = 40): List<Obs> = (0 until count).map { i ->
        val ang = i * 2 * PI / count; val rad = 60.0 + (i % 5) * 30
        at(rad * cos(ang), rad * sin(ang), 8.0, 3.0)
    }

    @Test fun helpers() {
        assertEquals("circular CEP = 1.1774σ", 0.5, Estimator.probWithin(10.0, 10.0, 11.774), 0.002)
        assertEquals("circular R95 = 2.4477σ", 24.477, Estimator.radiusFor(10.0, 10.0, 0.95), 0.05)
        assertEquals("degenerate ellipse R95 → 1.96σ", 19.6, Estimator.radiusFor(10.0, 0.001, 0.95), 0.2)
        assertEquals(0.975, Estimator.normCdf(1.959964), 1e-6)
        assertEquals("A", Estimator.letterFor(90.0)); assertEquals("B", Estimator.letterFor(70.0)); assertEquals("F", Estimator.letterFor(19.9))
        assertEquals(-3L, Estimator.lround(-2.5)); assertEquals(3L, Estimator.lround(2.5)); assertEquals(-2L, Estimator.lround(-2.4))
    }

    @Test fun ringAroundTheApIsAGoodFix() {
        val f = Estimator.fitAp(ring(), t0)
        assertTrue("fix within 15 m: ${err(f)} ($f)", f.valid && f.kind == "fix" && err(f) < 15.0)
        assertTrue("model recovered: p0 ${f.p0} n ${f.pathloss}", abs(f.p0 - p0) < 6 && abs(f.pathloss - n) < 0.5)
        assertTrue("R95 ${f.r95} covers the error ${err(f)}", f.r95 >= err(f))
        assertTrue("grade ${f.grade} (${f.score})", f.grade == "A" || f.grade == "B")
        assertTrue("geometry: inHull ${f.inHull} rbar ${f.rbar} lin ${f.linRatio} modes ${f.modes}", f.inHull && f.rbar < 0.2 && f.linRatio > 0.5 && f.modes == 1 && !f.ambiguous)
        // the noise scale is estimated now (σ0 = 6 dB is only its prior): a clean ring fits tighter than the bound at σ0
        assertTrue("DOP ${f.rssDop}, R95 ${f.r95} < CRLB R95 at σ0 ${f.crlbR95} (σ ${f.sigmaDb} dB)", f.rssDop > 0 && f.rssDop < 200 && f.crlbR95 > 0 && f.r95 < f.crlbR95)
        assertTrue("Spearman ${f.spearman}", f.spearman < -0.5)
        assertEquals("good", f.quality)
    }

    @Test fun onePlaceIsAtMostARegion() {
        val obs = (0 until 12).map { sample(apLat + 0.0008, apLon + 0.0003, 20.0, 3.0, t0, 0.3) }
        val f = Estimator.fitAp(obs, t0)
        assertTrue("${f.kind} ${f.grade}", f.kind != "fix" && (f.grade == "R" || !f.valid))
        val spread = (0 until 9).map { i -> sample(apLat + 0.0008 + (i % 3) * 0.0003, apLon, 90.0, 3.0, t0, 0.15) }
        assertNotEquals("spread inside the fix error", "fix", Estimator.fitAp(spread, t0).kind)
    }

    @Test fun outliersAreRejected() {
        val obs = ArrayList<Obs>()
        for (i in 0 until 40) { val ang = i * 2 * PI / 40; val rad = 50.0 + (i % 4) * 40; obs.add(at(rad * cos(ang), rad * sin(ang), 8.0, 3.0)) }
        val clean = Estimator.fitAp(obs, t0)
        for (i in 0 until 3) obs.add(Obs(lat = apLat + 0.0027, lon = apLon + 0.0005 * i, acc = 8.0, dbm = -40, t = t0))
        val f = Estimator.fitAp(obs, t0)
        assertTrue("baseline ${err(clean)}", clean.valid && err(clean) < 25.0)
        assertTrue("outliers: error ${err(f)} (was ${err(clean)}), rejected ${f.rejected}", f.valid && f.rejected >= 2 && err(f) < err(clean) + 10.0 && err(f) < 30.0)
    }

    private fun knownAround(count: Int, meLat: Double, meLon: Double, r0: Double): MutableList<Known> = (0 until count).map { i ->
        val ang = i * 2 * PI / count + 0.3; val rad = r0 + i * 15
        Known(lat = meLat + rad * sin(ang) / 111320.0, lon = meLon + rad * cos(ang) / (111320.0 * cos(meLat * PI / 180)), acc = 20.0, p0 = -38.0, pathloss = 2.6, haveModel = true,
            dbm = Estimator.lround(Estimator.modelDbm(-38.0, 2.6, rad) + rng.nextGaussian() * 3.0).toInt())
    }.toMutableList()

    @Test fun selfLocateAndIntegrity() {
        val meLat = 40.0040; val meLon = -75.0690
        val known = knownAround(5, meLat, meLon, 70.0)
        val s = Estimator.selfLocate(known)
        val e = Estimator.distanceM(s.lat, s.lon, meLat, meLon)
        assertTrue("self-locate: error $e ($s)", s.valid && e < 30.0)
        assertTrue("honest accuracy ${s.acc} vs $e", s.acc >= e * 0.5)
        known[2] = known[2].copy(lat = known[2].lat + 400.0 / 111320.0)
        val s2 = Estimator.selfLocate(known)
        val e2 = Estimator.distanceM(s2.lat, s2.lon, meLat, meLon)
        assertTrue("a moved AP is excluded: $s2, error $e2", s2.valid && s2.excluded >= 1 && e2 < 45.0)
        assertFalse("one beacon is not enough", Estimator.selfLocate(known.take(1)).valid)
    }

    @Test fun incrementalUpdatesConverge() {
        val obs = (0 until 10).map { i -> val ang = i * 2 * PI / 10; at(90 * cos(ang), 90 * sin(ang), 10.0, 3.0) }
        var f = Estimator.fitAp(obs, t0)
        if (f.kind != "fix") return          // the random draw gave a region: nothing to nudge (the golden test covers update())
        f.lat += mLat(20.0)
        val before = err(f); val r95 = f.r95
        for (i in 0 until 30) f = Estimator.update(f, sample(apLat + 0.0009 * sin(i.toDouble()), apLon + 0.0009 * cos(i.toDouble()), 8.0, 2.0))
        assertTrue("updates: $before → ${err(f)}", err(f) < before && err(f) < 40)
        assertTrue("covariance shrinks and stays positive", f.r95 <= r95 && f.cxx > 0 && f.cyy > 0)
    }

    @Test fun oneOrTwoSamplesAreARegion() {
        val obs = arrayListOf(at(80.0, 0.0, 8.0, 3.0))
        val f1 = Estimator.fitAp(obs, t0)
        assertTrue("one sample: ${f1.kind} R95 ${f1.r95} error ${err(f1)}", f1.valid && f1.kind == "region" && f1.grade == "R" && err(f1) < f1.r95)
        obs.add(at(0.0, 80.0, 8.0, 3.0))
        val f2 = Estimator.fitAp(obs, t0)
        assertTrue("two samples: R95 ${f2.r95} error ${err(f2)}", f2.valid && f2.kind == "region" && err(f2) < f2.r95)
        val misses = (0 until 8).map { i -> Miss(apLat + mLat(-200.0 + 50 * (i % 3)), apLon + mLon(-200.0 + 40 * (i / 3)), 3) }
        val f3 = Estimator.fitAp(obs, t0, Options(), Context(misses = misses))
        assertTrue("misses shrink the region: ${f2.r95} → ${f3.r95}", f3.valid && f3.r95 < f2.r95)
    }

    @Test fun aRoadIsNeverAnA() {
        val obs = (0 until 16).map { i -> at(-240.0 + 32 * i, -50.0, 8.0, 3.0) }
        val f = Estimator.fitAp(obs, t0)
        assertTrue("collinear: lin ${f.linRatio} inHull ${f.inHull}", f.valid && f.linRatio < 0.02 && !f.inHull)
        assertTrue("capped: ${f.score} ${f.grade}", f.score <= 59.0 && f.grade != "A" && f.grade != "B")
        assertTrue("suggests a place to sample", f.suggestGain > 0)
    }

    @Test fun travellingApsAreMobile() {
        val obs = ArrayList<Obs>()
        for (i in 0 until 4) obs.add(at(40.0 * i, 30.0, 10.0, 3.0))
        for (i in 0 until 4) obs.add(at(8000.0 + 40 * i, 30.0, 10.0, 3.0).also { it.dbm = -60 })
        val f = Estimator.fitAp(obs, t0)
        assertTrue("${f.kind} ${f.grade}", f.kind == "mobile" && f.grade == "M" && !f.valid)
        assertEquals("M", Estimator.fitAp(obs, t0, Options(), Context(mobile = true)).grade)
    }

    @Test fun aMovedApFitsTheRecentEpoch() {
        val obs = ArrayList<Obs>()
        for (i in 0 until 12) { val ang = i * 2 * PI / 12; obs.add(sample(apLat + 0.0027 + mLat(80 * sin(ang)), apLon + mLon(80 * cos(ang)), 8.0, 2.0, t0 - 200 * 86400L)) }
        for (o in obs) { val d = sqrt(Estimator.distanceM(o.lat, o.lon, apLat + 0.0027, apLon).pow(2) + 9); o.dbm = Estimator.lround(Estimator.modelDbm(p0, n, d)).toInt() }
        for (i in 0 until 12) { val ang = i * 2 * PI / 12; obs.add(sample(apLat + mLat(80 * sin(ang)), apLon + mLon(80 * cos(ang)), 8.0, 2.0, t0 - 5 * 86400L)) }
        val f = Estimator.fitAp(obs, t0)
        assertTrue("moved ${f.moved}, error ${err(f)}", f.valid && f.moved && err(f) < 25)
    }

    /** A second device hearing 8 dB louder: its per-AP deviation δ absorbs the offset even before it is calibrated. */
    @Test fun anUncalibratedDeviceOffsetIsAbsorbed() {
        val obs = (0 until 24).map { i -> val ang = i * 2 * PI / 24; val rad = 60.0 + (i % 3) * 40; at(rad * cos(ang), rad * sin(ang), 8.0, 2.5, t0, if (i % 2 != 0) "phone" else "", if (i % 2 != 0) 8.0 else 0.0) }
        val raw = Estimator.fitAp(obs, t0)
        val fixed = Estimator.fitAp(obs, t0, Options(), Context(deviceOffset = mapOf("phone" to 8.0)))
        assertTrue("device offset absorbed: rms ${raw.rms} vs ${fixed.rms} dB, error ${err(raw)} vs ${err(fixed)} m, devices ${fixed.devices}",
            fixed.valid && raw.valid && abs(raw.rms - fixed.rms) < 0.5 && err(raw) < err(fixed) + 3 && fixed.devices == 2)
    }

    /** A device that hears THIS AP 7 dB quieter than its calibration, at a third of the places: δ takes it, the fit stays. */
    @Test fun aQuieterSecondDeviceDoesNotDragTheFit() {
        val e0 = ArrayList<Double>(); val e1 = ArrayList<Double>()
        for (trial in 0 until 20) {
            val one = ArrayList<Obs>(); val two = ArrayList<Obs>()
            for (i in 0 until 24) {
                val ang = i * 2 * PI / 24; val rad = 40.0 + (i % 4) * 30
                val deck = i % 3 == 0
                one.add(at(rad * cos(ang), rad * sin(ang), 6.0, 4.0, t0, "phone"))
                two.add(at(rad * cos(ang), rad * sin(ang), 6.0, 4.0, t0, if (deck) "deck" else "phone", if (deck) -7.0 else 0.0))
            }
            e0.add(err(Estimator.fitAp(one, t0))); e1.add(err(Estimator.fitAp(two, t0)))
        }
        e0.sort(); e1.sort()
        assertTrue("median error ${e1[10]} m with the quieter device (one device ${e0[10]} m)", e1[10] < e0[10] + 4)
    }

    /** update() removes a device's calibrated offset from its level, as the batch fit does. */
    @Test fun anUpdateTakesTheDeviceOffset() {
        val base = Estimator.fitAp(ring(), t0)
        assertTrue("base fit ${base.kind}", base.valid)
        val o = at(100.0, 0.0, 5.0, 0.0, t0, "phone", 8.0)                 // heard 8 dB louder than this device would
        val raw = Estimator.update(base, o, Options(), 0.0); val cal = Estimator.update(base, o, Options(), 8.0)
        val host = Estimator.update(base, o.copy(device = "", dbm = o.dbm - 8), Options(), 0.0)
        assertTrue("calibrated update equals this device's (uncorrected differs by ${Estimator.distanceM(raw.lat, raw.lon, host.lat, host.lon)} m)",
            Estimator.distanceM(cal.lat, cal.lon, host.lat, host.lon) < 1e-6 && Estimator.distanceM(raw.lat, raw.lon, host.lat, host.lon) > 0.01)
    }

    @Test fun rttRangesTightenAThinFit() {
        val obs = (0 until 4).map { i -> val ang = i * 2 * PI / 4 + 0.4; at(70 * cos(ang), 70 * sin(ang), 8.0, 4.0) }
        val plain = Estimator.fitAp(obs, t0)
        val withRtt = obs.map { o -> o.copy(rangeM = sqrt(Estimator.distanceM(o.lat, o.lon, apLat, apLon).pow(2) + 9) + 1.0, rangeSd = 2.0) }
        val rtt = Estimator.fitAp(withRtt, t0)
        assertTrue("RTT: R95 ${plain.r95} → ${rtt.r95}, error ${err(rtt)}", rtt.valid && rtt.r95 < plain.r95 && err(rtt) < 25)
    }

    @Test fun hysteresisHoldsALetterOnce() {
        val base = Estimator.fitAp(ring(), t0).copy(valid = true, kind = "fix")
        val prev = base.copy(grade = "B", pendingGrade = "")
        val f = base.copy(cP = 0.855, cG = 0.855, cE = 0.855, cF = 0.855, cS = 0.855, cT = 0.855, cX = -1.0, ambiguous = false, modes = 1, inHull = true)   // score 85.5: an A by 0.5
        Estimator.grade(f, prev)
        assertEquals("B", f.grade); assertEquals("A", f.pendingGrade)
        val g = f.copy(); Estimator.grade(g, f)
        assertEquals("the second refit confirms A", "A", g.grade)
    }

    @Test fun staleDataAndDisagreeingExternalsCostPoints() {
        val obs = (0 until 20).map { i -> val ang = i * 2 * PI / 20; at(80 * cos(ang), 80 * sin(ang), 8.0, 3.0, t0 - 400 * 86400L) }
        val old = Estimator.fitAp(obs, t0)
        assertTrue("freshness ${old.cT}", old.cT >= 0.3 && old.cT < 0.31)
        val agree = Context(external = org.sworrl.beaconfix.estimate.External(true, apLat + mLat(10.0), apLon, 25.0))
        val clash = Context(external = org.sworrl.beaconfix.estimate.External(true, apLat + mLat(600.0), apLon, 25.0))
        val a = Estimator.fitAp(obs, t0, Options(), agree); val b = Estimator.fitAp(obs, t0, Options(), clash)
        assertTrue("external: D² ${a.extD2} vs ${b.extD2}, score ${a.score} vs ${b.score}", a.extD2 >= 0 && a.extD2 < 3 && b.extD2 > 20 && b.score < a.score)
    }

    @Test fun r95CoverageOnRandomGeometry() {
        var inside = 0; var total = 0
        for (trial in 0 until 60) {
            val k = 5 + trial % 12
            val obs = (0 until k).map { val a = rng.nextDouble() * 2 * PI; val r = 30 + rng.nextDouble() * 170; at(r * cos(a), r * sin(a), 12.0, 6.0) }
            val f = Estimator.fitAp(obs, t0)
            if (f.kind != "fix") continue
            ++total; if (err(f) <= f.r95) ++inside
        }
        assertTrue("R95 coverage $inside / $total", total >= 20 && inside >= 0.85 * total)
    }

    /** One road 30 m off: the AP and its mirror explain the levels equally; R95 must reach whichever is the AP. */
    @Test fun aRoadsMirrorIsInsideR95() {
        var inside = 0; var total = 0
        for (trial in 0 until 60) {
            val obs = (0 until 30).map { at(-150 + rng.nextDouble() * 300, -30.0, 5.0, 5.0) }
            val f = Estimator.fitAp(obs, t0)
            if (f.kind != "fix") continue
            ++total; if (err(f) <= f.r95) ++inside
        }
        assertTrue("one road: R95 covers $inside / $total", total >= 20 && inside >= 0.85 * total)
    }

    /** One absurd reading (+60 dBm) among exact ones: its robust weight underflowed to 0 and voided the region (NaN). */
    @Test fun anAbsurdReadingDoesNotVoidARegion() {
        val obs = ArrayList<Obs>()
        for (i in 0 until 4) {
            val x = -30.0 + i * 20; val y = 40.0; val d = sqrt(x * x + y * y + 9.0)
            obs.add(Obs(lat = apLat + mLat(y), lon = apLon + mLon(x), acc = 6.0, t = t0, dbm = Estimator.lround(Estimator.modelDbm(p0, n, d)).toInt()))
        }
        obs.add(Obs(lat = apLat + mLat(340.0), lon = apLon, acc = 6.0, t = t0, dbm = 60))
        val f = Estimator.fitAp(obs, t0)
        assertTrue("a +60 dBm reading: still a ${f.kind} (valid ${f.valid}, R95 ${f.r95})", f.valid && f.kind == "region" && f.lat.isFinite() && f.lon.isFinite())
    }

    @Test fun bandPriorsAndGradeWeights() {
        val o24 = EstimateRepository.optionsFor(2437); val o5 = EstimateRepository.optionsFor(5180); val o6 = EstimateRepository.optionsFor(5975)
        assertEquals(-40.0, o24.p0Mean, 0.0); assertEquals(2.4, o24.defaultN, 0.0)
        assertEquals(-47.0, o5.p0Mean, 0.0); assertEquals(2.7, o5.defaultN, 0.0)
        assertEquals(-48.0, o6.p0Mean, 0.0); assertEquals(2.7, o6.defaultN, 0.0)
        // unknown band (as the desktop): wide enough for 2.4 and 5 GHz
        val o0 = EstimateRepository.optionsFor(0)
        assertEquals(-42.0, o0.p0Mean, 0.0); assertEquals(10.0, o0.p0Sd, 0.0); assertEquals(2.5, o0.defaultN, 0.0)
        assertEquals(1.0, EstimateRepository.weightFor("A"), 0.0); assertEquals(0.8, EstimateRepository.weightFor("C"), 0.0)
        assertEquals(0.6, EstimateRepository.weightFor("D"), 0.0); assertEquals(0.4, EstimateRepository.weightFor("R"), 0.0); assertEquals(0.3, EstimateRepository.weightFor("F"), 0.0)
    }
}
