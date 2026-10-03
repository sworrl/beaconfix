package org.sworrl.beaconfix.data.db

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import androidx.room.Transaction
import androidx.room.Upsert
import kotlinx.coroutines.flow.Flow

@Dao
interface ApDao {
    @Upsert suspend fun upsert(ap: ApEntity)
    @Upsert suspend fun upsertAll(aps: List<ApEntity>)
    @Query("SELECT * FROM aps WHERE bssid = :bssid") suspend fun get(bssid: String): ApEntity?
    @Query("SELECT * FROM aps ORDER BY lastSeen DESC") fun all(): Flow<List<ApEntity>>
    @Query("SELECT * FROM aps WHERE lat IS NOT NULL") fun positioned(): Flow<List<ApEntity>>
    @Query("SELECT * FROM aps WHERE lat IS NOT NULL") suspend fun positionedNow(): List<ApEntity>
    @Query("SELECT COUNT(*) FROM aps") fun count(): Flow<Int>
    @Query("SELECT COUNT(*) FROM aps WHERE lat IS NOT NULL") fun positionedCount(): Flow<Int>
    @Query("UPDATE aps SET lat=:lat, lon=:lon, acc=:acc, posSource=:src, refDbm=:refDbm, pathExp=:pathExp, residual=:residual WHERE bssid=:bssid")
    suspend fun setPosition(bssid: String, lat: Double, lon: Double, acc: Double, src: String, refDbm: Double?, pathExp: Double?, residual: Double?)
    /** Our own graded estimate (EstimateRepository): the position and every grading column at once. */
    @Query("UPDATE aps SET lat=:lat, lon=:lon, acc=:acc, posSource=:src, refDbm=:refDbm, pathExp=:pathExp, residual=:residual, fitKind=:fitKind, grade=:grade, score=:score, " +
        "r95=:r95, cep50=:cep50, pWithin25=:pWithin25, cxx=:cxx, cxy=:cxy, cyy=:cyy, semiMajor=:semiMajor, semiMinor=:semiMinor, orient=:orient, vantage=:vantage, devices=:devices, " +
        "fitMetrics=:fitMetrics, gradedAt=:gradedAt WHERE bssid=:bssid")
    suspend fun setEstimate(bssid: String, lat: Double, lon: Double, acc: Double, src: String, refDbm: Double?, pathExp: Double?, residual: Double?,
                            fitKind: String?, grade: String?, score: Double?, r95: Double?, cep50: Double?, pWithin25: Double?, cxx: Double?, cxy: Double?, cyy: Double?,
                            semiMajor: Double?, semiMinor: Double?, orient: Double?, vantage: Int?, devices: Int?, fitMetrics: String?, gradedAt: Long?)
    /** A grade without a position change (kind mobile: "travels with you"). */
    @Query("UPDATE aps SET fitKind=:fitKind, grade=:grade, score=:score, vantage=:vantage, devices=:devices, fitMetrics=:fitMetrics, gradedAt=:gradedAt WHERE bssid=:bssid")
    suspend fun setGrade(bssid: String, fitKind: String?, grade: String?, score: Double?, vantage: Int?, devices: Int?, fitMetrics: String?, gradedAt: Long?)
    @Query("UPDATE aps SET home=:home WHERE bssid IN (:bssids)") suspend fun setHome(bssids: List<String>, home: Boolean)
    @Query("UPDATE aps SET home=0") suspend fun clearHome()
    @Query("SELECT bssid FROM aps") suspend fun allBssids(): List<String>
}

