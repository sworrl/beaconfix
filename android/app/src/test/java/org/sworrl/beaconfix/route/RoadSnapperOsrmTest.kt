package org.sworrl.beaconfix.route

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.collector.MotionMode
import org.sworrl.beaconfix.data.db.FixEntity
import java.util.Locale

/** The OSRM side of [RoadSnapper]: request shape, answer parsing, and how often it asks (the demo server's terms). */
class RoadSnapperOsrmTest {
    private val t0 = 1_700_000_000_000L

    /** A drive north at ~25 m/s, one fix every 4 s (100 m apart): [n] fixes. */
    private fun drive(n: Int, source: String = "phone-vehicle") = (0 until n).map { i ->
        FixEntity(id = i.toLong() + 1, time = t0 + i * 4_000L, lat = 39.75 + i * 0.0009, lon = -104.99, acc = 5.0, source = source)
    }

    private class Fake(var reply: (String) -> OsrmReply) {
        val urls = mutableListOf<String>()
        val pauses = mutableListOf<Long>()
        var clock = 0L
        fun snapper() = RoadSnapper(fetch = { u -> urls += u; reply(u) }, now = { clock }, pause = { pauses += it; clock += it })
    }

    private fun okFor(url: String): OsrmReply {
        val coords = url.substringAfter("/driving/").substringBefore('?').split(';')
        val geo = coords.joinToString(",") { c -> val (lon, lat) = c.split(','); "[$lon,${lat.toDouble() + 0.00001}]" }
        return OsrmReply(200, """{"code":"Ok","matchings":[{"confidence":0.9,"geometry":{"coordinates":[$geo],"type":"LineString"},"legs":[],"distance":100.0,"duration":10.0}],"tracepoints":[]}""")
    }

    @Test fun windowsHoldAtMostTenPointsAndShareTheirEnds() {
        val w = RoadSnapper.windows(drive(25))
        assertTrue(w.all { it.size <= RoadSnapper.MAX_COORDS })
        assertEquals(listOf(10, 10, 7), w.map { it.size })
        for (i in 1 until w.size) assertEquals(w[i - 1].last(), w[i].first())
        assertEquals(drive(25).last(), w.last().last())
        assertTrue(RoadSnapper.windows(drive(1)).isEmpty())
    }

    @Test fun thinningKeepsPointsFiftyMetresApart() {
        // fixes every 10 m: one in five survives, and the choice of earlier points does not change as the drive grows
        val dense = (0 until 30).map { i -> FixEntity(id = i.toLong(), time = t0 + i * 1_000L, lat = 39.75 + i * 0.00009, lon = -104.99, acc = 5.0, source = "phone-vehicle") }
        val a = RoadSnapper.thin(dense.take(20)); val b = RoadSnapper.thin(dense)
        assertEquals(a, b.take(a.size))
        assertTrue(b.size in 5..7)
        // shorter than the spacing: its two ends
        assertEquals(2, RoadSnapper.thin(dense.take(3)).size)
    }

    @Test fun urlIsLocaleProofAndMatchesTheServerSyntax() {
        val saved = Locale.getDefault()
        try {
            Locale.setDefault(Locale.GERMANY)
            val u = RoadSnapper.matchUrl(drive(3))
            assertTrue(u, u.startsWith("https://router.project-osrm.org/match/v1/driving/-104.990000,39.750000;-104.990000,39.750900;"))
            assertTrue(u, u.endsWith("?overview=full&geometries=geojson&steps=false&radiuses=25;25;25"))
            assertFalse(u, Regex("""\d,\d{6},""").containsMatchIn(u.substringAfter("/driving/").substringBefore('?').replace(";", " ")))
        } finally { Locale.setDefault(saved) }
    }

