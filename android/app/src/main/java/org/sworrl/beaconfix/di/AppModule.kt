package org.sworrl.beaconfix.di

import android.content.Context
import androidx.room.Room
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import org.sworrl.beaconfix.data.db.AppDatabase
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
object AppModule {
    @Provides @Singleton
    fun database(@ApplicationContext ctx: Context): AppDatabase =
        Room.databaseBuilder(ctx, AppDatabase::class.java, "beaconfix.db").addMigrations(MIGRATION_2_3).fallbackToDestructiveMigration().build()

    /** 1.2 → 1.3: the anchors table (docs/RANGING.md §4). Everything else is untouched, so no data is lost on update. */
    private val MIGRATION_2_3 = object : androidx.room.migration.Migration(2, 3) {
        override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
            db.execSQL("CREATE TABLE IF NOT EXISTS `anchors` (`id` TEXT NOT NULL, `json` TEXT NOT NULL, `name` TEXT NOT NULL, `kind` TEXT NOT NULL, `lat` REAL NOT NULL, `lon` REAL NOT NULL, `rv` INTEGER NOT NULL, `ref` INTEGER NOT NULL, `deleted` INTEGER NOT NULL, `placedAt` TEXT NOT NULL, `seq` INTEGER NOT NULL, `dirty` INTEGER NOT NULL, PRIMARY KEY(`id`))")
        }
    }
}
