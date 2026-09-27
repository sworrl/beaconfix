package org.sworrl.beaconfix.importer

import com.google.gson.stream.JsonReader
import com.google.gson.stream.JsonToken
import java.io.InputStream
import java.io.InputStreamReader
import java.time.Instant
import java.time.LocalDateTime
import java.time.OffsetDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/** What every importer produces: positions (fixes/track), Wi-Fi scans (to be paired with a position), stops, and beacon security. */
data class ImpPosition(val time: Long, val lat: Double, val lon: Double, val acc: Double, val source: String = "import", val alt: Double? = null)
data class ImpScan(val time: Long, val aps: List<ImpAp>)
data class ImpAp(val bssid: String, val dbm: Int, val ssid: String = "", val security: String = "", val freq: Int = 0)
data class ImpStop(val start: Long, val end: Long, val lat: Double, val lon: Double, val name: String = "", val placeId: String = "", val semantic: String = "")
data class ImpObservation(val bssid: String, val time: Long, val lat: Double, val lon: Double, val acc: Double, val dbm: Int, val ssid: String = "", val freq: Int = 0, val security: String = "")

class ImportResult {
    val positions = ArrayList<ImpPosition>(); val scans = ArrayList<ImpScan>(); val stops = ArrayList<ImpStop>(); val observations = ArrayList<ImpObservation>()
    var format = ""; var notes = ArrayList<String>()
    val firstTime get() = (positions.map { it.time } + scans.map { it.time } + observations.map { it.time }).minOrNull()
    val lastTime get() = (positions.map { it.time } + scans.map { it.time } + observations.map { it.time }).maxOrNull()
}

/** Progress callback: bytes read so far, items so far. */
typealias Progress = (bytes: Long, items: Int) -> Unit

object Importers {
    /** Sniff the format from the first bytes / the file name. */
    fun detect(head: String, name: String): String {
        val h = head.trimStart()
        return when {
            h.startsWith("WigleWifi-") -> "wigle"
            name.endsWith(".gpx", true) || h.contains("<gpx") -> "gpx"
            name.endsWith(".kml", true) || h.contains("<kml") -> "kml"
            h.startsWith("{") && (h.contains("\"rawSignals\"") || h.contains("\"semanticSegments\"") || h.contains("\"timelineEdits\"")) -> "timeline"
            h.startsWith("{") && h.contains("\"locations\"") -> "records"
            h.startsWith("{") && h.contains("\"timelineObjects\"") -> "semantic"
            h.startsWith("{") && (h.contains("\"aps\"") || h.contains("\"observations\"")) -> "beaconfix"
            name.endsWith(".json", true) -> "timeline"      // Timeline.json starts with the big arrays further in; try the streaming reader
            name.endsWith(".csv", true) -> "wigle"
            else -> "unknown"
        }
    }

    fun parse(format: String, input: InputStream, progress: Progress = { _, _ -> }): ImportResult = when (format) {
        "wigle" -> wigle(input, progress); "gpx" -> gpx(input, progress); "kml" -> kml(input, progress)
        "records" -> records(input, progress); "semantic" -> semantic(input, progress); "beaconfix" -> beaconfix(input, progress)
        else -> timeline(input, progress)
    }

