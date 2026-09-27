package org.sworrl.beaconfix.share

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.util.Log
import android.widget.Toast
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.flow.mapLatest
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.collector.LocationSource
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.api.DevicesPositions
import org.sworrl.beaconfix.data.api.LocationDto
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.estimate.Geo
import org.sworrl.beaconfix.help.HelpRepository
import org.sworrl.beaconfix.help.HelpSnapshot
import org.sworrl.beaconfix.identity.IdentityStore
import org.sworrl.beaconfix.ui.Intents
import javax.inject.Inject

/**
 * Share my location (one-time, through the share sheet — no live tracking, no relay) and the "Where's the RV" card.
 * [shareLocation] takes a fresh fix (8 s at most, else the last phone fix), adds the address Help resolved for that
 * spot, and opens the share sheet with coordinates and map links.
 */
@OptIn(ExperimentalCoroutinesApi::class)
@HiltViewModel
class ShareViewModel @Inject constructor(
    @ApplicationContext private val app: Context,
    private val db: AppDatabase,
    private val cache: DesktopCache,
    private val location: LocationSource,
    private val help: HelpRepository,
    private val identity: IdentityStore,
) : ViewModel() {
    val sharing = MutableStateFlow(false)

    /** Re-reads the card once a minute while it is on screen, so "seen 5 min ago" stays true. */
    private val minute = flow { while (true) { emit(System.currentTimeMillis()); delay(60_000) } }

    val rv: StateFlow<RvCardModel.State> = combine(db.fixes().latest(), cache.snapshot("location"), cache.snapshot("devices"), minute) { _, loc, dev, now -> Triple(loc, dev, now) }
        .mapLatest { (loc, dev, now) ->
            val fixes = db.fixes()
            val devices = cache.decode<DevicesPositions>(dev)
            val spots = listOf(RvCardModel.fromFix(fixes.lastDesktop()), RvCardModel.fromLocation(cache.decode<LocationDto>(loc), loc?.fetchedAt ?: 0)) +
                RvCardModel.fromDevices(devices, dev?.fetchedAt ?: 0)
            RvCardModel.state(spots, devices, dev?.fetchedAt ?: 0, fixes.lastPhone(), identity.deviceName, now)
        }
        .flowOn(Dispatchers.IO)
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), RvCardModel.State())

    /** The share sheet with this phone's position; [ctx] should be the Activity. Does nothing while a share is being prepared. */
    fun shareLocation(ctx: Context) {
        if (sharing.value) return
        sharing.value = true
        viewModelScope.launch {
            try {
                val fresh = withContext(Dispatchers.IO) { runCatching { location.current(FRESH_FIX_MS) }.getOrNull() }
                val (lat, lon, acc) = fresh?.let { Triple(it.latitude, it.longitude, if (it.hasAccuracy()) it.accuracy.toDouble() else null) }
                    ?: withContext(Dispatchers.IO) { db.fixes().lastPhone() }?.let { Triple(it.lat, it.lon, it.acc) }
                    ?: run { Toast.makeText(app, app.getString(R.string.share_no_fix), Toast.LENGTH_LONG).show(); return@launch }
                val text = ShareText.location(lat, lon, acc, addressNear(help.snapshot.value, lat, lon))
                Log.i(TAG, "share location (${if (fresh != null) "fresh" else "last"} fix)")
                val target = ctx.takeUnless { it.finishing() } ?: app
                Intents.shareText(target, text, app.getString(R.string.share_subject))
            } finally {
                sharing.value = false
            }
        }
    }

    /** The share sheet with the RV's spot. */
    fun shareRv(ctx: Context, rv: RvCardModel.Spot) {
        Intents.shareText(ctx, ShareText.location(rv.lat, rv.lon, rv.acc.takeIf { it > 0 }, null, label = app.getString(R.string.rv_share_label)), app.getString(R.string.rv_share_label))
    }

    private tailrec fun Context.finishing(): Boolean = when (this) {
        is Activity -> isFinishing || isDestroyed
        is ContextWrapper -> baseContext.finishing()
        else -> false
    }

    companion object {
        private const val TAG = "BfShare"
        const val FRESH_FIX_MS = 8_000L
        /** Help's address is for its origin; it is only added when the shared spot is that close to it. */
        const val ADDRESS_NEAR_M = 300.0

        /** "1 Test Street, Testville, PA", when [s] resolved an address within [ADDRESS_NEAR_M] of the spot; else null. */
        fun addressNear(s: HelpSnapshot, lat: Double, lon: Double): String? {
            val a = s.address ?: return null
            if (s.origin == "none" || (s.originLat == 0.0 && s.originLon == 0.0)) return null
            if (Geo.distanceM(s.originLat, s.originLon, lat, lon) > ADDRESS_NEAR_M) return null
            return listOf(a.line, a.locality, a.state).map { it.trim() }.filter { it.isNotEmpty() }.distinct().joinToString(", ").ifBlank { null }
        }
    }
}
