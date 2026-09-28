package org.sworrl.beaconfix.poi

import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.db.PoiEntity
import java.util.Locale
import kotlin.math.cos

/**
 * The phone's two Overpass queries and what it keeps from the answers.
 *
 * - [help]: police, fire, hospitals, clinics, urgent care, pharmacies and vets in a 25 km box (the desktop's "wide"
 *   radius for these categories). Kept: the nearest 15 per category within 25 km, scope `near`.
 * - [pediatric]: the desktop's children's ER query (`Locator::queryPediatric`, 3.8): every hospital in the
 *   `pedsRadiusKm` box (classified here, which also yields the general ERs and a children's hospital's campus ER) plus
 *   pediatric clinics / urgent care in a 50 km box. Only exact-tag lookups: value regexes and `around:` over a 300 km
 *   box ran into the server's timeout. Kept: the nearest 5 confirmed pediatric ERs within the radius plus the nearest 5
 *   other pediatric sites, 5 pediatric urgent care within 50 km and 8 general ERs within 80 km, scope `far`.
 */
object HelpQuery {
    const val HELP_RADIUS_M = 25_000
    const val URGENT_RADIUS_M = 50_000
    const val ER_RADIUS_M = 80_000
    const val HELP_TIMEOUT_S = 25
    const val PEDS_TIMEOUT_S = 60
    const val KEEP_PER_CAT = 15
    const val KEEP_PEDS_ER = 5
    const val KEEP_PEDS_URGENT = 5
    const val KEEP_ER = 8

    /** "south,west,north,east" around a point, 5 decimals (the desktop's bboxFor). */
    fun bbox(lat: Double, lon: Double, radiusM: Int): String {
        val dLat = radiusM / 111320.0
        val dLon = radiusM / (111320.0 * cos(Math.toRadians(lat)))
        return String.format(Locale.US, "%.5f,%.5f,%.5f,%.5f", lat - dLat, lon - dLon, lat + dLat, lon + dLon)
    }

    fun help(bbox: String): String =
        "[out:json][timeout:$HELP_TIMEOUT_S];(" +
            "nwr[amenity~\"^(police|fire_station|hospital|clinic|urgent_care|pharmacy|veterinary)$\"]($bbox);" +
            "nwr[healthcare~\"^(hospital|clinic|urgent_care|pharmacy)$\"]($bbox);" +
            ");out center tags qt 1500;"

    fun pediatric(far: String, urg: String): String {
        val kid = "[name~\"pa?ediatric|kids|child\",i]"
        val spec = "[\"healthcare:speciality\"~\"pa?ediatric\",i]"
        return "[out:json][timeout:$PEDS_TIMEOUT_S];(" +
            "nwr[amenity=hospital]($far);nwr[healthcare=hospital]($far);" +
            "nwr[building=hospital][name~\"child|pa?ediatric\",i]($far);nwr[\"emergency:paediatric\"=yes]($far);" +
            "nwr[amenity=clinic]$kid($urg);nwr[amenity=doctors]$kid($urg);nwr[amenity=urgent_care]$kid($urg);" +
            "nwr[healthcare=clinic]$kid($urg);nwr[healthcare=urgent_care]$kid($urg);" +
            "nwr[healthcare=clinic]$spec($urg);nwr[healthcare=urgent_care]$spec($urg);nwr[healthcare=doctor]$spec($urg);" +
            ");out center tags qt;"
    }

    /** One element with its classification and distance from the search origin. */
    data class Hit(val el: OsmElement, val r: PedsClassifier.Result, val distM: Double)

    private fun hits(els: List<OsmElement>, res: List<PedsClassifier.Result>, lat: Double, lon: Double): List<Hit> =
        els.indices.mapNotNull { i ->
            val e = els[i]; val r = res[i]
            if (r.dropped || r.cat !in HelpCategories.KEYS || (e.lat == 0.0 && e.lon == 0.0)) null
            else Hit(e, r, PedsClassifier.distanceM(lat, lon, e.lat, e.lon))
        }

