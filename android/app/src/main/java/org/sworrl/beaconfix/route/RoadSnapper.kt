package org.sworrl.beaconfix.route

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.doubleOrNull
import okhttp3.OkHttpClient
import okhttp3.Request
import org.sworrl.beaconfix.BuildConfig
import org.sworrl.beaconfix.collector.MotionMode
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.estimate.Geo
import java.io.IOException
import java.util.Locale
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton

data class SnappedPoint(
    val lat: Double,
    val lon: Double,
    val isSnapped: Boolean = false
)

data class RouteSegment(
    val mode: MotionMode,
    val points: List<SnappedPoint>,
    val startTime: Long = 0L,
    val endTime: Long = 0L
)

/** One OSRM answer: HTTP status and body (null body = no answer at all). */
data class OsrmReply(val http: Int, val body: String?)

/**
 * Road snapping for the map's travelled routes. Vehicular stretches go to the FOSSGIS OSRM demo server's `match`
 * service (router.project-osrm.org), which is free for reasonable non-commercial use under its terms: at most one
 * request a second, an identifying User-Agent, credit for the routes ([org.sworrl.beaconfix.ui.map.TileStyles.ROUTING_CREDIT]).
 * The server takes at most [MAX_COORDS] trace points per request, so a stretch is thinned to [MIN_SPACING_M] and sent
 * in windows of [MAX_COORDS] that share their end points. A window is asked about once: full windows and closed-off
 * stretches are cached (an answer or a "no match"); the still-growing last window of a drive in progress is drawn
 * smoothed locally until it fills up. A pass sends at most [MAX_REQUESTS_PER_PASS]; a 429, a 5xx or no answer pauses
 * the service for [BACKOFF_MS]. Everything not snapped yet is drawn with the local smoothing.
 */
