package org.sworrl.beaconfix.alpr.core

import java.io.File

/**
 * The on-disk overflow for plate events that cannot be uploaded yet (server out of reach, or the network policy says
 * wait). Each event is two files, `<createdMs>_<id>_<h|p>.jpg` (the crop) and `.json` (its meta), written via a temp
 * name and renamed, the meta last, so a crash never leaves a half event. Bounded: [capBytes] total, oldest dropped
 * first; unsent ordinary events expire after [maxAgeMs]. Hotlist events (evidence) never expire or get dropped, but
 * count toward the cap. Nothing else is ever written here, and an entry is deleted as soon as it uploads.
 */
class SpillQueue(private val dir: File) {
    data class Entry(val id: String, val createdMs: Long, val hotlist: Boolean, val jpg: File, val json: File) {
        val bytes get() = jpg.length() + json.length()
    }

    init { dir.mkdirs(); cleanup() }

    @Synchronized fun put(e: PendingEvent): Entry {
        val base = "${e.createdMs}_${e.id}_${if (e.hotlist) "h" else "p"}"
        val jpg = File(dir, "$base.jpg"); val json = File(dir, "$base.json")
        val tj = File(dir, "$base.jpg.tmp"); tj.writeBytes(e.jpeg); if (!tj.renameTo(jpg)) { tj.delete(); error("spill: rename failed") }
        val tm = File(dir, "$base.json.tmp"); tm.writeText(AlprJson.json.encodeToString(FrameMeta.serializer(), e.meta))
        if (!tm.renameTo(json)) { tm.delete(); jpg.delete(); error("spill: rename failed") }
        return Entry(e.id, e.createdMs, e.hotlist, jpg, json)
    }

    /** Oldest first, hotlist events ahead of ordinary ones. */
    @Synchronized fun list(): List<Entry> = dir.listFiles { f -> f.name.endsWith(".json") }.orEmpty().mapNotNull { parse(it) }
        .sortedWith(compareByDescending<Entry> { it.hotlist }.thenBy { it.createdMs })

    @Synchronized fun load(e: Entry): PendingEvent? = runCatching {
        PendingEvent(e.id, e.createdMs, AlprJson.json.decodeFromString(FrameMeta.serializer(), e.json.readText()), e.jpg.readBytes())
    }.getOrNull()

    @Synchronized fun remove(id: String) { dir.listFiles { f -> f.name.contains("_${id}_") }.orEmpty().forEach { it.delete() } }

    @Synchronized fun totalBytes(): Long = dir.listFiles().orEmpty().sumOf { it.length() }

    @Synchronized fun count(): Int = dir.listFiles { f -> f.name.endsWith(".json") }?.size ?: 0

    /**
     * Drop expired ordinary events, then the oldest ordinary events while over [capBytes]. Returns how many were dropped.
     * Hotlist events are never dropped here.
     */
    @Synchronized fun enforce(capBytes: Long, maxAgeMs: Long, now: Long): Int {
        var dropped = 0
        val all = list().toMutableList()
        all.filter { !it.hotlist && now - it.createdMs > maxAgeMs }.forEach { remove(it.id); all.remove(it); dropped++ }
        var total = all.sumOf { it.bytes }
        for (e in all.filter { !it.hotlist }.sortedBy { it.createdMs }) {
            if (total <= capBytes) break
            total -= e.bytes; remove(e.id); dropped++
        }
        return dropped
    }

    @Synchronized fun clear() { dir.listFiles().orEmpty().forEach { it.delete() } }

    /** Temp files from an interrupted write and crops without meta. */
    @Synchronized fun cleanup() {
        val files = dir.listFiles().orEmpty()
        files.filter { it.name.endsWith(".tmp") }.forEach { it.delete() }
        val metas = files.filter { it.name.endsWith(".json") }.map { it.name.removeSuffix(".json") }.toSet()
        files.filter { it.name.endsWith(".jpg") && it.name.removeSuffix(".jpg") !in metas }.forEach { it.delete() }
    }

    private fun parse(json: File): Entry? {
        val base = json.name.removeSuffix(".json")
        val parts = base.split('_')
        if (parts.size != 3) return null
        val ms = parts[0].toLongOrNull() ?: return null
        val jpg = File(dir, "$base.jpg")
        if (!jpg.exists()) return null
        return Entry(parts[1], ms, parts[2] == "h", jpg, json)
    }
}
