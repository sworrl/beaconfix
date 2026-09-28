package org.sworrl.beaconfix.help

import java.util.Locale

/**
 * Children's ERs for a desktop older than 3.8 (no "pediatric" feature), from names alone (pure). A `health` place whose
 * name reads like a children's hospital (the C6 `pedName` rule) and not like a clinic, rehab or psychiatric site (the
 * C6 `notEr` rule) becomes tier 2 ("ER not confirmed — call ahead"), or tier 1 when it is tagged with an emergency
 * department. A place tagged `emergency=no` stays what it is.
 */
object PedsFallback {
    /** C6 `pedName`, on the normalized name. */
    val PED_NAME = Regex("""\b(children'?s?|child|pa?ediatric\w*|kids?)\b""")
    /** C6 `notEr`, on the normalized name (the classifier's own list). */
    val NOT_ER = org.sworrl.beaconfix.poi.PedsClassifier.NOT_ER

    /** Lower case with typographic apostrophes made plain, as C6 `nm`. */
    fun norm(name: String): String = name.lowercase(Locale.ROOT).replace('’', '\'')

    fun pediatricName(name: String): Boolean { val n = norm(name); return PED_NAME.containsMatchIn(n) && !NOT_ER.containsMatchIn(n) }

    /** The tier a name-only guess gives a `health` place, or 0 when it is not a children's hospital. */
    fun tier(cat: String, name: String, emergency: Boolean, er: String, peds: Int): Int = when {
        cat != "health" || peds != 0 || er == "no" -> 0
        !pediatricName(name) -> 0
        emergency || er == "yes" -> 1
        else -> 2
    }

    /** [c] re-read as a children's ER when [tier] says so; unchanged otherwise. */
    fun apply(c: NearestHelp.Candidate): NearestHelp.Candidate {
        val t = tier(c.cat, c.name, c.emergency, c.er, c.peds)
        if (t == 0) return c
        return c.copy(cat = "peds_er", peds = t, er = if (t == 1) "yes" else c.er, guessed = true)
    }
}
