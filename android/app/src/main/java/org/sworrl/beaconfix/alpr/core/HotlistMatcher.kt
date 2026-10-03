package org.sworrl.beaconfix.alpr.core

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/** `GET /api/mobile/hotlist` (FalconEyez). */
@Serializable
data class Hotlist(
    @SerialName("updated_at") val updatedAt: String = "",
    val entries: List<HotlistEntry> = emptyList(),
    @SerialName("own_plates") val ownPlates: List<String> = emptyList(),
    @SerialName("blocked_regions") val blockedRegions: List<String> = emptyList(),
)

@Serializable
data class HotlistEntry(
    @Serializable(with = LenientString::class) val id: String = "",
    val plate: String = "",
    @SerialName("plate_state") val plateState: String = "",
    /** Only some characters are known: '*' marks an unknown one (then the length is known), else a run of the plate. */
    val partial: Boolean = false,
    @SerialName("alert_type") val alertType: String = "",
    val title: String = "",
    val summary: String = "",
    @SerialName("vehicle_desc") val vehicleDesc: String = "",
    val url: String = "",
    @SerialName("expires_at") val expiresAt: String = "",
)

enum class MatchKind { EXACT, POSSIBLE }

/**
 * [read]: the plate text we settled on; [candidate]: the string that matched (equal to [read] for an exact match, a
 * lattice alternative for a possible one); [ratio]: how likely the candidate is relative to [read] (1 for exact).
 */
data class HotlistMatch(val entry: HotlistEntry, val kind: MatchKind, val read: String, val candidate: String = read, val ratio: Double = 1.0)

/**
 * Matches plate reads against the hotlist.
 *
 * Exact: the settled text equals a hotlist plate (both normalised). Possible: a hotlist plate is one of the [TOP_N]
 * most likely strings of the read's candidate lattice (per-slot top-3, fused over the vehicle's frames) and at least
 * [MIN_RATIO] as likely as the settled text; each hotlist plate's score is the sum of log p(character) over slots.
 * This replaces a fixed table of confusable characters: whether "B" or "8" is a plausible alternative is whatever the
 * recognizer said about that very slot. Partial alert plates ('*' = unknown character) are matched against the same
 * candidates and are never more than possible. Our own plates are never matched (exact, among the lattice's
 * candidates, or with the classic confusables folded, so our own car is not reported through a misread). Expired
 * entries are ignored.
 */
class HotlistMatcher(hotlist: Hotlist, private val nowMs: () -> Long = System::currentTimeMillis) {
    private val own = hotlist.ownPlates.map { PlateText.normalize(it) }.filter { it.isNotEmpty() }.toSet()
    private val ownFolded = own.map { canonical(it) }.toSet()
    private val byPlate: Map<String, List<HotlistEntry>> = hotlist.entries
        .filter { !isPartial(it) && PlateText.normalize(it.plate).length >= 2 }
        .groupBy { PlateText.normalize(it.plate) }
    /** Partial plates as (pattern, anchored): anchored = same length with '*' for each unknown character. */
    private val partials: List<Triple<String, Boolean, HotlistEntry>> = hotlist.entries.filter { isPartial(it) }.mapNotNull { e ->
        val pat = buildString { for (c in e.plate.uppercase()) when (c) { in 'A'..'Z', in '0'..'9' -> append(c); '*', '?', '_' -> append('*') } }
        if (pat.count { it != '*' } < 3) null else Triple(pat, pat.contains('*'), e)
    }

    private fun isPartial(e: HotlistEntry) = e.partial || e.plate.any { it == '*' || it == '?' || it == '_' }

    val size get() = byPlate.size

    fun isOwn(read: String): Boolean {
        val r = PlateText.normalize(read)
        return r.isNotEmpty() && (r in own || canonical(r) in ownFolded)
    }

    /** Own plate: the text, or any of the lattice's likely alternatives. */
    fun isOwn(read: String, lattice: PlateLattice?): Boolean =
        isOwn(read) || (lattice != null && own.isNotEmpty() && candidates(read, lattice).any { it.first in own })

    /** A read with no candidate lattice (text only): exact and partial matches. */
    fun match(read: String): List<HotlistMatch> = match(read, null)

