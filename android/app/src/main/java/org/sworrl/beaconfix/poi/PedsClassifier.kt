package org.sworrl.beaconfix.poi

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.longOrNull
import java.util.Locale
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.roundToLong
import kotlin.math.sin
import kotlin.math.sqrt

/** One Overpass element: a node's position, or a way's / relation's center. Tags are OSM's, all strings. */
data class OsmElement(val type: String, val id: Long, val lat: Double, val lon: Double, val tags: Map<String, String>) {
    /** `way/329264979`, the same key the desktop sends and the cache stores. */
    val key: String get() = "$type/$id"

    companion object {
        /** From one entry of an Overpass `elements` array (`out center tags`); null when it has no position. */
        fun fromOverpass(el: JsonObject): OsmElement? {
            fun num(o: JsonObject?, k: String) = (o?.get(k) as? JsonPrimitive)?.doubleOrNull
            val center = el["center"] as? JsonObject
            val lat = num(el, "lat") ?: num(center, "lat") ?: return null
            val lon = num(el, "lon") ?: num(center, "lon") ?: return null
            val type = (el["type"] as? JsonPrimitive)?.contentOrNull ?: return null
            val id = (el["id"] as? JsonPrimitive)?.longOrNull ?: return null
            val tags = (el["tags"] as? JsonObject)?.mapNotNull { (k, v) -> (v as? JsonPrimitive)?.contentOrNull?.let { k to it } }?.toMap() ?: emptyMap()
            return OsmElement(type, id, lat, lon, tags)
        }

        fun fromOverpass(el: kotlinx.serialization.json.JsonElement): OsmElement? = runCatching { fromOverpass(el.jsonObject) }.getOrNull()
    }
}

/**
 * OpenStreetMap tags → BeaconFix place category, with the pediatric ER rules. A line-by-line port of the desktop's
 * `src/poiclassify.cpp` (plan C6); both are checked against the same fixture, `tests/fixtures/pediatric_tags.json`.
 *
 * Pediatric tiers ([Result.peds]): 0 none · 1 dedicated pediatric ER, confirmed · 2 children's hospital, ER not
 * confirmed · 3 general ER with a pediatrics department (stays in "health") · 4 pediatric urgent care (not an ER).
 *
 * The phone only asks Overpass for the help categories, so [baseCategory] keeps just the emergency / medical part of
 * the desktop's rules (police … vet, in the desktop's order); anything else comes back as "" (not a place we show).
 */
object PedsClassifier {
    data class Result(
        /** "" = not a place we show */
        val cat: String = "",
        val peds: Int = 0,
        /** "yes" | "no" | "" (unknown) */
        val er: String = "",
        /** tier 2: the ER hospital on the same campus (within 600 m) */
        val campus: String = "",
        /** the confidence text ("ER not confirmed — call ahead", "not an ER", …) */
        val detail: String = "",
        /** er == "yes", or a general hospital open 24/7 */
        val emergency: Boolean = false,
        /** amenity/healthcare=hospital with an emergency department (a campus candidate) */
        val hospitalEr: Boolean = false,
        /** phone || contact:phone */
        val phone: String = "",
        /** from addr:* */
        val address: String = "",
        /** a duplicate of a nearby object with the same name */
        val dropped: Boolean = false,
    )

    const val NOT_CONFIRMED = "ER not confirmed — call ahead"
    const val LIKELY_24 = "likely ER (24/7) — call ahead"
    const val CAMPUS_M = 600.0
    const val DEDUPE_M = 500.0

    private val PED_TOKENS = setOf("paediatrics", "pediatrics", "paediatric", "pediatric", "paediatrician", "pediatrician", "specialist_pediatrician")
    private val PED_NAME = Regex("""\b(children'?s?|child|pa?ediatric\w*|kids?)\b""")
    private val NOT_ER = Regex("""rehab|behavio|psychiat|hospice|home\b|dental|outpatient|specialty (care|center)|medical office|pavilion|therapy|surgery center|shriners""")
    private val URGENT = Regex("""urgent|express care|after.?hours|walk.?in|immediate care""")
    private val SPACES = Regex("""\s+""")

    private fun Map<String, String>.s(k: String) = this[k] ?: ""

