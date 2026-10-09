package org.sworrl.beaconfix.ui.vm

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.collector.CollectorService
import org.sworrl.beaconfix.collector.CollectorStatus
import org.sworrl.beaconfix.collector.WifiScanner
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.DiscoveredDesktop
import org.sworrl.beaconfix.data.Discovery
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.api.LocationDto
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.estimate.EstimateRepository
import org.sworrl.beaconfix.sync.PairState
import org.sworrl.beaconfix.sync.PairingRepository
import org.sworrl.beaconfix.sync.SyncRepository
import org.sworrl.beaconfix.sync.SyncScheduler
import javax.inject.Inject

data class HomeUi(val desktops: List<DesktopEntity> = emptyList(), val desktopFix: LocationDto? = null, val desktopError: String = "",
                  val phoneFix: FixEntity? = null, val aps: Int = 0, val positioned: Int = 0, val obs: Int = 0, val unsynced: Int = 0, val collectorOn: Boolean = false)

@HiltViewModel
class HomeViewModel @Inject constructor(private val store: DesktopStore, private val db: AppDatabase, private val prefs: Prefs, @ApplicationContext private val ctx: Context) : ViewModel() {
    private val desktopFix = MutableStateFlow<LocationDto?>(null)
    private val desktopError = MutableStateFlow("")
    val ui: StateFlow<HomeUi> = combine(store.all(), db.fixes().latest(), db.aps().count(), db.aps().positionedCount(), db.observations().count(), db.observations().unsyncedCount(), prefs.collectorOn, desktopFix, desktopError) { a ->
        @Suppress("UNCHECKED_CAST")
        HomeUi(a[0] as List<DesktopEntity>, a[7] as LocationDto?, a[8] as String, a[1] as FixEntity?, a[2] as Int, a[3] as Int, a[4] as Int, a[5] as Int, a[6] as Boolean)
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), HomeUi())

    init { refreshDesktop() }
    fun refreshDesktop() = viewModelScope.launch {
        val d = store.paired().firstOrNull() ?: run { desktopError.value = ""; return@launch }
        try {
            val r = store.api(d).location(store.auth(d)!!)
            if (r.isSuccessful) { desktopFix.value = r.body(); desktopError.value = "" }
            else if (r.code() == 401) { store.forgetToken(d.id); desktopError.value = "token rejected — pair again" }
            else desktopError.value = "desktop answered ${r.code()}"
        } catch (e: Exception) { desktopError.value = "desktop unreachable" }
    }
    fun toggleCollector(on: Boolean) = viewModelScope.launch { prefs.setCollectorOn(on); CollectorService.ensure(ctx, prefs) }
}

@HiltViewModel
class MapViewModel @Inject constructor(db: AppDatabase, private val store: DesktopStore) : ViewModel() {
    val aps: StateFlow<List<ApEntity>> = db.aps().positioned().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val phoneTrack: StateFlow<List<FixEntity>> = db.fixes().since(System.currentTimeMillis() - 24 * 3600_000L).stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val desktopTrack: StateFlow<List<FixEntity>> = db.fixes().desktopTrack().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val latest: StateFlow<FixEntity?> = db.fixes().latest().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), null)
}

@HiltViewModel
class BeaconsViewModel @Inject constructor(db: AppDatabase, private val estimates: EstimateRepository, status: CollectorStatus) : ViewModel() {
    val query = MutableStateFlow(""); val sort = MutableStateFlow("seen")
    /** bssid → its newest Wi-Fi RTT range: the stored ones of the last week (read once), overlaid by this session's live ones. */
    private val stored = MutableStateFlow<Map<String, org.sworrl.beaconfix.ranging.ApRange>>(emptyMap())
    val ranges: StateFlow<Map<String, org.sworrl.beaconfix.ranging.ApRange>> = combine(stored, status.state) { a, st -> if (st.rttRanges.isEmpty()) a else a + st.rttRanges }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyMap())
    init {
        viewModelScope.launch {
            runCatching { db.observations().latestRanges(System.currentTimeMillis() - 7 * 86_400_000L) }.getOrNull()?.let { rows ->
                stored.value = rows.associate { it.bssid to org.sworrl.beaconfix.ranging.ApRange(it.bssid, it.rangeM, it.rangeSd ?: 0.0, 0, it.time) }
            }
        }
    }
    val aps: StateFlow<List<ApEntity>> = combine(db.aps().all(), query, sort) { list, q, s ->
        val f = if (q.isBlank()) list else list.filter { it.ssid.contains(q, true) || it.bssid.contains(q, true) }
        when (s) { "name" -> f.sortedBy { it.ssid.lowercase() }; "acc" -> f.sortedBy { it.acc ?: 1e9 }; "seen" -> f.sortedByDescending { it.lastSeen }; else -> f }
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    fun refitAll() = viewModelScope.launch { estimates.refitAll() }
}

