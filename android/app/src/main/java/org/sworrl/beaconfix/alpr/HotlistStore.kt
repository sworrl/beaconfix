package org.sworrl.beaconfix.alpr

import android.content.Context
import android.util.Log
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import org.sworrl.beaconfix.alpr.core.AlprJson
import org.sworrl.beaconfix.alpr.core.Hotlist
import org.sworrl.beaconfix.alpr.core.HotlistMatcher
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The FalconEyez hotlist (public AMBER / Silver / Blue alerts), our own plates and the blocked regions, cached in app
 * storage so on-device matching works with no connection to the RV. Refreshed whenever the server is reachable,
 * at most every [REFRESH_MS] unless forced.
 */
@Singleton
class HotlistStore @Inject constructor(@ApplicationContext ctx: Context, private val link: FalconLink, private val status: AlprStatus) {
    private val file = File(ctx.noBackupFilesDir, "alpr_hotlist.json")
    private val stampFile = File(ctx.noBackupFilesDir, "alpr_hotlist.stamp")
    private val list = MutableStateFlow(load())
    val hotlist: StateFlow<Hotlist> = list.asStateFlow()
    @Volatile var matcher: HotlistMatcher = HotlistMatcher(list.value); private set
    @Volatile var lastSyncMs: Long = stampFile.takeIf { it.exists() }?.readText()?.trim()?.toLongOrNull() ?: 0L; private set

    init { publish() }

    private fun load(): Hotlist = runCatching { AlprJson.json.decodeFromString(Hotlist.serializer(), file.readText()) }.getOrDefault(Hotlist())

    private fun publish() { status.set { it.copy(hotlistSize = list.value.entries.size, lastHotlistSyncMs = lastSyncMs) } }

    /** Fetches a fresh copy when due (or [force]); true when the cache is now current. */
    suspend fun refresh(force: Boolean = false): Boolean = withContext(Dispatchers.IO) {
        if (!force && System.currentTimeMillis() - lastSyncMs < REFRESH_MS) return@withContext true
        when (val r = link.hotlist()) {
            is Call.Ok -> {
                val h = r.value
                runCatching {
                    val tmp = File(file.path + ".tmp"); tmp.writeText(AlprJson.json.encodeToString(Hotlist.serializer(), h)); tmp.renameTo(file)
                    lastSyncMs = System.currentTimeMillis(); stampFile.writeText(lastSyncMs.toString())
                }
                list.value = h; matcher = HotlistMatcher(h); publish()
                Log.i(TAG, "hotlist: ${h.entries.size} entries, ${h.ownPlates.size} own, blocked ${h.blockedRegions}")
                true
            }
            else -> { Log.i(TAG, "hotlist refresh: $r"); false }
        }
    }

    fun clear() { file.delete(); stampFile.delete(); lastSyncMs = 0; list.value = Hotlist(); matcher = HotlistMatcher(list.value); publish() }

    companion object {
        const val REFRESH_MS = 15 * 60_000L
        private const val TAG = "AlprHotlist"
    }
}