    /** "123 Main St, Town, ST 12345" from addr:* tags. */
    fun address(t: Map<String, String>): String {
        val street = t.s("addr:street"); val hn = t.s("addr:housenumber"); val unit = t.s("addr:unit")
        var line1 = if (hn.isEmpty()) street else if (street.isEmpty()) hn else "$hn $street"
        if (unit.isNotEmpty() && line1.isNotEmpty()) line1 += " #$unit"
        val city = t.s("addr:city").ifEmpty { t.s("addr:town") }.ifEmpty { t.s("addr:village") }
        val state = t.s("addr:state").ifEmpty { t.s("addr:province") }
        val post = t.s("addr:postcode")
        val parts = ArrayList<String>()
        if (line1.isNotEmpty()) parts += line1
        if (city.isNotEmpty()) parts += city
        var tail = state; if (post.isNotEmpty()) tail += (if (tail.isEmpty()) "" else " ") + post
        if (tail.isNotEmpty()) parts += tail
        if (parts.isEmpty() && t.s("addr:full").isNotEmpty()) parts += t.s("addr:full")
        return parts.joinToString(", ")
    }

    /** "pediatric ER", "children's hospital — ER not confirmed", … ("" for 0). */
    fun tierLabel(peds: Int): String = when (peds) {
        1 -> "pediatric ER"
        2 -> "children's hospital — ER not confirmed"
        3 -> "general ER with a pediatrics dept."
        4 -> "pediatric urgent care — not an ER"
        else -> ""
    }

    /** The desktop's pre-pediatric rules for the help categories (healthcare=hospital alone is a hospital). */
    fun baseCategory(t: Map<String, String>): String {
        val am = t.s("amenity"); val shop = t.s("shop"); val hc = t.s("healthcare")
        return when {
            am == "police" -> "police"
            am == "fire_station" -> "fire"
            am == "hospital" || hc == "hospital" -> "health"
            am == "clinic" || am == "doctors" || am == "urgent_care" || hc == "urgent_care" || hc == "clinic" || hc == "doctor" -> "urgent"
            am == "pharmacy" || shop == "chemist" || hc == "pharmacy" -> "pharmacy"
            am == "dentist" || hc == "dentist" -> "dentist"
            am == "veterinary" -> "vet"
            else -> ""
        }
    }

