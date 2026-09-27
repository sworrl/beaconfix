package org.sworrl.beaconfix.wifi

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.util.Log
import androidx.annotation.RequiresApi
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.collector.ObservationRecorder
import javax.inject.Inject
import javax.inject.Singleton

/** The Wi-Fi network this phone is connected to. [ssid] is "" when Android hides it (no location permission). */
data class CurrentWifi(val ssid: String, val bssid: String, val security: String, val frequencyMhz: Int = 0) {
    val band: String get() = ObservationRecorder.bandOf(frequencyMhz)
    val channel: Int get() = ObservationRecorder.channelOf(frequencyMhz)
}

/**
 * Watches the connected Wi-Fi with a ConnectivityManager callback (TRANSPORT_WIFI; with FLAG_INCLUDE_LOCATION_INFO on
 * API 31+ so the name and BSSID come through). Security comes from `WifiInfo.currentSecurityType` on API 31+, refined
 * or (before 31) replaced by the matching scan result's capabilities. [start] is idempotent; it also feeds
 * [WifiAlerts]. Nothing is scanned: only the phone's existing scan cache is read.
 */
@Singleton
class CurrentNetworkMonitor @Inject constructor(@ApplicationContext private val ctx: Context, private val alerts: WifiAlerts) {
    private val _current = MutableStateFlow<CurrentWifi?>(null)
    val current: StateFlow<CurrentWifi?> = _current.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val lock = Any()
    private var callback: Callback? = null
    private var alertJob: Job? = null
    private val networks = LinkedHashMap<Network, CurrentWifi?>()
    private var capsCache: Pair<String, String?>? = null      // bssid → scan capabilities

    private val cm by lazy { ctx.getSystemService(ConnectivityManager::class.java) }
    private val wifi by lazy { ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager }

    fun start() {
        synchronized(lock) {
            if (alertJob == null) alertJob = scope.launch { current.filterNotNull().collect { alerts.onConnected(it) } }
            if (callback == null) register()
        }
    }

    /** Read the connection again, e.g. after location permission was granted (which un-hides the name). */
    fun refresh() {
        synchronized(lock) {
            if (callback == null) return
            unregister(); register()
        }
    }

    private fun register() {
        val c = cm ?: return
        val cb = if (Build.VERSION.SDK_INT >= 31) Callback(ConnectivityManager.NetworkCallback.FLAG_INCLUDE_LOCATION_INFO) else Callback()
        try {
            c.registerNetworkCallback(NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).build(), cb)
            callback = cb
        } catch (e: Exception) {
            Log.w(TAG, "can't watch Wi-Fi: $e")
        }
    }

    private fun unregister() {
        callback?.let { runCatching { cm?.unregisterNetworkCallback(it) } }
        callback = null
        networks.clear()
    }

    private fun publish() { _current.value = networks.values.lastOrNull { it != null } }

    private inner class Callback : ConnectivityManager.NetworkCallback {
        constructor() : super()
        @RequiresApi(31) constructor(flags: Int) : super(flags)

        override fun onCapabilitiesChanged(network: Network, caps: NetworkCapabilities) {
            val w = runCatching { read(caps) }.getOrNull()
            synchronized(lock) { if (callback === this) { networks[network] = w; publish() } }
        }

        override fun onLost(network: Network) {
            synchronized(lock) { if (callback === this) { networks.remove(network); publish() } }
        }
    }

    private fun read(caps: NetworkCapabilities): CurrentWifi? {
        val info: WifiInfo = (if (Build.VERSION.SDK_INT >= 31) caps.transportInfo as? WifiInfo else @Suppress("DEPRECATION") wifi?.connectionInfo) ?: return null
        val ssid = WifiGrade.cleanSsid(info.ssid)
        val bssid = info.bssid?.uppercase()?.takeIf { it != "02:00:00:00:00:00" }.orEmpty()
        val type = if (Build.VERSION.SDK_INT >= 31) info.currentSecurityType else null
        // the scan result only matters when the type can't tell (before API 31) or is plain PSK (WPA1 / TKIP?)
        val scanCaps = if (type == null || type == WifiGrade.TYPE_PSK || type == WifiGrade.TYPE_UNKNOWN) scanCaps(bssid, ssid) else null
        return CurrentWifi(ssid, bssid, WifiGrade.security(type, scanCaps), info.frequency)
    }

    private fun scanCaps(bssid: String, ssid: String): String? {
        capsCache?.let { (b, c) -> if (b == bssid && bssid.isNotEmpty() && c != null) return c }
        val caps = try {
            @Suppress("MissingPermission")
            val results = wifi?.scanResults.orEmpty()
            (results.firstOrNull { bssid.isNotEmpty() && it.BSSID.equals(bssid, ignoreCase = true) }
                ?: results.filter { ssid.isNotEmpty() && ssidOf(it) == ssid }.maxByOrNull { it.level })?.capabilities
        } catch (e: SecurityException) { null }
        capsCache = bssid to caps
        return caps
    }

    private fun ssidOf(r: android.net.wifi.ScanResult): String =
        if (Build.VERSION.SDK_INT >= 33) (r.wifiSsid?.toString()?.trim('"') ?: "") else @Suppress("DEPRECATION") r.SSID.orEmpty()

    companion object { const val TAG = "BfWifi" }
}
