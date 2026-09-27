package org.sworrl.beaconfix.ui.vm

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.mapLatest
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.DesktopLive
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.DesktopView
import org.sworrl.beaconfix.data.DevFlags
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.estimate.Geo
import org.sworrl.beaconfix.help.HelpRepository
import org.sworrl.beaconfix.help.LivePlaces
import org.sworrl.beaconfix.help.OpeningHours
import org.sworrl.beaconfix.help.Origin
import org.sworrl.beaconfix.ui.places.PlaceFilters
import java.time.Instant
import java.time.LocalDateTime
import java.time.ZoneId
import java.util.Locale
import javax.inject.Inject

/** A place as the list shows it: measured from the current [Origin]; [closes] "17:00" while open. */
data class PlaceUi(val row: PoiEntity, val distM: Double, val brgDeg: Double, val openNow: Boolean?, val closes: String?)

/** What the banner above the list says about the data. [newest] epoch ms (0 none); [distM] from the search origin (-1 unknown). */
data class PlacesStatus(val newest: Long = 0, val distM: Double = -1.0, val fromPhone: Boolean = false, val near: String = "")

enum class PlacesEmpty { NONE, UNPAIRED, UNREACHABLE, FETCHING, FILTERED }

/** Pure list logic, JVM-tested. */
object PlacesModel {
    fun localAt(now: Long): LocalDateTime = LocalDateTime.ofInstant(Instant.ofEpochMilli(now), ZoneId.systemDefault())

    /** Filter, then measure from [o] (or each row's own search origin when nothing is known) and sort nearest first. */
    fun build(rows: List<PoiEntity>, o: Origin.Chosen, f: PlaceFilters.Filter, now: Long): List<PlaceUi> {
        val at = localAt(now)
        return rows.filter { PlaceFilters.matches(it, f, at) }.map { r ->
            val (fromLat, fromLon) = if (o.known) o.lat to o.lon else r.originLat to r.originLon
            val known = Origin.valid(fromLat, fromLon)
            val open = if (r.hours.isBlank()) null else OpeningHours.isOpen(r.hours, at)
            val closes = if (open == true) OpeningHours.closesAt(r.hours, at)?.let { String.format(Locale.US, "%02d:%02d", it.hour, it.minute) } else null
            PlaceUi(r, if (known) Geo.distanceM(fromLat, fromLon, r.lat, r.lon) else -1.0, if (known) Geo.bearingDeg(fromLat, fromLon, r.lat, r.lon) else 0.0, open, closes)
        }.sortedWith(compareBy<PlaceUi>({ if (it.distM < 0) 1 else 0 }, { it.distM }, { it.row.name.lowercase(Locale.ROOT) }))
    }

    fun status(rows: List<PoiEntity>, o: Origin.Chosen, near: String = ""): PlacesStatus {
        val newest = rows.maxByOrNull { it.fetchedAt } ?: return PlacesStatus()
        val d = if (o.known && Origin.valid(newest.originLat, newest.originLon)) Geo.distanceM(o.lat, o.lon, newest.originLat, newest.originLon) else -1.0
        return PlacesStatus(newest.fetchedAt, d, newest.source == DesktopCache.PHONE, near)
    }

    fun empty(paired: Boolean, reachable: Boolean, total: Int, shown: Int): PlacesEmpty = when {
        shown > 0 -> PlacesEmpty.NONE
        total > 0 -> PlacesEmpty.FILTERED
        !paired -> PlacesEmpty.UNPAIRED
        !reachable -> PlacesEmpty.UNREACHABLE
        else -> PlacesEmpty.FETCHING
    }

