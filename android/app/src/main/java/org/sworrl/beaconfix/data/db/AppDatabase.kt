package org.sworrl.beaconfix.data.db

import androidx.room.Database
import androidx.room.RoomDatabase

@Database(
    entities = [ApEntity::class, ObservationEntity::class, FixEntity::class, PoiEntity::class, DesktopEntity::class, IdentityEntity::class, PendingLinkEntity::class, AnchorEntity::class, SnapshotEntity::class, EstimateHistoryEntity::class],
    version = 5,
    exportSchema = true,
)
abstract class AppDatabase : RoomDatabase() {
    abstract fun aps(): ApDao
    abstract fun observations(): ObservationDao
    abstract fun fixes(): FixDao
    abstract fun pois(): PoiDao
    abstract fun desktops(): DesktopDao
    abstract fun identity(): IdentityDao
    abstract fun anchors(): AnchorDao
    abstract fun snapshots(): SnapshotDao
    abstract fun estimateHistory(): EstimateHistoryDao
}
