package org.sworrl.beaconfix.tile

import org.sworrl.beaconfix.help.HelpKind
import org.sworrl.beaconfix.help.HelpPlace
import org.sworrl.beaconfix.help.HelpSnapshot
import org.sworrl.beaconfix.ui.metres
import kotlin.math.roundToInt

/**
 * The short texts the quick-access surfaces show for a [HelpSnapshot] — the Quick Settings tile, the Help widget's
 * one-line size, the status notification's help line and the new-area heads-up. Pure (no Android types) so it is
 * unit-tested. Distances go through [metres] (so they follow the units setting).
 *
 * Medical wording rules: a tier-2 site (children's hospital, ER not confirmed) always says "call ahead"; urgent care
 * always says "not an ER"; the general ER is never dropped in favour of the pediatric one.
 */
object TileModel {
    /** What the "Nearest help" tile shows. [active] = there is something to show. */
    data class HelpTile(val subtitle: String, val active: Boolean, val description: String)

    /** What the collector tile shows. */
    data class CollectorTile(val subtitle: String, val active: Boolean)

    const val NO_DATA = "No data yet"
    const val NONE_FOUND = "Nothing found"

    fun help(s: HelpSnapshot, dist: (Double) -> String = ::metres): HelpTile {
        val peds = s.first(HelpKind.PEDS_ER)
        val er = s.first(HelpKind.ER)
        if (s.fetchedAt <= 0L && s.places.isEmpty()) return HelpTile(NO_DATA, false, "Nearest help: no data yet")
        val sub = when {
            peds != null -> {
                val d = distText(peds, dist)
                val tail = when {
                    s.stale -> "saved"
                    peds.tier == 2 -> "call ahead"
                    else -> eta(peds.driveS)
                }
                listOf("${tileKind(peds)} $d".trim(), tail).filter { it.isNotBlank() }.joinToString(" · ")
            }
            er != null -> listOf("ER ${distText(er, dist)}".trim(), if (s.stale) "saved" else "").filter { it.isNotBlank() }.joinToString(" · ")
            else -> NONE_FOUND
        }
        val desc = buildString {
            append("Nearest help: ")
            append(listOfNotNull(peds?.let { "${longKind(it)} ${it.name}, ${distText(it, dist)}" }, er?.let { "ER ${it.name}, ${distText(it, dist)}" })
                .joinToString("; ").ifEmpty { NONE_FOUND.lowercase() })
            if (s.stale) append(" (saved)")
        }
        return HelpTile(sub, peds != null || er != null, desc)
    }

    /** Answers older than this are shown as "saved" even if they were fresh when computed. */
    const val MAX_FRESH_MS = 24 * 3600_000L

    /** [s] as the surfaces should show it at [now]: an answer from more than a day ago counts as stale ("saved"). */
    fun aged(s: HelpSnapshot, now: Long): HelpSnapshot =
        if (!s.stale && s.fetchedAt > 0 && now - s.fetchedAt > MAX_FRESH_MS) s.copy(stale = true) else s

    /**
     * The answer the surfaces should show: the live one from HelpRepository when it was computed after the saved copy
     * ([HelpSnapshot.computedAt], not the data's age: the phone moving recomputes picks and distances from the same, or
     * older, data) and has places, or the saved copy is empty too; otherwise the saved copy (process restarted, or a
     * refresh that found nothing in reach). A copy saved before 1.4's computedAt reads 0, so any live answer beats it.
     */
    fun newest(live: HelpSnapshot?, saved: HelpSnapshot): HelpSnapshot =
        if (live != null && live.computedAt > saved.computedAt && (live.places.isNotEmpty() || saved.places.isEmpty())) live else saved

    fun collector(on: Boolean): CollectorTile = CollectorTile(if (on) "On" else "Paused", on)

    /** Tile / widget word for a pediatric site by tier. */
    fun tileKind(p: HelpPlace): String = when {
        p.kind == HelpKind.ER -> "ER"
        p.notEr || p.tier == 4 -> "Kids urgent care"
        p.tier == 2 -> "Kids hospital"
        p.tier == 3 -> "ER + peds"
        else -> "Kids ER"
    }

