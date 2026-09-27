package org.sworrl.beaconfix.ui.map

import android.graphics.Color
import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Box
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import org.osmdroid.tileprovider.MapTileProviderBasic
import org.osmdroid.tileprovider.tilesource.OnlineTileSourceBase
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.tileprovider.tilesource.XYTileSource
import org.osmdroid.util.MapTileIndex
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.TilesOverlay
import org.sworrl.beaconfix.R

/**
 * The map styles, all keyless and the same upstreams as the desktop (`src/tilesource.cpp`): Streets (OpenStreetMap),
 * Dark (OpenStreetMap with inverted colours), Topo (OpenTopoMap, tiles to zoom 17) and Satellite (Esri World Imagery
 * with Esri's boundaries-and-places labels on top). The choice is kept in `Prefs.mapStyle`.
 */
object TileStyles {
    const val STREETS = "streets"
    const val DARK = "dark"
    const val TOPO = "topo"
    const val SATELLITE = "satellite"

    enum class Style(val key: String, @StringRes val label: Int) {
        STREETS_STYLE(STREETS, R.string.map_style_streets), DARK_STYLE(DARK, R.string.map_style_dark),
        TOPO_STYLE(TOPO, R.string.map_style_topo), SATELLITE_STYLE(SATELLITE, R.string.map_style_satellite),
    }

    val KEYS = Style.entries.map { it.key }

    /** An unknown or empty stored value means [STREETS]. */
    fun normalize(key: String?): String = if (key != null && key in KEYS) key else STREETS

    fun label(key: String?): Int = Style.entries.first { it.key == normalize(key) }.label

    /** OpenTopoMap: tiles exist to zoom 17; osmdroid scales the deepest level past that. */
    val OPEN_TOPO: OnlineTileSourceBase = XYTileSource("OpenTopoMap", 0, 17, 256, ".png", arrayOf("https://tile.opentopomap.org/"),
        "Map data © OpenStreetMap contributors, SRTM · style © OpenTopoMap (CC-BY-SA)")

    /** Esri World Imagery (z/y/x order), as on the desktop and in the Plasma widget. */
    val ESRI_IMAGERY: OnlineTileSourceBase = EsriSource("EsriWorldImagery", "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/",
        "Imagery © Esri, Maxar, Earthstar Geographics")

    /** Esri's transparent boundaries-and-places labels, drawn over the imagery. */
    val ESRI_LABELS: OnlineTileSourceBase = EsriSource("EsriBoundariesPlaces", "https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/",
        "Labels © Esri")

    fun source(key: String?): OnlineTileSourceBase = when (normalize(key)) {
        TOPO -> OPEN_TOPO
        SATELLITE -> ESRI_IMAGERY
        else -> TileSourceFactory.MAPNIK
    }

    private class EsriSource(name: String, base: String, copyright: String) : OnlineTileSourceBase(name, 0, 19, 256, "", arrayOf(base), copyright) {
        override fun getTileURLString(pMapTileIndex: Long): String =
            baseUrl + MapTileIndex.getZoom(pMapTileIndex) + "/" + MapTileIndex.getY(pMapTileIndex) + "/" + MapTileIndex.getX(pMapTileIndex)
    }
}

/**
 * Applies a style to a MapView on every redraw of MapScreen: sets the tile source (only when it changes), the dark
 * colour filter, and, for Satellite, adds the labels layer (call it right after `map.overlays.clear()`, so the labels
 * sit under every marker). Owns the labels layer's tile provider; [detach] releases it.
 */
class TileStyler {
    private var labels: TilesOverlay? = null
    private var labelsMap: MapView? = null

    fun apply(map: MapView, key: String?) {
        val k = TileStyles.normalize(key)
        val src = TileStyles.source(k)
        if (map.tileProvider.tileSource.name() != src.name()) map.setTileSource(src)
        map.overlayManager.tilesOverlay.setColorFilter(if (k == TileStyles.DARK) TilesOverlay.INVERT_COLORS else null)
        if (k == TileStyles.SATELLITE) map.overlays.add(labelsFor(map))
    }

    private fun labelsFor(map: MapView): TilesOverlay {
        labels?.let { if (labelsMap === map) return it }
        detach()
        val provider = MapTileProviderBasic(map.context.applicationContext, TileStyles.ESRI_LABELS)
        provider.tileRequestCompleteHandlers.add(map.tileRequestCompleteHandler)
        return TilesOverlay(provider, map.context).apply { loadingBackgroundColor = Color.TRANSPARENT; loadingLineColor = Color.TRANSPARENT }
            .also { labels = it; labelsMap = map }
    }

    /** Stops the labels layer's tile threads (MapScreen leaves the composition). Safe to call more than once. */
    fun detach() {
        val l = labels ?: return
        labels = null
        runCatching { l.onDetach(labelsMap) }
        labelsMap = null
    }
}

/** A chip naming the current style; tapping it offers Streets / Dark / Topo / Satellite. */
@Composable
fun MapStyleChip(style: String?, onPick: (String) -> Unit, modifier: Modifier = Modifier) {
    var open by remember { mutableStateOf(false) }
    val current = stringResource(TileStyles.label(style))
    val cd = stringResource(R.string.map_style_cd, current)
    Box(modifier) {
        FilterChip(selected = TileStyles.normalize(style) != TileStyles.STREETS, onClick = { open = true }, label = { Text(current) },
            modifier = Modifier.semantics { contentDescription = cd })
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (s in TileStyles.Style.entries) {
                val sel = TileStyles.normalize(style) == s.key
                DropdownMenuItem(text = { Text(stringResource(s.label)) }, leadingIcon = { RadioButton(selected = sel, onClick = null) },
                    onClick = { open = false; onPick(s.key) })
            }
        }
    }
}
