package org.sworrl.beaconfix.collector

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.wifi.WifiManager
import android.os.Build
import android.provider.Settings
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import org.sworrl.beaconfix.estimate.ScanSample
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.resume

@Singleton
class WifiScanner @Inject constructor(@ApplicationContext private val ctx: Context) {
    private val wifi by lazy { ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager }

    /** Android 10+ throttles foreground apps to 4 scans per 2 minutes unless "Wi-Fi scan throttling" is off in Developer options. */
    fun throttlingOn(): Boolean = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) wifi.isScanThrottleEnabled
        else Settings.Global.getInt(ctx.contentResolver, "wifi_scan_throttle_enabled", 1) == 1

    /** Request a fresh scan and wait for its results (or fall back to the cached list). */
    suspend fun scan(timeoutMs: Long = 12_000): List<ScanSample> {
        val fresh = withTimeoutOrNull(timeoutMs) {
            suspendCancellableCoroutine<Boolean> { cont ->
                val rx = object : BroadcastReceiver() {
                    override fun onReceive(c: Context, i: Intent) {
                        runCatching { ctx.unregisterReceiver(this) }
                        if (cont.isActive) cont.resume(i.getBooleanExtra(WifiManager.EXTRA_RESULTS_UPDATED, false))
                    }
                }
                ctx.registerReceiver(rx, IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION))
                cont.invokeOnCancellation { runCatching { ctx.unregisterReceiver(rx) } }
                @Suppress("DEPRECATION")
                if (!wifi.startScan()) { runCatching { ctx.unregisterReceiver(rx) }; cont.resume(false) }
            }
        }
        return latest(fresh == true)
    }

    fun latest(fresh: Boolean = false): List<ScanSample> = try {
        @Suppress("MissingPermission")
        wifi.scanResults.map {
            val ssid = if (Build.VERSION.SDK_INT >= 33) (it.wifiSsid?.toString()?.trim('"') ?: "") else @Suppress("DEPRECATION") it.SSID
            ScanSample(it.BSSID.uppercase(), ssid, it.level, it.frequency, it.capabilities)
        }.also { lastFresh = fresh }
    } catch (e: SecurityException) { emptyList() }
    var lastFresh = false; private set

    /** The scan cache as ScanResults (Wi-Fi RTT needs them: RangingRequest.Builder.addAccessPoint); empty without permission. */
    fun rawLatest(): List<android.net.wifi.ScanResult> = try { @Suppress("MissingPermission") wifi.scanResults } catch (e: SecurityException) { emptyList() }

    /** How long ago any scan on this phone (ours or another app's) last saw [bssid], in ms; null when the scan cache does not hold it. */
    fun seenAgoMs(bssid: String): Long? = try {
        @Suppress("MissingPermission")
        wifi.scanResults.firstOrNull { it.BSSID.equals(bssid, ignoreCase = true) }?.let { (android.os.SystemClock.elapsedRealtimeNanos() / 1000 - it.timestamp) / 1000 }
    } catch (e: SecurityException) { null }

    /** The BSSID of the network this phone is connected to (its own hotspot never appears in scans). */
    fun connectedBssid(): String? = try { @Suppress("DEPRECATION") wifi.connectionInfo?.bssid?.uppercase()?.takeIf { it != "02:00:00:00:00:00" } } catch (e: SecurityException) { null }
}
