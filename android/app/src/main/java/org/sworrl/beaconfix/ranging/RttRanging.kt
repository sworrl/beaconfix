package org.sworrl.beaconfix.ranging

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.net.MacAddress
import android.net.wifi.ScanResult
import android.net.wifi.WifiManager
import android.net.wifi.rtt.RangingRequest
import android.net.wifi.rtt.RangingResult
import android.net.wifi.rtt.RangingResultCallback
import android.net.wifi.rtt.ResponderConfig
import android.net.wifi.rtt.WifiRttManager
import android.location.LocationManager
import android.os.Build
import android.os.PowerManager
import androidx.core.content.ContextCompat
import androidx.core.location.LocationManagerCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import org.sworrl.beaconfix.data.api.RttInfo
import org.sworrl.beaconfix.data.api.RttSample
import java.util.concurrent.Executor
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.resume

/**
 * Wi-Fi RTT (IEEE 802.11mc FTM) to the desktop's responder AP (docs/RANGING.md §3.3, §6). One call = one burst.
 * The responder is described by `GET /api/v1/ranging/info` (BSSID, frequency, bandwidth, preamble), so the hidden AP
 * does not need to be in our scan results on Android 13+; older phones fall back to the ScanResult when it is.
 *
 * Android turns Wi-Fi RTT off for the whole device in deep Doze (AOSP RttServiceImpl: `isAvailable()` is false while
 * `PowerManager.isDeviceIdleMode()`), and a locked, still, unplugged phone reaches deep Doze a minute or two after the
 * screen goes off. Showing the app over the lock screen does not end it; unlocking, charging or moving the phone does.
 * [state] says why no burst went out (sent to the desktop as `RangingPost.rttState`), [lastError] says it in words.
 */
@Singleton
class RttRanging @Inject constructor(@ApplicationContext private val ctx: Context) {
    private val mgr: WifiRttManager? by lazy { if (ctx.packageManager.hasSystemFeature(PackageManager.FEATURE_WIFI_RTT)) ctx.getSystemService(WifiRttManager::class.java) else null }
    private val wifi by lazy { ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager }
    val supported: Boolean get() = mgr != null
    val available: Boolean get() = mgr?.isAvailable == true
    fun permitted(): Boolean = ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED &&
        (Build.VERSION.SDK_INT < 33 || ContextCompat.checkSelfPermission(ctx, Manifest.permission.NEARBY_WIFI_DEVICES) == PackageManager.PERMISSION_GRANTED)
    @Volatile var lastError: String = ""; private set
    /** Why the last [range] call did or did not measure: one of the [RttState] values, or `failed:<code>`. */
    @Volatile var state: String = ""; private set
    /** The last [range] call put a request on the air (false when it returned early: Doze, Wi-Fi off, no permission, bad config). */
    @Volatile var sent: Boolean = false; private set
    private val power by lazy { ctx.getSystemService(PowerManager::class.java) }
    private val location by lazy { ctx.getSystemService(LocationManager::class.java) }
    /** The device is in deep Doze: Android has switched Wi-Fi RTT off for everyone until it is unlocked, charged or moved. */
    val dozing: Boolean get() = runCatching { power?.isDeviceIdleMode == true }.getOrDefault(false)
    private fun setState(st: String, msg: String) { state = st; lastError = msg }

    /** RTT is unavailable: say which of Android's conditions (RttServiceImpl.isAvailable) is missing. */
    private fun unavailable() {
        when {
            dozing -> setState(RttState.DOZE, "RTT paused: the phone is in Doze (Android turns RTT off) - unlock it or put it on a charger")
            !runCatching { wifi.isWifiEnabled }.getOrDefault(true) -> setState(RttState.WIFI_OFF, "Wi-Fi is off")
            !runCatching { location?.let { LocationManagerCompat.isLocationEnabled(it) } ?: true }.getOrDefault(true) -> setState(RttState.LOCATION_OFF, "Location is off (Android needs it for Wi-Fi RTT)")
            else -> setState(RttState.UNAVAILABLE, "Wi-Fi RTT unavailable right now")
        }
    }

