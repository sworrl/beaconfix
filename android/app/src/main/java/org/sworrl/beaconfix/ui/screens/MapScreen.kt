package org.sworrl.beaconfix.ui.screens

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Color as AColor
import android.graphics.drawable.BitmapDrawable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import org.sworrl.beaconfix.ui.DoomBatteryChip
import org.sworrl.beaconfix.ui.DoomBatteryDialog
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.SmallFloatingActionButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.ui.platform.LocalContext
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import kotlin.math.roundToInt
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.hilt.navigation.compose.hiltViewModel
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.util.BoundingBox
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.FolderOverlay
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.Overlay
import org.osmdroid.views.overlay.Polygon
import org.osmdroid.views.overlay.Polyline
import org.osmdroid.events.MapEventsReceiver
import org.osmdroid.views.overlay.MapEventsOverlay
import org.sworrl.beaconfix.ui.vm.AnchorsViewModel
import org.sworrl.beaconfix.ui.screens.AnchorEditorSheet
import org.sworrl.beaconfix.data.api.PoiDto
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.ui.EmptyState
import org.sworrl.beaconfix.ui.FixGrade
import kotlin.math.roundToInt
import org.sworrl.beaconfix.ui.SecurityText
import org.sworrl.beaconfix.ui.gradeGlyph
import org.sworrl.beaconfix.ui.secName
import org.sworrl.beaconfix.ui.vm.LiveViewModel
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.map.MapFilterRow
import org.sworrl.beaconfix.ui.map.MapFilter
import org.sworrl.beaconfix.ui.map.MapFocus
import org.sworrl.beaconfix.ui.map.MapPlacesViewModel
import org.sworrl.beaconfix.ui.map.MapStyleChip
import org.sworrl.beaconfix.ui.map.PlaceSheet
import org.sworrl.beaconfix.ui.map.PlaceSheetModel
import org.sworrl.beaconfix.ui.map.PlacesOverlay
import org.sworrl.beaconfix.ui.map.TileStyler
import org.sworrl.beaconfix.ui.map.MapAttribution
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

