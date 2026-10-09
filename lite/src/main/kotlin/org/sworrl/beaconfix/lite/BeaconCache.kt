package org.sworrl.beaconfix.lite

import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.EOFException
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream

/**
 * What this device knows about the APs around it: where each one is, how sure that is, and which ones a service
 * doesn't know or which turned out to have moved, so they aren't asked about again.
 *
 * Built for little IO on weak flash:
 * - fixed 20-byte records (`bflite-aps.bin`), 6-byte MAC keys, positions in 1e-7 degrees (~1 cm);
 * - changes are appended to a journal (`bflite-aps.jnl`) only at [flush], and only when something changed; the
 *   journal is folded into the main file (written to a temp file, then renamed) once it holds a quarter as many
 *   records as the main file, so a crash mid-write loses at most the last change and never the cache;
 * - loaded once, on first use; the services' answers include ~100 neighbours per request, so a few lookups fill
 *   a town and later fixes there need no network at all.
 *
 * Not thread-safe by itself: [LiteLocator] calls it under its own lock.
 */
class BeaconCache(private val dir: File, private val maxEntries: Int = 30_000) {
    class Entry(var latE7: Int, var lonE7: Int, var accM: Int, var kind: Int, var strikes: Int, var day: Int) {
        val lat get() = latE7 / 1e7
        val lon get() = lonE7 / 1e7
        val placed get() = kind == SERVICE || kind == IMPORTED || kind == LEARNED
    }

    companion object {
        /** Placed by a lookup service (Apple, BeaconDB): fully trusted. */
        const val SERVICE = 1
        /** Imported from a BeaconFix database: trusted a little less (its position may be our own estimate). */
        const val IMPORTED = 2
        /** Placed by this device from its own fixes: a rough guess, weighted low, never sent anywhere. */
        const val LEARNED = 3
        /** Asked about, and nobody knows it: not asked again for [RETRY_DAYS]. */
        const val UNKNOWN = 4
        /** Its mapped position kept contradicting the others (it moved, or travels): ignored for [RETRY_DAYS]. */
        const val MOVED = 5

        const val RETRY_DAYS = 30
        /** A service placement older than this is asked about again (APs do move house). */
        const val REFRESH_DAYS = 365
        private const val RECORD = 20
        private const val MAGIC = 0x42464c31 // "BFL1"
        private const val EPOCH_DAY_2020 = 18262L

        fun today(nowMs: Long = System.currentTimeMillis()): Int = (nowMs / 86_400_000L - EPOCH_DAY_2020).toInt().coerceIn(0, 65535)

        fun weight(kind: Int): Double = when (kind) { SERVICE -> 1.0; IMPORTED -> 0.8; LEARNED -> 0.3; else -> 0.0 }
    }

    private val main = File(dir, "bflite-aps.bin")
    private val journal = File(dir, "bflite-aps.jnl")
    private val map = HashMap<Long, Entry>()
    private val changed = LinkedHashSet<Long>()
    private var journalRecords = 0
    private var loaded = false

    val size: Int get() { load(); return map.size }
    fun placedCount(): Int { load(); return map.values.count { it.placed } }

    operator fun get(key: Long): Entry? { load(); return map[key] }

    /** A position worth solving with (not unknown, not moved). */
    fun placed(key: Long): Entry? = get(key)?.takeIf { it.placed }

    /** Worth a network request: never asked, or the last answer (unknown / moved / an old placement) has aged out. */
    fun shouldAsk(key: Long, day: Int = today()): Boolean {
        val e = get(key) ?: return true
        return when (e.kind) {
            UNKNOWN, MOVED -> day - e.day >= RETRY_DAYS
            SERVICE -> day - e.day >= REFRESH_DAYS
            LEARNED -> true            // ours is a guess: a real placement is better whenever we're asking anyway
            else -> false
        }
    }

    /** Record a placement; returns false (and writes nothing) when it is what we have already. */
    fun put(key: Long, lat: Double, lon: Double, accM: Double, kind: Int, day: Int = today()): Boolean {
        if (key < 0 || !Geo.valid(lat, lon)) return false
        load()
        val la = Math.round(lat * 1e7).toInt(); val lo = Math.round(lon * 1e7).toInt()
        val ac = Math.round(accM).toInt().coerceIn(1, 65535)
        val e = map[key]
        if (e != null) {
            // a service placement is never replaced by an import or our own guess; an unchanged one isn't rewritten
            if (e.kind == SERVICE && kind != SERVICE) return false
            if (e.kind == IMPORTED && kind == LEARNED) return false
            if (e.kind == kind && kotlin.math.abs(e.latE7 - la) < 100 && kotlin.math.abs(e.lonE7 - lo) < 100 && e.accM == ac && day - e.day < 30) return false
            e.latE7 = la; e.lonE7 = lo; e.accM = ac; e.kind = kind; e.strikes = 0; e.day = day
        } else map[key] = Entry(la, lo, ac, kind, 0, day)
        changed += key
        return true
    }

