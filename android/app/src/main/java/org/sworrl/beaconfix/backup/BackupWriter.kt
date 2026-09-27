package org.sworrl.beaconfix.backup

import com.google.gson.JsonElement
import com.google.gson.JsonParser
import com.google.gson.stream.JsonWriter
import org.sworrl.beaconfix.data.db.AnchorEntity
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.data.db.ObservationEntity
import java.io.Writer
import java.time.Instant
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/**
 * The phone's own data as a BeaconFix export (pure: rows in, JSON out, streamed with Gson's JsonWriter).
 *
 * The file is the desktop's `--db-export` shape, so both importers take it: this app's Import screen (Importers.detect
 * sees `"aps"` in the first bytes) and the desktop's `POST /api/v1/db/import` / `--db-import` (it needs
 * `"beaconfix":"mapdb"` in the first 64 KB and inserts rows by column name, so every row uses the desktop's column
 * names and nothing else). Layout: `{"beaconfix":"mapdb","aps":[…],"observations":[…],"fixes":[…],"anchors":[…],
 * "format":"beaconfix-export","version":1,"device":…,"created":…,"exported":…}`.
 *
 * What is in it: every beacon (bssid, name, band, security; our own fitted position as the desktop's "peer" position),
 * this phone's observations (not the ones pulled from a desktop), this phone's fixes (not the desktop's track; each
 * tagged with [device] so a desktop files them as another device's history, never as its own track) and the anchors
 * (the desktop keeps the newer copy of each). Never: the identity seed, tokens, paired desktops or settings.
 */
object BackupWriter {
    data class Counts(val aps: Int = 0, val observations: Int = 0, val fixes: Int = 0, val anchors: Int = 0)

    /** Keys the history importers (phone and desktop) look for before "aps": a string equal to one must not appear verbatim. */
    private val TRAPS = setOf("locations", "rawSignals", "semanticSegments", "timelineEdits", "timelineObjects", "userLocationProfile", "timelinePath", "visit")

    private val ISO: DateTimeFormatter = DateTimeFormatter.ISO_LOCAL_DATE_TIME

    /** Local time without offset, seconds precision: the exact form the sync pushes, so a desktop dedupes observations it already has. */
    fun iso(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String = LocalDateTime.ofInstant(Instant.ofEpochMilli(ms), zone).withNano(0).format(ISO)

    /** An observation this phone made (or imported itself); rows pulled from a desktop are already there. */
    fun keepObservation(o: ObservationEntity) = !o.remote && o.bssid.length == 17 && finite(o.lat, o.lon, o.acc) && o.acc > 0
    /** A fix of this phone (or one it imported); the desktop's own track is the desktop's. */
    fun keepFix(f: FixEntity) = f.source != "desktop" && f.time > 0 && finite(f.lat, f.lon, f.acc)
    fun keepAp(a: ApEntity) = a.bssid.length == 17

    fun write(out: Writer, aps: Sequence<ApEntity>, observations: Sequence<ObservationEntity>, fixes: Sequence<FixEntity>, anchors: Sequence<AnchorEntity>,
              device: String, now: Long = System.currentTimeMillis(), zone: ZoneId = ZoneId.systemDefault()): Counts {
        val w = JsonWriter(out)
        w.isHtmlSafe = true                 // "<gpx" / "<kml" never appear verbatim either
        w.serializeNulls = false
        var nAps = 0; var nObs = 0; var nFix = 0; var nAnc = 0
        w.beginObject()
        w.name("beaconfix").value("mapdb")
        w.name("aps").beginArray()
        for (a in aps) {
            if (!keepAp(a)) continue
            w.beginObject()
            w.name("bssid").value(a.bssid.uppercase())
            str(w, "ssid", a.ssid)
            if (a.freq > 0) w.name("freq").value(a.freq.toLong())
            if (a.band.isNotEmpty()) str(w, "band", a.band)
            if (a.ch > 0) w.name("ch").value(a.ch.toLong())
            if (a.firstSeen > 0) w.name("first_seen").value(iso(a.firstSeen, zone))
            if (a.lastSeen > 0) w.name("last_seen").value(iso(a.lastSeen, zone))
            if (a.timesSeen > 0) w.name("times_seen").value(a.timesSeen.toLong())
            if (a.security.isNotEmpty()) str(w, "security", a.security)
            val lat = a.lat; val lon = a.lon; val acc = a.acc
            if (a.posSource == "observed" && lat != null && lon != null && acc != null && finite(lat, lon, acc) && acc > 0) {
                w.name("peer_lat").value(lat); w.name("peer_lon").value(lon); w.name("peer_acc").value(acc)
                str(w, "peer_from", device)
            }
            w.endObject(); nAps++
        }
        w.endArray()
        w.name("observations").beginArray()
        for (o in observations) {
            if (!keepObservation(o)) continue
            w.beginObject()
            w.name("bssid").value(o.bssid.uppercase())
            w.name("time").value(iso(o.time, zone))
            w.name("lat").value(o.lat); w.name("lon").value(o.lon); w.name("acc").value(o.acc)
            w.name("dbm").value(o.dbm.toLong())
            w.name("fix_source").value("android")
            str(w, "device", device)
            w.endObject(); nObs++
        }
        w.endArray()
        w.name("fixes").beginArray()
        for (f in fixes) {
            if (!keepFix(f)) continue
            w.beginObject()
            w.name("time").value(iso(f.time, zone))
            w.name("lat").value(f.lat); w.name("lon").value(f.lon); w.name("acc").value(f.acc)
            str(w, "source", f.source)
            if (f.provider.isNotEmpty()) str(w, "provider", f.provider)
            if (f.place.isNotEmpty()) str(w, "place", f.place)
            str(w, "device", device)
            w.endObject(); nFix++
        }
        w.endArray()
        w.name("anchors").beginArray()
        for (a in anchors) {
            val el = runCatching { JsonParser.parseString(a.json) }.getOrNull()?.takeIf { it.isJsonObject } ?: continue
            element(w, el); nAnc++
        }
        w.endArray()
        w.name("format").value("beaconfix-export")
        w.name("version").value(1)
        str(w, "device", device)
        w.name("created").value(now)
        w.name("exported").value(iso(now, zone))
        w.endObject()
        w.flush()
        return Counts(nAps, nObs, nFix, nAnc)
    }

    private fun finite(vararg v: Double) = v.all { it.isFinite() }

    private fun str(w: JsonWriter, name: String, value: String) { w.name(name); strValue(w, value) }

    /** A string value; one that equals an importer's key is written with its first letter \u-escaped (same text once parsed). */
    private fun strValue(w: JsonWriter, value: String) {
        if (value in TRAPS) w.jsonValue("\"" + String.format("\\u%04x", value[0].code) + value.substring(1) + "\"") else w.value(value)
    }

    /** Re-emits parsed JSON (an anchor) through the same rules; keys that equal an importer's key are dropped. */
    private fun element(w: JsonWriter, e: JsonElement) {
        when {
            e.isJsonNull -> w.nullValue()
            e.isJsonObject -> { w.beginObject(); for ((k, v) in e.asJsonObject.entrySet()) { if (k in TRAPS || v.isJsonNull) continue; w.name(k); element(w, v) }; w.endObject() }
            e.isJsonArray -> { w.beginArray(); for (v in e.asJsonArray) element(w, v); w.endArray() }
            else -> { val p = e.asJsonPrimitive; when { p.isBoolean -> w.value(p.asBoolean); p.isNumber -> w.value(p.asNumber); else -> strValue(w, p.asString) } }
        }
    }
}