/**
 * Beacons with SSID labels and security colours, uncertainty circles, both tracks, the cached places (every desktop and this phone), the RV, and a follow toggle.
 *
 * The map is one osmdroid [MapView] whose overlays are a fixed stack of layers ([MapLayers]), set up once. Each layer
 * is refilled by its own effect, keyed on exactly what it draws, so a ranging tick, a new GPS fix or a desktop event
 * redraws the one or two small layers it touches instead of clearing and re-creating every circle, marker and track.
 * Track splitting, road snapping and the beacons' ellipses are computed off the main thread.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MapScreen(
    live: LiveViewModel = hiltViewModel(),
    anchorsVm: AnchorsViewModel = hiltViewModel(),
    placesVm: MapPlacesViewModel = hiltViewModel(),
    hubVm: org.sworrl.beaconfix.ui.vm.HubViewModel = hiltViewModel(),
    inspectVm: org.sworrl.beaconfix.sightings.ui.InspectionViewModel = hiltViewModel(),
) {
    val ctx = LocalContext.current
    val aps by live.mapAps.collectAsState(); val track by live.phoneTrack.collectAsState(); val desk by live.desktopTrack.collectAsState()
    val views by live.views.collectAsState(); val me by live.phone.collectAsState()
    val desktopHeatmap by live.desktopHeatmap.collectAsState()
    val anchorsList by anchorsVm.anchors.collectAsState(); val editing by anchorsVm.editing.collectAsState()
    val ranges by live.ranges.collectAsState()
    val flockCameras by live.flockCameras.collectAsState()
    val activePlan by inspectVm.activePlan.collectAsState()
    val inspectedPlan by inspectVm.inspectedCameraPlan.collectAsState()
    val isInspectLoading by inspectVm.loading.collectAsState()
    val liveGuardAlert by inspectVm.liveGuardAlert.collectAsState()
    val inspectionSheetState = androidx.compose.material3.rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val doomBatteryMode by live.doomBatteryMode.collectAsState()
    var showDoomBatteryDialog by rememberSaveable { mutableStateOf(false) }
    var anchorsLayer by remember { mutableStateOf(true) }
    var heatmapLayer by remember { mutableStateOf(true) }
    var heatmapOpacity by rememberSaveable { mutableFloatStateOf(0.35f) }
    LaunchedEffect(heatmapLayer) { if (heatmapLayer) live.loadHeatmap() }
    var camerasLayer by remember { mutableStateOf(true) }
    var circlesLayer by rememberSaveable { mutableStateOf(true) }
    var selectedApBssid by remember { mutableStateOf<String?>(null) }
    // this phone's own route (the desktop's fixes are in the same table and have their own track below)
    val phone = remember(track) { track.filter { it.source.startsWith("phone") } }
    // road-snapped off the main thread; a fix that arrives while a pass runs waits for it (a StateFlow conflates)
    @android.annotation.SuppressLint("ProduceStateDoesNotAssignValue")   // it does, per track, inside collect (lint only looks one lambda deep)
    val routeSegments by produceState(initialValue = emptyList<org.sworrl.beaconfix.route.RouteSegment>(), live) {
        live.phoneTrack.collect { t -> value = live.snapper.snapTrack(t.filter { it.source.startsWith("phone") }) }
    }
    val anchorMarkers = remember { HashMap<String, Marker>() }
    val dragging = remember { HashSet<String>() }
    var zoomBand by remember { mutableStateOf(bandOf(16.0)) }
    // the area the camera layer covers: the view plus a margin, renewed only once the view leaves it; cones from zoom 13
    var cullBox by remember { mutableStateOf<BoundingBox?>(null) }
    var camDetail by remember { mutableStateOf(true) }
    var follow by remember { mutableStateOf(true) }; var labels by remember { mutableStateOf(true) }
    val ranged = ranges.values.mapNotNull { it.rangedFix }.filter { System.currentTimeMillis() - it.time < 30_000 }.minByOrNull { it.acc }
    val latest = ranged?.let { org.sworrl.beaconfix.data.db.FixEntity(time = it.time, lat = it.lat, lon = it.lon, acc = it.acc, source = "phone-range", provider = if (it.bearingDeg != null) "ranged" else "ring") }
        ?: me.fix ?: views.firstOrNull()?.location?.takeIf { it.valid }?.let { org.sworrl.beaconfix.data.db.FixEntity(time = 0, lat = it.lat, lon = it.lon, acc = it.accuracy, source = "desktop") }
    // places: the offline cache, through the saved filter; the place whose sheet is open always draws
    val cached by placesVm.places.collectAsState(); val style by placesVm.style.collectAsState(); val filter by placesVm.filter.collectAsState()
    val prefetchTo by placesVm.prefetchTarget.collectAsState()
    var sheet by remember { mutableStateOf<org.sworrl.beaconfix.data.db.PoiEntity?>(null) }
    var sharedPin by remember { mutableStateOf<PlacesOverlay.Pin?>(null) }
    val pois = remember(filter, cached, sheet) { MapFilter.apply(filter, cached).let { l -> sheet?.takeIf { s -> s.source.isNotEmpty() && l.none { it.key == s.key } }?.let { l + it } ?: l } }
    val placesOverlay = remember { PlacesOverlay() }
    val styler = remember { TileStyler() }
    DisposableEffect(styler) { onDispose { styler.detach() } }
    var devicesLayer by remember { mutableStateOf(true) }
    // every node the hub knows (desktop, Steam Deck, …) while the map is up; merged with the LAN desktops' answers, freshest per device
    val hubDevices by hubVm.devices.collectAsState()
    DisposableEffect(hubVm) { hubVm.live(true); onDispose { hubVm.live(false) } }
    val devices = remember(views, hubDevices, devicesLayer) { if (devicesLayer) org.sworrl.beaconfix.net.HubLive.merge(views.flatMap { v -> v.devices + (v.location?.takeIf { it.valid }?.let { l -> listOf(org.sworrl.beaconfix.data.api.LinkedDevice(v.desktop.name.ifEmpty { v.desktop.hostname }, "desktop", "", "", l.lat, l.lon, l.accuracy, l.time, l.ageS, l.source, true)) } ?: emptyList()) }.distinctBy { it.device }, hubDevices) else emptyList() }
    val unsetNodes = remember(devices) { devices.filter { (it.kind == "esp32-node" || it.kind == "node" || it.kind == "mesh") && (it.lat == 0.0 && it.lon == 0.0) } }
    // the RV = the desktop's last known fix (FixDao.lastDesktop(): the desktop track is newest first), when no live desktop position is drawn
    val rvLabel = desk.firstOrNull()?.let { stringResource(R.string.map_rv_pin, ago(it.time)) } ?: ""
    val rvPin = desk.firstOrNull()?.takeIf { devicesLayer && views.none { v -> v.location?.valid == true } }?.let { PlacesOverlay.RvPin(it.lat, it.lon, it.acc, rvLabel) }
    // rendered labels; bounded, since range and age labels change text as they tick
    val cache = remember { object : LinkedHashMap<String, BitmapDrawable>(64, 0.75f, true) { override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, BitmapDrawable>?) = size > 300 } }
    // the beacon layer's own, larger than its working set (at most MAX_AP_LABELS names + the unnamed dots), so a refill hits
    // every label instead of cycling an LRU, and camera / device labels never push beacon labels out
    val apCache = remember { object : LinkedHashMap<String, BitmapDrawable>(64, 0.75f, true) { override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, BitmapDrawable>?) = size > MAX_AP_LABELS + 100 } }
    val layers = remember { MapLayers() }
    var mapRef by remember { mutableStateOf<MapView?>(null) }
    var ticker by remember { mutableStateOf("") }
    var menu by remember { mutableStateOf(false) }
    // "Show on map" from Help / Places / the trip journal / a shared link: re-centre, open the sheet (or drop a pin), then clear the target
    val focus by MapFocus.target.collectAsState()
    val sharedLabel = stringResource(R.string.map_shared_place); val placeLabel = stringResource(R.string.map_place)
    LaunchedEffect(focus, mapRef) {
        val t = focus ?: return@LaunchedEffect
        val m = mapRef ?: return@LaunchedEffect
        follow = false
        PlacesOverlay.centreOn(m, t.lat, t.lon, t.zoom)
        val inspectId = t.inspectCamId
        if (inspectId != null) {
            val cam = flockCameras.firstOrNull { it.id == inspectId }
            val myLat = latest?.lat ?: t.lat
            val myLon = latest?.lon ?: t.lon
            inspectVm.inspectCamera(
                inspectId,
                cam?.lat ?: t.lat,
                cam?.lon ?: t.lon,
                cam?.operatorName ?: "",
                cam?.model ?: "",
                cam?.direction ?: "",
                myLat,
                myLon
            )
        } else {
            val key = t.poiKey
            if (key != null) sheet = placesVm.find(key) ?: PlaceSheetModel.synthetic(t, placeLabel)
            else sharedPin = PlacesOverlay.Pin(t.lat, t.lon, t.label.ifBlank { sharedLabel })
        }
        MapFocus.target.value = null
    }
    val map = mapRef
    LaunchedEffect(map, style) {
        val m = map ?: return@LaunchedEffect
        style?.let { styler.apply(m, it) }
        layers.attribution?.style = style
    }
    // follow: glide to a new position (or when following is switched back on), not on every redraw
    LaunchedEffect(map, follow, latest?.lat, latest?.lon) {
        val m = map ?: return@LaunchedEffect
        if (follow && latest != null) m.controller.animateTo(GeoPoint(latest.lat, latest.lon))
    }
    // Traveled routes: road-snapped vehicular tracks and natural footpaths, then the desktop's recent track
    LaunchedEffect(map, routeSegments, if (routeSegments.isEmpty()) phone else null, desk, desktopHeatmap, heatmapLayer, heatmapOpacity) {
        val m = map ?: return@LaunchedEffect
        val segments = withContext(Dispatchers.Default) {
            if (routeSegments.isNotEmpty()) routeSegments.map { seg -> seg.mode to seg.points.map { GeoPoint(it.lat, it.lon) } }
            else splitIntoSegments(phone).map { org.sworrl.beaconfix.collector.MotionMode.IN_VEHICLE to it }
        }
        val deskSegs = withContext(Dispatchers.Default) { splitIntoSegments(desk) }
        val deskHeatmapSegs = withContext(Dispatchers.Default) { splitRoutePointsIntoSegments(desktopHeatmap) }
        
        layers.attribution?.routing = routeSegments.any { s -> s.points.any { it.isSnapped } }
        m.refill(layers.tracks) { out ->
            // Route Heatmap overlay: smooth ambient heat glow & core along travel routes
            // Opacity is adjustable so satellite/map imagery and building detail remain completely visible
            if (heatmapLayer) {
                val glowAlpha = (heatmapOpacity * 0.40f * 255).roundToInt().coerceIn(8, 255)
                val coreAlpha = (heatmapOpacity * 0.85f * 255).roundToInt().coerceIn(16, 255)
                val glowCol = AColor.argb(glowAlpha, 255, 145, 0)
                val coreCol = AColor.argb(coreAlpha, 255, 220, 0)

                val drawHeat = { pts: List<GeoPoint> ->
                    if (pts.size >= 2) {
                        out.add(Polyline(m).apply {
                            setPoints(pts)
                            outlinePaint.color = glowCol
                            outlinePaint.strokeWidth = 14f
                        })
                        out.add(Polyline(m).apply {
                            setPoints(pts)
                            outlinePaint.color = coreCol
                            outlinePaint.strokeWidth = 3.5f
                        })
                    }
                }
                segments.forEach { drawHeat(it.second) }
                deskHeatmapSegs.forEach { drawHeat(it) }
            }

            // Road-snapped vehicular centerlines and pedestrian footpaths
            for ((mode, pts) in segments) {
                if (pts.size < 2) continue
                if (mode == org.sworrl.beaconfix.collector.MotionMode.IN_VEHICLE) {
                    out.add(Polyline(m).apply {
                        setPoints(pts)
                        outlinePaint.color = AColor.parseColor("#00E5FF")
                        outlinePaint.strokeWidth = 2.8f
                    })
                } else {
                    out.add(Polyline(m).apply {
                        setPoints(pts)
                        outlinePaint.color = AColor.parseColor("#6CFF8A")
                        outlinePaint.strokeWidth = 2.8f
                        outlinePaint.pathEffect = android.graphics.DashPathEffect(floatArrayOf(10f, 8f), 0f)
                    })
                }
            }

            // Desktop recent track: only drawn when heatmap is disabled, preventing yellow line clutter over buildings
            if (!heatmapLayer) {
                for (pts in deskSegs) {
                    if (pts.size < 2) continue
                    out.add(Polyline(m).apply {
                        setPoints(pts)
                        outlinePaint.color = AColor.parseColor("#FFD166")
                        outlinePaint.strokeWidth = 3.5f
                        outlinePaint.pathEffect = android.graphics.DashPathEffect(floatArrayOf(8f, 6f), 0f)
                    })
                }
            }
        }
    }
    // AP uncertainty circles (transparent fill so roads/basemap stay crystal clear, highlighted on tap); the rings are
    // computed off the main thread once per beacon list, the layer redraws for the list, a tap, labels or the zoom band
    @android.annotation.SuppressLint("ProduceStateDoesNotAssignValue")   // it does (below, after the off-thread compute): a lint false positive
    val apShapes by produceState(ApShapes(emptyList(), emptyMap()), aps) {
        val shapes = withContext(Dispatchers.Default) { ApShapes(aps, aps.mapNotNull { a -> apShape(a)?.let { a.bssid to it } }.toMap()) }
        value = shapes
    }
    // where beacon names are drawn: the culled area once the view has settled (a glide re-culls on every frame)
    var labelBox by remember { mutableStateOf<BoundingBox?>(null) }
    LaunchedEffect(cullBox) { if (labelBox != null) delay(SETTLE_MS); labelBox = cullBox }
    // The markers of the last full build, by bssid, and the names they show: a settled pan only swaps the icons whose
    // name came or went, rather than rebuilding every Marker and Polygon (thousands in a city: a hitch after each pan)
    val apMarkers = remember { HashMap<String, Marker>() }
    var apNamed by remember { mutableStateOf<Set<String>>(emptySet()) }
    LaunchedEffect(map, apShapes, circlesLayer, labels, zoomBand, selectedApBssid) {
        val m = map ?: return@LaunchedEffect
        val named = withContext(Dispatchers.Default) { apNamedIn(apShapes.aps, if (labels) labelBox else null, m.zoomLevelDouble) }
        apMarkers.clear(); apNamed = named
        m.refill(layers.aps) { out ->
            for (a in apShapes.aps) {
                val p = GeoPoint(a.lat!!, a.lon!!)
                val g = SecurityText.grade(a.security)
                val col = when { a.home -> "#FF4FD8"; g == "critical" -> "#FF4D4D"; g == "weak" && a.posSource != "placed" -> "#FF9F43"; a.posSource == "observed" -> "#35D6FF"; a.posSource == "placed" -> "#FFD166"; else -> "#9FB0C8" }
                val graded = FixGrade.graded(a)
                val mobile = graded && FixGrade.isMobile(a)
                val isSelected = (selectedApBssid != null && a.bssid == selectedApBssid)
                val shape = apShapes.shapes[a.bssid]
                if (circlesLayer && shape != null) {
                    when (shape.kind) {
                        ApShape.REGION -> out.add(Polygon(m).apply {
                            points = shape.points
                            fillPaint.color = if (isSelected) FixGrade.argb(a.grade, 0x2E) else AColor.TRANSPARENT
                            outlinePaint.color = FixGrade.argb(a.grade, if (isSelected) 0xDD else 0x55)
                            outlinePaint.strokeWidth = if (isSelected) 2.5f else 1f
                        })
                        ApShape.ELLIPSE -> out.add(Polygon(m).apply {
                            points = shape.points
                            fillPaint.color = if (isSelected) FixGrade.argb(a.grade, 0x33) else AColor.TRANSPARENT
                            outlinePaint.color = FixGrade.argb(a.grade, if (isSelected) 0xFF else 0x88)
                            outlinePaint.strokeWidth = if (isSelected) 2.5f else 1.2f
                            if (shape.dashed && !isSelected) outlinePaint.pathEffect = android.graphics.DashPathEffect(floatArrayOf(8f, 6f), 0f)
                        })
                        else -> out.add(Polygon(m).apply {
                            points = shape.points
                            fillPaint.color = if (isSelected) AColor.parseColor("#30" + col.drop(1)) else AColor.TRANSPARENT
                            outlinePaint.color = AColor.parseColor(if (isSelected) col else "#66" + col.drop(1))
                            outlinePaint.strokeWidth = if (isSelected) 2.2f else 1f
                            outlinePaint.pathEffect = android.graphics.DashPathEffect(floatArrayOf(6f, 6f), 0f)
                        })
                    }
                }
                val gradeLine = when {
                    !graded -> ""
                    mobile -> "\n" + m.context.getString(R.string.fit_map_mobile)
                    else -> "\n" + m.context.getString(R.string.fit_map_snippet, a.grade ?: "", (a.score ?: 0.0).toInt(), org.sworrl.beaconfix.ui.metres(a.r95 ?: (a.acc ?: 0.0) * 2.45), ((a.pWithin25 ?: 0.0) * 100).roundToInt())
                }
                out.add(Marker(m).apply {
                    position = p; setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                    icon = apIcon(m, apCache, a, a.bssid in named)
                    apMarkers[a.bssid] = this
                    title = a.ssid.ifEmpty { "(hidden)" }
                    snippet = "${a.bssid} · ${a.band} GHz ch ${a.ch}\n${secName(a.security)} · ${g}\n±${(a.acc ?: 0.0).toInt()} m (${a.posSource})" + (a.residual?.let { "\nfit ${it.toInt()} m" } ?: "") + gradeLine + (live.status.state.value.rttRanges[a.bssid]?.let { "\n" + it.text } ?: "") + "\n" + (SecurityText.forSecurity(a.security).firstOrNull()?.nerd ?: "")
                    setOnMarkerClickListener { mk, _ ->
                        selectedApBssid = a.bssid
                        mk.showInfoWindow()
                        true
                    }
                })
            }
        }
    }
    LaunchedEffect(if (labels) labelBox else null) {             // the view settled elsewhere: only the names move
        val m = map ?: return@LaunchedEffect
        val named = withContext(Dispatchers.Default) { apNamedIn(apShapes.aps, if (labels) labelBox else null, m.zoomLevelDouble) }
        if (named == apNamed) return@LaunchedEffect
        val byId = apShapes.aps.associateBy { it.bssid }
        for (id in (named - apNamed) + (apNamed - named)) {
            val a = byId[id] ?: continue
            apMarkers[id]?.icon = apIcon(m, apCache, a, id in named)
        }
        apNamed = named
        m.invalidate()
    }
    LaunchedEffect(map, pois, labels, zoomBand, rvPin, sharedPin) {
        val m = map ?: return@LaunchedEffect
        m.refill(layers.places) { out -> placesOverlay.draw(m, pois, labels, rvPin, sharedPin, onTap = { sheet = it }, onPin = { pin -> sheet = PlaceSheetModel.synthetic(MapFocus.Target(pin.lat, pin.lon, label = pin.label), pin.label) }, into = out) }
    }
    // the cameras for the area on screen: asked for again once the view is CAMERA_REFETCH_M from where they were last
    // answered for, after it has stood still SETTLE_MS (a glide re-culls on every frame: each frame restarts this and
    // cancels the wait); a failed fetch leaves camerasFrom alone, so the next pan, or the desktop answering again, retries
    val camerasFrom by live.camerasFrom.collectAsState()
    val desktopUp = views.any { it.error.isEmpty() && it.fetched > 0 }
    LaunchedEffect(cullBox, camerasLayer, camerasFrom, desktopUp) {
        val c = cullBox?.centerWithDateLine ?: return@LaunchedEffect
        if (!camerasLayer || (c.latitude == 0.0 && c.longitude == 0.0)) return@LaunchedEffect
        val last = camerasFrom
        if (last != null && org.sworrl.beaconfix.estimate.Geo.distanceM(last.first, last.second, c.latitude, c.longitude) < CAMERA_REFETCH_M) return@LaunchedEffect
        delay(SETTLE_MS)
        live.refreshCameras(c.latitude, c.longitude, CAMERA_REFETCH_M)
    }
    // Surveillance & Flock camera pins (rendered above AP circles and places for clear visibility): only those in the
    // culled area, at most MAX_CAMERAS nearest the middle of it, so thousands of cameras never become thousands of overlays
    LaunchedEffect(map, flockCameras, camerasLayer, labels, cullBox, camDetail) {
        val m = map ?: return@LaunchedEffect
        val box = cullBox
        val shown = if (!camerasLayer || box == null) emptyList() else withContext(Dispatchers.Default) {
            val c = box.centerWithDateLine
            flockCameras.filter { (it.lat != 0.0 || it.lon != 0.0) && box.contains(it.lat, it.lon) }
                .sortedBy { org.sworrl.beaconfix.estimate.Geo.distanceM(c.latitude, c.longitude, it.lat, it.lon) }.take(MAX_CAMERAS)
        }
        layers.attribution?.cameras = shown.isNotEmpty()
        m.refill(layers.cameras) { out ->
            if (camerasLayer) {
                for (cam in shown) {
                    if (cam.lat == 0.0 && cam.lon == 0.0) continue
                    val gp = GeoPoint(cam.lat, cam.lon)
                    val isVetted = cam.vetted
                    val isPassed = cam.passCount > 0
                    val color = if (isPassed) "#FF2A4B" else if (isVetted) "#00E5FF" else "#FF9100"
                    val glyph = if (isPassed) "🚨" else if (isVetted) "🛡" else "📷"
                    val bearing = parseDirectionToDegrees(cam.direction, cam.notes)

                    // Directional FOV cone on the road surface or 360-degree radar ring (a few pixels below zoom 13: left out)
                    if (camDetail && bearing != null) {
                        val conePoints = buildFovCone(gp, bearing, 55.0, 42.0)
                        out.add(Polygon(m).apply {
                            points = conePoints
                            fillPaint.color = AColor.parseColor(if (isPassed) "#40FF2A4B" else if (isVetted) "#2800E5FF" else "#28FF9100")
                            outlinePaint.color = AColor.parseColor(color)
                            outlinePaint.strokeWidth = if (isPassed) 2.4f else 1.8f
                            title = "${cam.model} FOV (${cam.direction})"
                        })
                    } else if (camDetail) {
                        out.add(Polygon(m).apply {
                            points = Polygon.pointsAsCircle(gp, if (isPassed) 32.0 else 25.0)
                            fillPaint.color = AColor.parseColor(if (isPassed) "#30FF2A4B" else if (isVetted) "#1A00E5FF" else "#1AFF9100")
                            outlinePaint.color = AColor.parseColor(color)
                            outlinePaint.strokeWidth = if (isPassed) 1.8f else 1.2f
                            outlinePaint.pathEffect = android.graphics.DashPathEffect(floatArrayOf(6f, 6f), 0f)
                        })
                    }

                    // Glowing beacon halo (pulsing red if passed)
                    if (camDetail) out.add(Polygon(m).apply {
                        points = Polygon.pointsAsCircle(gp, if (isPassed) 16.0 else 12.0)
                        fillPaint.color = AColor.parseColor(if (isPassed) "#66FF2A4B" else if (isVetted) "#4400E5FF" else "#44FF9100")
                        outlinePaint.color = AColor.TRANSPARENT
                    })

                    // High-contrast shield marker with pass count badge
                    val camLabel = if (labels) (if (isPassed) "${cam.model} [${cam.passCount}x]" else cam.model) else ""
                    out.add(Marker(m).apply {
                        position = gp
                        setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                        icon = labelIcon(m, cache, glyph, camLabel, AColor.parseColor(color), false, big = true)
                        title = "${cam.model} (${cam.operatorName.ifEmpty { "Flock Safety" }})"
                        snippet = "Passes: ${cam.passCount}\nStatus: ${if (isVetted) "Field Vetted" else "Candidate Location"}\nDirection: ${cam.direction.ifEmpty { "Omni / Unspecified" }}\n${cam.notes}\n[Tap to inspect unseen]"
                        setOnMarkerClickListener { mk, _ ->
                            val myLat = latest?.lat ?: cam.lat
                            val myLon = latest?.lon ?: cam.lon
                            inspectVm.inspectCamera(
                                cam.id,
                                cam.lat,
                                cam.lon,
                                cam.operatorName,
                                cam.model,
                                cam.direction,
                                myLat,
                                myLon
                            )
                            mk.showInfoWindow()
                            true
                        }
                    })
                }
            }
        }
    }
    // inspection unseen layer (docs/SIGHTINGS.md §9.5): avoid regions, route legs, safe vantages with look direction, exposures
    val planToDraw = activePlan ?: inspectedPlan
    LaunchedEffect(map, planToDraw) {
        val m = map ?: return@LaunchedEffect
        val p = planToDraw
        m.refill(layers.inspection) { out ->
            if (p != null) {
                // 1. Avoid regions (semi-transparent red polygons and discs)
                for (reg in p.avoidRegions) {
                    if (reg.coordinates.size >= 3) {
                        out.add(Polygon(m).apply {
                            points = reg.coordinates.map { GeoPoint(it.first, it.second) }
                            fillPaint.color = AColor.parseColor("#35FF2A4B")
                            outlinePaint.color = AColor.parseColor("#CCFF2A4B")
                            outlinePaint.strokeWidth = 2.5f
                            outlinePaint.pathEffect = android.graphics.DashPathEffect(floatArrayOf(8f, 8f), 0f)
                        })
                    }
                }

                // 2. Approach Leg (vibrant green)
                if (p.toVantageLeg.ok && p.toVantageLeg.coordinates.size >= 2) {
                    out.add(Polyline(m).apply {
                        setPoints(p.toVantageLeg.coordinates.map { GeoPoint(it.first, it.second) })
                        outlinePaint.color = AColor.parseColor("#FF32D75F")
                        outlinePaint.strokeWidth = 7f
                    })
                }

                // 3. Departure Leg (vibrant cyan)
                if (p.awayLeg.ok && p.awayLeg.coordinates.size >= 2) {
                    out.add(Polyline(m).apply {
                        setPoints(p.awayLeg.coordinates.map { GeoPoint(it.first, it.second) })
                        outlinePaint.color = AColor.parseColor("#FF32BEFF")
                        outlinePaint.strokeWidth = 7f
                    })
                }

                // 4. Exposures (bright danger red)
                for (exp in p.exposures) {
                    out.add(Polyline(m).apply {
                        setPoints(listOf(GeoPoint(exp.entry.first, exp.entry.second), GeoPoint(exp.exit.first, exp.exit.second)))
                        outlinePaint.color = AColor.parseColor("#FFFF2020")
                        outlinePaint.strokeWidth = 12f
                    })
                }

                // 5. Vantage Point markers with look-arrows pointing at target camera
                for ((idx, vp) in p.vantages.withIndex()) {
                    val vGp = GeoPoint(vp.lat, vp.lon)
                    val arrowPts = buildFovCone(vGp, vp.bearingToCamera, 22.0, 16.0)
                    out.add(Polygon(m).apply {
                        points = arrowPts
                        fillPaint.color = AColor.parseColor("#FFFFD700")
                        outlinePaint.color = AColor.BLACK
                        outlinePaint.strokeWidth = 2f
                    })
                    out.add(Marker(m).apply {
                        position = vGp
                        setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                        icon = labelIcon(m, cache, "${idx + 1}", "Vantage (${vp.side})", AColor.parseColor("#FFD700"), false, big = true)
                        title = "Safe Vantage #${idx + 1} (${vp.side})"
                        snippet = "${vp.reason}\nDistance: ${vp.distanceM.toInt()} m\nLook: ${vp.bearingToCamera.toInt()}°"
                    })
                }
            }
        }
    }
    // linked devices: glyph by kind, name + age, accuracy ring, dashed line + distance to this phone when close
    LaunchedEffect(map, devices, latest) {
        val m = map ?: return@LaunchedEffect
        m.refill(layers.devices) { out ->
            for (dv in devices) {
                if (dv.lat == 0.0 && dv.lon == 0.0) continue
                val gp = GeoPoint(dv.lat, dv.lon)
                val isNode = dv.kind == "esp32-node" || dv.kind == "node" || dv.kind == "mesh"
                val isBase = dv.role == "base_station" || dv.role == "base"
                val glyph = when {
                    isBase -> "🏠"
                    isNode -> "📡"
                    dv.kind == "android" || dv.kind == "phone" -> "📱"
                    dv.kind == "desktop" -> "🖥"
                    else -> "💻"
                }
                val age = dv.ageS?.let { a -> if (a < 90) "now" else if (a < 3600) "${(a / 60).toInt()} min" else "${(a / 3600).toInt()} h" } ?: ""
                val battInfo = if (dv.battPct >= 0) " · 🔋${dv.battPct}%" else ""
                val roleInfo = if (isBase) " · Base" else ""
                val labelText = (dv.identityName.ifEmpty { dv.device }) + roleInfo + battInfo + (if (age.isNotEmpty()) " · $age" else "")
                val nodeColor = if (isBase) "#FFD166" else if (isNode) "#35D6FF" else if (dv.online) "#7CF2C4" else "#9FB0C8"
                if (dv.acc > 0) out.add(Polygon(m).apply { points = Polygon.pointsAsCircle(gp, dv.acc.coerceIn(5.0, 2000.0)); fillPaint.color = AColor.parseColor(if (isNode) "#1A35D6FF" else "#1AFFD166"); outlinePaint.color = AColor.parseColor(nodeColor); outlinePaint.strokeWidth = 1.2f })
                latest?.let { me0 -> val dist = org.sworrl.beaconfix.estimate.Geo.distanceM(me0.lat, me0.lon, dv.lat, dv.lon); if (dist < 2000) out.add(Polyline(m).apply { setPoints(listOf(GeoPoint(me0.lat, me0.lon), gp)); outlinePaint.color = AColor.parseColor(if (isNode) "#AA35D6FF" else "#AAFFD166"); outlinePaint.strokeWidth = 3f; outlinePaint.pathEffect = android.graphics.DashPathEffect(floatArrayOf(12f, 10f), 0f); title = "${dist.toInt()} m to ${dv.device}" }) }
                out.add(Marker(m).apply { position = gp; setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER); icon = labelIcon(m, cache, glyph, labelText, AColor.parseColor(nodeColor), false, big = true); title = dv.device; snippet = "${dv.kind}${if (dv.identityName.isNotEmpty()) " · ${dv.identityName}" else ""}${if (isBase) " · Base Station" else ""}${if (dv.battPct >= 0) " · Batt: ${dv.battPct}%" else ""}\n±${dv.acc.toInt()} m · ${dv.source}${if (age.isNotEmpty()) " · $age ago" else ""}${if (dv.beacons > 0) "\nhears ${dv.beacons} beacons" else ""}" })
            }
        }
    }
    // anchors: ⌖ with the name, draggable (long-press the pin, then move), survey circle
    LaunchedEffect(map, anchorsList, anchorsLayer) {
        val m = map ?: return@LaunchedEffect
        m.refill(layers.anchors) { out ->
            if (anchorsLayer) for (an in anchorsList) {
                val gp = GeoPoint(an.lat, an.lon)
                val isRef = an.ref || an.kind == "fixed-point"
                val ringCol = if (isRef) "#FFD166" else "#B388FF"
                out.add(Polygon(m).apply {
                    points = Polygon.pointsAsCircle(gp, an.accM.coerceIn(0.3, 500.0))
                    fillPaint.color = AColor.parseColor(if (isRef) "#28FFD166" else "#22B388FF")
                    outlinePaint.color = AColor.parseColor(ringCol)
                    outlinePaint.strokeWidth = if (isRef) 2.5f else 1.5f
                })
                val mk = anchorMarkers.getOrPut(an.id) { Marker(m).apply {
                    isDraggable = true; setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                    setOnMarkerDragListener(object : Marker.OnMarkerDragListener {
                        override fun onMarkerDragStart(marker: Marker) { dragging += an.id }
                        override fun onMarkerDrag(marker: Marker) {}
                        override fun onMarkerDragEnd(marker: Marker) { dragging -= an.id; anchorsVm.move(an.id, marker.position.latitude, marker.position.longitude) }
                    })
                    setOnMarkerClickListener { mm, _ -> anchorsVm.anchors.value.firstOrNull { it.id == an.id }?.let { anchorsVm.edit(it) }; mm.showInfoWindow(); true }
                } }
                if (an.id !in dragging) mk.position = gp
                val glyph = if (isRef) "⚓" else if (an.kind == "esp32-node") "📡" else "⌖"
                val badge = if (isRef) " · FIXED REF" else if (an.rv) " · RV" else ""
                mk.icon = labelIcon(m, cache, glyph, an.name + badge, AColor.parseColor(if (isRef) "#FFD166" else "#B388FF"), false, big = true)
                mk.title = an.name + (if (isRef) " [Fixed Ground Truth]" else "")
                mk.snippet = "${kindName(an.kind)} · ±${an.accM} m · ${an.source}" + (if (an.bssids.isNotEmpty()) "\n${an.bssids.joinToString(" ")}" else "") + "\nlong-press to drag · tap to edit"
                out.add(mk)
            }
        }
    }
    // measured ranges: a ring (or a point at the bearing) around the desktop's anchor / position, labelled with the distance
    LaunchedEffect(map, ranges, views) {
        val m = map ?: return@LaunchedEffect
        m.refill(layers.ranges) { out ->
            for (rs in ranges.values) {
                val b = rs.best ?: continue
                val centre = rs.anchor?.let { GeoPoint(it.lat, it.lon) } ?: rs.desktop.let { d -> views.firstOrNull { it.desktop.id == d.id }?.location?.takeIf { it.valid }?.let { GeoPoint(it.lat, it.lon) } } ?: continue
                val col = when (b.cls) { "adjacent" -> "#6CFF8A"; "room" -> "#35D6FF"; "near" -> "#FFD166"; else -> "#9FB0C8" }
                out.add(Polygon(m).apply { points = Polygon.pointsAsCircle(centre, b.distanceM.coerceAtLeast(0.2)); fillPaint.color = AColor.TRANSPARENT; outlinePaint.color = AColor.parseColor(col); outlinePaint.strokeWidth = 3f; outlinePaint.pathEffect = android.graphics.DashPathEffect(floatArrayOf(10f, 8f), 0f); title = rs.line })
                if (b.sigmaM > 0.05) { out.add(Polygon(m).apply { points = Polygon.pointsAsCircle(centre, (b.distanceM + b.sigmaM).coerceAtLeast(0.3)); fillPaint.color = AColor.parseColor("#14" + col.drop(1)); outlinePaint.color = AColor.TRANSPARENT }) }
                rs.rangedFix?.let { rf -> out.add(Polyline(m).apply { setPoints(listOf(centre, GeoPoint(rf.lat, rf.lon))); outlinePaint.color = AColor.parseColor(col); outlinePaint.strokeWidth = 4f; title = rs.line }) }
                out.add(Marker(m).apply { position = centre; setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER); icon = labelIcon(m, cache, "💻", "${rs.name} · ${org.sworrl.beaconfix.ranging.RangeSession.fmtM(b.distanceM)} ±${org.sworrl.beaconfix.ranging.RangeSession.fmtM(b.sigmaM)}", AColor.parseColor(col), false, big = true); title = rs.name; snippet = rs.line + "\n" + b.cls + " · " + b.method.joinToString("+") })
            }
        }
    }
    LaunchedEffect(map, latest) {
        val m = map ?: return@LaunchedEffect
        m.refill(layers.me) { out -> latest?.let { out.add(Marker(m).apply { position = GeoPoint(it.lat, it.lon); setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER); icon = labelIcon(m, cache, "⌖", if (it.source == "phone-range") "you (ranged)" else "you", AColor.WHITE, true, big = true); title = "You (${it.source})"; snippet = "±${it.acc.toInt()} m" }) } }
    }
    Box(Modifier.fillMaxSize()) {
        AndroidView(
            modifier = Modifier.fillMaxSize().semantics { contentDescription = "Map of beacons and positions" },
            factory = { ctx -> MapView(ctx).apply {
                setTileSource(TileSourceFactory.MAPNIK); setMultiTouchControls(true); maxZoomLevel = 23.0; controller.setZoom(16.0); latest?.let { controller.setCenter(GeoPoint(it.lat, it.lon)) }
                val events = MapEventsOverlay(object : MapEventsReceiver {
                    override fun singleTapConfirmedHelper(p: GeoPoint?): Boolean = false
                    override fun longPressHelper(p: GeoPoint?): Boolean { if (p == null) return false; anchorsVm.newAt(p.latitude, p.longitude); return true }
                })
                // bottom to top: (Satellite's labels, added by TileStyler), taps on the bare map, the layers, the credit line
                overlays.add(events); overlays.addAll(layers.stack); overlays.add(MapAttribution(ctx).also { it.style = style; layers.attribution = it })
                // the beacon and place labels change at zoom 11 and 15: only crossing one of those redraws them
                fun recull() { val bb = boundingBox; val c = cullBox; if (c == null || !(c.contains(bb.latNorth, bb.lonWest) && c.contains(bb.latSouth, bb.lonEast))) cullBox = bb.increaseByScale(2f) }
                addOnFirstLayoutListener { _, _, _, _, _ -> recull() }
                addMapListener(object : org.osmdroid.events.MapListener {
                    override fun onScroll(e: org.osmdroid.events.ScrollEvent?): Boolean { recull(); return false }
                    override fun onZoom(e: org.osmdroid.events.ZoomEvent?): Boolean {
                        val z = e?.zoomLevel ?: zoomLevelDouble
                        val b = bandOf(z); if (b != zoomBand) zoomBand = b
                        (z >= 13).let { d -> if (d != camDetail) camDetail = d }
                        recull(); return false
                    }
                })
                mapRef = this
            } },
        )
        Column(Modifier.align(Alignment.TopStart).padding(8.dp)) {
            Surface(tonalElevation = 3.dp, shape = MaterialTheme.shapes.small) {
                Column {
                    Text("  ${aps.size} beacons placed · ${pois.size} places · gold = mapped · cyan = fitted here · red = insecure · magenta = home  ", style = MaterialTheme.typography.labelSmall)
                    if (aps.any { FixGrade.graded(it) }) Text("  " + stringResource(R.string.fit_map_legend) + "  ", style = MaterialTheme.typography.labelSmall)
                }
            }
            Row(modifier = Modifier.horizontalScroll(rememberScrollState()), verticalAlignment = Alignment.CenterVertically) {
                DoomBatteryChip(
                    mode = doomBatteryMode,
                    onClick = { showDoomBatteryDialog = true },
                    modifier = Modifier.padding(end = 6.dp)
                )
                FilterChip(selected = labels, onClick = { labels = !labels }, label = { Text("Aa") })
                FilterChip(selected = devicesLayer, onClick = { devicesLayer = !devicesLayer }, label = { Text("Devices") }, modifier = Modifier.padding(start = 6.dp))
                MapStyleChip(style, { placesVm.setStyle(it) }, Modifier.padding(start = 6.dp))
                FilterChip(selected = anchorsLayer, onClick = { anchorsLayer = !anchorsLayer }, label = { Text("⌖") }, modifier = Modifier.padding(start = 6.dp))
                FilterChip(selected = heatmapLayer, onClick = { heatmapLayer = !heatmapLayer }, label = { Text("Heatmap" + if (heatmapLayer) " ${(heatmapOpacity * 100).toInt()}%" else "") }, modifier = Modifier.padding(start = 6.dp))
                if (heatmapLayer) {
                    FilterChip(
                        selected = false,
                        onClick = {
                            heatmapOpacity = when {
                                heatmapOpacity < 0.30f -> 0.40f
                                heatmapOpacity < 0.55f -> 0.70f
                                heatmapOpacity < 0.85f -> 1.0f
                                else -> 0.20f
                            }
                        },
                        label = { Text("Opacity: ${(heatmapOpacity * 100).toInt()}%") },
                        modifier = Modifier.padding(start = 4.dp)
                    )
                }
                FilterChip(selected = camerasLayer, onClick = { camerasLayer = !camerasLayer }, label = { Text("Cameras") }, modifier = Modifier.padding(start = 6.dp))
                FilterChip(selected = circlesLayer, onClick = { circlesLayer = !circlesLayer }, label = { Text("Circles") }, modifier = Modifier.padding(start = 6.dp))
            }
            MapFilterRow(filter, { placesVm.setFilter(it) })
            if (unsetNodes.isNotEmpty()) {
                Surface(
                    onClick = {
                        mapRef?.let { m ->
                            val target = unsetNodes.first()
                            anchorsVm.newAt(m.mapCenter.latitude, m.mapCenter.longitude, 1.0, "map-pick", "esp32-node")
                        }
                    },
                    modifier = Modifier.padding(top = 4.dp),
                    color = MaterialTheme.colorScheme.tertiaryContainer,
                    shape = MaterialTheme.shapes.small,
                    tonalElevation = 4.dp
                ) {
                    Row(modifier = Modifier.padding(horizontal = 10.dp, vertical = 5.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text("📡 ${if (unsetNodes.size == 1) "${unsetNodes.first().device} (Position Unset)" else "${unsetNodes.size} Mesh Nodes Unset"} — Tap to Place Here", style = MaterialTheme.typography.labelSmall, fontWeight = androidx.compose.ui.text.font.FontWeight.Bold, color = MaterialTheme.colorScheme.onTertiaryContainer)
                    }
                }
            }
        }
        RefitOverlay(mapRef, live.refits.events, onTicker = { ticker = it })
        // below map resolution (the ring would be a few pixels): a proximity inset drawn to scale
        val near = ranges.values.mapNotNull { rs -> rs.best?.let { rs to it } }.minByOrNull { it.second.distanceM }
        val mpp = mapRef?.let { m -> org.osmdroid.util.TileSystem.GroundResolution(m.mapCenter.latitude, m.zoomLevelDouble) } ?: 1.0
        if (near != null && near.second.distanceM / mpp < 60) ProximityInset(near.first, near.second, Modifier.align(Alignment.TopEnd).padding(top = 64.dp, end = 8.dp))
        editing?.let { AnchorEditorSheet(it, anchorsVm) }
        sheet?.let { PlaceSheet(it, latest) { sheet = null } }
        if (ticker.isNotEmpty()) Surface(Modifier.align(Alignment.BottomStart).padding(16.dp), tonalElevation = 3.dp, shape = MaterialTheme.shapes.small) { Text("  $ticker  ", style = MaterialTheme.typography.labelSmall) }
        Column(Modifier.align(Alignment.BottomEnd).padding(16.dp)) {
            SmallFloatingActionButton(onClick = { menu = true }, containerColor = MaterialTheme.colorScheme.surfaceVariant) { Text("⋯") }
            androidx.compose.material3.DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                androidx.compose.material3.DropdownMenuItem(text = { Text("Replay last refit animation") }, onClick = { menu = false; live.replayLastRefit() })
                androidx.compose.material3.DropdownMenuItem(text = { Text(if (follow) "Stop following" else "Follow my position") }, onClick = { menu = false; follow = !follow })
                androidx.compose.material3.DropdownMenuItem(text = { Text(if (labels) "Hide names" else "Show names") }, onClick = { menu = false; labels = !labels })
                if (unsetNodes.isNotEmpty()) {
                    androidx.compose.material3.DropdownMenuItem(
                        text = { Text("📡 Place ${unsetNodes.first().device} at map centre") },
                        onClick = {
                            menu = false
                            mapRef?.let { m ->
                                anchorsVm.newAt(m.mapCenter.latitude, m.mapCenter.longitude, 1.0, "map-pick", "esp32-node")
                            }
                        }
                    )
                }
                androidx.compose.material3.DropdownMenuItem(text = { Text("Place an antenna at the map centre") }, onClick = { menu = false; mapRef?.let { m -> anchorsVm.newAt(m.mapCenter.latitude, m.mapCenter.longitude) } })
                androidx.compose.material3.DropdownMenuItem(text = { Text("⚓ Place Fixed Known Point (Anchor)") }, onClick = { menu = false; mapRef?.let { m -> anchorsVm.newAt(m.mapCenter.latitude, m.mapCenter.longitude, 0.5, "map-pick", "fixed-point") } })
                androidx.compose.material3.DropdownMenuItem(text = { Text("Place an antenna at my position (GNSS average)") }, onClick = { menu = false; anchorsVm.newAt(0.0, 0.0, 1.0, "gps-average"); anchorsVm.startAveraging() })
                androidx.compose.material3.DropdownMenuItem(text = { Column { Text(stringResource(R.string.map_prefetch)); if (prefetchTo == null) Text(stringResource(R.string.map_prefetch_needs_control), style = MaterialTheme.typography.labelSmall) } },
                    enabled = prefetchTo != null, onClick = { menu = false; placesVm.prefetch() })
            }
            SmallFloatingActionButton(onClick = { follow = !follow }, modifier = Modifier.padding(top = 8.dp), containerColor = if (follow) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant) { Text("◎") }
        }
        if (aps.isEmpty() && latest == null) Box(Modifier.align(Alignment.Center)) { EmptyState("🗺", "Nothing to show yet", "Turn the collector on for your own beacons, or pair a desktop to see its map.") }

        if (inspectedPlan != null) {
            org.sworrl.beaconfix.sightings.ui.InspectionSheet(
                plan = inspectedPlan,
                loading = isInspectLoading,
                onDismiss = { inspectVm.closePlanSheet() },
                onStartPrivateInspection = { plan -> inspectVm.startPrivateInspection(plan) },
                onOpenGpx = { plan -> inspectVm.openInExternalNavigation(ctx, plan) },
                onProfileChange = { prof ->
                    val myLat = latest?.lat ?: inspectedPlan!!.cameraLat
                    val myLon = latest?.lon ?: inspectedPlan!!.cameraLon
                    inspectVm.inspectCamera(
                        inspectedPlan!!.cameraId,
                        inspectedPlan!!.cameraLat,
                        inspectedPlan!!.cameraLon,
                        inspectedPlan!!.operatorName,
                        inspectedPlan!!.model,
                        inspectedPlan!!.direction,
                        myLat,
                        myLon,
                        prof
                    )
                },
                sheetState = inspectionSheetState
            )
        }

        if (activePlan != null) {
            org.sworrl.beaconfix.sightings.ui.InspectionActiveHud(
                plan = activePlan!!,
                guardAlert = liveGuardAlert,
                currentLocation = latest?.let { Pair(it.lat, it.lon) },
                onDone = { inspectVm.stopInspection() },
                modifier = Modifier.align(Alignment.TopCenter).padding(top = 8.dp)
            )
        }

        if (showDoomBatteryDialog) {
            DoomBatteryDialog(
                currentMode = doomBatteryMode,
                onSelectMode = { mode ->
                    live.setDoomBatteryMode(mode)
                    showDoomBatteryDialog = false
                },
                onDismiss = { showDoomBatteryDialog = false }
            )
        }
    }
}

/** Two devices at their measured separation, to scale, for when the map cannot resolve sub-metre distances. */
@Composable
private fun ProximityInset(rs: org.sworrl.beaconfix.ranging.RangeSession, b: org.sworrl.beaconfix.ranging.LocalRange, modifier: Modifier) {
    val col = when (b.cls) { "adjacent" -> androidx.compose.ui.graphics.Color(0xFF6CFF8A); "room" -> androidx.compose.ui.graphics.Color(0xFF35D6FF); "near" -> androidx.compose.ui.graphics.Color(0xFFFFD166); else -> androidx.compose.ui.graphics.Color(0xFF9FB0C8) }
    Surface(modifier, tonalElevation = 4.dp, shape = MaterialTheme.shapes.medium) {
        Column(Modifier.padding(10.dp).width(190.dp)) {
            Text("${rs.name} · ${org.sworrl.beaconfix.ranging.RangeSession.fmtM(b.distanceM)}", style = MaterialTheme.typography.labelLarge, color = col)
            Text("${org.sworrl.beaconfix.ranging.RangeSession.methodName(b.method)} ±${org.sworrl.beaconfix.ranging.RangeSession.fmtM(b.sigmaM)} · ${b.cls}", style = MaterialTheme.typography.labelSmall)
            val span = (b.distanceM + b.sigmaM).coerceAtLeast(1.0) * 1.35
            androidx.compose.foundation.Canvas(Modifier.fillMaxWidth().height(70.dp)) {
                val ppm = (size.width - 40.dp.toPx()) / span.toFloat(); val y = size.height / 2; val x0 = 20.dp.toPx(); val x1 = x0 + (b.distanceM * ppm).toFloat()
                // uncertainty band, the separation line, the two devices
                drawLine(col.copy(alpha = 0.25f), androidx.compose.ui.geometry.Offset(x0 + ((b.lowM) * ppm).toFloat(), y), androidx.compose.ui.geometry.Offset(x0 + ((b.highM) * ppm).toFloat(), y), strokeWidth = 14.dp.toPx())
                drawLine(col, androidx.compose.ui.geometry.Offset(x0, y), androidx.compose.ui.geometry.Offset(x1, y), strokeWidth = 3.dp.toPx(), pathEffect = androidx.compose.ui.graphics.PathEffect.dashPathEffect(floatArrayOf(10f, 8f)))
                drawCircle(androidx.compose.ui.graphics.Color.White, 9.dp.toPx(), androidx.compose.ui.geometry.Offset(x0, y)); drawCircle(col, 6.dp.toPx(), androidx.compose.ui.geometry.Offset(x0, y))
                drawCircle(androidx.compose.ui.graphics.Color.White, 9.dp.toPx(), androidx.compose.ui.geometry.Offset(x1, y)); drawCircle(col, 6.dp.toPx(), androidx.compose.ui.geometry.Offset(x1, y))
                // 1 m scale bar
                val sb = (1.0 * ppm).toFloat(); drawLine(androidx.compose.ui.graphics.Color.Gray, androidx.compose.ui.geometry.Offset(x0, size.height - 6.dp.toPx()), androidx.compose.ui.geometry.Offset(x0 + sb, size.height - 6.dp.toPx()), strokeWidth = 2.dp.toPx())
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = androidx.compose.foundation.layout.Arrangement.SpaceBetween) { Text("💻 ${rs.name}", style = MaterialTheme.typography.labelSmall); Text("📱 you", style = MaterialTheme.typography.labelSmall) }
            Text("— 1 m", style = MaterialTheme.typography.labelSmall, color = androidx.compose.ui.graphics.Color.Gray)
        }
    }
}

