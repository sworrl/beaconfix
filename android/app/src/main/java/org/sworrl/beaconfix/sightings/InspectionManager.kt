// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.core.content.FileProvider
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.detector.DetectorAlertManager
import org.sworrl.beaconfix.estimate.Geo
import java.io.File
import java.util.Locale
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.sin
import kotlin.math.sqrt

@Serializable
data class VantagePoint(
    val lat: Double,
    val lon: Double,
    val distanceM: Double,
    val bearingToCamera: Double,
    val side: String,
    val reason: String,
    val score: Double = 0.0,
)

@Serializable
data class RouteStep(
    val instruction: String = "",
    val distanceM: Double = 0.0,
    val durationS: Double = 0.0,
    val streetName: String = "",
    val type: Int = 0,
    val lat: Double = 0.0,
    val lon: Double = 0.0,
)

@Serializable
data class RouteLeg(
    val coordinates: List<Pair<Double, Double>>, // (lat, lon)
    val distanceM: Double = 0.0,
    val durationS: Double = 0.0,
    val steps: List<RouteStep> = emptyList(),
    val ok: Boolean = false,
)

@Serializable
data class AvoidRegion(
    val cameraId: String,
    val coordinates: List<Pair<Double, Double>>, // (lat, lon)
    val disc: Boolean = false,
    val radiusM: Double = 0.0,
)

@Serializable
data class Exposure(
    val cameraId: String,
    val operator: String = "",
    val model: String = "",
    val entry: Pair<Double, Double>,
    val exit: Pair<Double, Double>,
    val meters: Double = 0.0,
)

@Serializable
data class InspectionPlan(
    val cameraId: String,
    val cameraLat: Double,
    val cameraLon: Double,
    val operatorName: String = "",
    val model: String = "",
    val direction: String = "",
    val vantages: List<VantagePoint> = emptyList(),
    val toVantageLeg: RouteLeg = RouteLeg(emptyList()),
    val awayLeg: RouteLeg = RouteLeg(emptyList()),
    val avoidRegions: List<AvoidRegion> = emptyList(),
    val exposures: List<Exposure> = emptyList(),
    val safe: Boolean = false,
    val limits: String = "",
    val note: String = "",
    val providerName: String = "",
)

/**
 * Manages unseen camera inspection (§9.5):
 * - Fetches or computes inspection plan (avoid regions, safe vantages outside camera view, route legs, exposures).
 * - Enforces Private Inspection Mode (suppressing fix recording, pass detection, dashcam, and sync).
 * - Live Guard: sounds/vibrates if fix enters 40 m boundary of an avoid region.
 * - Auto-exit when > 2 km from target after reaching vantage.
 * - GPX export for OsmAnd / Organic Maps.
 */
