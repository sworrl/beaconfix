package org.sworrl.beaconfix.estimate

import kotlin.math.asin
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

object Geo {
    const val R = 6371000.0
    fun distanceM(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val dLat = Math.toRadians(lat2 - lat1); val dLon = Math.toRadians(lon2 - lon1)
        val a = sin(dLat / 2) * sin(dLat / 2) + cos(Math.toRadians(lat1)) * cos(Math.toRadians(lat2)) * sin(dLon / 2) * sin(dLon / 2)
        return 2 * R * asin(sqrt(a))
    }
    fun bearingDeg(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val f1 = Math.toRadians(lat1); val f2 = Math.toRadians(lat2); val dl = Math.toRadians(lon2 - lon1)
        val y = sin(dl) * cos(f2); val x = cos(f1) * sin(f2) - sin(f1) * cos(f2) * cos(dl)
        return (Math.toDegrees(atan2(y, x)) + 360) % 360
    }
    /** Local east/north metres relative to an origin (flat-earth, fine for < 50 km). */
    class Local(val lat0: Double, val lon0: Double) {
        private val kx = Math.toRadians(1.0) * R * cos(Math.toRadians(lat0))
        private val ky = Math.toRadians(1.0) * R
        fun toXY(lat: Double, lon: Double) = doubleArrayOf((lon - lon0) * kx, (lat - lat0) * ky)
        fun toLatLon(x: Double, y: Double) = doubleArrayOf(lat0 + y / ky, lon0 + x / kx)
    }
    fun compass(deg: Double): String {
        val names = arrayOf("N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW")
        return names[((deg % 360 + 360) % 360 / 22.5).toInt() % 16]
    }
}
