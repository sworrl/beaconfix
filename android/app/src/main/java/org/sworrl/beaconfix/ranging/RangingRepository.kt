package org.sworrl.beaconfix.ranging

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.wifi.rtt.WifiRttManager
import android.os.PowerManager
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import org.sworrl.beaconfix.anchors.AnchorRepository
import org.sworrl.beaconfix.collector.CollectorStatus
import org.sworrl.beaconfix.collector.WifiScanner
import org.sworrl.beaconfix.data.DesktopLive
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.api.AnchorDto
import org.sworrl.beaconfix.data.api.Baro
import org.sworrl.beaconfix.data.api.BleSample
import org.sworrl.beaconfix.data.api.DeviceRange
import org.sworrl.beaconfix.data.api.RangingFix
import org.sworrl.beaconfix.data.api.RangingInfo
import org.sworrl.beaconfix.data.api.RangingPost
import org.sworrl.beaconfix.data.api.RttSample
import org.sworrl.beaconfix.data.api.WifiRssi
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.estimate.Geo
import org.sworrl.beaconfix.identity.IdentityStore
import java.time.Instant
import java.time.format.DateTimeFormatter
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin
import kotlin.math.sqrt

/** The phone's own fused estimate of the range to one desktop (docs/RANGING.md §5), next to what the desktop replied. */
data class LocalRange(val distanceM: Double, val sigmaM: Double, val lowM: Double, val highM: Double, val cls: String, val method: List<String>,
                      val bearingDeg: Double? = null, val bearingSigmaDeg: Double? = null, val time: Long = System.currentTimeMillis())

/** Where the phone is when the range to an anchored desktop is combined with its own fix: a ring around the anchor, or a point when the bearing is known. */
data class RangedFix(val lat: Double, val lon: Double, val acc: Double, val anchorLat: Double, val anchorLon: Double, val radiusM: Double, val sigmaM: Double, val bearingDeg: Double?, val time: Long = System.currentTimeMillis())

data class RangeSession(
    val desktop: DesktopEntity, val identityId: String = "", val info: RangingInfo? = null, val infoAt: Long = 0, val supported: Boolean? = null,
    val remote: DeviceRange? = null, val remoteAt: Long = 0, val local: LocalRange? = null,
    val error: String = "", val rttError: String = "", val rttState: String = "", val bleError: String = "", val rttCount: Int = 0, val bleCount: Int = 0, val posts: Int = 0, val lastPost: Long = 0,
    /** Last time the desktop answered (ranging/info or a ranging POST): RTT bursts stop when it has not for [RangingRepository.REACH_MS]. */
    val reachableAt: Long = 0,
    val anchor: AnchorDto? = null, val rangedFix: RangedFix? = null, val active: Boolean = false,
) {
    /** The estimate to show: the desktop's (it fuses both directions) while fresh, else ours. */
    val best: LocalRange? get() = remote?.takeIf { it.distanceM != null && System.currentTimeMillis() - remoteAt < 20_000 }?.let { r -> LocalRange(r.distanceM!!, r.sigmaM ?: 0.0, r.lowM ?: r.distanceM, r.highM ?: r.distanceM, r.cls, r.method, r.bearingDeg, r.bearingSigmaDeg, remoteAt) } ?: local?.takeIf { System.currentTimeMillis() - it.time < 30_000 }
    val name: String get() = desktop.name.ifEmpty { desktop.hostname.ifEmpty { desktop.host } }
    /** "Desktop · 0.6 m (Wi-Fi RTT ±0.3 m)" */
    val line: String get() = best?.let { b -> "$name · ${fmtM(b.distanceM)} (${methodName(b.method)} ±${fmtM(b.sigmaM)})" } ?: ""
    companion object {
        fun fmtM(m: Double) = if (m < 10) String.format(java.util.Locale.US, "%.1f m", m) else if (m < 1000) "${m.toInt()} m" else String.format(java.util.Locale.US, "%.1f km", m / 1000)
        fun methodName(m: List<String>) = when { "rtt" in m -> "Wi-Fi RTT"; "ble" in m -> "BLE"; "wifi-diff" in m -> "Wi-Fi fingerprint"; "fix" in m -> "fixes"; m.isEmpty() -> "range"; else -> m.first() }
    }
}