@Dao
interface ObservationDao {
    @Insert(onConflict = OnConflictStrategy.IGNORE) suspend fun insertAll(rows: List<ObservationEntity>): List<Long>
    @Insert suspend fun insert(row: ObservationEntity): Long
    @Query("SELECT * FROM observations WHERE bssid = :bssid ORDER BY time") suspend fun forAp(bssid: String): List<ObservationEntity>
    @Query("SELECT COUNT(*) FROM observations WHERE bssid = :bssid") suspend fun countFor(bssid: String): Int
    @Query("SELECT * FROM observations WHERE synced = 0 AND remote = 0 ORDER BY id LIMIT :limit") suspend fun unsynced(limit: Int): List<ObservationEntity>
    @Query("SELECT COUNT(*) FROM observations WHERE synced = 0 AND remote = 0") fun unsyncedCount(): Flow<Int>
    @Query("SELECT COUNT(*) FROM observations") fun count(): Flow<Int>
    @Query("UPDATE observations SET synced = 1 WHERE id IN (:ids)") suspend fun markSynced(ids: List<Long>)
    @Query("SELECT DISTINCT bssid FROM observations WHERE id > :sinceId") suspend fun touchedSince(sinceId: Long): List<String>
    @Query("SELECT MAX(id) FROM observations") suspend fun maxId(): Long?
    /** This phone's own rows (not pulled, not imported from a file) after [sinceId], oldest first: its scans for the misses. */
    @Query("SELECT id, time, lat, lon, acc FROM observations WHERE remote = 0 AND source NOT LIKE 'import:%' AND id > :sinceId ORDER BY id LIMIT :limit")
    suspend fun ownScanRows(sinceId: Long, limit: Int): List<ScanRow>
    /** Each AP's newest RTT range since [since] (ms; the time index keeps it cheap). SQLite takes the bare columns from the MAX(time) row. */
    @Query("SELECT bssid, rangeM, rangeSd, MAX(time) AS time FROM observations WHERE time > :since AND rangeM IS NOT NULL GROUP BY bssid") suspend fun latestRanges(since: Long): List<ApRangeRow>
    @Query("SELECT COUNT(*) FROM observations WHERE bssid=:bssid AND ABS(time - :time) < 1500 AND ABS(lat - :lat) < 0.00001 AND ABS(lon - :lon) < 0.00001") suspend fun duplicates(bssid: String, time: Long, lat: Double, lon: Double): Int
}

@Dao
interface FixDao {
    @Insert suspend fun insert(fix: FixEntity)
    @Query("SELECT * FROM fixes ORDER BY time DESC LIMIT 1") fun latest(): Flow<FixEntity?>
    @Query("SELECT * FROM fixes WHERE source LIKE 'phone%' ORDER BY time DESC LIMIT 1") suspend fun latestPhone(): FixEntity?
    @Query("SELECT * FROM fixes WHERE time > :since ORDER BY time") fun since(since: Long): Flow<List<FixEntity>>
    @Query("SELECT * FROM fixes WHERE source='desktop' ORDER BY time DESC LIMIT 500") fun desktopTrack(): Flow<List<FixEntity>>
    @Query("DELETE FROM fixes WHERE time < :before") suspend fun prune(before: Long)
    @Query("SELECT * FROM fixes WHERE source LIKE 'phone%' ORDER BY time DESC LIMIT 1") suspend fun lastPhone(): FixEntity?
    @Query("SELECT * FROM fixes WHERE source = 'desktop' ORDER BY time DESC LIMIT 1") suspend fun lastDesktop(): FixEntity?
    @Query("SELECT EXISTS(SELECT 1 FROM fixes WHERE source = 'desktop' AND time = :time)") suspend fun existsDesktopAt(time: Long): Boolean
    @Query("SELECT * FROM fixes WHERE source NOT LIKE 'desktop%' AND time > :t ORDER BY time") suspend fun phoneSince(t: Long): List<FixEntity>
    @Query("SELECT * FROM fixes WHERE source NOT LIKE 'desktop%' AND time > :since ORDER BY time ASC LIMIT :limit") suspend fun phoneFixesSince(since: Long, limit: Int = 200): List<FixEntity>
    @Query("SELECT * FROM fixes WHERE source NOT LIKE 'desktop%' ORDER BY time ASC") suspend fun allPhoneFixes(): List<FixEntity>
    @Query("INSERT OR IGNORE INTO fixes (time, lat, lon, acc, source, provider) SELECT DISTINCT time, lat, lon, acc, 'phone-gps', 'fused' FROM observations WHERE lat != 0.0 AND lon != 0.0") suspend fun backfillFromObservations()
    /** Collapse desktop fixes pulled more than once (same timestamp) to the first copy; returns the rows removed. */
    @Query(DEDUPE_DESKTOP_FIXES) suspend fun dedupeDesktop(): Int
}

/** The last estimates per AP (schema v5), newest first; [append] keeps at most [ESTIMATE_HISTORY_KEEP] rows per BSSID. */
@Dao
interface EstimateHistoryDao {
    @Insert suspend fun insert(row: EstimateHistoryEntity): Long
    @Query("DELETE FROM estimate_history WHERE bssid = :bssid AND id NOT IN (SELECT id FROM estimate_history WHERE bssid = :bssid ORDER BY id DESC LIMIT :keep)")
    suspend fun trim(bssid: String, keep: Int)
    @Query("SELECT * FROM estimate_history WHERE bssid = :bssid ORDER BY id DESC") suspend fun forAp(bssid: String): List<EstimateHistoryEntity>
    @Query("SELECT COUNT(*) FROM estimate_history") suspend fun count(): Int

    @Transaction
    suspend fun append(row: EstimateHistoryEntity) {
        insert(row)
        trim(row.bssid, ESTIMATE_HISTORY_KEEP)
    }
}