/** The map's overlays, one folder per layer in draw order (bottom first); each is refilled only when its own inputs change. */
private class MapLayers {
    val tracks = FolderOverlay(); val aps = FolderOverlay(); val places = FolderOverlay(); val cameras = FolderOverlay()
    val inspection = FolderOverlay()
    val devices = FolderOverlay(); val anchors = FolderOverlay(); val ranges = FolderOverlay(); val me = FolderOverlay()
    val stack: List<Overlay> get() = listOf(tracks, aps, places, cameras, inspection, devices, anchors, ranges, me)
    var attribution: MapAttribution? = null
}

/** Replaces [layer]'s contents with what [build] adds (straight into its item list: FolderOverlay.add re-scans the bounds each time), then redraws. */
private inline fun MapView.refill(layer: FolderOverlay, build: (MutableList<Overlay>) -> Unit) {
    val items = ArrayList<Overlay>()
    build(items)
    layer.items.clear(); layer.items.addAll(items)
    invalidate()
}

/** The most cameras drawn at once, and how far the view moves before the cameras are asked for again. */
private const val MAX_CAMERAS = 300
private const val CAMERA_REFETCH_M = 20_000.0
/** The most beacon names drawn at once; how long the view stands still before names and cameras follow it. */
private const val MAX_AP_LABELS = 250
private const val SETTLE_MS = 400L

