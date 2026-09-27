package org.sworrl.beaconfix.backup

import com.google.gson.JsonParser
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.db.AnchorEntity
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.data.db.ObservationEntity
import org.sworrl.beaconfix.importer.Importers
import java.io.StringWriter
import java.time.ZoneId

/** Synthetic rows only: MACs 02:00:00:00:00:0x, positions around 40.0, -75.0. */
class BackupRoundTripTest {
    private val zone = ZoneId.of("America/New_York")
    private val t0 = 1_790_000_000_000L
    private fun mac(i: Int) = String.format("02:00:00:00:00:%02X", i)

    private val aps = listOf(
        ApEntity(bssid = mac(1), ssid = "Test Net", freq = 2412, band = "2.4", ch = 1, firstSeen = t0, lastSeen = t0 + 60_000, timesSeen = 3, security = "wpa2",
            lat = 40.001, lon = -75.001, acc = 12.0, posSource = "observed"),
        ApEntity(bssid = mac(2), ssid = "locations", security = "open", lat = 40.002, lon = -75.002, acc = 30.0, posSource = "desktop"),   // a trap SSID
        ApEntity(bssid = mac(3), ssid = "<gpx> & 'kml'", security = "wpa3"),
    )
    private val obs = listOf(
        ObservationEntity(id = 1, bssid = mac(1), time = t0, lat = 40.0, lon = -75.0, acc = 8.0, dbm = -61, source = "gps"),
        ObservationEntity(id = 2, bssid = mac(2), time = t0 + 30_000, lat = 40.0001, lon = -75.0001, acc = 9.0, dbm = -70, source = "fused", synced = true),
        ObservationEntity(id = 3, bssid = mac(3), time = t0 + 60_000, lat = 40.0002, lon = -75.0002, acc = 10.0, dbm = -80, source = "desktop", remote = true),   // pulled: left out
    )
    private val fixes = listOf(
        FixEntity(id = 1, time = t0, lat = 40.0, lon = -75.0, acc = 8.0, source = "phone-gps", provider = "fused"),
        FixEntity(id = 2, time = t0 + 120_000, lat = 40.01, lon = -75.01, acc = 15.0, source = "phone-wifi", place = "Testville"),
        FixEntity(id = 3, time = t0 + 180_000, lat = 40.02, lon = -75.02, acc = 20.0, source = "desktop"),   // the desktop's own track: left out
    )
    private val anchors = listOf(
        AnchorEntity(id = "a1", json = """{"id":"a1","name":"Test roof antenna","kind":"antenna","lat":40.0,"lon":-75.0,"accM":0.5,"rv":true,"placedAt":"2026-09-01T10:00:00Z"}""",
            name = "Test roof antenna", kind = "antenna", lat = 40.0, lon = -75.0, rv = true, ref = false, deleted = false, placedAt = "2026-09-01T10:00:00Z", seq = 1, dirty = false),
        AnchorEntity(id = "bad", json = "not json", name = "", kind = "", lat = 0.0, lon = 0.0, rv = false, ref = false, deleted = false, placedAt = "", seq = 0, dirty = false),
    )

    private fun backup(): Pair<String, BackupWriter.Counts> {
        val sw = StringWriter()
        val c = BackupWriter.write(sw, aps.asSequence(), obs.asSequence(), fixes.asSequence(), anchors.asSequence(), device = "Test phone", now = t0 + 3_600_000, zone = zone)
        return sw.toString() to c
    }

    @Test fun detectsAsABeaconFixExportOnThePhone() {
        val (json, _) = backup()
        assertEquals("beaconfix", Importers.detect(json.take(4096), "phone-2026-09-27.json"))
        assertTrue(json.startsWith("{\"beaconfix\":\"mapdb\",\"aps\":["))       // the desktop needs "beaconfix"; "aps" comes right after (F6)
    }

