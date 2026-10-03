package org.sworrl.beaconfix.alpr.core

import kotlin.math.ln

/**
 * US licence-plate format grammar, used after fusion as a prior: where the lattice is unsure whether a slot is "8" or
 * "B", a string that fits the plate formats of the state we are driving in (the GPS state, `FrameContext.regionCode`)
 * wins over one that fits no format at all. Plus the read gates: a minimum length, and a US/region check on long reads.
 *
 * The table is our own compilation of the standard passenger series as state DMVs publish them (public facts, not
 * copied from any software). Patterns: `L` a letter, `D` a digit. Only states whose series we are sure of are listed;
 * the others, and vanity / specialty plates everywhere, get no state bonus (never a penalty beyond losing the bonus),
 * so an unlisted format is still read, just without help.
 */
object PlateFormats {
    /** Reads shorter than this do not count (2–4 character reads are almost always a fragment or a sticker). */
    const val MIN_LEN = 5
    /** No US standard plate is longer; vanity plates reach 8 in a few states. */
    const val MAX_LEN = 8
    /** From this length on a read must look like a US plate (format or the recognizer's region head). */
    const val REGION_GATE_LEN = 7
    /** Minimum "North American" probability from the recognizer's region head for a long read with no US format. */
    const val MIN_REGION_PROB = 0.3f

    val STATES: Map<String, List<String>> = mapOf(
        "AK" to listOf("LLLDDD"),
        "AZ" to listOf("LLLDDDD", "LLLDDD"),
        "CA" to listOf("DLLLDDD"),
        "CO" to listOf("LLLLDD", "LLLDDD", "DDDLLL"),
        "DC" to listOf("LLDDDD"),
        "DE" to listOf("DDDDDD", "DDDDD"),
        "FL" to listOf("LLLDLL", "LLLLDD", "DDDLLL"),
        "GA" to listOf("LLLDDDD"),
        "HI" to listOf("LLLDDD"),
        "IA" to listOf("LLLDDD", "DDDLLL"),
        "IL" to listOf("LLDDDDD"),
        "KS" to listOf("DDDLLL"),
        "KY" to listOf("DDDLLL"),
        "LA" to listOf("DDDLLL"),
        "MA" to listOf("DLLDDD", "DLLLDD"),
        "MD" to listOf("DLLDDDD"),
        "ME" to listOf("DDDDLL"),
        "MI" to listOf("LLLDDDD"),
        "MN" to listOf("LLLDDD", "DDDLLL"),
        "MO" to listOf("LLDLDL"),
        "NC" to listOf("LLLDDDD"),
        "ND" to listOf("LLLDDD"),
        "NE" to listOf("LLLDDD"),
        "NH" to listOf("DDDDDDD", "DDDDDD"),
        "NJ" to listOf("LDDLLL", "LLLDDL"),
        "NM" to listOf("LLLDDD", "DDDLLL"),
        "NY" to listOf("LLLDDDD"),
        "OH" to listOf("LLLDDDD"),
        "OK" to listOf("LLLDDD"),
        "OR" to listOf("DDDLLL", "LLLDDD"),
        "PA" to listOf("LLLDDDD"),
        "RI" to listOf("LLDDD", "DDDDDD", "DDDDD"),
        "SC" to listOf("LLLDDD"),
        "TX" to listOf("LLLDDDD", "LLDLDDD"),
        "VA" to listOf("LLLDDDD"),
        "VT" to listOf("LLLDDD"),
        "WA" to listOf("LLLDDDD", "DDDLLL"),
        "WI" to listOf("LLLDDDD", "DDDLLL"),
    )

    /** Every listed format (the "looks like a US plate" set). */
    val US: Set<String> = STATES.values.flatten().toSet()

    /** Prior weights (log) relative to a string that fits no format. */
    private val LOG_STATE = ln(50.0)
    private val LOG_US = ln(5.0)

    enum class Fit { STATE, US, NONE }

    fun matches(text: String, pattern: String): Boolean =
        text.length == pattern.length && text.indices.all { i -> when (pattern[i]) { 'L' -> text[i] in 'A'..'Z'; 'D' -> text[i] in '0'..'9'; else -> true } }

    fun fit(text: String, state: String?): Fit = when {
        state != null && STATES[state].orEmpty().any { matches(text, it) } -> Fit.STATE
        US.any { matches(text, it) } -> Fit.US
        else -> Fit.NONE
    }

    /** A structurally valid US plate (any listed state's format). */
    fun valid(text: String) = US.any { matches(text, it) }

    /**
     * Whether a read counts at all: [MIN_LEN]..[MAX_LEN] characters, and from [REGION_GATE_LEN] on either a US format
     * or the recognizer's region head saying North America ([regionProb] < 0 = the model has no region head).
     */
    fun gate(text: String, regionProb: Float = -1f): Boolean = when {
        text.length < MIN_LEN || text.length > MAX_LEN -> false
        text.length < REGION_GATE_LEN -> true
        valid(text) -> true
        regionProb < 0f -> text.length < MAX_LEN
        else -> regionProb >= MIN_REGION_PROB
    }

    data class Choice(val text: String, val fit: Fit, val score: Double)

    /**
     * The most probable string under lattice × format prior: the lattice's best string against, for each format of the
     * right length, the best string that fits it using only slot candidates (a format that needs a character no slot
     * offers is not considered). [state]: the two-letter state we are in (null = unknown: only the US-wide prior).
     */
    fun choose(l: PlateLattice, state: String?): Choice {
        fun prior(f: Fit) = when (f) { Fit.STATE -> LOG_STATE; Fit.US -> LOG_US; Fit.NONE -> 0.0 }
        val best = l.best()
        val bf = fit(best, state)
        var out = Choice(best, bf, l.logp(best) + prior(bf))
        val pats = (STATES[state].orEmpty() + US).distinct().filter { it.length == l.length }
        for (p in pats) {
            val s = constrained(l, p) ?: continue
            val f = fit(s, state)
            val sc = l.logp(s) + prior(f)
            if (sc > out.score + 1e-9) out = Choice(s, f, sc)
        }
        return out
    }

    /** The best string fitting [pattern] from [l]'s slot candidates, or null if some slot has no fitting candidate. */
    private fun constrained(l: PlateLattice, pattern: String): String? {
        val sb = StringBuilder(pattern.length)
        for (i in pattern.indices) {
            val c = l.slots[i].sortedByDescending { it.p }.firstOrNull { matches(it.c.toString(), pattern[i].toString()) } ?: return null
            sb.append(c.c)
        }
        return sb.toString()
    }
}