/** Which label thresholds [zoom] is past: beacon and place names show from 15, pediatric ERs' from 11. */
private fun bandOf(zoom: Double): Int = when { zoom >= 15 -> 2; zoom >= 11 -> 1; else -> 0 }

/** A beacon's uncertainty outline, in map coordinates. */
private class ApShape(val kind: Int, val points: List<GeoPoint>, val dashed: Boolean) {
    companion object { const val REGION = 0; const val ELLIPSE = 1; const val CIRCLE = 2 }
}

/** The beacon list and the outlines computed for it, published together so a layer never pairs one list with another's shapes. */
/** The beacons named on the map: in the label area, at most MAX_AP_LABELS nearest its middle (a dense city is a pile of
 *  text past that); below zoom 15 only the critical, non-mobile ones. */
private fun apNamedIn(aps: List<ApEntity>, box: BoundingBox?, zoomed: Double): Set<String> {
    if (box == null) return emptySet()
    val c = box.centerWithDateLine
    return aps.filter { a -> (zoomed >= 15 || (!(FixGrade.graded(a) && FixGrade.isMobile(a)) && SecurityText.grade(a.security) == "critical")) && box.contains(a.lat!!, a.lon!!) }
        .sortedBy { org.sworrl.beaconfix.estimate.Geo.distanceM(c.latitude, c.longitude, it.lat!!, it.lon!!) }.take(MAX_AP_LABELS).mapTo(HashSet()) { it.bssid }
}

