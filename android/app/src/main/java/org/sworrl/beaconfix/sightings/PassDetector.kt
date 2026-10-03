// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.sin
import kotlin.math.sqrt

/** One route fix of one device: [acc] / [speedMps] only when the receiver reported them. */
data class RouteFix(val timeMs: Long, val lat: Double, val lon: Double, val acc: Double? = null, val speedMps: Double? = null,
                    val bearingDeg: Double? = null, val device: String = "", val source: String = "")

/**
 * A camera of the desktop's camera map (`/api/v1/flock`). [type] is §2.0's camera_type (alpr | webcam | ptz | cctv |
 * enforcement | not_camera), [tags] the OSM tags when known, [webcam] a public live feed (`contact:webcam`).
 */
data class PassCamera(
    val id: String, val lat: Double, val lon: Double, val model: String = "", val operator: String = "", val source: String = "",
    val direction: String = "", val detectionMethod: String = "", val notes: String = "", val type: String = "alpr",
    val webcam: String? = null, val tags: JsonObject = JsonObject(emptyMap()), val sourceConfidence: Int = 100,
    /** the camera's maker (DeFlock's brand / OSM manufacturer), "" unknown: picks the §2.3 cone */
    val manufacturer: String = "",
    /** the desktop's camera trust (§2.6, 0–1), < 0 when the desktop did not send one */
    val trust: Double = -1.0,
)

/** §2.3: an ALPR reads only inside a narrow cone and range (vendor datasheets). */
data class Cone(val halfDeg: Double = 12.5, val minM: Double = 5.0, val maxM: Double = 35.0, val name: String = "unknown fixed ALPR")

/** A detected pass (§2.1–2.4); unknowns are null. */
data class Pass(
    val camera: PassCamera,
    val closestMs: Long, val lat: Double, val lon: Double, val acc: Double?,
    val distanceM: Double, val speedKmh: Double?, val headingDeg: Double?, val approachBearingDeg: Double,
    val cameraDirDeg: Double?, val facing: Boolean?, val frontVisible: Boolean, val rearVisible: Boolean,
    val frontBearingDeg: Double, val rearBearingDeg: Double, val directions: List<Double>,
    val dwellS: Double, val fixes: Int, val fixDevices: List<String>, val fixSources: List<String>,
    val enterDistanceM: Double, val exitDistanceM: Double, val startMs: Long, val endMs: Long,
    val cone: Cone, val inConeS: Double, val pInCone: Double, val pRead: Double,
    val confidence: Int, val confidenceAlpr: Int,
) {
    val cameraType: String get() = camera.type.ifEmpty { "alpr" }
}

/**
 * docs/SIGHTINGS.md §2 on the phone — a port of the desktop's `src/plateevents.cpp` (PlateEvents::detectPasses,
 * classifyCamera, parseDirections, coneFor, confidenceFor), so a pass computed here and the same pass computed by
 * the desktop's backfill agree. Pure (no Android); unit-tested in PassDetectorTest.
 */
object PassDetector {
    const val RADIUS_M = 65.0
    const val GAP_MS = 5 * 60_000L
    const val MERGE_WINDOW_MS = 10 * 60_000L
    const val MAX_SEGMENT_M = 1500.0
    const val SIDE_MS = 10_000L
    const val CONE_MARGIN_DEG = 3.0
    const val CAPTURE = 0.97
    const val READ = 0.93
    private const val R = 6_371_000.0
    private const val D2R = Math.PI / 180.0

