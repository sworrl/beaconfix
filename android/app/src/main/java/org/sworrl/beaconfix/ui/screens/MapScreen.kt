package org.sworrl.beaconfix.ui.screens

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Color as AColor
import android.graphics.drawable.BitmapDrawable
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
import androidx.compose.material3.SmallFloatingActionButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.hilt.navigation.compose.hiltViewModel
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.Polygon
import org.osmdroid.views.overlay.Polyline
import org.osmdroid.events.MapEventsReceiver
import org.osmdroid.views.overlay.MapEventsOverlay
import org.sworrl.beaconfix.ui.vm.AnchorsViewModel
import org.sworrl.beaconfix.ui.screens.AnchorEditorSheet
import org.sworrl.beaconfix.data.api.PoiDto
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.ui.EmptyState
import org.sworrl.beaconfix.ui.SecurityText
import org.sworrl.beaconfix.ui.gradeGlyph
import org.sworrl.beaconfix.ui.secName
import org.sworrl.beaconfix.ui.vm.LiveViewModel

/** Beacons with SSID labels and security colours, uncertainty circles, both tracks, the desktop's places, and a follow toggle. */
@Composable
fun MapScreen(live: LiveViewModel = hiltViewModel(), anchorsVm: AnchorsViewModel = hiltViewModel()) {
    val aps by live.positioned.collectAsState(); val phone by live.phoneTrack.collectAsState(); val desk by live.desktopTrack.collectAsState()
    val views by live.views.collectAsState(); val me by live.phone.collectAsState()
    val anchorsList by anchorsVm.anchors.collectAsState(); val editing by anchorsVm.editing.collectAsState()
    val ranges by live.ranges.collectAsState()
    var anchorsLayer by remember { mutableStateOf(true) }
    val anchorMarkers = remember { HashMap<String, Marker>() }
    val dragging = remember { HashSet<String>() }
    var eventsOverlay by remember { mutableStateOf<MapEventsOverlay?>(null) }
    var zoomTick by remember { mutableStateOf(0) }
    var follow by remember { mutableStateOf(true) }; var labels by remember { mutableStateOf(true) }; var places by remember { mutableStateOf(true) }; var layer by remember { mutableStateOf("map") }
    val ranged = ranges.values.mapNotNull { it.rangedFix }.filter { System.currentTimeMillis() - it.time < 30_000 }.minByOrNull { it.acc }
    val latest = ranged?.let { org.sworrl.beaconfix.data.db.FixEntity(time = it.time, lat = it.lat, lon = it.lon, acc = it.acc, source = "phone-range", provider = if (it.bearingDeg != null) "ranged" else "ring") }
        ?: me.fix ?: views.firstOrNull()?.location?.takeIf { it.valid }?.let { org.sworrl.beaconfix.data.db.FixEntity(time = 0, lat = it.lat, lon = it.lon, acc = it.accuracy, source = "desktop") }
    val pois = if (places) views.flatMap { it.pois } else emptyList()
    var devicesLayer by remember { mutableStateOf(true) }
    val devices = if (devicesLayer) views.flatMap { v -> v.devices + (v.location?.takeIf { it.valid }?.let { l -> listOf(org.sworrl.beaconfix.data.api.LinkedDevice(v.desktop.name.ifEmpty { v.desktop.hostname }, "desktop", "", "", l.lat, l.lon, l.accuracy, l.time, l.ageS, l.source, true)) } ?: emptyList()) }.distinctBy { it.device } else emptyList()
    val cache = remember { HashMap<String, BitmapDrawable>() }
    var mapRef by remember { mutableStateOf<MapView?>(null) }
    var ticker by remember { mutableStateOf("") }
    var menu by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxSize()) {
        AndroidView(
            modifier = Modifier.fillMaxSize().semantics { contentDescription = "Map of beacons and positions" },
            factory = { ctx -> MapView(ctx).apply {
                setTileSource(TileSourceFactory.MAPNIK); setMultiTouchControls(true); maxZoomLevel = 22.0; controller.setZoom(16.0); latest?.let { controller.setCenter(GeoPoint(it.lat, it.lon)) }; mapRef = this
                eventsOverlay = MapEventsOverlay(object : MapEventsReceiver {
                    override fun singleTapConfirmedHelper(p: GeoPoint?): Boolean = false
                    override fun longPressHelper(p: GeoPoint?): Boolean { if (p == null) return false; anchorsVm.newAt(p.latitude, p.longitude); return true }
                })
                var lastZ = -1
                addMapListener(object : org.osmdroid.events.MapListener { override fun onScroll(e: org.osmdroid.events.ScrollEvent?) = false; override fun onZoom(e: org.osmdroid.events.ZoomEvent?): Boolean { val z = e?.zoomLevel?.toInt() ?: -1; if (z != lastZ) { lastZ = z; zoomTick++ }; return false } })
            } },
            update = { map ->
                val want = if (layer == "sat") ESRI_IMAGERY else TileSourceFactory.MAPNIK
                if (map.tileProvider.tileSource.name() != want.name()) map.setTileSource(want)
                map.overlays.clear()
                map.overlays.add(org.osmdroid.views.overlay.CopyrightOverlay(map.context))
                eventsOverlay?.let { map.overlays.add(it) }
                @Suppress("UNUSED_EXPRESSION") zoomTick
                if (follow && latest != null) map.controller.animateTo(GeoPoint(latest.lat, latest.lon))
                if (desk.size > 1) map.overlays.add(Polyline(map).apply { setPoints(desk.map { GeoPoint(it.lat, it.lon) }); outlinePaint.color = AColor.parseColor("#FFD166"); outlinePaint.strokeWidth = 5f })
                val pt = phone.filter { it.source.startsWith("phone") }
                if (pt.size > 1) map.overlays.add(Polyline(map).apply { setPoints(pt.map { GeoPoint(it.lat, it.lon) }); outlinePaint.color = AColor.parseColor("#35D6FF"); outlinePaint.strokeWidth = 5f })
                for (p in pois) map.overlays.add(Marker(map).apply { position = GeoPoint(p.lat, p.lon); setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER); icon = labelIcon(map, cache, p.icon.ifEmpty { "📍" }, if (map.zoomLevelDouble >= 15) p.name.ifEmpty { p.label } else "", AColor.parseColor(p.color.ifEmpty { "#9FB0C8" }), false); title = p.name.ifEmpty { p.label }; snippet = listOf(p.label, p.address, p.detail, p.phone, p.hours).filter { it.isNotEmpty() }.joinToString("\n") })
                val zoomed = map.zoomLevelDouble
                for (a in aps) {
                    val p = GeoPoint(a.lat!!, a.lon!!)
                    val g = SecurityText.grade(a.security)
                    val col = when { a.home -> "#FF4FD8"; g == "critical" -> "#FF4D4D"; g == "weak" && a.posSource != "placed" -> "#FF9F43"; a.posSource == "observed" -> "#35D6FF"; a.posSource == "placed" -> "#FFD166"; else -> "#9FB0C8" }
                    map.overlays.add(Polygon(map).apply { points = Polygon.pointsAsCircle(p, (a.acc ?: 50.0).coerceIn(5.0, 1500.0)); fillPaint.color = AColor.parseColor("#22" + col.drop(1)); outlinePaint.color = AColor.parseColor(col); outlinePaint.strokeWidth = 1.5f })
                    map.overlays.add(Marker(map).apply {
                        position = p; setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                        icon = labelIcon(map, cache, if (g == "critical" || g == "weak") gradeGlyph(g) else "", if (labels && (zoomed >= 15 || g == "critical")) a.ssid.ifEmpty { "(hidden)" } else "", AColor.parseColor(col), true)
                        title = a.ssid.ifEmpty { "(hidden)" }
                        snippet = "${a.bssid} · ${a.band} GHz ch ${a.ch}\n${secName(a.security)} · ${g}\n±${(a.acc ?: 0.0).toInt()} m (${a.posSource})" + (a.residual?.let { "\nfit ${it.toInt()} m" } ?: "") + "\n" + (SecurityText.forSecurity(a.security).firstOrNull()?.nerd ?: "")
                    })
                }
                // linked devices: glyph by kind, name + age, accuracy ring, dashed line + distance to this phone when close
                for (dv in devices) {
                    if (dv.lat == 0.0 && dv.lon == 0.0) continue
                    val gp = GeoPoint(dv.lat, dv.lon); val glyph = if (dv.kind == "android" || dv.kind == "phone") "📱" else "💻"
                    val age = dv.ageS?.let { a -> if (a < 90) "now" else if (a < 3600) "${(a / 60).toInt()} min" else "${(a / 3600).toInt()} h" } ?: ""
                    if (dv.acc > 0) map.overlays.add(Polygon(map).apply { points = Polygon.pointsAsCircle(gp, dv.acc.coerceIn(5.0, 2000.0)); fillPaint.color = AColor.parseColor("#1AFFD166"); outlinePaint.color = AColor.parseColor("#FFD166"); outlinePaint.strokeWidth = 1.2f })
                    latest?.let { me0 -> val dist = org.sworrl.beaconfix.estimate.Geo.distanceM(me0.lat, me0.lon, dv.lat, dv.lon); if (dist < 2000) map.overlays.add(Polyline(map).apply { setPoints(listOf(GeoPoint(me0.lat, me0.lon), gp)); outlinePaint.color = AColor.parseColor("#AAFFD166"); outlinePaint.strokeWidth = 3f; outlinePaint.pathEffect = android.graphics.DashPathEffect(floatArrayOf(12f, 10f), 0f); title = "${dist.toInt()} m to ${dv.device}" }) }
                    map.overlays.add(Marker(map).apply { position = gp; setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER); icon = labelIcon(map, cache, glyph, (dv.identityName.ifEmpty { dv.device }) + (if (age.isNotEmpty()) " · $age" else ""), AColor.parseColor(if (dv.online) "#FFD166" else "#9FB0C8"), false, big = true); title = dv.device; snippet = "${dv.kind}${if (dv.identityName.isNotEmpty()) " · ${dv.identityName}" else ""}\n±${dv.acc.toInt()} m · ${dv.source}${if (age.isNotEmpty()) " · $age ago" else ""}${if (dv.beacons > 0) "\nhears ${dv.beacons} beacons" else ""}" })
                }
                // anchors: ⌖ with the name, draggable (long-press the pin, then move), survey circle
                if (anchorsLayer) for (an in anchorsList) {
                    val gp = GeoPoint(an.lat, an.lon)
                    map.overlays.add(Polygon(map).apply { points = Polygon.pointsAsCircle(gp, an.accM.coerceIn(0.3, 500.0)); fillPaint.color = AColor.parseColor("#22B388FF"); outlinePaint.color = AColor.parseColor("#B388FF"); outlinePaint.strokeWidth = 1.5f })
                    val mk = anchorMarkers.getOrPut(an.id) { Marker(map).apply {
                        isDraggable = true; setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                        setOnMarkerDragListener(object : Marker.OnMarkerDragListener {
                            override fun onMarkerDragStart(marker: Marker) { dragging += an.id }
                            override fun onMarkerDrag(marker: Marker) {}
                            override fun onMarkerDragEnd(marker: Marker) { dragging -= an.id; anchorsVm.move(an.id, marker.position.latitude, marker.position.longitude) }
                        })
                        setOnMarkerClickListener { m, _ -> anchorsVm.anchors.value.firstOrNull { it.id == an.id }?.let { anchorsVm.edit(it) }; m.showInfoWindow(); true }
                    } }
                    if (an.id !in dragging) mk.position = gp
                    mk.icon = labelIcon(map, cache, "⌖", an.name + (if (an.rv) " ·RV" else ""), AColor.parseColor(if (an.ref) "#FFD166" else "#B388FF"), false, big = true)
                    mk.title = an.name; mk.snippet = "${kindName(an.kind)} · ±${an.accM} m · ${an.source}" + (if (an.bssids.isNotEmpty()) "\n${an.bssids.joinToString(" ")}" else "") + "\nlong-press to drag · tap to edit"
                    map.overlays.add(mk)
                }
                // measured ranges: a ring (or a point at the bearing) around the desktop's anchor / position, labelled with the distance
                for (rs in ranges.values) {
                    val b = rs.best ?: continue
                    val centre = rs.anchor?.let { GeoPoint(it.lat, it.lon) } ?: rs.desktop.let { d -> views.firstOrNull { it.desktop.id == d.id }?.location?.takeIf { it.valid }?.let { GeoPoint(it.lat, it.lon) } } ?: continue
                    val col = when (b.cls) { "adjacent" -> "#6CFF8A"; "room" -> "#35D6FF"; "near" -> "#FFD166"; else -> "#9FB0C8" }
                    map.overlays.add(Polygon(map).apply { points = Polygon.pointsAsCircle(centre, b.distanceM.coerceAtLeast(0.2)); fillPaint.color = AColor.TRANSPARENT; outlinePaint.color = AColor.parseColor(col); outlinePaint.strokeWidth = 3f; outlinePaint.pathEffect = android.graphics.DashPathEffect(floatArrayOf(10f, 8f), 0f); title = rs.line })
                    if (b.sigmaM > 0.05) { map.overlays.add(Polygon(map).apply { points = Polygon.pointsAsCircle(centre, (b.distanceM + b.sigmaM).coerceAtLeast(0.3)); fillPaint.color = AColor.parseColor("#14" + col.drop(1)); outlinePaint.color = AColor.TRANSPARENT }) }
                    rs.rangedFix?.let { rf -> map.overlays.add(Polyline(map).apply { setPoints(listOf(centre, GeoPoint(rf.lat, rf.lon))); outlinePaint.color = AColor.parseColor(col); outlinePaint.strokeWidth = 4f; title = rs.line }) }
                    map.overlays.add(Marker(map).apply { position = centre; setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER); icon = labelIcon(map, cache, "💻", "${rs.name} · ${org.sworrl.beaconfix.ranging.RangeSession.fmtM(b.distanceM)} ±${org.sworrl.beaconfix.ranging.RangeSession.fmtM(b.sigmaM)}", AColor.parseColor(col), false, big = true); title = rs.name; snippet = rs.line + "\n" + b.cls + " · " + b.method.joinToString("+") })
                }
                latest?.let { map.overlays.add(Marker(map).apply { position = GeoPoint(it.lat, it.lon); setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER); icon = labelIcon(map, cache, "⌖", if (it.source == "phone-range") "you (ranged)" else "you", AColor.WHITE, true, big = true); title = "You (${it.source})"; snippet = "±${it.acc.toInt()} m" }) }
                map.invalidate()
            },
        )
        Column(Modifier.align(Alignment.TopStart).padding(8.dp)) {
            Surface(tonalElevation = 3.dp, shape = MaterialTheme.shapes.small) { Text("  ${aps.size} beacons placed · ${pois.size} places · gold = mapped · cyan = fitted here · red = insecure · magenta = home  ", style = MaterialTheme.typography.labelSmall) }
            Row { FilterChip(selected = labels, onClick = { labels = !labels }, label = { Text("Aa") }); FilterChip(selected = places, onClick = { places = !places }, label = { Text("Places") }, modifier = Modifier.padding(start = 6.dp)); FilterChip(selected = devicesLayer, onClick = { devicesLayer = !devicesLayer }, label = { Text("Devices") }, modifier = Modifier.padding(start = 6.dp)); FilterChip(selected = layer == "sat", onClick = { layer = if (layer == "sat") "map" else "sat" }, label = { Text("Sat") }, modifier = Modifier.padding(start = 6.dp)); FilterChip(selected = anchorsLayer, onClick = { anchorsLayer = !anchorsLayer }, label = { Text("⌖") }, modifier = Modifier.padding(start = 6.dp)) }
        }
        RefitOverlay(mapRef, live.refits.events, onTicker = { ticker = it })
        // below map resolution (the ring would be a few pixels): a proximity inset drawn to scale
        val near = ranges.values.mapNotNull { rs -> rs.best?.let { rs to it } }.minByOrNull { it.second.distanceM }
        val mpp = mapRef?.let { m -> org.osmdroid.util.TileSystem.GroundResolution(m.mapCenter.latitude, m.zoomLevelDouble) } ?: 1.0
        if (near != null && near.second.distanceM / mpp < 60) ProximityInset(near.first, near.second, Modifier.align(Alignment.TopEnd).padding(top = 64.dp, end = 8.dp))
        editing?.let { AnchorEditorSheet(it, anchorsVm) }
        if (ticker.isNotEmpty()) Surface(Modifier.align(Alignment.BottomStart).padding(16.dp), tonalElevation = 3.dp, shape = MaterialTheme.shapes.small) { Text("  $ticker  ", style = MaterialTheme.typography.labelSmall) }
        Column(Modifier.align(Alignment.BottomEnd).padding(16.dp)) {
            SmallFloatingActionButton(onClick = { menu = true }, containerColor = MaterialTheme.colorScheme.surfaceVariant) { Text("⋯") }
            androidx.compose.material3.DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                androidx.compose.material3.DropdownMenuItem(text = { Text("Replay last refit animation") }, onClick = { menu = false; live.replayLastRefit() })
                androidx.compose.material3.DropdownMenuItem(text = { Text(if (follow) "Stop following" else "Follow my position") }, onClick = { menu = false; follow = !follow })
                androidx.compose.material3.DropdownMenuItem(text = { Text(if (labels) "Hide names" else "Show names") }, onClick = { menu = false; labels = !labels })
                androidx.compose.material3.DropdownMenuItem(text = { Text("Place an antenna at the map centre") }, onClick = { menu = false; mapRef?.let { m -> anchorsVm.newAt(m.mapCenter.latitude, m.mapCenter.longitude) } })
                androidx.compose.material3.DropdownMenuItem(text = { Text("Place an antenna at my position (GNSS average)") }, onClick = { menu = false; anchorsVm.newAt(0.0, 0.0, 1.0, "gps-average"); anchorsVm.startAveraging() })
            }
            SmallFloatingActionButton(onClick = { follow = !follow }, modifier = Modifier.padding(top = 8.dp), containerColor = if (follow) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant) { Text("◎") }
        }
        if (aps.isEmpty() && latest == null) Box(Modifier.align(Alignment.Center)) { EmptyState("🗺", "Nothing to show yet", "Turn the collector on for your own beacons, or pair a desktop to see its map.") }
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

/** Esri World Imagery, as on the desktop and in the Plasma widget (z/y/x order); osmdroid scales the deepest level for zooms past 19. */
private val ESRI_IMAGERY = object : org.osmdroid.tileprovider.tilesource.OnlineTileSourceBase("EsriWorldImagery", 0, 19, 256, "",
    arrayOf("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/"), "Imagery © Esri, Maxar, Earthstar Geographics") {
    override fun getTileURLString(pMapTileIndex: Long): String = baseUrl + org.osmdroid.util.MapTileIndex.getZoom(pMapTileIndex) + "/" + org.osmdroid.util.MapTileIndex.getY(pMapTileIndex) + "/" + org.osmdroid.util.MapTileIndex.getX(pMapTileIndex)
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