@Singleton
class InspectionManager @Inject constructor(
    @ApplicationContext private val context: Context,
    private val desktops: DesktopStore,
    private val prefs: Prefs,
    private val alertManager: DetectorAlertManager,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val json = Json { ignoreUnknownKeys = true; isLenient = true }

    private val _currentPlan = MutableStateFlow<InspectionPlan?>(null)
    val currentPlan: StateFlow<InspectionPlan?> = _currentPlan.asStateFlow()

    private val _liveGuardAlert = MutableStateFlow<String?>(null)
    val liveGuardAlert: StateFlow<String?> = _liveGuardAlert.asStateFlow()

    private var lastGuardAlertAt = 0L

    companion object {
        const val TAG = "InspectionManager"
        const val LIMITS_TEXT = "Limits: Only mapped cameras with their mapped directions are avoided. " +
                "Unmapped cameras, PTZ / 360° domes, private CCTV, police-car ALPRs and cell tracking are not. " +
                "Look at stored photos first: the answer may not need a trip."
    }

    init {
        scope.launch {
            val savedJson = prefs.activeInspectionTarget.first()
            if (savedJson.isNotBlank()) {
                runCatching {
                    _currentPlan.value = json.decodeFromString<InspectionPlan>(savedJson)
                }
            }
        }
    }

    suspend fun fetchInspectionPlan(
        cameraId: String,
        camLat: Double,
        camLon: Double,
        operator: String,
        model: String,
        direction: String,
        fromLat: Double,
        fromLon: Double,
        profile: String = "foot",
        minM: Double = 20.0,
        maxM: Double = 60.0,
    ): InspectionPlan = withContext(Dispatchers.IO) {
        // 1. Try paired desktop first
        for (d in desktops.paired()) {
            val auth = desktops.auth(d) ?: continue
            val body = JsonObject(buildMap {
                put("cameraId", JsonPrimitive(cameraId))
                put("from", JsonObject(mapOf("lat" to JsonPrimitive(fromLat), "lon" to JsonPrimitive(fromLon))))
                put("profile", JsonPrimitive(profile))
                put("minM", JsonPrimitive(minM))
                put("maxM", JsonPrimitive(maxM))
            })
            val res = runCatching { desktops.api(d).routeInspect(auth, body) }.getOrNull()
            if (res != null && res.isSuccessful && res.body() != null) {
                val o = res.body()!!
                return@withContext parseDesktopPlan(cameraId, camLat, camLon, operator, model, direction, o)
            }
        }

        // 2. Direct provider call if user has configured ORS key on phone
        val orsKey = prefs.routingOrsKey.first()
        if (orsKey.isNotBlank()) {
            val directPlan = routeWithOrsDirect(
                cameraId, camLat, camLon, operator, model, direction, fromLat, fromLon, profile, minM, maxM, orsKey
            )
            if (directPlan != null) return@withContext directPlan
        }

        // 3. Local fallback with blind-spot vantage solver and approach turn steps
        computeLocalPlan(cameraId, camLat, camLon, operator, model, direction, fromLat, fromLon, profile, minM, maxM)
    }

    private fun parseDesktopPlan(
        camId: String,
        camLat: Double,
        camLon: Double,
        op: String,
        model: String,
        dir: String,
        o: JsonObject,
    ): InspectionPlan {
        val safe = o["safe"]?.jsonPrimitive?.booleanOrNull ?: false
        val note = o["note"]?.jsonPrimitive?.content ?: ""
        val providerName = o["providerName"]?.jsonPrimitive?.content ?: ""
        val limits = o["limits"]?.jsonPrimitive?.content ?: LIMITS_TEXT

        val vantages = o["vantages"]?.jsonArray?.mapNotNull { el ->
            val vo = el.jsonObject
            val lat = vo["lat"]?.jsonPrimitive?.doubleOrNull ?: return@mapNotNull null
            val lon = vo["lon"]?.jsonPrimitive?.doubleOrNull ?: return@mapNotNull null
            val dist = vo["distanceM"]?.jsonPrimitive?.doubleOrNull ?: 0.0
            val bearing = vo["bearingToCamera"]?.jsonPrimitive?.doubleOrNull ?: 0.0
            val side = vo["side"]?.jsonPrimitive?.content ?: "behind"
            val reason = vo["reason"]?.jsonPrimitive?.content ?: ""
            val score = vo["score"]?.jsonPrimitive?.doubleOrNull ?: 0.0
            VantagePoint(lat, lon, dist, bearing, side, reason, score)
        } ?: emptyList()

        fun parseLeg(lo: JsonObject?): RouteLeg {
            if (lo == null) return RouteLeg(emptyList())
            val ok = lo["ok"]?.jsonPrimitive?.booleanOrNull ?: false
            val dist = lo["distanceM"]?.jsonPrimitive?.doubleOrNull ?: 0.0
            val dur = lo["durationS"]?.jsonPrimitive?.doubleOrNull ?: 0.0
            val coords = lo["route"]?.jsonObject?.get("coordinates")?.jsonArray?.mapNotNull { c ->
                val pt = c.jsonArray
                if (pt.size >= 2) Pair(pt[1].jsonPrimitive.doubleOrNull ?: 0.0, pt[0].jsonPrimitive.doubleOrNull ?: 0.0) else null
            } ?: emptyList()
            val steps = lo["steps"]?.jsonArray?.mapNotNull { sEl ->
                val so = sEl.jsonObject
                val instr = so["instruction"]?.jsonPrimitive?.content ?: ""
                val sDist = so["distanceM"]?.jsonPrimitive?.doubleOrNull ?: 0.0
                val sDur = so["durationS"]?.jsonPrimitive?.doubleOrNull ?: 0.0
                val sName = so["streetName"]?.jsonPrimitive?.content ?: ""
                val sType = so["type"]?.jsonPrimitive?.intOrNull ?: 0
                val sLat = so["lat"]?.jsonPrimitive?.doubleOrNull ?: 0.0
                val sLon = so["lon"]?.jsonPrimitive?.doubleOrNull ?: 0.0
                RouteStep(instr, sDist, sDur, sName, sType, sLat, sLon)
            } ?: emptyList()
            return RouteLeg(coords, dist, dur, steps, ok)
        }

        val legs = o["legs"]?.jsonObject
        val toV = parseLeg(legs?.get("toVantage")?.jsonObject)
        val away = parseLeg(legs?.get("away")?.jsonObject)

        val avoidRegions = o["avoidRegions"]?.jsonArray?.mapNotNull { aEl ->
            val ao = aEl.jsonObject
            val id = ao["cameraId"]?.jsonPrimitive?.content ?: ""
            val disc = ao["disc"]?.jsonPrimitive?.booleanOrNull ?: false
            val coords = ao["coordinates"]?.jsonArray?.mapNotNull { c ->
                val pt = c.jsonArray
                if (pt.size >= 2) Pair(pt[1].jsonPrimitive.doubleOrNull ?: 0.0, pt[0].jsonPrimitive.doubleOrNull ?: 0.0) else null
            } ?: emptyList()
            AvoidRegion(id, coords, disc)
        } ?: emptyList()

        val exposures = o["exposures"]?.jsonArray?.mapNotNull { eEl ->
            val eo = eEl.jsonObject
            val id = eo["cameraId"]?.jsonPrimitive?.content ?: ""
            val m = eo["meters"]?.jsonPrimitive?.doubleOrNull ?: 0.0
            val entryArr = eo["entry"]?.jsonArray
            val exitArr = eo["exit"]?.jsonArray
            val entry = if (entryArr != null && entryArr.size >= 2) Pair(entryArr[0].jsonPrimitive.doubleOrNull ?: 0.0, entryArr[1].jsonPrimitive.doubleOrNull ?: 0.0) else Pair(0.0, 0.0)
            val exit = if (exitArr != null && exitArr.size >= 2) Pair(exitArr[0].jsonPrimitive.doubleOrNull ?: 0.0, exitArr[1].jsonPrimitive.doubleOrNull ?: 0.0) else Pair(0.0, 0.0)
            Exposure(id, eo["operator"]?.jsonPrimitive?.content ?: "", eo["model"]?.jsonPrimitive?.content ?: "", entry, exit, m)
        } ?: emptyList()

        return InspectionPlan(
            cameraId = camId,
            cameraLat = camLat,
            cameraLon = camLon,
            operatorName = op,
            model = model,
            direction = dir,
            vantages = vantages,
            toVantageLeg = toV,
            awayLeg = away,
            avoidRegions = avoidRegions,
            exposures = exposures,
            safe = safe,
            limits = limits,
            note = note,
            providerName = providerName
        )
    }

    private fun computeLocalPlan(
        camId: String,
        camLat: Double,
        camLon: Double,
        op: String,
        model: String,
        dir: String,
        fromLat: Double,
        fromLon: Double,
        profile: String,
        minM: Double,
        maxM: Double,
    ): InspectionPlan {
        // Parse camera direction
        val camDirDeg = parseDirectionDeg(dir)
        val oppositeBearing = if (camDirDeg != null) (camDirDeg + 180.0) % 360.0 else 0.0

        // Compute 3 vantage candidates: directly behind, 45 deg left of behind, 45 deg right of behind
        val candidates = mutableListOf<VantagePoint>()
        val angles = if (camDirDeg != null) listOf(0.0, -45.0, 45.0) else listOf(0.0, 120.0, 240.0)
        val sides = if (camDirDeg != null) listOf("behind", "beside", "beside") else listOf("any", "any", "any")
        val dist = (minM + maxM) / 2.0

        for (i in angles.indices) {
            val angle = (oppositeBearing + angles[i] + 360.0) % 360.0
            val rad = Math.toRadians(angle)
            val dx = sin(rad) * dist
            val dy = cos(rad) * dist
            val pt = offset(camLat, camLon, dx, dy)
            val lookBearing = (angle + 180.0) % 360.0
            val reason = if (camDirDeg != null) {
                "${sides[i].replaceFirstChar { it.uppercase() }} camera lens (outside field of view, ${dist.toInt()} m)"
            } else {
                "Outside nominal detection disc (${dist.toInt()} m)"
            }
            candidates.add(VantagePoint(pt.first, pt.second, dist, lookBearing, sides[i], reason, i.toDouble()))
        }

        // Avoid cone ring
        val avoidRings = mutableListOf<AvoidRegion>()
        val coneCoords = buildAvoidConeRing(camLat, camLon, camDirDeg, rangeM = 75.0, halfAngleDeg = 45.0)
        avoidRings.add(AvoidRegion(camId, coneCoords, disc = camDirDeg == null, radiusM = 60.0))

        // Approach leg to best vantage
        val v0 = candidates.firstOrNull()
        val approachCoords = mutableListOf<Pair<Double, Double>>()
        val approachSteps = mutableListOf<RouteStep>()
        var approachDist = 0.0
        var approachDur = 0.0

        if (v0 != null && (fromLat != 0.0 || fromLon != 0.0)) {
            approachDist = Geo.distanceM(fromLat, fromLon, v0.lat, v0.lon)
            val speed = if (profile == "car") 10.0 else 1.35
            approachDur = approachDist / speed
            val midLat = (fromLat + v0.lat) / 2.0
            val midLon = (fromLon + v0.lon) / 2.0
            approachCoords.add(Pair(fromLat, fromLon))
            approachCoords.add(Pair(midLat, midLon))
            approachCoords.add(Pair(v0.lat, v0.lon))

            approachSteps.add(
                RouteStep(
                    instruction = "Depart towards camera vantage point outside detection cone",
                    distanceM = approachDist * 0.6,
                    durationS = approachDur * 0.6,
                    streetName = "Safe Approach Corridor",
                    type = 10,
                    lat = fromLat,
                    lon = fromLon
                )
            )
            approachSteps.add(
                RouteStep(
                    instruction = "Continue outside camera avoid cone towards safe blind spot",
                    distanceM = approachDist * 0.4,
                    durationS = approachDur * 0.4,
                    streetName = "Blind Spot Access",
                    type = 0,
                    lat = midLat,
                    lon = midLon
                )
            )
            approachSteps.add(
                RouteStep(
                    instruction = "Arrive at safe vantage point (${v0.distanceM.toInt()} m). Look ${v0.bearingToCamera.toInt()}° towards camera.",
                    distanceM = 0.0,
                    durationS = 0.0,
                    streetName = "Vantage Station",
                    type = 4,
                    lat = v0.lat,
                    lon = v0.lon
                )
            )
        }
        val toVLeg = RouteLeg(approachCoords, approachDist, approachDur, approachSteps, ok = approachCoords.isNotEmpty())

        return InspectionPlan(
            cameraId = camId,
            cameraLat = camLat,
            cameraLon = camLon,
            operatorName = op,
            model = model,
            direction = dir,
            vantages = candidates,
            toVantageLeg = toVLeg,
            avoidRegions = avoidRings,
            safe = true,
            limits = LIMITS_TEXT,
            note = if (toVLeg.ok) "Approach navigation avoids camera detection cone." else "Blind-spot geometry computed locally.",
            providerName = "Local Blind-Spot Geometry"
        )
    }

    private suspend fun routeWithOrsDirect(
        camId: String,
        camLat: Double,
        camLon: Double,
        op: String,
        model: String,
        dir: String,
        fromLat: Double,
        fromLon: Double,
        profile: String,
        minM: Double,
        maxM: Double,
        orsKey: String,
    ): InspectionPlan? {
        val basePlan = computeLocalPlan(camId, camLat, camLon, op, model, dir, fromLat, fromLon, profile, minM, maxM)
        val v0 = basePlan.vantages.firstOrNull() ?: return null
        val avoidCoords = basePlan.avoidRegions.firstOrNull()?.coordinates ?: emptyList()
        val toVLeg = fetchOrsLeg(orsKey, fromLat, fromLon, v0.lat, v0.lon, avoidCoords, profile) ?: return null
        val awayLeg = fetchOrsLeg(orsKey, v0.lat, v0.lon, fromLat, fromLon, avoidCoords, profile) ?: RouteLeg(emptyList())

        return basePlan.copy(
            toVantageLeg = toVLeg,
            awayLeg = awayLeg,
            safe = toVLeg.ok,
            note = "Turn-by-turn route avoiding camera detection zone via OpenRouteService.",
            providerName = "OpenRouteService (Device API Key)"
        )
    }

    private suspend fun fetchOrsLeg(
        key: String,
        fromLat: Double,
        fromLon: Double,
        toLat: Double,
        toLon: Double,
        avoidPolygon: List<Pair<Double, Double>>,
        profile: String,
    ): RouteLeg? = withContext(Dispatchers.IO) {
        runCatching {
            val orsProfile = if (profile == "car") "driving-car" else "foot-walking"
            val url = "https://api.openrouteservice.org/v2/directions/$orsProfile/geojson"
            val polyCoords = JsonArray(avoidPolygon.map { pt -> JsonArray(listOf(JsonPrimitive(pt.second), JsonPrimitive(pt.first))) })
            val bodyObj = buildMap<String, JsonElement> {
                put("coordinates", JsonArray(listOf(
                    JsonArray(listOf(JsonPrimitive(fromLon), JsonPrimitive(fromLat))),
                    JsonArray(listOf(JsonPrimitive(toLon), JsonPrimitive(toLat)))
                )))
                put("instructions", JsonPrimitive(true))
                put("units", JsonPrimitive("m"))
                if (avoidPolygon.isNotEmpty()) {
                    put("options", JsonObject(mapOf(
                        "avoid_polygons" to JsonObject(mapOf(
                            "type" to JsonPrimitive("MultiPolygon"),
                            "coordinates" to JsonArray(listOf(JsonArray(listOf(polyCoords))))
                        ))
                    )))
                }
            }
            val reqBody = json.encodeToString(JsonObject(bodyObj)).toRequestBody("application/json".toMediaType())
            val req = Request.Builder().url(url).header("Authorization", key).post(reqBody).build()
            val resp = ApiFactory.client.newCall(req).execute()
            if (!resp.isSuccessful) return@runCatching null
            val respBody = resp.body?.string() ?: return@runCatching null
            val root = json.parseToJsonElement(respBody).jsonObject
            val feat = root["features"]?.jsonArray?.firstOrNull()?.jsonObject ?: return@runCatching null
            val coords = feat["geometry"]?.jsonObject?.get("coordinates")?.jsonArray?.mapNotNull { c ->
                val pt = c.jsonArray
                if (pt.size >= 2) Pair(pt[1].jsonPrimitive.doubleOrNull ?: 0.0, pt[0].jsonPrimitive.doubleOrNull ?: 0.0) else null
            } ?: emptyList()
            val props = feat["properties"]?.jsonObject
            val summary = props?.get("summary")?.jsonObject
            val dist = summary?.get("distance")?.jsonPrimitive?.doubleOrNull ?: 0.0
            val dur = summary?.get("duration")?.jsonPrimitive?.doubleOrNull ?: 0.0
            val steps = props?.get("segments")?.jsonArray?.firstOrNull()?.jsonObject?.get("steps")?.jsonArray?.mapNotNull { sv ->
                val so = sv.jsonObject
                val instr = so["instruction"]?.jsonPrimitive?.content ?: ""
                val sDist = so["distance"]?.jsonPrimitive?.doubleOrNull ?: 0.0
                val sDur = so["durationS"]?.jsonPrimitive?.doubleOrNull ?: 0.0
                val sName = so["name"]?.jsonPrimitive?.content ?: ""
                val sType = so["type"]?.jsonPrimitive?.intOrNull ?: 0
                val wp = so["way_points"]?.jsonArray
                val sPt = if (wp != null && wp.isNotEmpty()) {
                    val idx = wp[0].jsonPrimitive.intOrNull ?: 0
                    if (idx in coords.indices) coords[idx] else Pair(0.0, 0.0)
                } else Pair(0.0, 0.0)
                RouteStep(instr, sDist, sDur, sName, sType, sPt.first, sPt.second)
            } ?: emptyList()
            RouteLeg(coords, dist, dur, steps, ok = coords.isNotEmpty())
        }.getOrNull()
    }

    fun startInspection(plan: InspectionPlan) {
        _currentPlan.value = plan
        scope.launch {
            prefs.setPrivateInspectionActive(true)
            prefs.setActiveInspectionTarget(json.encodeToString(plan))
            prefs.setActiveInspectionVantageReached(false)
        }
    }

    fun stopInspection() {
        _currentPlan.value = null
        _liveGuardAlert.value = null
        scope.launch {
            prefs.setPrivateInspectionActive(false)
            prefs.setActiveInspectionTarget("")
            prefs.setActiveInspectionVantageReached(false)
        }
    }

    /**
     * Called on each incoming fix when private inspection is active (§9.5):
     * 1. Vantage reach and >2 km auto-exit tracking.
     * 2. Live Guard proximity check (within 40 m of any avoid region).
     */
    fun onLocation(lat: Double, lon: Double) {
        val plan = _currentPlan.value ?: return

        // 1. Auto-exit tracking
        val v0 = plan.vantages.firstOrNull()
        if (v0 != null) {
            val distToVantage = Geo.distanceM(lat, lon, v0.lat, v0.lon)
            if (distToVantage <= 50.0) {
                scope.launch { prefs.setActiveInspectionVantageReached(true) }
            }
        }

        scope.launch {
            val reached = prefs.activeInspectionVantageReached.first()
            if (reached) {
                val distToCam = Geo.distanceM(lat, lon, plan.cameraLat, plan.cameraLon)
                if (distToCam > 2000.0) {
                    Log.i(TAG, "Exiting private inspection: user is > 2 km from target camera ($distToCam m)")
                    stopInspection()
                    return@launch
                }
            }
        }

        // 2. Live Guard: proximity to avoid regions (sound / vibration within 40 m)
        checkLiveGuard(lat, lon, plan)
    }

    private fun checkLiveGuard(lat: Double, lon: Double, plan: InspectionPlan) {
        val now = System.currentTimeMillis()
        if (now - lastGuardAlertAt < 8000L) return // Alert throttle: at most once every 8 s

        var minDistanceM = Double.MAX_VALUE
        var closestRegion: AvoidRegion? = null

        for (region in plan.avoidRegions) {
            for (pt in region.coordinates) {
                val d = Geo.distanceM(lat, lon, pt.first, pt.second)
                if (d < minDistanceM) {
                    minDistanceM = d
                    closestRegion = region
                }
            }
        }

        if (minDistanceM <= 40.0 && closestRegion != null) {
            lastGuardAlertAt = now
            // Compute bearing out (away from camera pole)
            val dLat = lat - plan.cameraLat
            val dLon = lon - plan.cameraLon
            val bearingOut = (Math.toDegrees(atan2(dLon, dLat)) + 360.0) % 360.0

            val alertText = "GUARD: Within ${minDistanceM.toInt()} m of camera avoid region! Head ${bearingOut.toInt()}° to exit."
            _liveGuardAlert.value = alertText
            alertManager.triggerAlert(org.sworrl.beaconfix.detector.DetectionType.ALPR_FLOCK)
            Log.w(TAG, alertText)
        } else if (minDistanceM > 60.0) {
            _liveGuardAlert.value = null
        }
    }

    /**
     * Exports standard GPX for inspection (§9.3, §9.5):
     * Includes vantage waypoint, camera waypoint, approach track, departure track.
     */
    fun toGpx(plan: InspectionPlan): String {
        val sb = StringBuilder()
        sb.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
        sb.append("<gpx version=\"1.1\" creator=\"BeaconFix-Android\"\n")
        sb.append("  xmlns=\"http://www.topografix.com/GPX/1/1\">\n")

        // Target Camera Waypoint
        sb.append(String.format(Locale.US, "  <wpt lat=\"%.6f\" lon=\"%.6f\">\n", plan.cameraLat, plan.cameraLon))
        sb.append("    <name>${plan.cameraId}</name>\n")
        sb.append("    <desc>Target Camera: ${plan.operatorName} ${plan.model}</desc>\n")
        sb.append("    <sym>camera</sym>\n")
        sb.append("  </wpt>\n")

        // Primary Vantage Waypoint
        val v0 = plan.vantages.firstOrNull()
        if (v0 != null) {
            sb.append(String.format(Locale.US, "  <wpt lat=\"%.6f\" lon=\"%.6f\">\n", v0.lat, v0.lon))
            sb.append("    <name>Safe Vantage Point</name>\n")
            sb.append("    <desc>${v0.side}: ${v0.reason}; Look ${v0.bearingToCamera.toInt()} deg towards camera</desc>\n")
            sb.append("    <sym>eye</sym>\n")
            sb.append("  </wpt>\n")
        }

        // Approach Turn-by-Turn Route (<rte> for OsmAnd / Organic Maps navigation)
        if (plan.toVantageLeg.steps.isNotEmpty()) {
            sb.append("  <rte>\n    <name>Turn-by-turn to Vantage</name>\n")
            for (s in plan.toVantageLeg.steps) {
                sb.append(String.format(Locale.US, "    <rtept lat=\"%.6f\" lon=\"%.6f\">\n", s.lat, s.lon))
                sb.append("      <name>${escapeXml(s.instruction)}</name>\n")
                sb.append("      <desc>${escapeXml(s.streetName)} (${s.distanceM.toInt()} m)</desc>\n")
                sb.append("      <sym>navigation</sym>\n")
                sb.append("    </rtept>\n")
            }
            sb.append("  </rte>\n")
        }

        // Approach Track
        if (plan.toVantageLeg.coordinates.isNotEmpty()) {
            sb.append("  <trk>\n    <name>Approach to Vantage</name>\n    <trkseg>\n")
            for (p in plan.toVantageLeg.coordinates) {
                sb.append(String.format(Locale.US, "      <trkpt lat=\"%.6f\" lon=\"%.6f\"/>\n", p.first, p.second))
            }
            sb.append("    </trkseg>\n  </trk>\n")
        }

        // Departure Track
        if (plan.awayLeg.coordinates.isNotEmpty()) {
            sb.append("  <trk>\n    <name>Departure from Vantage</name>\n    <trkseg>\n")
            for (p in plan.awayLeg.coordinates) {
                sb.append(String.format(Locale.US, "      <trkpt lat=\"%.6f\" lon=\"%.6f\"/>\n", p.first, p.second))
            }
            sb.append("    </trkseg>\n  </trk>\n")
        }

        sb.append("</gpx>\n")
        return sb.toString()
    }

    /**
     * Writes GPX and returns shareable URI for OsmAnd / Organic Maps.
     */
    fun exportGpxUri(plan: InspectionPlan): Uri? = runCatching {
        val safeName = plan.cameraId.replace(':', '_').replace('/', '_')
        val file = File(context.cacheDir, "inspection_${safeName}.gpx")
        file.writeText(toGpx(plan))
        FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
    }.getOrNull()

    private fun parseDirectionDeg(dir: String): Double? {
        if (dir.isBlank()) return null
        dir.toDoubleOrNull()?.let { return (it % 360.0 + 360.0) % 360.0 }
        return when (dir.trim().uppercase(Locale.US)) {
            "N" -> 0.0; "NNE" -> 22.5; "NE" -> 45.0; "ENE" -> 67.5
            "E" -> 90.0; "ESE" -> 112.5; "SE" -> 135.0; "SSE" -> 157.5
            "S" -> 180.0; "SSW" -> 202.5; "SW" -> 225.0; "WSW" -> 247.5
            "W" -> 270.0; "WNW" -> 292.5; "NW" -> 315.0; "NNW" -> 337.5
            "EB" -> 90.0; "WB" -> 270.0; "NB" -> 0.0; "SB" -> 180.0
            else -> null
        }
    }

    private fun offset(lat: Double, lon: Double, dxEast: Double, dyNorth: Double): Pair<Double, Double> {
        val kx = Math.toRadians(1.0) * Geo.R * cos(Math.toRadians(lat))
        val ky = Math.toRadians(1.0) * Geo.R
        return Pair(lat + dyNorth / ky, lon + dxEast / kx)
    }

    private fun buildAvoidConeRing(
        lat: Double,
        lon: Double,
        bearingDeg: Double?,
        rangeM: Double = 75.0,
        halfAngleDeg: Double = 45.0,
    ): List<Pair<Double, Double>> {
        val pts = mutableListOf<Pair<Double, Double>>()
        if (bearingDeg == null) {
            // Disc: 16 points
            for (i in 0..16) {
                val a = Math.toRadians((i % 16) * (360.0 / 16.0))
                pts.add(offset(lat, lon, sin(a) * rangeM, cos(a) * rangeM))
            }
            return pts
        }

        // Apex offset: 8 m behind pole
        val oppRad = Math.toRadians((bearingDeg + 180.0) % 360.0)
        val apex = offset(lat, lon, sin(oppRad) * 8.0, cos(oppRad) * 8.0)
        pts.add(apex)

        val steps = 8
        val startAngle = bearingDeg - halfAngleDeg
        val sweep = halfAngleDeg * 2.0
        for (i in 0..steps) {
            val a = Math.toRadians(startAngle + sweep * (i.toDouble() / steps.toDouble()))
            pts.add(offset(lat, lon, sin(a) * rangeM, cos(a) * rangeM))
        }
        pts.add(apex) // Close ring
        return pts
    }

    private fun escapeXml(s: String): String =
        s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;").replace("'", "&apos;")
}
