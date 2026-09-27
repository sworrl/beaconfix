package org.sworrl.beaconfix.wifi

import org.sworrl.beaconfix.collector.ObservationRecorder
import org.sworrl.beaconfix.ui.SecurityText

/**
 * How safe the Wi-Fi this phone is connected to is, in plain words for the family. Pure (no Android calls), so it
 * is unit-tested. The security vocabulary is the app's own (`ObservationRecorder.securityOf`: open, owe, wep, wpa1,
 * wpa2-tkip, wpa2, wpa2-eap, wpa2/3, wpa3, wpa3-eap192) and the longer explanation is `ui.SecurityText`'s.
 *
 * Open, WEP and TKIP (WPA1, WPA2 with TKIP) are bad; OWE, WPA2 and WPA3 are ok; a home network (matching the
 * home patterns) is ok whatever it uses.
 */
object WifiGrade {
    enum class Level { BAD, OK, UNKNOWN }

    data class Verdict(val level: Level, val label: String, val detail: String, val security: String, val home: Boolean)

    // android.net.wifi.WifiInfo.SECURITY_TYPE_* (API 31), copied so the mapping stays pure
    const val TYPE_UNKNOWN = -1
    const val TYPE_OPEN = 0
    const val TYPE_WEP = 1
    const val TYPE_PSK = 2
    const val TYPE_EAP = 3
    const val TYPE_SAE = 4
    const val TYPE_EAP_WPA3_ENTERPRISE_192_BIT = 5
    const val TYPE_OWE = 6
    const val TYPE_WAPI_PSK = 7
    const val TYPE_WAPI_CERT = 8
    const val TYPE_EAP_WPA3_ENTERPRISE = 9
    const val TYPE_OSEN = 10
    const val TYPE_PASSPOINT_R1_R2 = 11
    const val TYPE_PASSPOINT_R3 = 12
    const val TYPE_DPP = 13

    /**
     * The connection's security from `WifiInfo.currentSecurityType` ([type], API 31+, null before) and the matching
     * scan result's capabilities ([scanCaps]). PSK does not say WPA1 or TKIP, so the scan result refines it.
     */
    fun security(type: Int?, scanCaps: String?): String {
        val fromScan = scanCaps?.takeIf { it.isNotBlank() }?.let { ObservationRecorder.securityOf(it) }
        return when (type) {
            TYPE_OPEN -> "open"
            TYPE_WEP -> "wep"
            TYPE_OWE -> "owe"
            TYPE_SAE, TYPE_DPP -> "wpa3"
            TYPE_EAP_WPA3_ENTERPRISE_192_BIT -> "wpa3-eap192"
            TYPE_EAP, TYPE_EAP_WPA3_ENTERPRISE, TYPE_PASSPOINT_R1_R2, TYPE_PASSPOINT_R3, TYPE_OSEN, TYPE_WAPI_CERT -> "wpa2-eap"
            TYPE_PSK -> fromScan?.takeIf { it == "wpa1" || it == "wpa2-tkip" } ?: "wpa2"
            TYPE_WAPI_PSK -> "wpa2"
            else -> fromScan ?: ""
        }
    }

    fun grade(security: String, home: Boolean = false): Verdict {
        val (label, level) = when (security) {
            "open" -> "Open — not encrypted" to Level.BAD
            "wep" -> "WEP — encryption broken" to Level.BAD
            "wpa1" -> "WPA1 / TKIP — encryption broken" to Level.BAD
            "wpa2-tkip" -> "WPA2 with TKIP — weak encryption" to Level.BAD
            "owe" -> "Enhanced Open (OWE) — encrypted" to Level.OK
            "wpa2" -> "WPA2 — encrypted" to Level.OK
            "wpa2-eap" -> "WPA2-Enterprise — encrypted" to Level.OK
            "wpa2/3" -> "WPA2/WPA3 — encrypted" to Level.OK
            "wpa3" -> "WPA3 — encrypted" to Level.OK
            "wpa3-eap192" -> "WPA3-Enterprise 192-bit — encrypted" to Level.OK
            else -> "Security not reported" to Level.UNKNOWN
        }
        val detail = SecurityText.forSecurity(security).firstOrNull()?.nerd.orEmpty()
        return Verdict(if (home) Level.OK else level, label, detail, security, home)
    }

    /** Same matching as the collector's home filter: a glob on the SSID or the BSSID. */
    fun isHome(ssid: String, bssid: String, patterns: Set<String>): Boolean =
        patterns.any { (ssid.isNotEmpty() && ObservationRecorder.glob(it, ssid)) || (bssid.isNotEmpty() && ObservationRecorder.glob(it, bssid)) }

    /** `WifiInfo.getSSID()` → the plain name; "" when Android hides it (`<unknown ssid>`, no location permission). */
    fun cleanSsid(raw: String?): String {
        if (raw.isNullOrEmpty() || raw == "<unknown ssid>") return ""
        return if (raw.length >= 2 && raw.startsWith('"') && raw.endsWith('"')) raw.substring(1, raw.length - 1) else raw
    }

    const val ADVICE = "Prefer Starlink or your hotspot for banking."

    /** The notification text for a bad network, or null when [security] is not one. */
    fun alertText(ssid: String, security: String): String? = when (security) {
        "open" -> (if (ssid.isEmpty()) "Open Wi-Fi network" else "Open network '$ssid'") + ": others nearby can see unencrypted traffic. $ADVICE"
        "wep", "wpa1", "wpa2-tkip" -> {
            val what = when (security) { "wep" -> "WEP"; "wpa1" -> "WPA1/TKIP"; else -> "WPA2 with TKIP" }
            (if (ssid.isEmpty()) "Weakly encrypted Wi-Fi network" else "Weakly encrypted network '$ssid'") + " ($what): others nearby can break its encryption. $ADVICE"
        }
        else -> null
    }
}
