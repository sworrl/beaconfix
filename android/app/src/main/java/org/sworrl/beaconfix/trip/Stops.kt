package org.sworrl.beaconfix.trip

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.estimate.Geo
import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.OffsetDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/** One position on the journal's path: a phone or desktop fix, or a stop from the desktop's track. [acc] ≤ 0 = unknown. */
data class PathPoint(val time: Long, val lat: Double, val lon: Double, val acc: Double = 0.0, val ele: Double? = null)

/**
 * A place the RV (or this phone) stayed. [arrival]/[departure] are epoch ms; [departure] is the last time it was seen
 * there, and [open] marks the newest stop, where it may still be. [dwellS] is -1 when unknown; [legM] is the distance
 * covered since the previous stop (for the first one, since the first position known); [elevM] is null without elevation.
 */
data class Stop(
    val lat: Double, val lon: Double,
    val arrival: Long, val departure: Long?, val dwellS: Long,
    val legM: Double, val elevM: Double? = null,
    val place: String = "", val source: String = "", val open: Boolean = false,
)

/** One calendar day of the journal (local time), stops oldest first. [distanceM] = the legs that ended that day. */
data class JournalDay(val date: LocalDate, val stops: List<Stop>, val distanceM: Double)

/** The trip journal: [source] = [Stops.TRACK] | [Stops.DESKTOP] | [Stops.PHONE] | "" (nothing yet); [path] = the positions it was built from. */
data class Journal(val source: String = "", val days: List<JournalDay> = emptyList(), val path: List<PathPoint> = emptyList()) {
    val stops: List<Stop> get() = days.flatMap { it.stops }
    val isEmpty: Boolean get() = days.isEmpty()
}

/**
 * The trip journal's rules (pure). Stops come from the desktop's own track (its `/track` answer is a list of stops),
 * else from the desktop's fixes stored by sync, else from this phone's fixes. From fixes, a stop is staying within
 * [STAY_M] for at least [STAY_MS]; fixes coarser than [MAX_ACC_M] and jumps faster than [MAX_KMH] are skipped first.
 */
object Stops {
    const val TRACK = "track"
    const val DESKTOP = "desktop"
    const val PHONE = "phone"

    const val STAY_M = 150.0
    const val STAY_MS = 10 * 60_000L
    const val MAX_ACC_M = 200.0
    const val MAX_KMH = 250.0

    private val json = Json { ignoreUnknownKeys = true; isLenient = true; coerceInputValues = true }

    /** One entry of the desktop's `/track` (locator.cpp: a stop with arrival, departure, dwell, leg and elevation). */
    @Serializable
    private data class TrackStop(
        val lat: Double = 0.0, val lon: Double = 0.0, val acc: Double = 0.0, val source: String = "", val place: String = "",
        val time: String = "", val departed: String = "", val dwellSecs: Long = -1, val legKm: Double = -1.0, val legSecs: Long = -1,
        val elev: Double? = null, val city: String = "", val region: String = "", val country: String = "",
    )
    @Serializable private data class TrackBody(val track: List<TrackStop> = emptyList())

    /** A desktop timestamp: with an offset (`…Z`, `…+02:00`) as given, without one in [zone] (the desktop writes local time). */
    fun parseTime(s: String?, zone: ZoneId): Long? {
        if (s.isNullOrBlank()) return null
        runCatching { return OffsetDateTime.parse(s).toInstant().toEpochMilli() }
        if (s.length < 19) return null
        return runCatching { LocalDateTime.parse(s.take(19), DateTimeFormatter.ISO_LOCAL_DATE_TIME).atZone(zone).toInstant().toEpochMilli() }.getOrNull()
    }

    /** The stops in a raw `/track` body (the `track` snapshot), oldest first; coarse or undated entries are skipped. */
    fun fromTrack(raw: String?, zone: ZoneId = ZoneId.systemDefault()): List<Stop> {
        if (raw.isNullOrBlank()) return emptyList()
        val rows = runCatching { json.decodeFromString(TrackBody.serializer(), raw).track }.getOrDefault(emptyList())
        data class T(val s: TrackStop, val at: Long)
        val dated = rows.mapNotNull { r ->
            if (r.acc > MAX_ACC_M) return@mapNotNull null
            if (r.lat == 0.0 && r.lon == 0.0) return@mapNotNull null
            parseTime(r.time, zone)?.let { T(r, it) }
        }.sortedBy { it.at }
        val out = ArrayList<Stop>()
        for ((i, t) in dated.withIndex()) {
            val r = t.s
            val dep = parseTime(r.departed, zone)
            val dwell = when { r.dwellSecs >= 0 -> r.dwellSecs; dep != null && dep >= t.at -> (dep - t.at) / 1000; else -> -1L }
            val prev = out.lastOrNull()
            val leg = when { r.legKm >= 0 -> r.legKm * 1000; prev != null -> Geo.distanceM(prev.lat, prev.lon, r.lat, r.lon); else -> 0.0 }
            out += Stop(r.lat, r.lon, t.at, dep ?: if (dwell >= 0) t.at + dwell * 1000 else null, dwell, leg, r.elev,
                place = r.place.ifEmpty { listOf(r.city, r.region).filter { it.isNotEmpty() }.joinToString(", ") },
                source = TRACK, open = i == dated.lastIndex && dep == null)
        }
        return out
    }

