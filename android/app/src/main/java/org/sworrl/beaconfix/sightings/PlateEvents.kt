// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import org.sworrl.beaconfix.data.api.FlockCameraDto
import org.sworrl.beaconfix.data.api.PlateEventDto
import org.sworrl.beaconfix.data.db.PlateEventEntity
import java.time.Instant
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlin.math.roundToInt

/** Plate-event records: pass → row, wire ↔ row, and the §1.1 merge. Pure; unit-tested in PlateEventsTest. */
object PlateEvents {
    const val CAMERA_PASS = "camera_pass"
    const val PLATE_SEARCH = "plate_search"
    const val SRC_PHONE_LIVE = "phone_live"
    const val SRC_DASHCAM = "dashcam"
    const val SRC_BACKFILL = "route_backfill"
    const val SRC_LIVE = "live_route"
    const val SRC_HIBF = "haveibeenflocked"

    private val json = Json { ignoreUnknownKeys = true; isLenient = true }
    private val iso: DateTimeFormatter = DateTimeFormatter.ISO_LOCAL_DATE_TIME

    fun localIso(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String = LocalDateTime.ofInstant(Instant.ofEpochMilli(ms), zone).withNano(0).format(iso)

    /** The desktop's local ISO ("2026-10-02T12:00:00", maybe with an offset or a fraction) → epoch ms; null when unreadable. */
    fun parseLocal(s: String?, zone: ZoneId = ZoneId.systemDefault()): Long? {
        if (s.isNullOrBlank()) return null
        val t = s.trim().replace(' ', 'T')
        runCatching { return java.time.OffsetDateTime.parse(t).toInstant().toEpochMilli() }
        runCatching { return Instant.parse(t).toEpochMilli() }
        runCatching { return LocalDateTime.parse(t.take(19)).atZone(zone).toInstant().toEpochMilli() }
        return null
    }

    /** A camera of `/api/v1/flock` for the detector: the desktop's camera_type when it sends one of §2.0's, else classified here. */
    fun camera(c: FlockCameraDto): PassCamera {
        val tags = c.tags ?: JsonObject(emptyMap())
        val type = c.cameraType?.lowercase()?.takeIf { it in PassDetector.TYPES } ?: PassDetector.classifyCamera(c.model, c.source, c.id, tags)
        val webcam = c.webcam?.takeIf { it.startsWith("http://") || it.startsWith("https://") } ?: PassDetector.webcamUrl(tags).ifEmpty { null }
        // the state (§4.4 matching): addr:state, else ", TX" at the end of the notes (as the desktop does)
        val st = (tags["addr:state"] as? JsonPrimitive)?.contentOrNull ?: Hibf.stateFromText(c.notes)
        val withState = if (st.isNullOrEmpty() || tags.containsKey("addr:state")) tags else JsonObject(tags + ("_state" to JsonPrimitive(st)))
        return PassCamera(c.id, c.lat, c.lon, c.model, c.operatorName, c.source, c.direction, c.detectionMethod, c.notes, type, webcam, withState, c.confidence, c.manufacturer, c.trust)
    }

    /** The camera's two-letter state, when known (§4.4). */
    fun cameraState(cam: PassCamera): String? {
        val s = (cam.tags["addr:state"] as? JsonPrimitive)?.contentOrNull ?: (cam.tags["_state"] as? JsonPrimitive)?.contentOrNull
        return s?.takeIf { it.length == 2 }?.uppercase()
    }

    private fun Double.r(n: Int): Double { val f = Math.pow(10.0, n.toDouble()); return Math.round(this * f) / f }

    /** §2.2 / §2.3 / §2.4's `metrics` object of a pass (the desktop's passEvent keys). */
    fun metricsOf(p: Pass, plateInferred: Boolean, leakyMatch: String? = null, extra: Map<String, JsonElement> = emptyMap(), zone: ZoneId = ZoneId.systemDefault()): JsonObject = buildJsonObject {
        val dirKnown = p.directions.isNotEmpty()
        put("dwellS", p.dwellS.r(1)); put("fixes", p.fixes)
        putJsonArray("fixDevices") { p.fixDevices.forEach { add(JsonPrimitive(it)) } }
        putJsonArray("fixSources") { p.fixSources.forEach { add(JsonPrimitive(it)) } }
        put("enterDistanceM", p.enterDistanceM.r(1)); put("exitDistanceM", p.exitDistanceM.r(1))
        put("frontBearingDeg", p.frontBearingDeg.r(1)); put("rearBearingDeg", p.rearBearingDeg.r(1))
        put("frontVisible", if (dirKnown) JsonPrimitive(p.frontVisible) else JsonNull)
        put("rearVisible", if (dirKnown) JsonPrimitive(p.rearVisible) else JsonNull)
        putJsonArray("cameraDirections") { p.directions.forEach { add(JsonPrimitive(it)) } }
        put("plateInferred", plateInferred)
        put("cameraSource", p.camera.source)
        put("confidenceAlpr", p.confidenceAlpr)
        put("suspectedOnly", PassDetector.suspectedOnly(p.camera.source, p.camera.model))
        put("startTime", localIso(p.startMs, zone)); put("endTime", localIso(p.endMs, zone))
        put("coneHalfDeg", p.cone.halfDeg); put("coneMarginDeg", PassDetector.CONE_MARGIN_DEG)
        putJsonArray("rangeM") { add(JsonPrimitive(p.cone.minM)); add(JsonPrimitive(p.cone.maxM)) }
        put("coneModel", p.cone.name)
        put("inConeS", p.inConeS.r(1)); put("pInCone", p.pInCone.r(3)); put("pRead", p.pRead.r(3))
        put("sourceConfidence", p.camera.sourceConfidence)
        if (leakyMatch != null) put("leakyMatch", leakyMatch)
        PassDetector.classifyNote(p.camera.tags).takeIf { it.isNotEmpty() }?.let { put("typeNote", it) }
        for ((k, v) in extra) put(k, v)
    }

    /** The one honest sentence of a pass (§2.0, §6) — the desktop's passDetails. */
    fun detailsOf(cameraType: String, operator: String, model: String, distanceM: Double, facing: Boolean?): String {
        val d = Math.round(distanceM).toString()
        val op = if (operator.isEmpty()) "" else " ($operator)"
        return when (cameraType) {
            "webcam" -> "Passed a public traffic webcam$op $d m away — it does not read plates."
            "ptz" -> "Passed a Flock PTZ video camera$op $d m away — a video camera, not a plate reader."
            "enforcement" -> "Passed a speed / red-light enforcement camera$op $d m away — it photographs only vehicles it catches."
            "not_camera" -> "Passed a sensor that is not a camera$op $d m away."
            "alpr", "" -> {
                val who = listOf(operator, model).joinToString(" ").trim().replace(Regex("\\s+"), " ")
                val face = when (facing) { true -> "camera faced you"; false -> "camera faced away"; null -> "facing unknown" }
                "Passed an ALPR camera${if (who.isEmpty()) "" else " ($who)"} $d m away: your plate was likely read ($face)."
            }
            else -> "Passed a surveillance camera$op $d m away — it does not read plates."
        }
    }
    fun detailsOf(p: Pass): String = detailsOf(p.cameraType, p.camera.operator, p.camera.model, p.distanceM, p.facing)

    /** A camera type as words. */
    fun typeLabel(type: String?): String = when (type) {
        "alpr", null, "" -> "ALPR"
        "webcam" -> "public traffic webcam"
        "ptz" -> "Flock PTZ video camera"
        "enforcement" -> "speed / red-light camera"
        "not_camera" -> "not a camera"
        else -> "surveillance camera"
    }

    /** "View source" of a camera (the desktop's cameraSourceLink): its OSM object, a detection's spot, else the DeFlock map. */
    fun sourceLink(cam: PassCamera): Pair<String, String> {
        osmUrl(cam.id)?.let { return it to "OpenStreetMap" }
        val ll = "%.6f".format(java.util.Locale.US, cam.lat) to "%.6f".format(java.util.Locale.US, cam.lon)
        if (cam.id.startsWith("det:")) return "https://www.openstreetmap.org/?mlat=${ll.first}&mlon=${ll.second}#map=19/${ll.first}/${ll.second}" to "BeaconFix detection (${cam.source})"
        return "https://deflock.me/map#map=19/${ll.first}/${ll.second}" to (if (cam.source.isEmpty()) "DeFlock camera map" else "DeFlock / flocklocations · ${cam.source}")
    }

    /** A live pass of this phone (§2.1) as a `plate_events` row, marked for pushing (the desktop's passEvent). */
    fun fromPass(p: Pass, source: String, plate: String, plateInferred: Boolean, device: String, leaky: Boolean, leakyMatch: String?, now: Long,
                 zone: ZoneId = ZoneId.systemDefault(), extraMetrics: Map<String, JsonElement> = emptyMap()): PlateEventEntity {
        val cam = p.camera
        val (url, name) = sourceLink(cam)
        val cleanTags = JsonObject(cam.tags - "_state")
        val raw = buildJsonObject {
            put("camera", buildJsonObject {
                put("id", cam.id); put("lat", cam.lat); put("lon", cam.lon); put("source", cam.source); put("model", cam.model); put("operator", cam.operator)
                put("direction", PassDetector.directionText(cam)); put("type", cam.type)
                cam.webcam?.let { put("webcam", it) }
            })
            if (cleanTags.isNotEmpty()) put("osmTags", cleanTags)
        }
        return PlateEventEntity(
            uid = PassDetector.passUid(cam.id, p.closestMs), kind = CAMERA_PASS, plate = plate, time = localIso(p.closestMs, zone), timeMs = p.closestMs,
            lat = p.lat, lon = p.lon, acc = p.acc?.r(1), cameraId = cam.id, cameraLat = cam.lat, cameraLon = cam.lon,
            distanceM = p.distanceM.r(1), speedKmh = p.speedKmh?.r(1), headingDeg = p.headingDeg?.r(1), approachBearingDeg = p.approachBearingDeg.r(1),
            cameraDirDeg = p.cameraDirDeg?.r(1), facing = p.facing?.let { if (it) 1 else 0 },
            operator = cam.operator, agency = null, model = cam.model, cameraType = p.cameraType, source = source, sourceUrl = url, sourceName = name,
            confidence = p.confidence, leaky = if (leaky) 1 else 0, details = detailsOf(p),
            metrics = metricsOf(p, plateInferred, leakyMatch, extraMetrics, zone).toString(), raw = raw.toString(), device = device,
            createdAt = localIso(now, zone), updatedAt = localIso(now, zone), dirty = true, hubDirty = true,
        )
    }

    /** "View source" of a camera: its OSM object when it is one. */
    fun osmUrl(cameraId: String): String? {
        val m = Regex("""^osm:(node|way|relation)/(\d+)$""").matchEntire(cameraId) ?: return null
        return "https://www.openstreetmap.org/${m.groupValues[1]}/${m.groupValues[2]}"
    }

    // ── wire ↔ row ──
    fun boolish(e: JsonElement?): Int? {
        val p = e as? JsonPrimitive ?: return null
        if (p is JsonNull) return null
        p.booleanOrNull?.let { return if (it) 1 else 0 }
        return p.doubleOrNull?.let { if (it != 0.0) 1 else 0 }
    }

    /** An object / array stays as its JSON text; a string holding JSON is taken as is; null and JsonNull → null. */
    fun jsonText(e: JsonElement?): String? = when (e) {
        null, JsonNull -> null
        is JsonPrimitive -> if (e.isString) e.content.ifEmpty { null } else e.toString()
        else -> e.toString()
    }

    private fun jsonOrNull(s: String?): JsonElement? = s?.let { runCatching { json.parseToJsonElement(it) }.getOrElse { _ -> JsonPrimitive(s) } }

    fun fromDto(d: PlateEventDto, zone: ZoneId = ZoneId.systemDefault()): PlateEventEntity = PlateEventEntity(
        uid = d.uid, kind = d.kind.ifEmpty { CAMERA_PASS }, plate = d.plate, time = d.time, timeMs = parseLocal(d.time, zone) ?: 0L,
        lat = d.lat, lon = d.lon, acc = d.acc, cameraId = d.cameraId, cameraLat = d.cameraLat, cameraLon = d.cameraLon, distanceM = d.distanceM,
        speedKmh = d.speedKmh, headingDeg = d.headingDeg, approachBearingDeg = d.approachBearingDeg, cameraDirDeg = d.cameraDirDeg, facing = boolish(d.facing),
        operator = d.operator, agency = d.agency, model = d.model, cameraType = d.cameraType, source = d.source.ifEmpty { "desktop" },
        sourceUrl = d.sourceUrl, sourceName = d.sourceName, confidence = d.confidence?.roundToInt(), leaky = boolish(d.leaky) ?: 0, details = d.details,
        metrics = jsonText(d.metrics), raw = jsonText(d.raw), device = d.device, createdAt = d.createdAt, updatedAt = d.updatedAt, seq = d.seq ?: 0,
    )

    fun toDto(e: PlateEventEntity): PlateEventDto = PlateEventDto(
        uid = e.uid, kind = e.kind, plate = e.plate, time = e.time, lat = e.lat, lon = e.lon, acc = e.acc, cameraId = e.cameraId, cameraLat = e.cameraLat,
        cameraLon = e.cameraLon, distanceM = e.distanceM, speedKmh = e.speedKmh, headingDeg = e.headingDeg, approachBearingDeg = e.approachBearingDeg,
        cameraDirDeg = e.cameraDirDeg, facing = e.facing?.let { JsonPrimitive(it) }, operator = e.operator, agency = e.agency, model = e.model,
        cameraType = e.cameraType, source = e.source, sourceUrl = e.sourceUrl, sourceName = e.sourceName, confidence = e.confidence?.toDouble(),
        leaky = JsonPrimitive(e.leaky), details = e.details, metrics = jsonOrNull(e.metrics), raw = jsonOrNull(e.raw), device = e.device,
        createdAt = e.createdAt, updatedAt = e.updatedAt,
    )

    // ── §1.1 merge ──
    /** Live sources outrank backfill: the phone saw it live (dash cam above phone live above the desktop's live route). */
    fun sourceRank(source: String?): Int = when (source) { SRC_DASHCAM -> 4; SRC_PHONE_LIVE -> 3; SRC_LIVE -> 2; SRC_BACKFILL -> 1; else -> 0 }

    /**
     * [incoming] is the same event as [existing] (same uid, or a pass of the same camera within ±10 min): the result keeps
     * the existing uid, takes the incoming measurement when it ranks higher (a live source over backfill, else the
     * higher confidence), and fills every remaining null from the other side. Local bookkeeping (dirty, hubDirty, notified) and
     * the desktop's seq are the caller's.
     */
    fun merge(existing: PlateEventEntity, incoming: PlateEventEntity): PlateEventEntity {
        val ri = sourceRank(incoming.source); val re = sourceRank(existing.source)
        val takeIncoming = ri > re || (ri == re && (incoming.confidence ?: -1) > (existing.confidence ?: -1))
        val w = if (takeIncoming) incoming else existing      // the measurement that wins
        val o = if (takeIncoming) existing else incoming      // fills its gaps
        return existing.copy(
            kind = w.kind, plate = w.plate?.ifEmpty { null } ?: o.plate, time = w.time, timeMs = w.timeMs,
            lat = w.lat ?: o.lat, lon = w.lon ?: o.lon, acc = w.acc ?: o.acc, cameraId = w.cameraId ?: o.cameraId,
            cameraLat = w.cameraLat ?: o.cameraLat, cameraLon = w.cameraLon ?: o.cameraLon, distanceM = w.distanceM ?: o.distanceM,
            speedKmh = w.speedKmh ?: o.speedKmh, headingDeg = w.headingDeg ?: o.headingDeg, approachBearingDeg = w.approachBearingDeg ?: o.approachBearingDeg,
            cameraDirDeg = w.cameraDirDeg ?: o.cameraDirDeg, facing = w.facing ?: o.facing, operator = w.operator ?: o.operator, agency = w.agency ?: o.agency,
            model = w.model ?: o.model, cameraType = w.cameraType ?: o.cameraType, source = w.source, sourceUrl = w.sourceUrl ?: o.sourceUrl,
            sourceName = w.sourceName ?: o.sourceName, confidence = w.confidence ?: o.confidence, leaky = maxOf(w.leaky, o.leaky), details = w.details ?: o.details,
            metrics = w.metrics ?: o.metrics, raw = w.raw ?: o.raw, device = w.device ?: o.device, createdAt = existing.createdAt ?: incoming.createdAt,
            updatedAt = maxOf(existing.updatedAt.orEmpty(), incoming.updatedAt.orEmpty()).ifEmpty { null },
        )
    }

    /** Whether the merged row says anything [old] did not (so a re-push is worth it). */
    fun changed(old: PlateEventEntity, new: PlateEventEntity): Boolean =
        old.copy(dirty = false, hubDirty = false, notified = false, seq = 0, updatedAt = null) != new.copy(dirty = false, hubDirty = false, notified = false, seq = 0, updatedAt = null)

    // ── metrics helpers for the screens ──
    fun metricsObject(s: String?): JsonObject? = s?.let { runCatching { json.parseToJsonElement(it) as? JsonObject }.getOrNull() }

    /** A metrics value as display text. */
    fun show(e: JsonElement): String = when (e) {
        JsonNull -> "—"
        is JsonPrimitive -> e.content
        is JsonArray -> e.joinToString(", ") { show(it) }
        is JsonObject -> e.entries.joinToString(", ") { "${it.key}: ${show(it.value)}" }
    }
}
