package org.sworrl.beaconfix.poi

import javax.inject.Inject
import javax.inject.Singleton

/**
 * [PhonePlaces] over the Overpass API (help categories only; user-initiated, one request at a time, back-off on failure).
 *
 * STUB (A0): A4 fills in the body (and may add constructor dependencies). Until then no search is made.
 */
@Singleton
class OverpassPhonePlaces @Inject constructor() : PhonePlaces {
    override val busyUntil: Long get() = 0L

    override suspend fun refreshAround(lat: Double, lon: Double, force: Boolean, pediatric: Boolean): PhonePlacesResult =
        PhonePlacesResult(false, note = "phone search not available")
}
