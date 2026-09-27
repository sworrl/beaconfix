package org.sworrl.beaconfix.help

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.time.LocalDateTime

/**
 * The C5 scenarios with the classifier's expected output as input (the classifier itself is PedsClassifier's and the
 * desktop's). When the shared fixture file is present, every scenario's help picks are checked against it too.
 */
class NearestHelpTest {
    /** A round synthetic origin beside the campus (the picks and the drive time match the C5 scenarios). */
    private val oLat = 39.65; private val oLon = -79.95

    private val wvuCampus = NearestHelp.Candidate(key = "way/1135527599", cat = "peds_er", name = "WVU Medicine Children's", lat = 39.654058, lon = -79.955153,
        phone = "+13045981111", peds = 2, campus = "Ruby Memorial Hospital")
    private val ruby = NearestHelp.Candidate(key = "way/463483594", cat = "health", name = "Ruby Memorial Hospital", lat = 39.654462, lon = -79.959066,
        phone = "+1 855 988 2273", hours = "24/7", er = "yes", emergency = true)
    private val upmc = NearestHelp.Candidate(key = "way/329264979", cat = "peds_er", name = "UPMC Children's Hospital of Pittsburgh", lat = 40.467202, lon = -79.953245,
        phone = "+1 412 692 5325", hours = "24/7", peds = 1, er = "yes", emergency = true)

    private fun List<NearestHelp.Pick>.of(kind: String) = firstOrNull { it.place.kind == kind }?.place

    @Test fun morgantown() {
        val picks = NearestHelp.pick(listOf(wvuCampus, ruby, upmc), oLat, oLon)
        val peds = picks.of(HelpKind.PEDS_ER)!!
        assertEquals("WVU Medicine Children's", peds.name)
        assertEquals("Ruby Memorial Hospital", peds.campus)
        assertEquals("ER on campus: Ruby Memorial Hospital — call ahead", NearestHelp.confidence(peds))
        assertEquals("Ruby Memorial Hospital", picks.of(HelpKind.ER)!!.name)
        assertNull(picks.of(HelpKind.PEDS_CLOSER))
    }

    @Test fun morgantownWithoutRuby() {
        val picks = NearestHelp.pick(listOf(wvuCampus.copy(campus = ""), upmc), oLat, oLon)
        val peds = picks.of(HelpKind.PEDS_ER)!!
        assertEquals("UPMC Children's Hospital of Pittsburgh", peds.name)
        assertEquals(1, peds.tier)
        assertEquals("~1 h 50 min (est.)", DriveEstimate.text(peds.driveS, peds.driveEst))
        val closer = picks.of(HelpKind.PEDS_CLOSER)!!
        assertEquals("WVU Medicine Children's", closer.name)
        assertEquals("ER not confirmed — call ahead", NearestHelp.confidence(closer))
        assertNull("no general ER in this scenario", picks.of(HelpKind.ER))
    }

    @Test fun poisonControlOnlyInTheUs() {
        assertEquals("1-800-222-1222", NearestHelp.poisonControl("US"))
        assertEquals("1-800-222-1222", NearestHelp.poisonControl("us"))
        assertNull(NearestHelp.poisonControl("CA"))
        assertNull(NearestHelp.poisonControl(""))
        assertNull(NearestHelp.poisonControl(null))
    }

    @Test fun theErRowIsNeverAPediatricSite() {
        val picks = NearestHelp.pick(listOf(wvuCampus, upmc), oLat, oLon)
        assertNull(picks.of(HelpKind.ER))
        val withGeneral = NearestHelp.pick(listOf(wvuCampus, upmc, ruby), oLat, oLon)
        assertEquals(HelpKind.ER, withGeneral.of(HelpKind.ER)!!.kind)
        assertNotEquals("peds_er", withGeneral.first { it.place.kind == HelpKind.ER }.from.cat)
    }

