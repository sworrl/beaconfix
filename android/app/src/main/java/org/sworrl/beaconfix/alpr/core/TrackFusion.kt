package org.sworrl.beaconfix.alpr.core

import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.max

/** One OCR read of a tracked plate, weighted by its frame quality [q]. */
data class PlateObs(val read: PlateRead, val q: Float, val atMs: Long)

/**
 * A vehicle's plate fused over its frames. [raw] is the fused lattice's best string, [text] the string after the
 * US-format prior ([PlateFormats.choose]); [lattice] is the fused per-slot top-3 used for hotlist matching.
 */
data class FusedPlate(
    val raw: String,
    val text: String,
    val fit: PlateFormats.Fit,
    /** Mean fused probability of [text]'s characters. */
    val conf: Float,
    val lattice: PlateLattice,
    /** Reads in the winning group. */
    val frames: Int,
    /** Reads whose own text equals [text], and the best confidence among them. */
    val exactReads: Int,
    val exactBestConf: Float,
    /** Best single-read confidence in the winning group. */
    val bestConf: Float,
    val regionProb: Float,
)

/**
 * Multi-frame fusion of one vehicle's reads.
 *
 * 1. Group vote (the idea of Frigate's LPR, MIT): reads are clustered by Jaro-Winkler similarity ≥ [GROUP_SIMILARITY];
 *    the cluster with the most quality weight wins, so one bad frame (a fragment, the car next to it) cannot steer it.
 * 2. Per-character voting inside the winner: for every slot, each character's score is the quality-weighted sum of
 *    log p over the reads (from each read's top-3; a character outside a read's top-3 gets that read's leftover mass),
 *    normalised by the total weight (a weighted geometric mean, so many frames do not make it over-confident; the
 *    frame counts in [AcceptRules] carry that). Only reads of the winning (weighted modal) length vote on slots.
 */
object TrackFusion {
    const val GROUP_SIMILARITY = 0.85

    fun groups(obs: List<PlateObs>): List<List<PlateObs>> {
        val out = ArrayList<MutableList<PlateObs>>()
        for (o in obs.sortedByDescending { it.q }) {
            val g = out.firstOrNull { PlateSimilarity.jaroWinkler(it[0].read.text, o.read.text) >= GROUP_SIMILARITY }
            if (g != null) g += o else out += mutableListOf(o)
        }
        return out
    }

    private fun weight(o: PlateObs) = max(o.q, 0.02f)

    /** The weighted per-slot vote over [obs] (all assumed to be one plate). Null when there is nothing to vote on. */
    fun vote(obs: List<PlateObs>): PlateLattice? {
        val withText = obs.filter { it.read.slots.isNotEmpty() }
        if (withText.isEmpty()) return null
        val len = withText.groupBy { it.read.slots.size }.maxByOrNull { (_, v) -> v.sumOf { weight(it).toDouble() } }!!.key
        val voters = withText.filter { it.read.slots.size == len }
        val lats = voters.map { PlateLattice(it.read.slots) }
        val w = voters.map { weight(it).toDouble() }
        val total = w.sum()
        val slots = (0 until len).map { i ->
            val cands = LinkedHashSet<Char>(); for (v in voters) for (c in v.read.slots[i]) cands += c.c
            val score = cands.associateWith { c -> lats.indices.sumOf { r -> w[r] * ln(lats[r].p(i, c).toDouble()) } / total }
            // everything outside the candidates, as one pooled pseudo-candidate
            val otherLog = lats.indices.sumOf { r -> w[r] * ln(lats[r].p(i, OUTSIDE).toDouble()) } / total
            val others = max(0, PlateLattice.SYMBOLS - cands.size)
            val top = score.values.maxOrNull() ?: otherLog
            val z = score.values.sumOf { exp(it - top) } + others * exp(otherLog - top)
            score.entries.sortedByDescending { it.value }.take(3).map { (c, s) -> SlotChar(c, (exp(s - top) / z).toFloat().r3()) }
        }
        return PlateLattice(slots)
    }

