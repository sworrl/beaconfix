package org.sworrl.beaconfix.help

import org.sworrl.beaconfix.data.db.FixEntity

/**
 * Where distances to help are measured from (pure). The phone's own fix wins while it is fresh and decent
 * ([PHONE_MAX_AGE_MS], [PHONE_MAX_ACC_M]); otherwise the RV (the desktop's fix); with no RV position either, whatever
 * phone fix there is, however old.
 */
object Origin {
    const val PHONE = "phone"
    const val RV = "rv"
    const val NONE = "none"
    const val PHONE_MAX_AGE_MS = 15 * 60_000L
    const val PHONE_MAX_ACC_M = 1000.0

    /** A position: [accM] < 0 = unknown accuracy, [time] epoch ms. */
    data class Fix(val lat: Double, val lon: Double, val accM: Double = -1.0, val time: Long = 0L)

    /** The chosen origin: [kind] is [PHONE], [RV] or [NONE]. */
    data class Chosen(val kind: String, val lat: Double = 0.0, val lon: Double = 0.0, val accM: Double = -1.0, val time: Long = 0L) {
        val known get() = kind != NONE
        fun ageMs(now: Long) = if (time > 0) (now - time).coerceAtLeast(0) else 0L
    }

    fun of(f: FixEntity?): Fix? = f?.takeIf { valid(it.lat, it.lon) }?.let { Fix(it.lat, it.lon, it.acc, it.time) }

    /** A phone fix that is recent and precise enough to measure from. */
    fun phoneUsable(phone: Fix?, now: Long): Boolean =
        phone != null && now - phone.time <= PHONE_MAX_AGE_MS && (phone.accM < 0 || phone.accM <= PHONE_MAX_ACC_M)

    fun choose(phone: Fix?, rv: Fix?, now: Long): Chosen {
        val p = phone?.takeIf { valid(it.lat, it.lon) }
        val r = rv?.takeIf { valid(it.lat, it.lon) }
        return when {
            phoneUsable(p, now) -> Chosen(PHONE, p!!.lat, p.lon, p.accM, p.time)
            r != null -> Chosen(RV, r.lat, r.lon, r.accM, r.time)
            p != null -> Chosen(PHONE, p.lat, p.lon, p.accM, p.time)
            else -> Chosen(NONE)
        }
    }

    /** "from you" / "from the RV" (empty when nothing is known). */
    fun label(kind: String): String = when (kind) { PHONE -> "from you"; RV -> "from the RV"; else -> "" }

    /** Rejects (0, 0) and out-of-range values: the desktop and the database use 0 for "no fix". */
    fun valid(lat: Double, lon: Double) = lat in -90.0..90.0 && lon in -180.0..180.0 && !(lat == 0.0 && lon == 0.0) && !lat.isNaN() && !lon.isNaN()
}
