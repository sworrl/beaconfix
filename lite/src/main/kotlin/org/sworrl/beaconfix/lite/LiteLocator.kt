package org.sworrl.beaconfix.lite

import java.io.File
import kotlin.math.max
import kotlin.math.min

/**
 * BeaconFix Lite: where this device is, from the Wi-Fi it can hear, for as little battery, network and flash as the
 * answer allows. Blocking and synchronous (call it off the main thread); one instance per cache directory.
 *
 * A [locate] does the least work that still gives a good answer:
 * 1. Same APs as last time ([Config.reuseSimilarity]) and the fix is young enough: return it. No solve, no IO.
 * 2. Enough of the strongest APs are placed in the [BeaconCache]: solve offline ([SelfLocate]).
 * 3. Otherwise ask Apple about the strongest unplaced ones (ten per request, up to [Config.maxLookups] requests),
 *    keep every placement it returns, neighbours included, then solve. BeaconDB when Apple can't place two.
 * 4. Learn: APs the integrity check drops get a strike (two = moved, ignored for a month); heard-but-unplaced APs
 *    near a good fix get a low-weight guess of their own, so a place we've been can be solved with no network.
 * The cache is written once at the end, and only if something changed.
 */
class LiteLocator(
    dir: File,
    http: Http? = UrlHttp(),
    val config: Config = Config(),
) {
    class Config(
        /** Weighted overlap with the last scan at which the last fix is simply reused (0–1). */
        val reuseSimilarity: Double = 0.8,
        /** A reused fix older than this is solved again even when nothing changed. */
        val reuseMaxAgeMs: Long = 6 * 3600_000L,
        /** The strongest this many usable APs vote; weaker ones add almost nothing but noise and work. */
        val maxAps: Int = 24,
        /** Solve offline with at least this many placed APs, if they include half of the six strongest. */
        val offlineMinPlaced: Int = 3,
        /** Apple requests a fix may cost (ten BSSIDs each). */
        val maxLookups: Int = 2,
        /** Learn guesses for unplaced APs only from fixes at least this good (1-sigma, m). */
        val learnFromAccM: Double = 40.0,
    )

    val cache = BeaconCache(dir)
    private val apple = http?.let { AppleWps(it) }
    private val beaconDb = http?.let { BeaconDb(it) }
    private val stateFile = File(dir, "bflite-last.txt")

    @Volatile var last: LiteFix? = null; private set
    private var lastSig: Map<Long, Int> = emptyMap()
    private var stateLoaded = false
    private var stateWrittenAt = 0L
    private var stateWrittenLat = 0.0; private var stateWrittenLon = 0.0

    /**
     * [scan]: what the radio hears now. [travelling]: SSIDs that move with this device (its own router, the network
     * it's joined to). [allowNetwork]: false = cache only (metered link, flight mode). Null = nothing could place us,
     * which is a real answer and must not be faked.
     */
    @Synchronized
    fun locate(scan: List<Heard>, travelling: Collection<String> = emptyList(), allowNetwork: Boolean = true, now: Long = System.currentTimeMillis()): LiteFix? {
        loadState()
        val day = BeaconCache.today(now)
        val usable = scan.asSequence().filter { ApFilter.usable(it, travelling) }
            .filter { cache[it.key]?.kind != BeaconCache.MOVED || cache.shouldAsk(it.key, day) }
            .distinctBy { it.key }.sortedByDescending { it.rssi }.take(config.maxAps).toList()
        if (usable.isEmpty()) return null

        // 1. nothing changed: the last answer stands
        val sig = signature(usable)
        last?.let { l ->
            if (now - l.timeMs < config.reuseMaxAgeMs && similarity(sig, lastSig) >= config.reuseSimilarity)
                return l.copy(source = "reuse", lookups = 0, heard = scan.size)
        }

        // 2./3. placed APs, asking for more only when too few of the strong ones are known
        var lookups = 0
        fun placedAps() = usable.filter { cache.placed(it.key) != null }
        var placed = placedAps()
        val strongest = usable.take(6)
        val strongPlaced = strongest.count { cache.placed(it.key) != null }
        if ((placed.size < config.offlineMinPlaced || strongPlaced * 2 < strongest.size) && allowNetwork && apple != null) {
            val ask = usable.filter { cache.shouldAsk(it.key, day) }.map { it.key }
            for (batch in ask.chunked(AppleWps.BATCH)) {
                if (lookups >= config.maxLookups) break
                val got = apple.lookup(batch); lookups++
                val returned = HashSet<Long>()
                for (p in got) { cache.put(p.key, p.lat, p.lon, p.accM, BeaconCache.SERVICE, day); returned += p.key }
                for (k in batch) if (k !in returned) cache.markUnknown(k, day)
                placed = placedAps()
                if (placed.size >= max(config.offlineMinPlaced, 4)) break
            }
        }

        var fix: LiteFix? = null
        if (placed.size >= 2) fix = solve(usable, placed, scan.size, lookups, now, day)
        else if (placed.size == 1) fix = single(placed[0], scan.size, lookups, now)
        if ((fix == null || placed.size < 2) && allowNetwork && beaconDb != null && usable.size >= 2) {
            beaconDb.locate(usable)?.let { (lat, lon, acc) ->
                lookups++
                fix = LiteFix(lat, lon, acc, acc * 2.45, "beacondb", "n/a", scan.size, usable.size, 0, lookups, now)
            }
        }
        val f = fix
        if (f != null) {
            learn(usable, f, day)
            last = f; lastSig = sig
            saveState(f, sig, now)
        }
        cache.flush()
        return f
    }

    private fun solve(usable: List<Heard>, placed: List<Heard>, heard: Int, lookups: Int, now: Long, day: Int): LiteFix? {
        var aps = placed
        // a mapped position kilometres from the rest is an AP that moved or travels: out before the solve. (With 4+
        // the integrity check would catch it too, but it can't with 2 or 3, and it costs a solve round either way.)
        if (aps.size >= 2) {
            val med = medianPoint(aps)
            val far = aps.filter { val e = cache.placed(it.key)!!; Geo.distanceM(med.first, med.second, e.lat, e.lon) > 3000 + e.accM }
            if (far.isNotEmpty() && aps.size - far.size >= 2) { for (h in far) cache.strike(h.key, day); aps = aps - far.toSet() }
        }
        val known = aps.map { h -> val e = cache.placed(h.key)!!; SelfLocate.Known(e.lat, e.lon, e.accM.toDouble(), h.rssi, h.freqMhz, BeaconCache.weight(e.kind)) }
        val r = SelfLocate.solve(known)
        if (!r.valid) return null
        val dropped = r.excluded.map { aps[it].key }.toSet()
        for (k in dropped) cache.strike(k, day)
        if (r.integrity == "ok" || r.integrity == "repaired") for (h in aps) if (h.key !in dropped) cache.clearStrike(h.key)
        val source = if (lookups > 0) "apple" else "cache"
        return LiteFix(r.lat, r.lon, r.accM, r.r95M, source, r.integrity, heard, r.used, r.rejected, lookups, now)
    }

    /** One placed AP: we're within radio range of it, which is all it can say. */
    private fun single(h: Heard, heard: Int, lookups: Int, now: Long): LiteFix {
        val e = cache.placed(h.key)!!
        val acc = max(e.accM.toDouble(), SelfLocate.distanceFor(h.rssi, h.freqMhz)) + 20.0
        return LiteFix(e.lat, e.lon, acc, acc * 2.45, if (lookups > 0) "apple" else "cache", "unverified", heard, 1, 0, lookups, now)
    }

    /**
     * A good fix places the APs nobody else could, roughly: at the fix, as far off as their level says. Weighted low
     * (0.3) and never sent anywhere; enough to solve a revisit offline when the services don't know the street.
     */
    private fun learn(usable: List<Heard>, f: LiteFix, day: Int) {
        if (f.accM > config.learnFromAccM || f.source == "reuse" || f.source == "beacondb") return
        if (f.integrity == "failed") return
        for (h in usable) {
            val e = cache[h.key]
            if (e != null && e.kind != BeaconCache.UNKNOWN && e.kind != BeaconCache.LEARNED) continue
            val acc = f.accM + SelfLocate.distanceFor(h.rssi, h.freqMhz)
            if (e != null && e.kind == BeaconCache.LEARNED) {
                // average towards the new sighting, the closer (louder) one counting more; no write for a small move
                val w = min(0.5, e.accM / (e.accM + acc))
                val lat = e.lat + w * (f.lat - e.lat); val lon = e.lon + w * (f.lon - e.lon)
                if (Geo.distanceM(e.lat, e.lon, lat, lon) > 5 || acc < e.accM) cache.put(h.key, lat, lon, min(e.accM.toDouble(), acc), BeaconCache.LEARNED, day)
            } else cache.put(h.key, f.lat, f.lon, acc, BeaconCache.LEARNED, day)
        }
    }

    private fun medianPoint(aps: List<Heard>): Pair<Double, Double> {
        val lats = aps.map { cache.placed(it.key)!!.lat }.sorted(); val lons = aps.map { cache.placed(it.key)!!.lon }.sorted()
        return lats[lats.size / 2] to lons[lons.size / 2]
    }

    // ── what "the same place" means: overlap of the strong APs, weighted by how loud they are ──

    private fun signature(aps: List<Heard>): Map<Long, Int> = aps.take(12).associate { it.key to (it.rssi + 100).coerceAtLeast(1) }

    /** Weighted Jaccard: sum of the smaller weights over sum of the larger, over the union of both scans. */
    fun similarity(a: Map<Long, Int>, b: Map<Long, Int>): Double {
        if (a.isEmpty() || b.isEmpty()) return 0.0
        var lo = 0.0; var hi = 0.0
        for ((k, w) in a) { val v = b[k] ?: 0; lo += min(w, v); hi += max(w, v) }
        for ((k, v) in b) if (k !in a) hi += v
        return if (hi <= 0) 0.0 else lo / hi
    }

    // ── the last fix survives a restart (a few hundred bytes, rewritten only when the answer moved) ──

    private fun loadState() {
        if (stateLoaded) return
        stateLoaded = true
        runCatching {
            if (!stateFile.exists()) return
            val lines = stateFile.readLines()
            val p = lines[0].split(' ')
            val f = LiteFix(p[0].toDouble(), p[1].toDouble(), p[2].toDouble(), p[3].toDouble(), p[4], p[5], p[6].toInt(), p[7].toInt(), p[8].toInt(), 0, p[9].toLong())
            val sig = HashMap<Long, Int>()
            for (l in lines.drop(1)) { val q = l.split(' '); if (q.size == 2) sig[q[0].toLong(16)] = q[1].toInt() }
            last = f; lastSig = sig
            stateWrittenAt = f.timeMs; stateWrittenLat = f.lat; stateWrittenLon = f.lon
        }
    }

    private fun saveState(f: LiteFix, sig: Map<Long, Int>, now: Long) {
        if (now - stateWrittenAt < 3600_000L && Geo.distanceM(stateWrittenLat, stateWrittenLon, f.lat, f.lon) < 25) return
        runCatching {
            stateFile.parentFile?.mkdirs()
            val sb = StringBuilder()
            sb.append(String.format(java.util.Locale.US, "%.7f %.7f %.1f %.1f %s %s %d %d %d %d\n", f.lat, f.lon, f.accM, f.r95M, f.source, f.integrity, f.heard, f.used, f.excluded, f.timeMs))
            for ((k, w) in sig) sb.append(java.lang.Long.toHexString(k)).append(' ').append(w).append('\n')
            val tmp = File(stateFile.parentFile, stateFile.name + ".tmp")
            tmp.writeText(sb.toString())
            if (!tmp.renameTo(stateFile)) { stateFile.delete(); tmp.renameTo(stateFile) }
            stateWrittenAt = now; stateWrittenLat = f.lat; stateWrittenLon = f.lon
        }
    }

    /**
     * A position this device trusts more than its own (a GPS fix outdoors, a BeaconFix desktop on the same LAN): the
     * APs heard now that no service placed are learned around it, so the next fix here is solved offline. It also
     * becomes the last fix (no lookup needed for the same scan).
     */
    @Synchronized
    fun teach(scan: List<Heard>, lat: Double, lon: Double, accM: Double, travelling: Collection<String> = emptyList(), now: Long = System.currentTimeMillis()) {
        if (!Geo.valid(lat, lon) || accM <= 0) return
        loadState()
        val usable = scan.filter { ApFilter.usable(it, travelling) }.distinctBy { it.key }.sortedByDescending { it.rssi }.take(config.maxAps)
        if (usable.isEmpty()) return
        val f = LiteFix(lat, lon, accM, accM * 2.45, "taught", "n/a", scan.size, usable.size, 0, 0, now)
        learn(usable, f, BeaconCache.today(now))
        last = f; lastSig = signature(usable)
        saveState(f, lastSig, now)
        cache.flush()
    }

    /** Placements from elsewhere (a BeaconFix desktop's export, a neighbour's survey): bssid → (lat, lon, accuracy). */
    @Synchronized
    fun import(placements: Map<String, Triple<Double, Double, Double>>): Int {
        var n = 0
        for ((b, p) in placements) if (cache.put(Mac.key(b), p.first, p.second, p.third, BeaconCache.IMPORTED)) n++
        cache.flush()
        return n
    }
}
