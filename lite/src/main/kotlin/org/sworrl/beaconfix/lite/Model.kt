package org.sworrl.beaconfix.lite

/** One access point in a scan: [rssi] in dBm, [freqMhz] 0 when unknown. */
class Heard(val bssid: String, val rssi: Int, val freqMhz: Int = 0, val ssid: String = "") {
    /** The BSSID as a 48-bit number (the cache's key); -1 when it doesn't parse. */
    val key: Long = Mac.key(bssid)
    override fun toString() = "$bssid $rssi dBm $freqMhz MHz '$ssid'"
}

/**
 * A position.
 *
 * [accM] is the 1-sigma radius (68 %), [r95M] the radius that holds the true position 95 % of the time.
 * [source]: "cache" (solved from APs already known here, no network), "apple" / "beacondb" (a lookup was
 * needed), "reuse" (the same APs as last time: the last fix, no work at all).
 * [integrity]: "ok" (4+ APs agree), "repaired" (one or more disagreeing APs were dropped), "unverified"
 * (too few APs to cross-check), "failed" (they disagree and dropping some didn't fix it), "n/a" (a
 * service's answer). [lookups]: network requests this fix cost.
 */
data class LiteFix(
    val lat: Double,
    val lon: Double,
    val accM: Double,
    val r95M: Double,
    val source: String,
    val integrity: String,
    val heard: Int,
    val used: Int,
    val excluded: Int,
    val lookups: Int,
    val timeMs: Long,
) {
    fun toJson(): String = "{\"lat\":%.7f,\"lon\":%.7f,\"acc\":%.1f,\"r95\":%.1f,\"source\":\"%s\",\"integrity\":\"%s\",\"heard\":%d,\"used\":%d,\"excluded\":%d,\"lookups\":%d,\"time\":%d}"
        .format(java.util.Locale.US, lat, lon, accM, r95M, source, integrity, heard, used, excluded, lookups, timeMs)
}

/** MAC addresses as 48-bit longs: 8 bytes in memory and 6 on disk instead of a 17-character string. */
object Mac {
    /** "2:3a:c4:9:42:ec" (Apple drops leading zeros) or "02-3a-c4-09-42-EC" → 0x023ac40942ec; -1 when it isn't a MAC. */
    fun key(mac: String): Long {
        var v = 0L; var octet = 0; var digits = 0; var parts = 0
        for (c in mac) {
            val d = Character.digit(c, 16)
            if (d >= 0) { if (digits == 2) return -1; octet = octet * 16 + d; digits++ }
            else if (c == ':' || c == '-') { if (digits == 0) return -1; v = (v shl 8) or octet.toLong(); octet = 0; digits = 0; parts++ }
            else if (!c.isWhitespace()) return -1
        }
        if (digits == 0) return -1
        v = (v shl 8) or octet.toLong(); parts++
        return if (parts == 6) v else -1
    }

    fun text(key: Long): String {
        val sb = StringBuilder(17)
        for (i in 5 downTo 0) {
            val o = ((key ushr (8 * i)) and 0xff).toInt()
            sb.append(Character.forDigit(o shr 4, 16)).append(Character.forDigit(o and 15, 16))
            if (i > 0) sb.append(':')
        }
        return sb.toString()
    }

    /** Apple writes octets without leading zeros: "2:11:22:3:44:5". */
    fun apple(key: Long): String {
        val sb = StringBuilder(17)
        for (i in 5 downTo 0) {
            sb.append(Integer.toHexString(((key ushr (8 * i)) and 0xff).toInt()))
            if (i > 0) sb.append(':')
        }
        return sb.toString()
    }
}