    /** Fuses [obs] (reads that passed the gates) for the state [state] (two-letter code or null). */
    fun fuse(obs: List<PlateObs>, state: String?): FusedPlate? {
        val gs = groups(obs.filter { it.read.text.isNotEmpty() })
        val win = gs.maxWithOrNull(compareBy<List<PlateObs>> { g -> g.sumOf { weight(it).toDouble() } }.thenBy { it.size }) ?: return null
        val lat = vote(win) ?: return null
        val choice = PlateFormats.choose(lat, state)
        val text = choice.text
        val exact = win.filter { it.read.text == text }
        val conf = if (text.isEmpty()) 0f else text.indices.map { lat.p(it, text[it]) }.average().toFloat()
        val rp = win.filter { it.read.regionProb >= 0f }
        val region = if (rp.isEmpty()) -1f else (rp.sumOf { weight(it) * it.read.regionProb.toDouble() } / rp.sumOf { weight(it).toDouble() }).toFloat()
        return FusedPlate(lat.best(), text, choice.fit, conf.r3(), lat, win.size, exact.size, exact.maxOfOrNull { it.read.conf } ?: 0f,
            win.maxOf { it.read.conf }, region)
    }

    /** A character no read proposes for a slot (only its leftover probability applies). */
    private const val OUTSIDE = '\u0000'

    private fun Float.r3() = Math.round(this * 1000f) / 1000f
}

/**
 * When a fused plate is trusted enough to act on (alerts, the plate text of the event). Thresholds after
 * mssdvd/plates-tracker-public `DedupEngine` (MIT), reimplemented: 3 frames with a best read ≥ 0.70; or 2 reads that
 * agree exactly with the result at ≥ 0.75; or 1 read ≥ 0.90 of a valid US format.
 */
object AcceptRules {
    fun accepted(f: FusedPlate): Boolean =
        (f.frames >= 3 && f.bestConf >= 0.70f) ||
            (f.exactReads >= 2 && f.exactBestConf >= 0.75f) ||
            (f.exactReads >= 1 && f.exactBestConf >= 0.90f && PlateFormats.valid(f.text))
}

/**
 * The plates seen recently, one [Group] per vehicle, each fed by its tracks; decides when a vehicle becomes one event.
 * A group lives 10 s after its last sighting, or 60 s once accepted (so a car that drops out of view behind a truck
 * and comes back is still one vehicle), as in plates-tracker's DedupEngine. A new track whose read matches a group
 * that has no live track joins it. [S] is the caller's best-frame payload (crop, thumbnail), kept only until emitted.
 */