    @Test fun erPrefersAConfirmedEmergencyDepartmentAndSkipsNoEr() {
        val near = NearestHelp.Candidate(key = "n/1", cat = "health", name = "Test Clinic Hospital", lat = oLat + 0.001, lon = oLon)
        val noEr = NearestHelp.Candidate(key = "n/2", cat = "health", name = "Test Rehab Hospital", lat = oLat + 0.0005, lon = oLon, er = "no")
        assertEquals("Ruby Memorial Hospital", NearestHelp.pick(listOf(near, ruby, noEr), oLat, oLon).of(HelpKind.ER)!!.name)
        assertEquals("Test Clinic Hospital", NearestHelp.pick(listOf(near, noEr), oLat, oLon).of(HelpKind.ER)!!.name)
        assertNull(NearestHelp.pick(listOf(noEr), oLat, oLon).of(HelpKind.ER))
    }

    @Test fun urgentCareIsNeverAnEr() {
        val urg = NearestHelp.Candidate(key = "n/3", cat = "urgent", name = "Test Walk-In", lat = oLat, lon = oLon + 0.01, hours = "Mo-Fr 08:00-17:00")
        val kids = NearestHelp.Candidate(key = "n/4", cat = "peds_urgent", name = "Test Kids Express Care", lat = oLat, lon = oLon + 0.02, peds = 4)
        val picks = NearestHelp.pick(listOf(urg, kids), oLat, oLon, at = LocalDateTime.of(2026, 9, 26, 10, 0))   // a Saturday
        assertTrue(picks.of(HelpKind.URGENT)!!.notEr)
        assertEquals(false, picks.of(HelpKind.URGENT)!!.openNow)
        val k = picks.of(HelpKind.PEDS_URGENT)!!
        assertTrue(k.notEr); assertEquals(4, k.tier); assertEquals("Not an ER", NearestHelp.confidence(k))
        assertNull(k.openNow)
    }

    @Test fun theUrgentCareRowIsAnUrgentCareNotTheNearestClinic() {
        // the desktop's "urgent" category holds every clinic and doctor's office
        val chiro = NearestHelp.Candidate(key = "n/5", cat = "urgent", name = "Test Chiropractic", detail = "chiropractic", lat = oLat, lon = oLon + 0.001)
        val office = NearestHelp.Candidate(key = "n/6", cat = "urgent", name = "Test Family Practice", detail = "doctor's office", lat = oLat, lon = oLon + 0.002)
        val tagged = NearestHelp.Candidate(key = "n/7", cat = "urgent", name = "Test Health Center", detail = "urgent care", lat = oLat, lon = oLon + 0.05)
        val named = NearestHelp.Candidate(key = "n/8", cat = "urgent", name = "Test MedExpress", detail = "clinic", lat = oLat, lon = oLon + 0.03)
        assertEquals("Test MedExpress", NearestHelp.pick(listOf(chiro, office, tagged, named), oLat, oLon).of(HelpKind.URGENT)!!.name)
        assertEquals("Test Health Center", NearestHelp.pick(listOf(chiro, office, tagged), oLat, oLon).of(HelpKind.URGENT)!!.name)
        assertNull(NearestHelp.pick(listOf(chiro, office), oLat, oLon).of(HelpKind.URGENT))
        assertTrue(NearestHelp.isUrgentCare("Test After Hours Care", ""))
        assertTrue(NearestHelp.isUrgentCare("Test Immediate Care", "clinic"))
        assertTrue(NearestHelp.isUrgentCare("Test Walk-In Clinic", ""))
        assertFalse(NearestHelp.isUrgentCare("Test Urgently Needed Supplies", "clinic"))
        assertFalse(NearestHelp.isUrgentCare("Test Dental", "dentist"))
    }

