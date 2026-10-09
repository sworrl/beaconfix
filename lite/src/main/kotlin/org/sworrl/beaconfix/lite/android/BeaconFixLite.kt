package org.sworrl.beaconfix.lite.android

import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.wifi.WifiManager
import android.os.Build
import android.os.SystemClock
import android.util.Log
import org.sworrl.beaconfix.lite.Heard
import org.sworrl.beaconfix.lite.Http
import org.sworrl.beaconfix.lite.LiteFix
import org.sworrl.beaconfix.lite.LiteLocator
import org.sworrl.beaconfix.lite.Pacer
import org.sworrl.beaconfix.lite.UrlHttp
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * BeaconFix Lite on Android: Wi-Fi scan in, [LiteFix] out. Blocking; call [locate] from a background thread.
 *
 *     val lite = BeaconFixLite(context)
 *     val fix = lite.locate()                  // null: nothing here could place us
 *     handler.postDelayed(next, lite.nextCheckMs)
 *
 * Needs ACCESS_FINE_LOCATION (Android's rule for reading scan results), ACCESS_WIFI_STATE, CHANGE_WIFI_STATE (to ask
 * for a scan) and INTERNET (for the rare lookup). No Google Play services, no GPS.
 *
 * Battery: a scan the system made in the last [scanMaxAgeMs] is used as it is (another app's, or Android's own
 * background scans) and no new one is asked for. Android allows an app four scans per two minutes; a refused request
 * is not an error, the cache is as fresh as we're allowed.
 */
class BeaconFixLite @JvmOverloads constructor(
    context: Context,
    dir: File = File(context.noBackupFilesDir, "beaconfix-lite"),
    http: Http? = UrlHttp(),
    config: LiteLocator.Config = LiteLocator.Config(),
    /** SSIDs that travel with this device (its owner's router, a hotspot), on top of the one it's joined to. */
    var travelling: List<String> = emptyList(),
    val scanMaxAgeMs: Long = 5 * 60_000L,
    /**
     * Treat the joined network as travelling (never a vote). Right for a phone or a player that follows its owner
     * (their router goes where they go); a device that stays put (a picture frame) should pass false, since the
     * network it's joined to is then its best reference. The locator drops a contradicting AP either way.
     */
    val joinedTravels: Boolean = true,
) {
    private val ctx = context.applicationContext
    private val wifi = ctx.getSystemService(Context.WIFI_SERVICE) as? WifiManager
    val locator = LiteLocator(dir, http, config)
    val pacer = Pacer()

    /** The wait the [Pacer] suggests after the last [locate]. */
    val nextCheckMs: Long get() = pacer.intervalMs

    /** [allowNetwork] false: cache only. */
    @JvmOverloads
    fun locate(allowNetwork: Boolean = true): LiteFix? {
        val scan = scan()
        val fix = if (scan.isEmpty()) null else locator.locate(scan, if (joinedTravels) travelling + joinedSsid() else travelling, allowNetwork)
        pacer.next(fix)
        if (fix != null) Log.i(TAG, "${fix.source} ±${fix.accM.toInt()} m (r95 ${fix.r95M.toInt()} m, ${fix.integrity}, ${fix.used}/${fix.heard} APs, ${fix.lookups} lookups); next in ${pacer.intervalMs / 60_000} min")
        else Log.i(TAG, "no fix from ${scan.size} APs; next in ${pacer.intervalMs / 60_000} min")
        return fix
    }

    /** What the radio hears: the system's results when fresh, else a scan we ask for (and wait up to [waitMs] for). */
    @SuppressLint("MissingPermission")
    @JvmOverloads
    fun scan(waitMs: Long = 8_000L): List<Heard> {
        val w = wifi ?: return emptyList()
        var results = runCatching { w.scanResults }.getOrNull().orEmpty()
        val nowUs = SystemClock.elapsedRealtime() * 1000
        val newest = results.maxOfOrNull { it.timestamp } ?: 0L
        if (results.isEmpty() || nowUs - newest > scanMaxAgeMs * 1000) {
            if (requestScan(w, waitMs)) results = runCatching { w.scanResults }.getOrNull().orEmpty()
        }
        // a result the radio hasn't heard for a while is where we were, not where we are
        val cutoff = SystemClock.elapsedRealtime() * 1000 - scanMaxAgeMs * 2000
        return results.filter { it.timestamp >= cutoff || it.timestamp == 0L }
            .map { Heard(it.BSSID ?: "", it.level, it.frequency, ssidOf(it)) }
    }

    @Suppress("DEPRECATION")
    private fun ssidOf(r: android.net.wifi.ScanResult): String = r.SSID ?: ""

    @SuppressLint("MissingPermission")
    private fun requestScan(w: WifiManager, waitMs: Long): Boolean {
        val latch = CountDownLatch(1)
        val rx = object : BroadcastReceiver() { override fun onReceive(c: Context?, i: Intent?) { latch.countDown() } }
        return try {
            val filter = IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION)
            if (Build.VERSION.SDK_INT >= 33) ctx.registerReceiver(rx, filter, Context.RECEIVER_NOT_EXPORTED)
            else @Suppress("UnspecifiedRegisterReceiverFlag") ctx.registerReceiver(rx, filter)
            @Suppress("DEPRECATION")
            val started = runCatching { w.startScan() }.getOrDefault(false)
            started && latch.await(waitMs, TimeUnit.MILLISECONDS)
        } catch (_: Throwable) { false } finally { runCatching { ctx.unregisterReceiver(rx) } }
    }

    /** The network we're joined to is almost always ours, and ours travels with us: never a vote. */
    private fun joinedSsid(): List<String> {
        @Suppress("DEPRECATION")
        val s = runCatching { wifi?.connectionInfo?.ssid }.getOrNull()?.trim('"')?.trim().orEmpty()
        return if (s.isBlank() || s == "<unknown ssid>") emptyList() else listOf(s)
    }

    companion object { private const val TAG = "BeaconFixLite" }
}
