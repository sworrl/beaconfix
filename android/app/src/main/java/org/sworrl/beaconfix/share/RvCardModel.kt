package org.sworrl.beaconfix.share

import org.sworrl.beaconfix.data.api.DevicesPositions
import org.sworrl.beaconfix.data.api.LinkedDevice
import org.sworrl.beaconfix.data.api.LocationDto
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.estimate.Geo
import java.time.LocalDateTime
import java.time.OffsetDateTime
import java.time.ZoneId

/**
 * "Where's the RV" (pure; unit-tested). The RV is the newest of: the desktop's fixes synced to this phone
 * (`FixDao.lastDesktop()`), the desktop's own saved `location` snapshot, and the desktop or Pi entries of the saved
 * `devices` snapshot. Everything is measured from this phone's last fix, so it works with no desktop in reach.
 */
object RvCardModel {
    /** One known position of an RV computer. [time] = when the position was taken (epoch ms). */
    data class Spot(val lat: Double, val lon: Double, val acc: Double, val time: Long, val name: String, val kind: String)

    /** Another device of ours, as the RV last saw it. */
    data class Device(val name: String, val kind: String, val lat: Double, val lon: Double, val time: Long, val distM: Double?)

    /** What the card shows. [distM]/[brgDeg] are null without a phone fix. */
    data class State(val rv: Spot? = null, val distM: Double? = null, val brgDeg: Double? = null, val ageMs: Long? = null, val devices: List<Device> = emptyList())

    val RV_KINDS = setOf("desktop", "pi")

    fun fromFix(f: FixEntity?): Spot? = f?.takeIf { usable(it.lat, it.lon) && it.time > 0 }?.let { Spot(it.lat, it.lon, it.acc, it.time, it.place, "desktop") }

    /** The desktop's `/location` as saved at [fetchedAt]; its age then was `age_s`. */
    fun fromLocation(l: LocationDto?, fetchedAt: Long, name: String = ""): Spot? {
        if (l == null || !l.valid || !usable(l.lat, l.lon)) return null
        val t = timeOf(l.time, l.ageS, fetchedAt) ?: return null
        return Spot(l.lat, l.lon, l.accuracy, t, name.ifBlank { l.place }, "desktop")
    }

    /** The RV computers (desktop / Pi) among the linked devices saved at [fetchedAt]. */
    fun fromDevices(d: DevicesPositions?, fetchedAt: Long): List<Spot> =
        d?.devices.orEmpty().filter { it.kind.lowercase() in RV_KINDS && usable(it.lat, it.lon) }
            .mapNotNull { dv -> timeOf(dv.time, dv.ageS, fetchedAt)?.let { Spot(dv.lat, dv.lon, dv.acc, it, dv.identityName.ifBlank { dv.device }, dv.kind.lowercase()) } }

    /** The newest spot (a later fix wins; on a tie the more accurate). */
    fun pick(spots: List<Spot?>): Spot? = spots.filterNotNull().maxWithOrNull(compareBy<Spot> { it.time }.thenByDescending { if (it.acc > 0) it.acc else Double.MAX_VALUE })

    /**
     * The card: the RV, its distance, bearing and age from [phone] (a phone fix only), and the linked devices other than
     * this phone ([selfName]) and the RV computers, nearest first.
     */
    fun state(spots: List<Spot?>, devices: DevicesPositions?, devicesFetchedAt: Long, phone: FixEntity?, selfName: String, now: Long): State {
        val rv = pick(spots)
        val me = phone?.takeIf { it.source.startsWith("phone") && usable(it.lat, it.lon) }
        val others = devices?.devices.orEmpty()
            .filter { it.kind.lowercase() !in RV_KINDS && !isSelf(it, selfName) && usable(it.lat, it.lon) }
            .map { dv ->
                Device(dv.identityName.ifBlank { dv.device }.ifBlank { dv.kind }, dv.kind, dv.lat, dv.lon, timeOf(dv.time, dv.ageS, devicesFetchedAt) ?: 0L,
                    me?.let { Geo.distanceM(it.lat, it.lon, dv.lat, dv.lon) })
            }
            .sortedWith(compareBy<Device> { it.distM ?: Double.MAX_VALUE }.thenByDescending { it.time })
        if (rv == null) return State(devices = others)
        return State(rv,
            me?.let { Geo.distanceM(it.lat, it.lon, rv.lat, rv.lon) },
            me?.let { Geo.bearingDeg(it.lat, it.lon, rv.lat, rv.lon) },
            (now - rv.time).coerceAtLeast(0), others)
    }

    private fun isSelf(d: LinkedDevice, selfName: String) = selfName.isNotBlank() && d.device.equals(selfName, ignoreCase = true)

    private fun usable(lat: Double, lon: Double) = !(lat == 0.0 && lon == 0.0) && kotlin.math.abs(lat) <= 90 && kotlin.math.abs(lon) <= 180

    /** Position time: [fetchedAt] minus the age the desktop reported, else the ISO [time] (local when it has no offset). */
    fun timeOf(time: String, ageS: Double?, fetchedAt: Long): Long? {
        if (ageS != null && ageS >= 0 && fetchedAt > 0) return fetchedAt - (ageS * 1000).toLong()
        if (time.isBlank()) return null
        return runCatching { OffsetDateTime.parse(time).toInstant().toEpochMilli() }.getOrNull()
            ?: runCatching { LocalDateTime.parse(time.take(19)).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli() }.getOrNull()
    }

    /** Minutes / hours / days, for "seen 5 min ago": null = just now (under 90 s). */
    data class Age(val value: Long, val unit: Char)
    fun age(ms: Long): Age? = when {
        ms < 90_000 -> null
        ms < 3_600_000 -> Age(ms / 60_000, 'm')
        ms < 48 * 3_600_000L -> Age(ms / 3_600_000, 'h')
        else -> Age(ms / 86_400_000, 'd')
    }
}
