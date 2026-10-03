package org.sworrl.beaconfix.alpr

import android.graphics.Bitmap
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import javax.inject.Inject
import javax.inject.Singleton

/** One on-device read for the status card. [thumb] lives in memory only and is gone when the process ends. */
data class RecentRead(
    val text: String, val conf: Float, val atMs: Long, val lat: Double?, val lon: Double?,
    /** "hotlist" | "possible" | "" */
    val match: String = "", val thumb: Bitmap? = null,
)

data class AlprState(
    val running: Boolean = false,
    /** Why the camera is not capturing right now (null = capturing or stopped). */
    val paused: String? = null,
    val cameraSize: String = "",
    val fps: Float = 0f,
    val latencyMs: Int = 0,
    val detectMs: Int = 0,
    val ocrMs: Int = 0,
    val framesAnalysed: Long = 0,
    val platesSeen: Long = 0,
    val platesRead: Long = 0,
    val events: Long = 0,
    /** Plates followed right now (one per vehicle). */
    val tracks: Int = 0,
    /** Extra window passes over frames the rate limit skipped, for plates still too small to read. */
    val burstFrames: Long = 0,
    /** Heat step 0–3 (docs: ThermalPolicy) and the thermal headroom (NaN = unknown). */
    val thermalLevel: Int = 0,
    val headroom: Float = Float.NaN,
    /** "auto" or the short-shutter setting in use ("1/1000 s · ISO 400"). */
    val shutter: String = "",
    val memPending: Int = 0,
    val memBytes: Long = 0,
    val spillCount: Int = 0,
    val spillBytes: Long = 0,
    val uploaded: Long = 0,
    val dropped: Long = 0,
    val lastUploadMs: Long = 0,
    val lastUploadError: String = "",
    /** null = not tried yet. */
    val reachable: Boolean? = null,
    val activeUrl: String = "",
    val lastHotlistSyncMs: Long = 0,
    val hotlistSize: Int = 0,
    val thermal: String = "",
    val charging: Boolean? = null,
    val region: String = "",
    val modelError: String = "",
)

/** Process-wide live state of the dash cam for the UI, the tile and the notification. */
@Singleton
class AlprStatus @Inject constructor() {
    private val s = MutableStateFlow(AlprState())
    val state: StateFlow<AlprState> = s.asStateFlow()
    private val r = MutableStateFlow<List<RecentRead>>(emptyList())
    /** Newest first, at most [HISTORY] (text only beyond the last [THUMBS], which keep their thumbnail). */
    val recent: StateFlow<List<RecentRead>> = r.asStateFlow()

    fun set(f: (AlprState) -> AlprState) = s.update(f)

    fun addRead(read: RecentRead) = r.update { list ->
        (listOf(read) + list).take(HISTORY).mapIndexed { i, x -> if (i >= THUMBS && x.thumb != null) x.copy(thumb = null) else x }
    }

    fun clearThumbs() = r.update { list -> list.map { it.copy(thumb = null) } }

    companion object { const val HISTORY = 100; const val THUMBS = 8 }
}