    fun distanceM(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val dLat = (lat2 - lat1) * D2R; val dLon = (lon2 - lon1) * D2R
        val a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * D2R) * cos(lat2 * D2R) * sin(dLon / 2) * sin(dLon / 2)
        return 2 * R * Math.asin(sqrt(minOf(1.0, a)))
    }

    fun bearingDeg(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val y = sin((lon2 - lon1) * D2R) * cos(lat2 * D2R)
        val x = cos(lat1 * D2R) * sin(lat2 * D2R) - sin(lat1 * D2R) * cos(lat2 * D2R) * cos((lon2 - lon1) * D2R)
        return (atan2(y, x) / D2R + 360.0) % 360.0
    }

    fun angleDiff(a: Double, b: Double): Double { val d = abs(a - b) % 360.0; return if (d > 180.0) 360.0 - d else d }

    // ── §2.0 classification ──
    private val PLATE_READER = Regex("""\b(alpr|lpr|anpr)\b|plate|falcon|sparrow|autovu|sharpv|vigilant|elsag|platesmart|neology|rekor|l5f|l6q""", RegexOption.IGNORE_CASE)
    private val NOT_PLATE = Regex("""^\s*camera\s*$|other surveillance|traffic cam|cctv|webcam""", RegexOption.IGNORE_CASE)
    /** Flock's Raven is a gunshot detector (an RF detection of it, or a list's label): not a camera at all */
    private val GUNSHOT = Regex("""\braven\b|gunshot""", RegexOption.IGNORE_CASE)
    val TYPES = setOf("alpr", "webcam", "ptz", "cctv", "enforcement", "not_camera")

    fun isPlateReaderModel(model: String): Boolean = PLATE_READER.containsMatchIn(model)
    fun isOsmId(id: String): Boolean = id.startsWith("osm:")

    private fun JsonObject.str(k: String): String = (this[k] as? JsonPrimitive)?.contentOrNull.orEmpty()
    private fun typeTokens(v: String): List<String> = v.split(Regex("[;,]")).map { it.trim().uppercase() }.filter { it.isNotEmpty() }
    private fun List<String>.hasAny(vararg w: String) = w.any { it in this }

    fun webcamUrl(tags: JsonObject): String {
        for (k in listOf("contact:webcam", "webcam", "surveillance:webcam")) {
            val v = tags.str(k).trim()
            if (v.startsWith("http://") || v.startsWith("https://")) return v.substringBefore(';').trim()
        }
        return ""
    }

    /** A note about the tags: a misspelt ALPR type (reduced confidence), contact:webcam on an ALPR (a tagging conflict). */
    fun classifyNote(tags: JsonObject): String {
        val st = typeTokens(tags.str("surveillance:type"))
        val notes = ArrayList<String>()
        if (st.hasAny("ALRP", "APLR", "AMPR") && !st.hasAny("ALPR", "ANPR")) notes += "surveillance:type is a misspelt ALPR (${tags.str("surveillance:type")}): reduced confidence"
        val alpr = st.hasAny("ALPR", "ANPR", "ALRP", "APLR", "AMPR") || tags.str("camera:type").equals("ALPR", true) || tags.str("surveillance").equals("ANPR", true)
        if (alpr && webcamUrl(tags).isNotEmpty()) notes += "contact:webcam on an ALPR: a tagging conflict"
        return notes.joinToString("; ")
    }

    /** §2.0, in order: enforcement, not_camera, alpr, webcam, ptz, cctv. Non-OSM rows are plate-reader lists (alpr). */
    fun classifyCamera(model: String, source: String, id: String, tags: JsonObject = JsonObject(emptyMap())): String {
        val osm = isOsmId(id) || source.equals("osm", ignoreCase = true)
        if (!osm) {
            if (GUNSHOT.containsMatchIn(model) && !isPlateReaderModel(model)) return "not_camera"
            return if (NOT_PLATE.containsMatchIn(model) && !isPlateReaderModel(model)) "cctv" else "alpr"
        }
        if (tags.isNotEmpty()) {
            val st = typeTokens(tags.str("surveillance:type"))
            if (tags.str("highway") == "speed_camera" || (tags["_enforcement"] as? JsonPrimitive)?.booleanOrNull == true || tags.str("enforcement").isNotEmpty()) return "enforcement"
            if (st.hasAny("GUNSHOT_DETECTOR", "GUARD") || (tags.str("man_made") == "monitoring_station" && tags.str("monitoring:traffic").isNotEmpty())) return "not_camera"
            if (st.hasAny("ALPR", "ANPR", "ALRP", "APLR", "AMPR") || tags.str("camera:type").equals("ALPR", true) || tags.str("surveillance").equals("ANPR", true)) return "alpr"
            val camera = st.hasAny("CAMERA")
            if (camera && webcamUrl(tags).isNotEmpty()) return "webcam"
            if (camera && (tags.str("manufacturer").contains("Flock", true) || tags.str("brand").contains("Flock", true))) return "ptz"
            return "cctv"
        }
        // no tags: DeFlock lists only man_made=surveillance + surveillance:type=ALPR; an older import is what it made of them
        if (source.equals("deflock", ignoreCase = true)) return "alpr"
        if (isPlateReaderModel(model)) return "alpr"
        if (webcamUrl(tags).isNotEmpty()) return "webcam"
        return "cctv"
    }

    /** Only a "Suspected" list knows it (§2.4: × 0.7). */
    fun suspectedOnly(source: String, model: String): Boolean = source.contains("suspected", true) || model.contains("(suspected)", true)

    /** A Flock Safety camera (the 12 h HaveIBeenFlocked schedule, §4.5). */
    fun isFlock(operator: String?, model: String?): Boolean =
        listOfNotNull(operator, model).any { it.contains("flock", true) } || Regex("falcon|condor|sparrow", RegexOption.IGNORE_CASE).containsMatchIn(model.orEmpty())

    // ── §2.3 directions ──
    private val CARDINALS = listOf("N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW")
    private val WORDS = mapOf("NORTH" to "N", "SOUTH" to "S", "EAST" to "E", "WEST" to "W", "NORTHEAST" to "NE", "SOUTHEAST" to "SE", "SOUTHWEST" to "SW",
        "NORTHWEST" to "NW", "NB" to "N", "SB" to "S", "EB" to "E", "WB" to "W")
    private val NUMERIC = Regex("""^[+-]?(\d+(\.\d*)?|\.\d+)$""")

    private fun cardinal(s: String): Double? {
        val u0 = s.trim().uppercase(); val u = WORDS[u0] ?: u0
        val i = CARDINALS.indexOf(u)
        return if (i < 0) null else i * 22.5
    }

    private fun number(t: String): Double? = t.trim().takeIf { NUMERIC.matches(it) }?.toDoubleOrNull()

    private fun oneDirection(tok: String, zeroUnknown: Boolean = true): Double? {
        val t = tok.trim()
        if (t.isEmpty()) return null
        number(t)?.let { d ->
            if (zeroUnknown && d == 0.0) return null                 // direction=0: unknown (the DeFlock app writes 0)
            return ((d % 360.0) + 360.0) % 360.0
        }
        return cardinal(t)
    }

    /** `direction` wins over `camera:direction` (§2.3). */
    fun directionText(cam: PassCamera): String {
        val d = cam.tags.str("direction").trim(); if (d.isNotEmpty()) return d
        val cd = cam.tags.str("camera:direction").trim(); if (cd.isNotEmpty()) return cd
        return cam.direction
    }

    /**
     * Degrees, cardinals (N … NNW), spelled-out and bound directions (NORTHEAST, EB), ranges `a-b` (the centre, clockwise;
     * `0-360` = none), lists separated by `;` or `,`; 0 = unknown. The grammar follows DeFlock's parser
     * (github.com/FoggedLens/deflock-data data/cameras/lib.mjs, MIT), as the desktop's PlateEvents::parseDirections does.
     */
    fun parseDirections(s: String?): List<Double> {
        if (s.isNullOrEmpty()) return emptyList()
        val out = ArrayList<Double>()
        for (part in s.split(';', ',')) {
            val p = part.trim()
            if (p.isEmpty()) continue
            val dash = if (p.length > 1) p.indexOf('-', 1) else -1
            if (dash > 0) {
                val a = oneDirection(p.substring(0, dash), false); val b = oneDirection(p.substring(dash + 1), false)
                if (a == null || b == null) continue
                val span = (b - a + 360.0) % 360.0
                val rawA = number(p.substring(0, dash)) ?: 0.0; val rawB = number(p.substring(dash + 1)) ?: 0.0
                if (span == 0.0 && rawB - rawA >= 359.0) continue            // all round: not a direction
                out += (a + span / 2.0) % 360.0
                continue
            }
            oneDirection(p)?.let { out += it }
        }
        return out
    }

    // ── §2.3 cone ──
    fun coneFor(cam: PassCamera): Cone {
        val parts = listOf(cam.model, cam.manufacturer) + listOf("manufacturer", "brand", "camera:model", "model", "operator").map { cam.tags.str(it) } + cam.operator
        val t = parts.joinToString(" ")
        fun has(re: String) = Regex(re, RegexOption.IGNORE_CASE).containsMatchIn(t)
        return when {
            has("falcon[ -]*(lr|long)|long[- ]range") -> Cone(7.0, 15.0, 76.0, "Flock Falcon LR")
            has("""motorola|vigilant|\bl5f\b|\bl6q\b""") -> Cone(12.0, 8.0, 23.0, "Motorola / Vigilant L5F, L6Q")
            has("genetec|autovu|sharpv") -> Cone(12.0, 3.0, 45.0, "Genetec AutoVu SharpV")
            has("verkada") -> Cone(22.0, 3.0, 20.0, "Verkada")
            has("flock|falcon|sparrow") -> Cone(10.0, 6.0, 25.0, "Flock Falcon")
            else -> Cone()
        }
    }

    // ── §2.4 ──
    fun confidenceFor(pRead: Double, cam: PassCamera, asAlpr: Boolean): Int {
        var c = 100.0 * pRead
        if (cam.trust >= 0) c *= cam.trust                                    // §2.6: the desktop's trust replaces the old × 0.7
        else if (suspectedOnly(cam.source, cam.model)) c *= 0.7
        if (classifyNote(cam.tags).contains("misspelt")) c *= 0.85
        val v = Math.round(c).toInt()
        return if (asAlpr) v else minOf(40, v)
    }

    /** §1.1: `pass:<camera_id>:<unix minute of the closest approach>`. */
    fun passUid(cameraId: String, closestMs: Long): String = "pass:$cameraId:${Math.floorDiv(closestMs, 60_000L)}"

    /** Same camera, closest approaches within ±10 min: the same pass (§1.1). */
    fun samePass(cameraA: String?, msA: Long, cameraB: String?, msB: Long): Boolean = cameraA != null && cameraA == cameraB && abs(msA - msB) <= MERGE_WINDOW_MS

    // ── §2.1 / §2.2 ──
    private class Xy(val x: Double, val y: Double)
    private class Proj(private val lat0: Double, private val lon0: Double) {
        private val kx = cos(lat0 * D2R) * R * D2R
        fun to(lat: Double, lon: Double) = Xy((lon - lon0) * kx, (lat - lat0) * R * D2R)
    }

    private fun segClosest(a: Xy, b: Xy): Pair<Double, Double> {
        val dx = b.x - a.x; val dy = b.y - a.y; val l2 = dx * dx + dy * dy
        val u = (if (l2 > 0) -(a.x * dx + a.y * dy) / l2 else 0.0).coerceIn(0.0, 1.0)
        return hypot(a.x + u * dx, a.y + u * dy) to u
    }

    private fun segInside(a: Xy, b: Xy, r: Double): Pair<Double, Double>? {
        val dx = b.x - a.x; val dy = b.y - a.y
        val aa = dx * dx + dy * dy; val bb = 2 * (a.x * dx + a.y * dy); val cc = a.x * a.x + a.y * a.y - r * r
        if (aa <= 0) return if (cc <= 0) 0.0 to 1.0 else null
        val disc = bb * bb - 4 * aa * cc
        if (disc < 0) return null
        val s = sqrt(disc)
        val u0 = maxOf(0.0, (-bb - s) / (2 * aa)); val u1 = minOf(1.0, (-bb + s) / (2 * aa))
        return if (u1 < u0) null else u0 to u1
    }

    fun segValid(a: RouteFix, b: RouteFix): Boolean {
        val dt = b.timeMs - a.timeMs
        return dt in 0..GAP_MS && distanceM(a.lat, a.lon, b.lat, b.lon) <= MAX_SEGMENT_M
    }

    /** The closest point of segment a→b to [c]: distance and segment parameter (LiveTracker). */
    fun closestOnSegment(c: PassCamera, a: RouteFix, b: RouteFix): Pair<Double, Double> {
        val pj = Proj(c.lat, c.lon)
        return segClosest(pj.to(a.lat, a.lon), pj.to(b.lat, b.lon))
    }

    private fun positionAt(f: List<RouteFix>, xy: List<Xy>, ms: Long, pc: Xy, tc: Long, vx: Double, vy: Double): Xy {
        for (k in 0 until f.size - 1) {
            if (f[k].timeMs <= ms && f[k + 1].timeMs >= ms && segValid(f[k], f[k + 1])) {
                val dt = f[k + 1].timeMs - f[k].timeMs
                val u = if (dt > 0) (ms - f[k].timeMs).toDouble() / dt.toDouble() else 0.0
                return Xy(xy[k].x + u * (xy[k + 1].x - xy[k].x), xy[k].y + u * (xy[k + 1].y - xy[k].y))
            }
        }
        val s = (ms - tc) / 1000.0
        return Xy(pc.x + vx * s, pc.y + vy * s)
    }

    private fun bearingOf(p: Xy): Double? = if (hypot(p.x, p.y) < 0.5) null else (atan2(p.x, p.y) / D2R + 360.0) % 360.0

    /** Camera → (x, y) east/north metres, in its cone and range for one of [dirs] (the phone's front-frame pinning). */
    fun inCone(cam: PassCamera, cone: Cone, dirs: List<Double>, lat: Double, lon: Double): Boolean {
        val p = Proj(cam.lat, cam.lon).to(lat, lon)
        val dd = hypot(p.x, p.y)
        if (dd < cone.minM || dd > cone.maxM) return false
        val b = bearingOf(p) ?: return false
        return dirs.any { angleDiff(b, it) <= cone.halfDeg + CONE_MARGIN_DEG }
    }

    // 7-point Gauss–Hermite quadrature (§2.4: the fix error integrated)
    private val GX = doubleArrayOf(-2.651961356835233, -1.673551628767471, -0.816287882858965, 0.0, 0.816287882858965, 1.673551628767471, 2.651961356835233)
    private val GW = doubleArrayOf(0.0009717812450995, 0.05451558281912703, 0.4256072526101278, 0.8102646175568073, 0.4256072526101278, 0.05451558281912703, 0.0009717812450995)

    private class Smp(val x: Double, val y: Double, val dt: Double, val before: Boolean, val in65: Boolean)

    /** Every pass of [cam] along one device's [route] (sorted here). */
    fun detect(route: List<RouteFix>, cam: PassCamera): List<Pass> {
        val fixes = route.sortedBy { it.timeMs }
        val n = fixes.size
        if (n == 0) return emptyList()
        val pj = Proj(cam.lat, cam.lon)
        val xy = fixes.map { pj.to(it.lat, it.lon) }
        val d = xy.map { hypot(it.x, it.y) }
        // segments near the camera → runs of linked fixes
        val runs = ArrayList<IntArray>()
        val covered = BooleanArray(n)
        for (k in 0 until n - 1) {
            if (!segValid(fixes[k], fixes[k + 1])) continue
            if (segClosest(xy[k], xy[k + 1]).first > RADIUS_M) continue
            if (runs.isNotEmpty() && runs.last()[1] == k) runs.last()[1] = k + 1 else runs += intArrayOf(k, k + 1)
            covered[k] = true; covered[k + 1] = true
        }
        for (i in 0 until n) if (!covered[i] && d[i] <= RADIUS_M) runs += intArrayOf(i, i)   // a lone fix (gaps on both sides)
        runs.sortBy { it[0] }
        val dirs = parseDirections(directionText(cam))
        val cone = coneFor(cam)
        val out = ArrayList<Pass>()
        for (r in runs) {
            val ra = r[0]; val rb = r[1]
            // the closest approach on the polyline
            var seg = -1; var bestT = 0.0; var best = 1e18
            if (ra == rb) best = d[ra]
            for (k in ra until rb) { val (dk, t) = segClosest(xy[k], xy[k + 1]); if (dk < best) { best = dk; seg = k; bestT = t } }
            val fa = fixes[if (seg < 0) ra else seg]; val fb = fixes[if (seg < 0) ra else seg + 1]
            val t = if (seg < 0) 0.0 else bestT
            val ms = fa.timeMs + Math.round(t * (fb.timeMs - fa.timeMs).toDouble())
            val lat = fa.lat + t * (fb.lat - fa.lat); val lon = fa.lon + t * (fb.lon - fa.lon)
            val acc = if (fa.acc != null && fb.acc != null) fa.acc + t * (fb.acc - fa.acc) else fa.acc ?: fb.acc
            val pc = if (seg < 0) xy[ra] else Xy(xy[seg].x + t * (xy[seg + 1].x - xy[seg].x), xy[seg].y + t * (xy[seg + 1].y - xy[seg].y))
            var vx = 0.0; var vy = 0.0; var speed: Double? = null; var heading: Double? = null
            if (seg >= 0) {
                val len = distanceM(fa.lat, fa.lon, fb.lat, fb.lon); val dt = (fb.timeMs - fa.timeMs) / 1000.0
                val at = if (t < 0.5) fa else fb
                if ((t == 0.0 || t == 1.0) && at.speedMps != null && at.speedMps >= 0) speed = at.speedMps * 3.6
                else if (dt > 0) speed = len / dt * 3.6
                if (len >= 3.0) heading = bearingDeg(fa.lat, fa.lon, fb.lat, fb.lon)
                if (dt > 0) { vx = (xy[seg + 1].x - xy[seg].x) / dt; vy = (xy[seg + 1].y - xy[seg].y) / dt }
            } else if (fa.speedMps != null && fa.speedMps >= 0) speed = fa.speedMps * 3.6
            val approach = bearingOf(pc) ?: 0.0
            // the run: fixes inside, time inside, entry / exit
            var fixCount = 0
            val devs = ArrayList<String>(); val srcs = ArrayList<String>()
            for (i in ra..rb) {
                if (d[i] <= RADIUS_M) fixCount++
                if (fixes[i].device !in devs) devs += fixes[i].device
                if (fixes[i].source.isNotEmpty() && fixes[i].source !in srcs) srcs += fixes[i].source
            }
            var dwell = 0.0
            for (k in ra until rb) segInside(xy[k], xy[k + 1], RADIUS_M)?.let { (t0, t1) -> dwell += (t1 - t0) * (fixes[k + 1].timeMs - fixes[k].timeMs) / 1000.0 }
            // §2.2: camera → us 10 s before and after the closest approach
            val frontBearing = bearingOf(positionAt(fixes, xy, ms - SIDE_MS, pc, ms, vx, vy)) ?: approach
            val rearBearing = bearingOf(positionAt(fixes, xy, ms + SIDE_MS, pc, ms, vx, vy)) ?: approach
            // §2.3: the run's polyline sampled every ~0.2 s; inside the cone and the range before / after the closest approach
            val sm = ArrayList<Smp>()
            if (ra == rb) sm += Smp(xy[ra].x, xy[ra].y, 0.0, true, d[ra] <= RADIUS_M)
            for (k in ra until rb) {
                val tt = (fixes[k + 1].timeMs - fixes[k].timeMs) / 1000.0
                val m = ceil(tt / 0.2).toInt().coerceIn(1, 400)
                for (i2 in 0 until m) {
                    val u = (i2 + 0.5) / m
                    val x = xy[k].x + u * (xy[k + 1].x - xy[k].x); val y = xy[k].y + u * (xy[k + 1].y - xy[k].y)
                    val tms = fixes[k].timeMs + u * (fixes[k + 1].timeMs - fixes[k].timeMs)
                    sm += Smp(x, y, tt / m, tms < ms.toDouble(), hypot(x, y) <= RADIUS_M)
                }
            }
            val half = cone.halfDeg + CONE_MARGIN_DEG
            fun inRange(x: Double, y: Double): Boolean { val dd = hypot(x, y); return dd >= cone.minM && dd <= cone.maxM }
            fun inConeDir(x: Double, y: Double, dir: Double): Boolean {
                if (!inRange(x, y)) return false
                val b = bearingOf(Xy(x, y)) ?: return false
                return angleDiff(b, dir) <= half
            }
            fun inCone(x: Double, y: Double): Boolean = dirs.any { inConeDir(x, y, it) }
            var inConeS = 0.0; var front = false; var rear = false; var facing: Boolean? = null; var camDir: Double? = null
            val pIn: Double
            if (dirs.isNotEmpty()) {
                for (q in sm) {
                    if (!inCone(q.x, q.y)) continue
                    inConeS += q.dt
                    if (ra == rb) { front = true; rear = true } else if (q.before) front = true else rear = true
                }
                facing = front || rear
                camDir = dirs.firstOrNull { dir -> sm.any { inConeDir(it.x, it.y, dir) } } ?: dirs.first()
                // the whole track shifted by a Gaussian offset (fix errors are correlated over a pass), 7×7 Gauss–Hermite
                val sigma = maxOf(1.0, (if (acc != null && acc > 0) acc else 8.0) / 1.515)   // accuracy = the 68 % radius; per-axis σ
                var p = 0.0
                for (ix in 0 until 7) for (iy in 0 until 7) {
                    val ox = sqrt(2.0) * sigma * GX[ix]; val oy = sqrt(2.0) * sigma * GX[iy]
                    if (sm.any { inCone(it.x + ox, it.y + oy) }) p += GW[ix] * GW[iy] / Math.PI
                }
                pIn = p.coerceIn(0.0, 1.0)
            } else {
                // no known direction: the fraction of the run (inside 65 m) within the range, × 0.5
                var within = 0.0; var total = 0.0
                for (q in sm) { if (!q.in65) continue; total += q.dt; if (inRange(q.x, q.y)) within += q.dt }
                val frac = if (total > 0) within / total else if (inRange(xy[ra].x, xy[ra].y)) 1.0 else 0.0
                inConeS = within
                pIn = frac * 0.5
            }
            val pRead = pIn * CAPTURE * READ
            val confAlpr = confidenceFor(pRead, cam, true)
            val type = cam.type.ifEmpty { "alpr" }
            out += Pass(
                camera = cam, closestMs = ms, lat = lat, lon = lon, acc = acc, distanceM = best, speedKmh = speed, headingDeg = heading,
                approachBearingDeg = approach, cameraDirDeg = camDir, facing = facing, frontVisible = front, rearVisible = rear,
                frontBearingDeg = frontBearing, rearBearingDeg = rearBearing, directions = dirs, dwellS = dwell, fixes = fixCount,
                fixDevices = devs, fixSources = srcs, enterDistanceM = d[ra], exitDistanceM = d[rb], startMs = fixes[ra].timeMs, endMs = fixes[rb].timeMs,
                cone = cone, inConeS = inConeS, pInCone = pIn, pRead = pRead,
                confidence = if (type == "alpr") confAlpr else confidenceFor(pRead, cam, false), confidenceAlpr = confAlpr,
            )
        }
        return out
    }
}

