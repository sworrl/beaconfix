package org.sworrl.beaconfix.ui.vm

import android.content.Context
import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.firstOrNull
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.DesktopLive
import org.sworrl.beaconfix.data.api.Trip
import org.sworrl.beaconfix.data.api.TripDto
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.trip.GpxWriter
import org.sworrl.beaconfix.trip.Journal
import org.sworrl.beaconfix.trip.Stops
import java.time.LocalDate
import java.time.ZoneId
import javax.inject.Inject

/**
 * The trip screen's offline half: the journal of stops (from the desktop's saved track, else its stored fixes, else
 * this phone's fixes), the last saved trip summary for when no desktop view has one, this phone's own day, and the
 * GPX export. Everything here reads the local database, so it works with the desktop out of reach.
 */
@HiltViewModel
class TripViewModel @Inject constructor(
    private val db: AppDatabase, private val cache: DesktopCache, private val live: DesktopLive,
    @ApplicationContext private val ctx: Context,
) : ViewModel() {
    data class PhoneSummary(val fixes24h: Int = 0, val last: FixEntity? = null, val today: Stops.PhoneDay = Stops.PhoneDay(0.0, 0, 0))
    data class TripUi(
        val journal: Journal = Journal(),
        /** when the track the journal came from was saved (0 = not from the desktop's track) */
        val journalAt: Long = 0,
        /** the newest saved `/trip` answer and when it was saved */
        val savedTrip: Trip? = null, val savedTripAt: Long = 0,
        val phone: PhoneSummary = PhoneSummary(),
        val loaded: Boolean = false,
    )

    private val tick = MutableStateFlow(0)
    val message = MutableStateFlow("")

    val ui: StateFlow<TripUi> = combine(cache.snapshot("track"), cache.snapshot("trip"), tick) { track, trip, _ ->
        val now = System.currentTimeMillis()
        val zone = ZoneId.systemDefault()
        val desk = runCatching { db.fixes().desktopTrack().firstOrNull() ?: emptyList() }.getOrDefault(emptyList())
        val phone = runCatching { db.fixes().phoneSince(now - HISTORY_MS) }.getOrDefault(emptyList())
        val journal = Stops.journal(track?.json, desk, phone, zone)
        TripUi(
            journal = journal, journalAt = if (journal.source == Stops.TRACK) track?.fetchedAt ?: 0L else 0L,
            savedTrip = cache.decode<TripDto>(trip)?.trip, savedTripAt = trip?.fetchedAt ?: 0L,
            phone = PhoneSummary(phone.count { it.time > now - 24 * 3600_000L }, phone.lastOrNull(), Stops.phoneDay(phone, now, zone)),
            loaded = true,
        )
    }.flowOn(Dispatchers.Default).stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), TripUi())

    init { refresh() }

    /** Ask the desktops for their track (the journal updates when the snapshot lands); re-read the phone's fixes. */
    fun refresh() = viewModelScope.launch {
        runCatching { live.refreshAll(setOf("track")) }
        tick.value++
    }

    fun gpxFileName(today: LocalDate = LocalDate.now()) = "beaconfix-trip-$today.gpx"

    /** Write the journal's path and stops to [uri] (from `CreateDocument`). */
    fun exportGpx(uri: Uri) = viewModelScope.launch {
        val j = ui.value.journal
        message.value = runCatching {
            withContext(Dispatchers.IO) {
                ctx.contentResolver.openOutputStream(uri, "wt")?.bufferedWriter()?.use { w -> GpxWriter.write(w, j.path, j.stops, "BeaconFix trip") } ?: error("cannot open that file")
            }
            ctx.getString(R.string.a9_export_done, j.path.size, j.stops.size)
        }.getOrElse { e -> ctx.getString(R.string.a9_export_failed, e.message ?: e.toString()) }
    }

    companion object {
        /** How far back the phone's own fixes are read for the journal. */
        const val HISTORY_MS = 30L * 24 * 3600_000L
    }
}