    /** The RV's newest position: a live view's fix or the stored desktop fix. */
    fun rvFix(views: Collection<DesktopView>, stored: Origin.Fix?): Origin.Fix? {
        val live = views.mapNotNull { v -> v.location?.takeIf { it.valid && v.fetched > 0 }?.let { Origin.Fix(it.lat, it.lon, it.accuracy, v.fetched - ((it.ageS ?: 0.0) * 1000).toLong()) } }
        return (live + listOfNotNull(stored)).filter { Origin.valid(it.lat, it.lon) }.maxByOrNull { it.time }
    }
}

/**
 * Places from the offline mirror (every desktop and this phone, DesktopCache) plus what the live desktop views hold,
 * measured from the phone when its fix is fresh and from the RV otherwise. Works with no desktop in reach.
 */
@OptIn(ExperimentalCoroutinesApi::class)
@HiltViewModel
class PlacesViewModel @Inject constructor(
    private val cache: DesktopCache,
    private val db: AppDatabase,
    private val live: DesktopLive,
    store: DesktopStore,
    private val help: HelpRepository,
) : ViewModel() {
    val filter = MutableStateFlow(PlaceFilters.Filter())
    val refreshing = MutableStateFlow(false)
    private val searching = MutableStateFlow(false)
    /** True while "Search from this phone" runs. */
    val phoneSearching: StateFlow<Boolean> = searching

    /** A clock that ticks every 30 s so "open now" and "saved … ago" stay true. */
    private val clock: Flow<Long> = flow { while (true) { emit(System.currentTimeMillis()); delay(30_000) } }

    private val all: StateFlow<List<PoiEntity>> = combine(cache.pois(), live.views) { cached, views -> DesktopCache.dedupe(cached + LivePlaces.rows(views.values)) }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    /** Where distances are measured from; re-chosen whenever a fix lands or a desktop answers. */
    val origin: StateFlow<Origin.Chosen> = combine(db.fixes().latest(), live.views, clock) { _, views, now -> views to now }
        .mapLatest { (views, now) ->
            val rv = PlacesModel.rvFix(if (DevFlags.desktopBlocked()) emptyList() else views.values, Origin.of(db.fixes().lastDesktop()))
            Origin.choose(Origin.of(db.fixes().lastPhone()), rv, now)
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), Origin.Chosen(Origin.NONE))

    val places: StateFlow<List<PlaceUi>> = combine(all, origin, filter, clock) { rows, o, f, now -> PlacesModel.build(rows, o, f, now) }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    val total: StateFlow<Int> = all.map { it.size }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    val status: StateFlow<PlacesStatus> = combine(all, origin, db.fixes().latest()) { rows, o, _ -> rows to o }
        .mapLatest { (rows, o) -> PlacesModel.status(rows, o, db.fixes().lastDesktop()?.place.orEmpty()) }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), PlacesStatus())

    val paired: StateFlow<Boolean> = store.all().map { l -> l.any { it.paired } }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), true)

    /** A desktop answered this session (and the dev switches do not hide it). */
    val reachable: StateFlow<Boolean> = live.views.map { v -> !DevFlags.desktopBlocked() && v.values.any { it.error.isEmpty() && it.fetched > 0 } }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), false)

    init {
        // opening Places: ask the desktops; with none in reach Help may search from the phone (its own rules apply)
        viewModelScope.launch { help.refresh(force = false) }
    }

    fun setFilter(f: PlaceFilters.Filter) { filter.value = f }

    /** Pull-to-refresh: the desktops' places and help, then a forced help refresh (the phone searches if no desktop answers). */
    fun refresh() {
        if (refreshing.value) return
        viewModelScope.launch {
            refreshing.value = true
            try {
                if (!DevFlags.desktopBlocked()) withTimeoutOrNull(12_000) { live.refreshAll(setOf("pois", "emergency")) }
                help.refresh(force = true)
            } finally { refreshing.value = false }
        }
    }

    /** "Search from this phone" when no desktop is reachable. */
    fun searchFromPhone() {
        viewModelScope.launch { searching.value = true; try { help.refresh(force = true) } finally { searching.value = false } }
    }
}
