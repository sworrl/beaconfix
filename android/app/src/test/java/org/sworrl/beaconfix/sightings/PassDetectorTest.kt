package org.sworrl.beaconfix.sightings

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.cos
import kotlin.math.roundToInt

/** docs/SIGHTINGS.md §2 (the desktop's src/plateevents.cpp, ported): synthetic drives past a camera. */
class PassDetectorTest {
    private val lat0 = 39.63; private val lon0 = -79.95
    private val k = Math.PI / 180 * 6_371_000.0
    private fun at(x: Double, y: Double) = lat0 + y / k to lon0 + x / (k * cos(Math.toRadians(lat0)))
    private fun fix(t: Long, x: Double, y: Double, acc: Double? = 5.0, speed: Double? = null): RouteFix { val (la, lo) = at(x, y); return RouteFix(t, la, lo, acc, speed, device = "phone", source = "gps") }
    private fun tags(json: String): JsonObject = Json.parseToJsonElement(json).jsonObject
    private fun cam(direction: String = "", model: String = "Falcon", source: String = "osm", type: String = "alpr", tags: JsonObject = JsonObject(emptyMap()), webcam: String? = null) =
        PassCamera("osm:node/42", lat0, lon0, model = model, operator = "Morgantown Police Department", source = source, direction = direction, type = type, tags = tags, webcam = webcam)

    /** Eastbound at 15 m/s on the line y = [y], a fix every 2 s from x = -295 m (no fix lands on x = 0). */
    private fun drive(y: Double = 20.0, from: Double = -295.0, to: Double = 295.0, t0: Long = 1_760_000_000_000): List<RouteFix> {
        val out = ArrayList<RouteFix>(); var x = from; var t = t0
        while (x <= to) { out += fix(t, x, y); x += 30.0; t += 2000 }
        return out
    }

    @Test fun closestApproachIsOnThePolylineWithInterpolatedTime() {
        val route = drive()
        val p = PassDetector.detect(route, cam()).single()
        assertEquals(20.0, p.distanceM, 0.05)
        // x = 0 is reached 295 / 15 = 19.667 s after the first fix
        assertEquals((route[0].timeMs + 19_667).toDouble(), p.closestMs.toDouble(), 2.0)
        assertEquals(54.0, p.speedKmh!!, 0.01)
        assertEquals(90.0, p.headingDeg!!, 0.1)
        assertEquals(0.0, p.approachBearingDeg, 0.5)              // due north of the camera
        assertEquals(270.0, p.frontBearingDeg, 30.0)              // 10 s before: 150 m west
        assertEquals(90.0, p.rearBearingDeg, 30.0)
        assertEquals(PassDetector.passUid("osm:node/42", p.closestMs), "pass:osm:node/42:${p.closestMs / 60_000}")
        // no direction: P(in cone) = the share of the run inside 65 m that is within the Falcon's 6–25 m, × 0.5
        assertNull(p.facing)
        assertEquals(10.0, p.cone.halfDeg, 0.0); assertEquals(25.0, p.cone.maxM, 0.0)
        val share = 30.0 / (2 * Math.sqrt(65.0 * 65 - 20.0 * 20))
        assertEquals(share * 0.5, p.pInCone, 0.01)
        assertEquals((100 * p.pInCone * 0.97 * 0.93).roundToInt(), p.confidence)
        assertEquals(2 * Math.sqrt(65.0 * 65 - 20.0 * 20) / 15.0, p.dwellS, 0.05)
    }

    @Test fun segmentsCountNotJustTheFixPoints() {
        // two fixes 80 m apart, each 44.7 m from the camera; the line between them passes 20 m away
        val p = PassDetector.detect(listOf(fix(0, -40.0, 20.0), fix(4000, 40.0, 20.0)), cam()).single()
        assertEquals(20.0, p.distanceM, 0.05)
        assertEquals(2000.0, p.closestMs.toDouble(), 1.0)
        assertEquals(72.0, p.speedKmh!!, 0.01)
        assertEquals(2, p.fixes)
        assertEquals(listOf("phone"), p.fixDevices)
    }

