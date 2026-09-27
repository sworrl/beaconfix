package org.sworrl.beaconfix.help

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Nearest help (children's ER, ER, urgent care, police, fire, pharmacy, vet) from the best origin, desktop or phone.
 *
 * STUB (A0): the signatures are frozen; A1 fills in the body (and may add constructor dependencies).
 * Contract: [snapshot] always holds the last good answer; [refresh] never throws — on failure it keeps the previous
 * snapshot and sets [HelpSnapshot.lastError]; [refreshIfStale] only ever talks to a desktop (no Overpass).
 */
@Singleton
class HelpRepository @Inject constructor() {
    private val _snapshot = MutableStateFlow(HelpSnapshot())
    val snapshot: StateFlow<HelpSnapshot> = _snapshot.asStateFlow()

    suspend fun refresh(force: Boolean = false): HelpSnapshot = _snapshot.value

    suspend fun refreshIfStale(maxAgeMs: Long = 30 * 60_000L) {
        if (System.currentTimeMillis() - _snapshot.value.fetchedAt > maxAgeMs) refresh(force = false)
    }
}
