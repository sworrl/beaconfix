package org.sworrl.beaconfix.lite

import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL

/** The one network primitive the locator needs; swap it for tests, a proxy, or a device's own HTTP stack. */
fun interface Http {
    /** POST [body]; the reply body, or null on any failure (no exceptions). */
    fun post(url: String, contentType: String, userAgent: String, body: ByteArray): ByteArray?
}

/** java.net, nothing else: short timeouts, no redirects, one request per connection. */
class UrlHttp(private val timeoutMs: Int = 10_000) : Http {
    override fun post(url: String, contentType: String, userAgent: String, body: ByteArray): ByteArray? = try {
        val c = URL(url).openConnection() as HttpURLConnection
        try {
            c.requestMethod = "POST"; c.connectTimeout = timeoutMs; c.readTimeout = timeoutMs
            c.instanceFollowRedirects = false; c.doOutput = true
            c.setRequestProperty("Content-Type", contentType); c.setRequestProperty("User-Agent", userAgent)
            c.setFixedLengthStreamingMode(body.size)
            c.outputStream.use { it.write(body) }
            if (c.responseCode in 200..299) c.inputStream.use { it.readBytes() } else null
        } finally { c.disconnect() }
    } catch (_: Exception) { null }
}

/** A placement a service gave for one BSSID. */
class Placement(val key: Long, val lat: Double, val lon: Double, val accM: Double)

/**
 * Apple's Wi-Fi positioning (gs-loc.apple.com): no account, no token, answers per BSSID, and volunteers the mapped
 * positions of ~100 neighbours of each one asked about. Those neighbours are why a lookup is rare: they go in the
 * cache, and the next fix in the area is solved offline. Request and reply are small protobufs, hand-encoded here
 * (no protobuf dependency for a few hundred bytes of wire format).
 */
class AppleWps(private val http: Http) {
    companion object {
        const val URL = "https://gs-loc.apple.com/clls/wloc"
        private const val UA = "locationd/1753.17 CFNetwork/711.1.12 Darwin/14.0.0"
        /** The request frames its body length in one byte: ten BSSIDs a request keeps it under 256. */
        const val BATCH = 10

        fun request(keys: List<Long>): ByteArray {
            val body = ByteArrayOutputStream()
            for (k in keys) body.write(lenField(2, lenField(1, Mac.apple(k).toByteArray(Charsets.ISO_8859_1))))
            body.write(varint(3L shl 3)); body.write(varint(0))       // noise 0
            body.write(varint(4L shl 3)); body.write(varint(100))     // signal 100
            val b = body.toByteArray()
            val out = ByteArrayOutputStream()
            out.write(byteArrayOf(0, 1, 0, 5)); out.write("en_US".toByteArray(Charsets.ISO_8859_1))
            out.write(byteArrayOf(0, 0x13)); out.write("com.apple.locationd".toByteArray(Charsets.ISO_8859_1))
            out.write(byteArrayOf(0, 0x0a)); out.write("8.1.12B411".toByteArray(Charsets.ISO_8859_1))
            out.write(byteArrayOf(0, 0, 0, 1, 0, 0, 0))
            out.write(b.size and 0xff); out.write(b)
            return out.toByteArray()
        }

        /** Every placed device in a reply (the ones asked about and the neighbours). */
        fun parse(reply: ByteArray): List<Placement> {
            if (reply.size <= 10) return emptyList()
            val out = ArrayList<Placement>()
            for (dev in fields(reply, 10, reply.size)) {
                if (dev.tag != 2 || dev.wire != 2) continue
                var key = -1L; var lat = -180.0; var lon = -180.0; var acc = -1.0
                for (f in fields(reply, dev.start, dev.end)) {
                    if (f.tag == 1 && f.wire == 2) key = Mac.key(String(reply, f.start, f.end - f.start, Charsets.ISO_8859_1))
                    else if (f.tag == 2 && f.wire == 2) for (l in fields(reply, f.start, f.end)) when (l.tag) {
                        1 -> lat = l.v / 1e8
                        2 -> lon = l.v / 1e8
                        3 -> acc = l.v.toDouble()
                    }
                }
                // -180,-180 is Apple's "unknown"; 0,0 is nobody's house
                if (key < 0 || lat < -90 || !Geo.valid(lat, lon)) continue
                out += Placement(key, lat, lon, if (acc > 0) acc else 100.0)
            }
            return out
        }

        private fun varint(value: Long): ByteArray {
            var v = value
            val out = ByteArrayOutputStream()
            do {
                var c = (v and 0x7f).toInt(); v = v ushr 7
                if (v != 0L) c = c or 0x80
                out.write(c)
            } while (v != 0L)
            return out.toByteArray()
        }

        private fun lenField(tag: Int, data: ByteArray): ByteArray = varint((tag shl 3).toLong() or 2L) + varint(data.size.toLong()) + data

        /** A protobuf field: its value (varint) or its payload's range in the buffer (length-delimited). No copying. */
        private class Field(val tag: Int, val wire: Int, val v: Long, val start: Int, val end: Int)

        private fun fields(b: ByteArray, from: Int, to: Int): List<Field> {
            val out = ArrayList<Field>()
            var i = from
            fun readVarint(): Long? {
                var v = 0L; var s = 0
                while (i < to && s < 64) {
                    val c = b[i++].toInt() and 0xff
                    v = v or ((c and 0x7f).toLong() shl s); s += 7
                    if (c and 0x80 == 0) return v
                }
                return null
            }
            while (i < to) {
                val k = readVarint() ?: break
                val tag = (k ushr 3).toInt(); val wire = (k and 7L).toInt()
                when (wire) {
                    0 -> { val v = readVarint() ?: break; out += Field(tag, wire, v, 0, 0) }
                    2 -> {
                        val len = (readVarint() ?: break).toInt()
                        if (len < 0 || i + len > to) break
                        out += Field(tag, wire, 0, i, i + len); i += len
                    }
                    1 -> { if (i + 8 > to) break; i += 8 }
                    5 -> { if (i + 4 > to) break; i += 4 }
                    else -> break
                }
            }
            return out
        }
    }