    @Test fun gapsAndLongSegmentsSplitRuns() {
        val a = listOf(fix(0, -30.0, 10.0), fix(2000, 0.0, 10.0))
        val b = listOf(fix(2000 + 6 * 60_000, 0.0, 30.0), fix(4000 + 6 * 60_000, 30.0, 30.0))
        val passes = PassDetector.detect(b + a, cam())
        assertEquals(2, passes.size)
        assertEquals(10.0, passes[0].distanceM, 0.05); assertEquals(30.0, passes[1].distanceM, 0.05)
        // a 2 km jump is not interpolated: the fixes stand alone, both far away
        assertTrue(PassDetector.detect(listOf(fix(0, -1000.0, 10.0), fix(10_000, 1000.0, 10.0)), cam()).isEmpty())
        // a lone fix inside the circle is a pass of its own, with the fix's own speed
        val lone = PassDetector.detect(listOf(fix(0, 5.0, 12.0, speed = 10.0)), cam()).single()
        assertEquals(13.0, lone.distanceM, 0.05); assertEquals(36.0, lone.speedKmh!!, 0.01)
    }

    @Test fun fixesOutsideTheRadiusMakeNoPass() {
        assertTrue(PassDetector.detect(drive(y = 80.0), cam()).isEmpty())
    }

    @Test fun facingFromTheConeBeforeAndAfter() {
        val route = drive(y = 3.0)                                  // the camera stands 3 m off the lane
        val w = PassDetector.detect(route, cam("W")).single()       // looks west, up the road: sees us coming
        assertEquals(true, w.frontVisible); assertEquals(false, w.rearVisible); assertEquals(true, w.facing)
        assertEquals(270.0, w.cameraDirDeg!!, 0.0)
        assertTrue(w.inConeS > 0.5)
        assertTrue(w.pInCone > 0.5)
        assertEquals((100 * w.pInCone * 0.97 * 0.93).roundToInt(), w.confidence)
        val e = PassDetector.detect(route, cam("90")).single()      // looks east: sees us leave (rear plate)
        assertEquals(false, e.frontVisible); assertEquals(true, e.rearVisible); assertEquals(true, e.facing)
        val n = PassDetector.detect(route, cam("N")).single()       // looks across the road
        assertEquals(false, n.facing)
        assertTrue(n.confidence < w.confidence)
        assertNull("direction=0 means unknown", PassDetector.detect(route, cam("0")).single().facing)
        assertEquals(true, PassDetector.detect(route, cam("0;270")).single().facing)
        // the OSM tag wins over the stored direction, `direction` over `camera:direction`
        assertEquals(true, PassDetector.detect(route, cam("N", tags = tags("""{"direction":"270","camera:direction":"0"}"""))).single().facing)
    }

    @Test fun directionsParse() {
        assertEquals(listOf(45.0), PassDetector.parseDirections("NE"))
        assertEquals(listOf(337.5), PassDetector.parseDirections("nnw"))
        assertEquals(listOf(90.0, 270.0), PassDetector.parseDirections("90; 270"))
        assertEquals(listOf(0.0), PassDetector.parseDirections("350-10"))
        assertEquals(listOf(120.0), PassDetector.parseDirections("100-140"))
        assertEquals(listOf(355.5), PassDetector.parseDirections("-4.5"))
        assertEquals(listOf(90.0), PassDetector.parseDirections("EB"))
        assertEquals(listOf(90.0), PassDetector.parseDirections("0;90"))
        assertTrue(PassDetector.parseDirections("0").isEmpty())
        assertTrue(PassDetector.parseDirections("0-360").isEmpty())
        // DeFlock's grammar (lib.mjs), as the desktop's cameraimport_test checks it
        assertEquals(listOf(90.0, 270.0), PassDetector.parseDirections("90,270"))
        assertEquals(listOf(45.0), PassDetector.parseDirections("NORTHEAST"))
        assertEquals(listOf(225.0), PassDetector.parseDirections("southwest"))
        assertEquals(0.5, PassDetector.parseDirections("338-23").single(), 1e-9)
        assertEquals(0.0, PassDetector.parseDirections("WSW-ESE").single(), 1e-9)
        assertTrue(PassDetector.parseDirections("").isEmpty())
        assertTrue(PassDetector.parseDirections("towards the bank").isEmpty())
    }

