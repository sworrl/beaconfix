package org.sworrl.beaconfix

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.importer.Importers

/** Small synthetic fixtures written from the format descriptions — no real export data. */
class ImportersTest {
    private fun s(text: String) = text.byteInputStream()

    @Test fun detect() {
        assertEquals("wigle", Importers.detect("WigleWifi-1.6,appRelease=2.9\nMAC,SSID", "x.csv"))
        assertEquals("timeline", Importers.detect("{\"semanticSegments\":[],\"rawSignals\":[]}", "Timeline.json"))
        assertEquals("records", Importers.detect("{\"locations\":[{", "Records.json"))
        assertEquals("semantic", Importers.detect("{\"timelineObjects\":[", "2024_JANUARY.json"))
        assertEquals("gpx", Importers.detect("<?xml version=\"1.0\"?><gpx", "walk.gpx"))
        assertEquals("kml", Importers.detect("<kml", "history.kml"))
        assertEquals("beaconfix", Importers.detect("{\"aps\":[],\"observations\":[]}", "export.json"))
    }

    @Test fun macAndTime() {
        assertEquals("00:11:22:33:44:55", Importers.parseMac("73588229205"))          // decimal int64
        assertEquals("AA:BB:CC:DD:EE:FF", Importers.parseMac("aabbccddeeff"))
        assertEquals("AA:BB:CC:DD:EE:FF", Importers.parseMac("aa:bb:cc:dd:ee:ff"))
        assertEquals(1700000000000L, Importers.parseTime("1700000000000"))
        assertEquals(1700000000000L, Importers.parseTime("1700000000"))
        assertEquals(1700000000000L, Importers.parseTime("2023-11-14T22:13:20.000Z"))
        assertEquals(40.00 to -75.06, Importers.parseLatLng("40.00°, -75.06°"))
    }

    @Test fun timelineJson() {
        val json = """{"semanticSegments":[{"startTime":"2026-09-01T10:00:00.000Z","endTime":"2026-09-01T12:00:00.000Z","visit":{"topCandidate":{"placeLocation":{"latLng":"40.0030°, -75.0680°"},"placeId":"ChIJx","semanticType":"HOME"}}},
          {"startTime":"2026-09-01T12:00:00.000Z","endTime":"2026-09-01T12:30:00.000Z","timelinePath":[{"point":"40.0031°, -75.0681°","durationMinutesOffsetFromStartTime":"5"},{"point":"40.0040°, -75.0690°","durationMinutesOffsetFromStartTime":"15"}]}],
          "rawSignals":[{"position":{"LatLng":"40.0030°, -75.0680°","accuracyMeters":12,"altitudeMeters":120.5,"source":"WIFI","timestamp":"2026-09-01T10:05:00.000Z"}},
                        {"wifiScan":{"deliveryTime":"2026-09-01T10:05:20.000Z","devicesRecords":[{"mac":73588229205,"rawRssi":-61},{"mac":"aa:bb:cc:dd:ee:ff","rawRssi":-72}]}},
                        {"position":{"LatLng":"40.0032°, -75.0682°","accuracyMeters":8,"source":"GPS","timestamp":"2026-09-01T10:06:00.000Z"}},
                        {"wifiScan":{"deliveryTime":"2026-09-01T10:30:00.000Z","devicesRecords":[{"mac":1,"rawRssi":-50}]}}]}"""
        val r = Importers.timeline(s(json)) { _, _ -> }
        assertEquals(4, r.positions.size)            // 2 raw + 2 path points
        assertEquals(2, r.scans.size); assertEquals(1, r.stops.size); assertEquals("ChIJx", r.stops[0].placeId)
        assertEquals(120.5, r.positions.first { it.alt != null }.alt!!, 0.01)
        val made = Importers.pairScans(r)
        assertEquals(2, made)                          // the 10:30 scan has no position within ±60 s
        val o = r.observations.first { it.bssid == "00:11:22:33:44:55" }
        assertTrue(o.lat > 40.0030 && o.lat < 40.0032)  // interpolated between 10:05 and 10:06
        assertEquals(12.0, o.acc, 0.01)
    }