    /** One ranging burst to [info]'s BSSID; empty when unsupported, unavailable, unpermitted or failed (see [state], [lastError]). */
    @SuppressLint("MissingPermission")
    suspend fun range(info: RttInfo, burst: Int = 8): List<RttSample> {
        sent = false
        val m = mgr ?: run { setState(RttState.UNSUPPORTED, "no Wi-Fi RTT on this phone"); return emptyList() }
        if (!permitted()) { setState(RttState.NO_PERMISSION, "needs the location / nearby-devices permission"); return emptyList() }
        if (!m.isAvailable) {
            val before = state; unavailable()
            if (state != before) android.util.Log.i("BeaconFixRtt", "no burst: state=$state ($lastError)")
            return emptyList()
        }
        val req = runCatching { request(info, burst) }.getOrElse { setState(RttState.BAD_CONFIG, it.message ?: "bad responder config"); return emptyList() } ?: run { state = RttState.BAD_CONFIG; return emptyList() }
        val direct: Executor = Executor { it.run() }
        var failure: Int? = null; var threw = false
        val results = withTimeoutOrNull(6000) {
            suspendCancellableCoroutine<List<RangingResult>?> { cont ->
                try {
                    m.startRanging(req, direct, object : RangingResultCallback() {
                        override fun onRangingFailure(code: Int) { failure = code; if (cont.isActive) cont.resume(null) }
                        override fun onRangingResults(results: MutableList<RangingResult>) { if (cont.isActive) cont.resume(results) }
                    })
                    sent = true
                } catch (e: Exception) { threw = true; setState("failed:exception", e.message ?: e.toString()); if (cont.isActive) cont.resume(null) }
            }
        }
        if (results == null) {
            val code = failure
            when {
                // STATUS_CODE_FAIL_RTT_NOT_AVAILABLE: RTT went away between isAvailable and the request (Doze starting, Wi-Fi off)
                code == RangingResultCallback.STATUS_CODE_FAIL_RTT_NOT_AVAILABLE -> unavailable()
                code != null -> setState("failed:$code", "ranging failed ($code)")
                threw -> {}
                else -> setState(RttState.TIMEOUT, "ranging timed out")
            }
            android.util.Log.i("BeaconFixRtt", "no burst: state=$state (${lastError})")
            return emptyList()
        }
        val out = ArrayList<RttSample>()
        var why = RttState.NO_RESPONSE to "no response from the responder"
        for (r in results) {
            val ok = r.status == RangingResult.STATUS_SUCCESS
            android.util.Log.i("BeaconFixRtt", "bssid=${info.bssid} f=${info.freqMHz}/${info.bandwidthMHz}MHz c0=${info.centerFreq0MHz} pre=${info.preamble} status=${r.status}" +
                (if (ok) " distMm=${r.distanceMm} stdMm=${r.distanceStdDevMm} rssi=${r.rssi} attempted=${r.numAttemptedMeasurements} ok=${r.numSuccessfulMeasurements} ts=${r.rangingTimestampMillis}" else ""))
            if (r.status != RangingResult.STATUS_SUCCESS) { if (r.status == RangingResult.STATUS_RESPONDER_DOES_NOT_SUPPORT_IEEE80211MC) why = RttState.NOT_80211MC to "responder does not support 802.11mc"; continue }
            val mac = runCatching { r.macAddress?.toString() }.getOrNull() ?: info.bssid
            out += RttSample(mac.uppercase(), r.distanceMm, r.distanceStdDevMm, r.rssi, burst, r.numSuccessfulMeasurements, r.rangingTimestampMillis.let { if (it > 0) System.currentTimeMillis() - (android.os.SystemClock.elapsedRealtime() - it) else System.currentTimeMillis() })
        }
        if (out.isNotEmpty()) setState(RttState.OK, "") else setState(why.first, why.second)
        return out
    }

    private fun request(info: RttInfo, burst: Int): RangingRequest? {
        val b = RangingRequest.Builder()
        if (Build.VERSION.SDK_INT >= 33) {
            val cw = when (info.bandwidthMHz) { 40 -> ScanResult.CHANNEL_WIDTH_40MHZ; 80 -> ScanResult.CHANNEL_WIDTH_80MHZ; 160 -> ScanResult.CHANNEL_WIDTH_160MHZ; else -> ScanResult.CHANNEL_WIDTH_20MHZ }
            val pre = when (info.preamble.lowercase()) { "legacy" -> ScanResult.PREAMBLE_LEGACY; "vht" -> ScanResult.PREAMBLE_VHT; "he" -> ScanResult.PREAMBLE_HE; else -> ScanResult.PREAMBLE_HT }
            val cfg = ResponderConfig.Builder().setMacAddress(MacAddress.fromString(info.bssid)).setResponderType(ResponderConfig.RESPONDER_AP).set80211mcSupported(true)
                .setChannelWidth(cw).setFrequencyMhz(info.freqMHz).setCenterFreq0Mhz(if (info.centerFreq0MHz > 0) info.centerFreq0MHz else info.freqMHz).setCenterFreq1Mhz(info.centerFreq1MHz).setPreamble(pre).build()
            b.addResponder(cfg)
            b.setRttBurstSize(burst.coerceIn(RangingRequest.getMinRttBurstSize(), RangingRequest.getMaxRttBurstSize()))
        } else {
            @Suppress("MissingPermission")
            val sr = wifi.scanResults.firstOrNull { it.BSSID.equals(info.bssid, ignoreCase = true) } ?: run { setState(RttState.BAD_CONFIG, "responder not in scan results (Android 12 needs it there)"); return null }
            if (!sr.is80211mcResponder) { setState(RttState.NOT_80211MC, "scan result says no 802.11mc"); return null }
            b.addAccessPoint(sr)
        }
        return b.build()
    }
}

/** `RangingPost.rttState` (docs/API.md): why the phone did or did not send RTT bursts in this post. */
object RttState {
    const val OK = "ok"                         // bursts measured
    const val DOZE = "doze"                     // deep Doze: Android has RTT off until the phone is unlocked, charged or moved
    const val WIFI_OFF = "wifi-off"
    const val LOCATION_OFF = "location-off"
    const val UNAVAILABLE = "unavailable"       // isAvailable false for another reason
    const val UNSUPPORTED = "unsupported"       // no FEATURE_WIFI_RTT
    const val NO_PERMISSION = "no-permission"
    const val NO_RESPONSE = "no-response"       // the request ran, the responder did not answer
    const val NOT_80211MC = "not-80211mc"
    const val TIMEOUT = "timeout"
    const val BAD_CONFIG = "bad-config"
    const val NO_RESPONDER = "no-responder"     // the desktop serves no responder (ranging/info rtt disabled)
    const val IDLE = "idle"                     // not in a session (collector off, app in the background): RTT is not attempted
    const val AWAY = "away"                     // the desktop is not around (responder not in the Wi-Fi scan, its BLE not heard, or its API not answering): no burst
    const val BACKOFF = "backoff"               // the last bursts got no answer: waiting (2.5 s doubling to 2 min) before the next one
    // plus "failed:<code>" for RangingResultCallback.onRangingFailure codes other than RTT_NOT_AVAILABLE
}