/**
 * Live detection (phone live): fixes in, finished passes out — the desktop's `PlateEvents::LiveTracker`. A pass is
 * finished when we have left the 65 m circle, or 2 min after its closest approach; a camera does not pass again while
 * we are still inside its circle or within 10 min of its last pass.
 */
class LiveTracker {
    class Active(val cam: PassCamera, val enteredMs: Long, var bestMs: Long, var bestD: Double, var left: Boolean = false)
    private class Done(val ms: Long, var inside: Boolean)

    private val buf = ArrayList<RouteFix>()
    private val active = ArrayList<Active>()
    private val done = HashMap<String, Done>()

    fun activeCameras(): List<Active> = active.toList()
    private fun camera(id: String): Active? = active.firstOrNull { it.cam.id == id }

    private fun trim(nowMs: Long) {
        var keepFrom = nowMs - KEEP_MS
        for (a in active) keepFrom = minOf(keepFrom, a.enteredMs - PassDetector.GAP_MS)
        while (buf.size > 2 && buf.first().timeMs < keepFrom) buf.removeAt(0)
    }

    private fun finish(id: String): List<Pass> {
        val i = active.indexOfFirst { it.cam.id == id }
        if (i < 0) return emptyList()
        val a = active.removeAt(i)
        val best = PassDetector.detect(buf, a.cam).minByOrNull { abs(it.closestMs - a.bestMs) }
        done[id] = Done(best?.closestMs ?: a.bestMs, !a.left)
        return listOfNotNull(best)
    }