    // ── time helpers ──────────────────────────────────────────────────────────
    fun parseTime(s: String?): Long? {
        if (s.isNullOrBlank()) return null
        s.toLongOrNull()?.let { return if (it > 100_000_000_000L) it else it * 1000 }        // ms or s epoch
        return runCatching { OffsetDateTime.parse(s).toInstant().toEpochMilli() }.getOrNull()
            ?: runCatching { Instant.parse(s).toEpochMilli() }.getOrNull()
            ?: runCatching { LocalDateTime.parse(s.take(19), DateTimeFormatter.ISO_LOCAL_DATE_TIME).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli() }.getOrNull()
            ?: runCatching { LocalDateTime.parse(s.take(19), DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli() }.getOrNull()
    }
    /** "40.0029°, -75.0680°" / "geo:40.00,-75.06" / "40.00, -75.06" */
    fun parseLatLng(s: String?): Pair<Double, Double>? {
        if (s == null) return null
        val m = Regex("(-?\\d+(?:\\.\\d+)?)\\s*°?\\s*,\\s*(-?\\d+(?:\\.\\d+)?)").find(s) ?: return null
        return m.groupValues[1].toDouble() to m.groupValues[2].toDouble()
    }
    /** Timeline MACs: decimal int64 (e.g. "123456789012") or hex with/without colons. */
    fun parseMac(v: String?): String? {
        if (v.isNullOrBlank()) return null
        val t = v.trim()
        if (t.contains(':') && t.length == 17) return t.uppercase()
        if (Regex("^[0-9A-Fa-f]{12}$").matches(t)) return t.chunked(2).joinToString(":").uppercase()
        val n = t.toLongOrNull() ?: return null
        if (n < 0 || n > 0xFFFFFFFFFFFFL) return null
        return String.format("%012X", n).chunked(2).joinToString(":")
    }
    /** Android/WiGLE capability strings → our security enum. */
    fun securityOf(cap: String): String = org.sworrl.beaconfix.collector.ObservationRecorder.securityOf(cap)

    // ── Google Timeline (on-device export, 2024+) ────────────────────────────
    fun timeline(input: InputStream, progress: Progress): ImportResult {
        val r = ImportResult(); r.format = "timeline"
        val cin = CountingStream(input)
        JsonReader(InputStreamReader(cin, Charsets.UTF_8)).use { j ->
            j.isLenient = true
            j.beginObject()
            while (j.hasNext()) {
                when (j.nextName()) {
                    "rawSignals" -> { j.beginArray(); while (j.hasNext()) { rawSignal(j, r); if (r.positions.size % 500 == 0) progress(cin.count, r.positions.size + r.scans.size) }; j.endArray() }
                    "semanticSegments" -> { j.beginArray(); while (j.hasNext()) { segment(j, r) }; j.endArray() }
                    else -> j.skipValue()
                }
            }
            j.endObject()
        }
        progress(cin.count, r.positions.size + r.scans.size)
        return r
    }
    private fun rawSignal(j: JsonReader, r: ImportResult) {
        j.beginObject()
        while (j.hasNext()) {
            when (j.nextName()) {
                "position" -> {
                    var lat: Double? = null; var lon: Double? = null; var t: Long? = null; var acc = 100.0; var src = "timeline"; var alt: Double? = null
                    j.beginObject()
                    while (j.hasNext()) when (j.nextName()) {
                        "LatLng", "point", "latLng" -> if (j.peek() == JsonToken.STRING) parseLatLng(j.nextString())?.let { lat = it.first; lon = it.second } else { j.beginObject(); while (j.hasNext()) when (j.nextName()) { "latitude" -> lat = j.nextDouble(); "longitude" -> lon = j.nextDouble(); "latE7" -> lat = j.nextLong() / 1e7; "lngE7" -> lon = j.nextLong() / 1e7; else -> j.skipValue() }; j.endObject() }
                        "timestamp" -> t = parseTime(j.nextString())
                        "accuracyMeters" -> acc = j.nextDouble()
                        "altitudeMeters" -> alt = j.nextDouble()
                        "source" -> src = "timeline:" + j.nextString().lowercase()
                        else -> j.skipValue()
                    }
                    j.endObject()
                    if (lat != null && lon != null && t != null) r.positions += ImpPosition(t, lat!!, lon!!, acc, src, alt)
                }
                "wifiScan" -> {
                    var t: Long? = null; val aps = ArrayList<ImpAp>()
                    j.beginObject()
                    while (j.hasNext()) when (j.nextName()) {
                        "deliveryTime" -> t = parseTime(j.nextString())
                        "devicesRecords", "devices" -> { j.beginArray(); while (j.hasNext()) { var mac: String? = null; var rssi = -100; j.beginObject(); while (j.hasNext()) when (j.nextName()) { "mac", "macAddress" -> mac = parseMac(if (j.peek() == JsonToken.NUMBER) j.nextLong().toString() else j.nextString()); "rawRssi", "rssi" -> rssi = j.nextInt(); else -> j.skipValue() }; j.endObject(); if (mac != null) aps += ImpAp(mac, rssi) }; j.endArray() }
                        else -> j.skipValue()
                    }
                    j.endObject()
                    if (t != null && aps.isNotEmpty()) r.scans += ImpScan(t, aps)
                }
                else -> j.skipValue()
            }
        }
        j.endObject()
    }
    private fun segment(j: JsonReader, r: ImportResult) {
        var start: Long? = null; var end: Long? = null
        j.beginObject()
        while (j.hasNext()) when (j.nextName()) {
            "startTime" -> start = parseTime(j.nextString()); "endTime" -> end = parseTime(j.nextString())
            "visit" -> { var lat: Double? = null; var lon: Double? = null; var name = ""; var pid = ""; var sem = ""
                j.beginObject(); while (j.hasNext()) when (j.nextName()) { "topCandidate" -> { j.beginObject(); while (j.hasNext()) when (j.nextName()) { "placeLocation" -> { if (j.peek() == JsonToken.STRING) parseLatLng(j.nextString())?.let { lat = it.first; lon = it.second } else { j.beginObject(); while (j.hasNext()) when (j.nextName()) { "latLng" -> parseLatLng(j.nextString())?.let { lat = it.first; lon = it.second }; else -> j.skipValue() }; j.endObject() } }; "placeId", "placeID" -> pid = j.nextString(); "semanticType" -> sem = j.nextString(); "name" -> name = j.nextString(); else -> j.skipValue() }; j.endObject() }; else -> j.skipValue() }; j.endObject()
                if (lat != null && lon != null && start != null) r.stops += ImpStop(start!!, end ?: start!!, lat!!, lon!!, name, pid, sem) }
            "timelinePath" -> { j.beginArray(); while (j.hasNext()) { var lat: Double? = null; var lon: Double? = null; var t: Long? = null; j.beginObject(); while (j.hasNext()) when (j.nextName()) { "point" -> parseLatLng(j.nextString())?.let { lat = it.first; lon = it.second }; "time", "durationMinutesOffsetFromStartTime" -> { val v = j.nextString(); t = parseTime(v) ?: v.toDoubleOrNull()?.let { min -> start?.plus((min * 60000).toLong()) } }; else -> j.skipValue() }; j.endObject(); if (lat != null && lon != null) r.positions += ImpPosition(t ?: start ?: 0L, lat!!, lon!!, 200.0, "timeline:path") }; j.endArray() }
            else -> j.skipValue()
        }
        j.endObject()
    }

    // ── Takeout Records.json ─────────────────────────────────────────────────
    fun records(input: InputStream, progress: Progress): ImportResult {
        val r = ImportResult(); r.format = "records"; val cin = CountingStream(input)
        JsonReader(InputStreamReader(cin, Charsets.UTF_8)).use { j ->
            j.isLenient = true; j.beginObject()
            while (j.hasNext()) { if (j.nextName() != "locations") { j.skipValue(); continue }
                j.beginArray()
                while (j.hasNext()) {
                    var lat: Double? = null; var lon: Double? = null; var t: Long? = null; var acc = 100.0; var alt: Double? = null; val scans = ArrayList<ImpScan>()
                    j.beginObject()
                    while (j.hasNext()) when (j.nextName()) {
                        "latitudeE7" -> lat = j.nextLong() / 1e7; "longitudeE7" -> lon = j.nextLong() / 1e7
                        "timestamp" -> t = parseTime(j.nextString()); "timestampMs" -> t = parseTime(j.nextString())
                        "accuracy" -> acc = j.nextDouble(); "altitude" -> alt = j.nextDouble()
                        "wifiScan", "activeWifiScan" -> { var st: Long? = null; val aps = ArrayList<ImpAp>(); j.beginObject(); while (j.hasNext()) when (j.nextName()) { "timestampMs", "deliveryTime" -> st = parseTime(j.nextString()); "accessPoints", "devicesRecords" -> { j.beginArray(); while (j.hasNext()) { var mac: String? = null; var rssi = -100; j.beginObject(); while (j.hasNext()) when (j.nextName()) { "mac", "macAddress" -> mac = parseMac(if (j.peek() == JsonToken.NUMBER) j.nextLong().toString() else j.nextString()); "strength", "rawRssi", "rssi" -> rssi = j.nextInt(); else -> j.skipValue() }; j.endObject(); if (mac != null) aps += ImpAp(mac, rssi) }; j.endArray() }; else -> j.skipValue() }; j.endObject(); if (aps.isNotEmpty()) scans += ImpScan(st ?: t ?: 0L, aps) }
                        else -> j.skipValue()
                    }
                    j.endObject()
                    if (lat != null && lon != null && t != null) { r.positions += ImpPosition(t!!, lat!!, lon!!, acc, "takeout", alt); scans.forEach { s -> r.scans += if (s.time == 0L) s.copy(time = t!!) else s } }
                    if (r.positions.size % 1000 == 0) progress(cin.count, r.positions.size)
                }
                j.endArray()
            }
            j.endObject()
        }
        progress(cin.count, r.positions.size); return r
    }

    // ── Takeout Semantic Location History ────────────────────────────────────
    fun semantic(input: InputStream, progress: Progress): ImportResult {
        val r = ImportResult(); r.format = "semantic"; val cin = CountingStream(input)
        JsonReader(InputStreamReader(cin, Charsets.UTF_8)).use { j ->
            j.isLenient = true; j.beginObject()
            while (j.hasNext()) { if (j.nextName() != "timelineObjects") { j.skipValue(); continue }
                j.beginArray()
                while (j.hasNext()) {
                    j.beginObject()
                    while (j.hasNext()) when (j.nextName()) {
                        "placeVisit" -> { var lat: Double? = null; var lon: Double? = null; var name = ""; var pid = ""; var s: Long? = null; var e: Long? = null
                            j.beginObject(); while (j.hasNext()) when (j.nextName()) { "location" -> { j.beginObject(); while (j.hasNext()) when (j.nextName()) { "latitudeE7" -> lat = j.nextLong() / 1e7; "longitudeE7" -> lon = j.nextLong() / 1e7; "name" -> name = j.nextString(); "placeId" -> pid = j.nextString(); else -> j.skipValue() }; j.endObject() }; "duration" -> { j.beginObject(); while (j.hasNext()) when (j.nextName()) { "startTimestamp", "startTimestampMs" -> s = parseTime(j.nextString()); "endTimestamp", "endTimestampMs" -> e = parseTime(j.nextString()); else -> j.skipValue() }; j.endObject() }; else -> j.skipValue() }; j.endObject()
                            if (lat != null && lon != null && s != null) r.stops += ImpStop(s!!, e ?: s!!, lat!!, lon!!, name, pid) }
                        "activitySegment" -> { var s: Long? = null; val pts = ArrayList<Triple<Double, Double, Long?>>()
                            j.beginObject(); while (j.hasNext()) when (j.nextName()) { "duration" -> { j.beginObject(); while (j.hasNext()) when (j.nextName()) { "startTimestamp", "startTimestampMs" -> s = parseTime(j.nextString()); else -> j.skipValue() }; j.endObject() }
                                "waypointPath", "simplifiedRawPath" -> { j.beginObject(); while (j.hasNext()) { val k = j.nextName(); if (k == "waypoints" || k == "points") { j.beginArray(); while (j.hasNext()) { var la: Double? = null; var lo: Double? = null; var t: Long? = null; j.beginObject(); while (j.hasNext()) when (j.nextName()) { "latE7" -> la = j.nextLong() / 1e7; "lngE7" -> lo = j.nextLong() / 1e7; "timestamp", "timestampMs" -> t = parseTime(j.nextString()); else -> j.skipValue() }; j.endObject(); if (la != null && lo != null) pts += Triple(la!!, lo!!, t) }; j.endArray() } else j.skipValue() }; j.endObject() }
                                else -> j.skipValue() }; j.endObject()
                            pts.forEach { (la, lo, t) -> r.positions += ImpPosition(t ?: s ?: 0L, la, lo, 300.0, "takeout:path") } }
                        else -> j.skipValue()
                    }
                    j.endObject()
                }
                j.endArray()
            }
            j.endObject()
        }
        progress(cin.count, r.stops.size + r.positions.size); return r
    }

    // ── WiGLE CSV ────────────────────────────────────────────────────────────
    fun wigle(input: InputStream, progress: Progress): ImportResult {
        val r = ImportResult(); r.format = "wigle"
        input.bufferedReader().useLines { lines ->
            var header: List<String>? = null; var n = 0
            for (line in lines) {
                if (line.startsWith("WigleWifi-")) continue
                if (header == null) { header = csv(line).map { it.trim() }; continue }
                val f = csv(line); if (f.size < 9) continue
                val h = header!!; fun col(name: String) = h.indexOf(name).let { if (it >= 0 && it < f.size) f[it] else "" }
                if (col("Type").isNotEmpty() && col("Type") != "WIFI") continue
                val mac = parseMac(col("MAC")) ?: continue
                val lat = col("CurrentLatitude").toDoubleOrNull() ?: continue; val lon = col("CurrentLongitude").toDoubleOrNull() ?: continue
                val t = parseTime(col("FirstSeen")) ?: continue
                val acc = col("AccuracyMeters").toDoubleOrNull() ?: 30.0
                val rssi = col("RSSI").toIntOrNull() ?: -100
                val freq = col("Frequency").toIntOrNull() ?: 0
                r.observations += ImpObservation(mac, t, lat, lon, acc, rssi, col("SSID"), freq, securityOf(col("AuthMode")))
                if (++n % 2000 == 0) progress(0, n)
            }
        }
        progress(0, r.observations.size); return r
    }
    private fun csv(line: String): List<String> { val out = ArrayList<String>(); val sb = StringBuilder(); var q = false; for (c in line) { when { c == '"' -> q = !q; c == ',' && !q -> { out += sb.toString(); sb.setLength(0) }; else -> sb.append(c) } }; out += sb.toString(); return out }

    // ── GPX / KML ────────────────────────────────────────────────────────────
    fun gpx(input: InputStream, progress: Progress): ImportResult {
        val r = ImportResult(); r.format = "gpx"; val text = input.bufferedReader().readText()
        for (m in Regex("<(?:trkpt|wpt|rtept)\\s+([^>]*?)>(.*?)</(?:trkpt|wpt|rtept)>", RegexOption.DOT_MATCHES_ALL).findAll(text)) {
            val attrs = m.groupValues[1]; val body = m.groupValues[2]
            val lat = Regex("lat=\"(-?[\\d.]+)\"").find(attrs)?.groupValues?.get(1)?.toDoubleOrNull() ?: continue
            val lon = Regex("lon=\"(-?[\\d.]+)\"").find(attrs)?.groupValues?.get(1)?.toDoubleOrNull() ?: continue
            val t = Regex("<time>([^<]+)</time>").find(body)?.groupValues?.get(1)?.let { parseTime(it) } ?: 0L
            val ele = Regex("<ele>([^<]+)</ele>").find(body)?.groupValues?.get(1)?.toDoubleOrNull()
            r.positions += ImpPosition(t, lat, lon, 15.0, "gpx", ele)
        }
        progress(text.length.toLong(), r.positions.size); return r
    }
    fun kml(input: InputStream, progress: Progress): ImportResult {
        val r = ImportResult(); r.format = "kml"; val text = input.bufferedReader().readText()
        for (m in Regex("<coordinates>([^<]+)</coordinates>").findAll(text)) for (tok in m.groupValues[1].trim().split(Regex("\\s+"))) {
            val p = tok.split(','); if (p.size < 2) continue
            val lon = p[0].toDoubleOrNull() ?: continue; val lat = p[1].toDoubleOrNull() ?: continue
            r.positions += ImpPosition(0L, lat, lon, 30.0, "kml", p.getOrNull(2)?.toDoubleOrNull())
        }
        for (m in Regex("<gx:coord>([^<]+)</gx:coord>").findAll(text)) { val p = m.groupValues[1].trim().split(Regex("\\s+")); if (p.size >= 2) r.positions += ImpPosition(0L, p[1].toDouble(), p[0].toDouble(), 30.0, "kml") }
        progress(text.length.toLong(), r.positions.size); return r
    }

    // ── BeaconFix desktop export ─────────────────────────────────────────────
    fun beaconfix(input: InputStream, progress: Progress): ImportResult {
        val r = ImportResult(); r.format = "beaconfix"; val cin = CountingStream(input)
        JsonReader(InputStreamReader(cin, Charsets.UTF_8)).use { j ->
            j.isLenient = true; j.beginObject()
            while (j.hasNext()) when (j.nextName()) {
                "observations" -> { j.beginArray(); while (j.hasNext()) { var b = ""; var t: Long? = null; var la: Double? = null; var lo: Double? = null; var acc = 100.0; var dbm = -100; j.beginObject(); while (j.hasNext()) when (j.nextName()) { "bssid" -> b = j.nextString().uppercase(); "time" -> t = parseTime(j.nextString()); "lat" -> la = j.nextDouble(); "lon" -> lo = j.nextDouble(); "acc" -> acc = j.nextDouble(); "dbm" -> dbm = j.nextInt(); else -> j.skipValue() }; j.endObject(); if (b.length == 17 && t != null && la != null && lo != null) r.observations += ImpObservation(b, t!!, la!!, lo!!, acc, dbm) }; j.endArray() }
                "fixes" -> { j.beginArray(); while (j.hasNext()) { var t: Long? = null; var la: Double? = null; var lo: Double? = null; var acc = 100.0; var place = ""; j.beginObject(); while (j.hasNext()) when (j.nextName()) { "time" -> t = parseTime(j.nextString()); "lat" -> la = j.nextDouble(); "lon" -> lo = j.nextDouble(); "acc" -> acc = j.nextDouble(); "place" -> place = j.nextString(); else -> j.skipValue() }; j.endObject(); if (t != null && la != null && lo != null) { r.positions += ImpPosition(t!!, la!!, lo!!, acc, "beaconfix"); r.stops += ImpStop(t!!, t!!, la!!, lo!!, place) } }; j.endArray() }
                "aps" -> { j.beginArray(); while (j.hasNext()) { var b = ""; var ssid = ""; var sec = ""; j.beginObject(); while (j.hasNext()) when (j.nextName()) { "bssid" -> b = j.nextString().uppercase(); "ssid" -> ssid = j.nextString(); "security" -> sec = j.nextString(); else -> j.skipValue() }; j.endObject(); if (b.length == 17) r.notes += "ap:$b|$ssid|$sec" }; j.endArray() }
                else -> j.skipValue()
            }
            j.endObject()
        }
        progress(cin.count, r.observations.size + r.positions.size); return r
    }

    // ── pairing scans with positions ─────────────────────────────────────────
    /** Every scan gets the position nearest in time (±60 s), interpolated between neighbours when both lie within 2 min; skipped when > 100 m. */
    fun pairScans(r: ImportResult, maxAcc: Double = 100.0): Int {
        if (r.scans.isEmpty() || r.positions.isEmpty()) return 0
        val pos = r.positions.filter { it.time > 0 }.sortedBy { it.time }
        val times = LongArray(pos.size) { pos[it].time }
        var made = 0
        for (s in r.scans) {
            var i = times.binarySearch(s.time); if (i < 0) i = -i - 1
            val before = pos.getOrNull(i - 1); val after = pos.getOrNull(i)
            val near = listOfNotNull(before, after).minByOrNull { Math.abs(it.time - s.time) } ?: continue
            if (Math.abs(near.time - s.time) > 60_000) continue
            var lat = near.lat; var lon = near.lon; var acc = near.acc
            if (before != null && after != null && after.time - before.time in 1..120_000 && s.time in before.time..after.time) {
                val f = (s.time - before.time).toDouble() / (after.time - before.time)
                lat = before.lat + (after.lat - before.lat) * f; lon = before.lon + (after.lon - before.lon) * f; acc = Math.max(before.acc, after.acc)
            }
            if (acc > maxAcc) continue
            for (a in s.aps) { r.observations += ImpObservation(a.bssid, s.time, lat, lon, acc, a.dbm, a.ssid, a.freq, a.security); made++ }
        }
        return made
    }
}

class CountingStream(private val inner: InputStream) : InputStream() {
    var count = 0L; private set
    override fun read(): Int = inner.read().also { if (it >= 0) count++ }
    override fun read(b: ByteArray, off: Int, len: Int): Int = inner.read(b, off, len).also { if (it > 0) count += it }
    override fun close() = inner.close()
}
