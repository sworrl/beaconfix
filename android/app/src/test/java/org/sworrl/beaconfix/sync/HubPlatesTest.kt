package org.sworrl.beaconfix.sync

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.ChangesDto
import org.sworrl.beaconfix.data.api.FixDto
import org.sworrl.beaconfix.data.api.PlateEventsPage
import org.sworrl.beaconfix.data.db.PlateEventEntity
import org.sworrl.beaconfix.net.HubSyncBody
import org.sworrl.beaconfix.sightings.PlateEvents

/** Plate events over the hub: the `db/sync` body and answer, the `db/changes` rows, and the plate cursor. No network. */
class HubPlatesTest {
    private fun obj(s: String) = Json.parseToJsonElement(s).jsonObject
    private val ev = PlateEventEntity(uid = "pass:osm:node/42:29333332", kind = PlateEvents.CAMERA_PASS, plate = "ABC-1234", time = "2025-10-09T08:53:20", timeMs = 1_760_000_000_000,
        lat = 40.0, lon = -75.0, acc = 5.0, cameraId = "osm:node/42", distanceM = 21.0, facing = 1, cameraType = "alpr", source = PlateEvents.SRC_PHONE_LIVE, confidence = 90,
        metrics = """{"dwellS":4.2,"fixes":3}""", raw = """{"camera":{"id":"osm:node/42"}}""", device = "Test phone", dirty = true, hubDirty = true)

    @Test fun thePushIsARecordOnlyDbSyncBody() {
        val body = HubPlates.body("Test phone", "id-test", listOf(PlateEvents.toDto(ev)))
        val o = obj(ApiFactory.json.encodeToString(HubSyncBody.serializer(), body))
        assertEquals(setOf("device", "kind", "identity", "observations", "fixes", "plateEvents"), o.keys)
        assertEquals("android", o["kind"]!!.jsonPrimitive.content)
        assertEquals(0, o["observations"]!!.jsonArray.size); assertEquals(0, o["fixes"]!!.jsonArray.size)
        val e = o["plateEvents"]!!.jsonArray.single().jsonObject
        // the column names the hub's PlateWatch::ingest / MapDb::mergePlateEvent read; metrics and raw as objects
        assertEquals("pass:osm:node/42:29333332", e["uid"]!!.jsonPrimitive.content)
        assertEquals("camera_pass", e["kind"]!!.jsonPrimitive.content)
        assertEquals("osm:node/42", e["camera_id"]!!.jsonPrimitive.content)
        assertEquals("phone_live", e["source"]!!.jsonPrimitive.content)
        assertEquals("2025-10-09T08:53:20", e["time"]!!.jsonPrimitive.content)
        assertEquals(21.0, e["distance_m"]!!.jsonPrimitive.content.toDouble(), 0.0)
        assertTrue(e["metrics"] is JsonObject); assertTrue(e["raw"] is JsonObject)
        assertFalse("records only: no images go to the hub", "media" in e)
        assertFalse("no local bookkeeping on the wire", e.keys.any { it in setOf("dirty", "hubDirty", "hub_dirty", "notified", "id") })
    }

    @Test fun anObservationPushIsUnchanged() {
        val o = obj(ApiFactory.json.encodeToString(HubSyncBody.serializer(), HubSyncBody("Test phone", "android", null, emptyList(), listOf(FixDto(40.0, -75.0, 5.0, "2026-10-01T12:00:00")), null)))
        assertEquals(setOf("device", "kind", "observations", "fixes"), o.keys)
    }

    @Test fun theAnswerDecidesWhatClears() {
        val sent = listOf("a", "b", "c")
        fun answer(n: Int) = obj("""{"accepted":{"observations":0,"aps":0,"fixes":0,"anchors":0,"plateEvents":$n},"cursor":812.0,"refitQueued":false,"device":"Test phone"}""")
        assertEquals(3, HubPlates.accepted(answer(3)))
        assertEquals(HubPlates.Outcome(pushed = sent), HubPlates.outcome(sent, answer(3)))
        assertEquals(HubPlates.Outcome(split = true), HubPlates.outcome(sent, answer(2)))
        assertEquals(HubPlates.Outcome(refused = listOf("a")), HubPlates.outcome(listOf("a"), answer(0)))
        // a hub that does not take plate events says nothing about them: nothing clears
        val old = obj("""{"accepted":{"observations":0,"aps":0,"fixes":0,"anchors":0},"cursor":812}""")
        assertNull(HubPlates.accepted(old))
        assertTrue(HubPlates.outcome(sent, old).unsupported)
        assertTrue(HubPlates.outcome(sent, null).unsupported)
        // a hub that answers where each one landed (as POST /plate-events does): renames and refusals exactly
        val exact = obj("""{"accepted":{"plateEvents":2},"plateEventUids":["a",null,"pass:osm:node/42:29333330"]}""")
        assertEquals(HubPlates.Outcome(pushed = listOf("a", "c"), renames = listOf("c" to "pass:osm:node/42:29333330"), refused = listOf("b")), HubPlates.outcome(sent, exact))
        // a uid list of the wrong length is ignored: the count decides
        assertEquals(HubPlates.Outcome(split = true), HubPlates.outcome(sent, obj("""{"accepted":{"plateEvents":2},"plateEventUids":["a"]}""")))
    }