@HiltViewModel
class SurveyViewModel @Inject constructor(val status: CollectorStatus, private val scanner: WifiScanner, private val prefs: Prefs, @ApplicationContext private val ctx: Context) : ViewModel() {
    val throttleHintSeen = prefs.throttleHintSeen.stateIn(viewModelScope, SharingStarted.Eagerly, true)
    fun throttled() = scanner.throttlingOn()
    fun surveyOn() { CollectorService.start(ctx, CollectorService.ACTION_SURVEY_ON) }
    fun surveyOff() { if (status.state.value.presence) runCatching { ctx.startService(android.content.Intent(ctx, CollectorService::class.java).setAction(CollectorService.ACTION_SURVEY_OFF)) } }
    fun dismissHint() = viewModelScope.launch { prefs.setThrottleHintSeen() }
}

data class SyncUi(val desktops: List<DesktopEntity> = emptyList(), val unsynced: Int = 0, val obs: Int = 0, val aps: Int = 0, val lastReport: String = "", val lastAt: Long = 0, val running: Boolean = false, val autoSync: Boolean = true, val unmetered: Boolean = false)

@HiltViewModel
class SyncViewModel @Inject constructor(private val store: DesktopStore, db: AppDatabase, private val prefs: Prefs, private val sync: SyncRepository, private val scheduler: SyncScheduler) : ViewModel() {
    private val running = MutableStateFlow(false)
    val ui: StateFlow<SyncUi> = combine(store.all(), db.observations().unsyncedCount(), db.observations().count(), db.aps().count(), prefs.lastSyncReport, prefs.lastSyncAt, running, prefs.autoSync, prefs.unmeteredOnly) { a ->
        @Suppress("UNCHECKED_CAST")
        SyncUi(a[0] as List<DesktopEntity>, a[1] as Int, a[2] as Int, a[3] as Int, a[4] as String, a[5] as Long, a[6] as Boolean, a[7] as Boolean, a[8] as Boolean)
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), SyncUi())
    fun syncNow() = viewModelScope.launch { running.value = true; try { sync.syncAll() } finally { running.value = false } }
    fun setAuto(v: Boolean) = viewModelScope.launch { prefs.setAutoSync(v) }
    fun setUnmetered(v: Boolean) = viewModelScope.launch { prefs.setUnmeteredOnly(v); scheduler.ensurePeriodic(v) }
    fun forget(d: DesktopEntity) = viewModelScope.launch { store.remove(d.id) }
}

data class SettingsUi(
    val collectorOn: Boolean = false,
    val interval: Int = 60,
    val maxAcc: Int = 60,
    val home: Set<String> = emptySet(),
    val routingOrsKey: String = "",
    val routingGraphhopperKey: String = "",
    val routingProvider: String = "ors"
)