@Singleton
class RoadSnapper internal constructor(
    private val fetch: suspend (String) -> OsrmReply,
    private val now: () -> Long,
    private val pause: suspend (Long) -> Unit,
) {
    @Inject constructor() : this(::httpGet, System::currentTimeMillis, { delay(it) })

    /** Window key → snapped points (an empty list = OSRM found no match: draw it smoothed, do not ask again). */
    private val windowCache = object : LinkedHashMap<String, List<SnappedPoint>>(256, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, List<SnappedPoint>>?) = size > MAX_CACHED_WINDOWS
    }
    private val gate = Mutex()
    @Volatile private var lastRequestAt = 0L
    @Volatile private var backoffUntil = 0L
    /** OSRM requests left in the current [snapTrack] pass. */
    private var budget = 0

    /**
     * Splits fixes into vehicular vs pedestrian segments, snapping vehicular segments
     * to road centerlines via OSRM map-matching (with local trajectory smoothing fallback),
     * and preserving exact natural geometry for pedestrian footpaths.
     */
    suspend fun snapTrack(fixes: List<FixEntity>): List<RouteSegment> = withContext(Dispatchers.IO) {
        val valid = fixes.filter { it.lat != 0.0 && it.lon != 0.0 }.sortedBy { it.time }
        if (valid.size < 2) return@withContext emptyList()

        val rawSegments = partitionSegments(valid)
        budget = MAX_REQUESTS_PER_PASS
        rawSegments.mapIndexedNotNull { i, seg ->
            when (seg.mode) {
                MotionMode.IN_VEHICLE -> {
                    val snapped = matchVehicularSegment(seg.rawPoints, open = i == rawSegments.lastIndex && now() - seg.rawPoints.last().time < OPEN_MS)
                    RouteSegment(
                        mode = MotionMode.IN_VEHICLE,
                        points = snapped,
                        startTime = seg.rawPoints.first().time,
                        endTime = seg.rawPoints.last().time
                    )
                }
                MotionMode.ON_FOOT -> {
                    val footPoints = filterPedestrianJitter(seg.rawPoints)
                    if (footPoints.size < 2) null
                    else RouteSegment(
                        mode = MotionMode.ON_FOOT,
                        points = footPoints,
                        startTime = seg.rawPoints.first().time,
                        endTime = seg.rawPoints.last().time
                    )
                }
                MotionMode.STATIONARY -> {
                    val centroid = computeStationaryCentroid(seg.rawPoints)
                    RouteSegment(
                        mode = MotionMode.STATIONARY,
                        points = listOf(centroid),
                        startTime = seg.rawPoints.first().time,
                        endTime = seg.rawPoints.last().time
                    )
                }
            }
        }
    }

    private data class RawSegment(
        val mode: MotionMode,
        val rawPoints: List<FixEntity>
    )

    private fun partitionSegments(fixes: List<FixEntity>): List<RawSegment> {
        val result = mutableListOf<RawSegment>()
        if (fixes.isEmpty()) return result

        var currentMode = MotionMode.STATIONARY
        var currentChunk = mutableListOf<FixEntity>()

        for (i in fixes.indices) {
            val f = fixes[i]
            if (currentChunk.isEmpty()) {
                currentChunk.add(f)
                currentMode = when {
                    f.source.contains("vehicle") -> MotionMode.IN_VEHICLE
                    f.source.contains("foot") -> MotionMode.ON_FOOT
                    f.source.contains("stationary") -> MotionMode.STATIONARY
                    else -> MotionMode.STATIONARY
                }
                continue
            }

            val prev = currentChunk.last()
            val dtSec = (f.time - prev.time) / 1000.0
            val distM = Geo.distanceM(prev.lat, prev.lon, f.lat, f.lon)

            // Gap greater than 10 minutes indicates a separate trip
            if (dtSec > 600.0) {
                if (currentChunk.isNotEmpty()) {
                    result.add(RawSegment(currentMode, currentChunk.toList()))
                    currentChunk = mutableListOf()
                }
                currentChunk.add(f)
                currentMode = when {
                    f.source.contains("vehicle") -> MotionMode.IN_VEHICLE
                    f.source.contains("foot") -> MotionMode.ON_FOOT
                    f.source.contains("stationary") -> MotionMode.STATIONARY
                    else -> MotionMode.STATIONARY
                }
                continue
            }

            val pointMode = when {
                f.source.contains("vehicle") -> MotionMode.IN_VEHICLE
                f.source.contains("foot") -> MotionMode.ON_FOOT
                f.source.contains("stationary") -> MotionMode.STATIONARY
                else -> {
                    var isSpike = false
                    if (i < fixes.size - 1) {
                        val next = fixes[i + 1]
                        val dNext = Geo.distanceM(f.lat, f.lon, next.lat, next.lon)
                        val dChord = Geo.distanceM(prev.lat, prev.lon, next.lat, next.lon)
                        if (distM > 30.0 && dNext > 30.0 && dChord < 18.0) {
                            isSpike = true
                        }
                    }
                    if (isSpike) {
                        currentMode
                    } else {
                        val speedKmh = if (dtSec > 0.5) (distM / dtSec) * 3.6 else 0.0
                        when {
                            speedKmh >= 14.0 -> MotionMode.IN_VEHICLE
                            speedKmh in 0.8..14.0 -> MotionMode.ON_FOOT
                            else -> MotionMode.STATIONARY
                        }
                    }
                }
            }

            // Hysteresis: red lights and short stops (< 180s) within driving remain in vehicle mode.
            // Short pedestrian stops (< 120s) remain on foot.
            val targetMode = if (pointMode == MotionMode.STATIONARY && currentMode == MotionMode.IN_VEHICLE && dtSec < 180.0) {
                MotionMode.IN_VEHICLE
            } else if (pointMode == MotionMode.STATIONARY && currentMode == MotionMode.ON_FOOT && dtSec < 120.0) {
                MotionMode.ON_FOOT
            } else {
                pointMode
            }

            if (currentChunk.size >= 2 && targetMode != currentMode) {
                result.add(RawSegment(currentMode, currentChunk.toList()))
                currentChunk = mutableListOf(prev, f)
                currentMode = targetMode
            } else {
                currentMode = targetMode
                currentChunk.add(f)
            }
        }

        if (currentChunk.isNotEmpty()) {
            result.add(RawSegment(currentMode, currentChunk))
        }

        return result
    }

    /**
     * [points] snapped window by window (see the class comment); a window OSRM cannot match, or has not been asked
     * about yet, is drawn smoothed. [open]: the drive is still going on, so its last, partial window is not sent yet.
     */
    private suspend fun matchVehicularSegment(points: List<FixEntity>, open: Boolean): List<SnappedPoint> {
        if (points.size < 2) return points.map { SnappedPoint(it.lat, it.lon, isSnapped = false) }
        val out = mutableListOf<SnappedPoint>()
        for (w in windows(thin(points))) {
            val first = w.first(); val last = w.last()
            val partial = w.size < MAX_COORDS
            val cached = synchronized(windowCache) { windowCache[windowKey(w)] }
            val snapped = cached ?: if (partial && open) null else osrmMatch(w)?.also { r -> synchronized(windowCache) { windowCache[windowKey(w)] = r } }
            val piece = snapped?.takeIf { it.isNotEmpty() } ?: smoothVehicularTrajectory(points.filter { it.time in first.time..last.time })
            // windows share their end points: drop the repeated join
            out += if (out.isNotEmpty() && piece.isNotEmpty() && Geo.distanceM(out.last().lat, out.last().lon, piece.first().lat, piece.first().lon) < 1.0) piece.drop(1) else piece
        }
        // the fixes after the last thinned point (less than [MIN_SPACING_M] on) are drawn as they are
        points.last().let { p -> if (out.isEmpty() || Geo.distanceM(out.last().lat, out.last().lon, p.lat, p.lon) >= 1.0) out += SnappedPoint(p.lat, p.lon, isSnapped = false) }
        return out
    }

    /**
     * One OSRM `match` request for a window: the snapped geometry, an empty list when OSRM says the trace matches no
     * road (NoMatch / NoSegment: worth remembering, not retrying), or null when there is no answer to trust right now
     * (rate limited, server trouble, offline, this pass's budget spent, backing off) — try again on a later pass.
     */
    private suspend fun osrmMatch(w: List<FixEntity>): List<SnappedPoint>? = gate.withLock {
        if (budget <= 0 || now() < backoffUntil) return@withLock null
        budget--
        val wait = lastRequestAt + MIN_GAP_MS - now()
        if (wait > 0) pause(wait)
        lastRequestAt = now()
        val r = runCatching { fetch(matchUrl(w)) }.getOrElse { OsrmReply(0, null) }
        when (val m = parseMatch(r.http, r.body)) {
            is Match.Snapped -> m.points
            Match.NoMatch -> emptyList()
            Match.Retry -> { backoffUntil = now() + BACKOFF_MS; null }
            Match.Rejected -> emptyList()
        }
    }

    private fun smoothVehicularTrajectory(points: List<FixEntity>): List<SnappedPoint> {
        val out = mutableListOf<SnappedPoint>()
        var prevLat = 0.0
        var prevLon = 0.0

        for (i in points.indices) {
            val pt = points[i]
            if (i == 0) {
                out.add(SnappedPoint(pt.lat, pt.lon, isSnapped = false))
                prevLat = pt.lat
                prevLon = pt.lon
                continue
            }

            val d = Geo.distanceM(prevLat, prevLon, pt.lat, pt.lon)
            if (d > 250.0 && pt.acc > 30.0) {
                // Reject high-error multipath jump
                continue
            }

            // Exponential smoothing along road heading
            val smoothLat = 0.65 * pt.lat + 0.35 * prevLat
            val smoothLon = 0.65 * pt.lon + 0.35 * prevLon
            out.add(SnappedPoint(smoothLat, smoothLon, isSnapped = false))
            prevLat = smoothLat
            prevLon = smoothLon
        }

        return out
    }

    private fun computeStationaryCentroid(points: List<FixEntity>): SnappedPoint {
        if (points.isEmpty()) return SnappedPoint(0.0, 0.0)
        var sumLat = 0.0
        var sumLon = 0.0
        var sumWeight = 0.0
        for (pt in points) {
            val w = 1.0 / (pt.acc.coerceAtLeast(1.0) * pt.acc.coerceAtLeast(1.0))
            sumLat += pt.lat * w
            sumLon += pt.lon * w
            sumWeight += w
        }
        return if (sumWeight > 0.0) {
            SnappedPoint(sumLat / sumWeight, sumLon / sumWeight, isSnapped = false)
        } else {
            val best = points.minByOrNull { it.acc } ?: points.first()
            SnappedPoint(best.lat, best.lon, isSnapped = false)
        }
    }

    private fun filterPedestrianJitter(points: List<FixEntity>): List<SnappedPoint> {
        if (points.size < 2) return points.map { SnappedPoint(it.lat, it.lon, isSnapped = false) }

        // Filter 1: Drop coarse GPS multipath spikes > 35m
        val filteredAcc = points.filter { it.acc <= 35.0 || points.size <= 2 }
        val base = if (filteredAcc.size >= 2) filteredAcc else points

        // Filter 2: Drop single-point spike jumps (point i jumps > 30m out and back within 18m)
        val nonSpikes = mutableListOf<FixEntity>()
        for (i in base.indices) {
            val curr = base[i]
            if (i > 0 && i < base.size - 1) {
                val prev = base[i - 1]
                val next = base[i + 1]
                val dPrev = Geo.distanceM(prev.lat, prev.lon, curr.lat, curr.lon)
                val dNext = Geo.distanceM(curr.lat, curr.lon, next.lat, next.lon)
                val dChord = Geo.distanceM(prev.lat, prev.lon, next.lat, next.lon)
                if (dPrev > 30.0 && dNext > 30.0 && dChord < 18.0) {
                    continue
                }
            }
            nonSpikes.add(curr)
        }

        // Filter 3: Compress micro-jitter (points < 2.5m apart along pedestrian path)
        val out = mutableListOf<SnappedPoint>()
        for (pt in nonSpikes) {
            if (out.isEmpty()) {
                out.add(SnappedPoint(pt.lat, pt.lon, isSnapped = false))
            } else {
                val last = out.last()
                val d = Geo.distanceM(last.lat, last.lon, pt.lat, pt.lon)
                if (d >= 2.5) {
                    out.add(SnappedPoint(pt.lat, pt.lon, isSnapped = false))
                }
            }
        }
        if (out.size < 2 && nonSpikes.isNotEmpty()) {
            return nonSpikes.map { SnappedPoint(it.lat, it.lon, isSnapped = false) }
        }
        return out
    }

    /** What one OSRM answer means for the window it was about. */
    sealed interface Match {
        data class Snapped(val points: List<SnappedPoint>) : Match
        /** NoMatch / NoSegment: no road fits the trace. */
        data object NoMatch : Match
        /** The request itself was refused (TooBig, InvalidValue, …): asking again will not help. */
        data object Rejected : Match
        /** 429, a 5xx, no answer, or an answer that is not OSRM's: back off and ask later. */
        data object Retry : Match
    }

    companion object {
        /** The demo server's limit on trace points per `match` request (measured: 10 pass, 11 get TooBig). */
        const val MAX_COORDS = 10
        /** Trace points are kept at least this far apart, so one request covers about half a kilometre of road. */
        const val MIN_SPACING_M = 50.0
        /** Search radius per trace point (m). */
        const val RADIUS_M = 25
        /** At most one request a second (the server's terms), with a margin. */
        const val MIN_GAP_MS = 1_100L
        const val MAX_REQUESTS_PER_PASS = 20
        const val BACKOFF_MS = 5 * 60_000L
        /** A drive whose last fix is newer than this is still going on. */
        const val OPEN_MS = 2 * 60_000L
        const val MAX_CACHED_WINDOWS = 4_000
        const val BASE_URL = "https://router.project-osrm.org/match/v1/driving/"
        val USER_AGENT = "BeaconFix-Android/${BuildConfig.VERSION_NAME} (+https://github.com/sworrl/beaconfix)"

        private val json = Json { ignoreUnknownKeys = true; isLenient = true }
        private val http by lazy {
            OkHttpClient.Builder().connectTimeout(5, TimeUnit.SECONDS).readTimeout(8, TimeUnit.SECONDS).callTimeout(12, TimeUnit.SECONDS).build()
        }

        private fun httpGet(url: String): OsrmReply = try {
            http.newCall(Request.Builder().url(url).header("User-Agent", USER_AGENT).header("Accept", "application/json").build()).execute()
                .use { r -> OsrmReply(r.code, r.body?.string()) }
        } catch (e: IOException) { OsrmReply(0, null) }

        /**
         * [points] with each kept point at least [MIN_SPACING_M] from the one before, picked greedily from the start, so
         * a growing drive keeps the same earlier points (and its full windows their cache keys). A stretch shorter than
         * that is its first and last fix.
         */
        fun thin(points: List<FixEntity>): List<FixEntity> {
            val out = mutableListOf<FixEntity>()
            for (p in points) if (out.isEmpty() || Geo.distanceM(out.last().lat, out.last().lon, p.lat, p.lon) >= MIN_SPACING_M) out += p
            return if (out.size < 2 && points.size >= 2) listOf(points.first(), points.last()) else out
        }

        /** Windows of at most [MAX_COORDS] points, each starting where the previous one ended (a single point is no window). */
        fun windows(points: List<FixEntity>): List<List<FixEntity>> {
            if (points.size < 2) return emptyList()
            val out = mutableListOf<List<FixEntity>>()
            var start = 0
            while (start < points.lastIndex) {
                val end = minOf(start + MAX_COORDS - 1, points.lastIndex)
                out += points.subList(start, end + 1)
                start = end
            }
            return out
        }

        fun windowKey(w: List<FixEntity>): String =
            String.format(Locale.US, "%d_%d_%d_%.5f_%.5f_%.5f_%.5f", w.size, w.first().time, w.last().time, w.first().lat, w.first().lon, w.last().lat, w.last().lon)

        /** `match/v1/driving/lon,lat;…?…`, Locale.US numbers (a comma decimal would be a different request). */
        fun matchUrl(w: List<FixEntity>): String =
            BASE_URL + w.joinToString(";") { String.format(Locale.US, "%.6f,%.6f", it.lon, it.lat) } +
                "?overview=full&geometries=geojson&steps=false&radiuses=" + w.joinToString(";") { RADIUS_M.toString() }

        /** An OSRM `match` answer ([http] status, [body]) → [Match]. Every matching's geometry is concatenated in order. */
        fun parseMatch(http: Int, body: String?): Match {
            val root = body?.let { runCatching { json.parseToJsonElement(it) }.getOrNull() } as? JsonObject
            val code = (root?.get("code") as? JsonPrimitive)?.contentOrNull
            if (http == 429 || http >= 500 || http == 0 || root == null || code == null) return Match.Retry
            when (code) {
                "Ok" -> {}
                "NoMatch", "NoSegment" -> return Match.NoMatch
                else -> return Match.Rejected
            }
            val out = mutableListOf<SnappedPoint>()
            for (m in (root["matchings"] as? JsonArray).orEmpty()) {
                val coords = ((m as? JsonObject)?.get("geometry") as? JsonObject)?.get("coordinates") as? JsonArray ?: continue
                for (c in coords) {
                    val pair = c as? JsonArray ?: continue
                    val lon = (pair.getOrNull(0) as? JsonPrimitive)?.doubleOrNull ?: continue
                    val lat = (pair.getOrNull(1) as? JsonPrimitive)?.doubleOrNull ?: continue
                    if (out.isEmpty() || out.last().lat != lat || out.last().lon != lon) out += SnappedPoint(lat, lon, isSnapped = true)
                }
            }
            return if (out.size >= 2) Match.Snapped(out) else Match.NoMatch
        }
    }
}