    /** The same, with the confidence wording spelled out (notification, heads-up). */
    fun longKind(p: HelpPlace): String = when {
        p.kind == HelpKind.ER -> "ER"
        p.notEr || p.tier == 4 -> "Kids urgent care (not an ER)"
        p.tier == 2 -> "Kids hospital (call ahead)"
        p.tier == 3 -> "ER with pediatrics"
        else -> "Kids ER"
    }

    /** The always-text confidence badge for the children's site (same wording as the Help screen). */
    fun tierText(p: HelpPlace): String = when {
        p.notEr || p.tier == 4 -> "Not an ER"
        p.tier == 1 -> "Children's ER"
        p.tier == 2 && p.campus.isNotBlank() -> "Children's hospital — ER on campus: ${p.campus} — call ahead"
        p.tier == 2 -> "Children's hospital — ER not confirmed, call ahead"
        p.tier == 3 -> "ER · pediatrics dept."
        p.kind == HelpKind.ER -> "Nearest ER"
        else -> ""
    }

    /**
     * "~45 min", "~1 h 50 min", "~2 h" from seconds: rounded to 5 minutes with a 5-minute minimum (the Help screen's
     * rule, without its "(est.)"); "" when unknown.
     */
    fun eta(driveS: Int): String {
        if (driveS < 0) return ""
        val m = maxOf(5, ((driveS / 60.0) / 5.0).roundToInt() * 5)
        if (m < 60) return "~$m min"
        val h = m / 60; val r = m % 60
        return if (r == 0) "~$h h" else "~$h h $r min"
    }

    fun distText(p: HelpPlace, dist: (Double) -> String = ::metres): String = if (p.distM >= 0) dist(p.distM) else ""

    /** The one-line widget size: "🧸 38 km · ~45 min" (or "ER 12 km" without a children's site). */
    fun headline(s: HelpSnapshot, dist: (Double) -> String = ::metres): String {
        val peds = s.first(HelpKind.PEDS_ER)
        val er = s.first(HelpKind.ER)
        return when {
            peds != null -> "🧸 " + listOf(distText(peds, dist), if (s.stale) "saved" else if (peds.tier == 2) "call ahead" else eta(peds.driveS)).filter { it.isNotBlank() }.joinToString(" · ")
            er != null -> "ER " + listOf(distText(er, dist), if (s.stale) "saved" else eta(er.driveS)).filter { it.isNotBlank() }.joinToString(" · ")
            else -> ""
        }
    }

    /** The status notification's help line; "" when there is nothing to say. */
    fun notificationLine(s: HelpSnapshot, dist: (Double) -> String = ::metres): String {
        val parts = ArrayList<String>()
        s.first(HelpKind.PEDS_ER)?.let { p -> parts += "🧸 ${longKind(p)}: ${p.name} · " + listOf(distText(p, dist), eta(p.driveS)).filter { it.isNotBlank() }.joinToString(" · ") }
        s.first(HelpKind.ER)?.let { p -> parts += "ER: ${p.name} · ${distText(p, dist)}".trimEnd(' ', '·') }
        if (parts.isEmpty()) return ""
        return parts.joinToString(" — ") + if (s.stale) " (saved)" else ""
    }

    /** The heads-up text: "Nearest help here: Kids ER <name> <dist> · ER <name> <dist>". */
    fun alertText(s: HelpSnapshot, dist: (Double) -> String = ::metres): String {
        val parts = ArrayList<String>()
        s.first(HelpKind.PEDS_ER)?.let { p -> parts += "${longKind(p)} ${p.name} ${distText(p, dist)}".trim() }
        s.first(HelpKind.ER)?.let { p -> parts += "ER ${p.name} ${distText(p, dist)}".trim() }
        if (parts.isEmpty()) (s.first(HelpKind.URGENT) ?: s.first(HelpKind.PEDS_URGENT))?.let { p -> parts += "Urgent care (not an ER) ${p.name} ${distText(p, dist)}".trim() }
        return "Nearest help here: " + parts.joinToString(" · ")
    }
}
