// Wi-Fi RTT to ordinary APs (ranging/ApRtt.kt): burst combination, offset correction, selection / batching / cooldown,
// and the observation JSON the phone pushes (rangeM / rangeSd only when the AP answered).
package org.sworrl.beaconfix.ranging

import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.ObservationDto
import org.sworrl.beaconfix.data.db.ObservationEntity
import org.sworrl.beaconfix.estimate.EstimateRepository
import org.sworrl.beaconfix.sync.SyncRepository

class ApRttTest {
    private fun b(mm: Int, std: Int = 400, n: Int = 8) = ApBurst(mm, std, n)

    // ── combination ──
    @Test fun medianOfBurstsIgnoresALateOutlier() {
        val r = ApRttMath.combine("AA", listOf(b(7200), b(7400), b(7300), b(29_000)), 1)!!
        assertEquals(7.35, r.rangeM, 1e-9)                  // median of 4: (7.3 + 7.4) / 2
        assertEquals(4, r.bursts)
        assertTrue("σ stays near the cluster's, not the outlier's: ${r.rangeSd}", r.rangeSd < 1.0)
    }

    @Test fun burstsWithFewerThanTwoGoodMeasurementsAreDropped() {
        assertNull(ApRttMath.combine("AA", listOf(b(5000, n = 1), b(6000, n = 0)), 1))
        val r = ApRttMath.combine("AA", listOf(b(5000, n = 1), b(6000, n = 2)), 1)!!
        assertEquals(6.0, r.rangeM, 1e-9); assertEquals(1, r.bursts)
    }

    @Test fun singleBurstSigmaIsItsOwnStdOverRootNFloored() {
        // 2.4 m spread over 4 measurements → 1.2 m for the burst's mean
        assertEquals(1.2, ApRttMath.combine("AA", listOf(b(10_000, std = 2400, n = 4)), 1)!!.rangeSd, 1e-9)
        // a tiny reported σ is floored at 0.3 m
        assertEquals(ApRttMath.FLOOR_M, ApRttMath.combine("AA", listOf(b(10_000, std = 10, n = 8)), 1)!!.rangeSd, 1e-9)
    }

    @Test fun spreadAcrossBurstsWidensSigma() {
        val tight = ApRttMath.combine("AA", listOf(b(10_000, 300), b(10_050, 300), b(10_100, 300)), 1)!!
        val loose = ApRttMath.combine("AA", listOf(b(8_000, 300), b(10_000, 300), b(13_000, 300)), 1)!!
        assertEquals(ApRttMath.FLOOR_M, tight.rangeSd, 1e-9)
        // 1.4826·MAD = 1.4826·2 m, ×1.2533/√3 for the median
        assertEquals(ApRttMath.round2(1.2533 * 1.4826 * 2.0 / Math.sqrt(3.0)), loose.rangeSd, 1e-9)
        assertTrue(loose.rangeSd > tight.rangeSd)
        // two bursts: |d1 − d2|/√2
        val two = ApRttMath.combine("AA", listOf(b(10_000, 100), b(14_000, 100)), 1)!!
        assertEquals(12.0, two.rangeM, 1e-9)
        assertEquals(ApRttMath.round2(1.2533 * 4.0 / Math.sqrt(2.0) / Math.sqrt(2.0)), two.rangeSd, 1e-9)
    }

    // ── offset ──
    @Test fun offsetIsSubtractedAndItsSigmaAddedInQuadrature() {
        val r = ApRttMath.combine("AA", listOf(b(7_800, std = 2400, n = 4)), 1, offsetM = 0.5, offsetSd = 0.9)!!
        assertEquals(7.3, r.rangeM, 1e-9)
        assertEquals(1.5, r.rangeSd, 1e-9)              // √(1.2² + 0.9²)
    }

    @Test fun negativeCorrectedRangeIsClampedAndWidened() {
        val r = ApRttMath.combine("AA", listOf(b(1_000, std = 100)), 1, offsetM = 2.5)!!
        assertEquals(0.1, r.rangeM, 1e-9)
        assertTrue(r.rangeSd >= 1.6 - 1e-9)             // covers the 1.6 m it was pushed by
    }

    @Test fun onlyAPlausibleFreshOffsetIsReused() {
        assertEquals(0.0, ApRttMath.usableOffset(null), 0.0)
        assertEquals(0.55, ApRttMath.usableOffset(0.55), 0.0)
        assertEquals(-1.2, ApRttMath.usableOffset(-1.2), 0.0)
        assertEquals("stale", 0.0, ApRttMath.usableOffset(0.55, stale = true), 0.0)
        // the Pixel ↔ AX210 soft-AP pair offset (~14 m, docs/RANGING.md §8) is the responder's, not the phone's
        assertEquals(0.0, ApRttMath.usableOffset(13.95), 0.0)
        assertEquals(0.0, ApRttMath.usableOffset(Double.NaN), 0.0)
    }

    @Test fun text() {
        assertEquals("RTT 7.3 m ± 0.9", ApRange("AA", 7.3, 0.9, 3, 0).text)
    }

    // ── selection, batching, cooldown ──
    private fun c(i: Int, dbm: Int) = RttCandidate("02:00:00:00:00:%02X".format(i), dbm)