@HiltViewModel
class SettingsViewModel @Inject constructor(private val prefs: Prefs, @ApplicationContext private val ctx: Context, private val widgets: org.sworrl.beaconfix.widget.WidgetUpdater, private val notifier: org.sworrl.beaconfix.widget.StatusNotifier) : ViewModel() {
    val statusNotification: StateFlow<Boolean> = prefs.statusNotification.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), true)
    /** show = the presence service comes up (and its permanent card); hide = the service stops and the card goes. */
    fun setStatusNotification(v: Boolean) = viewModelScope.launch { prefs.setStatusNotification(v); if (v) runCatching { CollectorService.start(ctx) } else { CollectorService.stop(ctx); notifier.clear() }; widgets.refresh(renderMap = false) }
    val ui: StateFlow<SettingsUi> = combine<Any, SettingsUi>(
        prefs.collectorOn, prefs.collectIntervalSec, prefs.maxFixAccM, prefs.homePatterns,
        prefs.routingOrsKey, prefs.routingGraphhopperKey, prefs.routingProvider
    ) { a ->
        SettingsUi(
            a[0] as Boolean,
            a[1] as Int,
            a[2] as Int,
            @Suppress("UNCHECKED_CAST") (a[3] as Set<String>),
            a[4] as String,
            a[5] as String,
            a[6] as String
        )
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), SettingsUi())
    fun setCollector(on: Boolean) = viewModelScope.launch { prefs.setCollectorOn(on); CollectorService.ensure(ctx, prefs) }
    fun setInterval(s: Int) = viewModelScope.launch { prefs.setCollectInterval(s) }
    fun setMaxAcc(m: Int) = viewModelScope.launch { prefs.setMaxFixAcc(m) }
    fun setHome(text: String) = viewModelScope.launch { prefs.setHomePatterns(text.lines().map { it.trim() }.filter { it.isNotEmpty() }.toSet()) }
    fun setRoutingOrsKey(key: String) = viewModelScope.launch { prefs.setRoutingOrsKey(key.trim()) }
    fun setRoutingGraphhopperKey(key: String) = viewModelScope.launch { prefs.setRoutingGraphhopperKey(key.trim()) }
    val doomBatteryMode: StateFlow<org.sworrl.beaconfix.data.DoomBatteryMode> = prefs.doomBatteryMode.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), org.sworrl.beaconfix.data.DoomBatteryMode.DEFAULT)
    fun setDoomBatteryMode(mode: org.sworrl.beaconfix.data.DoomBatteryMode) = viewModelScope.launch { prefs.setDoomBatteryMode(mode) }
    fun setRoutingProvider(provider: String) = viewModelScope.launch { prefs.setRoutingProvider(provider) }
}

@HiltViewModel
class PairViewModel @Inject constructor(private val discovery: Discovery, private val pairing: PairingRepository, private val ranging: org.sworrl.beaconfix.ranging.RangingRepository) : ViewModel() {
    fun ranged(desktopId: String) = ranging.ranged(desktopId)?.best
    val state = MutableStateFlow<PairState>(PairState.Idle)
    val found = MutableStateFlow<List<DiscoveredDesktop>>(emptyList())
    init { viewModelScope.launch { runCatching { discovery.discover().collect { found.value = it } } } }
    fun probe(host: String, port: Int, tls: Boolean) = viewModelScope.launch { state.value = PairState.Probing(host); state.value = pairing.probe(host.trim(), port, tls) }
    fun pair() = viewModelScope.launch {
        val found = state.value as? PairState.Found ?: return@launch
        // identity login first (no code, no prompt) when the desktop speaks identity and knows us; else the pairing code
        pairing.loginWithIdentity(found.desktop, found.hello)?.let { if (it is PairState.Paired) { state.value = it; return@launch } else if (it is PairState.Failed) { identityNote.value = it.message } }
        state.value = pairing.pair(found.desktop) { state.value = it }
    }
    val identityNote = MutableStateFlow("")
    fun reset() { state.value = PairState.Idle }
    fun cancel() = viewModelScope.launch { pairing.cancel(); state.value = PairState.Idle }
}


/** App-level state: is there an identity yet, plus the automation hooks (sync now, one scan). */
@HiltViewModel
class RootViewModel @Inject constructor(identity: org.sworrl.beaconfix.identity.IdentityStore, private val sync: SyncRepository, private val link: org.sworrl.beaconfix.link.LinkRepository, private val recorder: org.sworrl.beaconfix.collector.ObservationRecorder, private val prefs: Prefs, @ApplicationContext private val ctx: Context) : ViewModel() {
    val hasIdentity: StateFlow<Boolean?> = kotlinx.coroutines.flow.combine(identity.current, kotlinx.coroutines.flow.flowOf(Unit)) { r, _ -> r != null && identity.seed() != null }.stateIn(viewModelScope, SharingStarted.Eagerly, null)
    fun syncNow() = viewModelScope.launch { runCatching { sync.syncAll() } }
    fun scanOnce() = viewModelScope.launch { runCatching { recorder.scanAndRecord(fresh = true) } }
    fun collector(on: Boolean) = viewModelScope.launch { prefs.setCollectorOn(on); org.sworrl.beaconfix.collector.CollectorService.ensure(ctx, prefs) }
    fun hubEnrolThroughPc() = link.enrolThroughLinkedPc()
}