/**
 * Device-to-device ranging (docs/RANGING.md): every few seconds while the app is in the foreground or the collector runs,
 * range the desktop's Wi-Fi RTT responder, gather the BLE samples heard from it, the shared-AP RSSI vector, barometer and
 * motion, POST them to `/api/v1/ranging` (the desktop fuses both directions and replies with its estimate), and fuse
 * locally too with [RangeMath] so the phone has a number even when the desktop is slow. With the presence service alone
 * (collector off, app in the background) only low-power BLE advertising/scanning continues so the desktop can hear us.
 */
@Singleton
class RangingRepository @Inject constructor(
    @ApplicationContext private val ctx: Context, private val desktops: DesktopStore, private val identity: IdentityStore, private val scanner: WifiScanner,
    private val status: CollectorStatus, private val db: AppDatabase, private val anchors: AnchorRepository, private val live: DesktopLive,
    private val rtt: RttRanging, private val ble: BleRanging, private val motion: MotionSensors, private val widgets: dagger.Lazy<org.sworrl.beaconfix.widget.WidgetUpdater>,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val _sessions = MutableStateFlow<Map<String, RangeSession>>(emptyMap())
    val sessions: StateFlow<Map<String, RangeSession>> = _sessions
    val desktopAnchor = MutableStateFlow<AnchorDto?>(null)
    @Volatile private var presence = false
    @Volatile private var foreground = false
    private var job: Job? = null
    private val filters = HashMap<String, RangeFilter>()
    private val lastTick = HashMap<String, Long>()
    private val deskAps = HashMap<String, Pair<Long, Map<String, Int>>>()     // desktop id → (fetched, bssid → dbm)
    private var lastLine = ""; private var lastLineAt = 0L
    /** Wakes the loop's sleep early: Doze ended / RTT became available again (no 2.5–20 s wait for the next burst). */
    private val wake = Channel<Unit>(Channel.CONFLATED)

    init {
        // Android switches Wi-Fi RTT off for the whole device in deep Doze and back on when it ends (unlock, charger,
        // motion). Range again the moment that happens instead of at the next tick; both are system broadcasts.
        val r = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                android.util.Log.i("BeaconFixRtt", "${i.action?.substringAfterLast('.')}: doze=${rtt.dozing} rttAvailable=${rtt.available}")
                wake.trySend(Unit)
            }
        }
        runCatching {
            ContextCompat.registerReceiver(ctx, r, IntentFilter().apply { addAction(PowerManager.ACTION_DEVICE_IDLE_MODE_CHANGED); addAction(WifiRttManager.ACTION_WIFI_RTT_STATE_CHANGED) },
                ContextCompat.RECEIVER_NOT_EXPORTED)
        }
    }

    fun presence(on: Boolean) { presence = on; kick() }
    fun foreground(on: Boolean) { foreground = on; kick() }
    fun anchorsChanged() { scope.launch { runCatching { refreshAnchors() } } }
    private fun sessionMode() = foreground || status.state.value.running
    private fun active() = presence || foreground
    private fun kick() { if (active() && job == null) job = scope.launch { loop() } }

    fun ranged(desktopId: String): RangeSession? = _sessions.value[desktopId]
    /** The nearest desktop's line for the notification / widget, or "". */
    fun line(): String = _sessions.value.values.mapNotNull { s -> s.best?.let { it.distanceM to s.line } }.minByOrNull { it.first }?.second ?: ""
    /** The best current ranged fix (phone on the ring / at the point around an anchored desktop). */
    fun rangedFix(): RangedFix? = _sessions.value.values.mapNotNull { it.rangedFix }.filter { System.currentTimeMillis() - it.time < 30_000 }.minByOrNull { it.acc }

    private suspend fun loop() {
        try {
            while (scope.isActive && active()) {
                val paired = runCatching { desktops.paired() }.getOrDefault(emptyList())
                val mode = sessionMode()
                // Deep Doze: RTT is off for the whole phone and it is not moving (motion ends Doze), so advertise and scan at the
                // low-power rates, stop the motion sensors and tick once a minute; the receiver in init wakes us when Doze ends.
                val dozing = rtt.dozing
                val radio = mode && !dozing
                val ids = _sessions.value.values.map { it.identityId }.filter { it.isNotEmpty() }
                ble.watch(ids)
                identity.currentNow()?.id?.let { runCatching { ble.startAdvertising(it, radio) } }
                runCatching { ble.startScanning(radio) }
                if (radio) motion.start() else motion.stop()
                val keep = HashMap<String, RangeSession>()
                for (d in paired) keep[d.id] = runCatching { tick(_sessions.value[d.id]?.copy(desktop = d) ?: RangeSession(d), mode, dozing) }.getOrElse { e -> (_sessions.value[d.id] ?: RangeSession(d)).copy(error = e.message ?: e.toString()) }
                _sessions.value = keep
                pushLine()
                withTimeoutOrNull(if (dozing) DOZE_TICK_MS else if (mode) 2500L else 20_000L) { wake.receive() }
            }
        } finally { ble.stop(); motion.stop(); job = null; _sessions.value = _sessions.value.mapValues { it.value.copy(active = false) }; if (active()) kick() }
    }

    private suspend fun tick(s0: RangeSession, mode: Boolean, dozing: Boolean = false): RangeSession {
        var s = s0.copy(active = true, error = "")
        val d = s.desktop; val api = desktops.api(d); val auth = desktops.auth(d) ?: return s.copy(error = "not paired")
        val now = System.currentTimeMillis()
        // ── the desktop's ranging info + identity, every 5 min (a 404 = an older desktop: local maths only, retry in 10 min) ──
        if (s.info == null || now - s.infoAt > 5 * 60_000L || (s.supported == false && now - s.infoAt > 10 * 60_000L)) {
            val r = withTimeoutOrNull(6000) { runCatching { api.rangingInfo(auth) }.getOrNull() }
            s = when {
                r == null -> s.copy(infoAt = now, error = "desktop unreachable")
                r.code() == 404 -> s.copy(supported = false, infoAt = now, info = null, reachableAt = now)
                r.isSuccessful -> s.copy(supported = true, infoAt = now, info = r.body(), reachableAt = now).also { r.body()?.ble?.serviceUuid?.takeIf { it.isNotEmpty() }?.let { u -> ble.setServiceUuid(u) } }
                else -> s.copy(infoAt = now, error = "ranging/info HTTP ${r.code()}")
            }
            if (s.identityId.isEmpty()) withTimeoutOrNull(4000) { runCatching { api.hello() }.getOrNull()?.body()?.identity?.id }?.let { s = s.copy(identityId = it.lowercase()) }
            s = s.copy(anchor = s.info?.anchor ?: localAnchorFor(d))
            desktopAnchor.value = _sessions.value.values.firstNotNullOfOrNull { it.anchor } ?: s.anchor
        }
        val f = filters.getOrPut(d.id) { RangeFilter() }
        val dt = ((now - (lastTick[d.id] ?: now)) / 1000.0).coerceIn(0.0, 60.0); lastTick[d.id] = now
        val moving = motion.moving
        f.predict(dt, moving)
        val methods = ArrayList<String>(); var evidence = false
        // ── Wi-Fi RTT burst ──
        var rttSamples: List<RttSample> = emptyList()
        val rttInfo = s.info?.rtt?.takeIf { it.enabled && it.bssid.isNotEmpty() } ?: rttOverride
        val hold = if (mode && rttInfo != null && rttInfo.enabled && rttInfo.bssid.isNotEmpty()) rttHold(d.id, s, rttInfo.bssid, now) else null
        if (hold != null) s = s.copy(rttError = hold.second, rttState = hold.first)
        else if (mode && rttInfo != null && rttInfo.enabled && rttInfo.bssid.isNotEmpty()) {
            rttSamples = rtt.range(rttInfo)
            s = s.copy(rttError = rtt.lastError, rttState = rtt.state, rttCount = s.rttCount + rttSamples.size)
            // Only a request that went on the air counts as the minute's probe: the Doze ticks call range() too (it returns
            // at once with state=doze), and counting those held the first burst after Doze back by up to a minute.
            if (rtt.sent) lastRttTry[d.id] = now
            if (rttSamples.isNotEmpty()) { rttFails.remove(d.id); rttNextAt.remove(d.id); lastRttOkAt[d.id] = now }
            else if (rtt.state == RttState.NO_RESPONSE || rtt.state == RttState.TIMEOUT || rtt.state.startsWith("failed:")) {
                val k = (rttFails[d.id] ?: 0) + 1; rttFails[d.id] = k; rttNextAt[d.id] = now + backoffMs(k)
            }
            val offset = s.remote?.calib?.rttOffsetM ?: 0.0
            for (r in rttSamples) { f.updateRtt(r.distMm / 1000.0, max(RangeMath.RTT_FLOOR, r.stdMm / 1000.0 / sqrt(max(r.n, 1).toDouble())), offset); evidence = true; lastEvidence[d.id] = now }
            if (rttSamples.isNotEmpty()) methods += "rtt"
        } else s = when {
            !rtt.supported -> s.copy(rttError = "no Wi-Fi RTT on this phone", rttState = RttState.UNSUPPORTED)
            rttInfo == null || !rttInfo.enabled || rttInfo.bssid.isEmpty() -> s.copy(rttError = if (s.info != null) "the desktop has no RTT responder up" else "", rttState = RttState.NO_RESPONDER)
            else -> s.copy(rttError = "", rttState = RttState.IDLE)      // not a session: RTT runs only with the app open or the collector on
        }
        // ── BLE heard from the desktop (this tick's new samples go to the desktop; the last 10 s make our level) ──
        val bleNew: List<BleSample> = if (s.identityId.isNotEmpty()) ble.drain(s.identityId) else emptyList()
        s = s.copy(bleError = ble.lastError, bleCount = s.bleCount + bleNew.size)
        val batch = bleBatch.getOrPut(d.id) { ArrayList() }
        if (batch.isEmpty() && bleNew.isNotEmpty()) bleBatchStart[d.id] = now
        batch += bleNew
        val flushMs = if (moving) 2_000L else if (bleFlushed.add(d.id)) 5_000L else 30_000L
        if (batch.isNotEmpty() && now - (bleBatchStart[d.id] ?: now) >= flushMs) {
            val level = RangeMath.levelFromSamples(batch.map { it.rssi.toDouble() }, 3, (batch.maxOf { it.time } - batch.minOf { it.time }) / 1000.0, moving)
            if (level.valid) {
                val tx = batch.map { it.txPower }.firstOrNull { it != 127 } ?: s.info?.ble?.txPower ?: 127
                val p0 = s.remote?.calib?.bleP0 ?: RangeMath.priorP0Ble(tx); val n = s.remote?.calib?.bleN ?: RangeMath.N_BLE
                f.updateRssi(0, level.dbm, level.sigma, p0, n); evidence = true; lastEvidence[d.id] = now
            }
            batch.clear()
        }
        if (bleNew.isNotEmpty() || (now - (lastBleAt[d.id] ?: 0L) < 30_000)) methods += "ble"
        if (bleNew.isNotEmpty()) lastBleAt[d.id] = now
        // ── Wi-Fi vector: everything both of us hear (home APs included — the best co-location witness) ──
        val scan = status.state.value.scan.takeIf { now - status.state.value.lastScanAt < 60_000 } ?: scanner.latest()
        val wifi = scan.sortedByDescending { it.dbm }.take(40).map { WifiRssi(it.bssid.uppercase(), it.dbm, it.freq) }
        val fp = if (now - s.reachableAt < REACH_MS) fingerprint(d, auth, wifi, now) else null
        // ── fixes (phone: latest, ≤ 2 min; desktop: the live view) ──
        val pf = db.fixes().latestPhone()?.takeIf { now - it.time < 120_000 }
        val dl = live.views.value[d.id]?.location?.takeIf { it.valid }
        val fixG = if (pf != null && dl != null) { val e = AnchorMath.enu(dl.lat, dl.lon, null, pf.lat, pf.lon, null); val v = pf.acc * pf.acc + dl.accuracy * dl.accuracy; Gauss2(true, e.e, e.n, v, 0.0, v) } else Gauss2()
        // ── local fusion ──
        val haveRange = evidence || now - (lastEvidence[d.id] ?: 0L) < 60_000
        val rel = runCatching { RangeMath.relativePosterior(RelInput(haveRange, f.u, f.puu, fp != null && fp.ok, fp ?: Fingerprint(), Gauss2(), fixG)) }.getOrNull()
        val local = if (rel != null && rel.valid) LocalRange(rel.distanceM, rel.sigmaM, rel.lowM, rel.highM, rel.cls, methods + (if (fp?.ok == true) listOf("wifi-diff") else emptyList()) + (if (fixG.valid) listOf("fix") else emptyList()), if (rel.haveBearing) rel.bearingDeg else null, if (rel.haveBearing) rel.bearingSigmaDeg else null)
                    else if (haveRange) LocalRange(f.distanceM, f.sigmaM, f.lowM, f.highM, RangeMath.classify(true, f.lowM, f.highM), methods) else s.local
        s = s.copy(local = local)
        // ── tell the desktop (it fuses the reverse BLE link and its own Wi-Fi and replies with its estimate) ──
        // In Doze only when the RTT state changed or once a minute (it has nothing new to say); never while backing off an unreachable desktop.
        val stateChanged = s.rttState != s0.rttState
        val due = (if (dozing) stateChanged || now - s.lastPost > DOZE_TICK_MS else mode || now - s.lastPost > 20_000) && now >= (postNextAt[d.id] ?: 0L)
        if (s.supported == true && due && (rttSamples.isNotEmpty() || bleNew.isNotEmpty() || wifi.isNotEmpty() || stateChanged)) {
            val body = RangingPost(identity.deviceName, DateTimeFormatter.ISO_INSTANT.format(Instant.ofEpochMilli(now)), rttSamples, bleNew, wifi,
                motion.hPa?.let { Baro(it) }, moving, pf?.let { RangingFix(it.lat, it.lon, it.acc, it.time, if (it.source == "phone-wifi") "wifi" else "gps") },
                s.rttState.ifEmpty { null })
            val r = withTimeoutOrNull(6000) { runCatching { api.postRanging(auth, body) }.getOrNull() }
            // no answer: retry in 5 s doubling to 5 min instead of every tick (away from home the desktop is simply not there)
            if (r == null) { val k = (postFails[d.id] ?: 0) + 1; postFails[d.id] = k; postNextAt[d.id] = now + minOf(5_000L shl minOf(k - 1, 6), 300_000L) }
            else { postFails.remove(d.id); postNextAt.remove(d.id) }
            s = when {
                r == null -> s.copy(error = "ranging post: no answer")
                r.code() == 404 -> s.copy(supported = false, infoAt = now, reachableAt = now)
                r.isSuccessful -> s.copy(remote = r.body(), remoteAt = now, posts = s.posts + 1, lastPost = now, reachableAt = now)
                else -> s.copy(error = "ranging post HTTP ${r.code()}", lastPost = now, reachableAt = now)
            }
        }
        // ── absolute: a ring (or point) around the desktop's anchor ──
        s = s.copy(rangedFix = rangedFix(s, pf))
        return s
    }
    private val bleBatch = HashMap<String, ArrayList<BleSample>>()
    private val bleBatchStart = HashMap<String, Long>()
    private val bleFlushed = HashSet<String>()
    private val lastBleAt = HashMap<String, Long>()
    private val lastEvidence = HashMap<String, Long>()
    private val rttFails = HashMap<String, Int>()          // consecutive bursts that measured nothing
    private val rttNextAt = HashMap<String, Long>()        // backoff: no burst before this
    private val lastRttOkAt = HashMap<String, Long>()
    private val lastRttTry = HashMap<String, Long>()
    private val postFails = HashMap<String, Int>()
    private val postNextAt = HashMap<String, Long>()

    /**
     * Why no RTT burst goes out this tick, or null to range now. A burst is 8 FTM exchanges on the responder's channel: spend
     * them only while the desktop answers over the network and is plausibly in radio range, and back off while the responder
     * does not answer. Doze, Wi-Fi off and missing permissions are not holds: [RttRanging.range] returns at once with the reason.
     */
    private fun rttHold(id: String, s: RangeSession, bssid: String, now: Long): Pair<String, String>? {
        if (!rtt.supported || !rtt.available || !rtt.permitted()) return null
        if (rttOverride == null && now - s.reachableAt > REACH_MS) return RttState.AWAY to "the desktop is not answering: no RTT bursts until it does"
        val next = rttNextAt[id] ?: 0L
        if (now < next) return RttState.BACKOFF to "the responder did not answer ${rttFails[id] ?: 0} bursts in a row: next try in ${(next - now + 999) / 1000} s"
        // In range: a burst answered in the last minute, the desktop's BLE heard in the last minute, or the responder in a scan
        // of the last 3 min. Without any of that (after a long Doze nothing is fresh) a probe burst finds out, retried at once
        // when it failed (the first burst after Doze often times out, and a Doze maintenance window lasts about a minute),
        // then at most one probe a minute.
        val inRange = now - (lastRttOkAt[id] ?: 0L) < 60_000 || now - (lastBleAt[id] ?: 0L) < 60_000 || (scanner.seenAgoMs(bssid) ?: Long.MAX_VALUE) < 180_000
        if (!inRange && (rttFails[id] ?: 0) >= 2 && now - (lastRttTry[id] ?: 0L) < 60_000) return RttState.AWAY to "the responder is not in the Wi-Fi scan and the desktop's BLE is not heard: one probe burst a minute"
        return null
    }
    companion object {
        /** How long without an answer from the desktop before RTT bursts stop. */
        const val REACH_MS = 5 * 60_000L
        /** Loop period and POST heartbeat in deep Doze (RTT is off; Doze ending wakes the loop at once). */
        const val DOZE_TICK_MS = 60_000L
        /** Delay before the next burst after [k] in a row measured nothing: two quick retries (the first burst after Doze often times out), then 5 s doubling to 2 min. */
        fun backoffMs(k: Int): Long = if (k <= 2) 0L else minOf(2_500L shl minOf(k - 2, 6), 120_000L)
    }

    /** Test hook (`am start … --es rtt_bssid …`): the responder to range when the desktop does not serve /ranging/info yet. */
    @Volatile var rttOverride: org.sworrl.beaconfix.data.api.RttInfo? = null
    fun overrideRtt(info: org.sworrl.beaconfix.data.api.RttInfo?) { rttOverride = info; kick() }

    /** Shared-AP differential fingerprint (§5.4a): the desktop's current levels come from its /aps (refreshed every 30 s). */
    private suspend fun fingerprint(d: DesktopEntity, auth: String, wifi: List<WifiRssi>, now: Long): Fingerprint? {
        val cached = deskAps[d.id]
        val theirs = if (cached != null && now - cached.first < 30_000) cached.second else {
            val m = withTimeoutOrNull(5000) { runCatching { desktops.api(d).aps(auth) }.getOrNull()?.body()?.aps?.filter { it.dbm > -100 }?.associate { it.bssid.uppercase() to it.dbm } } ?: cached?.second ?: emptyMap()
            deskAps[d.id] = now to m; m
        }
        if (theirs.isEmpty()) return null
        val byGroup = HashMap<String, MutableList<Pair<Int, Int>>>()
        for (w in wifi) { val a = theirs[w.bssid] ?: continue; val p = w.bssid.split(':'); if (p.size != 6) continue; byGroup.getOrPut(p.subList(1, 5).joinToString(":")) { ArrayList() } += a to w.rssi }
        if (byGroup.size < 3) return null
        val pairs = byGroup.map { (g, l) -> val sg = RangeMath.DB_PER_NEPER / sqrt(l.size.toDouble()); DiffPair(g, l.map { it.first.toDouble() }.average(), sg, l.map { it.second.toDouble() }.average(), sg) }
        return runCatching { RangeMath.fingerprintDistance(pairs) }.getOrNull()
    }

    private suspend fun localAnchorFor(d: DesktopEntity): AnchorDto? {
        val all = anchors.allNow().filter { it.kind == "this-computer" || it.kind == "rtt-responder" }
        if (all.isEmpty()) return null
        val dl = live.views.value[d.id]?.location?.takeIf { it.valid } ?: return all.firstOrNull { it.ref } ?: all.first()
        return all.filter { Geo.distanceM(it.lat, it.lon, dl.lat, dl.lon) < 100 }.minByOrNull { Geo.distanceM(it.lat, it.lon, dl.lat, dl.lon) } ?: all.firstOrNull { it.ref }
    }
    private suspend fun refreshAnchors() { _sessions.value = _sessions.value.mapValues { (_, s) -> s.copy(anchor = s.info?.anchor ?: localAnchorFor(s.desktop)) }; desktopAnchor.value = _sessions.value.values.firstNotNullOfOrNull { it.anchor } }

    /** The phone on the ring around an anchored desktop: at the bearing when it is known, else the ring point nearest its own fix. */
    private fun rangedFix(s: RangeSession, pf: org.sworrl.beaconfix.data.db.FixEntity?): RangedFix? {
        val a = s.anchor ?: return null; val b = s.best ?: return null
        if (b.sigmaM > 8.0 || b.distanceM > 200.0) return null
        val brg = b.bearingDeg
        val sigma = sqrt(b.sigmaM * b.sigmaM + a.accM * a.accM)
        return if (brg != null) {
            val p = AnchorMath.fromEnu(a.lat, a.lon, null, b.distanceM * sin(Math.toRadians(brg)), b.distanceM * cos(Math.toRadians(brg)), 0.0)
            RangedFix(p.lat, p.lon, max(sigma, b.distanceM * Math.toRadians(b.bearingSigmaDeg ?: 20.0)), a.lat, a.lon, b.distanceM, b.sigmaM, brg)
        } else if (pf != null && Geo.distanceM(pf.lat, pf.lon, a.lat, a.lon) < 500) {
            val e = AnchorMath.enu(a.lat, a.lon, null, pf.lat, pf.lon, null); val r = sqrt(e.e * e.e + e.n * e.n)
            val (x, y) = if (r > 0.01) (e.e / r * b.distanceM) to (e.n / r * b.distanceM) else 0.0 to b.distanceM
            val p = AnchorMath.fromEnu(a.lat, a.lon, null, x, y, 0.0)
            RangedFix(p.lat, p.lon, max(sigma, kotlin.math.min(pf.acc, b.distanceM)), a.lat, a.lon, b.distanceM, b.sigmaM, null)
        } else RangedFix(a.lat, a.lon, b.distanceM + sigma, a.lat, a.lon, b.distanceM, b.sigmaM, null)
    }

    /** Refresh the notification / widgets when the range line changed meaningfully (at most every 10 s). */
    private fun pushLine() {
        val l = line(); val now = System.currentTimeMillis()
        if (l == lastLine || now - lastLineAt < 10_000) return
        lastLine = l; lastLineAt = now
        runCatching { widgets.get().touch("range") }
    }
}
