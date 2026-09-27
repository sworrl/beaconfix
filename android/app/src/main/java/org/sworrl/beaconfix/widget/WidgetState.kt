package org.sworrl.beaconfix.widget

import kotlinx.serialization.Serializable

/** Everything the four home-screen widgets show, snapshotted by [WidgetUpdater] into DataStore so a widget can render without the app running. */
@Serializable
data class TopBeacon(val ssid: String, val bssid: String, val dbm: Int, val security: String, val home: Boolean = false)

@Serializable
data class WidgetState(
    val updated: Long = 0,
    val identityName: String = "", val identityId: String = "",
    // position: the best of the desktop's fix (when paired and fresh) and the phone's own
    val place: String = "", val lat: Double = 0.0, val lon: Double = 0.0, val acc: Double = -1.0,
    val source: String = "",          // gps | wifi | desktop | ""
    val fixTime: Long = 0,
    val atHome: Boolean? = null, val awayText: String = "",
    // beacons
    val inRange: Int = 0, val scanTime: Long = 0,
    val open: Int = 0, val wep: Int = 0, val wpa1: Int = 0, val tkip: Int = 0, val wpa2: Int = 0, val wpa3: Int = 0, val other: Int = 0,
    val worst: String = "",           // critical | weak | ok | strong | ""
    val top: List<TopBeacon> = emptyList(),
    // sync
    val desktopName: String = "", val desktopPaired: Boolean = false, val lastSync: Long = 0, val unsynced: Int = 0, val collectorOn: Boolean = false,
    val apsKnown: Int = 0, val apsPositioned: Int = 0,
    // map snapshot (PNG path under filesDir) rendered around the fix
    val mapPath: String = "", val mapTime: Long = 0,
    // nearest linked device (from the desktop's devices list), e.g. "Desktop 0.8 km NW"
    val nearestDevice: String = "",
    // the measured range to the nearest desktop, e.g. "Desktop · 0.6 m (Wi-Fi RTT ±0.3 m)"
    val rangeLine: String = "",
)
