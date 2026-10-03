package org.sworrl.beaconfix.detector

import android.content.Context
import android.content.SharedPreferences
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.AudioManager
import android.media.ToneGenerator
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject
import javax.inject.Singleton
import android.app.NotificationChannel
import android.app.NotificationManager
import kotlin.math.PI
import kotlin.math.sin

enum class DetectionType(val displayName: String) {
    ALPR_FLOCK("Flock & ALPR Cameras"),
    BLE_TRACKER("BLE Trackers & Beacons"),
    WIFI_BEACON("Unmapped Wi-Fi Beacons"),
    EMERGENCY_HELP("Emergency Facilities")
}

enum class SoundTone(val displayName: String) {
    RADAR_CHIRP("Radar Chirp (Escort style)"),
    GEIGER_CLICK("Geiger Counter Click"),
    WARBLE("Alert Warble"),
    DOUBLE_BEEP("Double Beep"),
    SONAR_PING("Sonar Ping"),
    OFF("Silent (Mute)")
}

enum class HapticPattern(val displayName: String) {
    URGENT_PULSE("Urgent Double Pulse"),
    DOUBLE_TAP("Sharp Double Tap"),
    SINGLE_TICK("Subtle Single Tick"),
    LONG_BUZZ("Heavy Alert Buzz"),
    OFF("Disabled")
}

data class DetectorSettings(
    val soundAlpr: SoundTone = SoundTone.RADAR_CHIRP,
    val vibeAlpr: HapticPattern = HapticPattern.URGENT_PULSE,
    val soundBle: SoundTone = SoundTone.DOUBLE_BEEP,
    val vibeBle: HapticPattern = HapticPattern.DOUBLE_TAP,
    val soundWifi: SoundTone = SoundTone.SONAR_PING,
    val vibeWifi: HapticPattern = HapticPattern.SINGLE_TICK,
    val soundEmergency: SoundTone = SoundTone.WARBLE,
    val vibeEmergency: HapticPattern = HapticPattern.LONG_BUZZ,
    val geigerModeEnabled: Boolean = true,
    val maxDetectionRangeM: Double = 1000.0,
    val masterSoundEnabled: Boolean = true,
    val masterHapticsEnabled: Boolean = true
)

