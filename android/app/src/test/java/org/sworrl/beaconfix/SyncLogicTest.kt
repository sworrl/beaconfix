package org.sworrl.beaconfix

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.collector.ObservationRecorder
import org.sworrl.beaconfix.data.db.ObservationEntity
import org.sworrl.beaconfix.estimate.ScanSample
import org.sworrl.beaconfix.sync.SyncRepository

class SyncLogicTest {
    private fun obs(i: Int) = ObservationEntity(id = i.toLong(), bssid = "AA:BB:CC:DD:EE:%02X".format(i), time = 1_700_000_000_000L + i, lat = 39.7, lon = -80.07, acc = 8.0, dbm = -60, source = "phone-gps")

    @Test fun pushBatchesStayUnderTheDesktopBodyLimit() {
        val rows = (0 until 200).map { obs(it) }
        val chunk = SyncRepository.sizeLimited(rows)
        assertTrue("chunk ${chunk.size}", chunk.size in 20..30)
        val bytes = chunk.size * 120 + 30
        assertTrue("bytes $bytes", bytes <= 3600)
        assertEquals(1, SyncRepository.sizeLimited(rows.take(1)).size)
    }

    @Test fun isoRoundTrip() {
        val t = 1_700_000_000_000L
        val s = SyncRepository.iso(t)
        assertEquals(19, s.length)
        assertTrue(Math.abs(SyncRepository.parseIso(s) - t) < 1000)
    }

    @Test fun securityVocabularyMatchesTheDesktop() {
        assertEquals("open", ObservationRecorder.securityOf("[ESS]"))
        assertEquals("wep", ObservationRecorder.securityOf("[WEP][ESS]"))
        assertEquals("wpa1", ObservationRecorder.securityOf("[WPA-PSK-TKIP][ESS]"))
        assertEquals("wpa2-tkip", ObservationRecorder.securityOf("[WPA2-PSK-CCMP+TKIP][RSN-PSK-CCMP+TKIP][ESS]"))
        assertEquals("wpa2", ObservationRecorder.securityOf("[WPA2-PSK-CCMP][RSN-PSK-CCMP][ESS]"))
        assertEquals("wpa2/3", ObservationRecorder.securityOf("[WPA2-PSK+SAE-CCMP][RSN-PSK+SAE-CCMP][ESS][MFPC]"))
        assertEquals("wpa3", ObservationRecorder.securityOf("[RSN-SAE-CCMP][ESS][MFPR][MFPC]"))
        assertEquals("owe", ObservationRecorder.securityOf("[RSN-OWE-CCMP][ESS]"))
        assertEquals("wpa2-eap", ObservationRecorder.securityOf("[WPA2-EAP/SHA1-CCMP][RSN-EAP/SHA1-CCMP][ESS]"))
    }

    @Test fun homeGlobsMatchSsidAndBssid() {
        val s = ScanSample("AA:BB:CC:1A:2B:3C", "MyRouter 5G", -50, 5180)
        assertTrue(ObservationRecorder.isHome(s, setOf("MyRouter*")))
        assertTrue(ObservationRecorder.isHome(s, setOf("AA:BB:CC:?A:2B:3?")))
        assertFalse(ObservationRecorder.isHome(s, setOf("Other*", "DD:EE:*")))
        assertFalse(ObservationRecorder.isHome(s, emptySet()))
    }

    @Test fun channelsAndBands() {
        assertEquals(6, ObservationRecorder.channelOf(2437)); assertEquals("2.4", ObservationRecorder.bandOf(2437))
        assertEquals(48, ObservationRecorder.channelOf(5240)); assertEquals("5", ObservationRecorder.bandOf(5240))
        assertEquals(37, ObservationRecorder.channelOf(6135)); assertEquals("6", ObservationRecorder.bandOf(6135))
    }
}
