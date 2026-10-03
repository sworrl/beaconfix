// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import org.sworrl.beaconfix.data.api.PlateEventDto
import org.sworrl.beaconfix.data.db.PlateEventEntity
import org.sworrl.beaconfix.data.db.PlateEventMediaEntity

/**
 * Where a plate event is pushed to and pulled from. Each has its own "waiting to send" flag and its own feed cursor:
 * a LAN desktop takes the record and its images ([PlateEventEntity.dirty]); the hub (`/api/v3` `db/sync`, `db/changes`)
 * takes the record only ([PlateEventEntity.hubDirty]) — the hub host's disk is small, so images wait for a desktop.
 */
enum class PlateDest { LAN, HUB }

/** The rows the §1.1 bookkeeping needs: the Room DAO in the app, a map in the unit tests. */
interface PlateRows {
    suspend fun byUid(uid: String): PlateEventEntity?
    /** The `camera_pass` of [cameraId] in [fromMs, toMs] closest to [atMs]. */
    suspend fun passNear(cameraId: String, fromMs: Long, toMs: Long, atMs: Long): PlateEventEntity?
    suspend fun insert(e: PlateEventEntity): Long
    suspend fun update(e: PlateEventEntity)
    suspend fun delete(uid: String)
    suspend fun moveMedia(from: String, to: String)
    suspend fun insertMedia(m: PlateEventMediaEntity): Long
}

/**
 * Plate events stored once, whichever way they arrive (docs/SIGHTINGS.md §1.1): the same uid, else a pass of the same
 * camera within ±10 min, is the same event — merged, never duplicated. A row that arrives a second time (over the
 * LAN after the hub, or the other way round) is not new, so it is never announced twice. The caller holds the lock.
 */
class PlateLedger(private val rows: PlateRows) {
    suspend fun findSame(e: PlateEventEntity): PlateEventEntity? = rows.byUid(e.uid)
        ?: if (e.kind == PlateEvents.CAMERA_PASS && e.cameraId != null && e.timeMs > 0)
            rows.passNear(e.cameraId, e.timeMs - PassDetector.MERGE_WINDOW_MS, e.timeMs + PassDetector.MERGE_WINDOW_MS, e.timeMs) else null

    /** An event recorded (or re-recorded) here: stored merged, and waiting for every destination when it says something new. */
    suspend fun upsertLocal(e: PlateEventEntity): PlateEventEntity {
        val old = findSame(e)
        if (old == null) { val row = e.copy(id = 0, dirty = true, hubDirty = true); val id = rows.insert(row); return row.copy(id = id) }
        val merged = PlateEvents.merge(old, e)
        if (!PlateEvents.changed(old, merged)) return old
        val row = merged.copy(dirty = true, hubDirty = true)
        rows.update(row)
        return row
    }

    /**
     * Events from a feed of [dest] into the rows (merged per §1.1); returns the ones new to this phone. The feed's
     * copy is the reference for [dest] only: [dest]'s flag stays set only when ours carried more than it had (a higher
     * source rank); the other destination's flag is left alone, so a pass the hub already has still goes to the
     * desktop (with its images), and the other way round. Images are listed from a LAN desktop only.
     */
    suspend fun ingest(list: List<PlateEventDto>, dest: PlateDest): List<PlateEventEntity> {
        val fresh = ArrayList<PlateEventEntity>()
        for (dto in list) {
            if (dto.uid.isEmpty()) continue
            val incoming0 = PlateEvents.fromDto(dto)
            // the LAN feed's seq is the desktop's (shown as "feed seq"); the hub's counts in another sequence
            val incoming = if (dest == PlateDest.LAN) incoming0 else incoming0.copy(seq = 0)
            var old = findSame(incoming)
            if (old != null && old.uid != incoming.uid) { rename(old.uid, incoming.uid); old = rows.byUid(incoming.uid) }
            val row = if (old == null) {
                val id = rows.insert(incoming); fresh += incoming.copy(id = id); incoming
            } else {
                val next = settle(old, incoming, dest)
                if (next != old) rows.update(next); next
            }
            if (dest == PlateDest.LAN) for (md in dto.media.orEmpty()) {
                if (md.uid.isEmpty()) continue
                val camPhoto = md.kind == "camera_photo"
                rows.insertMedia(PlateEventMediaEntity(uid = md.uid, eventUid = md.eventUid ?: if (camPhoto) null else row.uid, cameraId = md.cameraId ?: if (camPhoto) row.cameraId else null,
                    kind = md.kind.ifEmpty { "dashcam" }, mime = md.mime.ifEmpty { "image/png" }, width = md.width, height = md.height, bytes = md.bytes, attribution = md.attribution,
                    license = md.license, capturedAt = md.capturedAt, originalUrl = md.originalUrl, remote = true))
            }
        }
        return fresh
    }

    /** [from] is known as [to] now (a desktop or the hub merged it into its own pass): one row, the flags of both. */
    suspend fun rename(from: String, to: String) {
        val a = rows.byUid(from) ?: return
        val b = rows.byUid(to)
        if (b != null) {
            rows.update(PlateEvents.merge(b, a).copy(dirty = b.dirty || a.dirty, hubDirty = b.hubDirty || a.hubDirty, notified = a.notified || b.notified))
            rows.delete(from)
        } else rows.update(a.copy(uid = to))
        rows.moveMedia(from, to)
    }

    companion object {
        /** [old] merged with [dest]'s copy [incoming]: the row to store (pure; see [ingest]). */
        fun settle(old: PlateEventEntity, incoming: PlateEventEntity, dest: PlateDest): PlateEventEntity {
            val m = PlateEvents.merge(old, incoming)
            val oursRanksHigher = PlateEvents.sourceRank(old.source) > PlateEvents.sourceRank(incoming.source)
            return m.copy(
                seq = if (dest == PlateDest.LAN) incoming.seq else old.seq,
                raw = incoming.raw ?: old.raw,
                dirty = if (dest == PlateDest.LAN) old.dirty && oursRanksHigher else old.dirty,
                hubDirty = if (dest == PlateDest.HUB) old.hubDirty && oursRanksHigher else old.hubDirty,
            )
        }
    }
}