    @Test fun aShortCountIsResolvedOneByOne() {
        val took = HubPlates.Outcome(pushed = listOf("a")); val no = HubPlates.Outcome(refused = listOf("b"))
        // the hub stores plate events (it took one): the one it refused is refused for what it is
        assertEquals(HubPlates.Outcome(pushed = listOf("a"), dropped = listOf("b")), HubPlates.afterSplit(listOf("a" to took, "b" to no)))
        // it took none: nothing is decided, all stay queued
        val none = HubPlates.afterSplit(listOf("a" to HubPlates.Outcome(refused = listOf("a")), "b" to no))
        assertTrue(none.pushed.isEmpty() && none.dropped.isEmpty()); assertEquals(listOf("a", "b"), none.refused)
    }

    @Test fun changesCarryPlateEventsAndAnOddRowIsSkipped() {
        val page = ApiFactory.json.decodeFromString(ChangesDto.serializer(), """{"since":800,"aps":[],"observations":[],"fixes":[],"anchors":[],
            "plateEvents":[{"uid":"pass:osm:node/42:29333332","kind":"camera_pass","plate":"ABC-1234","time":"2025-10-09T08:53:20","lat":40.0,"lon":-75.0,"acc":null,
              "camera_id":"osm:node/42","camera_lat":40.0002,"camera_lon":-75.0001,"distance_m":48.0,"facing":1,"operator":"Example PD","camera_type":"alpr",
              "source":"route_backfill","confidence":70,"leaky":0,"metrics":{"dwellS":4.2},"raw":{"camera":{"id":"osm:node/42"}},"device":"","created_at":"2025-10-09T09:00:00",
              "updated_at":"2025-10-09T09:00:00","seq":805},
              {"uid":"bad","kind":"camera_pass","confidence":"high"},
              {"kind":"plate_search","time":"2025-10-09T08:00:00"}],
            "count":3,"cursor":812,"more":false}""")
        assertEquals("812", HubSync.cursorText(page.cursor))
        val evs = HubPlates.decode(page.plateEvents)
        assertEquals(listOf("pass:osm:node/42:29333332"), evs.map { it.uid })
        val row = PlateEvents.fromDto(evs.single())
        assertEquals("osm:node/42", row.cameraId); assertEquals(1, row.facing); assertEquals("""{"camera":{"id":"osm:node/42"}}""", row.raw)
        // a LAN desktop's /db/changes without the key still decodes
        assertTrue(ApiFactory.json.decodeFromString(ChangesDto.serializer(), """{"cursor":"5","aps":[],"more":false}""").plateEvents.isEmpty())
    }

    @Test fun thePlateCursorFollowsTheFeedUnlessBehind() {
        // a fresh enrolment: both start at 0, the plate events of every page are taken
        assertTrue(HubPlates.inStep(null, 0)); assertEquals(812L, HubPlates.follow(null, 812))
        assertTrue(HubPlates.inStep(812, 812)); assertEquals(900L, HubPlates.follow(812, 900))
        // ahead after a catch-up: still in step, never moved back
        assertTrue(HubPlates.inStep(950, 900)); assertEquals(950L, HubPlates.follow(950, 900))
        // enrolled before the feed carried plate events: behind, so the page's plate events are left to the catch-up
        assertFalse(HubPlates.inStep(null, 812)); assertTrue(HubPlates.behind(null, 812)); assertFalse(HubPlates.behind(812, 812))
        assertFalse(HubPlates.inStep(0, null)); assertFalse(HubPlates.behind(0, null))
        assertEquals(812L, HubPlates.cursorLong("812.0")); assertNull(HubPlates.cursorLong("abc")); assertNull(HubPlates.cursorLong("-3"))
    }

    @Test fun theCatchUpJoinsTheFeedOnceDry() {
        assertEquals(500L to true, HubPlates.catchUp(0, 500, more = true, main = 812))
        assertEquals("dry: every plate event up to the feed cursor is in", 812L to false, HubPlates.catchUp(500, 640, more = false, main = 812))
        assertEquals(900L to false, HubPlates.catchUp(500, 900, more = false, main = 812))
        assertEquals("a page that does not move stops", 500L to false, HubPlates.catchUp(500, 500, more = true, main = 812))
        val page = PlateEventsPage(cursor = JsonPrimitive(640.0), more = false)
        assertEquals(640L, HubPlates.pageCursor(page, 500))
        assertEquals(640L, HubPlates.pageCursor(PlateEventsPage(cursor = JsonPrimitive("640")), 500))
        assertEquals(500L, HubPlates.pageCursor(PlateEventsPage(), 500))
    }
}