    /** From the help answer: the nearest [KEEP_PER_CAT] per help category within [HELP_RADIUS_M]. */
    fun selectHelp(els: List<OsmElement>, res: List<PedsClassifier.Result>, lat: Double, lon: Double): List<Hit> =
        hits(els, res, lat, lon).filter { it.distM <= HELP_RADIUS_M }
            .groupBy { it.r.cat }.values.flatMap { l -> l.sortedBy { it.distM }.take(KEEP_PER_CAT) }

    /**
     * From the pediatric answer: pediatric ERs within [radiusM] (the nearest [KEEP_PEDS_ER] tier-1 sites first, then the
     * nearest [KEEP_PEDS_ER] others: [PedsClassifier.keepPediatricEr]), pediatric urgent care within 50 km, general ERs
     * within 80 km.
     */
    fun selectPediatric(els: List<OsmElement>, res: List<PedsClassifier.Result>, lat: Double, lon: Double, radiusM: Int): List<Hit> {
        val all = hits(els, res, lat, lon)
        fun nearest(l: List<Hit>, n: Int) = l.sortedBy { it.distM }.take(n)
        return PedsClassifier.keepPediatricEr(all.filter { it.r.cat == "peds_er" && it.distM <= radiusM }, KEEP_PEDS_ER, { it.r.peds }, { it.distM }) +
            nearest(all.filter { it.r.cat == "peds_urgent" && it.distM <= URGENT_RADIUS_M }, KEEP_PEDS_URGENT) +
            nearest(all.filter { it.r.cat == "health" && it.r.emergency && it.distM <= ER_RADIUS_M }, KEEP_ER)
    }

    private fun hcLabel(t: Map<String, String>): String {
        val hc = t["healthcare"].orEmpty(); val am = t["amenity"].orEmpty(); val sp = t["healthcare:speciality"].orEmpty()
        return when {
            hc == "urgent_care" || am == "urgent_care" -> "urgent care"
            am == "doctors" || hc == "doctor" -> if (sp.isEmpty()) "doctor's office" else sp.replace('_', ' ')
            am == "clinic" || hc == "clinic" -> "clinic"
            else -> ""
        }
    }

    /** A cached place row for a hit (the desktop's makePoi for the help categories), source [DesktopCache.PHONE]. */
    fun toEntity(h: Hit, scope: String, originLat: Double, originLon: Double, now: Long): PoiEntity {
        val t = h.el.tags; val r = h.r; val cat = r.cat
        val c = HelpCategories.of(cat)
        val name = t["name"].orEmpty().ifEmpty { t["brand"].orEmpty() }.ifEmpty { t["operator"].orEmpty() }
        val ia = t["internet_access"].orEmpty()
        val wifi = ia == "wlan" || ia == "yes"
        val wheelchair = t["wheelchair"].orEmpty()
        val d = ArrayList<String>()
        if (r.detail.isNotEmpty()) d += r.detail                     // the ER confidence first
        val brand = t["brand"].orEmpty()
        if (brand.isNotEmpty() && brand != name) d += brand
        if ((cat == "urgent" || cat == "peds_urgent") && hcLabel(t).isNotEmpty()) d += hcLabel(t)
        if (wheelchair.isNotEmpty() && wheelchair != "no") d += if (wheelchair == "yes") "♿" else "♿ limited"
        if (wifi) d += "Wi-Fi"
        val (driveS, driveM) = if (HelpCategories.driveCat(cat)) PedsClassifier.driveEstimate(h.distM) else 0 to 0
        return PoiEntity(
            source = DesktopCache.PHONE, key = h.el.key, scope = DesktopCache.scopeOf(scope),
            cat = cat, label = c?.label ?: cat, grp = c?.group ?: HelpCategories.GROUP, icon = c?.icon.orEmpty(), color = c?.color.orEmpty(),
            name = name, detail = d.joinToString(" · "), address = r.address, lat = h.el.lat, lon = h.el.lon,
            phone = r.phone, hours = t["opening_hours"].orEmpty(), website = t["website"].orEmpty().ifEmpty { t["contact:website"].orEmpty() },
            wheelchair = wheelchair, emergency = r.emergency, wifi = wifi, peds = r.peds, er = r.er, campus = r.campus,
            driveS = driveS, driveM = driveM, driveEst = true,
            fetchedAt = now, originLat = originLat, originLon = originLon,
        )
    }
}
