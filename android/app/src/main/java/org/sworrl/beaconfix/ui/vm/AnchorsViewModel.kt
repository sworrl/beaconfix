package org.sworrl.beaconfix.ui.vm

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import org.sworrl.beaconfix.anchors.AnchorRepository
import org.sworrl.beaconfix.anchors.BssidGroup
import org.sworrl.beaconfix.collector.CollectorStatus
import org.sworrl.beaconfix.collector.LocationSource
import org.sworrl.beaconfix.collector.WifiScanner
import org.sworrl.beaconfix.data.api.AnchorDto
import org.sworrl.beaconfix.estimate.Geo
import org.sworrl.beaconfix.estimate.ScanSample
import javax.inject.Inject
import kotlin.math.max
import kotlin.math.sqrt

/** Anchors: the list, the editor draft, the BSSID chooser's scan, and the "stand next to it" GNSS averaging. */
@HiltViewModel
class AnchorsViewModel @Inject constructor(
    private val repo: AnchorRepository, private val scanner: WifiScanner, private val status: CollectorStatus, private val location: LocationSource,
    private val ranging: org.sworrl.beaconfix.ranging.RangingRepository,
) : ViewModel() {
    val anchors: StateFlow<List<AnchorDto>> = repo.anchors.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val scan = MutableStateFlow<List<ScanSample>>(emptyList())
    val groups: List<BssidGroup> get() = AnchorRepository.groups(scan.value)
    val scanning = MutableStateFlow(false)
    fun rescan(fresh: Boolean = true) = viewModelScope.launch {
        scan.value = status.state.value.scan.ifEmpty { scanner.latest() }
        if (!fresh) return@launch
        scanning.value = true
        try { runCatching { scanner.scan(9000) }.getOrNull()?.let { if (it.isNotEmpty()) scan.value = it } } finally { scanning.value = false }
    }

    /** The anchor being edited (null = no sheet). */
    val editing = MutableStateFlow<AnchorDto?>(null)
    fun edit(a: AnchorDto?) { editing.value = a; if (a != null) rescan(fresh = false) }
    fun newAt(lat: Double, lon: Double, acc: Double = 1.0, source: String = "map-pick", kind: String = "wifi-ap") { editing.value = AnchorDto(lat = lat, lon = lon, accM = acc, source = source, kind = kind); rescan(fresh = false) }
    fun update(a: AnchorDto) { editing.value = a }
    val message = MutableStateFlow("")
    fun save(a: AnchorDto) = viewModelScope.launch { runCatching { repo.save(a) }.onSuccess { message.value = "saved ${it.name}"; editing.value = null; ranging.anchorsChanged() }.onFailure { message.value = it.message ?: "save failed" } }
    fun delete(id: String) = viewModelScope.launch { repo.delete(id); editing.value = null; ranging.anchorsChanged() }
    fun move(id: String, lat: Double, lon: Double) = viewModelScope.launch { repo.get(id)?.let { repo.save(it.copy(lat = lat, lon = lon, source = "map-pick")) }; ranging.anchorsChanged() }

    /** Where the desktop's ranging info says its antenna is (its `this-computer` anchor), for "the desktop is here" shortcuts. */
    val desktopAnchor: StateFlow<AnchorDto?> get() = ranging.desktopAnchor

    // ── precise "use my position": average GNSS fixes while you stand next to the antenna ──
    data class Averaging(val n: Int = 0, val lat: Double = 0.0, val lon: Double = 0.0, val acc: Double = -1.0, val alt: Double? = null, val elapsedS: Int = 0, val done: Boolean = false, val error: String = "")
    val averaging = MutableStateFlow<Averaging?>(null)
    private var avgJob: Job? = null
    fun startAveraging(maxS: Int = 60) {
        avgJob?.cancel()
        averaging.value = Averaging()
        avgJob = viewModelScope.launch {
            val t0 = System.currentTimeMillis()
            var sw = 0.0; var sx = 0.0; var sy = 0.0; var n = 0; var salt = 0.0; var nalt = 0
            val pts = ArrayList<DoubleArray>()
            val r = withTimeoutOrNull(maxS * 1000L) {
                location.updates(1000, 0f).collect { l ->
                    val acc = max(l.accuracy.toDouble(), 1.0); val w = 1.0 / (acc * acc)
                    sw += w; sx += w * l.latitude; sy += w * l.longitude; n++
                    if (l.hasAltitude()) { salt += l.altitude; nalt++ }
                    pts += doubleArrayOf(l.latitude, l.longitude)
                    val lat = sx / sw; val lon = sy / sw
                    // 1-σ of the weighted mean, floored by the observed scatter of the fixes about it (GNSS errors are correlated over seconds)
                    val spread = if (n > 1) sqrt(pts.sumOf { p -> val d = Geo.distanceM(p[0], p[1], lat, lon); d * d } / (n - 1)) else acc
                    val a = max(sqrt(1.0 / sw), spread / sqrt(n.toDouble()))
                    averaging.value = Averaging(n, lat, lon, a, if (nalt > 0) salt / nalt else null, ((System.currentTimeMillis() - t0) / 1000).toInt())
                }
            }
            averaging.value = (averaging.value ?: Averaging()).let { if (it.n == 0) it.copy(done = true, error = if (r == null) "no GNSS fixes — is location on?" else "stopped") else it.copy(done = true) }
        }
    }
    fun stopAveraging(): Averaging? { avgJob?.cancel(); avgJob = null; val a = averaging.value?.copy(done = true); averaging.value = a; return a?.takeIf { it.n > 0 } }
    fun clearAveraging() { avgJob?.cancel(); avgJob = null; averaging.value = null }
}
