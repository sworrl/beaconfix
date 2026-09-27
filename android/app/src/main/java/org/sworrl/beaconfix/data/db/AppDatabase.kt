package org.sworrl.beaconfix.data.db

import androidx.room.Database
import androidx.room.RoomDatabase

@Database(
    entities = [ApEntity::class, ObservationEntity::class, FixEntity::class, PoiEntity::class, DesktopEntity::class],
    version = 1,
    exportSchema = false,
)
abstract class AppDatabase : RoomDatabase() {
    abstract fun aps(): ApDao
    abstract fun observations(): ObservationDao
    abstract fun fixes(): FixDao
    abstract fun pois(): PoiDao
    abstract fun desktops(): DesktopDao
}
