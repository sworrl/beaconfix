package org.sworrl.beaconfix.alpr

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.sworrl.beaconfix.alpr.core.FrameMeta
import org.sworrl.beaconfix.alpr.core.PendingEvent
import org.sworrl.beaconfix.alpr.core.SpillQueue
import java.io.File

class SpillQueueTest {
    // under the module's build dir (never /tmp)
    private val dir = File("build/alpr-test/spill-${System.nanoTime()}")
    private lateinit var q: SpillQueue

    @Before fun setUp() { q = SpillQueue(dir) }
    @After fun tearDown() { dir.deleteRecursively() }

    private fun ev(id: String, at: Long, hot: Boolean = false, size: Int = 10_000) =
        PendingEvent(id, at, FrameMeta(capturedAt = "2026-10-02T12:00:00.000-05:00", reason = if (hot) FrameMeta.REASON_HOTLIST else FrameMeta.REASON_PLATE), ByteArray(size) { 7 })

    @Test fun putListLoadRemove() {
        q.put(ev("a", 1000)); q.put(ev("b", 2000, hot = true)); q.put(ev("c", 1500))
        assertEquals(listOf("b", "a", "c"), q.list().map { it.id })          // hotlist first, then oldest
        val loaded = q.load(q.list().first())!!
        assertEquals("b", loaded.id); assertTrue(loaded.hotlist); assertEquals(10_000, loaded.jpeg.size)
        q.remove("b")
        assertEquals(listOf("a", "c"), q.list().map { it.id })
        assertEquals(2, q.count())
        assertTrue(q.totalBytes() > 20_000)
    }

    @Test fun capDropsOldestOrdinaryAndKeepsHotlist() {
        q.put(ev("h-old", 100, hot = true)); q.put(ev("p1", 200)); q.put(ev("p2", 300)); q.put(ev("p3", 400))
        val cap = 2 * 10_500L + 1_000                                              // room for ~2 events
        val dropped = q.enforce(cap, Long.MAX_VALUE, 1_000)
        assertEquals(2, dropped)
        assertEquals(setOf("h-old", "p3"), q.list().map { it.id }.toSet())
        assertTrue(q.totalBytes() <= cap)
    }

    @Test fun hotlistIsKeptEvenOverTheCap() {
        q.put(ev("h1", 100, hot = true)); q.put(ev("h2", 200, hot = true)); q.put(ev("p", 300))
        q.enforce(5_000, Long.MAX_VALUE, 1_000)
        assertEquals(setOf("h1", "h2"), q.list().map { it.id }.toSet())
    }

    @Test fun ordinaryEventsExpireHotlistDoesNot() {
        val day = 24 * 3600_000L
        q.put(ev("old", 0)); q.put(ev("hot", 0, hot = true)); q.put(ev("new", day))
        assertEquals(1, q.enforce(Long.MAX_VALUE, day, day + 1))
        assertEquals(setOf("hot", "new"), q.list().map { it.id }.toSet())
    }

    @Test fun halfWrittenEventsAreCleanedUp() {
        q.put(ev("ok", 1))
        File(dir, "5_orphan_p.jpg").writeBytes(ByteArray(10))
        File(dir, "6_tmp_p.json.tmp").writeText("{")
        SpillQueue(dir)
        assertEquals(listOf("ok"), q.list().map { it.id })
        assertFalse(File(dir, "5_orphan_p.jpg").exists()); assertFalse(File(dir, "6_tmp_p.json.tmp").exists())
    }

    @Test fun clearEmptiesTheDirectory() {
        q.put(ev("a", 1)); q.clear()
        assertEquals(0, q.count()); assertEquals(0L, q.totalBytes())
    }
}
