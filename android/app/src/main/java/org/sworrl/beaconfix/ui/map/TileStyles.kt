package org.sworrl.beaconfix.ui.map

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
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
import org.osmdroid.tileprovider.tilesource.TileSourcePolicy
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.tileprovider.tilesource.XYTileSource
import org.osmdroid.util.MapTileIndex
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Overlay
import org.osmdroid.views.overlay.TilesOverlay
import org.sworrl.beaconfix.R

/**
 * The map styles, all keyless and the same upstreams as the desktop (`src/tilesource.cpp`): Streets (OpenStreetMap),
 * Dark (OpenStreetMap with inverted colours), Topo (OpenTopoMap, tiles to zoom 17) and the satellite sources, each with
 * Esri's boundaries-and-places labels on top: Esri World Imagery (Vantor 30-50 cm, the newest high-resolution imagery
 * served free), Esri Clarity (sharper processing, can be older), USGS National Map (US public-domain orthoimagery, z16)
 * and NASA GIBS VIIRS (yesterday's global pass, ~375 m, z9: the newest, coarse). The choice is kept in `Prefs.mapStyle`.
 */
object TileStyles {
    const val STREETS = "streets"
    const val DARK = "dark"
    const val TOPO = "topo"
    const val SATELLITE = "satellite"
    const val SAT_CLARITY = "satellite_clarity"
    const val SAT_USGS = "satellite_usgs"
    const val SAT_VIIRS = "satellite_viirs"

    enum class Style(val key: String, @StringRes val label: Int) {
        STREETS_STYLE(STREETS, R.string.map_style_streets), DARK_STYLE(DARK, R.string.map_style_dark),
        TOPO_STYLE(TOPO, R.string.map_style_topo), SATELLITE_STYLE(SATELLITE, R.string.map_style_satellite),
        SAT_CLARITY_STYLE(SAT_CLARITY, R.string.map_style_sat_clarity), SAT_USGS_STYLE(SAT_USGS, R.string.map_style_sat_usgs),
        SAT_VIIRS_STYLE(SAT_VIIRS, R.string.map_style_sat_viirs),
    }

    /** Imagery styles: Esri's labels go on top, and the credit line is drawn light. */
    fun isSatellite(key: String?): Boolean = normalize(key).let { it == SATELLITE || it == SAT_CLARITY || it == SAT_USGS || it == SAT_VIIRS }

    val KEYS = Style.entries.map { it.key }

    /** An unknown or empty stored value means [SATELLITE] (the hybrid: imagery, roads, place labels). */
    fun normalize(key: String?): String = if (key != null && key in KEYS) key else SATELLITE

    fun label(key: String?): Int = Style.entries.first { it.key == normalize(key) }.label

    /**
     * OpenTopoMap: tiles exist to zoom 17; osmdroid scales the deepest level past that. A small volunteer-run server, so
     * the same policy osmdroid gives OSM's own tiles: no bulk download, no preventive loading around the viewport, two
     * connections. The credit line is the one OpenTopoMap asks for (map data, SRTM, and the CC-BY-SA style).
     */
    val OPEN_TOPO: OnlineTileSourceBase = XYTileSource("OpenTopoMap", 0, 17, 256, ".png", arrayOf("https://tile.opentopomap.org/"),
        "Map data: © OpenStreetMap contributors, SRTM · Map style: © OpenTopoMap (CC-BY-SA)",
        TileSourcePolicy(2, TileSourcePolicy.FLAG_NO_BULK or TileSourcePolicy.FLAG_NO_PREVENTIVE or TileSourcePolicy.FLAG_USER_AGENT_MEANINGFUL or TileSourcePolicy.FLAG_USER_AGENT_NORMALIZED))

    /** Esri World Imagery (z/y/x order), as on the desktop and in the Plasma widget; Esri's attribution text for it. */
    val ESRI_IMAGERY: OnlineTileSourceBase = EsriSource("EsriWorldImagery", "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/",
        "Powered by Esri · Source: Esri, Vantor, Earthstar Geographics, and the GIS User Community")

    /** Esri World Imagery (Clarity): the same imagery, sharper-processed, sometimes an older capture. */
    val ESRI_CLARITY: OnlineTileSourceBase = EsriSource("EsriWorldImageryClarity", "https://clarity.maptiles.arcgis.com/arcgis/rest/services/World_Imagery/MapServer/tile/",
        "Powered by Esri · Source: Esri, Vantor, Earthstar Geographics, and the GIS User Community")