    /** Category and pediatric tier of one object (no campus / dedupe pass). */
    fun classify(t: Map<String, String>): Result {
        val am = t.s("amenity"); val hc = t.s("healthcare"); val bld = t.s("building")
        val tokens = t.s("healthcare:speciality").lowercase(Locale.ROOT).split(';').map { it.trim() }.filter { it.isNotEmpty() }
        var pedSpec = t.s("health_specialty:paediatrics") == "yes" || t.s("emergency:paediatric") == "yes"
        var dental = am == "dentist" || hc == "dentist"
        for (tok in tokens) {
            if (tok in PED_TOKENS) pedSpec = true
            else if ((tok.startsWith("paediatric_") || tok.startsWith("pediatric_")) && !tok.contains("dent")) pedSpec = true
            if (tok.contains("dent")) dental = true
        }
        val nm = (t.s("name") + " " + t.s("alt_name") + " " + t.s("official_name")).lowercase(Locale.ROOT).replace('’', '\'')
        if (nm.contains("dent")) dental = true
        val pedName = PED_NAME.containsMatchIn(nm)
        val notEr = NOT_ER.containsMatchIn(nm)
        val isHosp = am == "hospital" || hc == "hospital"
        val erYes = t.s("emergency") == "yes" || t.s("emergency:paediatric") == "yes" ||
            "emergency" in tokens || "paediatric_emergency" in tokens || "pediatric_emergency" in tokens
        val erNo = t.s("emergency") == "no"
        val urgent = t.s("urgent_care") == "yes" || "urgent" in tokens || "urgent_care" in tokens || URGENT.containsMatchIn(nm)
        val general = "general" in tokens
        val h24 = t.s("opening_hours").trim() == "24/7"

        val base = Result(phone = t.s("phone").ifEmpty { t.s("contact:phone") }, address = address(t), hospitalEr = isHosp && erYes)
        // rule 5 for a hospital: the general ER view of it
        fun generalHospital(r: Result) = r.copy(cat = "health", er = if (erYes) "yes" else if (erNo) "no" else "")
        fun finish(r: Result): Result {
            val emergency = r.er == "yes" || (r.cat == "health" && h24 && r.er != "no")
            var detail = r.detail
            if (r.cat == "health" && detail.isEmpty()) {
                if (emergency) detail = "emergency dept."
                else if (r.er == "no") detail = "no ER"
            }
            return r.copy(emergency = emergency, detail = detail)
        }

        // 1. a hospital that is pediatric by speciality or by name (a general hospital with a pediatrics
        //    department is rule 2, unless its name says children's)
        if (isHosp && (pedSpec || pedName) && !(general && !pedName)) {
            if (erNo) return finish(generalHospital(base).copy(detail = "children's hospital · no ER"))
            if (notEr) return finish(generalHospital(base))
            if (erYes) return finish(base.copy(cat = "peds_er", peds = 1, er = "yes", detail = "pediatric ER"))
            return finish(base.copy(cat = "peds_er", peds = 2, detail = if (h24) LIKELY_24 else NOT_CONFIRMED))
        }
        // 2. a general ER with a pediatrics department
        if (isHosp && pedSpec && general && !pedName && erYes)
            return finish(base.copy(cat = "health", peds = 3, er = "yes", detail = "ER · pediatrics dept."))
        // 3. a children's hospital mapped only as a building
        if (bld == "hospital" && !isHosp && pedName && !notEr)
            return finish(base.copy(cat = "peds_er", peds = 2, detail = if (h24) LIKELY_24 else NOT_CONFIRMED))
        // 4. pediatric urgent care (not an ER)
        val clinicLike = am == "clinic" || am == "doctors" || am == "urgent_care" || hc == "clinic" || hc == "doctor" || hc == "urgent_care"
        if (clinicLike && (pedSpec || pedName) && urgent && !dental)
            return finish(base.copy(cat = "peds_urgent", peds = 4, detail = "not an ER"))
        // 5. everything else
        val cat = baseCategory(t)
        return finish(if (cat == "health") generalHospital(base) else base.copy(cat = cat))
    }

    private fun normName(t: Map<String, String>) = t.s("name").lowercase(Locale.ROOT).replace('’', '\'').trim().replace(SPACES, " ")
    private fun medical(cat: String) = cat == "peds_er" || cat == "peds_urgent" || cat == "health" || cat == "urgent"

    /**
     * [classify] each element, then the campus pass (600 m), then the dedupe (same name within 500 m). The result
     * list is aligned with the input; duplicates come back with `dropped = true`.
     */
    fun classifyAll(els: List<OsmElement>): List<Result> {
        val out = els.map { classify(it.tags) }.toMutableList()
        // Campus: a children's hospital with no confirmed ER takes the nearest other ER hospital within 600 m
        for (i in els.indices) {
            val r = out[i]
            if (r.cat != "peds_er" || r.peds != 2 || r.er == "yes") continue
            var best = -1; var bd = CAMPUS_M
            for (j in els.indices) {
                if (j == i || !out[j].hospitalEr || els[j].key == els[i].key) continue
                if (els[j].tags.s("name").isEmpty() || normName(els[j].tags) == normName(els[i].tags)) continue   // its own twin is the dedupe's business
                val d = distanceM(els[i].lat, els[i].lon, els[j].lat, els[j].lon)
                if (d <= bd) { bd = d; best = j }
            }
            if (best >= 0) {
                val campus = els[best].tags.s("name")
                out[i] = r.copy(campus = campus, detail = "ER on campus: $campus — call ahead")
            }
        }
        // Dedupe: the same name within 500 m is one place (a hospital drawn twice: the site and the building).
        // The object tagged amenity/healthcare wins; it takes over a phone / address the other one had.
        fun score(i: Int): Int {
            val t = els[i].tags
            return (if ("amenity" in t || "healthcare" in t) 4 else 0) + (if (out[i].er == "yes") 2 else 0) + (if (out[i].phone.isEmpty()) 0 else 1)
        }
        for (i in els.indices) {
            if (out[i].dropped || !medical(out[i].cat)) continue
            val ni = normName(els[i].tags)
            if (ni.isEmpty()) continue
            for (j in i + 1 until els.size) {
                if (out[j].dropped || !medical(out[j].cat) || normName(els[j].tags) != ni) continue
                if (distanceM(els[i].lat, els[i].lon, els[j].lat, els[j].lon) > DEDUPE_M) continue
                val dropI = score(j) > score(i)
                val keep = if (dropI) j else i; val drop = if (dropI) i else j
                out[drop] = out[drop].copy(dropped = true)
                if (out[keep].phone.isEmpty()) out[keep] = out[keep].copy(phone = out[drop].phone)
                if (out[keep].address.isEmpty()) out[keep] = out[keep].copy(address = out[drop].address)
                if (dropI) break
            }
        }
        return out
    }

