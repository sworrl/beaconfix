package org.sworrl.beaconfix.poi

/**
 * The phone's own help-places search (OpenStreetMap), used only when no desktop answers. Bound in di/SeamsModule.
 * Results land in data.DesktopCache (source "phone"); callers read them from there, not from the result.
 */
interface PhonePlaces {
    /**
     * Search around [lat]/[lon] for the help categories (and, with [pediatric], the wide children's ER search) unless
     * the cached data is still fresh ([force] skips that check, never the back-off). Never throws.
     */
    suspend fun refreshAround(lat: Double, lon: Double, force: Boolean = false, pediatric: Boolean = true): PhonePlacesResult

    /** Epoch ms until which the search is backing off after a failure (0 = not backing off). */
    val busyUntil: Long
}

/** [ok] = a search ran and stored its rows ([count]); [skipped] = nothing was requested (fresh, off, metered, offline…); [note] says why. */
data class PhonePlacesResult(val ok: Boolean, val count: Int = 0, val note: String = "", val skipped: Boolean = false)
