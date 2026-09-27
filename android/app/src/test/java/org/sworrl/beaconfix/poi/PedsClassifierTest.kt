package org.sworrl.beaconfix.poi

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.double
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * The Kotlin port against the shared fixture, with the same checks as the desktop's tests/poiclassify_test.cpp:
 * both classifiers must give exactly these answers.
 */
class PedsClassifierTest {
    private val fixture: JsonObject by lazy {
        // unit tests run with the module (android/app) as the working directory
        val f = listOf(File("../../tests/fixtures/pediatric_tags.json"), File("tests/fixtures/pediatric_tags.json")).firstOrNull { it.isFile }
            ?: error("tests/fixtures/pediatric_tags.json not found from ${File(".").absolutePath}")
        Json.parseToJsonElement(f.readText()).jsonObject
    }

    private fun elements(s: JsonObject): List<OsmElement> = s["elements"]!!.jsonArray.map { v ->
        val e = v.jsonObject
        OsmElement(e["type"]!!.jsonPrimitive.content, e["id"]!!.jsonPrimitive.long, e["lat"]!!.jsonPrimitive.double, e["lon"]!!.jsonPrimitive.double,
            e["tags"]!!.jsonObject.mapValues { it.value.jsonPrimitive.content })
    }

    @Test fun fixtureScenarios() {
        val scenarios = fixture["scenarios"]!!.jsonArray
        assertTrue("fixture has scenarios", scenarios.isNotEmpty())
        val failures = ArrayList<String>()
        fun check(ok: Boolean, msg: String) { if (!ok) failures += msg }
        var checked = 0
        for (sv in scenarios) {
            val s = sv.jsonObject
            val name = s["name"]!!.jsonPrimitive.content
            val els = elements(s)
            val res = PedsClassifier.classifyAll(els)
            check(res.size == els.size, "$name: one result per element")
            val byKey = els.indices.associateBy { els[it].key }
            for (xv in s["expect"]!!.jsonArray) {
                val x = xv.jsonObject
                val id = x["id"]!!.jsonPrimitive.content
                val i = byKey[id]
                check(i != null, "$name: $id is in the elements")
                if (i == null) continue
                val r = res[i]; checked++
                val label = "$name: $id (${els[i].tags["name"]})"
                if (x["dropped"]?.jsonPrimitive?.boolean == true) { check(r.dropped, "$label is dropped as a duplicate"); continue }
                check(!r.dropped, "$label is kept")
                x["cat"]?.let { check(r.cat == it.jsonPrimitive.content, "$label cat ${r.cat}, expected ${it.jsonPrimitive.content}") }
                x["catNot"]?.jsonArray?.forEach { n -> check(r.cat != n.jsonPrimitive.content, "$label cat ${r.cat} must not be ${n.jsonPrimitive.content}") }
                x["peds"]?.let { check(r.peds == it.jsonPrimitive.int, "$label peds ${r.peds}, expected ${it.jsonPrimitive.int}") }
                x["er"]?.let { check(r.er == it.jsonPrimitive.content, "$label er '${r.er}', expected '${it.jsonPrimitive.content}'") }
                x["campusEr"]?.let { check(r.campus == it.jsonPrimitive.content, "$label campus '${r.campus}', expected '${it.jsonPrimitive.content}'") }
                x["emergency"]?.let { check(r.emergency == it.jsonPrimitive.boolean, "$label emergency ${r.emergency}, expected ${it.jsonPrimitive.boolean}") }
                x["phone"]?.let { check(r.phone == it.jsonPrimitive.content, "$label phone '${r.phone}', expected '${it.jsonPrimitive.content}'") }
                x["detailContains"]?.let { check(r.detail.contains(it.jsonPrimitive.content), "$label detail '${r.detail}' contains '${it.jsonPrimitive.content}'") }
            }

            // the help picks from the scenario's origin, as the desktop computes them
            val origin = s["origin"]!!.jsonObject
            val oLat = origin["lat"]!!.jsonPrimitive.double; val oLon = origin["lon"]!!.jsonPrimitive.double
            val idx = els.indices.filter { !res[it].dropped && res[it].cat.isNotEmpty() }
            val cands = idx.map { i ->
                val d = PedsClassifier.distanceM(oLat, oLon, els[i].lat, els[i].lon)
                PedsClassifier.HelpCandidate(res[i].cat, res[i].peds, res[i].campus, res[i].emergency, d, PedsClassifier.driveEstimate(d).first)
            }
            val p = PedsClassifier.pickHelp(cands)
            fun keyOf(k: Int) = if (k < 0) "null" else els[idx[k]].key
            val help = s["help"]!!.jsonObject
            for ((what, k) in listOf("pediatric" to p.pediatric, "pediatricCloser" to p.pediatricCloser, "hospital" to p.hospital, "pediatricUrgent" to p.pediatricUrgent)) {
                val want = help[what] ?: continue
                val w = if (want is JsonNull) "null" else want.jsonPrimitive.content
                check(keyOf(k) == w, "$name: help.$what = ${keyOf(k)}, expected $w")
            }
        }
        assertTrue("checked the expectations", checked >= 20)
        assertTrue(failures.joinToString("\n"), failures.isEmpty())
    }