    /** A new own fix and the cameras within a few hundred metres of it; returns the passes that finished. */
    fun addFix(f: RouteFix, nearby: List<PassCamera>): List<Pass> {
        val out = ArrayList<Pass>()
        val last = buf.lastOrNull()
        if (last != null && f.timeMs < last.timeMs) return out           // out of order: the desktop's backfill will see it
        if (last != null && f.timeMs == last.timeMs && f.lat == last.lat && f.lon == last.lon) return out
        buf += f
        val prev = if (buf.size > 1) buf[buf.size - 2] else null
        val linked = prev != null && PassDetector.segValid(prev, f)
        fun nearNow(c: PassCamera): Pair<Double, Long> {
            var dd = PassDetector.distanceM(c.lat, c.lon, f.lat, f.lon); var whenMs = f.timeMs
            if (linked && prev != null) {
                val (ds, t) = PassDetector.closestOnSegment(c, prev, f)
                if (ds < dd) { dd = ds; whenMs = prev.timeMs + Math.round(t * (f.timeMs - prev.timeMs).toDouble()) }
            }
            return dd to whenMs
        }
        // the passes in progress
        val toFinish = ArrayList<String>()
        for (a in active) {
            val (dd, whenMs) = nearNow(a.cam)
            if (dd < a.bestD) { a.bestD = dd; a.bestMs = whenMs }
            if (PassDetector.distanceM(f.lat, f.lon, a.cam.lat, a.cam.lon) > PassDetector.RADIUS_M) { a.left = true; toFinish += a.cam.id }
            else if (f.timeMs - a.bestMs >= FINISH_AFTER_MS) toFinish += a.cam.id
        }
        val it = done.entries.iterator()
        while (it.hasNext()) {                                          // left the circle: a later pass may start
            val e = it.next()
            val c = nearby.firstOrNull { n -> n.id == e.key }
            if (c == null || PassDetector.distanceM(f.lat, f.lon, c.lat, c.lon) > PassDetector.RADIUS_M) e.value.inside = false
            if (!e.value.inside && f.timeMs - e.value.ms > PassDetector.MERGE_WINDOW_MS) it.remove()
        }
        // new ones
        for (c in nearby) {
            if (camera(c.id) != null || c.id in toFinish) continue
            val dn = done[c.id]
            if (dn != null && (dn.inside || f.timeMs - dn.ms <= PassDetector.MERGE_WINDOW_MS)) continue
            val (dd, whenMs) = nearNow(c)
            if (dd > PassDetector.RADIUS_M) continue
            val a = Active(c, if (linked && prev != null) prev.timeMs else f.timeMs, whenMs, dd)
            active += a
            if (PassDetector.distanceM(f.lat, f.lon, c.lat, c.lon) > PassDetector.RADIUS_M) { a.left = true; toFinish += c.id }   // crossed between two fixes
        }
        for (id in toFinish) out += finish(id)
        trim(f.timeMs)
        return out
    }

    /** Time-based finalisation (no new fix). */
    fun tick(nowMs: Long): List<Pass> = active.filter { nowMs - it.bestMs >= FINISH_AFTER_MS }.map { it.cam.id }.flatMap { finish(it) }

    companion object {
        const val FINISH_AFTER_MS = 120_000L
        const val KEEP_MS = 10 * 60_000L
    }
}
