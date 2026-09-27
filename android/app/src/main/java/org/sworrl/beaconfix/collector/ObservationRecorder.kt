package org.sworrl.beaconfix.collector

import kotlinx.coroutines.flow.first
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.data.db.ObservationEntity
import org.sworrl.beaconfix.estimate.EstimateRepository
import org.sworrl.beaconfix.estimate.ScanSample
import javax.inject.Inject
import javax.inject.Singleton

/** One scan → observations for every heard BSSID at the phone's current position (or a Wi-Fi-only fix when GPS is silent). */
@Singleton
class ObservationRecorder @Inject constructor(
    private val db: AppDatabase,
    private val scanner: WifiScanner,
    private val location: LocationSource,
    private val estimates: EstimateRepository,
    private val status: CollectorStatus,
    private val prefs: Prefs,
) {
    suspend fun scanAndRecord(fresh: Boolean): Int {
        val scan = if (fresh) scanner.scan() else scanner.latest()
        val now = System.currentTimeMillis()
        val history = HashMap(status.state.value.history)
        for (s in scan) history[s.bssid] = ((history[s.bssid] ?: emptyList()) + s.dbm).takeLast(40)
        status.update { it.copy(lastScanAt = now, apsInScan = scan.size, scan = scan, history = history, throttled = scanner.throttlingOn(), error = "") }
        if (scan.isEmpty()) return 0
        val homePatterns = prefs.homePatterns.first()
        val connected = scanner.connectedBssid()
        // upsert AP rows
        val existing = db.aps().allBssids().toHashSet()
        for (s in scan) {
            val old = if (s.bssid in existing) db.aps().get(s.bssid) else null
            val home = isHome(s, homePatterns)
            db.aps().upsert((old ?: ApEntity(bssid = s.bssid, firstSeen = now)).copy(
                ssid = s.ssid.ifEmpty { old?.ssid ?: "" }, freq = s.freq, band = bandOf(s.freq), ch = channelOf(s.freq), lastSeen = now,
                timesSeen = (old?.timesSeen ?: 0) + 1, home = home || (old?.home ?: false),
                security = if (old?.security.isNullOrEmpty()) securityOf(s.capabilities) else old!!.security))
        }
        // position: GPS/fused first; else our own Wi-Fi estimate (never used to record observations — it would be circular)
        val loc = location.current()
        val fix = loc?.let { FixEntity(time = now, lat = it.latitude, lon = it.longitude, acc = it.accuracy.toDouble(), source = "phone-gps", provider = it.provider ?: "fused") }
        if (fix == null) {
            val wifi = estimates.locateFromScan(scan.filter { !isHome(it, homePatterns) && it.bssid != connected })
            status.update { it.copy(skippedNoFix = it.skippedNoFix + 1, lastFixAt = if (wifi != null) now else it.lastFixAt, lastFixAcc = wifi?.acc ?: it.lastFixAcc, lastFixSource = if (wifi != null) "wifi (${wifi.method}, ${wifi.used} APs)" else it.lastFixSource) }
            return 0
        }
        db.fixes().insert(fix)
        val maxAcc = prefs.maxFixAccM.first()
        if (fix.acc > maxAcc) { status.update { it.copy(lastFixAt = now, lastFixAcc = fix.acc, lastFixSource = "gps too coarse (${fix.acc.toInt()} m)") }; return 0 }
        val rows = scan.map { s -> ObservationEntity(bssid = s.bssid, time = now, lat = fix.lat, lon = fix.lon, acc = fix.acc, dbm = s.dbm, freq = s.freq, source = "phone-gps") }
        val ids = db.observations().insertAll(rows)
        val n = ids.count { it > 0 }
        status.update { it.copy(recordedTotal = it.recordedTotal + n, lastFixAt = now, lastFixAcc = fix.acc, lastFixSource = "gps ±${fix.acc.toInt()} m") }
        // incremental re-fit of the APs we just heard (cheap: a few dozen small least-squares problems)
        estimates.refit(scan.map { it.bssid }.filter { it != connected })
        return n
    }

    companion object {
        fun bandOf(freq: Int) = when { freq >= 5925 -> "6"; freq >= 4900 -> "5"; freq > 0 -> "2.4"; else -> "" }
        fun channelOf(freq: Int): Int = when {
            freq == 2484 -> 14
            freq in 2412..2472 -> (freq - 2407) / 5
            freq in 5180..5885 -> (freq - 5000) / 5
            freq in 5955..7115 -> (freq - 5950) / 5
            else -> 0
        }
        /** Same vocabulary as the desktop's classify(): open, owe, wep, wpa1, wpa2-tkip, wpa2, wpa2-eap, wpa2/3, wpa3, wpa3-eap192 */
        fun securityOf(cap: String): String {
            val c = cap.uppercase()
            return when {
                c.contains("SUITE_B_192") -> "wpa3-eap192"
                c.contains("SAE") && c.contains("PSK") -> "wpa2/3"
                c.contains("SAE") -> "wpa3"
                c.contains("OWE") -> "owe"
                c.contains("EAP") -> "wpa2-eap"
                c.contains("RSN") && c.contains("TKIP") -> "wpa2-tkip"
                c.contains("RSN") || c.contains("WPA2") -> "wpa2"
                c.contains("WPA") -> "wpa1"
                c.contains("WEP") -> "wep"
                else -> "open"
            }
        }
        fun isHome(s: ScanSample, patterns: Set<String>): Boolean = patterns.any { glob(it, s.ssid) || glob(it, s.bssid) }
        fun glob(pattern: String, value: String): Boolean {
            if (pattern.isEmpty()) return false
            val re = buildString { append('^'); for (ch in pattern) append(when (ch) { '*' -> ".*"; '?' -> "."; else -> Regex.escape(ch.toString()) }); append('$') }
            return Regex(re, RegexOption.IGNORE_CASE).matches(value)
        }
    }
}
