package org.sworrl.beaconfix.trip

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.importer.Importers
import org.w3c.dom.Element
import javax.xml.parsers.DocumentBuilderFactory

class GpxWriterTest {
    /** 2026-09-27T08:00:00Z */
    private val t0 = 1_790_496_000_000L
    private val path = (0 until 12).map { i ->
        PathPoint(t0 + i * 60_000L + 250, 40.0 + i * 0.001, -75.0 - i * 0.0005, 10.0, ele = if (i % 3 == 0) 200.0 + i else null)
    } + PathPoint(t0 + 3 * 3_600_000L, 40.5, -75.2)          // after a 3 h gap: a new segment
    private val stops = listOf(
        Stop(40.004, -75.002, t0 + 4 * 60_000L, t0 + 20 * 60_000L, 960, 450.0, elevM = 204.0, place = "Tom & Jerry's <Camp> \"A\""),
        Stop(40.5, -75.2, t0 + 3 * 3_600_000L, null, -1, 55_000.0, place = "", open = true),
    )

    private fun doc(xml: String) = DocumentBuilderFactory.newInstance().apply { isNamespaceAware = true }.newDocumentBuilder().parse(xml.byteInputStream())
    private fun Element.kids(tag: String) = getElementsByTagNameNS("http://www.topografix.com/GPX/1/1", tag).let { l -> (0 until l.length).map { l.item(it) as Element } }

    @Test fun writesAWellFormedGpx11Track() {
        val xml = GpxWriter.write(path, stops, "Test trip", now = t0)
        val gpx = doc(xml).documentElement
        assertEquals("gpx", gpx.localName); assertEquals("1.1", gpx.getAttribute("version"))
        val trkpts = gpx.kids("trkpt")
        assertEquals(path.size, trkpts.size)
        assertEquals(2, gpx.kids("trkseg").size)
        // UTC times, whole seconds
        for ((p, e) in path.zip(trkpts)) {
            val time = e.kids("time").single().textContent
            assertTrue(time, time.endsWith("Z")); assertEquals(20, time.length)
            assertEquals(GpxWriter.time(p.time), time)
            assertEquals(p.ele != null, e.kids("ele").isNotEmpty())
        }
        assertEquals("200.0", trkpts[0].kids("ele").single().textContent)
        assertEquals("2026-09-27T08:00:00Z", trkpts[0].kids("time").single().textContent)
        // one waypoint per stop, names escaped and read back intact
        val wpts = gpx.kids("wpt")
        assertEquals(2, wpts.size)
        assertEquals("Tom & Jerry's <Camp> \"A\"", wpts[0].kids("name").single().textContent)
        assertEquals("stayed 16 min", wpts[0].kids("desc").single().textContent)
        assertEquals("204.0", wpts[0].kids("ele").single().textContent)
        assertEquals("Stop", wpts[1].kids("name").single().textContent)
        assertTrue(wpts[1].kids("desc").isEmpty())
        // waypoints come before the track (GPX 1.1 element order)
        assertTrue(xml.indexOf("<wpt ") < xml.indexOf("<trk>"))
    }

    @Test fun roundTripsThroughTheImporter() {
        val xml = GpxWriter.write(path, stops, now = t0)
        assertEquals("gpx", Importers.detect(xml.take(400), "trip.gpx"))
        val r = Importers.parse("gpx", xml.byteInputStream())
        assertEquals(path.size + stops.size, r.positions.size)
        val byTime = r.positions.groupBy { it.time }
        for (p in path) {
            val back = byTime[p.time / 1000 * 1000]!!.first { Math.abs(it.lat - p.lat) < 1e-6 }
            assertEquals(p.lon, back.lon, 1e-6)
            if (p.ele != null) assertEquals(p.ele!!, back.alt!!, 0.05)
        }
        assertEquals(stops[0].arrival, r.positions.first().time)
    }

    @Test fun anEmptyJournalIsStillAValidFile() {
        val gpx = doc(GpxWriter.write(emptyList(), emptyList(), now = t0)).documentElement
        assertTrue(gpx.kids("trk").isEmpty()); assertTrue(gpx.kids("wpt").isEmpty())
        assertEquals("a &amp; b &lt;c&gt; &quot;d&quot; &apos;e&apos;", GpxWriter.esc("a & b <c> \"d\" 'e'\u0001"))
    }
}
