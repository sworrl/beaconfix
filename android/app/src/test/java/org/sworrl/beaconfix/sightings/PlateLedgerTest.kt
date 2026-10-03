package org.sworrl.beaconfix.sightings

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.api.PlateEventDto
import org.sworrl.beaconfix.data.api.PlateMediaDto
import org.sworrl.beaconfix.data.db.PlateEventEntity
import org.sworrl.beaconfix.data.db.PlateEventMediaEntity
import kotlin.math.abs

/**
 * §1.1 bookkeeping across destinations: the same event arriving from the hub and over the LAN (same uid, or the same
 * camera within ±10 min) is one row, announced once; each destination's "waiting to send" flag is its own.
 */
class PlateLedgerTest {
    /** The Room table, in a map: uid → row; passNear as the DAO's query. */
    private class MemRows : PlateRows {
        val rows = LinkedHashMap<String, PlateEventEntity>()
        val media = LinkedHashMap<String, PlateEventMediaEntity>()
        private var nextId = 1L
        override suspend fun byUid(uid: String) = rows[uid]
        override suspend fun passNear(cameraId: String, fromMs: Long, toMs: Long, atMs: Long) =
            rows.values.filter { it.kind == PlateEvents.CAMERA_PASS && it.cameraId == cameraId && it.timeMs in fromMs..toMs }.minByOrNull { abs(it.timeMs - atMs) }
        override suspend fun insert(e: PlateEventEntity): Long { require(e.uid !in rows) { "uid is unique" }; val id = nextId++; rows[e.uid] = e.copy(id = id); return id }
        override suspend fun update(e: PlateEventEntity) {
            val old = rows.entries.firstOrNull { it.value.id == e.id } ?: error("no row ${e.id}")
            rows.remove(old.key); rows[e.uid] = e
        }
        override suspend fun delete(uid: String) { rows.remove(uid) }
        override suspend fun moveMedia(from: String, to: String) { media.replaceAll { _, m -> if (m.eventUid == from) m.copy(eventUid = to) else m } }
        override suspend fun insertMedia(m: PlateEventMediaEntity): Long { media.putIfAbsent(m.uid, m); return 1 }
    }

    private val now = System.currentTimeMillis()
    private val cam = "osm:node/42"
    private val rows = MemRows()
    private val ledger = PlateLedger(rows)

    private fun local(atMs: Long = now - 60_000, source: String = PlateEvents.SRC_PHONE_LIVE) = PlateEventEntity(
        uid = PassDetector.passUid(cam, atMs), kind = PlateEvents.CAMERA_PASS, plate = "ABC-1234", time = PlateEvents.localIso(atMs), timeMs = atMs,
        lat = 40.0, lon = -75.0, cameraId = cam, cameraLat = 40.0002, cameraLon = -75.0001, distanceM = 21.0, cameraType = "alpr", source = source, confidence = 90, device = "Test phone")

    private fun dto(atMs: Long = now - 60_000, source: String = PlateEvents.SRC_BACKFILL, uid: String = PassDetector.passUid(cam, atMs), seq: Long = 17, conf: Double = 70.0,
                    media: List<PlateMediaDto>? = null) = PlateEventDto(
        uid = uid, kind = PlateEvents.CAMERA_PASS, plate = "ABC-1234", time = PlateEvents.localIso(atMs), cameraId = cam, distanceM = 48.0, cameraType = "alpr",
        operator = "Example PD", source = source, confidence = conf, leaky = JsonPrimitive(0), device = "", seq = seq, media = media)

    private fun search(uid: String = "hibf:0123456789abcdef01234567") = PlateEventDto(uid = uid, kind = PlateEvents.PLATE_SEARCH, plate = "ABC-1234",
        time = PlateEvents.localIso(now - 7_200_000), agency = "Example County SO", source = PlateEvents.SRC_HIBF, confidence = 100.0)

    @Test fun anEventFromTheHubIsNotNewAgainWhenTheLanBringsIt() = runBlocking {
        val fromHub = ledger.ingest(listOf(dto(), search()), PlateDest.HUB)
        assertEquals(2, fromHub.size)
        // later, at home: the desktop's feed has the same two (same uids) — merged, nothing new, nothing duplicated
        val fromLan = ledger.ingest(listOf(dto(seq = 90), search()), PlateDest.LAN)
        assertTrue(fromLan.isEmpty())
        assertEquals(2, rows.rows.size)
        assertEquals("the LAN feed's seq is the desktop's", 90L, rows.rows[dto().uid]!!.seq)
    }

    @Test fun theSamePassUnderAnotherUidWithinTenMinutesIsMergedNotDuplicated() = runBlocking {
        val t = now - 60_000
        assertEquals(1, ledger.ingest(listOf(dto(atMs = t, uid = PassDetector.passUid(cam, t))), PlateDest.HUB).size)
        // the desktop recorded the same pass 4 minutes off (its own uid)
        val later = ledger.ingest(listOf(dto(atMs = t + 4 * 60_000, uid = PassDetector.passUid(cam, t + 4 * 60_000), source = PlateEvents.SRC_LIVE)), PlateDest.LAN)
        assertTrue(later.isEmpty())
        assertEquals(1, rows.rows.size)
        assertEquals("the desktop's uid survives", PassDetector.passUid(cam, t + 4 * 60_000), rows.rows.keys.single())
        // 11 minutes away: another pass
        assertEquals(1, ledger.ingest(listOf(dto(atMs = t + 15 * 60_000)), PlateDest.HUB).size)
        assertEquals(2, rows.rows.size)
    }