    @Test fun recordsAndSemantic() {
        val rec = """{"locations":[{"latitudeE7":400030000,"longitudeE7":-750680000,"accuracy":20,"timestamp":"2023-05-01T08:00:00Z","altitude":120,
                       "wifiScan":{"accessPoints":[{"mac":"73588229205","strength":-55}]}},{"latitudeE7":400031000,"longitudeE7":-750681000,"accuracy":300,"timestampMs":"1682928060000"}]}"""
        val r = Importers.records(s(rec)) { _, _ -> }
        assertEquals(2, r.positions.size); assertEquals(1, r.scans.size); assertEquals(1, Importers.pairScans(r))
        val sem = """{"timelineObjects":[{"placeVisit":{"location":{"latitudeE7":400030000,"longitudeE7":-750680000,"name":"Camp","placeId":"P1"},"duration":{"startTimestamp":"2023-05-01T08:00:00Z","endTimestamp":"2023-05-01T09:00:00Z"}}},
                     {"activitySegment":{"duration":{"startTimestamp":"2023-05-01T09:00:00Z"},"waypointPath":{"waypoints":[{"latE7":400040000,"lngE7":-750690000},{"latE7":400050000,"lngE7":-750700000}]}}}]}"""
        val r2 = Importers.semantic(s(sem)) { _, _ -> }
        assertEquals(1, r2.stops.size); assertEquals("Camp", r2.stops[0].name); assertEquals(2, r2.positions.size)
    }

    @Test fun wigleCsv() {
        val csv = "WigleWifi-1.6,appRelease=2.90,model=Pixel,release=15,device=x,display=y,board=z,brand=google,star=Sol,body=3,subBody=0\n" +
            "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n" +
            "aa:bb:cc:dd:ee:01,Example Wi-Fi,[WPA2-PSK-CCMP][ESS],2026-09-01 10:05:00,6,2437,-61,40.0030,-75.0680,120,8,,,WIFI\n" +
            "aa:bb:cc:dd:ee:02,Open Cafe,[ESS],2026-09-01 10:05:01,44,5220,-70,40.0031,-75.0681,120,8,,,WIFI\n" +
            "11:22:33:44:55:66,Speaker,,2026-09-01 10:05:02,0,0,-80,40.0031,-75.0681,120,8,,,BT\n"
        val r = Importers.wigle(s(csv)) { _, _ -> }
        assertEquals(2, r.observations.size)
        assertEquals("wpa2", r.observations[0].security); assertEquals("open", r.observations[1].security); assertEquals(2437, r.observations[0].freq)
    }

    @Test fun gpxAndKml() {
        val gpx = """<?xml version="1.0"?><gpx><trk><trkseg><trkpt lat="40.0030" lon="-75.0680"><ele>120</ele><time>2026-09-01T10:00:00Z</time></trkpt><trkpt lat="40.0031" lon="-75.0681"><time>2026-09-01T10:01:00Z</time></trkpt></trkseg></trk></gpx>"""
        val r = Importers.gpx(s(gpx)) { _, _ -> }
        assertEquals(2, r.positions.size); assertEquals(120.0, r.positions[0].alt!!, 0.01)
        val kml = """<kml><Document><Placemark><LineString><coordinates>-75.0680,40.0030,120 -75.0681,40.0031,121</coordinates></LineString></Placemark></Document></kml>"""
        val k = Importers.kml(s(kml)) { _, _ -> }
        assertEquals(2, k.positions.size); assertEquals(40.0031, k.positions[1].lat, 1e-6)
    }

    @Test fun beaconfixExport() {
        val j = """{"aps":[{"bssid":"aa:bb:cc:dd:ee:01","ssid":"Example","security":"wpa2"}],"observations":[{"bssid":"aa:bb:cc:dd:ee:01","time":"2026-09-01T10:00:00","lat":40.003,"lon":-75.068,"acc":40,"dbm":-60}],"fixes":[{"time":"2026-09-01T10:00:00","lat":40.003,"lon":-75.068,"acc":40,"place":"Camp"}]}"""
        val r = Importers.beaconfix(s(j)) { _, _ -> }
        assertEquals(1, r.observations.size); assertEquals(1, r.positions.size); assertEquals("Camp", r.stops[0].name); assertEquals(1, r.notes.size)
    }
}