class PlateGroups<S>(
    val unacceptedTtlMs: Long = 10_000,
    val acceptedTtlMs: Long = 60_000,
    /** An accepted vehicle still in view after this long is emitted anyway (a car followed for minutes). */
    val holdMs: Long = 20_000,
    val maxObs: Int = 24,
) {
    enum class Reason { HOTLIST, TRACK_END, HOLD, EXPIRED }

    class Group<S> internal constructor(val id: Int, val firstMs: Long) {
        val obs = ArrayList<PlateObs>()
        val tracks = HashSet<Int>()
        val activeTracks = HashSet<Int>()
        var fused: FusedPlate? = null; internal set
        var accepted = false; internal set
        var acceptedMs = 0L; internal set
        var emitted = false; internal set
        var lastMs = firstMs; internal set
        /** Frames where the plate was big enough to read (whether or not the read passed the gates). */
        var readableFrames = 0; internal set
        var bestQ = -1f; internal set
        var shot: S? = null; internal set
        /** One of our own vehicles (set by the caller): never alerted, never uploaded. */
        var own = false
        /** Free for the caller: which alerts were already raised for this vehicle. */
        val alerted = HashSet<String>()
        val text get() = fused?.text.orEmpty()
    }

    class Due<S>(val group: Group<S>, val reason: Reason)

    companion object { const val BEST_MARGIN = 1.05f }

    private val list = ArrayList<Group<S>>()
    private val byTrack = HashMap<Int, Group<S>>()
    private var nextId = 1

    val groups: List<Group<S>> get() = list

    fun groupOf(track: Int): Group<S>? = byTrack[track]

    fun bestQuality(track: Int): Float = byTrack[track]?.bestQ ?: -1f

    /**
     * A readable-size sighting of [track]: [read] is null when the read failed the gates. Returns the group and whether
     * this frame is its best so far (then the caller stores its payload with [setShot]).
     */
    fun observe(track: Int, read: PlateRead?, q: Float, nowMs: Long, state: String?): Pair<Group<S>, Boolean> {
        var g = byTrack[track] ?: (read?.let { attachable(it.text, nowMs) } ?: Group<S>(nextId++, nowMs).also { list += it })
        g.tracks += track; g.activeTracks += track; byTrack[track] = g
        g.lastMs = nowMs; g.readableFrames++
        if (read != null && read.text.isNotEmpty()) {
            val first = g.obs.isEmpty()
            g.obs += PlateObs(read, q, nowMs)
            if (g.obs.size > maxObs) g.obs.remove(g.obs.minBy { it.q })
            if (first && g.tracks.size == 1) g = mergeIntoOlder(g, nowMs)
            refuse(g, nowMs, state)
        }
        val best = g.bestQ < 0f || q > g.bestQ * BEST_MARGIN     // a margin, so the crop is not re-encoded for noise
        if (best) g.bestQ = q
        return g to best
    }

    fun setShot(g: Group<S>, shot: S) { if (!g.emitted) g.shot = shot }

    fun trackEnded(track: Int) { byTrack[track]?.activeTracks?.remove(track) }

    /** Groups to emit now; the caller emits them and calls [markEmitted]. Also forgets expired groups. */
    fun due(nowMs: Long): List<Due<S>> {
        val out = ArrayList<Due<S>>()
        val gone = ArrayList<Group<S>>()
        for (g in list) {
            val idle = g.activeTracks.isEmpty()
            if (!g.emitted) when {
                g.accepted && idle -> out += Due(g, Reason.TRACK_END)
                g.accepted && nowMs - g.acceptedMs >= holdMs -> out += Due(g, Reason.HOLD)
                !g.accepted && idle && nowMs - g.lastMs >= unacceptedTtlMs && worthUnaccepted(g) -> out += Due(g, Reason.EXPIRED)
            }
            if (idle && nowMs - g.lastMs >= (if (g.accepted) acceptedTtlMs else unacceptedTtlMs)) gone += g
        }
        for (g in gone) { list.remove(g); g.tracks.forEach { byTrack.remove(it) } }
        return out
    }

    fun markEmitted(g: Group<S>) { g.emitted = true; g.shot = null }

    /** Capture stopped: every vehicle not yet emitted that is worth an event, as if its track had ended; then forget all. */
    fun flush(): List<Due<S>> {
        val out = list.filter { !it.emitted && (it.accepted || worthUnaccepted(it)) }.map { Due(it, if (it.accepted) Reason.TRACK_END else Reason.EXPIRED) }
        list.clear(); byTrack.clear()
        return out
    }

    /** A vehicle never accepted is still worth one upload (the server reads it again) when seen well enough. */
    private fun worthUnaccepted(g: Group<S>) = g.readableFrames >= 2 || (g.fused?.bestConf ?: 0f) >= 0.5f

    private fun refuse(g: Group<S>, nowMs: Long, state: String?) {
        g.fused = TrackFusion.fuse(g.obs, state)
        val f = g.fused
        if (!g.accepted && f != null && AcceptRules.accepted(f)) { g.accepted = true; g.acceptedMs = nowMs }
    }

    /** A live group with no current track whose text matches [text] (the same car seen again). */
    private fun attachable(text: String, nowMs: Long): Group<S>? {
        if (text.length < PlateFormats.MIN_LEN) return null
        return list.filter { it.activeTracks.isEmpty() && it.text.isNotEmpty() && nowMs - it.lastMs < (if (it.accepted) acceptedTtlMs else unacceptedTtlMs) }
            .map { it to PlateSimilarity.jaroWinkler(it.text, text) }
            .filter { it.second >= TrackFusion.GROUP_SIMILARITY }
            .maxByOrNull { it.second }?.first
    }

    /** [g] (a fresh track's group that just got its first text) folded into an older group of the same plate. */
    private fun mergeIntoOlder(g: Group<S>, nowMs: Long): Group<S> {
        val text = g.obs.first().read.text
        val old = attachable(text, nowMs)?.takeIf { it !== g } ?: return g
        old.obs += g.obs; old.readableFrames += g.readableFrames; old.lastMs = nowMs
        if (g.bestQ > old.bestQ && !old.emitted) { old.bestQ = g.bestQ; old.shot = g.shot }
        for (t in g.tracks) { old.tracks += t; byTrack[t] = old }
        old.activeTracks += g.activeTracks
        list.remove(g)
        return old
    }
}