/** A beacon's marker icon: its dot (or M for a mobile one), with its name when [named]. */
private fun apIcon(m: MapView, cache: HashMap<String, BitmapDrawable>, a: ApEntity, named: Boolean): BitmapDrawable {
    val g = SecurityText.grade(a.security)
    val col = when { a.home -> "#FF4FD8"; g == "critical" -> "#FF4D4D"; g == "weak" && a.posSource != "placed" -> "#FF9F43"; a.posSource == "observed" -> "#35D6FF"; a.posSource == "placed" -> "#FFD166"; else -> "#9FB0C8" }
    val name = if (named) a.ssid.ifEmpty { "(hidden)" } else ""
    return if (FixGrade.graded(a) && FixGrade.isMobile(a)) labelIcon(m, cache, "M", name, FixGrade.argb("M"), false)
        else labelIcon(m, cache, if (g == "critical" || g == "weak") gradeGlyph(g) else "", name, AColor.parseColor(col), true)
}

private class ApShapes(val aps: List<ApEntity>, val shapes: Map<String, ApShape>)

/** null for a beacon that travels with us (just its "M"); a region's R95 disc; a graded fix's 95 % ellipse; else the accuracy circle. */
private fun apShape(a: ApEntity): ApShape? {
    val p = GeoPoint(a.lat ?: return null, a.lon ?: return null)
    val graded = FixGrade.graded(a)
    return when {
        graded && FixGrade.isMobile(a) -> null
        graded && FixGrade.isRegion(a) && a.r95 != null -> ApShape(ApShape.REGION, Polygon.pointsAsCircle(p, a.r95.coerceIn(5.0, 3000.0)), false)
        graded && a.semiMajor != null && a.semiMajor > 0 -> ApShape(ApShape.ELLIPSE, FixGrade.ellipse95(a).map { GeoPoint(it.first, it.second) }, FixGrade.dashed(a))
        else -> ApShape(ApShape.CIRCLE, Polygon.pointsAsCircle(p, (a.acc ?: 50.0).coerceIn(5.0, 1500.0)), true)
    }
}

