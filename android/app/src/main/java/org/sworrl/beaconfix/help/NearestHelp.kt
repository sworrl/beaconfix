package org.sworrl.beaconfix.help

import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.estimate.Geo
import java.time.LocalDateTime
import java.util.Locale

/**
 * Nearest help from one origin (pure): the children's ER, a closer lower-confidence one, pediatric urgent care, the
 * nearest general ER, urgent care, police, fire, pharmacy and vet. The pediatric ranking is the desktop's
 * (`PoiClassify::pickHelp`, judge decision 1):
 *  - rank 0 = tier 1, or tier 2 with a confirmed ER on the same campus;
 *  - rank 1 = tier 2 without a campus ER, or tier 3 (a general ER with a pediatrics department);
 *  - `peds_er` = the shortest drive among rank 0, else among rank 1;
 *  - `peds_closer` = a rank-1 site with a shorter drive than a rank-0 pick;
 *  - `er` = the nearest `health` place with an emergency department, else the nearest `health` place not tagged
 *    `emergency=no`. It is never a `peds_er` place, so a children's pick never hides the general ER.
 */
object NearestHelp {
    const val POISON_CONTROL_US = "1-800-222-1222"

    /** The place categories Help reads (desktop keys, C1). */
    val HELP_CATS = setOf("police", "fire", "health", "urgent", "peds_er", "peds_urgent", "pharmacy", "vet")

    /**
     * A place that could be help. [driveS] > 0 is a drive time someone else computed from ([fromLat], [fromLon]);
     * [driveEst] false = a routed time. [source]/[fetchedAt]/[originLat]/[originLon] say who found it, when, and where
     * the search was made from. [guessed] = its tier comes from [PedsFallback], not a classifier.
     */
    data class Candidate(
        val key: String, val cat: String, val name: String, val lat: Double, val lon: Double,
        val phone: String = "", val address: String = "", val hours: String = "", val website: String = "",
        val peds: Int = 0, val er: String = "", val campus: String = "", val emergency: Boolean = false,
        val driveS: Int = 0, val driveEst: Boolean = true, val fromLat: Double = 0.0, val fromLon: Double = 0.0,
        val source: String = "", val fetchedAt: Long = 0, val originLat: Double = 0.0, val originLon: Double = 0.0,
        val guessed: Boolean = false, val detail: String = "",
    )

    fun fromPoi(p: PoiEntity) = Candidate(
        key = p.key, cat = p.cat, name = p.name.ifBlank { p.label }, detail = p.detail, lat = p.lat, lon = p.lon, phone = p.phone, address = p.address,
        hours = p.hours, website = p.website, peds = p.peds, er = p.er, campus = p.campus, emergency = p.emergency,
        driveS = p.driveS, driveEst = p.driveEst, fromLat = p.originLat, fromLon = p.originLon,
        source = p.source, fetchedAt = p.fetchedAt, originLat = p.originLat, originLon = p.originLon,
    )

    /** One pick: the place as shown plus the candidate it came from (for provenance). */
    data class Pick(val place: HelpPlace, val from: Candidate)

    /** The desktop's rank (0 best, 1 lower confidence, -1 not a children's ER). */
    fun rank(peds: Int, campus: String): Int = when {
        peds == 1 || (peds == 2 && campus.isNotBlank()) -> 0
        peds == 2 || peds == 3 -> 1
        else -> -1
    }

    fun isPedsSite(c: Candidate) = c.cat == "peds_er" || (c.cat == "health" && c.peds == 3)
    private fun hasEr(c: Candidate) = c.emergency || c.er == "yes"

