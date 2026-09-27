// POST /api/v1/ranging wire shape (docs/API.md) and the RTT burst backoff (RangingRepository.backoffMs).
package org.sworrl.beaconfix.ranging

import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.BleSample
import org.sworrl.beaconfix.data.api.RangingFix
import org.sworrl.beaconfix.data.api.RangingPost

class RangingWireTest {
    private fun encode(p: RangingPost): JsonObject = ApiFactory.json.parseToJsonElement(ApiFactory.json.encodeToString(p)).jsonObject

    @Test fun emptyAndDefaultValuesAreSent() {
        val o = encode(RangingPost("Pixel", "2026-09-27T12:00:00Z", emptyList(), listOf(BleSample(-80, null, 127, 1L)), emptyList(),
            null, false, RangingFix(1.0, 2.0, 3.0, 4L, "gps"), null))
        for (k in listOf("device", "time", "rtt", "ble", "wifi", "moving", "fix")) assertTrue("missing $k", k in o)
        assertEquals(0, o["rtt"]!!.jsonArray.size)
        assertEquals("false", o["moving"].toString())
        val b = o["ble"]!!.jsonArray[0].jsonObject
        assertEquals("127", b["txPower"].toString())
        assertFalse("channel is optional", "channel" in b)
        assertEquals("\"gps\"", o["fix"]!!.jsonObject["source"].toString())
        assertFalse("baro is optional", "baro" in o)
        assertFalse("rttState is optional", "rttState" in o)
    }

    @Test fun rttStateIsSentWhenSet() {
        val o = encode(RangingPost("Pixel", "t", emptyList(), emptyList(), emptyList(), null, true, null, RttState.DOZE))
        assertEquals("\"doze\"", o["rttState"].toString())
        assertEquals("true", o["moving"].toString())
    }

    @Test fun backoffTwoQuickRetriesThenDoublingToTwoMinutes() {
        assertEquals(listOf(0L, 0L, 5_000L, 10_000L, 20_000L, 40_000L, 80_000L, 120_000L, 120_000L, 120_000L),
            (1..10).map { RangingRepository.backoffMs(it) })
    }
}
