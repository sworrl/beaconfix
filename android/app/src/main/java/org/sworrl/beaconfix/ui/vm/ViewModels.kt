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
    fun toggleCollector(on: Boolean) = viewModelScope.launch { prefs.setCollectorOn(on); if (on) CollectorService.start(ctx) else CollectorService.stop(ctx) }
}

@HiltViewModel
class MapViewModel @Inject constructor(db: AppDatabase, private val store: DesktopStore) : ViewModel() {
    val aps: StateFlow<List<ApEntity>> = db.aps().positioned().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val phoneTrack: StateFlow<List<FixEntity>> = db.fixes().since(System.currentTimeMillis() - 24 * 3600_000L).stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val desktopTrack: StateFlow<List<FixEntity>> = db.fixes().desktopTrack().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val latest: StateFlow<FixEntity?> = db.fixes().latest().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), null)
}

@HiltViewModel
class BeaconsViewModel @Inject constructor(db: AppDatabase, private val estimates: EstimateRepository) : ViewModel() {
    val query = MutableStateFlow(""); val sort = MutableStateFlow("seen")
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
    fun surveyOff() { if (status.state.value.running) ctx.startService(android.content.Intent(ctx, CollectorService::class.java).setAction(CollectorService.ACTION_SURVEY_OFF)) }
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

data class SettingsUi(val collectorOn: Boolean = false, val interval: Int = 60, val maxAcc: Int = 60, val home: Set<String> = emptySet())

@HiltViewModel
class SettingsViewModel @Inject constructor(private val prefs: Prefs, @ApplicationContext private val ctx: Context) : ViewModel() {
    val ui: StateFlow<SettingsUi> = combine(prefs.collectorOn, prefs.collectIntervalSec, prefs.maxFixAccM, prefs.homePatterns) { a, b, c, d -> SettingsUi(a, b, c, d) }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), SettingsUi())
    fun setCollector(on: Boolean) = viewModelScope.launch { prefs.setCollectorOn(on); if (on) CollectorService.start(ctx) else CollectorService.stop(ctx) }
    fun setInterval(s: Int) = viewModelScope.launch { prefs.setCollectInterval(s) }
    fun setMaxAcc(m: Int) = viewModelScope.launch { prefs.setMaxFixAcc(m) }
    fun setHome(text: String) = viewModelScope.launch { prefs.setHomePatterns(text.lines().map { it.trim() }.filter { it.isNotEmpty() }.toSet()) }
}

@HiltViewModel
class PairViewModel @Inject constructor(private val discovery: Discovery, private val pairing: PairingRepository) : ViewModel() {
    val state = MutableStateFlow<PairState>(PairState.Idle)
    val found = MutableStateFlow<List<DiscoveredDesktop>>(emptyList())
    init { viewModelScope.launch { runCatching { discovery.discover().collect { found.value = it } } } }
    fun probe(host: String, port: Int, tls: Boolean) = viewModelScope.launch { state.value = PairState.Probing(host); state.value = pairing.probe(host.trim(), port, tls) }
    fun pair() = viewModelScope.launch {
        val d = (state.value as? PairState.Found)?.desktop ?: return@launch
        state.value = pairing.pair(d) { state.value = it }
    }
    fun reset() { state.value = PairState.Idle }
}
