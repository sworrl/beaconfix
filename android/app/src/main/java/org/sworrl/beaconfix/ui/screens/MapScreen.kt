package org.sworrl.beaconfix.ui.screens

import android.graphics.Color as AColor
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
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
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.ui.grade
import org.sworrl.beaconfix.ui.vm.MapViewModel

@Composable
fun MapScreen(vm: MapViewModel = hiltViewModel()) {
    val aps by vm.aps.collectAsState()
    val phone by vm.phoneTrack.collectAsState()
    val desk by vm.desktopTrack.collectAsState()
    val latest by vm.latest.collectAsState()
    val centred = remember { booleanArrayOf(false) }
    Box(Modifier.fillMaxSize()) {
        AndroidView(
            modifier = Modifier.fillMaxSize().semantics { contentDescription = "Map of beacons and positions" },
            factory = { ctx ->
                MapView(ctx).apply {
                    setTileSource(TileSourceFactory.MAPNIK)
                    setMultiTouchControls(true)
                    controller.setZoom(15.0)
                    latest?.let { controller.setCenter(GeoPoint(it.lat, it.lon)) }
                }
            },
            update = { map ->
                map.overlays.clear()
                if (!centred[0] && latest != null) { map.controller.setCenter(GeoPoint(latest!!.lat, latest!!.lon)); centred[0] = true }
                // desktop track (gold) and phone track (cyan)
                if (desk.size > 1) map.overlays.add(Polyline(map).apply { setPoints(desk.map { GeoPoint(it.lat, it.lon) }); outlinePaint.color = AColor.parseColor("#FFD166"); outlinePaint.strokeWidth = 5f })
                val pt = phone.filter { it.source.startsWith("phone") }
                if (pt.size > 1) map.overlays.add(Polyline(map).apply { setPoints(pt.map { GeoPoint(it.lat, it.lon) }); outlinePaint.color = AColor.parseColor("#35D6FF"); outlinePaint.strokeWidth = 5f })
                for (a in aps) {
                    val p = GeoPoint(a.lat!!, a.lon!!)
                    val g = grade(a.security)
                    val col = if (a.home) "#FF4FD8" else when (a.posSource) { "observed" -> "#35D6FF"; "placed" -> "#FFD166"; else -> "#9FB0C8" }
                    map.overlays.add(Polygon(map).apply {
                        points = Polygon.pointsAsCircle(p, (a.acc ?: 50.0).coerceIn(5.0, 1500.0))
                        fillPaint.color = AColor.parseColor("#22" + col.drop(1)); outlinePaint.color = AColor.parseColor(col); outlinePaint.strokeWidth = 1.5f
                    })
                    map.overlays.add(Marker(map).apply {
                        position = p; setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                        title = a.ssid.ifEmpty { "(hidden)" }
                        snippet = "${a.bssid} · ${a.band} GHz ch ${a.ch}\n±${(a.acc ?: 0.0).toInt()} m (${a.posSource})${if (a.residual != null) " · fit ${a.residual.toInt()} m" else ""}\n${g.label}: ${g.why}"
                        if (g.label == "OPEN" || g.label == "WEP" || g.label.startsWith("WPA1") || g.label.contains("TKIP")) alpha = 1f
                    })
                }
                latest?.let { map.overlays.add(Marker(map).apply { position = GeoPoint(it.lat, it.lon); title = "You (${it.source})"; snippet = "±${it.acc.toInt()} m" }) }
                map.invalidate()
            },
        )
        Surface(Modifier.align(Alignment.TopStart).padding(12.dp), tonalElevation = 3.dp, shape = MaterialTheme.shapes.small) {
            Text("  ${aps.size} beacons placed · gold = desktop/placed · cyan = fitted here · magenta = home  ", style = MaterialTheme.typography.labelSmall)
        }
        if (aps.isEmpty() && latest == null) Text("Nothing to show yet. Pair a desktop or turn the collector on.", Modifier.align(Alignment.Center).padding(24.dp))
        @Suppress("UNUSED_VARIABLE") val cd = stringResource(R.string.cd_map)
    }
}
