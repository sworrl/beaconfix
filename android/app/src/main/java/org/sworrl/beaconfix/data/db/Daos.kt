package org.sworrl.beaconfix.data.db

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
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
}

@Dao
interface PoiDao {
    @Upsert suspend fun upsertAll(rows: List<PoiEntity>)
    @Query("SELECT * FROM pois") fun all(): Flow<List<PoiEntity>>
    @Query("DELETE FROM pois") suspend fun clear()
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
