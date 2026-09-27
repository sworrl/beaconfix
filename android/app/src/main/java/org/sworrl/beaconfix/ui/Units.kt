package org.sworrl.beaconfix.ui

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.Prefs
import java.util.Locale
import kotlin.math.roundToInt

/**
 * Metric or imperial, app-wide. [metres] and [km] in Common.kt format through here, so every screen, widget and
 * notification follows Settings → Units. "auto" = imperial when the paired desktop's trip reports
 * `locale.units == "imperial"` or this phone's region is US, LR or MM; metric otherwise.
 * [imperial] is a plain volatile flag (readable from any thread); [state] is the same as a flow.
 */
object Units {
    @Volatile var imperial = false
        set(v) { field = v; _state.value = v }
    private val _state = MutableStateFlow(false)
    val state: StateFlow<Boolean> = _state.asStateFlow()

    val IMPERIAL_COUNTRIES = setOf("US", "LR", "MM")
    private const val FT_PER_M = 3.280839895
    private const val M_PER_MI = 1609.344
    private const val KM_PER_MI = 1.609344

    private var job: Job? = null
    private val json = Json { ignoreUnknownKeys = true; isLenient = true }

    /**
     * Follow the units pref (and, for "auto", the desktop's trip locale from [cache] plus the phone's region) for the
     * life of [scope]. Without [cache], "auto" goes by the phone's region only.
     */
    @Synchronized
    fun bind(prefs: Prefs, scope: CoroutineScope, cache: DesktopCache? = null) {
        job?.cancel()
        val desk: Flow<String?> = cache?.snapshot("trip")?.map { s -> s?.json?.let { desktopUnitsOf(it) } } ?: flowOf(null)
        job = scope.launch {
            combine(prefs.units, desk) { pref, d -> resolve(pref, d, Locale.getDefault().country) }
                .distinctUntilChanged()
                .collect { imperial = it }
        }
    }

    /** The rule for "auto" (pure). [pref]: auto | metric | imperial; [desktopUnits]: the trip's `locale.units`. */
    fun resolve(pref: String, desktopUnits: String?, country: String?): Boolean = when (pref) {
        "imperial" -> true
        "metric" -> false
        else -> desktopUnits.equals("imperial", ignoreCase = true) || (country != null && country.uppercase(Locale.ROOT) in IMPERIAL_COUNTRIES)
    }

    /** `locale.units` from a cached trip snapshot (either the bare trip or the `{"trip":{…}}` envelope); null if absent. */
    fun desktopUnitsOf(tripJson: String): String? = runCatching {
        val root = json.parseToJsonElement(tripJson) as? JsonObject ?: return null
        val trip = root["trip"] as? JsonObject ?: root
        val locale = trip["locale"] as? JsonObject ?: return null
        (locale["units"] as? JsonPrimitive)?.content?.takeIf { it.isNotBlank() }?.lowercase(Locale.ROOT)
    }.getOrNull()

    /** A distance in metres: "850 m" / "1.2 km", or "490 ft" / "1.0 mi" (feet below 0.1 mi). */
    fun dist(m: Double, imperial: Boolean = this.imperial): String {
        if (!imperial) return if (m >= 1000) String.format(Locale.US, "%.1f km", m / 1000) else "${m.toInt()} m"
        val ft = m * FT_PER_M
        if (ft < 528) return "${if (ft < 100) ft.roundToInt() else (ft / 10).roundToInt() * 10} ft"
        val mi = m / M_PER_MI
        return if (mi < 10) String.format(Locale.US, "%.1f mi", mi) else "${mi.roundToInt()} mi"
    }

    /** A distance already in kilometres: "5.0 km" / "58 km", or "3.1 mi" / "36 mi". */
    fun km(v: Double, imperial: Boolean = this.imperial): String {
        if (!imperial) return if (v < 10) String.format(Locale.US, "%.1f km", v) else "${v.toInt()} km"
        val mi = v / KM_PER_MI
        return if (mi < 10) String.format(Locale.US, "%.1f mi", mi) else "${mi.roundToInt()} mi"
    }

    /** A whole-kilometre setting ("150 km" / "93 mi"), e.g. the search radius slider. */
    fun wholeKm(v: Int, imperial: Boolean = this.imperial): String = if (imperial) "${(v / KM_PER_MI).roundToInt()} mi" else "$v km"

    /** A speed in m/s: "88 km/h" or "55 mph". */
    fun speed(mps: Double, imperial: Boolean = this.imperial): String =
        if (imperial) "${(mps * 3600 / M_PER_MI).roundToInt()} mph" else "${(mps * 3.6).roundToInt()} km/h"

    /** An elevation in metres: "300 m" or "984 ft". */
    fun elev(m: Double, imperial: Boolean = this.imperial): String =
        if (imperial) "${(m * FT_PER_M).roundToInt()} ft" else "${m.roundToInt()} m"
}
