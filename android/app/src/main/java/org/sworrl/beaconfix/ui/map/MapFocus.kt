package org.sworrl.beaconfix.ui.map

import kotlinx.coroutines.flow.MutableStateFlow

/**
 * "Show this on the map": any screen (Help, Places, the trip journal, an incoming shared link) sets [target] and
 * navigates to the map; the map re-centres there, opens the place sheet for [Target.poiKey] if given, then sets
 * [target] back to null.
 */
object MapFocus {
    val target = MutableStateFlow<Target?>(null)

    /** [poiKey] = a cached place's `PoiEntity.key` (e.g. `way/329264979`); [label] names a pin that is not a cached place. */
    data class Target(val lat: Double, val lon: Double, val zoom: Double = 16.0, val poiKey: String? = null, val label: String = "")
}
