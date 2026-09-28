package org.sworrl.beaconfix.poi

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.DesktopCache

class HelpQueryTest {
    private val lat = 40.0
    private val lon = -75.0

    private fun sample(): String = javaClass.classLoader!!.getResource("overpass_sample.json")!!.readText()

    @Test fun bboxMatchesTheDesktopFormula() {
        // 25 km: 25000 / 111320 = 0.22458° of latitude; / cos(40°) = 0.29317° of longitude
        assertEquals("39.77542,-75.29317,40.22458,-74.70683", HelpQuery.bbox(lat, lon, 25_000))
    }

    @Test fun helpQueryHasTheBoxAndOnlyTheHelpTags() {
        val box = HelpQuery.bbox(lat, lon, HelpQuery.HELP_RADIUS_M)
        val q = HelpQuery.help(box)
        assertTrue(q, q.startsWith("[out:json][timeout:25];"))
        assertTrue(q, q.contains("($box)"))
        for (b in box.split(',')) assertTrue(b, q.contains(b))
        assertTrue(q, q.contains("police|fire_station|hospital|clinic|urgent_care|pharmacy|veterinary"))
        assertTrue(q, q.contains("nwr[healthcare~\"^(hospital|clinic|urgent_care|pharmacy)$\"]"))
        for (other in listOf("fuel", "restaurant", "camp_site", "playground")) assertFalse(other, q.contains(other))
        assertTrue(q, q.endsWith("out center tags qt 1500;"))
    }

    @Test fun pediatricQueryHasBothBoxesAndThePediatricClauses() {
        val far = HelpQuery.bbox(lat, lon, 150_000); val urg = HelpQuery.bbox(lat, lon, HelpQuery.URGENT_RADIUS_M)
        val q = HelpQuery.pediatric(far, urg)
        assertTrue(q, q.startsWith("[out:json][timeout:60];"))
        assertTrue(q, q.contains("nwr[amenity=hospital]($far)"))
        assertTrue(q, q.contains("nwr[healthcare=hospital]($far)"))
        assertTrue(q, q.contains("nwr[\"emergency:paediatric\"=yes]($far)"))
        assertTrue(q, q.contains("[name~\"pa?ediatric|kids|child\",i]($urg)"))
        assertTrue(q, q.contains("[\"healthcare:speciality\"~\"pa?ediatric\",i]($urg)"))
        assertTrue(q, Regex("pa\\?ediatric").containsMatchIn(q))
        for (b in far.split(',') + urg.split(',')) assertTrue(b, q.contains(b))
        assertFalse("no around: over a wide box", q.contains("around"))
    }

    @Test fun sampleParsesToTheExpectedCategoriesAndTiers() {
        val r = OverpassClient.parse(200, sample())
        assertTrue(r.error, r.ok)
        assertEquals(10, r.elements.size)
        assertEquals(OsmElement("way", 1135527599L, 39.654058, -79.955153, r.elements[0].tags), r.elements[0])   // way center
        val res = PedsClassifier.classifyAll(r.elements).zip(r.elements).associate { (x, e) -> e.key to x }
        fun check(key: String, cat: String, peds: Int) { assertEquals("$key cat", cat, res[key]!!.cat); assertEquals("$key peds", peds, res[key]!!.peds); assertFalse(key, res[key]!!.dropped) }
        check("way/1135527599", "peds_er", 2)
        assertEquals("Ruby Memorial Hospital", res["way/1135527599"]!!.campus)
        check("way/463483594", "health", 0)
        check("way/329264979", "peds_er", 1)
        assertTrue(res["way/473666430"]!!.dropped)
        check("node/9000000005", "health", 3)
        check("node/9000000006", "peds_urgent", 4)
        check("node/9000000009", "health", 0)
        check("node/9000000010", "peds_er", 1)
        check("node/9000000101", "police", 0)
        check("node/9000000102", "pharmacy", 0)
    }

    private val oLat = 39.6350
    private val oLon = -79.9550