    @Test fun answersAreParsedByCode() {
        val ok = RoadSnapper.parseMatch(200, """{"code":"Ok","matchings":[{"geometry":{"coordinates":[[-79.955479,39.635696],[-79.955479,39.635696],[-79.955417,39.635678]],"type":"LineString"}},{"geometry":{"coordinates":[[-79.9540,39.6368],[-79.9539,39.6369]],"type":"LineString"}}]}""")
        assertTrue(ok is RoadSnapper.Match.Snapped)
        val pts = (ok as RoadSnapper.Match.Snapped).points
        assertEquals(4, pts.size)                                  // the repeated first coordinate is dropped
        assertEquals(39.635696, pts[0].lat, 1e-9); assertEquals(-79.955479, pts[0].lon, 1e-9)
        assertTrue(pts.all { it.isSnapped })
        assertEquals(RoadSnapper.Match.NoMatch, RoadSnapper.parseMatch(400, """{"code":"NoMatch","message":"Could not match the trace."}"""))
        assertEquals(RoadSnapper.Match.NoMatch, RoadSnapper.parseMatch(400, """{"message":"Could not find a matching segment for any coordinate.","code":"NoSegment"}"""))
        assertEquals(RoadSnapper.Match.Rejected, RoadSnapper.parseMatch(400, """{"message":"Too many trace coordinates","code":"TooBig"}"""))
        assertEquals(RoadSnapper.Match.Retry, RoadSnapper.parseMatch(429, """{"message":"Too Many Requests"}"""))
        assertEquals(RoadSnapper.Match.Retry, RoadSnapper.parseMatch(502, "<html>Bad Gateway</html>"))
        assertEquals(RoadSnapper.Match.Retry, RoadSnapper.parseMatch(0, null))
    }

    @Test fun aFinishedDriveIsAskedAboutOnceAtOneRequestASecond() = runBlocking {
        val f = Fake { okFor(it) }; f.clock = t0 + 3_600_000L
        val s = f.snapper()
        val segs = s.snapTrack(drive(25))
        assertEquals(1, segs.size); assertEquals(MotionMode.IN_VEHICLE, segs[0].mode)
        assertEquals(3, f.urls.size)
        assertTrue(f.urls.all { it.substringAfter("/driving/").substringBefore('?').split(';').size <= RoadSnapper.MAX_COORDS })
        assertTrue(f.urls.all { it.contains("radiuses=") })
        assertEquals(2, f.pauses.size); assertTrue(f.pauses.all { it >= 1_000 })
        assertTrue(segs[0].points.any { it.isSnapped })
        // drawn again: all from the cache
        s.snapTrack(drive(25))
        assertEquals(3, f.urls.size)
    }

    @Test fun aDriveInProgressSendsOnlyFullWindows() = runBlocking {
        val f = Fake { okFor(it) }
        val fixes = drive(14); f.clock = fixes.last().time + 10_000L
        val s = f.snapper()
        val segs = s.snapTrack(fixes)
        assertEquals(1, f.urls.size)                                // the first 10 points; the 5-point tail waits
        assertEquals(fixes.last().lat, segs[0].points.last().lat, 1e-6)
        s.snapTrack(drive(15)); assertEquals(1, f.urls.size)        // still one window and a tail
    }

    @Test fun rateLimitingBacksOffAndNoMatchIsRemembered() = runBlocking {
        var code = 429
        val f = Fake { u -> if (code == 429) OsrmReply(429, """{"message":"Too Many Requests"}""") else OsrmReply(400, """{"code":"NoMatch","message":"x"}""") }
        f.clock = t0 + 3_600_000L
        val s = f.snapper()
        val segs = s.snapTrack(drive(25))
        assertEquals(1, f.urls.size)                                // 429: nothing more this pass
        assertTrue(segs[0].points.size >= 2 && segs[0].points.none { it.isSnapped })
        s.snapTrack(drive(25)); assertEquals(1, f.urls.size)        // still backing off
        f.clock += RoadSnapper.BACKOFF_MS; code = 400
        s.snapTrack(drive(25)); assertEquals(4, f.urls.size)        // asked again: no road matches
        s.snapTrack(drive(25)); assertEquals(4, f.urls.size)        // remembered
    }
}
