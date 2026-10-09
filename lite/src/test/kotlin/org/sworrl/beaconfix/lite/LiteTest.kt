package org.sworrl.beaconfix.lite

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import kotlin.math.cos
import kotlin.math.sin

class LiteTest {
    // a made-up town: positions here are synthetic
    private val lat0 = 45.0
    private val lon0 = 7.0
    private val fr = Frame(lat0, lon0)

    private fun rng(seed: Long) = java.util.Random(seed)

    /** APs on a ring around the origin and the level each would be heard at from (tx, ty) metres. */
    private class Sim(val key: Long, val lat: Double, val lon: Double, val rssi: Int, val freq: Int)

    private fun town(n: Int, seed: Long, tx: Double = 0.0, ty: Double = 0.0, noiseDb: Double = 4.0, radius: Double = 60.0): List<Sim> {
        val r = rng(seed)
        return (0 until n).map { i ->
            val ang = 2 * Math.PI * i / n + r.nextDouble() * 0.5
            val dist = radius * (0.4 + r.nextDouble())
            val ax = dist * cos(ang); val ay = dist * sin(ang)
            val freq = if (i % 3 == 0) 5180 else 2437
            val d = maxOf(1.0, Math.hypot(ax - tx, ay - ty))
            val rssi = (SelfLocate.p0(freq) - 10 * SelfLocate.PATH_LOSS * Math.log10(d) + r.nextGaussian() * noiseDb).toInt().coerceIn(-99, -20)
            Sim(0x00112233_4400L + i, fr.lat(ay), fr.lon(ax), rssi, freq)
        }
    }

    @Test fun selfLocateFindsTheSpotAndDropsAMovedAp() {
        var worst = 0.0
        for (seed in 1L..20L) {
            val aps = town(10, seed)
            val known = aps.map { SelfLocate.Known(it.lat, it.lon, 8.0, it.rssi, it.freq) }.toMutableList()
            val r = SelfLocate.solve(known)
            assertTrue(r.valid)
            worst = maxOf(worst, Geo.distanceM(lat0, lon0, r.lat, r.lon))
        }
        println("selfLocate: worst error over 20 towns ${"%.1f".format(worst)} m")
        assertTrue("worst error $worst m", worst < 30)

        // one AP was mapped 2 km away (it moved house): it must be dropped, not drag the fix
        val aps = town(10, 7)
        val known = aps.mapIndexed { i, a ->
            if (i == 3) SelfLocate.Known(fr.lat(2000.0), fr.lon(0.0), 8.0, -45, a.freq) else SelfLocate.Known(a.lat, a.lon, 8.0, a.rssi, a.freq)
        }
        val r = SelfLocate.solve(known)
        println("moved AP: integrity ${r.integrity}, excluded ${r.excluded}, error ${"%.1f".format(Geo.distanceM(lat0, lon0, r.lat, r.lon))} m, acc ${r.accM.toInt()}")
        assertTrue(r.integrity, r.integrity == "repaired" || r.integrity == "ok")
        assertTrue("excluded ${r.excluded}", 3 in r.excluded)
        assertTrue("error ${Geo.distanceM(lat0, lon0, r.lat, r.lon)}", Geo.distanceM(lat0, lon0, r.lat, r.lon) < 30)
        assertTrue(r.r95M >= r.accM)
    }

    @Test fun macsRoundTrip() {
        val k = Mac.key("02:3a:c4:09:42:EC")
        assertEquals(0x023ac40942ecL, k)
        assertEquals(k, Mac.key("02:3a:c4:9:42:ec"))
        assertEquals(k, Mac.key("02-3a-c4-09-42-ec"))
        assertEquals("02:3a:c4:09:42:ec", Mac.text(k))
        assertEquals("2:3a:c4:9:42:ec", Mac.apple(k))
        assertEquals(-1L, Mac.key("02:3a:c4:09:42"))
        assertEquals(-1L, Mac.key("hello"))
        assertEquals(-1L, Mac.key("02:3a:c4:09:42:ec:01"))
    }

    @Test fun filterDropsOptOutsAndTravellers() {
        assertTrue(ApFilter.usable(Heard("00:11:22:33:44:55", -60, 2437, "Stanford Library")))
        assertTrue(!ApFilter.usable(Heard("00:11:22:33:44:55", -60, 2437, "cafe_nomap")))
        assertTrue(!ApFilter.usable(Heard("00:11:22:33:44:55", -60, 2437, "Jim's iPhone")))
        assertTrue(!ApFilter.usable(Heard("00:11:22:33:44:55", -60, 2437, "STARLINK")))
        assertTrue(!ApFilter.usable(Heard("00:11:22:33:44:55", -60, 2437, "HomeNet"), listOf("homenet")))
        assertTrue(ApFilter.usable(Heard("00:11:22:33:44:55", -60, 2437, "")))
    }