    @Test fun morgantownDetails() {
        val s = fixture["scenarios"]!!.jsonArray.map { it.jsonObject }.first { it["name"]!!.jsonPrimitive.content == "morgantown" }
        val els = elements(s); val res = PedsClassifier.classifyAll(els)
        val wvu = res[els.indexOfFirst { it.key == "way/1135527599" }]
        assertEquals("ER on campus: Ruby Memorial Hospital — call ahead", wvu.detail)
        // the kept UPMC object took nothing it lacked; the dropped building had no phone of its own
        val upmc = res[els.indexOfFirst { it.key == "way/329264979" }]
        assertEquals("+1 412 692 5325", upmc.phone); assertEquals("pediatric ER", upmc.detail)
    }

    @Test fun unitBitsTheFixtureDoesNotReach() {
        assertEquals(300 to 0, PedsClassifier.driveEstimate(0.0))                       // at least 5 minutes
        assertEquals(7200 to 140_000, PedsClassifier.driveEstimate(100_000.0))           // 100 km → 140 km of road, 2 h
        assertEquals(0, PedsClassifier.pedsRank(2, "Some ER")); assertEquals(1, PedsClassifier.pedsRank(2, ""))
        assertEquals(1, PedsClassifier.pedsRank(3, "")); assertEquals(0, PedsClassifier.pedsRank(1, "")); assertEquals(-1, PedsClassifier.pedsRank(4, ""))
        assertEquals("health", PedsClassifier.baseCategory(mapOf("healthcare" to "hospital")))
        assertTrue(PedsClassifier.classify(mapOf("amenity" to "hospital", "name" to "Anytown General", "opening_hours" to "24/7")).emergency)
        assertFalse(PedsClassifier.classify(mapOf("amenity" to "hospital", "name" to "Anytown General", "opening_hours" to "24/7", "emergency" to "no")).emergency)
        assertEquals("", PedsClassifier.classify(mapOf("amenity" to "fuel")).cat)       // the phone only knows the help categories
        assertEquals("1 Test Street #2, Testville, WV 00000", PedsClassifier.address(mapOf("addr:housenumber" to "1", "addr:street" to "Test Street", "addr:unit" to "2",
            "addr:city" to "Testville", "addr:state" to "WV", "addr:postcode" to "00000")))
        assertEquals("children's hospital — ER not confirmed", PedsClassifier.tierLabel(2))
    }

    @Test fun elementFromOverpassJson() {
        val node = Json.parseToJsonElement("""{"type":"node","id":42,"lat":40.0,"lon":-75.0,"tags":{"amenity":"police"}}""")
        assertEquals(OsmElement("node", 42, 40.0, -75.0, mapOf("amenity" to "police")), OsmElement.fromOverpass(node))
        val way = Json.parseToJsonElement("""{"type":"way","id":7,"center":{"lat":40.1,"lon":-75.1}}""")
        assertEquals(OsmElement("way", 7, 40.1, -75.1, emptyMap()), OsmElement.fromOverpass(way))
        assertEquals(null, OsmElement.fromOverpass(Json.parseToJsonElement("""{"type":"way","id":8}""")))
    }
}
