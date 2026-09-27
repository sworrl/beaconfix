package org.sworrl.beaconfix.nav

import java.net.URLDecoder

/**
 * Finds a position in text another app shared with BeaconFix (pure; no Android types, so it is unit-tested).
 *
 * Understands, in this order: `beaconfix://map?lat=&lon=&label=`, `geo:` URIs (`geo:lat,lon`, `geo:0,0?q=lat,lon(label)`),
 * Google Maps links (`!3d…!4d…` pin, `query=` / `q=` / `ll=` / `center=` / `destination=`, `@lat,lon`), OpenStreetMap
 * (`mlat`/`mlon`, then `#map=z/lat/lon`), Apple Maps (`ll=` / `coordinate=` / `sll=` with `q=` or `name=`), then a plain
 * "lat, lon" pair anywhere in the text (decimals required, so "Room 12, 34" is not a position).
 *
 * Rejects |lat| > 90, |lon| > 180 and exactly 0,0 (the "no position" placeholder of `geo:` URIs). Short links
 * (maps.app.goo.gl/…) cannot be resolved offline and give null.
 */
object IncomingLink {
    data class Parsed(val lat: Double, val lon: Double, val label: String = "")

    private const val NUM = """[-+]?\d{1,3}(?:\.\d+)?"""
    private const val DEC = """[-+]?\d{1,3}\.\d+"""
    private val PAIR = Regex("""^\s*($NUM)\s*,\s*($NUM)""")
    private val URI = Regex("""(?i)(?:beaconfix://|geo:|https?://)[^\s<>"']+""")
    private val GOOGLE_PIN = Regex("""!3d($NUM)!4d($NUM)""")
    private val AT = Regex("""@($NUM),($NUM)""")
    private val OSM_HASH = Regex("""#map=\d+(?:\.\d+)?/($NUM)/($NUM)""")
    private val PLACE_NAME = Regex("""/maps/place/([^/@?#]+)""")
    private val PLAIN = Regex("""(?<![\w.])($DEC)\s*°?\s*[,;]\s*($DEC)\s*°?(?![\w.])""")

    fun parse(text: String): Parsed? {
        if (text.isBlank()) return null
        for (m in URI.findAll(text)) fromUri(trimTail(m.value))?.let { return it }
        return PLAIN.find(text)?.let { valid(it.groupValues[1], it.groupValues[2], "") }
    }

    private fun fromUri(uri: String): Parsed? {
        val lower = uri.lowercase()
        return when {
            lower.startsWith("beaconfix://") -> if (lower.startsWith("beaconfix://map")) { val q = query(uri); valid(q["lat"], q["lon"], q["label"].orEmpty()) } else null
            lower.startsWith("geo:") -> geo(uri)
            else -> web(uri)
        }
    }

    /** `geo:lat,lon[;u=…][?q=…]`: a position in `q` (with its "(label)") wins over the path, which is 0,0 when q names the place. */
    private fun geo(uri: String): Parsed? {
        val body = uri.substring(4).removePrefix("//")
        val path = body.substringBefore('?').substringBefore(';')
        val q = query(uri)["q"]
        if (q != null) {
            PAIR.find(q)?.let { m ->
                val label = Regex("""\((.*)\)\s*$""").find(q)?.groupValues?.get(1)?.trim().orEmpty()
                return valid(m.groupValues[1], m.groupValues[2], label)
            }
        }
        val p = PAIR.find(path) ?: return null
        return valid(p.groupValues[1], p.groupValues[2], q?.takeIf { PAIR.find(it) == null }?.trim().orEmpty())
    }

    private fun web(uri: String): Parsed? {
        val q = query(uri)
        val host = uri.substringAfter("://").substringBefore('/').substringBefore('?').lowercase()
        val label = (q["name"] ?: q["label"] ?: q["q"]?.takeIf { PAIR.find(it) == null }
            ?: PLACE_NAME.find(uri)?.groupValues?.get(1)?.let { decode(it) })?.trim().orEmpty()
        // OpenStreetMap: the marker, then the view
        if ("openstreetmap" in host || host == "osm.org" || host.endsWith(".osm.org")) {
            valid(q["mlat"], q["mlon"], label)?.let { return it }
            OSM_HASH.find(uri)?.let { return valid(it.groupValues[1], it.groupValues[2], label) }
            return null
        }
        // Google's exact pin sits in the data= blob; @lat,lon is only the view's centre
        GOOGLE_PIN.find(uri)?.let { m -> valid(m.groupValues[1], m.groupValues[2], label)?.let { return it } }
        for (k in listOf("query", "q", "ll", "coordinate", "sll", "center", "destination", "daddr")) {
            val v = q[k] ?: continue
            PAIR.find(v)?.let { m -> valid(m.groupValues[1], m.groupValues[2], label)?.let { return it } }
        }
        AT.find(uri)?.let { return valid(it.groupValues[1], it.groupValues[2], label) }
        return null
    }

    /** Sentence punctuation after a link is not part of it; a ")" is, when it closes a "(" of the link ("(label)"). */
    private fun trimTail(u: String): String {
        var s = u
        while (s.isNotEmpty()) {
            val c = s.last()
            s = when {
                c in ".,;:!?]'\"" -> s.dropLast(1)
                c == ')' && s.count { it == ')' } > s.count { it == '(' } -> s.dropLast(1)
                else -> return s
            }
        }
        return s
    }

    /** The query parameters (decoded), without the fragment. */
    private fun query(uri: String): Map<String, String> {
        val qs = uri.substringAfter('?', "").substringBefore('#')
        if (qs.isEmpty()) return emptyMap()
        val out = LinkedHashMap<String, String>()
        for (part in qs.split('&')) {
            if (part.isEmpty()) continue
            val k = decode(part.substringBefore('=')).lowercase()
            if (k !in out) out[k] = decode(part.substringAfter('=', ""))
        }
        return out
    }

    private fun decode(s: String): String = runCatching { URLDecoder.decode(s, "UTF-8") }.getOrDefault(s)

    private fun valid(la: String?, lo: String?, label: String): Parsed? {
        val lat = la?.trim()?.toDoubleOrNull() ?: return null
        val lon = lo?.trim()?.toDoubleOrNull() ?: return null
        if (lat.isNaN() || lon.isNaN() || kotlin.math.abs(lat) > 90 || kotlin.math.abs(lon) > 180) return null
        if (lat == 0.0 && lon == 0.0) return null
        return Parsed(lat, lon, label.take(80))
    }
}
