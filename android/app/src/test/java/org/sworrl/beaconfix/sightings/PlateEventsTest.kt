package org.sworrl.beaconfix.sightings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.PlateEventDto
import org.sworrl.beaconfix.data.api.PlateEventsPage
import org.sworrl.beaconfix.data.db.PlateEventEntity
import java.time.ZoneOffset

/** §1.1 merging and the wire format of plate events. */
class PlateEventsTest {
    private val t = 1_760_000_000_000L
    private fun pass(source: String, conf: Int, dist: Double?, uid: String = PassDetector.passUid("osm:node/42", t), raw: String? = null) = PlateEventEntity(
        uid = uid, kind = PlateEvents.CAMERA_PASS, time = "2025-10-09T08:53:20", timeMs = t, cameraId = "osm:node/42", distanceM = dist, source = source,
        confidence = conf, raw = raw, operator = if (source == PlateEvents.SRC_BACKFILL) "Morgantown PD" else null, cameraType = "alpr")

    @Test fun aLivePassOutranksTheBackfillAndKeepsTheExistingUid() {
        val desk = pass(PlateEvents.SRC_BACKFILL, 70, 48.0, uid = "pass:osm:node/42:29333332", raw = """{"surveillance:type":"ALPR"}""")
        val phone = pass(PlateEvents.SRC_PHONE_LIVE, 60, 21.0)
        val m = PlateEvents.merge(desk, phone)
        assertEquals("pass:osm:node/42:29333332", m.uid)
        assertEquals(PlateEvents.SRC_PHONE_LIVE, m.source)
        assertEquals(21.0, m.distanceM!!, 0.0)
        assertEquals(60, m.confidence)
        assertEquals("""{"surveillance:type":"ALPR"}""", m.raw)       // nulls filled from the other side
        assertEquals("Morgantown PD", m.operator)
    }

    @Test fun sameRankTakesTheHigherConfidenceAndOnlyFillsNulls() {
        val a = pass(PlateEvents.SRC_BACKFILL, 70, 50.0)
        val b = pass(PlateEvents.SRC_BACKFILL, 85, null)
        val m = PlateEvents.merge(a, b)
        assertEquals(85, m.confidence)
        assertEquals(50.0, m.distanceM!!, 0.0)
        val lower = PlateEvents.merge(pass(PlateEvents.SRC_DASHCAM, 95, 12.0), pass(PlateEvents.SRC_BACKFILL, 99, 60.0))
        assertEquals(PlateEvents.SRC_DASHCAM, lower.source); assertEquals(12.0, lower.distanceM!!, 0.0)
        assertFalse(PlateEvents.changed(a, PlateEvents.merge(a, a)))
        assertTrue(PlateEvents.changed(a, m))
    }

    @Test fun feedDecodesSnakeOrCamelCase() {
        val snake = """{"events":[{"uid":"pass:osm:node/42:29333332","kind":"camera_pass","plate":"XYZ-2345","time":"2025-10-09T08:53:20","lat":39.6,"lon":-79.9,
            "camera_id":"osm:node/42","distance_m":21.5,"speed_kmh":54,"heading_deg":90,"camera_dir_deg":270,"facing":true,"camera_type":"alpr","source":"route_backfill",
            "source_url":"https://www.openstreetmap.org/node/42","confidence":95,"leaky":1,"metrics":{"dwellS":4.2,"fixes":3},"device":"","seq":17,
            "media":[{"uid":"0123456789abcdef0123456789abcdef","kind":"camera_photo","mime":"image/jxl","width":800,"height":600,"bytes":1234,"attribution":"Jane","license":"CC BY-SA 4.0"}]}],
            "cursor":17,"more":false}"""
        val page = ApiFactory.json.decodeFromString(PlateEventsPage.serializer(), snake)
        val e = PlateEvents.fromDto(page.events.single(), ZoneOffset.UTC)
        assertEquals("osm:node/42", e.cameraId); assertEquals(21.5, e.distanceM!!, 0.0); assertEquals(1, e.facing); assertEquals(1, e.leaky)
        assertEquals(95, e.confidence); assertEquals(17L, e.seq); assertEquals("""{"dwellS":4.2,"fixes":3}""", e.metrics)
        assertEquals(t, e.timeMs)
        assertEquals("CC BY-SA 4.0", page.events.single().media!!.single().license)
        val camel = """{"uid":"u","kind":"plate_search","time":"2025-01-01T00:00:00","cameraId":"c","distanceM":3.0,"sourceUrl":"https://x","facing":0,"metrics":"{\"a\":1}","source":"haveibeenflocked"}"""
        val c = PlateEvents.fromDto(ApiFactory.json.decodeFromString(PlateEventDto.serializer(), camel))
        assertEquals("c", c.cameraId); assertEquals(3.0, c.distanceM!!, 0.0); assertEquals("https://x", c.sourceUrl); assertEquals(0, c.facing); assertEquals("""{"a":1}""", c.metrics)
    }

    @Test fun pushEncodesColumnNamesWithObjectsForJson() {
        val e = pass(PlateEvents.SRC_PHONE_LIVE, 95, 20.0).copy(facing = 1, metrics = """{"dwellS":3.0}""", leaky = 1)
        val s = ApiFactory.json.encodeToString(PlateEventDto.serializer(), PlateEvents.toDto(e))
        assertTrue(s, s.contains("\"camera_id\":\"osm:node/42\""))
        assertTrue(s, s.contains("\"distance_m\":20.0"))
        assertTrue(s, s.contains("\"metrics\":{\"dwellS\":3.0}"))
        assertTrue(s, s.contains("\"facing\":1"))
        assertFalse("nulls are left out", s.contains("null"))
        // round trip
        val back = PlateEvents.fromDto(ApiFactory.json.decodeFromString(PlateEventDto.serializer(), s), ZoneOffset.UTC)
        assertEquals(e.uid, back.uid); assertEquals(e.metrics, back.metrics); assertEquals(1, back.facing); assertEquals(1, back.leaky)
    }

    @Test fun localTimesAndUrls() {
        assertEquals("2025-10-09T08:53:20", PlateEvents.localIso(t, ZoneOffset.UTC))
        assertEquals(t, PlateEvents.parseLocal(PlateEvents.localIso(t, ZoneOffset.UTC), ZoneOffset.UTC))
        assertEquals(t, PlateEvents.parseLocal("2025-10-09T08:53:20Z"))
        assertNull(PlateEvents.parseLocal("yesterday"))
        assertEquals("https://www.openstreetmap.org/node/42", PlateEvents.osmUrl("osm:node/42"))
        assertNull(PlateEvents.osmUrl("det:D8:F3:BC:11:22:33"))
    }

    @Test fun pushBatchesStayUnderTheBodyLimit() {
        val big = (0 until 50).map { PlateEvents.toDto(pass(PlateEvents.SRC_PHONE_LIVE, 95, 20.0, uid = "u$it").copy(raw = "\"" + "x".repeat(20_000) + "\"")) }
        val batch = PlateEventRepository.sizeLimited(big, 100_000)
        assertTrue(batch.size in 1..5)
        assertEquals(1, PlateEventRepository.sizeLimited(big.take(1), 10).size)
    }
}