    @Test fun cameraTypesFromTags() {
        fun c(t: String) = PassDetector.classifyCamera("", "osm", "osm:node/1", tags(t))
        assertEquals("enforcement", c("""{"highway":"speed_camera","surveillance:type":"ALPR"}"""))
        assertEquals("not_camera", c("""{"surveillance:type":"gunshot_detector"}"""))
        assertEquals("not_camera", c("""{"man_made":"monitoring_station","monitoring:traffic":"yes"}"""))
        assertEquals("alpr", c("""{"surveillance:type":"ALPR"}"""))
        assertEquals("alpr", c("""{"surveillance:type":"camera;ANPR"}"""))
        assertEquals("alpr", c("""{"camera:type":"alpr"}"""))
        assertEquals("alpr", c("""{"surveillance:type":"ALRP"}"""))
        assertTrue(PassDetector.classifyNote(tags("""{"surveillance:type":"ALRP"}""")).contains("misspelt"))
        assertTrue(PassDetector.classifyNote(tags("""{"surveillance:type":"ALPR","contact:webcam":"https://x"}""")).contains("tagging conflict"))
        // WVDOT CAM064: a WV511 traffic camera with a public feed
        assertEquals("webcam", c("""{"surveillance:type":"camera","surveillance:zone":"traffic","contact:webcam":"https://wv511.org/CameraListing.aspx?CAMID=CAM064"}"""))
        assertEquals("ptz", c("""{"surveillance:type":"camera","manufacturer":"Flock Safety"}"""))
        assertEquals("cctv", c("""{"surveillance:type":"camera","surveillance:zone":"traffic"}"""))
        // OSM rows whose tags the desktop has not fetched yet: the imported model
        assertEquals("alpr", PassDetector.classifyCamera("Falcon", "osm", "osm:node/1"))
        assertEquals("cctv", PassDetector.classifyCamera("CAMERA", "osm", "osm:node/1"))
        // non-OSM lists are plate readers unless their model says otherwise
        assertEquals("alpr", PassDetector.classifyCamera("", "deflock", "deflock:9"))
        assertEquals("alpr", PassDetector.classifyCamera("FS Ext Battery", "ble_scan", "det:AA"))
        assertEquals("cctv", PassDetector.classifyCamera("traffic cam", "community", "c:1"))
        assertEquals("https://x", PassDetector.webcamUrl(tags("""{"contact:webcam":"https://x;https://y"}""")))
    }

    @Test fun conesAndRanges() {
        assertEquals(Cone(10.0, 6.0, 25.0, "Flock Falcon"), PassDetector.coneFor(cam(model = "Falcon")))
        assertEquals(7.0, PassDetector.coneFor(cam(model = "Falcon LR")).halfDeg, 0.0)
        assertEquals(Cone(12.0, 8.0, 23.0, "Motorola / Vigilant L5F, L6Q"), PassDetector.coneFor(cam(model = "L5F")))
        assertEquals(45.0, PassDetector.coneFor(cam(model = "AutoVu SharpV")).maxM, 0.0)
        assertEquals(22.0, PassDetector.coneFor(PassCamera("x", 0.0, 0.0, tags = tags("""{"manufacturer":"Verkada"}"""))).halfDeg, 0.0)
        assertEquals(Cone(), PassDetector.coneFor(PassCamera("x", 0.0, 0.0, model = "ALPR")))
        assertEquals(12.0, PassDetector.coneFor(PassCamera("x", 0.0, 0.0, manufacturer = "Motorola Solutions")).halfDeg, 0.0)   // DeFlock's brand
    }