    /** USGS The National Map orthoimagery (USDA NAIP and others): US only, public domain, tiles to zoom 16. */
    val USGS_IMAGERY: OnlineTileSourceBase = EsriSource("UsgsImageryOnly", "https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/",
        "Imagery: USGS The National Map (public domain)", maxZoom = 16)

    /** NASA GIBS: VIIRS (NOAA-20) corrected-reflectance true colour from yesterday's pass (UTC; today's is incomplete). */
    val NASA_VIIRS: OnlineTileSourceBase = object : OnlineTileSourceBase("NasaGibsViirs", 0, 9, 256, ".jpg",
        arrayOf("https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/VIIRS_NOAA20_CorrectedReflectance_TrueColor/default/"),
        "Imagery: NASA EOSDIS GIBS · VIIRS NOAA-20") {
        override fun getTileURLString(pMapTileIndex: Long): String {
            val day = java.time.LocalDate.now(java.time.ZoneOffset.UTC).minusDays(1)
            return baseUrl + day + "/GoogleMapsCompatible_Level9/" + MapTileIndex.getZoom(pMapTileIndex) + "/" +
                MapTileIndex.getY(pMapTileIndex) + "/" + MapTileIndex.getX(pMapTileIndex) + ".jpg"
        }
    }

    /** Esri's transparent roads with their names, drawn over the imagery: the satellite styles are a hybrid. */
    val ESRI_ROADS: OnlineTileSourceBase = EsriSource("EsriTransportation", "https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Transportation/MapServer/tile/",
        "Roads © Esri, HERE, Garmin")

    /** Esri's transparent boundaries-and-places labels, drawn over the imagery (covered by the imagery's credit line). */
    val ESRI_LABELS: OnlineTileSourceBase = EsriSource("EsriBoundariesPlaces", "https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/",
        "Labels © Esri")

    /** Credit for the satellite hybrid's road and place-label overlays (Esri's services' copyrightText). */
    const val HYBRID_CREDIT = "Roads & labels: Esri, HERE, Garmin, © OpenStreetMap contributors"

    /** Credit for the road-snapped routes (the FOSSGIS OSRM server's terms ask for it), shown while they are drawn. */
    const val ROUTING_CREDIT = "Routes: OSRM"

    /** Credit for the camera layer's data (DeFlock's OSM-derived ALPRs, ODbL; flocklocations.com community reports, CC BY 4.0), shown while it is drawn. */
    const val CAMERA_CREDIT = "Cameras: DeFlock / © OpenStreetMap contributors (ODbL) · flocklocations.com (CC BY 4.0)"

    /** Styles whose tiles are mostly dark: the credit line is drawn light on them. */
    fun isDark(key: String?): Boolean = normalize(key) == DARK || isSatellite(key)

    /**
     * Credit [parts] laid out in lines no wider than [maxWidth]: each part is split at " · " and the pieces are packed
     * greedily, so a long credit wraps onto a second line instead of running off the screen.
     */
    fun wrapCredits(parts: List<String>, maxWidth: Float, measure: (String) -> Float): List<String> {
        val lines = mutableListOf<String>()
        var cur = ""
        for (piece in parts.flatMap { it.split(" · ") }.map { it.trim() }.filter { it.isNotEmpty() }) {
            val joined = if (cur.isEmpty()) piece else "$cur · $piece"
            if (cur.isNotEmpty() && measure(joined) > maxWidth) { lines += cur; cur = piece } else cur = joined
        }
        if (cur.isNotEmpty()) lines += cur
        return lines
    }

    fun source(key: String?): OnlineTileSourceBase = when (normalize(key)) {
        TOPO -> OPEN_TOPO
        SATELLITE -> ESRI_IMAGERY
        SAT_CLARITY -> ESRI_CLARITY
        SAT_USGS -> USGS_IMAGERY
        SAT_VIIRS -> NASA_VIIRS
        else -> TileSourceFactory.MAPNIK
    }

    private class EsriSource(name: String, base: String, copyright: String, maxZoom: Int = 19) : OnlineTileSourceBase(name, 0, maxZoom, 256, "", arrayOf(base), copyright) {
        override fun getTileURLString(pMapTileIndex: Long): String =
            baseUrl + MapTileIndex.getZoom(pMapTileIndex) + "/" + MapTileIndex.getY(pMapTileIndex) + "/" + MapTileIndex.getX(pMapTileIndex)
    }
}

