package org.sworrl.beaconfix.alpr

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.alpr.core.AlprJson
import org.sworrl.beaconfix.alpr.core.FrameMeta
import org.sworrl.beaconfix.alpr.core.Geofence
import org.sworrl.beaconfix.alpr.core.OnDevice
import org.sworrl.beaconfix.alpr.core.OnDevicePlate
import org.sworrl.beaconfix.alpr.core.Pairing
import org.sworrl.beaconfix.alpr.core.SlotDto
import java.time.ZoneId

class AlprCoreTest {
    @Test fun pairingPayload() {
        val p = Pairing.parse("falconeyez://pair?u=http%3A%2F%2F192.0.2.202%3A8900,http://100.64.0.5:8900/&t=ab+c/d%3D&n=RV%20FalconEyez")!!
        assertEquals(listOf("http://192.0.2.202:8900", "http://100.64.0.5:8900"), p.urls)
        assertEquals("ab+c/d=", p.token)
        assertEquals("RV FalconEyez", p.name)
        assertNull(Pairing.parse("falconeyez://pair?u=http://x:1"))                // no token
        assertNull(Pairing.parse("https://example.com/?u=http://x&t=1"))
        assertNull(Pairing.parse("falconeyez://pair?u=ftp://x&t=1"))
    }

    @Test fun urlNormalisation() {
        assertEquals("http://192.0.2.202:8900", Pairing.normalizeUrl("192.0.2.202:8900/"))
        assertEquals("https://rv.example.ts.net", Pairing.normalizeUrl("https://rv.example.ts.net/"))
        assertEquals("http://[fd7a:115c:a1e0::5]:8900", Pairing.normalizeUrl("http://[fd7a:115c:a1e0::5]:8900"))
        assertEquals("http://[fd7a:115c:a1e0::5]", Pairing.normalizeUrl("fd7a:115c:a1e0::5"))
        assertNull(Pairing.normalizeUrl(" "))
    }

    @Test fun metaUsesTheContractNames() {
        val m = FrameMeta(capturedAt = AlprJson.rfc3339(1_791_000_000_000L, ZoneId.of("America/Chicago")), lat = 32.7, lon = -97.1,
            speedMps = 12.3, cropBox = listOf(0.1, 0.2, 0.3, 0.4), frameSize = listOf(4080, 3072),
            onDevice = OnDevice(listOf(OnDevicePlate(listOf(0.4, 0.6, 0.6, 0.7), "ABC1234", 0.97, listOf(listOf(SlotDto("A", 0.98), SlotDto("Y", 0.01)))))))
        val s = AlprJson.json.encodeToString(FrameMeta.serializer(), m)
        for (k in listOf("\"captured_at\"", "\"speed_mps\"", "\"crop_box\"", "\"frame_size\"", "\"reason\":\"plate\"", "\"on_device\"", "\"slots\":[[{\"c\":\"A\",\"p\":0.98}")) assertTrue("$k in $s", s.contains(k))
        assertFalse("nulls omitted: $s", s.contains("heading_deg") || s.contains("null"))
        assertTrue(m.capturedAt, Regex("""\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}[+-]\d\d:\d\d""").matches(m.capturedAt))
        assertTrue(m.capturedAt.endsWith("-05:00") || m.capturedAt.endsWith("-06:00"))
    }

    @Test fun geofenceBlocksMaineNewHampshireArkansas() {
        val none = emptySet<String>()
        for ((name, ll) in mapOf("Little Rock" to (34.75 to -92.29), "Fayetteville AR" to (36.06 to -94.16), "Jonesboro" to (35.84 to -90.70),
                "Concord NH" to (43.21 to -71.54), "Lebanon NH" to (43.64 to -72.25), "Portsmouth NH" to (43.07 to -70.76),
                "Portland ME" to (43.66 to -70.26), "Bangor" to (44.80 to -68.77), "Caribou" to (46.86 to -68.01))) {
            val v = Geofence.check(none, ll.first, ll.second, null)
            assertTrue("$name blocked", v.blocked)
        }
        for ((name, ll) in mapOf("Memphis" to (35.15 to -90.05), "Dallas" to (32.78 to -96.80), "Burlington VT" to (44.48 to -73.21),
                "Woodstock VT" to (43.62 to -72.52), "Boston" to (42.36 to -71.06), "Tulsa" to (36.15 to -95.99), "Shreveport" to (32.52 to -93.75),
                "Montreal" to (45.50 to -73.57), "Saint John NB" to (45.27 to -66.06))) {
            assertFalse("$name allowed", Geofence.check(none, ll.first, ll.second, null).blocked)
        }
    }

    @Test fun geofenceRegionsAndGeocoder() {
        val v = Geofence.check(setOf("TX"), 32.78, -96.80, "TX")
        assertTrue(v.blocked); assertEquals("TX", v.region)
        assertFalse(Geofence.check(emptySet(), 43.21, -71.54, "VT").blocked)       // a fresh geocoder answer wins
        assertTrue(Geofence.check(emptySet(), 0.0, 0.0, "NH").blocked)             // the three are always blocked
        assertEquals("TX", Geofence.stateCode("Texas", "US"))
        assertEquals("NH", Geofence.stateCode("new hampshire", null))
        assertNull(Geofence.stateCode("Ontario", "CA"))
    }
}