const val ESTIMATE_HISTORY_KEEP = 20

/** Shared with MIGRATION_3_4, which runs the same cleanup once. */
const val DEDUPE_DESKTOP_FIXES = "DELETE FROM fixes WHERE source='desktop' AND id NOT IN (SELECT MIN(id) FROM fixes WHERE source='desktop' GROUP BY time)"

/** Cached places. Only `data.DesktopCache` writes here; it never deletes rows because a fetch failed or came back empty. */
@Dao
interface PoiDao {
    @Query("SELECT * FROM pois") fun all(): Flow<List<PoiEntity>>
    @Query("SELECT * FROM pois") suspend fun allNow(): List<PoiEntity>
    @Upsert suspend fun upsertAll(rows: List<PoiEntity>)
    @Query("DELETE FROM pois WHERE source = :source AND scope = :scope") suspend fun deleteScope(source: String, scope: String)
    @Query("DELETE FROM pois WHERE source = :source") suspend fun deleteSource(source: String)

    /** Swap one source's rows of one scope for [rows] atomically; other sources and the other scope are untouched. */
    @Transaction
    suspend fun replace(source: String, scope: String, rows: List<PoiEntity>) {
        deleteScope(source, scope)
        upsertAll(rows.map { if (it.source == source && it.scope == scope) it else it.copy(source = source, scope = scope) })
    }
}

/** The last answer per (source, kind); see [SnapshotEntity]. */
@Dao
interface SnapshotDao {
    @Upsert suspend fun put(s: SnapshotEntity)
    @Query("SELECT * FROM snapshots WHERE source = :source AND kind = :kind") suspend fun get(source: String, kind: String): SnapshotEntity?
    @Query("SELECT * FROM snapshots WHERE kind = :kind ORDER BY fetchedAt DESC LIMIT 1") fun newest(kind: String): Flow<SnapshotEntity?>
    @Query("SELECT * FROM snapshots WHERE kind = :kind ORDER BY fetchedAt DESC LIMIT 1") suspend fun newestNow(kind: String): SnapshotEntity?
}

@Dao
interface DesktopDao {
    @Upsert suspend fun upsert(d: DesktopEntity)
    @Query("SELECT * FROM desktops ORDER BY paired DESC, lastSeen DESC") fun all(): Flow<List<DesktopEntity>>
    @Query("SELECT * FROM desktops") suspend fun allNow(): List<DesktopEntity>
    @Query("SELECT * FROM desktops WHERE paired = 1") suspend fun paired(): List<DesktopEntity>
    @Query("SELECT * FROM desktops WHERE id = :id") suspend fun get(id: String): DesktopEntity?
    @Query("DELETE FROM desktops WHERE id = :id") suspend fun delete(id: String)
}

@Dao
interface AnchorDao {
    @Upsert suspend fun upsert(a: AnchorEntity)
    @Query("SELECT * FROM anchors WHERE deleted = 0 ORDER BY name") fun all(): Flow<List<AnchorEntity>>
    @Query("SELECT * FROM anchors WHERE deleted = 0") suspend fun allNow(): List<AnchorEntity>
    @Query("SELECT * FROM anchors") suspend fun allIncludingDeleted(): List<AnchorEntity>
    @Query("SELECT * FROM anchors WHERE id = :id") suspend fun get(id: String): AnchorEntity?
    @Query("SELECT * FROM anchors WHERE dirty = 1") suspend fun dirty(): List<AnchorEntity>
    @Query("UPDATE anchors SET dirty = 0 WHERE id IN (:ids)") suspend fun clean(ids: List<String>)
    @Query("DELETE FROM anchors WHERE id = :id") suspend fun purge(id: String)
}

@Dao
interface IdentityDao {
    @Upsert suspend fun upsert(e: IdentityEntity)
    @Query("SELECT * FROM identity LIMIT 1") fun current(): Flow<IdentityEntity?>
    @Query("SELECT * FROM identity LIMIT 1") suspend fun currentNow(): IdentityEntity?
    @Query("DELETE FROM identity") suspend fun clear()
    @Upsert suspend fun upsertPending(e: PendingLinkEntity)
    @Query("SELECT * FROM pending_links ORDER BY ts DESC") fun pending(): Flow<List<PendingLinkEntity>>
    @Query("SELECT * FROM pending_links") suspend fun pendingNow(): List<PendingLinkEntity>
    @Query("DELETE FROM pending_links WHERE id = :id") suspend fun removePending(id: String)
    @Query("DELETE FROM pending_links") suspend fun clearPending()
}