    /** Fix rows as path points. */
    fun points(fixes: List<FixEntity>): List<PathPoint> = fixes.map { PathPoint(it.time, it.lat, it.lon, it.acc) }

    /**
     * Sorted, with fixes coarser than [MAX_ACC_M] and single jumps faster than [MAX_KMH] removed. Three points in a
     * row that agree with each other but not with the last kept one mean that one was the outlier: they are kept.
     */
    fun clean(points: List<PathPoint>): List<PathPoint> {
        val kept = ArrayList<PathPoint>()
        val rejected = ArrayList<PathPoint>()
        for (p in points.sortedBy { it.time }) {
            if (p.acc > MAX_ACC_M) continue
            if (p.lat == 0.0 && p.lon == 0.0) continue
            val last = kept.lastOrNull()
            if (last == null) { kept += p; continue }
            if (p.time == last.time) continue
            if (plausible(last, p)) { kept += p; rejected.clear(); continue }
            if (rejected.isNotEmpty() && !plausible(rejected.last(), p)) rejected.clear()
            rejected += p
            if (rejected.size >= 3) {
                if (kept.size == 1) kept.clear()        // a lone first fix far from everything after it
                kept += rejected; rejected.clear()
            }
        }
        return kept
    }

    private fun plausible(a: PathPoint, b: PathPoint): Boolean {
        val h = (b.time - a.time) / 3_600_000.0
        if (h <= 0) return false
        return Geo.distanceM(a.lat, a.lon, b.lat, b.lon) / 1000.0 / h <= MAX_KMH
    }

    /** Path length in metres. */
    fun length(points: List<PathPoint>, from: Int = 0, to: Int = points.size - 1): Double {
        var m = 0.0
        for (i in maxOf(from, 0) until minOf(to, points.size - 1)) m += Geo.distanceM(points[i].lat, points[i].lon, points[i + 1].lat, points[i + 1].lon)
        return m
    }

    /** Stops in a cleaned path: runs of fixes within [STAY_M] of their centroid lasting at least [STAY_MS]. */
    fun detect(path: List<PathPoint>, source: String): List<Stop> {
        val out = ArrayList<Stop>()
        var i = 0; var lastEnd = 0
        while (i < path.size) {
            var sLat = path[i].lat; var sLon = path[i].lon; var n = 1; var j = i
            var eSum = path[i].ele ?: 0.0; var eN = if (path[i].ele != null) 1 else 0
            while (j + 1 < path.size && Geo.distanceM(sLat / n, sLon / n, path[j + 1].lat, path[j + 1].lon) <= STAY_M) {
                j++; sLat += path[j].lat; sLon += path[j].lon; n++
                path[j].ele?.let { eSum += it; eN++ }
            }
            if (path[j].time - path[i].time >= STAY_MS) {
                out += Stop(sLat / n, sLon / n, path[i].time, path[j].time, (path[j].time - path[i].time) / 1000,
                    legM = length(path, lastEnd, i),
                    elevM = if (eN > 0) eSum / eN else null, source = source, open = j == path.lastIndex)
                lastEnd = j; i = j + 1
            } else i++
        }
        return out
    }

    /** Stops grouped by local day of arrival, oldest day first. */
    fun byDay(stops: List<Stop>, zone: ZoneId = ZoneId.systemDefault()): List<JournalDay> =
        stops.sortedBy { it.arrival }.groupBy { Instant.ofEpochMilli(it.arrival).atZone(zone).toLocalDate() }
            .map { (d, s) -> JournalDay(d, s, s.sumOf { it.legM }) }.sortedBy { it.date }

    /**
     * The journal from the best source that has stops: the desktop's track ([trackJson]), else its stored fixes,
     * else this phone's. With no stops anywhere, the first source that has positions is still named (and its path
     * kept, for the GPX export).
     */
    fun journal(trackJson: String?, desktopFixes: List<FixEntity>, phoneFixes: List<FixEntity>, zone: ZoneId = ZoneId.systemDefault()): Journal {
        val track = fromTrack(trackJson, zone)
        if (track.isNotEmpty()) return Journal(TRACK, byDay(track, zone), track.map { PathPoint(it.arrival, it.lat, it.lon, 0.0, it.elevM) })
        val desk = clean(points(desktopFixes))
        val deskStops = detect(desk, DESKTOP)
        if (deskStops.isNotEmpty()) return Journal(DESKTOP, byDay(deskStops, zone), desk)
        val phone = clean(points(phoneFixes))
        val phoneStops = detect(phone, PHONE)
        if (phoneStops.isNotEmpty()) return Journal(PHONE, byDay(phoneStops, zone), phone)
        return when {
            desk.isNotEmpty() -> Journal(DESKTOP, emptyList(), desk)
            phone.isNotEmpty() -> Journal(PHONE, emptyList(), phone)
            else -> Journal()
        }
    }

    /** This phone's day: distance covered and stops since local midnight of [now]. */
    data class PhoneDay(val distanceM: Double, val stops: Int, val fixes: Int)

    fun phoneDay(phoneFixes: List<FixEntity>, now: Long, zone: ZoneId = ZoneId.systemDefault()): PhoneDay {
        val midnight = Instant.ofEpochMilli(now).atZone(zone).toLocalDate().atStartOfDay(zone).toInstant().toEpochMilli()
        val today = clean(points(phoneFixes.filter { it.time in midnight..now }))
        return PhoneDay(length(today), detect(today, PHONE).size, today.size)
    }
}