    /**
     * Pick nearest help from ([oLat], [oLon]), in [HelpKind] order. A drive time carried by a candidate is kept only
     * when it was routed (not an estimate) from within [keepDriveWithinM] of the origin; otherwise it is re-estimated.
     * [at] (local time) fills [HelpPlace.openNow]; null leaves it unknown.
     */
    fun pick(cands: List<Candidate>, oLat: Double, oLon: Double, at: LocalDateTime? = null, keepDriveWithinM: Double = 2000.0): List<Pick> {
        data class M(val c: Candidate, val d: Double, val s: Int, val est: Boolean)
        val ms = cands.filter { it.cat in HELP_CATS && Origin.valid(it.lat, it.lon) }.map { c ->
            val d = Geo.distanceM(oLat, oLon, c.lat, c.lon)
            val keep = c.driveS > 0 && !c.driveEst && Origin.valid(c.fromLat, c.fromLon) && Geo.distanceM(oLat, oLon, c.fromLat, c.fromLon) <= keepDriveWithinM
            if (keep) M(c, d, c.driveS, false) else M(c, d, DriveEstimate.seconds(d), true)
        }
        val byDrive = compareBy<M>({ it.s }, { it.d })
        val byDist = compareBy<M> { it.d }
        fun nearest(cat: String) = ms.filter { it.c.cat == cat }.minWithOrNull(byDist)

        val peds = ms.filter { isPedsSite(it.c) }
        val best0 = peds.filter { rank(it.c.peds, it.c.campus) == 0 }.minWithOrNull(byDrive)
        val best1 = peds.filter { rank(it.c.peds, it.c.campus) == 1 }.minWithOrNull(byDrive)
        val pedsEr = best0 ?: best1
        val closer = if (best0 != null && best1 != null && best1.s < best0.s) best1 else null
        val health = ms.filter { it.c.cat == "health" }
        val er = health.filter { hasEr(it.c) }.minWithOrNull(byDist) ?: health.filter { it.c.er != "no" }.minWithOrNull(byDist)

        fun place(kind: String, m: M?, notEr: Boolean = false): Pick? = m?.let {
            val c = it.c
            Pick(HelpPlace(
                kind = kind, name = c.name, lat = c.lat, lon = c.lon, phone = c.phone, address = c.address, hours = c.hours, website = c.website,
                osmKey = c.key, tier = if (kind == HelpKind.PEDS_URGENT) 4 else c.peds, er = if (hasEr(c) && c.er.isEmpty()) "yes" else c.er,
                campus = c.campus, notEr = notEr, distM = it.d, brgDeg = Geo.bearingDeg(oLat, oLon, c.lat, c.lon), driveS = it.s, driveEst = it.est,
                openNow = at?.let { t -> if (c.hours.isBlank()) null else OpeningHours.isOpen(c.hours, t) },
            ), c)
        }
        return listOfNotNull(
            place(HelpKind.PEDS_ER, pedsEr),
            place(HelpKind.PEDS_CLOSER, closer),
            place(HelpKind.PEDS_URGENT, nearest("peds_urgent"), notEr = true),
            place(HelpKind.ER, er),
            // the desktop's "urgent" category is every clinic and doctor's office (a chiropractor too): only an actual
            // urgent care may sit under that heading on an emergency screen
            place(HelpKind.URGENT, ms.filter { it.c.cat == "urgent" && isUrgentCare(it.c.name, it.c.detail) }.minWithOrNull(byDist), notEr = true),
            place(HelpKind.POLICE, nearest("police")),
            place(HelpKind.FIRE, nearest("fire")),
            place(HelpKind.PHARMACY, nearest("pharmacy")),
            place(HelpKind.VET, nearest("vet")),
        )
    }

    private val URGENT_NAME = Regex("""\burgent\b|express ?care|after.?hours|walk.?in|immediate ?care|convenient ?care|med ?express""", RegexOption.IGNORE_CASE)

    /**
     * An urgent care, not just any clinic (same rule as the desktop's `PoiClassify::isUrgentCare`): tagged
     * `urgent_care` (the place detail then says "urgent care"), or named like one (C6's `urgent` name rule).
     */
    fun isUrgentCare(name: String, detail: String): Boolean =
        detail.contains("urgent care", ignoreCase = true) || URGENT_NAME.containsMatchIn(name.replace('\u2019', '\''))

    /** Poison Control for the country ([cc] ISO 3166 alpha-2), or null where we do not know one. */
    fun poisonControl(cc: String?): String? = if (cc?.uppercase(Locale.ROOT) == "US") POISON_CONTROL_US else null

    /**
     * The confidence text a place must always carry (never hidden): "Pediatric ER", "ER on campus: X — call ahead",
     * "ER not confirmed — call ahead", "ER · pediatrics dept.", "Not an ER", or for a general ER "ER" / "ER not confirmed".
     */
    fun confidence(p: HelpPlace): String = when {
        p.notEr || p.tier == 4 -> "Not an ER"
        p.tier == 1 -> "Pediatric ER"
        p.tier == 2 && p.campus.isNotBlank() -> "ER on campus: ${p.campus} — call ahead"
        p.tier == 2 -> "ER not confirmed — call ahead"
        p.tier == 3 -> "ER · pediatrics dept."
        p.kind == HelpKind.ER -> if (p.er == "yes") "ER" else "ER not confirmed — call ahead"
        else -> ""
    }
}