/** Plate events and their images (schema v7, docs/SIGHTINGS.md). Merging (§1.1) is `sightings.PlateEventRepository`'s job. */
@Dao
interface PlateEventDao {
    @Insert(onConflict = OnConflictStrategy.ABORT) suspend fun insert(e: PlateEventEntity): Long
    @androidx.room.Update suspend fun update(e: PlateEventEntity)
    @Query("SELECT * FROM plate_events WHERE uid = :uid") suspend fun byUid(uid: String): PlateEventEntity?
    @Query("SELECT * FROM plate_events WHERE uid = :uid") fun watch(uid: String): Flow<PlateEventEntity?>
    /** Passes of [cameraId] within [fromMs]..[toMs] (the ±10 min merge window). */
    @Query("SELECT * FROM plate_events WHERE kind = 'camera_pass' AND camera_id = :cameraId AND time_ms BETWEEN :fromMs AND :toMs ORDER BY ABS(time_ms - :atMs) LIMIT 1")
    suspend fun passNear(cameraId: String, fromMs: Long, toMs: Long, atMs: Long): PlateEventEntity?
    @Query("SELECT * FROM plate_events ORDER BY time_ms DESC LIMIT :limit") fun recent(limit: Int = 500): Flow<List<PlateEventEntity>>
    @Query("SELECT * FROM plate_events WHERE dirty = 1 ORDER BY time_ms LIMIT :limit") suspend fun dirty(limit: Int = 200): List<PlateEventEntity>
    @Query("UPDATE plate_events SET dirty = 0 WHERE uid IN (:uids)") suspend fun clean(uids: List<String>)
    /** Waiting for the hub (records only: images never go there). */
    @Query("SELECT * FROM plate_events WHERE hub_dirty = 1 ORDER BY time_ms LIMIT :limit") suspend fun hubDirty(limit: Int = 200): List<PlateEventEntity>
    @Query("UPDATE plate_events SET hub_dirty = 0 WHERE uid IN (:uids)") suspend fun hubClean(uids: List<String>)
    @Query("UPDATE plate_events SET notified = 1 WHERE uid IN (:uids)") suspend fun markNotified(uids: List<String>)
    @Query("SELECT * FROM plate_events WHERE kind = 'camera_pass' AND time_ms >= :since ORDER BY time_ms DESC") suspend fun passesSince(since: Long): List<PlateEventEntity>
    @Query("SELECT COUNT(*) FROM plate_events") fun count(): Flow<Int>
    @Query("DELETE FROM plate_events WHERE uid = :uid") suspend fun delete(uid: String)

    @Insert(onConflict = OnConflictStrategy.IGNORE) suspend fun insertMedia(m: PlateEventMediaEntity): Long
    @androidx.room.Update suspend fun updateMedia(m: PlateEventMediaEntity)
    @Query("SELECT * FROM plate_event_media WHERE uid = :uid") suspend fun mediaByUid(uid: String): PlateEventMediaEntity?
    /** The media of an event: its dash-cam frames ∪ the photos of its camera (§1). */
    @Query("SELECT * FROM plate_event_media WHERE event_uid = :eventUid OR (camera_id IS NOT NULL AND camera_id = :cameraId) ORDER BY kind, id")
    fun mediaFor(eventUid: String, cameraId: String?): Flow<List<PlateEventMediaEntity>>
    @Query("SELECT * FROM plate_event_media WHERE event_uid = :eventUid OR (camera_id IS NOT NULL AND camera_id = :cameraId) ORDER BY kind, id")
    suspend fun mediaForNow(eventUid: String, cameraId: String?): List<PlateEventMediaEntity>
    /** The first image of each event, for the list's thumbnails. */
    @Query("SELECT * FROM plate_event_media WHERE event_uid IN (:eventUids) ORDER BY id") suspend fun mediaOfEvents(eventUids: List<String>): List<PlateEventMediaEntity>
    /** This phone's own frames the desktop does not have yet. Webcam stills never leave the phone (§2.0: the providers' terms). */
    @Query("SELECT * FROM plate_event_media WHERE path IS NOT NULL AND remote = 0 AND kind != 'webcam' ORDER BY id") suspend fun pendingUploads(): List<PlateEventMediaEntity>
    @Query("SELECT * FROM plate_event_media WHERE path IS NOT NULL") suspend fun localFiles(): List<PlateEventMediaEntity>
    @Query("UPDATE plate_event_media SET event_uid = :to WHERE event_uid = :from") suspend fun moveMedia(from: String, to: String)
    @Query("DELETE FROM plate_event_media WHERE uid = :uid") suspend fun deleteMedia(uid: String)
}
