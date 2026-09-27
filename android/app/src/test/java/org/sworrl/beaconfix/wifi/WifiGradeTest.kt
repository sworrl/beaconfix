package org.sworrl.beaconfix.wifi

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.ui.SecurityText
import org.sworrl.beaconfix.wifi.WifiGrade.Level

class WifiGradeTest {
    @Test fun openIsNotEncrypted() {
        val v = WifiGrade.grade("open")
        assertEquals("Open — not encrypted", v.label)
        assertEquals(Level.BAD, v.level)
        assertFalse(v.home)
        // the long explanation is SecurityText's, word for word
        assertEquals(SecurityText.byTitle("No RSN/WPA IE, privacy bit clear")!!.nerd, v.detail)
    }

    @Test fun badOkAndUnknown() {
        for (s in listOf("open", "wep", "wpa1", "wpa2-tkip")) assertEquals(s, Level.BAD, WifiGrade.grade(s).level)
        for (s in listOf("owe", "wpa2", "wpa2-eap", "wpa2/3", "wpa3", "wpa3-eap192")) assertEquals(s, Level.OK, WifiGrade.grade(s).level)
        assertEquals(Level.UNKNOWN, WifiGrade.grade("").level)
        assertEquals("Security not reported", WifiGrade.grade("").label)
        for (s in listOf("open", "wep", "wpa1", "wpa2-tkip", "owe", "wpa2", "wpa2-eap", "wpa2/3", "wpa3", "wpa3-eap192"))
            assertTrue(s, WifiGrade.grade(s).detail.isNotEmpty())
    }

    @Test fun aHomeNetworkIsOk() {
        val v = WifiGrade.grade("open", home = true)
        assertEquals(Level.OK, v.level); assertTrue(v.home); assertEquals("Open — not encrypted", v.label)
        assertTrue(WifiGrade.isHome("Test-RV-5G", "02:00:00:00:00:01", setOf("Test-RV*")))
        assertTrue(WifiGrade.isHome("", "02:00:00:00:00:01", setOf("02:00:00:00:00:0?")))
        assertFalse(WifiGrade.isHome("Test Cafe", "02:00:00:00:00:02", setOf("Test-RV*")))
        assertFalse(WifiGrade.isHome("", "", setOf("*")))                     // nothing known, nothing matched
    }

    @Test fun securityFromTheConnection() {
        assertEquals("open", WifiGrade.security(WifiGrade.TYPE_OPEN, null))
        assertEquals("wep", WifiGrade.security(WifiGrade.TYPE_WEP, null))
        assertEquals("owe", WifiGrade.security(WifiGrade.TYPE_OWE, null))
        assertEquals("wpa3", WifiGrade.security(WifiGrade.TYPE_SAE, "[RSN-SAE+PSK-CCMP][ESS]"))
        assertEquals("wpa2-eap", WifiGrade.security(WifiGrade.TYPE_EAP, null))
        assertEquals("wpa3-eap192", WifiGrade.security(WifiGrade.TYPE_EAP_WPA3_ENTERPRISE_192_BIT, null))
        // PSK alone can't tell WPA1 / TKIP apart: the scan result refines it
        assertEquals("wpa2", WifiGrade.security(WifiGrade.TYPE_PSK, null))
        assertEquals("wpa2", WifiGrade.security(WifiGrade.TYPE_PSK, "[RSN-PSK-CCMP][ESS]"))
        assertEquals("wpa1", WifiGrade.security(WifiGrade.TYPE_PSK, "[WPA-PSK-TKIP][ESS]"))
        assertEquals("wpa2-tkip", WifiGrade.security(WifiGrade.TYPE_PSK, "[RSN-PSK-TKIP][ESS]"))
        assertEquals("wpa2", WifiGrade.security(WifiGrade.TYPE_PSK, "[RSN-SAE+PSK-CCMP][ESS]"))   // joined with PSK on a transition BSS
        // before API 31 (no type) the scan result decides
        assertEquals("open", WifiGrade.security(null, "[ESS]"))
        assertEquals("wpa2", WifiGrade.security(null, "[WPA2-PSK-CCMP][ESS]"))
        assertEquals("", WifiGrade.security(null, null))
        assertEquals("", WifiGrade.security(WifiGrade.TYPE_UNKNOWN, ""))
    }

    @Test fun ssidCleanup() {
        assertEquals("Test Net", WifiGrade.cleanSsid("\"Test Net\""))
        assertEquals("", WifiGrade.cleanSsid("<unknown ssid>"))
        assertEquals("", WifiGrade.cleanSsid(null))
        assertEquals("0x74657374", WifiGrade.cleanSsid("0x74657374"))
    }

    @Test fun alertText() {
        assertEquals("Open network 'Test Cafe': others nearby can see unencrypted traffic. Prefer Starlink or your hotspot for banking.",
            WifiGrade.alertText("Test Cafe", "open"))
        assertEquals("Weakly encrypted network 'Test Motel' (WEP): others nearby can break its encryption. Prefer Starlink or your hotspot for banking.",
            WifiGrade.alertText("Test Motel", "wep"))
        assertNull(WifiGrade.alertText("Test Net", "wpa2"))
        assertNull(WifiGrade.alertText("Test Net", ""))
    }
}
