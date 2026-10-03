package org.sworrl.beaconfix.alpr

import android.content.Context
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import javax.inject.Inject
import javax.inject.Singleton

/** Dash-cam ALPR settings (plain SharedPreferences: nothing secret here; the pairing token lives in [FalconLink]). */
data class AlprConfig(
    /** The user wants the dash cam running (the toggle). */
    val enabled: Boolean = false,
    /** Capture only while the phone is charging. */
    val chargingOnly: Boolean = true,
    /** Ordinary plate events wait for an unmetered network; hotlist events go out on any network. */
    val wifiOnlyBacklog: Boolean = true,
    /** Disk spill cap for unsendable events, MB (max [MAX_SPILL_MB]). */
    val spillCapMb: Int = 100,
    /** "max" (≈12 MP, 4:3) | "4k" | "1080p". */
    val resolution: String = "max",
    /** Frames analysed per second (1–4); heat lowers it. */
    val targetFps: Int = 3,
    /** Columns of detector tiles across the frame (0 = full-frame pass only: fast, near plates only). */
    val tileCols: Int = 3,
    /** Post "possible match" notifications: a hotlist plate among the recognizer's alternatives, or an unconfirmed read. */
    val notifyPossible: Boolean = true,
    /** Run the models with ONNX Runtime's XNNPACK provider (else plain CPU; XNNPACK measured ~6× faster on a Pixel). */
    val xnnpack: Boolean = true,
    /** Re-read each vehicle's best frames with the larger recognizer (cct_s, ~8× the compute; fewer misreads). */
    val accurateOcr: Boolean = false,
    /** Short shutter (≈1/1000 s by day, ≤ 1/250 s at night, ISO raised) against motion blur. */
    val shortShutter: Boolean = true,
) {
    companion object { const val MAX_SPILL_MB = 500 }
}

@Singleton
class AlprSettings @Inject constructor(@ApplicationContext ctx: Context) {
    private val sp = ctx.getSharedPreferences("alpr_settings", Context.MODE_PRIVATE)
    private val state = MutableStateFlow(read())
    val config: StateFlow<AlprConfig> = state.asStateFlow()
    val value get() = state.value

    private fun read() = AlprConfig(
        enabled = sp.getBoolean("enabled", false),
        chargingOnly = sp.getBoolean("charging_only", true),
        wifiOnlyBacklog = sp.getBoolean("wifi_only_backlog", true),
        spillCapMb = sp.getInt("spill_cap_mb", 100).coerceIn(10, AlprConfig.MAX_SPILL_MB),
        resolution = sp.getString("resolution", "max") ?: "max",
        targetFps = sp.getInt("target_fps", 3).coerceIn(1, 4),
        tileCols = sp.getInt("tile_cols", 3).coerceIn(0, 4),
        notifyPossible = sp.getBoolean("notify_possible", true),
        xnnpack = sp.getBoolean("xnnpack", true),
        accurateOcr = sp.getBoolean("accurate_ocr", false),
        shortShutter = sp.getBoolean("short_shutter", true),
    )

    fun update(f: (AlprConfig) -> AlprConfig) {
        val n = f(state.value)
        sp.edit()
            .putBoolean("enabled", n.enabled).putBoolean("charging_only", n.chargingOnly).putBoolean("wifi_only_backlog", n.wifiOnlyBacklog)
            .putInt("spill_cap_mb", n.spillCapMb.coerceIn(10, AlprConfig.MAX_SPILL_MB)).putString("resolution", n.resolution)
            .putInt("target_fps", n.targetFps.coerceIn(1, 4)).putInt("tile_cols", n.tileCols.coerceIn(0, 4))
            .putBoolean("notify_possible", n.notifyPossible).putBoolean("xnnpack", n.xnnpack)
            .putBoolean("accurate_ocr", n.accurateOcr).putBoolean("short_shutter", n.shortShutter)
            .apply()
        state.value = n
    }
}