    @Test fun confidenceRules() {
        assertEquals(93, PassDetector.confidenceFor(0.93, cam(), true))
        assertEquals(59, PassDetector.confidenceFor(0.84, PassCamera("x", 0.0, 0.0, source = "Suspected"), true))   // × 0.7
        assertEquals(71, PassDetector.confidenceFor(0.84, cam(tags = tags("""{"surveillance:type":"ALRP"}""")), true))   // × 0.85
        assertEquals(40, PassDetector.confidenceFor(0.84, cam(), false))                                  // not an ALPR: ≤ 40
        assertTrue(PassDetector.suspectedOnly("osm", "Falcon (suspected)"))
        // a traffic webcam on the route: recorded, confidence ≤ 40, honest details
        val wc = PassCamera("osm:node/64", lat0, lon0, model = "CAMERA", operator = "WVDOT", source = "osm", direction = "W", type = "webcam", webcam = "https://wv511.org/x")
        val p = PassDetector.detect(drive(y = 3.0), wc).single()
        assertEquals("webcam", p.cameraType); assertTrue(p.confidence <= 40); assertTrue(p.confidenceAlpr >= p.confidence)
        assertEquals("Passed a public traffic webcam (WVDOT) 3 m away — it does not read plates.", PlateEvents.detailsOf(p))
        assertEquals("Passed an ALPR camera (Morgantown Police Department Falcon) 3 m away: your plate was likely read (camera faced you).",
            PlateEvents.detailsOf(PassDetector.detect(drive(y = 3.0), cam("W")).single()))
    }

    @Test fun liveTrackerFinishesOnLeavingTheCircle() {
        val lt = LiveTracker()
        val c = cam("W")
        val out = ArrayList<Pass>()
        for (f in drive(y = 3.0)) out += lt.addFix(f, listOf(c))
        assertEquals(1, out.size)
        assertEquals(3.0, out[0].distanceM, 0.05)
        assertEquals(true, out[0].facing)
        // within 10 min of that pass the same camera does not pass again
        assertTrue(drive(y = 3.0, t0 = 1_760_000_000_000 + 120_000).flatMap { lt.addFix(it, listOf(c)) }.isEmpty())
        // a stop inside the circle finishes 2 min after the closest approach
        val stop = LiveTracker()
        val t0 = 1_770_000_000_000
        assertTrue(stop.addFix(fix(t0, -30.0, 3.0), listOf(c)).isEmpty())
        assertTrue(stop.addFix(fix(t0 + 2000, -2.0, 3.0), listOf(c)).isEmpty())
        assertEquals(1, stop.tick(t0 + 2000 + LiveTracker.FINISH_AFTER_MS).size)
    }

    @Test fun mergeWindowAndFlock() {
        assertTrue(PassDetector.samePass("c", 0, "c", 10 * 60_000))
        assertFalse(PassDetector.samePass("c", 0, "c", 10 * 60_000 + 1))
        assertFalse(PassDetector.samePass("c", 0, "d", 0))
        assertTrue(PassDetector.isFlock("Flock Safety", "")); assertTrue(PassDetector.isFlock("", "Condor"))
        assertFalse(PassDetector.isFlock("WV State Police", "Motorola"))
    }

    @Test fun metricsCarryEveryNumber() {
        val p = PassDetector.detect(drive(y = 3.0), cam("W")).single()
        val m = PlateEvents.metricsOf(p, plateInferred = true)
        for (key in listOf("dwellS", "fixes", "fixDevices", "fixSources", "enterDistanceM", "exitDistanceM", "frontBearingDeg", "rearBearingDeg",
            "frontVisible", "rearVisible", "cameraDirections", "plateInferred", "cameraSource", "coneHalfDeg", "rangeM", "inConeS", "pInCone", "pRead", "confidenceAlpr"))
            assertTrue(key, m.containsKey(key))
        val row = PlateEvents.fromPass(p, PlateEvents.SRC_DASHCAM, "XYZ-2345", true, "Pixel", false, null, 0)
        assertEquals("https://www.openstreetmap.org/node/42", row.sourceUrl)
        assertEquals("OpenStreetMap", row.sourceName)
        assertEquals("alpr", row.cameraType)
        assertTrue(row.raw!!.contains("\"camera\""))
        assertTrue(row.dirty)
    }

    @Test
    fun importedRowsClassifyLikeTheDesktop() {
        assertEquals("alpr", PassDetector.classifyCamera("", "deflock", "osm:node/1006"))                 // DeFlock: ALPRs only
        assertEquals("not_camera", PassDetector.classifyCamera("Raven", "ble_scan", "det:C6:11:22:33:44:55"))   // a gunshot detector
        assertEquals("alpr", PassDetector.classifyCamera("Falcon", "wifi_scan", "det:B4:1E:52:12:34:56"))
        assertEquals("cctv", PassDetector.classifyCamera("Other surveillance camera", "3rd Party / Suspected", "flock:9"))
    }
}
