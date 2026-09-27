package org.sworrl.beaconfix.wifi

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class WifiAlertsThrottleTest {
    private var now = 1_800_000_000_000L
    private val hour = 3_600_000L
    private val store = WifiAlertThrottle.MemoryStore()
    private val throttle = WifiAlertThrottle({ now }, store)

    private val cafe = CurrentWifi("Test Cafe", "02:00:00:00:00:01", "open", 2437)

    @Test fun onePerSsidPerDay() {
        assertTrue(throttle.tryAcquire("Test Cafe"))
        assertFalse(throttle.tryAcquire("Test Cafe"))
        assertTrue(throttle.tryAcquire("Test Library"))                 // another network is its own
        now += 23 * hour
        assertFalse(throttle.tryAcquire("Test Cafe"))
        now += 2 * hour
        assertTrue(throttle.tryAcquire("Test Cafe"))
    }

    @Test fun theSecondAlertWithin24hIsSuppressed() {
        val first = WifiAlerts.decide(true, cafe, emptySet(), throttle)
        assertEquals("Open network 'Test Cafe': others nearby can see unencrypted traffic. Prefer Starlink or your hotspot for banking.", first)
        now += hour
        assertNull(WifiAlerts.decide(true, cafe.copy(bssid = "02:00:00:00:00:02", frequencyMhz = 5180), emptySet(), throttle))   // another AP, same name
        now += 24 * hour
        assertNotNull(WifiAlerts.decide(true, cafe, emptySet(), throttle))
    }

    @Test fun survivesARestart() {
        assertTrue(throttle.tryAcquire("Test Cafe"))
        val again = WifiAlertThrottle({ now + hour }, store)             // a new process, same store
        assertFalse(again.tryAcquire("Test Cafe"))
        assertFalse("names are stored hashed", store.keys().any { it.contains("Test") })
    }

    @Test fun onlyBadForeignNetworksWithTheSwitchOn() {
        assertNull(WifiAlerts.decide(false, cafe, emptySet(), throttle))                                  // alerts off
        assertNull(WifiAlerts.decide(true, cafe, setOf("Test Cafe"), throttle))                           // a home network
        assertNull(WifiAlerts.decide(true, cafe.copy(security = "wpa2"), emptySet(), throttle))           // encrypted
        assertNull(WifiAlerts.decide(true, cafe.copy(security = ""), emptySet(), throttle))               // unknown
        assertNull(WifiAlerts.decide(true, cafe.copy(ssid = ""), emptySet(), throttle))                   // name hidden: can't rule out home
        assertTrue("none of those used up the day's alert", throttle.tryAcquire("Test Cafe"))
        assertNotNull(WifiAlerts.decide(true, cafe.copy(ssid = "Test Motel", security = "wpa2-tkip"), emptySet(), throttle))
    }

    @Test fun aClockThatWentBackDoesNotBlockForever() {
        assertTrue(throttle.tryAcquire("Test Cafe"))
        now -= 48 * hour
        assertTrue(throttle.tryAcquire("Test Cafe"))
    }
}