    @Test fun eachDestinationClearsOnlyItsOwnFlag() = runBlocking {
        val mine = ledger.upsertLocal(local())
        assertTrue(mine.dirty); assertTrue(mine.hubDirty)
        // the hub sends our own pass back (same rank): the hub has it; the desktop does not yet
        assertTrue(ledger.ingest(listOf(dto(source = PlateEvents.SRC_PHONE_LIVE, conf = 90.0).copy(device = "Test phone")), PlateDest.HUB).isEmpty())
        rows.rows[mine.uid]!!.let { assertFalse(it.hubDirty); assertTrue("still waiting for the desktop (and its images)", it.dirty); assertEquals("the hub's seq is not the desktop's", 0L, it.seq) }
        // then the desktop's feed: now nothing waits
        ledger.ingest(listOf(dto(source = PlateEvents.SRC_PHONE_LIVE, conf = 90.0)), PlateDest.LAN)
        rows.rows[mine.uid]!!.let { assertFalse(it.dirty); assertFalse(it.hubDirty) }
    }

    @Test fun aBackfillFromTheHubDoesNotClearALiveRecordsFlag() = runBlocking {
        val mine = ledger.upsertLocal(local())
        ledger.ingest(listOf(dto(source = PlateEvents.SRC_BACKFILL)), PlateDest.HUB)
        val row = rows.rows[mine.uid]!!
        assertTrue("ours ranks higher: the hub still needs it", row.hubDirty); assertTrue(row.dirty)
        assertEquals(PlateEvents.SRC_PHONE_LIVE, row.source); assertEquals(21.0, row.distanceM!!, 0.0)
        assertEquals("gaps filled from the hub's copy", "Example PD", row.operator)
    }

    @Test fun imagesAreListedFromTheDesktopOnly() = runBlocking {
        val photo = PlateMediaDto(uid = "0123456789abcdef0123456789abcdef", kind = "camera_photo", mime = "image/jxl", cameraId = cam)
        ledger.ingest(listOf(dto(media = listOf(photo))), PlateDest.HUB)
        assertTrue(rows.media.isEmpty())
        ledger.ingest(listOf(dto(media = listOf(photo))), PlateDest.LAN)
        assertEquals(1, rows.media.size)
    }

    @Test fun aRenameKeepsBothFlagsAndTheNotification() = runBlocking {
        val mine = ledger.upsertLocal(local())
        rows.update(rows.rows[mine.uid]!!.copy(notified = true))
        val other = ledger.ingest(listOf(dto(atMs = now - 60 * 60_000, uid = "pass:$cam:1")), PlateDest.HUB).single()
        assertFalse(other.dirty || other.hubDirty)
        ledger.rename(mine.uid, other.uid)
        val r = rows.rows.values.single { it.uid == other.uid }
        assertTrue(r.dirty); assertTrue(r.hubDirty); assertTrue(r.notified)
        assertFalse(mine.uid in rows.rows)
    }

    @Test fun hubFreshEventsFollowTheLanAlertRules() = runBlocking {
        // away from home, the first hub pull brings a live pass from an hour ago, a search, and a big backfill
        val list = listOf(dto(atMs = now - 3_600_000, source = PlateEvents.SRC_LIVE, uid = "pass:$cam:live"), search()) +
            (1..30).map { i -> dto(atMs = now - i * 86_400_000L, uid = "pass:osm:node/$i:1").copy(cameraId = "osm:node/$i") }
        val fresh = ledger.ingest(list, PlateDest.HUB)
        assertEquals(32, fresh.size)
        val plan = Sightings.plan(fresh, now)
        assertEquals(listOf("pass:$cam:live", "hibf:0123456789abcdef01234567"), plan.single.map { it.uid })
        assertEquals("the backfill is one summary, not a flood", 30, plan.summaryPasses)
        // the same events over the LAN later: nothing to announce
        assertTrue(ledger.ingest(list.map { it.copy(seq = 99) }, PlateDest.LAN).isEmpty())
    }

    @Test fun settleIsPerDestination() {
        val old = local().copy(dirty = true, hubDirty = true, seq = 5)
        val incoming = PlateEvents.fromDto(dto(source = PlateEvents.SRC_PHONE_LIVE, conf = 90.0, seq = 40))
        PlateLedger.settle(old, incoming, PlateDest.HUB).let { assertFalse(it.hubDirty); assertTrue(it.dirty); assertEquals(5L, it.seq) }
        PlateLedger.settle(old, incoming, PlateDest.LAN).let { assertTrue(it.hubDirty); assertFalse(it.dirty); assertEquals(40L, it.seq) }
    }
}
