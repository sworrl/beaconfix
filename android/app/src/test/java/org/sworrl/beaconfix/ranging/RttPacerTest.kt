// RTT burst pacing (RttPacer, docs/RANGING.md §7 "Pacing"): fast while anything changes, one burst every 30 s once the
// distance has held for 2 min with nothing moving, fast again on movement, a BLE jump, a new distance or a ranging view.
package org.sworrl.beaconfix.ranging

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RttPacerTest {
    private val tick = 2_500L

    /** Run the loop from [from] for [ms] at one tick per 2.5 s; returns the times at which a burst went out. */
    private fun run(p: RttPacer, from: Long, ms: Long, d: (Long) -> Double? = { 0.6 }, phoneMoving: (Long) -> Boolean = { false },
                    deskMoving: (Long) -> Boolean = { false }, ble: (Long) -> List<Int> = { List(12) { -70 } }): List<Long> {
        val bursts = ArrayList<Long>()
        var t = from
        while (t < from + ms) {
            if (p.burstDue(t)) { p.burst(t); bursts += t }
            p.observe(t, d(t), phoneMoving(t), deskMoving(t), ble(t))
            t += tick
        }
        return bursts
    }

    @Test fun fastUntilTwoMinutesStableThenEveryThirtySeconds() {
        val p = RttPacer()
        val b = run(p, 0, 10 * 60_000L)
        val fast = b.filter { it < RttPacer.STABLE_MS }
        assertEquals("a burst every tick for the first 2 min", (RttPacer.STABLE_MS / tick).toInt(), fast.size)
        val slow = b.filter { it > RttPacer.STABLE_MS + tick }
        assertTrue("slow afterwards: ${slow.size} bursts in 8 min", slow.size in 15..17)
        slow.zipWithNext().forEach { (a, c) -> assertEquals(RttPacer.SLOW_MS, c - a) }
        assertTrue(p.slow)
    }

    @Test fun movementOnEitherSideReturnsToFastBursts() {
        for (side in listOf("phone", "desktop")) {
            val p = RttPacer()
            run(p, 0, 5 * 60_000L)
            assertTrue(p.slow)
            val t0 = 5 * 60_000L
            val moving = { t: Long -> t < t0 + 10_000L }
            val b = run(p, t0, 60_000L, phoneMoving = if (side == "phone") moving else { _ -> false }, deskMoving = if (side == "desktop") moving else { _ -> false })
            assertFalse("$side moving: fast", p.slow)
            assertTrue("$side moving: a burst every tick again (${b.size})", b.size >= (60_000L / tick).toInt() - 1)
            assertTrue(p.lastReason.contains(side))
        }
    }

    @Test fun aBleLevelJumpReturnsToFastBursts() {
        val p = RttPacer()
        run(p, 0, 5 * 60_000L)
        assertTrue(p.slow)
        // 4 dB: the same room (fading, a hand near the phone) — stays slow
        run(p, 5 * 60_000L, 30_000L, ble = { List(12) { -74 } })
        assertTrue("a 4 dB change is not movement", p.slow)
        // 10 dB: someone carried it off or stood in between
        val t0 = 5 * 60_000L + 30_000L
        val b = run(p, t0, 20_000L, ble = { List(12) { -84 } })
        assertFalse(p.slow)
        assertTrue(p.lastReason.contains("BLE"))
        assertTrue("fast bursts within a few ticks: ${b.size}", b.size >= 4)
    }

    @Test fun aNewDistanceRestartsTheStablePeriod() {
        val p = RttPacer()
        run(p, 0, 5 * 60_000L)
        assertTrue(p.slow)
        run(p, 5 * 60_000L, tick, d = { 0.9 })                    // within max(0.5 m, 20 %): still the same place
        assertTrue(p.slow)
        run(p, 5 * 60_000L + tick, tick, d = { 3.0 })
        assertFalse(p.slow)
        assertTrue(p.lastReason.contains("distance"))
        run(p, 5 * 60_000L + 2 * tick, RttPacer.STABLE_MS, d = { 3.0 })
        assertTrue("stable again at the new distance after 2 min", p.slow)
    }

    @Test fun openingARangingViewBoosts() {
        val p = RttPacer()
        run(p, 0, 5 * 60_000L)
        assertTrue(p.slow)
        val t0 = 5 * 60_000L
        p.boost(t0)
        assertFalse(p.slow)
        assertTrue("a burst right away", p.burstDue(t0))
        val b = run(p, t0, RttPacer.STABLE_MS)
        assertEquals("fast for 2 min after the view opened", (RttPacer.STABLE_MS / tick).toInt(), b.size)
        run(p, t0 + RttPacer.STABLE_MS, 10_000L)
        assertTrue("then slow again while nothing changes", p.slow)
    }

    @Test fun noEstimateStaysFast() {
        val p = RttPacer()
        val b = run(p, 0, 5 * 60_000L, d = { null })
        assertFalse(p.slow)
        assertEquals((5 * 60_000L / tick).toInt(), b.size)
    }
}
