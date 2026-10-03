package org.sworrl.beaconfix.alpr.core

import java.util.PriorityQueue
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min

/**
 * The candidate lattice of a plate: per slot, the recognizer's (or the fused) top-K characters with probabilities.
 * A character outside a slot's top-K gets that slot's leftover probability mass spread over the rest of the alphabet
 * (floored), so any string of the right length has a finite score.
 *
 * Scoring a string as the sum of log p(character) over slots, and enumerating the N best strings best-first, is the
 * idea of OpenALPR's "postprocess" step (top-N permutations of per-character candidates). This is our own code; no
 * OpenALPR (AGPL) code is used.
 */
class PlateLattice(val slots: List<List<SlotChar>>) {
    val length get() = slots.size

    /** Per slot, candidates sorted by probability (highest first). */
    private val sorted: List<List<SlotChar>> = slots.map { s -> s.sortedByDescending { it.p } }

    private val floor: FloatArray = FloatArray(slots.size) { i ->
        val s = slots[i]
        val rest = 1f - s.sumOf { it.p.toDouble() }.toFloat()
        max(MIN_P, rest / max(1, SYMBOLS - s.size))
    }

    fun p(slot: Int, c: Char): Float = slots[slot].firstOrNull { it.c == c }?.p?.let { max(it, MIN_P) } ?: floor[slot]

    /** True when every character of [s] is among its slot's candidates (and the lengths agree). */
    fun covers(s: String): Boolean = s.length == length && s.indices.all { i -> slots[i].any { it.c == s[i] } }

    /** Sum of log p over slots; -Inf when the length differs. */
    fun logp(s: String): Double {
        if (s.length != length) return Double.NEGATIVE_INFINITY
        var sum = 0.0
        for (i in s.indices) sum += ln(p(i, s[i]).toDouble())
        return sum
    }

    /** The most likely string (each slot's top candidate). */
    fun best(): String = buildString { for (s in sorted) if (s.isNotEmpty()) append(s[0].c) }

    /** Mean of each slot's top probability. */
    fun meanTop(): Float = if (sorted.isEmpty()) 0f else sorted.map { it.firstOrNull()?.p ?: 0f }.average().toFloat()

    /**
     * The [n] most likely strings made of slot candidates, best first, with their log scores. Best-first search over
     * index vectors: a vector's successors raise one slot index at or after the last raised slot, so every
     * combination is generated exactly once and scores never increase along a path.
     */
    fun kBest(n: Int): List<Pair<String, Double>> {
        if (n <= 0 || sorted.isEmpty() || sorted.any { it.isEmpty() }) return emptyList()
        class Node(val idx: IntArray, val last: Int, val score: Double)
        fun score(idx: IntArray): Double { var s = 0.0; for (i in idx.indices) s += ln(max(MIN_P, sorted[i][idx[i]].p).toDouble()); return s }
        val pq = PriorityQueue<Node>(compareByDescending { it.score })
        val start = IntArray(sorted.size)
        pq += Node(start, 0, score(start))
        val out = ArrayList<Pair<String, Double>>(n)
        while (pq.isNotEmpty() && out.size < n) {
            val node = pq.poll()!!
            out += String(CharArray(node.idx.size) { sorted[it][node.idx[it]].c }) to node.score
            for (j in node.last until node.idx.size) {
                if (node.idx[j] + 1 >= sorted[j].size) continue
                val nx = node.idx.copyOf(); nx[j]++
                pq += Node(nx, j, score(nx))
            }
        }
        return out
    }

    companion object {
        /** Characters the recognizer can output, pad excluded. */
        val SYMBOLS = PlateText.ALPHABET.length - 1
        const val MIN_P = 1e-4f

        fun of(read: PlateRead) = PlateLattice(read.slots)

        /** A lattice that is certain of [text] (each slot one candidate at [p]); for text-only reads. */
        fun certain(text: String, p: Float = 0.999f) = PlateLattice(text.map { listOf(SlotChar(it, p)) })
    }
}

/** String similarity for plate texts. */
object PlateSimilarity {
    /** Jaro-Winkler similarity in 0..1 (prefix scale 0.1, up to 4 characters). */
    fun jaroWinkler(a: String, b: String): Double {
        if (a == b) return if (a.isEmpty()) 0.0 else 1.0
        if (a.isEmpty() || b.isEmpty()) return 0.0
        val range = max(0, max(a.length, b.length) / 2 - 1)
        val am = BooleanArray(a.length); val bm = BooleanArray(b.length)
        var m = 0
        for (i in a.indices) {
            val lo = max(0, i - range); val hi = min(b.length - 1, i + range)
            for (j in lo..hi) if (!bm[j] && a[i] == b[j]) { am[i] = true; bm[j] = true; m++; break }
        }
        if (m == 0) return 0.0
        var t = 0; var k = 0
        for (i in a.indices) if (am[i]) { while (!bm[k]) k++; if (a[i] != b[k]) t++; k++ }
        val md = m.toDouble()
        val jaro = (md / a.length + md / b.length + (md - t / 2.0) / md) / 3.0
        var prefix = 0
        while (prefix < min(4, min(a.length, b.length)) && a[prefix] == b[prefix]) prefix++
        return jaro + prefix * 0.1 * (1 - jaro)
    }
}