/** A dot (or glyph) with an optional text label to its right, rendered once per (text, colour) and cached. */
private fun labelIcon(map: MapView, cache: HashMap<String, BitmapDrawable>, glyph: String, label: String, color: Int, dot: Boolean, big: Boolean = false): BitmapDrawable {
    val key = "$glyph|$label|$color|$dot|$big"
    cache[key]?.let { return it }
    val d = map.resources.displayMetrics.density
    val p = Paint(Paint.ANTI_ALIAS_FLAG); p.textSize = 11 * d; p.typeface = android.graphics.Typeface.DEFAULT_BOLD
    val gp = Paint(Paint.ANTI_ALIAS_FLAG); gp.textSize = (if (big) 18 else 12) * d
    val r = (if (big) 7 else 5) * d
    val gw = if (glyph.isNotEmpty()) gp.measureText(glyph) else 0f
    val lw = if (label.isNotEmpty()) p.measureText(label) + 10 * d else 0f
    val w = (r * 2 + gw + lw + 8 * d).toInt().coerceAtLeast((r * 2 + 4 * d).toInt()); val h = (Math.max(r * 2, 18 * d) + 6 * d).toInt()
    val bmp = Bitmap.createBitmap(w * 2, h, Bitmap.Config.ARGB_8888)     // the dot sits at the horizontal centre: the label extends right, the left half stays empty
    val c = Canvas(bmp); val cx = w.toFloat(); val cy = h / 2f
    if (dot) { p.color = color; p.alpha = 70; c.drawCircle(cx, cy, r * 2.2f, p); p.alpha = 255; c.drawCircle(cx, cy, r, p); p.color = AColor.WHITE; p.style = Paint.Style.STROKE; p.strokeWidth = 1.5f * d; c.drawCircle(cx, cy, r, p); p.style = Paint.Style.FILL }
    var x = cx + r + 3 * d
    if (glyph.isNotEmpty()) { gp.color = color; c.drawText(glyph, x, cy + gp.textSize / 3, gp); x += gw + 3 * d }
    if (label.isNotEmpty()) {
        val tw = p.measureText(label); val bg = Paint(Paint.ANTI_ALIAS_FLAG); bg.color = AColor.argb(200, 8, 13, 20)
        c.drawRoundRect(x, cy - 8 * d, x + tw + 8 * d, cy + 8 * d, 4 * d, 4 * d, bg)
        p.color = color; c.drawText(label, x + 4 * d, cy + p.textSize / 3, p)
    }
    return BitmapDrawable(map.resources, bmp).also { cache[key] = it }
}
@Suppress("unused") private fun unusedPoi(p: PoiDto, a: ApEntity) = Unit