    fun markUnknown(key: Long, day: Int = today()) {
        if (key < 0) return
        load()
        val e = map[key]
        if (e != null && e.placed) return
        if (e == null) map[key] = Entry(0, 0, 0, UNKNOWN, 0, day) else { e.kind = UNKNOWN; e.day = day }
        changed += key
    }

    /** The integrity check dropped this AP. Twice in a row and it's treated as moved. */
    fun strike(key: Long, day: Int = today()) {
        val e = get(key) ?: return
        if (!e.placed) return
        e.strikes++
        if (e.strikes >= 2) { e.kind = MOVED; e.day = day }
        changed += key
    }

    /** It agreed with the others: forget an earlier strike (no write when there was none). */
    fun clearStrike(key: Long) {
        val e = get(key) ?: return
        if (e.strikes == 0) return
        e.strikes = 0
        changed += key
    }

    /** Write what changed since the last flush. Returns the number of records written. */
    fun flush(): Int {
        if (changed.isEmpty()) return 0
        dir.mkdirs()
        val n = changed.size
        if (journalRecords + n > maxOf(1000, map.size / 4) || map.size > maxEntries) {
            compact()
        } else {
            DataOutputStream(BufferedOutputStream(FileOutputStream(journal, true), 8192)).use { out ->
                for (k in changed) map[k]?.let { write(out, k, it) }
            }
            journalRecords += n
        }
        changed.clear()
        return n
    }

    /** Fold the journal into the main file (and drop the oldest entries past [maxEntries]). */
    fun compact() {
        load()
        if (map.size > maxEntries) {
            val drop = map.entries.sortedWith(compareBy<Map.Entry<Long, Entry>>({ if (it.value.placed) 1 else 0 }, { it.value.day }))
                .take(map.size - maxEntries * 9 / 10).map { it.key }
            for (k in drop) map.remove(k)
        }
        dir.mkdirs()
        val tmp = File(dir, "bflite-aps.bin.tmp")
        DataOutputStream(BufferedOutputStream(FileOutputStream(tmp), 65536)).use { out ->
            out.writeInt(MAGIC); out.writeInt(map.size)
            for ((k, e) in map) write(out, k, e)
        }
        if (!tmp.renameTo(main)) { main.delete(); tmp.renameTo(main) }
        journal.delete()
        journalRecords = 0
        changed.clear()
    }

    private fun write(out: DataOutputStream, k: Long, e: Entry) {
        out.writeShort((k ushr 32).toInt()); out.writeInt(k.toInt())
        out.writeInt(e.latE7); out.writeInt(e.lonE7)
        out.writeShort(e.accM); out.writeByte(e.kind); out.writeByte(e.strikes.coerceAtMost(127)); out.writeShort(e.day)
    }

    private fun read(inp: DataInputStream): Pair<Long, Entry> {
        val hi = inp.readUnsignedShort().toLong(); val lo = inp.readInt().toLong() and 0xffffffffL
        val e = Entry(inp.readInt(), inp.readInt(), inp.readUnsignedShort(), inp.readUnsignedByte(), inp.readUnsignedByte(), inp.readUnsignedShort())
        return ((hi shl 32) or lo) to e
    }

    private fun load() {
        if (loaded) return
        loaded = true
        runCatching {
            if (main.exists()) DataInputStream(BufferedInputStream(FileInputStream(main), 65536)).use { inp ->
                if (inp.readInt() != MAGIC) return@use
                val n = inp.readInt()
                for (i in 0 until n) { val (k, e) = read(inp); map[k] = e }
            }
        }
        // the journal: later records win; a torn last record (power cut mid-append) is ignored
        runCatching {
            if (journal.exists()) DataInputStream(BufferedInputStream(FileInputStream(journal), 16384)).use { inp ->
                val n = journal.length() / RECORD
                for (i in 0 until n) { val (k, e) = try { read(inp) } catch (_: EOFException) { break }; map[k] = e; journalRecords++ }
            }
        }
    }
}
