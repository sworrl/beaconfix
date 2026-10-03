package org.sworrl.beaconfix.ui.map

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.drawable.BitmapDrawable
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.Overlay
import org.osmdroid.views.overlay.Polygon
import org.sworrl.beaconfix.data.db.PoiEntity
import android.graphics.Color as AColor

/**
 * Cached places (every desktop and this phone, from `DesktopCache.pois()`, so they draw offline too), the RV's last
 * known position, and a pin for a shared place, drawn into MapScreen's places layer whenever the places, the labels
 * setting or the zoom band change.
 *
 * Draw order: everything else first, then police and fire, then ERs, then pediatric ERs last (on top), 1.3× larger
 * with a pink ring. Tapping a place calls `onTap` (MapScreen opens the place sheet).
 */
class PlacesOverlay {
    /** The RV (the desktop's last known fix) when no live desktop position is on the map. */
    data class RvPin(val lat: Double, val lon: Double, val acc: Double, val label: String)

    /** A pin that is not a cached place, e.g. a location shared from another app. */
    data class Pin(val lat: Double, val lon: Double, val label: String)

    private class Icon(val drawable: BitmapDrawable, val anchorU: Float)

    /** Rendered labels, bounded so a day of panning does not pile up bitmaps. */
    private val icons = object : LinkedHashMap<String, Icon>(64, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Icon>?) = size > 240
    }

    /** Adds the places, the RV and the shared pin to [into] (MapScreen passes its places layer; the map's own list by default). */
    fun draw(map: MapView, rows: List<PoiEntity>, labels: Boolean, rv: RvPin?, pin: Pin?, onTap: (PoiEntity) -> Unit, onPin: (Pin) -> Unit, into: MutableList<Overlay> = map.overlays) {
        val zoom = map.zoomLevelDouble
        for (p in drawOrder(rows)) {
            val peds = p.cat == PEDS_ER
            val name = p.name.ifEmpty { p.label }
            val showLabel = labels && (zoom >= 15 || (peds && zoom >= 11))
            into.add(Marker(map).apply {
                position = GeoPoint(p.lat, p.lon)
                val ic = icon(map, p.icon.ifEmpty { "📍" }, if (showLabel) name else "", colorOf(p), ring = peds)
                icon = ic.drawable; setAnchor(ic.anchorU, 0.5f)
                title = name
                setOnMarkerClickListener { _, _ -> onTap(p); true }
            })
        }
        rv?.let { r ->
            val gp = GeoPoint(r.lat, r.lon)
            if (r.acc > 0) into.add(Polygon(map).apply {
                points = Polygon.pointsAsCircle(gp, r.acc.coerceIn(5.0, 2000.0))
                fillPaint.color = AColor.parseColor("#1AFFD166"); outlinePaint.color = AColor.parseColor("#FFD166"); outlinePaint.strokeWidth = 1.2f
            })
            into.add(Marker(map).apply {
                position = gp
                val ic = icon(map, "🚐", r.label, AColor.parseColor("#FFD166"), ring = false, big = true)
                icon = ic.drawable; setAnchor(ic.anchorU, 0.5f)
                title = r.label
                setOnMarkerClickListener { _, _ -> true }
            })
        }
        pin?.let { s ->
            into.add(Marker(map).apply {
                position = GeoPoint(s.lat, s.lon)
                val ic = icon(map, "📍", s.label, AColor.parseColor("#35D6FF"), ring = false, big = true)
                icon = ic.drawable; setAnchor(ic.anchorU, 0.5f)
                title = s.label
                setOnMarkerClickListener { _, _ -> onPin(s); true }
            })
        }
    }

    /** Glyph (with an optional ring) and a label to its right; the glyph's centre is the anchor. */
    private fun icon(map: MapView, glyph: String, label: String, color: Int, ring: Boolean, big: Boolean = false): Icon {
        val key = "$glyph|$label|$color|$ring|$big"
        icons[key]?.let { return it }
        val d = map.resources.displayMetrics.density
        val scale = if (ring) PEDS_SCALE else 1f
        val gp = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = (if (big) 18 else 14) * d * scale }
        val lp = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 11 * d; typeface = Typeface.DEFAULT_BOLD }
        val gw = gp.measureText(glyph)
        val r = gw / 2 + 4 * d                                   // ring / halo radius around the glyph
        val lw = if (label.isNotEmpty()) lp.measureText(label) + 8 * d else 0f
        val h = (2 * r + 4 * d).coerceAtLeast(20 * d)
        val w = (2 * r + 2 * d + (if (lw > 0) lw + 4 * d else 0f)).toInt().coerceAtLeast(1)
        val bmp = Bitmap.createBitmap(w, h.toInt().coerceAtLeast(1), Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        val cx = r + d; val cy = h / 2
        val halo = Paint(Paint.ANTI_ALIAS_FLAG).apply { this.color = AColor.argb(170, 8, 13, 20) }
        c.drawCircle(cx, cy, r, halo)
        if (ring) {
            val rp = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 2.5f * d; this.color = AColor.parseColor(PEDS_PINK) }
            c.drawCircle(cx, cy, r - 1.5f * d, rp)
        }
        c.drawText(glyph, cx - gw / 2, cy + gp.textSize / 3, gp)
        if (label.isNotEmpty()) {
            val x = cx + r + 2 * d
            val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { this.color = AColor.argb(200, 8, 13, 20) }
            c.drawRoundRect(x, cy - 8 * d, x + lw, cy + 8 * d, 4 * d, 4 * d, bg)
            lp.color = color; c.drawText(label, x + 4 * d, cy + lp.textSize / 3, lp)
        }
        return Icon(BitmapDrawable(map.resources, bmp), cx / w).also { icons[key] = it }
    }

    companion object {
        const val PEDS_ER = "peds_er"
        /** C1: the pediatric ER colour. */
        const val PEDS_PINK = "#FF5FA2"
        const val PEDS_SCALE = 1.3f

        /** A general ER: a hospital with an emergency department (or tier 3, ER with pediatrics), not one marked "no ER". */
        fun isEr(p: PoiEntity) = p.cat == "health" && p.er != "no" && (p.emergency || p.er == "yes" || p.peds == 3)

        /** Higher draws later (on top): 3 pediatric ER, 2 ER, 1 police / fire, 0 everything else. */
        fun rank(p: PoiEntity): Int = when {
            p.cat == PEDS_ER -> 3
            isEr(p) -> 2
            p.cat == "police" || p.cat == "fire" -> 1
            else -> 0
        }

        /**
         * Jump to a spot: stop a running follow animation (it would pull the map back), then zoom and centre. Before
         * the map's first layout osmdroid replays queued moves in order, so this waits for that layout instead.
         */
        fun centreOn(map: MapView, lat: Double, lon: Double, zoom: Double) {
            val go = { map.controller.stopAnimation(false); map.controller.setZoom(zoom); map.controller.setCenter(GeoPoint(lat, lon)) }
            if (map.isLayoutOccurred) go() else map.addOnFirstLayoutListener { _, _, _, _, _ -> go() }
        }

        /** [rows] in draw order (stable within a rank). */
        fun drawOrder(rows: List<PoiEntity>): List<PoiEntity> = rows.sortedBy { rank(it) }

        fun colorOf(p: PoiEntity): Int {
            val hex = p.color.ifEmpty { if (p.cat == PEDS_ER) PEDS_PINK else "#9FB0C8" }
            return runCatching { AColor.parseColor(hex) }.getOrDefault(AColor.LTGRAY)
        }
    }
}