    @Test fun helpSelectionKeepsWhatIsWithin25km() {
        val r = OverpassClient.parse(200, sample())
        val hits = HelpQuery.selectHelp(r.elements, PedsClassifier.classifyAll(r.elements), oLat, oLon)
        assertEquals(setOf("way/1135527599", "way/463483594", "node/9000000005", "node/9000000006", "node/9000000101", "node/9000000102"),
            hits.map { it.el.key }.toSet())
        val rows = hits.map { HelpQuery.toEntity(it, DesktopCache.NEAR, oLat, oLon, 1_000L) }
        val wvu = rows.single { it.key == "way/1135527599" }
        assertEquals(DesktopCache.PHONE, wvu.source); assertEquals("near", wvu.scope)
        assertEquals("peds_er", wvu.cat); assertEquals("Pediatric ER", wvu.label); assertEquals("🧸", wvu.icon); assertEquals("#ff5fa2", wvu.color); assertEquals("civic", wvu.grp)
        assertEquals(2, wvu.peds); assertEquals("Ruby Memorial Hospital", wvu.campus); assertEquals("+13045981111", wvu.phone)
        assertTrue(wvu.detail, wvu.detail.startsWith("ER on campus: Ruby Memorial Hospital — call ahead"))
        assertEquals(300, wvu.driveS); assertTrue(wvu.driveEst); assertEquals(1_000L, wvu.fetchedAt); assertEquals(oLat, wvu.originLat, 0.0)
        val ruby = rows.single { it.key == "way/463483594" }
        assertTrue(ruby.emergency); assertEquals("yes", ruby.er); assertEquals("24/7", ruby.hours); assertEquals("Hospital / ER", ruby.label)
        val police = rows.single { it.cat == "police" }
        assertEquals("1 Test Street, Testville, WV", police.address); assertTrue(police.driveS > 0)
        val pharmacy = rows.single { it.cat == "pharmacy" }
        assertEquals(0, pharmacy.driveS)                            // no drive time outside the help-drive categories
        assertEquals("Test Drug · ♿ limited", pharmacy.detail)
        val kids = rows.single { it.cat == "peds_urgent" }
        assertEquals("not an ER · clinic", kids.detail)
    }

    @Test fun pediatricSelectionKeepsPedsErsUrgentCareAndErs() {
        val r = OverpassClient.parse(200, sample())
        val hits = HelpQuery.selectPediatric(r.elements, PedsClassifier.classifyAll(r.elements), oLat, oLon, 150_000)
        // confirmed pediatric ERs (tier 1) nearest first, then the other pediatric sites
        assertEquals(listOf("node/9000000010", "way/329264979", "way/1135527599"), hits.filter { it.r.cat == "peds_er" }.map { it.el.key })
        assertEquals(listOf("node/9000000006"), hits.filter { it.r.cat == "peds_urgent" }.map { it.el.key })
        assertEquals(listOf("way/463483594", "node/9000000005"), hits.filter { it.r.cat == "health" }.map { it.el.key })   // ERs only
        // a smaller radius drops the far children's hospital
        val near = HelpQuery.selectPediatric(r.elements, PedsClassifier.classifyAll(r.elements), oLat, oLon, 50_000)
        assertFalse(near.any { it.el.key == "way/329264979" })
        val row = HelpQuery.toEntity(hits.first { it.el.key == "way/329264979" }, DesktopCache.FAR, oLat, oLon, 5L)
        assertEquals("far", row.scope); assertEquals(1, row.peds); assertEquals("yes", row.wheelchair); assertTrue(row.detail.contains("♿"))
    }

    @Test fun failuresAreRecognised() {
        assertFalse(OverpassClient.parse(429, sample()).ok)
        assertFalse(OverpassClient.parse(504, "").ok)
        assertFalse(OverpassClient.parse(200, "").ok)
        assertFalse(OverpassClient.parse(200, null).ok)
        assertFalse(OverpassClient.parse(200, "<html>busy</html>").ok)
        assertFalse(OverpassClient.parse(200, """{"version":0.6,"elements":[],"remark":"runtime error: Query timed out in \"query\" at line 1 after 26 seconds."}""").ok)
        assertFalse(OverpassClient.parse(200, """{"elements":[],"remark":"Dispatcher_Client::request_read_and_idx::timeout. The server is probably too busy to handle your request."}""").ok)
        assertFalse(OverpassClient.parse(200, """{"version":0.6}""").ok)
        val empty = OverpassClient.parse(200, """{"version":0.6,"elements":[]}""")
        assertTrue(empty.ok); assertTrue(empty.elements.isEmpty())
    }

    @Test fun userAgentNamesTheApp() {
        assertTrue(OverpassClient.USER_AGENT, OverpassClient.USER_AGENT.matches(Regex("""BeaconFix-Android/\S+ \(\+https://github\.com/sworrl/beaconfix\)""")))
        assertEquals(listOf("overpass-api.de", "overpass.kumi.systems"), OverpassClient.MIRRORS.map { OverpassClient.host(it) })
    }
}
