package org.sworrl.beaconfix.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class HealthStateTest {
    private val allGood = HealthInput(sdk = 34, fine = true, coarse = true, background = true, notificationPermission = true,
        notificationsEnabled = true, nearbyWifi = true, batteryUnrestricted = true, powerSaver = false)

    @Test fun everythingAllowedHidesTheCompactCard() {
        val s = HealthState.from(allGood)
        assertTrue(s.allOk)
        assertTrue(s.problems.isEmpty())
        assertTrue(s.items.all { it.ok })
        assertEquals(listOf("precise", "background", "notifications", "nearby", "battery", "power_saver"), s.items.map { it.key })
    }

    @Test fun missingBackgroundAsksForAllTheTime() {
        val s = HealthState.from(allGood.copy(background = false))
        assertFalse(s.allOk)
        val bg = s.problems.single()
        assertEquals(HealthState.BACKGROUND, bg.key)
        assertEquals(HealthFix.REQUEST_BACKGROUND, bg.fix)
    }

    @Test fun backgroundIsOnlyRequestedAfterForeground() {
        val s = HealthState.from(allGood.copy(fine = false, coarse = false, background = false))
        assertEquals(listOf(HealthState.PRECISE), s.problems.map { it.key })
        assertEquals(HealthFix.REQUEST_FOREGROUND, s.item(HealthState.PRECISE)!!.fix)
        val bg = s.item(HealthState.BACKGROUND)!!
        assertFalse(bg.ok); assertFalse(bg.problem); assertEquals(HealthFix.NONE, bg.fix)
    }

    @Test fun approximateOnlyIsAProblem() {
        val s = HealthState.from(allGood.copy(fine = false, coarse = true))
        assertEquals(HealthFix.REQUEST_FOREGROUND, s.problems.single().fix)
    }

    @Test fun notificationsAndNearby() {
        val denied = HealthState.from(allGood.copy(notificationPermission = false, nearbyWifi = false))
        assertEquals(setOf(HealthState.NOTIFICATIONS, HealthState.NEARBY), denied.problems.map { it.key }.toSet())
        assertEquals(HealthFix.REQUEST_NOTIFICATIONS, denied.item(HealthState.NOTIFICATIONS)!!.fix)
        // permission granted but the user blocked the app's notifications: only the settings page can fix it
        val blocked = HealthState.from(allGood.copy(notificationsEnabled = false))
        assertEquals(HealthFix.OPEN_NOTIFICATION_SETTINGS, blocked.problems.single().fix)
    }

    @Test fun batteryAdviceStaysOffTheCompactCard() {
        val s = HealthState.from(allGood.copy(batteryUnrestricted = false, powerSaver = true))
        assertTrue(s.allOk)
        assertEquals(HealthFix.OPEN_BATTERY_SETTINGS, s.item(HealthState.BATTERY)!!.fix)
        assertEquals(HealthFix.OPEN_POWER_SAVER, s.item(HealthState.POWER_SAVER)!!.fix)
        assertFalse(s.item(HealthState.BATTERY)!!.ok)
    }

    @Test fun olderAndroidHasNoSeparatePermissions() {
        val s = HealthState.from(allGood.copy(sdk = 28, background = false, notificationPermission = false, nearbyWifi = false))
        assertNull(s.item(HealthState.BACKGROUND))
        assertNull(s.item(HealthState.NEARBY))
        assertTrue(s.item(HealthState.NOTIFICATIONS)!!.ok)
        assertTrue(s.allOk)
    }
}