    /** Placements for [keys] (asked about) plus whatever neighbours Apple adds. Empty on failure. */
    fun lookup(keys: List<Long>): List<Placement> {
        val reply = http.post(URL, "application/x-www-form-urlencoded", UA, request(keys)) ?: return emptyList()
        return parse(reply)
    }
}

/**
 * BeaconDB (api.beacondb.net), the open Wi-Fi geolocation database: one position for a whole scan, two APs minimum.
 * GeoIP and cell fallbacks are switched off in the request; an answer wider than [GEOIP_TELL_M] is a GeoIP guess
 * leaking through anyway and is refused. A miss should look like a miss, not a city-sized guess dressed as a fix.
 */
class BeaconDb(private val http: Http, private val userAgent: String = "beaconfix-lite (+https://github.com/sworrl/beaconfix)") {
    companion object {
        const val URL = "https://api.beacondb.net/v1/geolocate"
        const val GEOIP_TELL_M = 5_000.0
        private val NUM = { name: String -> Regex("\"$name\"\\s*:\\s*(-?[0-9.]+(?:[eE][-+]?[0-9]+)?)") }
        private val LAT = NUM("lat"); private val LNG = NUM("lng"); private val ACC = NUM("accuracy")

        fun request(aps: List<Heard>): String {
            val sb = StringBuilder("{\"considerIp\":false,\"fallbacks\":{\"ipf\":false,\"lacf\":false},\"wifiAccessPoints\":[")
            aps.forEachIndexed { i, a ->
                if (i > 0) sb.append(',')
                sb.append("{\"macAddress\":\"").append(Mac.text(a.key)).append("\",\"signalStrength\":").append(a.rssi)
                if (a.freqMhz > 0) sb.append(",\"frequency\":").append(a.freqMhz)
                sb.append('}')
            }
            return sb.append("]}").toString()
        }

        /** (lat, lon, accuracy) or null. */
        fun parse(text: String): Triple<Double, Double, Double>? {
            val lat = LAT.find(text)?.groupValues?.get(1)?.toDoubleOrNull() ?: return null
            val lon = LNG.find(text)?.groupValues?.get(1)?.toDoubleOrNull() ?: return null
            val acc = ACC.find(text)?.groupValues?.get(1)?.toDoubleOrNull() ?: return null
            if (!Geo.valid(lat, lon) || acc <= 0 || acc > GEOIP_TELL_M) return null
            return Triple(lat, lon, acc)
        }
    }

    fun locate(aps: List<Heard>): Triple<Double, Double, Double>? {
        if (aps.size < 2) return null
        val reply = http.post(URL, "application/json", userAgent, request(aps).toByteArray(Charsets.UTF_8)) ?: return null
        return parse(String(reply, Charsets.UTF_8))
    }
}
