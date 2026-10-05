package org.sworrl.beaconfix.data

/**
 * OG DOOM-themed battery usage profiles for BeaconFix.
 * Controls GPS polling cadence, Wi-Fi scanning intervals, ALPR camera framerate,
 * and CPU wakelock behavior across the entire suite.
 */
enum class DoomBatteryMode(
    val id: String,
    val title: String,
    val subtitle: String,
    val tag: String,
    val description: String,
    val flavorText: String,
    val gpsIntervalMs: Long,
    val gpsMinDistanceM: Float,
    val wifiMovingIntervalMs: Long,
    val wifiStationaryIntervalMs: Long,
    val alprFps: Int,
    val holdWakeLock: Boolean,
    val iconEmoji: String,
    val colorHex: String
) {
    IM_TOO_YOUNG_TO_DIE(
        id = "young_to_die",
        title = "I'm Too Young to Die",
        subtitle = "Ultra battery saver · 30s GPS · Relaxed Wi-Fi",
        tag = "Ultra Saver",
        description = "Minimal battery draw. Infrequent GPS polling, relaxed Wi-Fi scans, and 1 fps ALPR. Keeps your battery alive for extended trips.",
        flavorText = "Half damage received, double ammo. Minimal GPS wakeups & relaxed scans. Keeps your phone alive all day.",
        gpsIntervalMs = 30_000L,
        gpsMinDistanceM = 25f,
        wifiMovingIntervalMs = 60_000L,
        wifiStationaryIntervalMs = 180_000L,
        alprFps = 1,
        holdWakeLock = false,
        iconEmoji = "🛡️",
        colorHex = "#4CAF50" // Green
    ),

    HEY_NOT_TOO_ROUGH(
        id = "not_too_rough",
        title = "Hey, Not Too Rough",
        subtitle = "Balanced endurance · 10s GPS · 20s Wi-Fi",
        tag = "Balanced",
        description = "Balanced for daily commuting. 10-second GPS fixes, moderate Wi-Fi cadence, and 2 fps ALPR.",
        flavorText = "Default enemy speed. Moderate tracking cadence for long commutes and road trips.",
        gpsIntervalMs = 10_000L,
        gpsMinDistanceM = 10f,
        wifiMovingIntervalMs = 20_000L,
        wifiStationaryIntervalMs = 60_000L,
        alprFps = 2,
        holdWakeLock = false,
        iconEmoji = "🔋",
        colorHex = "#35D6FF" // Cyan
    ),

    HURT_ME_PLENTY(
        id = "hurt_me_plenty",
        title = "Hurt Me Plenty",
        subtitle = "Active monitor · 3s GPS · 5s Wi-Fi (Default)",
        tag = "Standard",
        description = "The standard BeaconFix experience. Dynamic motion sensing, responsive 3-4s GPS updates, and 3 fps ALPR.",
        flavorText = "The original DOOM experience. Rapid fix updates, responsive motion sensing, balanced drain.",
        gpsIntervalMs = 4_000L,
        gpsMinDistanceM = 3f,
        wifiMovingIntervalMs = 7_000L,
        wifiStationaryIntervalMs = 45_000L,
        alprFps = 3,
        holdWakeLock = false,
        iconEmoji = "⚡",
        colorHex = "#FFD166" // Amber / Gold
    ),

    ULTRA_VIOLENCE(
        id = "ultra_violence",
        title = "Ultra-Violence",
        subtitle = "High cadence · 1s GPS · 2s Wi-Fi",
        tag = "High Perf",
        description = "Fast, relentless observation logging. 1-second GPS fixes, rapid 2-second Wi-Fi scans, 4 fps ALPR. Noticeable battery drain.",
        flavorText = "More monsters, faster reflexes. Sub-second GPS tracking and aggressive Wi-Fi sniffing. Heavy battery drain.",
        gpsIntervalMs = 1_000L,
        gpsMinDistanceM = 1f,
        wifiMovingIntervalMs = 2_000L,
        wifiStationaryIntervalMs = 15_000L,
        alprFps = 4,
        holdWakeLock = true,
        iconEmoji = "🔥",
        colorHex = "#FF7043" // Fiery Orange
    ),

    NIGHTMARE(
        id = "nightmare",
        title = "Nightmare!",
        subtitle = "Battery crusher · Raw 500ms GPS · Continuous sniffer",
        tag = "Crusher",
        description = "Total slaughter. Continuous zero-throttle Wi-Fi/BLE sniffer, raw 500ms GPS stream, persistent CPU wakelock. Will crush your battery and heat your phone!",
        flavorText = "Respawning monsters, insane speed. Zero throttling, raw GPS stream, continuous sniffer. Will crush your battery and heat your phone!",
        gpsIntervalMs = 500L,
        gpsMinDistanceM = 0f,
        wifiMovingIntervalMs = 1_500L,
        wifiStationaryIntervalMs = 4_000L,
        alprFps = 4,
        holdWakeLock = true,
        iconEmoji = "💀",
        colorHex = "#FF1744" // Crimson Doom Red
    );

    companion object {
        val DEFAULT = HURT_ME_PLENTY
        fun fromId(id: String?): DoomBatteryMode = entries.firstOrNull { it.id == id || it.name.equals(id, ignoreCase = true) } ?: DEFAULT
    }
}
