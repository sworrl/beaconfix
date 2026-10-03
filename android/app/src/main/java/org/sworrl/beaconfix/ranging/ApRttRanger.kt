package org.sworrl.beaconfix.ranging

import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.net.wifi.ScanResult
import android.net.wifi.rtt.RangingRequest
import android.net.wifi.rtt.RangingResult
import android.net.wifi.rtt.RangingResultCallback
import android.net.wifi.rtt.WifiRttManager
import android.os.Build
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import java.util.concurrent.Executor
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.resume

/**
 * One collection round: responders heard in the scan (802.11mc, of them 802.11az NTB), ranged, answered, and why not:
 * [state] is an [RttState] value (ok, unsupported, no-permission, doze, unavailable, timeout, failed:<code>), "cooldown"
 * (every responder was ranged here a moment ago) or "" (no responder heard).
 */
data class ApRttRound(val heard: Int = 0, val az: Int = 0, val tried: Int = 0, val answered: Int = 0, val ranges: Map<String, ApRange> = emptyMap(), val state: String = "")

/**
 * Wi-Fi RTT to any AP that answers (802.11mc FTM; 802.11az NTB on Android 15+, which `addAccessPoint` picks by itself
 * when both sides support it). Runs from the collector after a scan with a usable fix: the strongest responders not in
 * cooldown ([ApRttPlanner]), in requests of RangingRequest.getMaxPeers(), [BURSTS] rounds, combined per AP by
 * [ApRttMath.combine]. The collector is a foreground service, so Android's background RTT throttle does not apply;
 * deep Doze switches RTT off and the round then ends at once (state "unavailable"). Every missing piece (no
 * FEATURE_WIFI_RTT, no permission, RTT off) degrades silently to an empty round.
 */
@Singleton
class ApRttRanger @Inject constructor(@ApplicationContext private val ctx: Context, private val rtt: RttRanging) {
    private val mgr: WifiRttManager? by lazy { if (Build.VERSION.SDK_INT >= 28 && ctx.packageManager.hasSystemFeature(PackageManager.FEATURE_WIFI_RTT)) ctx.getSystemService(WifiRttManager::class.java) else null }
    private val planner = ApRttPlanner()

    /** 802.11mc responders in [scan]; plus 802.11az NTB-only ones on Android 15+ (ScanResult.is80211azNtbResponder is API 35). */
    fun responders(scan: List<ScanResult>): List<ScanResult> = scan.filter { isMc(it) || isAz(it) }.distinctBy { it.BSSID.uppercase() }
    fun isMc(s: ScanResult) = runCatching { s.is80211mcResponder }.getOrDefault(false)
    fun isAz(s: ScanResult) = Build.VERSION.SDK_INT >= 35 && runCatching { s.is80211azNtbResponder }.getOrDefault(false)