    @Test fun routedDriveTimesAreKeptOnlyNearTheirOrigin() {
        val routed = upmc.copy(driveS = 5000, driveEst = false, fromLat = oLat + 0.005, fromLon = oLon)
        assertEquals(5000, NearestHelp.pick(listOf(routed), oLat, oLon).of(HelpKind.PEDS_ER)!!.driveS)
        val far = NearestHelp.pick(listOf(routed), oLat - 0.2, oLon).of(HelpKind.PEDS_ER)!!
        assertNotEquals(5000, far.driveS); assertTrue(far.driveEst)
        val estimate = upmc.copy(driveS = 5000, driveEst = true, fromLat = oLat, fromLon = oLon)
        assertEquals(6600, NearestHelp.pick(listOf(estimate), oLat, oLon).of(HelpKind.PEDS_ER)!!.driveS)
    }

    @Test fun ranks() {
        assertEquals(0, NearestHelp.rank(1, "")); assertEquals(0, NearestHelp.rank(2, "Test General"))
        assertEquals(1, NearestHelp.rank(2, "")); assertEquals(1, NearestHelp.rank(3, ""))
        assertEquals(-1, NearestHelp.rank(4, "")); assertEquals(-1, NearestHelp.rank(0, ""))
    }

    @Test fun placesComeOutInHelpKindOrder() {
        val police = NearestHelp.Candidate(key = "n/5", cat = "police", name = "Test Police", lat = oLat, lon = oLon + 0.001)
        val picks = NearestHelp.pick(listOf(police, ruby, wvuCampus), oLat, oLon).map { it.place.kind }
        assertEquals(listOf(HelpKind.PEDS_ER, HelpKind.ER, HelpKind.POLICE), picks)
        assertFalse(picks.contains(HelpKind.PEDS_CLOSER))
    }

    /** Cross-check against the shared fixture (tests/fixtures/pediatric_tags.json), when this checkout has it. */
    @Test fun sharedFixtureHelpPicks() {
        val f = listOf("../../tests/fixtures/pediatric_tags.json", "../tests/fixtures/pediatric_tags.json", "tests/fixtures/pediatric_tags.json").map { File(it) }.firstOrNull { it.exists() }
        assumeTrue("shared fixture not in this checkout", f != null)
        val root = JsonParser.parseString(f!!.readText()).asJsonObject
        for (sc in root.getAsJsonArray("scenarios").map { it.asJsonObject }) {
            val name = sc["name"].asString
            val els = sc.getAsJsonArray("elements").map { it.asJsonObject }.associateBy { "${it["type"].asString}/${it["id"].asLong}" }
            val cands = sc.getAsJsonArray("expect").map { it.asJsonObject }.filter { !(it.bool("dropped") ?: false) && it.has("cat") }.map { x ->
                val id = x["id"].asString; val el = els.getValue(id); val tags = el.getAsJsonObject("tags")
                val er = x.str("er") ?: ""
                NearestHelp.Candidate(key = id, cat = x["cat"].asString, name = tags.str("name") ?: "", lat = el["lat"].asDouble, lon = el["lon"].asDouble,
                    peds = x["peds"]?.asInt ?: 0, er = er, campus = x.str("campusEr") ?: "", emergency = x.bool("emergency") ?: (er == "yes"))
            }
            val o = sc.getAsJsonObject("origin")
            val picks = NearestHelp.pick(cands, o["lat"].asDouble, o["lon"].asDouble)
            val help = sc.getAsJsonObject("help")
            for ((jsonKey, kind) in listOf("pediatric" to HelpKind.PEDS_ER, "pediatricCloser" to HelpKind.PEDS_CLOSER, "hospital" to HelpKind.ER, "pediatricUrgent" to HelpKind.PEDS_URGENT)) {
                if (!help.has(jsonKey)) continue
                val want = help[jsonKey].takeUnless { it.isJsonNull }?.asString
                assertEquals("$name: $jsonKey", want, picks.of(kind)?.osmKey)
            }
        }
    }

    private fun JsonObject.str(k: String) = this[k]?.takeUnless { it.isJsonNull }?.asString
    private fun JsonObject.bool(k: String) = this[k]?.takeUnless { it.isJsonNull }?.asBoolean
}
