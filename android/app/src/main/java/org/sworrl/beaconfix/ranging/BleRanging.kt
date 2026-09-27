package org.sworrl.beaconfix.ranging

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertisingSet
import android.bluetooth.le.AdvertisingSetCallback
import android.bluetooth.le.AdvertisingSetParameters
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.ParcelUuid
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import org.sworrl.beaconfix.data.api.BleSample
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton

/**
 * BLE ranging (docs/RANGING.md §9.1): we advertise the BeaconFix service data (rotating identity tag, our TX power, flags)
 * and scan for the desktop's advertisement, keeping every RSSI sample per known identity. Legacy, non-connectable,
 * non-scannable; 200 ms interval during a session, 1 s otherwise. Android does not expose the advertising channel.
 */
@Singleton
class BleRanging @Inject constructor(@ApplicationContext private val ctx: Context) {
    private val bt by lazy { ctx.getSystemService(BluetoothManager::class.java) }
    private val adapter get() = bt?.adapter
    val supported: Boolean get() = ctx.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE) && adapter != null
    val enabled: Boolean get() = runCatching { adapter?.isEnabled == true }.getOrDefault(false)
    fun permitted(): Boolean = if (Build.VERSION.SDK_INT >= 31) listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_ADVERTISE).all { ContextCompat.checkSelfPermission(ctx, it) == PackageManager.PERMISSION_GRANTED }
        else ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
    @Volatile var lastError: String = ""; private set
    @Volatile var advertising: Boolean = false; private set
    @Volatile var scanning: Boolean = false; private set
    @Volatile var ourTxPower: Int = 127; private set

    /** identity id (26 chars, lower-case) → samples heard since the last drain */
    private val heard = HashMap<String, ArrayList<BleSample>>()
    @Volatile private var watch: Set<String> = emptySet()          // identity ids we accept tags for
    private var expected: Map<String, String> = emptyMap()          // hex(tag) → identity id, for the current ±1 windows
    private var expectedWindow = -1L
    private var uuid: UUID = UUID.fromString(DEFAULT_UUID)
    private var set: AdvertisingSet? = null
    private var ownId: String = ""
    private var session = false
    private var scanSession = false
    private var scanStartedAt = 0L
    private val advCallback = object : AdvertisingSetCallback() {
        @SuppressLint("MissingPermission")
        override fun onAdvertisingSetStarted(advertisingSet: AdvertisingSet?, txPower: Int, status: Int) {
            if (status != ADVERTISE_SUCCESS || advertisingSet == null) { lastError = "advertising failed ($status)"; advertising = false; return }
            set = advertisingSet; ourTxPower = txPower; advertising = true; lastError = ""
            runCatching { advertisingSet.setAdvertisingData(data(txPower)) }     // byte 8 = the TX power the controller reported
        }
        override fun onAdvertisingSetStopped(advertisingSet: AdvertisingSet?) { advertising = false; set = null }
    }
    private var lastTagWindow = -1L

    fun setServiceUuid(u: String) { runCatching { UUID.fromString(u) }.getOrNull()?.let { if (it != uuid) { uuid = it; restart() } } }
    /** Which identities' advertisements we keep (paired desktops, linked devices). */
    fun watch(ids: Collection<String>) { watch = ids.map { it.lowercase() }.toSet(); expectedWindow = -1 }

    private fun window(nowS: Long = System.currentTimeMillis() / 1000) = nowS / 900
    private fun refreshExpected() {
        val w = window(); if (w == expectedWindow) return
        val m = HashMap<String, String>()
        for (id in watch) for (dw in -1..1) m[hex(RangeMath.bleTag(id, (w + dw) * 900))] = id
        expected = m; expectedWindow = w
    }

    // ── advertising ───────────────────────────────────────────────────────────
    @SuppressLint("MissingPermission")
    fun startAdvertising(identityId: String, sessionMode: Boolean) {
        if (!supported || !enabled || !permitted()) { lastError = if (!supported) "no BLE" else if (!enabled) "Bluetooth is off" else "needs the Nearby devices permission"; return }
        ownId = identityId.lowercase()
        if (advertising && session == sessionMode) { rotateIfDue(); return }
        stopAdvertising()
        session = sessionMode
        val adv = adapter?.bluetoothLeAdvertiser ?: run { lastError = "no LE advertiser"; return }
        val params = AdvertisingSetParameters.Builder().setLegacyMode(true).setConnectable(false).setScannable(false)
            .setInterval(if (sessionMode) 320 else AdvertisingSetParameters.INTERVAL_HIGH)     // 320 × 0.625 ms = 200 ms; INTERVAL_HIGH = 1600 = 1 s
            .setTxPowerLevel(AdvertisingSetParameters.TX_POWER_MEDIUM).build()
        try { adv.startAdvertisingSet(params, data(127), null, null, null, advCallback); advertising = true } catch (e: Exception) { lastError = e.message ?: e.toString() }
    }
    private fun data(txPower: Int): AdvertiseData {
        lastTagWindow = window()
        val flags = RangeMath.bleFlags(rtt = false, api = false, kind = 1, calibrating = false)
        return AdvertiseData.Builder().addServiceData(ParcelUuid(uuid), RangeMath.bleServiceData(RangeMath.bleTag(ownId, lastTagWindow * 900), txPower, flags)).setIncludeDeviceName(false).setIncludeTxPowerLevel(false).build()
    }
    /** The tag rotates every 15 minutes: refresh the advertisement when the window changed. */
    @SuppressLint("MissingPermission")
    fun rotateIfDue() { val s = set ?: return; if (window() != lastTagWindow) runCatching { s.setAdvertisingData(data(ourTxPower)) } }
    @SuppressLint("MissingPermission")
    fun stopAdvertising() { if (advertising || set != null) runCatching { adapter?.bluetoothLeAdvertiser?.stopAdvertisingSet(advCallback) }; set = null; advertising = false }

    // ── scanning ──────────────────────────────────────────────────────────────
    private val scanCb = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) { onResult(result) }
        override fun onBatchScanResults(results: MutableList<ScanResult>) { results.forEach { onResult(it) } }
        override fun onScanFailed(errorCode: Int) { scanning = false; lastError = "scan failed ($errorCode)" }
    }
    private fun onResult(r: ScanResult) {
        val bytes = r.scanRecord?.getServiceData(ParcelUuid(uuid)) ?: return
        val adv = RangeMath.parseServiceData(bytes) ?: return
        refreshExpected()
        val id = expected[hex(adv.tag)] ?: return
        // We watch desktops' identities. Another device can carry the same tag (the Pi agent advertised its desktop's
        // identity, kind pi): only a desktop/laptop advert is that desktop, anything else would pollute its down link.
        if (adv.kind != RangeMath.KIND_DESKTOP && adv.kind != RangeMath.KIND_LAPTOP) return
        val tx = if (adv.txPower != 127) adv.txPower else if (Build.VERSION.SDK_INT >= 26 && r.txPower != ScanResult.TX_POWER_NOT_PRESENT) r.txPower else 127
        val t = if (r.timestampNanos > 0) System.currentTimeMillis() - (android.os.SystemClock.elapsedRealtimeNanos() - r.timestampNanos) / 1_000_000 else System.currentTimeMillis()
        synchronized(heard) { heard.getOrPut(id) { ArrayList() }.let { l -> l += BleSample(r.rssi, null, tx, t); if (l.size > 600) l.subList(0, l.size - 600).clear() } }
        remoteFlags[id] = adv
    }
    /** The last flags each identity advertised (RTT responder? API reachable? kind). */
    val remoteFlags = java.util.concurrent.ConcurrentHashMap<String, BleAdvert>()

    /**
     * Scan for the desktop's advert. The filter must count as a real filter for the Bluetooth stack: an unfiltered scan is
     * suspended while the screen is off (Android 17 treated our `setServiceData(uuid, null)` filter that way: the phone stopped
     * hearing the desktop the moment its screen went off). `ByteArray(0)` = "any service data under this UUID", which the
     * stack keeps running screen-off at its screen-off duty cycle. A scan running longer than the stack's scan timeout
     * (10–30 min) is downgraded, so it is restarted every [SCAN_RESTART_MS].
     */
    @SuppressLint("MissingPermission")
    fun startScanning(sessionMode: Boolean) {
        if (!supported || !enabled || !permitted()) return
        val now = android.os.SystemClock.elapsedRealtime()
        if (scanning && scanSession == sessionMode && now - scanStartedAt < SCAN_RESTART_MS) return
        stopScanning()
        scanSession = sessionMode
        val scanner = adapter?.bluetoothLeScanner ?: return
        val settings = ScanSettings.Builder().setScanMode(if (sessionMode) ScanSettings.SCAN_MODE_LOW_LATENCY else ScanSettings.SCAN_MODE_LOW_POWER).setReportDelay(0).build()
        try { scanner.startScan(listOf(ScanFilter.Builder().setServiceData(ParcelUuid(uuid), ByteArray(0)).build()), settings, scanCb); scanning = true; scanStartedAt = now } catch (e: Exception) { lastError = e.message ?: e.toString() }
    }
    @SuppressLint("MissingPermission")
    fun stopScanning() { if (scanning) runCatching { adapter?.bluetoothLeScanner?.stopScan(scanCb) }; scanning = false }
    private fun restart() { val id = ownId; val s = session; stopAdvertising(); stopScanning(); if (id.isNotEmpty()) startAdvertising(id, s); startScanning(s) }
    fun stop() { stopAdvertising(); stopScanning() }

    /** Samples heard from [identityId] since the last call (they are consumed). */
    fun drain(identityId: String): List<BleSample> = synchronized(heard) { heard.remove(identityId.lowercase()) ?: emptyList() }
    /** The most recent samples from [identityId] within [windowMs], without consuming. */
    fun recent(identityId: String, windowMs: Long): List<BleSample> { val t = System.currentTimeMillis() - windowMs; return synchronized(heard) { (heard[identityId.lowercase()] ?: emptyList()).filter { it.time >= t } } }

    companion object {
        const val DEFAULT_UUID = "28c9f0bf-a089-4a95-b632-5e8ede1b03b6"
        /** Restart the scan before the Bluetooth stack's scan timeout (10 min on current AOSP) downgrades it. */
        const val SCAN_RESTART_MS = 9 * 60_000L
        fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }
    }
}
