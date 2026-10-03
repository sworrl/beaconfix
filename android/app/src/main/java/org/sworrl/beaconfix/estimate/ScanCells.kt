package org.sworrl.beaconfix.estimate

import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

/**
 * The ~15 m cells this phone scanned from, with the time span of those scans, and the misses they imply for an AP
 * (docs/GRADING.md §1.7): a port of the desktop's Locator scan cells (noteScanCell / missesFor in src/locator.cpp).
 * The desktop notes a cell at every scan; the phone has no scan log, so its scans are read back from its own
 * observation rows (one scan = the rows of one instant, one fix). A scan that heard nothing leaves no row and is lost.
 */
class ScanCells {
    class Cell(var lat: Double, var lon: Double, var count: Int, var first: Long, var last: Long)

    private val cells = HashMap<String, Cell>()
    private val index = HashMap<String, MutableList<String>>()

    val size: Int get() = cells.size
    fun clear() { cells.clear(); index.clear() }

    /** One scan at (lat, lon) with fix accuracy [acc] (m) at [tSec] (seconds since the epoch, 0 = unknown). */
    fun note(lat: Double, lon: Double, acc: Double, tSec: Long) {
        if (!(acc > 0) || acc > 60 || !lat.isFinite() || !lon.isFinite()) return
        val key = cellKey(lat, lon)
        var c = cells[key]
        if (c == null) {
            c = Cell(lat, lon, 0, tSec, 0)
            cells[key] = c
            index.getOrPut(bucket(lat, lon)) { ArrayList() }.add(key)
        } else { c.lat += (lat - c.lat) / (c.count + 1); c.lon += (lon - c.lon) / (c.count + 1) }
        ++c.count; c.last = max(c.last, tSec)
    }

    /** Where this phone scanned near the AP's samples, while the AP was around, without hearing it (nearest 150). */
    fun missesFor(obs: List<Obs>): List<Miss> {
        val out = ArrayList<Miss>()
        if (obs.isEmpty() || cells.isEmpty()) return out
        var lat = 0.0; var lon = 0.0; var first = 0L; var last = 0L; var own = 0
        val heard = HashSet<String>()
        for (o in obs) {
            lat += o.lat / obs.size; lon += o.lon / obs.size
            if (o.device.isNotEmpty()) continue
            ++own; heard.add(cellKey(o.lat, o.lon))
            if (o.t > 0) { first = if (first != 0L) min(first, o.t) else o.t; last = max(last, o.t) }
        }
        if (own == 0) return out                            // only our own scans say where it was NOT heard
        if (first == 0L) { last = Long.MAX_VALUE / 2 }
        class C(val d: Double, val m: Miss)
        val cand = ArrayList<C>()
        for (dy in -1..1) for (dx in -1..1) {
            val b = "${floor(lat * 100).toInt() + dy}:${floor(lon * 100).toInt() + dx}"
            val keys = index[b] ?: continue
            for (k in keys) {
                if (k in heard) continue
                val c = cells.getValue(k)
                if (c.last < first - 7 * 86400L || c.first > last + 30 * 86400L) continue   // not while the AP was around
                val d = Estimator.distanceM(lat, lon, c.lat, c.lon)
                if (d > 600) continue
                // a heard sample nearby means this cell is not a miss
                if (obs.any { Estimator.distanceM(it.lat, it.lon, c.lat, c.lon) < 15 }) continue
                cand.add(C(d, Miss(c.lat, c.lon, c.count)))
            }
        }
        cand.sortBy { it.d }
        for (i in 0 until min(cand.size, 150)) out.add(cand[i].m)
        return out
    }

    companion object {
        /** The desktop's scanCellKey: ~15 m cells (0.000135° of latitude, the longitude step widened by cos φ). */
        fun cellKey(lat: Double, lon: Double): String {
            val y = floor(lat / 0.000135).toInt()
            val c = max(0.05, cos((y * 0.000135) * PI / 180.0))
            val x = floor(lon * c / 0.000135).toInt()
            return "$y:$x"
        }
        /** The desktop's scanBucket: 0.01° cells. */
        fun bucket(lat: Double, lon: Double): String = "${floor(lat * 100).toInt()}:${floor(lon * 100).toInt()}"
    }
}
