package org.sworrl.beaconfix.lite

import kotlin.math.asin
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

object Geo {
    fun distanceM(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val r = 6_371_000.0
        val d2r = Math.PI / 180.0
        val dLat = (lat2 - lat1) * d2r
        val dLon = (lon2 - lon1) * d2r
        val a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * d2r) * cos(lat2 * d2r) * sin(dLon / 2) * sin(dLon / 2)
        return 2 * r * asin(sqrt(a.coerceIn(0.0, 1.0)))
    }

    fun valid(lat: Double, lon: Double): Boolean =
        !lat.isNaN() && !lon.isNaN() && lat in -90.0..90.0 && lon in -180.0..180.0 && !(lat == 0.0 && lon == 0.0)
}

/** A flat metric frame around (lat0, lon0): metres east (x) and north (y). Fine over the few km a scan covers. */
class Frame(private val lat0: Double, private val lon0: Double) {
    private val my = 111_320.0
    private val mx = 111_320.0 * cos(lat0 * Math.PI / 180.0)
    fun x(lon: Double) = (lon - lon0) * mx
    fun y(lat: Double) = (lat - lat0) * my
    fun lat(y: Double) = lat0 + y / my
    fun lon(x: Double) = lon0 + x / mx
}
