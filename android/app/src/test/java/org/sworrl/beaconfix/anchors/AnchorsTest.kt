package org.sworrl.beaconfix.anchors

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.api.AnchorDto
import org.sworrl.beaconfix.estimate.ScanSample

class AnchorsTest {
    /** One router, several radios: the BSSIDs differ in the first and last octet only → one group (docs/RANGING.md §5.4). */
    @Test fun groupsRadiosOfOneBox() {
        val scan = listOf(
            ScanSample("06:11:22:33:44:5B", "RVNet", -50, 5240), ScanSample("04:11:22:33:44:5B", "RVNet", -45, 2437), ScanSample("06:11:22:33:44:5C", "RVNet-guest", -52, 5240),
            ScanSample("AA:BB:CC:DD:EE:01", "Cafe", -70, 2412), ScanSample("AA:BB:CC:DD:EE:02", "Cafe-5G", -75, 5180),
            ScanSample("11:22:33:44:55:66", "", -80, 2462),
        )
        val g = AnchorRepository.groups(scan)
        assertEquals(3, g.size)
        assertEquals(listOf("04:11:22:33:44:5B", "06:11:22:33:44:5B", "06:11:22:33:44:5C"), g[0].bssids.sorted())
        assertEquals("RVNet", g[0].title); assertTrue(g[0].subtitle.contains("3 radios")); assertEquals(listOf("2.4", "5"), g[0].bands)
        assertEquals("(hidden)", g[2].title)
    }
    @Test fun normaliseUpperCasesAndClamps() {
        val a = AnchorRepository.normalise(AnchorDto(name = "  ", kind = "", lat = 1.0, lon = 2.0, accM = 0.0, bssids = listOf("aa:bb:cc:dd:ee:ff", "AA:BB:CC:DD:EE:FF", "bad")))
        assertEquals("custom", a.kind); assertEquals("Custom", a.name); assertEquals(0.05, a.accM, 1e-9); assertEquals(listOf("AA:BB:CC:DD:EE:FF"), a.bssids)
    }
    @Test fun jsonRoundTripKeepsTheContractFields() {
        val a = AnchorDto(id = "0b8f", name = "AX210 antenna", kind = "this-computer", lat = 40.00293, lon = -75.06806, alt = 120.0, heightM = 1.1, floor = 0, accM = 1.0, bssids = listOf("02:00:00:00:00:01"), rv = true, ref = true, headingDeg = 212.0, placedAt = "2026-09-27T03:10:00Z", source = "map-pick")
        val s = AnchorRepository.encode(a)
        for (k in listOf("\"rv\":true", "\"ref\":true", "\"accM\":1.0", "\"kind\":\"this-computer\"", "\"placedBy\":\"android\"", "\"source\":\"map-pick\"")) assertTrue("missing $k in $s", s.contains(k))
        assertTrue(!s.contains("rvOffset")); assertTrue(!s.contains("\"ble\""))          // nulls are omitted, not sent as null
        assertEquals(a, AnchorRepository.decode(s))
    }
}