    /** [read] (the settled text) with its candidate [lattice] (null = text only). */
    fun match(read: String, lattice: PlateLattice?): List<HotlistMatch> {
        val r = PlateText.normalize(read)
        if (r.length < 2 || isOwn(r, lattice)) return emptyList()
        val now = nowMs()
        val out = ArrayList<HotlistMatch>()
        byPlate[r]?.filter { live(it, now) }?.forEach { out += HotlistMatch(it, MatchKind.EXACT, r) }
        if (out.isNotEmpty()) return out
        val cands = if (lattice == null) listOf(r to 1.0) else candidates(r, lattice)
        val seen = HashSet<String>()
        for ((c, ratio) in cands) {
            if (c == r) continue
            byPlate[c]?.filter { live(it, now) && seen.add(it.id + c) }?.forEach { out += HotlistMatch(it, MatchKind.POSSIBLE, r, c, ratio) }
        }
        // a partial alert plate can never be confirmed beyond its known characters: at most a possible match
        for ((pat, anchored, e) in partials) {
            if (!live(e, now)) continue
            val hit = cands.firstOrNull { (c, _) -> partialMatch(pat, anchored, c) } ?: continue
            out += HotlistMatch(e, MatchKind.POSSIBLE, r, hit.first, hit.second)
        }
        return out
    }

    /** [read] plus the lattice's [TOP_N] best strings at least [MIN_RATIO] as likely as [read], with that ratio. */
    fun candidates(read: String, lattice: PlateLattice): List<Pair<String, Double>> {
        val base = lattice.logp(read).takeIf { it.isFinite() } ?: lattice.logp(lattice.best())
        val out = ArrayList<Pair<String, Double>>()
        out += read to 1.0
        for ((s, lp) in lattice.kBest(TOP_N)) {
            val ratio = kotlin.math.exp(lp - base)
            if (s != read && ratio >= MIN_RATIO) out += s to ratio
        }
        return out
    }

    private fun live(e: HotlistEntry, now: Long): Boolean {
        if (e.expiresAt.isBlank()) return true
        val t = runCatching { java.time.OffsetDateTime.parse(e.expiresAt).toInstant().toEpochMilli() }
            .recoverCatching { java.time.Instant.parse(e.expiresAt).toEpochMilli() }.getOrNull() ?: return true
        return t > now
    }

    companion object {
        /** Lattice strings checked per read. */
        const val TOP_N = 16
        /** A candidate this much less likely than the settled text is not worth a notification. */
        const val MIN_RATIO = 0.002

        /** Anchored: same length, every known character equal. Unanchored: the known run appears in the read. */
        fun partialMatch(pat: String, anchored: Boolean, read: String): Boolean =
            if (anchored) pat.length == read.length && pat.indices.all { pat[it] == '*' || pat[it] == read[it] } else read.contains(pat)

        /** The classic confusables folded to one form; only used to keep our own plate out through a misread. */
        fun canonical(s: String): String = buildString { for (c in s) append(when (c) { 'O' -> '0'; 'I' -> '1'; 'B' -> '8'; 'S' -> '5'; 'Z' -> '2'; 'G' -> '6'; else -> c }) }
    }
}

/** A JSON string or number as a string (FalconEyez ids are int64; this keeps the phone tolerant of either). */
object LenientString : kotlinx.serialization.KSerializer<String> {
    override val descriptor = kotlinx.serialization.descriptors.PrimitiveSerialDescriptor("LenientString", kotlinx.serialization.descriptors.PrimitiveKind.STRING)
    override fun serialize(encoder: kotlinx.serialization.encoding.Encoder, value: String) {
        val j = encoder as? kotlinx.serialization.json.JsonEncoder
        val n = value.toLongOrNull()
        if (j != null && n != null) j.encodeJsonElement(kotlinx.serialization.json.JsonPrimitive(n)) else encoder.encodeString(value)
    }
    override fun deserialize(decoder: kotlinx.serialization.encoding.Decoder): String {
        val j = decoder as? kotlinx.serialization.json.JsonDecoder ?: return decoder.decodeString()
        val e = j.decodeJsonElement()
        return (e as? kotlinx.serialization.json.JsonPrimitive)?.content ?: e.toString()
    }
}
