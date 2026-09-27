package org.sworrl.beaconfix.ui.map

import android.content.Context
import android.util.Log
import android.widget.Toast
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.DevFlags
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.data.db.PoiEntity
import javax.inject.Inject

/**
 * The map's places layer: every cached place (works offline), the saved style and filter, and "Ask the RV to save
 * map tiles here" (`POST /api/v1/prefetch`, control scope). The desktop's token only ever goes into that one request's
 * Authorization header — never into osmdroid's tile requests.
 */
@HiltViewModel
class MapPlacesViewModel @Inject constructor(
    @ApplicationContext private val app: Context,
    private val cache: DesktopCache,
    private val prefs: Prefs,
    private val store: DesktopStore,
) : ViewModel() {
    val places: StateFlow<List<PoiEntity>> = cache.pois().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    /** null until the saved value is read (the map keeps its current tiles meanwhile). */
    val style: StateFlow<String?> = prefs.mapStyle.stateIn(viewModelScope, SharingStarted.Eagerly, null)
    val filter: StateFlow<String?> = prefs.mapPoiFilter.stateIn(viewModelScope, SharingStarted.Eagerly, null)
    /** The paired desktop that may be asked to prefetch tiles (control scope), or null. */
    val prefetchTarget: StateFlow<DesktopEntity?> = store.all()
        .map { list -> list.firstOrNull { it.paired && store.hasScope(it, "control") && store.token(it.id) != null } }
        .flowOn(Dispatchers.IO)
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), null)

    fun setStyle(key: String) = viewModelScope.launch { prefs.setMapStyle(TileStyles.normalize(key)) }
    fun setFilter(key: String) = viewModelScope.launch { prefs.setMapPoiFilter(MapFilter.normalize(key)) }

    /** A cached place by its key (for MapFocus), or null. */
    suspend fun find(key: String): PoiEntity? =
        places.value.firstOrNull { it.key == key } ?: withContext(Dispatchers.IO) { runCatching { cache.poisNow() }.getOrDefault(emptyList()) }.firstOrNull { it.key == key }

    fun prefetch() = viewModelScope.launch {
        val d = prefetchTarget.value ?: return@launch say(R.string.map_prefetch_needs_control)
        if (DevFlags.desktopBlocked()) return@launch say(R.string.map_prefetch_failed)
        val auth = store.auth(d) ?: return@launch say(R.string.map_prefetch_needs_control)
        val code = withContext(Dispatchers.IO) { withTimeoutOrNull(10_000) { runCatching { store.api(d).prefetch(auth).code() }.getOrNull() } }
        Log.i(TAG, "prefetch ${d.name}: ${code ?: "no answer"}")
        say(when (code) { null -> R.string.map_prefetch_failed; 401, 403 -> R.string.map_prefetch_forbidden; in 200..299 -> R.string.map_prefetch_ok; else -> R.string.map_prefetch_failed })
    }

    private fun say(res: Int) { Toast.makeText(app, app.getString(res), Toast.LENGTH_LONG).show() }

    private companion object { const val TAG = "BfMap" }
}