    @Test fun cacheSurvivesRestartJournalAndATornWrite() {
        val dir = Files.createTempDirectory("bfl").toFile()
        val c = BeaconCache(dir)
        assertTrue(c.put(1L, 45.1, 7.1, 20.0, BeaconCache.SERVICE))
        assertTrue(!c.put(1L, 45.1, 7.1, 20.0, BeaconCache.SERVICE))       // unchanged: nothing to write
        c.markUnknown(2L)
        assertEquals(2, c.flush())
        assertEquals(0, c.flush())                                         // nothing changed since
        // a torn record at the end of the journal (power cut mid-append) is ignored
        File(dir, "bflite-aps.jnl").appendBytes(byteArrayOf(1, 2, 3))
        val d = BeaconCache(dir)
        assertEquals(45.1, d.placed(1L)!!.lat, 1e-7)
        assertEquals(BeaconCache.UNKNOWN, d[2L]!!.kind)
        assertTrue(!d.shouldAsk(2L))
        assertTrue(d.shouldAsk(2L, BeaconCache.today() + BeaconCache.RETRY_DAYS))
        // an import never overwrites a service placement
        assertTrue(!d.put(1L, 46.0, 8.0, 5.0, BeaconCache.IMPORTED))
        // compaction folds the journal into the main file
        d.compact()
        assertTrue(!File(dir, "bflite-aps.jnl").exists())
        assertEquals(45.1, BeaconCache(dir).placed(1L)!!.lat, 1e-7)
        dir.deleteRecursively()
    }

    @Test fun appleWireFormatRoundTrips() {
        val req = AppleWps.request(listOf(0x0011223344aaL))
        assertTrue(String(req, Charsets.ISO_8859_1).contains("0:11:22:33:44:aa"))
        val reply = fakeAppleReply(listOf(Triple(0x0011223344aaL, -33.8688, 151.2093), Triple(0x0011223344bbL, 48.8566, 2.3522)))
        val p = AppleWps.parse(reply)
        assertEquals(2, p.size)
        assertEquals(-33.8688, p[0].lat, 1e-7); assertEquals(151.2093, p[0].lon, 1e-7)
        assertEquals(2.3522, p[1].lon, 1e-7)
    }

    @Test fun beaconDbRefusesAGeoIpGuess() {
        assertNotNull(BeaconDb.parse("{\"location\":{\"lat\":45.1,\"lng\":7.1},\"accuracy\":35.0}"))
        assertNull(BeaconDb.parse("{\"location\":{\"lat\":45.1,\"lng\":7.1},\"accuracy\":25000}"))
        assertNull(BeaconDb.parse("{\"error\":{\"code\":404,\"message\":\"Not found\"}}"))
        assertTrue(BeaconDb.request(listOf(Heard("00:11:22:33:44:55", -60, 2437))).contains("\"considerIp\":false"))
    }

    @Test fun locatorAsksOnceThenReusesThenSolvesOffline() {
        val dir = Files.createTempDirectory("bfl").toFile()
        val area = town(30, 3, radius = 150.0)          // the service knows all 30; we hear the 10 nearest
        var requests = 0
        val http = Http { url, _, _, body ->
            if (!url.contains("apple")) return@Http null
            requests++
            fakeAppleReply(area.map { Triple(it.key, it.lat, it.lon) })   // every neighbour, like Apple
        }
        val heard = area.sortedByDescending { it.rssi }.take(10).map { Heard(Mac.text(it.key), it.rssi, it.freq, "net${it.key and 0xff}") }
        val loc = LiteLocator(dir, http)
        val f1 = loc.locate(heard, now = 1_000_000L)!!
        println("locator test: error ${"%.1f".format(Geo.distanceM(lat0, lon0, f1.lat, f1.lon))} m, acc ${f1.accM.toInt()} m, r95 ${f1.r95M.toInt()} m, ${f1.integrity}")
        assertEquals("apple", f1.source); assertEquals(1, f1.lookups); assertEquals(1, requests)
        // APs 60–210 m off and 4 dB of noise: the miss must sit inside the radius the fix claims
        assertTrue("error ${Geo.distanceM(lat0, lon0, f1.lat, f1.lon)} vs r95 ${f1.r95M}", Geo.distanceM(lat0, lon0, f1.lat, f1.lon) < minOf(f1.r95M, 70.0))

        // the same APs a minute later: the same answer, no work
        val f2 = loc.locate(heard, now = 1_060_000L)!!
        assertEquals("reuse", f2.source); assertEquals(1, requests)

        // a different corner of the same area: the neighbours Apple volunteered place it offline
        val other = area.sortedBy { it.rssi }.take(8).map { Heard(Mac.text(it.key), it.rssi, it.freq, "") }
        val f3 = loc.locate(other, now = 1_120_000L)!!
        assertEquals("cache", f3.source); assertEquals(0, f3.lookups); assertEquals(1, requests)

        // after a restart the last fix is still there to reuse, with nothing re-read from the network
        val again = LiteLocator(dir, http)
        val f4 = again.locate(other, now = 1_180_000L)!!
        assertEquals("reuse", f4.source); assertEquals(1, requests)
        dir.deleteRecursively()
    }

