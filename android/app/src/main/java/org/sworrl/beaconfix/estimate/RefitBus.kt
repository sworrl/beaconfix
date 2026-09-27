package org.sworrl.beaconfix.estimate

import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.serialization.Serializable
import javax.inject.Inject
import javax.inject.Singleton

/** A beacon's position moved or tightened: the same shape as the desktop's `ap_refit` event, produced locally or received. */
@Serializable
data class RefitEvent(
    val bssid: String, val ssid: String = "", val lat: Double, val lon: Double, val fromLat: Double? = null, val fromLon: Double? = null,
    val acc: Double, val prevAcc: Double? = null, val n: Int = 0, val vantage: Int = 0, val rms: Double = 0.0,
    val vantagePoints: List<Vantage> = emptyList(), val time: Long = System.currentTimeMillis(), val origin: String = "phone",
) { @Serializable data class Vantage(val lat: Double, val lon: Double, val dbm: Int, val device: String = "") }

@Singleton
class RefitBus @Inject constructor() {
    private val _events = MutableSharedFlow<RefitEvent>(replay = 1, extraBufferCapacity = 16)
    val events: SharedFlow<RefitEvent> = _events
    @Volatile var last: RefitEvent? = null
    fun emit(e: RefitEvent) { last = e; _events.tryEmit(e) }
    /** The desktop's rule: worth animating when the position moved > max(5 m, 10 % of the old accuracy) or the accuracy improved > 15 %. */
    fun worth(oldLat: Double?, oldLon: Double?, oldAcc: Double?, newLat: Double, newLon: Double, newAcc: Double): Boolean {
        if (oldLat == null || oldLon == null || oldAcc == null) return true
        val moved = Geo.distanceM(oldLat, oldLon, newLat, newLon)
        return moved > maxOf(5.0, 0.10 * oldAcc) || newAcc < 0.85 * oldAcc
    }
}
