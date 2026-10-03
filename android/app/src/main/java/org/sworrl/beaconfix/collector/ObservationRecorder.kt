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
    private val widgets: org.sworrl.beaconfix.widget.WidgetUpdater,
    private val desktops: org.sworrl.beaconfix.data.DesktopStore,
    private val motion: MotionDetector,
    private val identity: org.sworrl.beaconfix.identity.IdentityStore,
    private val alertManager: org.sworrl.beaconfix.detector.DetectorAlertManager,
    private val apRtt: org.sworrl.beaconfix.ranging.ApRttRanger,
    private val hubPresence: org.sworrl.beaconfix.net.HubPresence,
    @dagger.hilt.android.qualifiers.ApplicationContext private val appContext: android.content.Context,
) {
    private val noPositionEndpoint = HashSet<String>()
    private var lastReport = 0L
    /** Lightweight position report (`POST /api/v1/devices/position`) at most once a minute; a 404 marks the desktop as not having it. */
    private suspend fun reportPosition(fix: FixEntity, beacons: Int) {
        // the hub keeps its own cadence (5 s moving, 3 min stationary); this per-scan fix is its stationary heartbeat
        hubPresence.offer(fix.lat, fix.lon, fix.acc, fix.time, if (fix.source == "phone-wifi") "wifi" else "gps", null, beacons)
        if (System.currentTimeMillis() - lastReport < 60_000) return
        lastReport = System.currentTimeMillis()
        for (d in desktops.paired()) {
            if (d.id in noPositionEndpoint) continue
            val auth = desktops.auth(d) ?: continue
            val r = runCatching { desktops.api(d).devicePosition(auth, org.sworrl.beaconfix.data.api.DevicePositionBody(fix.lat, fix.lon, fix.acc, org.sworrl.beaconfix.sync.SyncRepository.iso(fix.time), beacons, if (fix.source == "phone-wifi") "wifi" else "gps")) }.getOrNull() ?: continue
            if (r.code() == 404) noPositionEndpoint += d.id
            runCatching {
                desktops.api(d).pushFixes(auth, org.sworrl.beaconfix.data.api.FixesBody(
                    fixes = listOf(org.sworrl.beaconfix.data.api.FixDto(fix.lat, fix.lon, fix.acc, org.sworrl.beaconfix.sync.SyncRepository.iso(fix.time), fix.source, fix.provider, fix.place)),
                    device = identity.deviceName,
                    identity = identity.currentNow()?.id
                ))
            }
        }
    }

    private suspend fun reportFlockSighting(bssid: String, ssid: String, lat: Double, lon: Double, det: SurveillanceSignatures.Detection) {
        for (d in desktops.paired()) {
            val auth = desktops.auth(d) ?: continue
            runCatching {
                desktops.api(d).reportFlockSighting(auth, org.sworrl.beaconfix.data.api.FlockSightingBody(
                    bssid = bssid, lat = lat, lon = lon, model = det.model, method = det.method, confidence = det.confidence, details = det.details,
                    ssid = ssid, tier = det.tier, cls = det.cls
                ))
            }
        }
    }
    suspend fun scanAndRecord(fresh: Boolean): Int {
        val scan = if (fresh) scanner.scan() else scanner.latest()
        val now = System.currentTimeMillis()
        val history = HashMap(status.state.value.history)
        for (s in scan) history[s.bssid] = ((history[s.bssid] ?: emptyList()) + s.dbm).takeLast(40)
        val raw = scanner.rawLatest(); val responders = apRtt.responders(raw)
        val heardRtt = responders.map { it.BSSID.uppercase() }.toSet()
        status.update { it.copy(lastScanAt = now, apsInScan = scan.size, scan = scan, history = history, throttled = scanner.throttlingOn(), error = "",
            rttHeard = heardRtt.size, rttHeardAz = responders.count { r -> apRtt.isAz(r) }, rttAnswered = answeredOf(it.rttRanges, heardRtt, now)) }
        widgets.note("scan: ${scan.size} beacons heard")
        widgets.touch("scan")
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
        // position: GPS/fused first; filter low-power wandering and idle jumps
        val rawLoc = location.current()
        val filtered = rawLoc?.let { motion.filterLocation(it) }
        if (filtered?.isRejectedJump == true) {
            status.update { it.copy(lastFixAt = now, lastFixAcc = rawLoc!!.accuracy.toDouble(), lastFixSource = "gps idle jump suppressed (${rawLoc.accuracy.toInt()} m)") }
            return 0
        }

        val loc = filtered?.location ?: rawLoc
        val mode = motion.status.value.mode
        val modeSource = when (mode) {
            MotionMode.IN_VEHICLE -> "phone-vehicle"
            MotionMode.ON_FOOT -> "phone-foot"
            MotionMode.STATIONARY -> "phone-stationary"
        }
        val chargingTag = if (motion.status.value.isCharging) ":charging" else ""
        val fix = loc?.let { FixEntity(time = now, lat = it.latitude, lon = it.longitude, acc = it.accuracy.toDouble(), source = modeSource, provider = (it.provider ?: "fused") + chargingTag) }
        if (fix == null) {
            val wifi = estimates.locateFromScan(scan.filter { !isHome(it, homePatterns) && it.bssid != connected })
            status.update { it.copy(skippedNoFix = it.skippedNoFix + 1, lastFixAt = if (wifi != null) now else it.lastFixAt, lastFixAcc = wifi?.acc ?: it.lastFixAcc, lastFixSource = if (wifi != null) "wifi (${wifi.method}, ${wifi.used} APs)" else it.lastFixSource) }
            return 0
        }

        val maxAcc = prefs.maxFixAccM.first()
        if (fix.acc > maxAcc) {
            status.update { it.copy(lastFixAt = now, lastFixAcc = fix.acc, lastFixSource = "gps too coarse (${fix.acc.toInt()} m)") }
            return 0
        }

        db.fixes().insert(fix)
        reportPosition(fix, scan.size)
        FlockDetectorKotlin.ensureLoaded(appContext)
        for (s in scan) {
            val det = FlockDetectorKotlin.evaluateWifi(s.bssid, s.ssid)   // docs/DETECTION.md: the shared tiered rules
            if (det.isFlock) {
                reportFlockSighting(s.bssid, s.ssid, fix.lat, fix.lon, det)
                alertManager.triggerAlert(org.sworrl.beaconfix.detector.DetectionType.ALPR_FLOCK)
                alertManager.updateLastDetectedCamera(det.model.ifEmpty { det.label }, 150, "Approaching")
            }
        }

        // Wi-Fi RTT to every 802.11mc / az responder that answers: the range rides on this scan's observation of that AP
        val ranges = if (responders.isEmpty()) emptyMap() else rangeResponders(raw, heardRtt, now, fix)
        val obsSource = if (filtered?.isStationaryClamped == true) "phone-stationary" else "phone-gps"
        val rows = scan.map { s -> val r = ranges[s.bssid]
            ObservationEntity(bssid = s.bssid, time = now, lat = fix.lat, lon = fix.lon, acc = fix.acc, dbm = s.dbm, freq = s.freq, source = obsSource, rangeM = r?.rangeM, rangeSd = r?.rangeSd) }
        val ids = db.observations().insertAll(rows)
        val n = ids.count { it > 0 }
        status.update { it.copy(recordedTotal = it.recordedTotal + n, lastFixAt = now, lastFixAcc = fix.acc, lastFixSource = "gps ±${fix.acc.toInt()} m") }
        // incremental re-fit of the APs we just heard (cheap: a few dozen small least-squares problems)
        estimates.refit(scan.map { it.bssid }.filter { it != connected })
        return n
    }

    /** One RTT round at [fix] (ranging.ApRttRanger); the phone's calibrated offset only when ApRttMath.usableOffset takes it. */
    private suspend fun rangeResponders(raw: List<android.net.wifi.ScanResult>, heard: Set<String>, now: Long, fix: FixEntity): Map<String, org.sworrl.beaconfix.ranging.ApRange> {
        val round = try {
            val off = prefs.rttOffset.first()
            val c = org.sworrl.beaconfix.ranging.ApRttMath.usableOffset(off?.first)
            apRtt.range(raw, now, fix.lat, fix.lon, c, if (c != 0.0) off?.second ?: 0.0 else 0.0)
        } catch (e: kotlinx.coroutines.CancellationException) { throw e } catch (e: Exception) { org.sworrl.beaconfix.ranging.ApRttRound(state = "failed:exception") }
        status.update { st ->
            var all = st.rttRanges + round.ranges
            if (all.size > 500) all = all.entries.sortedByDescending { it.value.time }.take(400).associate { it.key to it.value }
            st.copy(rttRanges = all, rttAnswered = answeredOf(all, heard, now), rttState = round.state)
        }
        return round.ranges
    }

    companion object {
        /** Responders heard now that gave a range in the last 2 min. */
        fun answeredOf(ranges: Map<String, org.sworrl.beaconfix.ranging.ApRange>, heard: Set<String>, now: Long): Int = heard.count { b -> ranges[b]?.let { now - it.time < 120_000 } == true }
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