    @Test fun anApMappedFarAwayIsStruckThenIgnored() {
        val dir = Files.createTempDirectory("bfl").toFile()
        val aps = town(6, 11)
        val loc = LiteLocator(dir, null)
        for (a in aps) loc.cache.put(a.key, a.lat, a.lon, 10.0, BeaconCache.SERVICE)
        loc.cache.put(aps[0].key, lat0 + 0.5, lon0, 10.0, BeaconCache.SERVICE)       // 55 km off: it travels with someone
        val heard = aps.map { Heard(Mac.text(it.key), it.rssi, it.freq, "") }
        val f = loc.locate(heard, allowNetwork = false, now = 5_000_000L)!!
        assertTrue("error ${Geo.distanceM(lat0, lon0, f.lat, f.lon)}", Geo.distanceM(lat0, lon0, f.lat, f.lon) < 40)
        assertEquals(1, loc.cache[aps[0].key]!!.strikes)
        // a different scan of the same place (no reuse), and the second strike makes it "moved"
        loc.locate(heard.map { Heard(it.bssid, it.rssi - 9, it.freqMhz) }.reversed().take(5) + heard[0], allowNetwork = false, now = 5_000_000L + 7 * 3600_000L)
        assertEquals(BeaconCache.MOVED, loc.cache[aps[0].key]!!.kind)
        dir.deleteRecursively()
    }

    @Test fun pacerBacksOffWhileNothingChanges() {
        val p = Pacer()
        val f = LiteFix(lat0, lon0, 20.0, 50.0, "cache", "ok", 10, 8, 0, 0, 0)
        assertEquals(p.baseMs, p.next(f))
        assertEquals(p.baseMs * 2, p.next(f.copy(source = "reuse")))
        assertEquals(p.baseMs * 4, p.next(f.copy(source = "reuse")))
        assertEquals(p.minMs, p.next(f.copy(lat = lat0 + 0.05)))
    }

    @Test fun cliReadsAndroidsScanTable() {
        val table = listOf(
            "    BSSID              Frequency      RSSI           Age(sec)     SSID                                 Flags",
            "  00:11:22:33:44:55       2437        -57          1.234      Corner Cafe Guest                  [WPA2-PSK-CCMP][ESS]",
            "  00:11:22:33:44:66       5180    -71(0:-71)       274.474                                         [ESS]",
            "00:11:22:33:44:77 -80 2412 Plain Line",
        )
        val h = Cli.parse(table)
        assertEquals(3, h.size)
        assertEquals("Corner Cafe Guest", h[0].ssid); assertEquals(-57, h[0].rssi); assertEquals(2437, h[0].freqMhz)
        assertEquals("", h[1].ssid); assertEquals(5180, h[1].freqMhz)
        assertEquals(-80, h[2].rssi); assertEquals("Plain Line", h[2].ssid)
    }

    // ── a reply in Apple's shape: 10 header bytes, then tag 2 per device {1: mac, 2: {1: lat·1e8, 2: lon·1e8, 3: acc}} ──
    private fun fakeAppleReply(devs: List<Triple<Long, Double, Double>>): ByteArray {
        fun varint(v0: Long): ByteArray { var v = v0; val o = ByteArrayOutputStream(); do { var c = (v and 0x7f).toInt(); v = v ushr 7; if (v != 0L) c = c or 0x80; o.write(c) } while (v != 0L); return o.toByteArray() }
        fun len(tag: Int, d: ByteArray) = varint((tag shl 3).toLong() or 2) + varint(d.size.toLong()) + d
        fun num(tag: Int, v: Long) = varint((tag shl 3).toLong()) + varint(v)
        val body = ByteArrayOutputStream()
        for ((k, lat, lon) in devs) {
            val loc = num(1, Math.round(lat * 1e8)) + num(2, Math.round(lon * 1e8)) + num(3, 12)
            body.write(len(2, len(1, Mac.apple(k).toByteArray()) + len(2, loc)))
        }
        return ByteArray(10) + body.toByteArray()
    }
}
