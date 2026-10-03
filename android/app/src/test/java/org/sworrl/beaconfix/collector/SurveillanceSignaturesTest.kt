// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.collector

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.File

/**
 * The shared rule set (data/signatures/surveillance.json) over the shared cases (tests/fixtures/surveillance_cases.json):
 * the desktop's tests/flockdetector_test.cpp runs the very same cases, so both platforms agree. The paths come from
 * app/build.gradle.kts (system properties), the files are never copied by hand.
 */
class SurveillanceSignaturesTest {
    private val sigFile = File(System.getProperty("beaconfix.signatures") ?: "../../data/signatures/surveillance.json")
    private val casesFile = File(System.getProperty("beaconfix.signatureCases") ?: "../../tests/fixtures/surveillance_cases.json")
    private val sigs by lazy { SurveillanceSignatures.parse(sigFile.readText()) }
    private val cases by lazy { Json.parseToJsonElement(casesFile.readText()).jsonObject }

    private fun JsonObject.s(k: String) = (this[k] as? JsonPrimitive)?.content ?: ""
    private fun JsonObject.b(k: String) = (this[k] as? JsonPrimitive)?.booleanOrNull ?: false
    private fun JsonObject.list(k: String) = (this[k] as? JsonArray)?.map { it.jsonPrimitive.content } ?: emptyList()

    private fun check(c: JsonObject, d: SurveillanceSignatures.Detection) {
        val name = c.s("case")
        val tier = c["tier"]!!.jsonPrimitive.int
        assertEquals("$name: tier (${d.rules})", tier, d.tier)
        if (tier >= 0) assertEquals("$name: class", c.s("class"), d.cls)
        assertEquals("$name: isFlock", c.b("isFlock"), d.isFlock)
        assertEquals("$name: informational", c.b("informational"), d.informational)
        if (tier < 0) assertTrue("$name: no match has no method", d.method.isEmpty() && d.confidence == 0)
    }

    @Test
    fun sharedWifiCases() {
        val wifi = cases["wifi"]!!.jsonArray
        assertTrue("Wi-Fi cases present", wifi.size >= 30)
        for (v in wifi) {
            val c = v.jsonObject
            check(c, sigs.evaluateWifi(c.s("bssid"), c.s("ssid"), c.list("probes")))
        }
    }

    @Test
    fun sharedBleCases() {
        val ble = cases["ble"]!!.jsonArray
        assertTrue("BLE cases present", ble.size >= 20)
        for (v in ble) {
            val c = v.jsonObject
            val companies = (c["companies"] as? JsonArray)?.map { x ->
                val o = x.jsonObject
                SurveillanceSignatures.BleCompany(o["id"]!!.jsonPrimitive.int, o.s("ascii").toByteArray(Charsets.ISO_8859_1))
            } ?: emptyList()
            check(c, sigs.evaluateBle(c.s("mac"), c.s("name"), c.list("services"), companies))
        }
    }

    @Test
    fun detectionCarriesModelMethodAndConfidence() {
        val d = sigs.evaluateWifi("70:C9:4E:00:00:01", "Flock-112233")
        assertEquals("wifi_mac+ssid", d.method)
        assertEquals("Falcon", d.model)
        assertEquals("alpr", d.cameraType)
        assertEquals(85, d.confidence)
        val r = sigs.evaluateBle("C6:11:22:33:44:55", "", listOf("3100"))
        assertEquals("Raven", r.model)
        assertEquals("not_camera", r.cameraType)
        assertEquals("ble_uuid", r.method)
        val x = sigs.evaluateWifi("00:03:7F:50:00:01", "")
        assertEquals(98, x.confidence)
        assertTrue(x.details.contains("tier 4"))
    }

    @Test
    fun loaderRefusesWhatMustNeverMatch() {
        val root = Json.parseToJsonElement(sigFile.readText()).jsonObject
        fun withMac(prefix: String): String {
            val mac = JsonArray(root["mac"]!!.jsonArray + JsonObject(mapOf("prefix" to JsonPrimitive(prefix), "class" to JsonPrimitive("flock"), "tier" to JsonPrimitive(0))))
            return JsonObject(root + ("mac" to mac)).toString()
        }
        try { SurveillanceSignatures.parse(withMac("70:B3:D5")); fail("a bare 70:B3:D5 entry must be refused") } catch (e: IllegalArgumentException) { assertTrue(e.message!!.contains("MA-S")) }
        try { SurveillanceSignatures.parse(withMac("12:34:56:78")); fail("a 32-bit prefix must be refused") } catch (_: IllegalArgumentException) {}
        try { SurveillanceSignatures.parse("""{"format":2}"""); fail("an unknown format must be refused") } catch (_: IllegalArgumentException) {}
    }

    @Test
    fun objectWithoutRulesDetectsNothing() {
        val d = FlockDetectorKotlin.evaluateWifi("B4:1E:52:12:34:56", "Flock-123456")
        assertFalse(d.isFlock)   // nothing loaded in a JVM test: no false alarm, no crash
        FlockDetectorKotlin.use(sigs)
        assertTrue(FlockDetectorKotlin.evaluateWifi("B4:1E:52:12:34:56", "Flock-123456").isFlock)
        assertEquals(sigs.version, FlockDetectorKotlin.version)
    }

    @Test
    fun macAndUuidNotation() {
        assertEquals("B41E52123456", SurveillanceSignatures.normalizeMac("b4-1e-52-12-34-56"))
        assertEquals("B41E52123456", SurveillanceSignatures.normalizeMac("b41e.5212.3456"))
        assertEquals("", SurveillanceSignatures.normalizeMac("B4:1E:52"))
        assertEquals("FE6B", SurveillanceSignatures.normalizeUuid("0000FE6B-0000-1000-8000-00805F9B34FB"))
        assertEquals("e8ccbb38-9532-46a8-9fe5-1814df172e6f", SurveillanceSignatures.normalizeUuid("E8CCBB38-9532-46A8-9FE5-1814DF172E6F"))
    }
}