@Singleton
class DetectorAlertManager @Inject constructor(
    @ApplicationContext private val context: Context
) {
    private val prefs: SharedPreferences = context.getSharedPreferences("beaconfix_detector_prefs", Context.MODE_PRIVATE)
    private val scope = CoroutineScope(Dispatchers.Default + Job())

    private val vibrator: Vibrator? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        val vm = context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager
        vm?.defaultVibrator
    } else {
        @Suppress("DEPRECATION")
        context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
    }

    private val _settings = MutableStateFlow(loadSettings())
    val settings: StateFlow<DetectorSettings> = _settings.asStateFlow()

    private val notifManager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    companion object {
        const val ALPR_CHANNEL_ID = "beaconfix_alpr_radar"
    }

    init {
        createNotificationChannel()
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                ALPR_CHANNEL_ID,
                "ALPR & Surveillance Radar Alerts",
                NotificationManager.IMPORTANCE_HIGH
            ).apply {
                description = "Radar sounds and proximity alerts for surveillance cameras"
                enableVibration(true)
                setShowBadge(true)
            }
            notifManager.createNotificationChannel(channel)
        }
    }

    // Camera-pass and plate-search notifications live in sightings.SightingAlerts (docs/SIGHTINGS.md §6): worded for
    // what the data shows, for the registered plates the desktop serves, and never for a traffic camera.

    private var geigerJob: Job? = null
    private var lastDistanceM: Double = Double.MAX_VALUE
    private var lastAlertTimeMs: Long = 0L

    fun updateSettings(newSettings: DetectorSettings) {
        _settings.value = newSettings
        prefs.edit()
            .putString("sound_alpr", newSettings.soundAlpr.name)
            .putString("vibe_alpr", newSettings.vibeAlpr.name)
            .putString("sound_ble", newSettings.soundBle.name)
            .putString("vibe_ble", newSettings.vibeBle.name)
            .putString("sound_wifi", newSettings.soundWifi.name)
            .putString("vibe_wifi", newSettings.vibeWifi.name)
            .putString("sound_emergency", newSettings.soundEmergency.name)
            .putString("vibe_emergency", newSettings.vibeEmergency.name)
            .putBoolean("geiger_mode", newSettings.geigerModeEnabled)
            .putFloat("max_range", newSettings.maxDetectionRangeM.toFloat())
            .putBoolean("master_sound", newSettings.masterSoundEnabled)
            .putBoolean("master_haptics", newSettings.masterHapticsEnabled)
            .apply()
    }

    private fun loadSettings(): DetectorSettings {
        return DetectorSettings(
            soundAlpr = SoundTone.valueOf(prefs.getString("sound_alpr", SoundTone.RADAR_CHIRP.name) ?: SoundTone.RADAR_CHIRP.name),
            vibeAlpr = HapticPattern.valueOf(prefs.getString("vibe_alpr", HapticPattern.URGENT_PULSE.name) ?: HapticPattern.URGENT_PULSE.name),
            soundBle = SoundTone.valueOf(prefs.getString("sound_ble", SoundTone.DOUBLE_BEEP.name) ?: SoundTone.DOUBLE_BEEP.name),
            vibeBle = HapticPattern.valueOf(prefs.getString("vibe_ble", HapticPattern.DOUBLE_TAP.name) ?: HapticPattern.DOUBLE_TAP.name),
            soundWifi = SoundTone.valueOf(prefs.getString("sound_wifi", SoundTone.SONAR_PING.name) ?: SoundTone.SONAR_PING.name),
            vibeWifi = HapticPattern.valueOf(prefs.getString("vibe_wifi", HapticPattern.SINGLE_TICK.name) ?: HapticPattern.SINGLE_TICK.name),
            soundEmergency = SoundTone.valueOf(prefs.getString("sound_emergency", SoundTone.WARBLE.name) ?: SoundTone.WARBLE.name),
            vibeEmergency = HapticPattern.valueOf(prefs.getString("vibe_emergency", HapticPattern.LONG_BUZZ.name) ?: HapticPattern.LONG_BUZZ.name),
            geigerModeEnabled = prefs.getBoolean("geiger_mode", true),
            maxDetectionRangeM = prefs.getFloat("max_range", 1000f).toDouble(),
            masterSoundEnabled = prefs.getBoolean("master_sound", true),
            masterHapticsEnabled = prefs.getBoolean("master_haptics", true)
        )
    }

    /**
     * Trigger immediate alert for a detected device or camera
     */
    fun triggerAlert(type: DetectionType, distanceM: Double? = null) {
        val s = _settings.value
        val now = System.currentTimeMillis()
        if (now - lastAlertTimeMs < 400L) return
        lastAlertTimeMs = now

        val tone = when (type) {
            DetectionType.ALPR_FLOCK -> s.soundAlpr
            DetectionType.BLE_TRACKER -> s.soundBle
            DetectionType.WIFI_BEACON -> s.soundWifi
            DetectionType.EMERGENCY_HELP -> s.soundEmergency
        }

        val pattern = when (type) {
            DetectionType.ALPR_FLOCK -> s.vibeAlpr
            DetectionType.BLE_TRACKER -> s.vibeBle
            DetectionType.WIFI_BEACON -> s.vibeWifi
            DetectionType.EMERGENCY_HELP -> s.vibeEmergency
        }

        if (s.masterHapticsEnabled && pattern != HapticPattern.OFF) {
            playHaptic(pattern)
        }

        if (s.masterSoundEnabled && tone != SoundTone.OFF) {
            playSound(tone)
        }
    }

    fun updateLastDetectedCamera(model: String, distFt: Int, direction: String) {
        prefs.edit()
            .putString("last_cam_model", model)
            .putInt("last_cam_dist_ft", distFt)
            .putString("last_cam_dir", direction)
            .apply()
    }

    /**
     * Update proximity distance to nearest ALPR camera for Geiger-counter audio feedback
     */
    fun updateProximityDistance(distanceM: Double) {
        lastDistanceM = distanceM
        val s = _settings.value
        if (!s.masterSoundEnabled || !s.geigerModeEnabled || s.soundAlpr == SoundTone.OFF || distanceM > s.maxDetectionRangeM) {
            stopGeiger()
            return
        }

        if (geigerJob == null || geigerJob?.isActive != true) {
            startGeiger()
        }
    }

    private fun startGeiger() {
        geigerJob?.cancel()
        geigerJob = scope.launch {
            while (true) {
                val d = lastDistanceM
                val s = _settings.value
                if (!s.masterSoundEnabled || !s.geigerModeEnabled || d > s.maxDetectionRangeM) {
                    break
                }

                // Geiger interval ramps up as distance decreases:
                // > 600m: 1 chirp every 1800ms
                // 300-600m: 1 chirp every 900ms
                // 150-300m: 1 chirp every 400ms
                // 50-150m: 1 chirp every 180ms
                // < 50m: 1 chirp every 90ms (solid alarm)
                val intervalMs = when {
                    d < 50.0 -> 90L
                    d < 150.0 -> 180L
                    d < 300.0 -> 400L
                    d < 600.0 -> 900L
                    else -> 1800L
                }

                if (s.masterSoundEnabled && s.soundAlpr != SoundTone.OFF) {
                    if (d < 100.0) {
                        playSynthesizedTone(frequencyHz = 1800, durationMs = 45)
                    } else {
                        playSynthesizedTone(frequencyHz = 1200, durationMs = 30)
                    }
                }

                if (s.masterHapticsEnabled && d < 120.0) {
                    playHaptic(HapticPattern.SINGLE_TICK)
                }

                delay(intervalMs)
            }
        }
    }

    private fun stopGeiger() {
        geigerJob?.cancel()
        geigerJob = null
    }

    private fun playHaptic(pattern: HapticPattern) {
        val vib = vibrator ?: return
        if (!vib.hasVibrator()) return

        when (pattern) {
            HapticPattern.URGENT_PULSE -> {
                val timings = longArrayOf(0, 140, 80, 220)
                val amps = intArrayOf(0, 255, 0, 255)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    vib.vibrate(VibrationEffect.createWaveform(timings, amps, -1))
                } else {
                    @Suppress("DEPRECATION")
                    vib.vibrate(timings, -1)
                }
            }
            HapticPattern.DOUBLE_TAP -> {
                val timings = longArrayOf(0, 60, 60, 60)
                val amps = intArrayOf(0, 200, 0, 200)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    vib.vibrate(VibrationEffect.createWaveform(timings, amps, -1))
                } else {
                    @Suppress("DEPRECATION")
                    vib.vibrate(timings, -1)
                }
            }
            HapticPattern.SINGLE_TICK -> {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    vib.vibrate(VibrationEffect.createPredefined(VibrationEffect.EFFECT_CLICK))
                } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    vib.vibrate(VibrationEffect.createOneShot(40, 180))
                } else {
                    @Suppress("DEPRECATION")
                    vib.vibrate(40)
                }
            }
            HapticPattern.LONG_BUZZ -> {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    vib.vibrate(VibrationEffect.createOneShot(320, 255))
                } else {
                    @Suppress("DEPRECATION")
                    vib.vibrate(320)
                }
            }
            HapticPattern.OFF -> {}
        }
    }

    private fun playSound(tone: SoundTone) {
        scope.launch {
            when (tone) {
                SoundTone.RADAR_CHIRP -> {
                    // Escort-style double sweep: 900Hz -> 1800Hz
                    playSynthesizedSweep(startFreq = 880, endFreq = 1760, durationMs = 60)
                    delay(40)
                    playSynthesizedSweep(startFreq = 1100, endFreq = 2100, durationMs = 70)
                }
                SoundTone.GEIGER_CLICK -> {
                    playSynthesizedTone(frequencyHz = 2400, durationMs = 25)
                }
                SoundTone.WARBLE -> {
                    playSynthesizedTone(frequencyHz = 850, durationMs = 70)
                    delay(30)
                    playSynthesizedTone(frequencyHz = 1350, durationMs = 90)
                }
                SoundTone.DOUBLE_BEEP -> {
                    playSynthesizedTone(frequencyHz = 1450, durationMs = 50)
                    delay(50)
                    playSynthesizedTone(frequencyHz = 1450, durationMs = 50)
                }
                SoundTone.SONAR_PING -> {
                    playSynthesizedSweep(startFreq = 620, endFreq = 480, durationMs = 120)
                }
                SoundTone.OFF -> {}
            }
        }
    }

    /**
     * Synthesize clean audio tone directly via AudioTrack
     */
    private fun playSynthesizedTone(frequencyHz: Int, durationMs: Int) {
        try {
            val sampleRate = 22050
            val numSamples = (sampleRate * (durationMs / 1000.0)).toInt().coerceAtLeast(100)
            val buffer = ShortArray(numSamples)
            for (i in 0 until numSamples) {
                val envelope = when {
                    i < numSamples * 0.1 -> i / (numSamples * 0.1)
                    i > numSamples * 0.8 -> (numSamples - i) / (numSamples * 0.2)
                    else -> 1.0
                }
                val angle = 2.0 * PI * i * frequencyHz / sampleRate
                buffer[i] = (sin(angle) * 32767 * envelope * 0.85).toInt().toShort()
            }

            val track = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(sampleRate)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .build()
                )
                .setBufferSizeInBytes(buffer.size * 2)
                .setTransferMode(AudioTrack.MODE_STATIC)
                .build()

            track.write(buffer, 0, buffer.size)
            track.play()
            scope.launch {
                delay(durationMs.toLong() + 50L)
                track.release()
            }
        } catch (_: Exception) {
            // Fallback to ToneGenerator if AudioTrack unavailable
            try {
                ToneGenerator(AudioManager.STREAM_NOTIFICATION, 85).apply {
                    startTone(ToneGenerator.TONE_PROP_BEEP, durationMs)
                    scope.launch {
                        delay(durationMs.toLong() + 40L)
                        release()
                    }
                }
            } catch (_: Exception) {}
        }
    }

    private fun playSynthesizedSweep(startFreq: Int, endFreq: Int, durationMs: Int) {
        try {
            val sampleRate = 22050
            val numSamples = (sampleRate * (durationMs / 1000.0)).toInt().coerceAtLeast(100)
            val buffer = ShortArray(numSamples)
            for (i in 0 until numSamples) {
                val progress = i.toDouble() / numSamples
                val currentFreq = startFreq + (endFreq - startFreq) * progress
                val envelope = when {
                    i < numSamples * 0.1 -> i / (numSamples * 0.1)
                    i > numSamples * 0.8 -> (numSamples - i) / (numSamples * 0.2)
                    else -> 1.0
                }
                val angle = 2.0 * PI * i * currentFreq / sampleRate
                buffer[i] = (sin(angle) * 32767 * envelope * 0.85).toInt().toShort()
            }

            val track = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(sampleRate)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .build()
                )
                .setBufferSizeInBytes(buffer.size * 2)
                .setTransferMode(AudioTrack.MODE_STATIC)
                .build()

            track.write(buffer, 0, buffer.size)
            track.play()
            scope.launch {
                delay(durationMs.toLong() + 50L)
                track.release()
            }
        } catch (_: Exception) {}
    }
}
