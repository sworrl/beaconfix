package org.sworrl.beaconfix.alpr.core

/** One candidate character for a slot. */
data class SlotChar(val c: Char, val p: Float)

/**
 * A plate read: the text, its confidence (mean of the chosen characters' probabilities) and the per-slot candidates.
 * [regionProb]: the recognizer's region head's probability that the plate is North American (US, Canada, Mexico);
 * -1 when the model has no region head.
 */
data class PlateRead(val text: String, val conf: Float, val slots: List<List<SlotChar>>, val regionProb: Float = -1f)

/**
 * Decoding of the fast-plate-ocr recognizer (cct_xs_v2_global): [MAX_SLOTS] heads over [ALPHABET], softmax
 * probabilities, '_' pads plates shorter than ten characters.
 */
object PlateText {
    const val ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_"
    const val PAD = '_'
    const val MAX_SLOTS = 10

    /** Decode one plate: [probs] = MAX_SLOTS × ALPHABET.length probabilities starting at [offset]. Keeps [topK] per slot. */
    fun decode(probs: FloatArray, offset: Int = 0, topK: Int = 3): PlateRead {
        val n = ALPHABET.length
        val sb = StringBuilder(); val slots = ArrayList<List<SlotChar>>(); var sum = 0f; var cnt = 0
        for (s in 0 until MAX_SLOTS) {
            val base = offset + s * n
            var best = 0
            for (i in 1 until n) if (probs[base + i] > probs[base + best]) best = i
            val ch = ALPHABET[best]
            if (ch == PAD) continue                // pads (fast-plate-ocr pads at the end; a stray pad mid-plate is dropped too)
            sb.append(ch); sum += probs[base + best]; cnt++
            val idx = (0 until n).filter { ALPHABET[it] != PAD }.sortedByDescending { probs[base + it] }.take(topK)
            slots += idx.map { SlotChar(ALPHABET[it], round3(probs[base + it])) }.filter { it.p >= 0.01f || it.c == ch }
        }
        return PlateRead(sb.toString(), if (cnt == 0) 0f else round3(sum / cnt), slots)
    }

    /** Upper-case, letters and digits only ("ABC-1234" → "ABC1234"). */
    fun normalize(s: String): String = buildString { for (c in s.uppercase()) if (c in 'A'..'Z' || c in '0'..'9') append(c) }

    private fun round3(v: Float) = Math.round(v * 1000f) / 1000f
}
