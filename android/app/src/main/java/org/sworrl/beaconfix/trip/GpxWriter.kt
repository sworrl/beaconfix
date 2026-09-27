package org.sworrl.beaconfix.trip

import java.time.Instant
import java.time.temporal.ChronoUnit
import java.util.Locale

/**
 * GPX 1.1 export of the trip journal (pure): one waypoint per stop, then one track whose segments break where the
 * path has a gap longer than [GAP_MS]. Times are UTC (`…Z`), elevation is written when known. Always closes
 * `<trkpt>`/`<wpt>` elements (the app's own GPX importer, and some others, expect that).
 */
object GpxWriter {
    const val GAP_MS = 60 * 60_000L
    const val CREATOR = "BeaconFix Android"

    fun write(path: List<PathPoint>, stops: List<Stop>, name: String = "BeaconFix trip", now: Long = System.currentTimeMillis()): String =
        StringBuilder().also { write(it, path, stops, name, now) }.toString()

    fun write(out: Appendable, path: List<PathPoint>, stops: List<Stop>, name: String = "BeaconFix trip", now: Long = System.currentTimeMillis()) {
        out.append("""<?xml version="1.0" encoding="UTF-8"?>""").append('\n')
        out.append("""<gpx version="1.1" creator="$CREATOR" xmlns="http://www.topografix.com/GPX/1/1" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd">""").append('\n')
        out.append("  <metadata><name>").append(esc(name)).append("</name><time>").append(time(now)).append("</time></metadata>\n")
        for (s in stops.sortedBy { it.arrival }) {
            out.append("  <wpt ").append(latLon(s.lat, s.lon)).append('>')
            s.elevM?.let { out.append("<ele>").append(ele(it)).append("</ele>") }
            out.append("<time>").append(time(s.arrival)).append("</time>")
            out.append("<name>").append(esc(s.place.ifEmpty { "Stop" })).append("</name>")
            if (s.dwellS >= 0) out.append("<desc>").append(esc("stayed " + dwell(s.dwellS))).append("</desc>")
            out.append("<type>stop</type></wpt>\n")
        }
        val pts = path.sortedBy { it.time }
        if (pts.isNotEmpty()) {
            out.append("  <trk><name>").append(esc(name)).append("</name>\n    <trkseg>\n")
            var prev: PathPoint? = null
            for (p in pts) {
                if (prev != null && p.time - prev.time > GAP_MS) out.append("    </trkseg>\n    <trkseg>\n")
                out.append("      <trkpt ").append(latLon(p.lat, p.lon)).append('>')
                p.ele?.let { out.append("<ele>").append(ele(it)).append("</ele>") }
                out.append("<time>").append(time(p.time)).append("</time></trkpt>\n")
                prev = p
            }
            out.append("    </trkseg>\n  </trk>\n")
        }
        out.append("</gpx>\n")
    }

    fun time(ms: Long): String = Instant.ofEpochMilli(ms).truncatedTo(ChronoUnit.SECONDS).toString()
    private fun latLon(lat: Double, lon: Double) = String.format(Locale.US, "lat=\"%.7f\" lon=\"%.7f\"", lat, lon)
    private fun ele(m: Double) = String.format(Locale.US, "%.1f", m)
    private fun dwell(s: Long) = when { s < 3600 -> "${s / 60} min"; else -> "${s / 3600} h ${s % 3600 / 60} min" }

    fun esc(s: String): String {
        val b = StringBuilder(s.length)
        for (c in s) when (c) {
            '&' -> b.append("&amp;"); '<' -> b.append("&lt;"); '>' -> b.append("&gt;"); '"' -> b.append("&quot;"); '\'' -> b.append("&apos;")
            else -> if (c < ' ' && c != '\t' && c != '\n' && c != '\r') Unit else b.append(c)
        }
        return b.toString()
    }
}