private fun parseDirectionToDegrees(dir: String, notes: String): Double? {
    val combined = (dir + " " + notes).uppercase()
    return when {
        "NORTHBOUND" in combined || combined.trim() == "NB" || combined.trim() == "N" -> 0.0
        "NORTHEAST" in combined || combined.trim() == "NE" -> 45.0
        "EASTBOUND" in combined || combined.trim() == "EB" || combined.trim() == "E" -> 90.0
        "SOUTHEAST" in combined || combined.trim() == "SE" -> 135.0
        "SOUTHBOUND" in combined || combined.trim() == "SB" || combined.trim() == "S" -> 180.0
        "SOUTHWEST" in combined || combined.trim() == "SW" -> 225.0
        "WESTBOUND" in combined || combined.trim() == "WB" || combined.trim() == "W" -> 270.0
        "NORTHWEST" in combined || combined.trim() == "NW" -> 315.0
        else -> {
            val num = Regex("""\b(\d{1,3})\s*(?:deg|°)?\b""").find(combined)?.groupValues?.get(1)?.toDoubleOrNull()
            if (num != null && num in 0.0..360.0) num else null
        }
    }
}

private fun buildFovCone(center: GeoPoint, bearingDeg: Double, distanceM: Double, spanDeg: Double): List<GeoPoint> {
    val pts = mutableListOf<GeoPoint>()
    pts.add(center)
    val startAngle = bearingDeg - spanDeg / 2.0
    val endAngle = bearingDeg + spanDeg / 2.0
    val steps = 8
    for (i in 0..steps) {
        val ang = startAngle + (endAngle - startAngle) * (i.toDouble() / steps)
        val radAng = Math.toRadians(ang)
        val dLat = (distanceM * kotlin.math.cos(radAng)) / 111319.5
        val dLon = (distanceM * kotlin.math.sin(radAng)) / (111319.5 * kotlin.math.cos(Math.toRadians(center.latitude)))
        pts.add(GeoPoint(center.latitude + dLat, center.longitude + dLon))
    }
    pts.add(center)
    return pts
}

