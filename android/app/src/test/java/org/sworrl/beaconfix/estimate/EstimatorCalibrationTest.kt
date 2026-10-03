package org.sworrl.beaconfix.estimate

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.EstimatorDto
import kotlin.math.sqrt

/** The desktop's calibration (GET /api/v1/estimator) as this phone's estimator options and context, and the scan-cell misses. */
class EstimatorCalibrationTest {
    private val answer = """{"version":4,"kappa":1.3,"calibration":{"anchors":3},"deviceOffsets":{"Google Pixel 10 Pro XL":5.5,"Steam Deck":-3.0,"tablet":2.0},
        "environment":{"2.4":{"p0":-41.0,"n":2.9,"S":[4.0,0.1,0.04],"samples":7,"sigmaP0":2.0,"sigmaN":0.2},"5":{"p0":-45.0,"n":5.2,"S":[4.0,0.1,0.25],"samples":5},
        "6":{"p0":-48.0,"n":3.1,"S":[4.0,0.1,0.01],"samples":2}},"grades":{"A":3}}"""

    private fun dto(): EstimatorDto = ApiFactory.json.decodeFromString(EstimatorDto.serializer(), answer)

    @Test fun parsesTheDesktopAnswer() {
        val d = dto()
        assertEquals(1.3, d.kappa, 0.0)
        assertEquals(5.5, d.deviceOffsets.getValue("Google Pixel 10 Pro XL"), 0.0)
        assertEquals(listOf(4.0, 0.1, 0.04), d.environment.getValue("2.4").s)
        assertEquals(7, d.environment.getValue("2.4").samples)
    }

    @Test fun optionsLikeTheDesktop() {
        val cal = EstimatorCalibration.fromDesktop(dto(), listOf("Google Pixel 10 Pro XL"), 1L)
        val o24 = EstimatorCalibration.optionsFor(2437, cal)
        assertEquals(-40.0, o24.p0Mean, 0.0); assertEquals(2.9, o24.defaultN, 0.0)
        assertEquals(sqrt(0.04) + 0.3, o24.nSd, 1e-12); assertEquals(1.3, o24.kappa, 0.0)
        val o5 = EstimatorCalibration.optionsFor(5180, cal)
        assertEquals(-47.0, o5.p0Mean, 0.0); assertEquals(4.5, o5.defaultN, 0.0); assertEquals(0.6, o5.nSd, 0.0)   // n and σ clamped
        val o6 = EstimatorCalibration.optionsFor(5975, cal)
        assertEquals(2.7, o6.defaultN, 0.0); assertEquals(0.5, o6.nSd, 0.0)        // two samples: the prior stays
        val o0 = EstimatorCalibration.optionsFor(0, cal)
        assertEquals(-42.0, o0.p0Mean, 0.0); assertEquals(2.5, o0.defaultN, 0.0); assertEquals(1.3, o0.kappa, 0.0)
        assertEquals(1.0, EstimatorCalibration.optionsFor(2437, null).kappa, 0.0)
    }

    @Test fun offsetsInThisPhonesFrame() {
        val cal = EstimatorCalibration.fromDesktop(dto(), listOf("Google Pixel 10 Pro XL", "pixel-token"), 1L)
        assertEquals("Google Pixel 10 Pro XL", cal.phoneName); assertEquals(5.5, cal.phoneOffsetDb, 0.0)
        val m = cal.deviceOffsets()
        assertEquals(-5.5, m.getValue(EstimatorCalibration.DESKTOP), 0.0)        // the desktop hears 5.5 dB quieter than this phone
        assertEquals(-8.5, m.getValue("Steam Deck"), 0.0)
        assertEquals(-3.5, m.getValue("tablet"), 0.0)
        assertTrue("this phone is the reference", "" !in m && "Google Pixel 10 Pro XL" !in m)
        // the token's name when the device name is unknown; neither → 0
        val byToken = EstimatorCalibration.fromDesktop(dto(), listOf("Other Phone", "tablet"), 1L)
        assertEquals(2.0, byToken.phoneOffsetDb, 0.0)
        val unknown = EstimatorCalibration.fromDesktop(dto(), listOf("Other Phone", ""), 1L)
        assertEquals(0.0, unknown.phoneOffsetDb, 0.0); assertTrue(EstimatorCalibration.DESKTOP !in unknown.deviceOffsets())
        // round trip through Prefs
        assertEquals(cal, EstimatorCalibration.decode(cal.encode()))
        assertNull(EstimatorCalibration.decode(""))
    }

    @Test fun scanCellsGiveTheMisses() {
        val lat0 = 40.0030; val lon0 = -75.0680
        val mLat = 1 / 111320.0
        val cells = ScanCells()
        // scans along a street 0–300 m north; the AP was heard only at the first two
        for (i in 0 until 11) cells.note(lat0 + i * 30 * mLat, lon0, 8.0, 1_790_000_000L + i * 60)
        cells.note(lat0 + 500 * mLat, lon0, 120.0, 1_790_000_000L)                          // a coarse fix: no cell
        assertEquals(11, cells.size)
        val heard = listOf(Obs(lat0, lon0, 8.0, -50, 1_790_000_000L), Obs(lat0 + 30 * mLat, lon0, 8.0, -60, 1_790_000_060L))
        val misses = cells.missesFor(heard)
        assertEquals(9, misses.size)
        assertTrue("nearest first", misses.first().lat < misses.last().lat)
        // the desktop's rows alone say nothing about where we scanned
        assertTrue(cells.missesFor(heard.map { it.copy(device = EstimatorCalibration.DESKTOP) }).isEmpty())
        // scans long after the AP was last heard are not misses
        val old = heard.map { it.copy(t = it.t - 60L * 86400L) }
        assertTrue(cells.missesFor(old).isEmpty())
    }
}
