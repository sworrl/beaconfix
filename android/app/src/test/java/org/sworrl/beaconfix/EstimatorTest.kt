package org.sworrl.beaconfix

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.estimate.Estimator
import org.sworrl.beaconfix.estimate.Geo
import org.sworrl.beaconfix.estimate.KnownAp
import org.sworrl.beaconfix.estimate.Sample
import kotlin.math.cos
import kotlin.math.log10
import kotlin.math.sin
import kotlin.random.Random

class EstimatorTest {
    private val lat0 = 40.00; private val lon0 = -75.07
    private fun rssi(d: Double, p0: Double = -38.0, n: Double = 2.7, noise: Double, rnd: Random) = (p0 - 10 * n * log10(maxOf(1.0, d)) + rnd.nextDouble(-noise, noise)).toInt()

    /** 40 samples on a walk around an AP 60 m across, 4 dB noise, 6 m GPS error → position within 15 m. */
    @Test fun fitsAnApFromAWalkAround() {
        val rnd = Random(7)
        val local = Geo.Local(lat0, lon0)
        val apX = 12.0; val apY = -8.0
        val samples = (0 until 40).map { i ->
            val ang = i / 40.0 * 2 * Math.PI; val r = 25 + 20 * sin(3 * ang)
            val x = r * cos(ang) + rnd.nextDouble(-6.0, 6.0); val y = r * sin(ang) + rnd.nextDouble(-6.0, 6.0)
            val d = Math.hypot(x - apX, y - apY)
            val ll = local.toLatLon(x, y)
            Sample(ll[0], ll[1], 6.0, rssi(d, noise = 4.0, rnd = rnd))
        }
        val fit = Estimator.fitAp(samples)
        assertNotNull(fit)
        val apLL = local.toLatLon(apX, apY)
        val err = Geo.distanceM(fit!!.lat, fit.lon, apLL[0], apLL[1])
        assertTrue("position error $err m, fit=$fit", err < 15.0)
        assertTrue("accuracy should be honest (${fit.acc} m ≥ error $err m)", fit.acc >= err * 0.5)
        assertTrue("path exponent plausible ${fit.pathExp}", fit.pathExp in 1.6..4.5)
    }

    @Test fun rejectsOutliers() {
        val rnd = Random(3)
        val local = Geo.Local(lat0, lon0)
        val samples = (0 until 30).map { i ->
            val ang = i / 30.0 * 2 * Math.PI; val x = 30 * cos(ang); val y = 30 * sin(ang)
            val d = Math.hypot(x, y); val ll = local.toLatLon(x, y)
            val bad = i % 7 == 0
            Sample(ll[0], ll[1], 5.0, if (bad) -35 else rssi(d, noise = 3.0, rnd = rnd))   // every 7th sample is a wild reflection
        }
        val fit = Estimator.fitAp(samples)!!
        assertTrue("error ${Geo.distanceM(fit.lat, fit.lon, lat0, lon0)}", Geo.distanceM(fit.lat, fit.lon, lat0, lon0) < 15.0)
    }

    @Test fun needsSpreadAndSamples() {
        assertNull(Estimator.fitAp(listOf(Sample(lat0, lon0, 5.0, -60), Sample(lat0, lon0, 5.0, -61))))
        val local = Geo.Local(lat0, lon0)
        val samePlace = (0 until 10).map { val ll = local.toLatLon(1.0, 1.0); Sample(ll[0], ll[1], 5.0, -50 - it % 3) }
        val f = Estimator.fitAp(samePlace)
        assertNull("one vantage point cannot fit a position", f)
    }

    @Test fun locatesThePhoneFromKnownAps() {
        val local = Geo.Local(lat0, lon0)
        val phone = doubleArrayOf(10.0, 5.0)
        val aps = listOf(doubleArrayOf(-40.0, 0.0), doubleArrayOf(40.0, 10.0), doubleArrayOf(0.0, 45.0), doubleArrayOf(20.0, -40.0)).map { p ->
            val d = Math.hypot(p[0] - phone[0], p[1] - phone[1]); val ll = local.toLatLon(p[0], p[1])
            KnownAp(ll[0], ll[1], 8.0, rssi(d, noise = 2.0, rnd = Random(1)), -38.0, 2.7)
        }
        val fix = Estimator.locatePhone(aps)!!
        val truth = local.toLatLon(phone[0], phone[1])
        val err = Geo.distanceM(fix.lat, fix.lon, truth[0], truth[1])
        assertTrue("phone error $err m (${fix.method})", err < 15.0)
        assertEquals("wls", fix.method)
    }
}