    fun distanceM(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val r = 6371000.0
        val p1 = Math.toRadians(lat1); val p2 = Math.toRadians(lat2)
        val dp = Math.toRadians(lat2 - lat1); val dl = Math.toRadians(lon2 - lon1)
        val a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2)
        return 2 * r * atan2(sqrt(a), sqrt(1 - a))
    }

    /** Straight-line distance × 1.4 at 70 km/h, rounded to 5 minutes (at least 5): (driveS, driveM). */
    fun driveEstimate(distM: Double): Pair<Int, Int> {
        val road = max(0.0, distM) * 1.4
        val secs = road / (70.0 / 3.6)
        return max(300, (secs / 300.0).roundToLong().toInt() * 300) to road.roundToLong().toInt()
    }

    // ── Nearest help (the desktop's pickHelp, for the fixture's help picks) ──────
    /** rank 0 = tier 1, or tier 2 with an ER on the same campus; rank 1 = tier 2 without one, or tier 3; -1 = not a pediatric site. */
    fun pedsRank(peds: Int, campus: String): Int = when {
        peds == 1 || (peds == 2 && campus.isNotEmpty()) -> 0
        peds == 2 || peds == 3 -> 1
        else -> -1
    }

    data class HelpCandidate(val cat: String, val peds: Int = 0, val campus: String = "", val emergency: Boolean = false, val distM: Double = 0.0, val driveS: Int = 0)

    /** Indices into the candidate list, -1 = none. */
    data class HelpPicks(val pediatric: Int = -1, val pediatricCloser: Int = -1, val pediatricUrgent: Int = -1, val hospital: Int = -1)

    /**
     * pediatric = the lowest driveS among rank 0, else among rank 1; pediatricCloser = a rank-1 site closer than a
     * rank-0 pick; pediatricUrgent = the nearest pediatric urgent care; hospital = the nearest general ER (health with
     * emergency), else the nearest health.
     */
    fun pickHelp(c: List<HelpCandidate>): HelpPicks {
        fun better(a: Int, b: Int): Boolean {
            if (b < 0) return true
            if (c[a].driveS != c[b].driveS) return c[a].driveS < c[b].driveS
            return c[a].distM < c[b].distM
        }
        var best0 = -1; var best1 = -1; var hospEr = -1; var hospAny = -1; var urgent = -1
        for (i in c.indices) {
            val h = c[i]
            val pedsSite = h.cat == "peds_er" || (h.cat == "health" && h.peds == 3)
            if (pedsSite) {
                val rank = pedsRank(h.peds, h.campus)
                if (rank == 0 && better(i, best0)) best0 = i
                else if (rank == 1 && better(i, best1)) best1 = i
            }
            if (h.cat == "peds_urgent" && better(i, urgent)) urgent = i
            if (h.cat == "health") {
                if (h.emergency && (hospEr < 0 || h.distM < c[hospEr].distM)) hospEr = i
                if (hospAny < 0 || h.distM < c[hospAny].distM) hospAny = i
            }
        }
        return HelpPicks(
            pediatric = if (best0 >= 0) best0 else best1,
            pediatricCloser = if (best0 >= 0 && best1 >= 0 && c[best1].driveS < c[best0].driveS) best1 else -1,
            pediatricUrgent = urgent,
            hospital = if (hospEr >= 0) hospEr else hospAny,
        )
    }
}
