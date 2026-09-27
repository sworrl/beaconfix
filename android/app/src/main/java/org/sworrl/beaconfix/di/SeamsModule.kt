package org.sworrl.beaconfix.di

import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import org.sworrl.beaconfix.poi.OverpassPhonePlaces
import org.sworrl.beaconfix.poi.PhonePlaces

/** Interfaces the 1.4 packages code against, bound to their implementations. */
@Module
@InstallIn(SingletonComponent::class)
abstract class SeamsModule {
    @Binds abstract fun phonePlaces(impl: OverpassPhonePlaces): PhonePlaces
}
