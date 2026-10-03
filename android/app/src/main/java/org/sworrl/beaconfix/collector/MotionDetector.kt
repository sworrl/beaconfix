package org.sworrl.beaconfix.collector

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.Location
import android.os.BatteryManager
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.sqrt

enum class MotionMode {
    STATIONARY,
    ON_FOOT,
    IN_VEHICLE
}

data class MotionStatus(
    val mode: MotionMode = MotionMode.STATIONARY,
    val speedKmh: Float = 0f,
    val isCharging: Boolean = false,
    val isPluggedAc: Boolean = false,
    val isPluggedUsb: Boolean = false,
    val isPluggedWireless: Boolean = false,
    val suggestedIntervalMs: Long = 45_000L,
    val lastStateChange: Long = System.currentTimeMillis()
)

@Singleton
class MotionDetector @Inject constructor(
    @ApplicationContext private val ctx: Context
) : SensorEventListener {

    private val _status = MutableStateFlow(MotionStatus())
    val status: StateFlow<MotionStatus> = _status.asStateFlow()

    private val sensorManager = ctx.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
    private val stepSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_STEP_DETECTOR)
    private val accelSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)

    private var recentSteps = 0
    private var lastStepTime = 0L
    private var accelVariance = 0f
    private var lastAccelMag = 9.8f

    private val batteryReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action == Intent.ACTION_BATTERY_CHANGED) {
                val plugged = intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1)
                val isPlugged = plugged == BatteryManager.BATTERY_PLUGGED_AC ||
                        plugged == BatteryManager.BATTERY_PLUGGED_USB ||
                        plugged == BatteryManager.BATTERY_PLUGGED_WIRELESS
                updateCharging(
                    isCharging = isPlugged,
                    ac = plugged == BatteryManager.BATTERY_PLUGGED_AC,
                    usb = plugged == BatteryManager.BATTERY_PLUGGED_USB,
                    wireless = plugged == BatteryManager.BATTERY_PLUGGED_WIRELESS
                )
            }
        }
    }

    init {
        // Register battery receiver
        val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
        ctx.registerReceiver(batteryReceiver, filter)

        // Register hardware sensors if available
        stepSensor?.let { sensorManager?.registerListener(this, it, SensorManager.SENSOR_DELAY_NORMAL) }
        accelSensor?.let { sensorManager?.registerListener(this, it, SensorManager.SENSOR_DELAY_UI) }
    }

    fun onLocationUpdate(loc: Location) {
        val speedKmh = if (loc.hasSpeed()) loc.speed * 3.6f else 0f
        val now = System.currentTimeMillis()
        val hasRecentSteps = (now - lastStepTime) < 15_000L

        val detectedMode = when {
            speedKmh >= 14f -> MotionMode.IN_VEHICLE
            speedKmh in 2.0f..14f -> if (hasRecentSteps) MotionMode.ON_FOOT else MotionMode.IN_VEHICLE
            speedKmh in 0.5f..2.0f -> MotionMode.ON_FOOT
            hasRecentSteps -> MotionMode.ON_FOOT
            else -> MotionMode.STATIONARY
        }

        val currentCharging = _status.value.isCharging
        val interval = calculateInterval(detectedMode, currentCharging)

        _status.update {
            it.copy(
                mode = detectedMode,
                speedKmh = speedKmh,
                suggestedIntervalMs = interval,
                lastStateChange = if (it.mode != detectedMode) now else it.lastStateChange
            )
        }
    }

    private var anchorLat: Double? = null
    private var anchorLon: Double? = null
    private var anchorAcc: Float = Float.MAX_VALUE
    private var anchorTime: Long = 0L
    private var pendingJumpCount: Int = 0
    private var pendingJumpLat: Double = 0.0
    private var pendingJumpLon: Double = 0.0

    data class FilteredFix(
        val location: Location,
        val isStationaryClamped: Boolean = false,
        val isRejectedJump: Boolean = false
    )

    /**
     * Filters low-power GPS wandering and idle multipath jump artifacts.
     * When stationary, clamps locations within a 32m anchor and rejects sudden
     * large jumps (>32m) unless confirmed by physical motion or consecutive consistent fixes.
     */
    fun filterLocation(loc: Location): FilteredFix {
        onLocationUpdate(loc)
        val mode = _status.value.mode
        val now = System.currentTimeMillis()

        if (mode != MotionMode.STATIONARY) {
            anchorLat = null
            anchorLon = null
            anchorAcc = Float.MAX_VALUE
            pendingJumpCount = 0
            return FilteredFix(loc, isStationaryClamped = false, isRejectedJump = false)
        }

        val aLat = anchorLat
        val aLon = anchorLon

        if (aLat == null || aLon == null) {
            anchorLat = loc.latitude
            anchorLon = loc.longitude
            anchorAcc = if (loc.hasAccuracy()) loc.accuracy else 25f
            anchorTime = now
            pendingJumpCount = 0
            return FilteredFix(loc, isStationaryClamped = false, isRejectedJump = false)
        }

        val distFromAnchor = org.sworrl.beaconfix.estimate.Geo.distanceM(aLat, aLon, loc.latitude, loc.longitude)

        if (distFromAnchor <= 32.0) {
            // Stationary micro-jitter: clamp to anchor
            if (loc.hasAccuracy() && loc.accuracy < 15f) {
                anchorLat = 0.94 * aLat + 0.06 * loc.latitude
                anchorLon = 0.94 * aLon + 0.06 * loc.longitude
                anchorAcc = minOf(anchorAcc, loc.accuracy)
            }
            pendingJumpCount = 0

            val clamped = Location(loc).apply {
                latitude = anchorLat!!
                longitude = anchorLon!!
                if (hasSpeed()) speed = 0f
            }
            return FilteredFix(clamped, isStationaryClamped = true, isRejectedJump = false)
        }

        // Distance > 32m while physically stationary: check for real motion vs low-power jump
        val hasPhysicalMovement = _status.value.speedKmh > 1.2f || (now - lastStepTime < 10_000L) || accelVariance > 0.45f
        if (hasPhysicalMovement) {
            anchorLat = loc.latitude
            anchorLon = loc.longitude
            anchorAcc = if (loc.hasAccuracy()) loc.accuracy else 25f
            anchorTime = now
            pendingJumpCount = 0
            return FilteredFix(loc, isStationaryClamped = false, isRejectedJump = false)
        }

        val distFromPending = org.sworrl.beaconfix.estimate.Geo.distanceM(pendingJumpLat, pendingJumpLon, loc.latitude, loc.longitude)
        if (distFromPending < 20.0 && loc.hasAccuracy() && loc.accuracy < 25f) {
            pendingJumpCount++
            if (pendingJumpCount >= 3) {
                anchorLat = loc.latitude
                anchorLon = loc.longitude
                anchorAcc = loc.accuracy
                anchorTime = now
                pendingJumpCount = 0
                return FilteredFix(loc, isStationaryClamped = false, isRejectedJump = false)
            }
        } else {
            pendingJumpLat = loc.latitude
            pendingJumpLon = loc.longitude
            pendingJumpCount = 1
        }

        // Low-power GPS jump artifact suppressed
        return FilteredFix(loc, isStationaryClamped = false, isRejectedJump = true)
    }

    private fun updateCharging(isCharging: Boolean, ac: Boolean, usb: Boolean, wireless: Boolean) {
        val mode = _status.value.mode
        val interval = calculateInterval(mode, isCharging)
        _status.update {
            it.copy(
                isCharging = isCharging,
                isPluggedAc = ac,
                isPluggedUsb = usb,
                isPluggedWireless = wireless,
                suggestedIntervalMs = interval
            )
        }
    }

    private fun calculateInterval(mode: MotionMode, isCharging: Boolean): Long {
        return when (mode) {
            MotionMode.IN_VEHICLE -> if (isCharging) 3_000L else 7_000L
            MotionMode.ON_FOOT -> if (isCharging) 5_000L else 10_000L
            MotionMode.STATIONARY -> if (isCharging) 20_000L else 45_000L
        }
    }

    override fun onSensorChanged(event: SensorEvent?) {
        if (event == null) return
        val now = System.currentTimeMillis()
        when (event.sensor.type) {
            Sensor.TYPE_STEP_DETECTOR -> {
                recentSteps++
                lastStepTime = now
                if (_status.value.speedKmh < 12f && _status.value.mode == MotionMode.STATIONARY) {
                    val interval = calculateInterval(MotionMode.ON_FOOT, _status.value.isCharging)
                    _status.update { it.copy(mode = MotionMode.ON_FOOT, suggestedIntervalMs = interval) }
                }
            }
            Sensor.TYPE_ACCELEROMETER -> {
                val x = event.values[0]
                val y = event.values[1]
                val z = event.values[2]
                val mag = sqrt((x * x + y * y + z * z).toDouble()).toFloat()
                val delta = kotlin.math.abs(mag - lastAccelMag)
                accelVariance = 0.9f * accelVariance + 0.1f * delta
                lastAccelMag = mag
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
}