/**
 * Applies a style to a MapView: sets the tile source (only when it changes), the dark colour filter, and, for
 * Satellite, puts the labels layer at the bottom of the overlay list (under every marker); any other style takes it
 * out again. Idempotent, so MapScreen calls it whenever the style changes. Owns the labels layer's tile provider;
 * [detach] releases it.
 */
class TileStyler {
    private var overlays: List<TilesOverlay> = emptyList()       // roads, then place labels
    private var overlaysMap: MapView? = null

    fun apply(map: MapView, key: String?) {
        val k = TileStyles.normalize(key)
        val src = TileStyles.source(k)
        if (map.tileProvider.tileSource.name() != src.name()) map.setTileSource(src)
        map.overlayManager.tilesOverlay.setColorFilter(if (k == TileStyles.DARK) TilesOverlay.INVERT_COLORS else null)
        if (TileStyles.isSatellite(k)) overlaysFor(map).forEachIndexed { i, o -> if (!map.overlays.contains(o)) map.overlays.add(i, o) }
        else overlays.forEach { map.overlays.remove(it) }
        map.invalidate()
    }

    private fun overlaysFor(map: MapView): List<TilesOverlay> {
        if (overlays.isNotEmpty() && overlaysMap === map) return overlays
        detach()
        overlays = listOf(TileStyles.ESRI_ROADS, TileStyles.ESRI_LABELS).map { src ->
            val provider = MapTileProviderBasic(map.context.applicationContext, src)
            provider.tileRequestCompleteHandlers.add(map.tileRequestCompleteHandler)
            TilesOverlay(provider, map.context).apply { loadingBackgroundColor = Color.TRANSPARENT; loadingLineColor = Color.TRANSPARENT }
        }
        overlaysMap = map
        return overlays
    }

    /** Stops the overlays' tile threads (MapScreen leaves the composition). Safe to call more than once. */
    fun detach() {
        val l = overlays.ifEmpty { return }
        overlays = emptyList()
        l.forEach { runCatching { it.onDetach(overlaysMap) } }
        overlaysMap = null
    }
}

/**
 * The map's credit line, bottom left, drawn last so no marker covers it: the tile source's own notice (OSM, OpenTopoMap
 * or Esri), plus [TileStyles.ROUTING_CREDIT] while road-snapped routes are drawn ([routing]) and [TileStyles.CAMERA_CREDIT]
 * while surveillance cameras are ([cameras]). It wraps onto a second line
 * rather than running off a narrow screen, and sits on a translucent plate in a colour that reads on the current
 * [style] (OSM's tile policy: the attribution must be legible, not hidden).
 */
class MapAttribution(ctx: Context) : Overlay() {
    var style: String? = null
    var routing = false
    var cameras = false
    private val density = ctx.resources.displayMetrics.density
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 10 * density }
    private val plate = Paint(Paint.ANTI_ALIAS_FLAG)

    override fun draw(c: Canvas, map: MapView, shadow: Boolean) {
        if (shadow) return
        val notice = map.tileProvider.tileSource.copyrightNotice.orEmpty()
        val pad = 6 * density
        val hybrid = if (TileStyles.isSatellite(style)) TileStyles.HYBRID_CREDIT else null
        val lines = TileStyles.wrapCredits(listOfNotNull(notice, hybrid, if (routing) TileStyles.ROUTING_CREDIT else null, if (cameras) TileStyles.CAMERA_CREDIT else null), c.width - 4 * pad) { text.measureText(it) }
        if (lines.isEmpty()) return
        val dark = TileStyles.isDark(style)
        text.color = if (dark) Color.WHITE else Color.rgb(0x22, 0x22, 0x22)
        plate.color = if (dark) Color.argb(140, 0, 0, 0) else Color.argb(170, 255, 255, 255)
        val lh = text.textSize * 1.25f
        val w = lines.maxOf { text.measureText(it) }
        val bottom = c.height - pad
        val top = bottom - lh * lines.size - pad / 2
        val pj = map.projection
        pj.save(c, false, false)
        c.drawRoundRect(pad, top, pad + w + pad, bottom, 3 * density, 3 * density, plate)
        lines.forEachIndexed { i, l -> c.drawText(l, pad * 1.5f, top + lh * (i + 1) - text.descent() / 2, text) }
        pj.restore(c, false)
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