    /** Range the due responders of [scan] at ([lat], [lon]); [offsetM]/[offsetSd] is the phone's own offset (0 when unknown). */
    @SuppressLint("MissingPermission")
    suspend fun range(scan: List<ScanResult>, now: Long, lat: Double?, lon: Double?, offsetM: Double, offsetSd: Double): ApRttRound {
        val resp = responders(scan)
        val heard = resp.size; val az = resp.count { isAz(it) }
        if (heard == 0) return ApRttRound()
        if (Build.VERSION.SDK_INT < 28) return ApRttRound(heard, az, state = RttState.UNSUPPORTED)     // WifiRttManager is Android 9+
        val m = mgr ?: return ApRttRound(heard, az, state = RttState.UNSUPPORTED)
        if (!rtt.permitted()) return ApRttRound(heard, az, state = RttState.NO_PERMISSION)
        if (!runCatching { m.isAvailable }.getOrDefault(false)) return ApRttRound(heard, az, state = if (rtt.dozing) RttState.DOZE else RttState.UNAVAILABLE)
        val byBssid = resp.associateBy { it.BSSID.uppercase() }
        val picks = planner.select(resp.map { RttCandidate(it.BSSID.uppercase(), it.level, isAz(it)) }, now, lat, lon)
        if (picks.isEmpty()) return ApRttRound(heard, az, state = COOLDOWN)     // all ranged here a moment ago
        val maxPeers = runCatching { RangingRequest.getMaxPeers() }.getOrDefault(10)
        val bursts = HashMap<String, MutableList<ApBurst>>()
        var live = picks.map { it.bssid }
        var state = RttState.OK
        rounds@ for (round in 0 until BURSTS) {
            val dead = HashSet<String>()
            for (batch in ApRttPlanner.batches(live, maxPeers)) {
                val res = request(m, batch.mapNotNull { byBssid[it] })
                if (res.failure != null) { state = res.failure; if (res.failure == RttState.UNAVAILABLE) break@rounds; continue }
                for (r in res.results) {
                    val mac = runCatching { r.macAddress?.toString()?.uppercase() }.getOrNull() ?: continue
                    when (r.status) {
                        RangingResult.STATUS_SUCCESS -> if (r.numSuccessfulMeasurements >= ApRttMath.MIN_OK)
                            bursts.getOrPut(mac) { ArrayList() } += ApBurst(r.distanceMm, r.distanceStdDevMm, r.numSuccessfulMeasurements, r.rssi, Build.VERSION.SDK_INT >= 35 && runCatching { r.is80211azNtbMeasurement }.getOrDefault(false))
                        RangingResult.STATUS_RESPONDER_DOES_NOT_SUPPORT_IEEE80211MC -> dead += mac     // advertised but refuses: no more rounds
                    }
                }
            }
            // a round nobody answered: the next ones would not either (saves the radio time)
            if (round == 0 && bursts.isEmpty()) break
            live = live.filter { it !in dead }
            if (live.isEmpty()) break
        }
        val ranges = HashMap<String, ApRange>()
        for (p in picks) {
            val r = bursts[p.bssid]?.let { ApRttMath.combine(p.bssid, it, now, offsetM, offsetSd) }
            if (r != null) ranges[p.bssid] = r
            planner.mark(p.bssid, now, lat, lon, r != null)
        }
        android.util.Log.i("BeaconFixRtt", "aps: heard=$heard (az=$az) tried=${picks.size} answered=${ranges.size} offset=$offsetM state=$state " +
            ranges.values.joinToString(" ") { "${it.bssid}=${it.rangeM}±${it.rangeSd}/${it.bursts}" })
        return ApRttRound(heard, az, picks.size, ranges.size, ranges, state)
    }

    private class Res(val results: List<RangingResult> = emptyList(), val failure: String? = null)

    @SuppressLint("MissingPermission")
    @androidx.annotation.RequiresApi(28)
    private suspend fun request(m: WifiRttManager, aps: List<ScanResult>): Res {
        if (aps.isEmpty()) return Res()
        val req = runCatching { RangingRequest.Builder().addAccessPoints(aps).build() }.getOrElse { return Res(failure = RttState.BAD_CONFIG) }
        val direct = Executor { it.run() }
        var failure: String? = null
        val out = withTimeoutOrNull(TIMEOUT_MS) {
            suspendCancellableCoroutine<List<RangingResult>?> { cont ->
                try {
                    m.startRanging(req, direct, object : RangingResultCallback() {
                        override fun onRangingFailure(code: Int) {
                            failure = if (code == RangingResultCallback.STATUS_CODE_FAIL_RTT_NOT_AVAILABLE) RttState.UNAVAILABLE else "failed:$code"
                            if (cont.isActive) cont.resume(null)
                        }
                        override fun onRangingResults(results: MutableList<RangingResult>) { if (cont.isActive) cont.resume(results) }
                    })
                } catch (e: Exception) { failure = if (e is SecurityException) RttState.NO_PERMISSION else "failed:exception"; if (cont.isActive) cont.resume(null) }
            }
        }
        return if (out != null) Res(out) else Res(failure = failure ?: RttState.TIMEOUT)
    }

    companion object {
        /** Requests per AP per stop: each is one burst (the platform's default burst size, 8 exchanges). */
        const val BURSTS = 3
        const val TIMEOUT_MS = 5_000L
        const val COOLDOWN = "cooldown"
    }
}
