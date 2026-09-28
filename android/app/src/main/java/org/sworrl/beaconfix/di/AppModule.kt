package org.sworrl.beaconfix.di

import android.content.Context
import androidx.room.Room
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.ALL_MIGRATIONS
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
object AppModule {
    /** Real migrations for every step (data/db/Migrations.kt) and no destructive fallback: an update never wipes the phone. */
    @Provides @Singleton
    fun database(@ApplicationContext ctx: Context): AppDatabase =
        Room.databaseBuilder(ctx, AppDatabase::class.java, "beaconfix.db").addMigrations(*ALL_MIGRATIONS).build()
}