private fun splitIntoSegments(fixes: List<org.sworrl.beaconfix.data.db.FixEntity>): List<List<GeoPoint>> {
    val valid = fixes.filter { it.lat != 0.0 && it.lon != 0.0 && it.acc <= 500 }.sortedBy { it.time }
    if (valid.size < 2) return emptyList()

    val segments = mutableListOf<List<GeoPoint>>()
    var cur = mutableListOf<GeoPoint>()
    var lastFix: org.sworrl.beaconfix.data.db.FixEntity? = null
    var lastAdded: GeoPoint? = null

    for (f in valid) {
        val gp = GeoPoint(f.lat, f.lon)
        if (lastFix != null) {
            val dtSec = if (f.time > 0 && lastFix.time > 0) Math.abs(f.time - lastFix.time) / 1000.0 else 0.0
            val dist = org.sworrl.beaconfix.estimate.Geo.distanceM(lastFix.lat, lastFix.lon, f.lat, f.lon)

            // Speed spike check (>35 m/s or ~78 mph)
            if (dtSec in 1.0..300.0 && (dist / dtSec) > 35.0 && dist > 200.0) {
                continue
            }

            // Trip break check (>15 min or >4000m)
            if (dtSec > 900.0 || dist > 4000.0) {
                if (cur.size > 1) segments.add(cur)
                cur = mutableListOf()
                lastAdded = null
            } else if (lastAdded != null) {
                val dLast = org.sworrl.beaconfix.estimate.Geo.distanceM(lastAdded.latitude, lastAdded.longitude, f.lat, f.lon)
                if (dLast < 25.0) {
                    lastFix = f
                    continue
                }
            }
        }
        cur.add(gp)
        lastFix = f
        lastAdded = gp
    }

    if (cur.size > 1) segments.add(cur)
    return segments
}


private fun splitRoutePointsIntoSegments(fixes: List<org.sworrl.beaconfix.data.api.RoutePointDto>): List<List<org.osmdroid.util.GeoPoint>> {
    if (fixes.size < 2) return emptyList()

    val segments = mutableListOf<List<org.osmdroid.util.GeoPoint>>()
    var cur = mutableListOf<org.osmdroid.util.GeoPoint>()
    var lastAdded: org.osmdroid.util.GeoPoint? = null

    for (f in fixes) {
        val gp = org.osmdroid.util.GeoPoint(f.lat, f.lon)
        if (lastAdded != null) {
            val dist = org.sworrl.beaconfix.estimate.Geo.distanceM(lastAdded.latitude, lastAdded.longitude, f.lat, f.lon)
            if (dist > 4000.0) {
                if (cur.size > 1) segments.add(cur)
                cur = mutableListOf()
                lastAdded = null
            } else if (dist < 25.0) {
                continue
            }
        }
        cur.add(gp)
        lastAdded = gp
    }

    if (cur.size > 1) segments.add(cur)
    return segments
}