    @Test fun strongestFirstCappedPerScan() {
        val p = ApRttPlanner(maxPerScan = 3)
        val got = p.select(listOf(c(1, -80), c(2, -50), c(3, -70), c(4, -60), c(2, -50)), 0, 40.0, -75.0)
        assertEquals(listOf(c(2, -50), c(4, -60), c(3, -70)).map { it.bssid }, got.map { it.bssid })
    }

    @Test fun batchesRespectMaxPeers() {
        assertEquals(listOf(10, 10, 3), ApRttPlanner.batches((1..23).toList(), 10).map { it.size })
        assertEquals(listOf(1, 1), ApRttPlanner.batches(listOf(1, 2), 0).map { it.size })
        assertTrue(ApRttPlanner.batches(emptyList<Int>(), 10).isEmpty())
    }

    @Test fun cooldownAtTheSamePlaceButNotAfterMoving() {
        val p = ApRttPlanner()
        val a = c(1, -60)
        p.mark(a.bssid, 0, 40.0, -75.0, answered = true)
        assertTrue(p.select(listOf(a), 10_000, 40.0, -75.0).isEmpty())                 // 10 s later, same spot
        assertTrue(p.select(listOf(a), 10_000, 40.00005, -75.0).isEmpty())             // 5.6 m away: still the same spot
        assertEquals(1, p.select(listOf(a), 10_000, 40.0005, -75.0).size)              // 56 m away: new geometry
        assertTrue(p.select(listOf(a), 10_000, null, null).isEmpty())                  // unknown position counts as the same spot
        assertEquals(1, p.select(listOf(a), ApRttPlanner.COOLDOWN_MS, 40.0, -75.0).size)
    }

    @Test fun silentApsBackOffWhereverWeAre() {
        val p = ApRttPlanner()
        val a = c(1, -60)
        p.mark(a.bssid, 0, 40.0, -75.0, answered = false)
        assertEquals("one miss: ordinary cooldown", 1, p.select(listOf(a), 30_000, 40.0, -75.0).size)
        p.mark(a.bssid, 30_000, 40.0, -75.0, answered = false)
        assertEquals(2, p.fails(a.bssid))
        assertTrue("two misses: 60 s even after moving", p.select(listOf(a), 60_000, 41.0, -75.0).isEmpty())
        assertEquals(1, p.select(listOf(a), 90_000, 40.0, -75.0).size)
        var t = 90_000L
        repeat(10) { p.mark(a.bssid, t, 40.0, -75.0, answered = false) }
        assertTrue(p.select(listOf(a), t + ApRttPlanner.MAX_BACKOFF_MS - 1, 40.0, -75.0).isEmpty())
        assertEquals("capped at 8 min", 1, p.select(listOf(a), t + ApRttPlanner.MAX_BACKOFF_MS, 40.0, -75.0).size)
        t += ApRttPlanner.MAX_BACKOFF_MS
        p.mark(a.bssid, t, 40.0, -75.0, answered = true)
        assertEquals("an answer resets it", 0, p.fails(a.bssid))
    }

    // ── storage → estimator, and the wire ──
    private val obs = ObservationEntity(bssid = "02:00:00:00:00:01", time = 1_700_000_000_000L, lat = 40.0, lon = -75.0, acc = 8.0, dbm = -61, source = "phone-gps")

    @Test fun estimatorObsCarriesTheRange() {
        val o = EstimateRepository.obsOf(obs.copy(rangeM = 7.3, rangeSd = 0.9))
        assertEquals(7.3, o.rangeM, 0.0); assertEquals(0.9, o.rangeSd, 0.0)
        assertEquals(-1.0, EstimateRepository.obsOf(obs).rangeM, 0.0)
    }

    @Test fun observationJsonHasRangeOnlyWhenMeasured() {
        val json = ApiFactory.json
        val plain = json.parseToJsonElement(json.encodeToString(SyncRepository.dtoOf(obs, "id"))).jsonObject
        assertFalse("rangeM" in plain); assertFalse("rangeSd" in plain)
        val ranged = json.parseToJsonElement(json.encodeToString(SyncRepository.dtoOf(obs.copy(rangeM = 7.3, rangeSd = 0.9), "id"))).jsonObject
        assertEquals(7.3, ranged["rangeM"]!!.jsonPrimitive.content.toDouble(), 0.0)
        assertEquals(0.9, ranged["rangeSd"]!!.jsonPrimitive.content.toDouble(), 0.0)
        assertEquals(-61, ranged["dbm"]!!.jsonPrimitive.content.toInt())
        // an older desktop's reply / a row without the keys still decodes
        val back = json.decodeFromString<ObservationDto>("""{"bssid":"02:00:00:00:00:01","dbm":-61,"lat":40.0,"lon":-75.0,"acc":8.0,"time":"2023-11-14T22:13:20"}""")
        assertNull(back.rangeM)
    }

    @Test fun rangedRowsCountLargerForTheFourKilobyteLimit() {
        val plain = SyncRepository.sizeLimited(List(100) { obs.copy(id = it.toLong()) }).size
        val ranged = SyncRepository.sizeLimited(List(100) { obs.copy(id = it.toLong(), rangeM = 7.3, rangeSd = 0.9) }).size
        assertTrue("$ranged < $plain", ranged < plain)
    }
}