    @Test fun parsesBackToTheSameCounts() {
        val (json, c) = backup()
        assertEquals(BackupWriter.Counts(aps = 3, observations = 2, fixes = 2, anchors = 1), c)
        val r = Importers.parse("beaconfix", json.byteInputStream())
        assertEquals(c.aps, r.notes.count { it.startsWith("ap:") })
        assertEquals(c.observations, r.observations.size)
        assertEquals(c.fixes, r.positions.size)
        assertTrue("ap:${mac(2)}|locations|open" in r.notes)                 // the escaped SSID reads back unchanged
        assertTrue("ap:${mac(3)}|<gpx> & 'kml'|wpa3" in r.notes)
    }

    @Test fun observationTimesReadBackOnThePhone() {
        val sw = StringWriter()
        BackupWriter.write(sw, aps.asSequence(), obs.asSequence(), fixes.asSequence(), emptySequence(), device = "Test phone")   // this machine's zone, as on the phone
        val r = Importers.parse("beaconfix", sw.toString().byteInputStream())
        assertEquals(listOf(t0, t0 + 30_000), r.observations.map { it.time })
        assertEquals(listOf(t0, t0 + 120_000), r.positions.map { it.time })
        assertEquals(listOf(-61, -70), r.observations.map { it.dbm })
    }

    @Test fun timesRoundTripInTheSyncFormat() {
        val (json, _) = backup()
        val o = JsonParser.parseString(json).asJsonObject
        val first = o.getAsJsonArray("observations")[0].asJsonObject
        assertEquals(BackupWriter.iso(t0, zone), first["time"].asString)
        assertFalse(first["time"].asString.contains("Z") || first["time"].asString.contains("+"))
    }

    @Test fun rowsUseOnlyTheDesktopsColumnNames() {
        val o = JsonParser.parseString(backup().first).asJsonObject
        val apCols = setOf("bssid", "ssid", "band", "ch", "freq", "first_seen", "last_seen", "times_seen", "security", "peer_lat", "peer_lon", "peer_acc", "peer_from")
        val obsCols = setOf("bssid", "time", "lat", "lon", "acc", "dbm", "fix_source", "device")
        val fixCols = setOf("time", "lat", "lon", "acc", "source", "provider", "place", "device")
        for (a in o.getAsJsonArray("aps")) assertTrue(a.asJsonObject.keySet().toString(), apCols.containsAll(a.asJsonObject.keySet()))
        for (a in o.getAsJsonArray("observations")) assertEquals(obsCols, a.asJsonObject.keySet())
        for (a in o.getAsJsonArray("fixes")) {
            assertTrue(fixCols.containsAll(a.asJsonObject.keySet()))
            assertEquals("Test phone", a.asJsonObject["device"].asString)    // never the desktop's own track
        }
        // our own fit becomes the desktop's "peer" position; a position the phone got from a desktop does not
        val ap1 = o.getAsJsonArray("aps")[0].asJsonObject; val ap2 = o.getAsJsonArray("aps")[1].asJsonObject
        assertEquals(40.001, ap1["peer_lat"].asDouble, 1e-9)
        assertFalse(ap2.has("peer_lat"))
        assertEquals("mapdb", o["beaconfix"].asString)
        assertEquals("beaconfix-export", o["format"].asString)
        assertEquals(1, o["version"].asInt)
    }

    @Test fun anchorsAreCopiedAndBrokenOnesSkipped() {
        val arr = JsonParser.parseString(backup().first).asJsonObject.getAsJsonArray("anchors")
        assertEquals(1, arr.size())
        assertEquals("Test roof antenna", arr[0].asJsonObject["name"].asString)
        assertTrue(arr[0].asJsonObject["rv"].asBoolean)
    }

    @Test fun trapWordsAndHtmlNeverAppearVerbatim() {
        val (json, _) = backup()
        assertFalse(json.contains("\"locations\""))
        assertFalse(json.contains("<gpx")); assertFalse(json.contains("<kml"))
    }

    @Test fun noSecrets() {
        val json = backup().first.lowercase()
        for (w in listOf("seed", "token", "secret")) assertFalse(w, json.contains(w))
    }
}
