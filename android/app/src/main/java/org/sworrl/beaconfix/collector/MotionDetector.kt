package org.sworrl.beaconfix.collector

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.TriggerEvent
import android.hardware.TriggerEventListener
import android.os.Handler
import android.os.Looper
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

    /**
     * Not going anywhere: no significant motion, no walking, no GPS speed and no walking-level shaking for
     * [IDLE_AFTER_MS]. Handling the phone in a chair doesn't count as moving. GPS, node fixes and scans back off while
     * this is true. It goes false the moment one of those shows up; the significant-motion sensor wakes the CPU for that,
     * so nothing has to poll while the phone sleeps.
     */
    private val _idle = MutableStateFlow(false)
    val idle: StateFlow<Boolean> = _idle.asStateFlow()
    @Volatile private var lastMovedAt = System.currentTimeMillis()
    @Volatile private var idleAt: Pair<Double, Double>? = null
    private val main = Handler(Looper.getMainLooper())
    private val idleCheck = object : Runnable {
        override fun run() {
            if (!_idle.value && System.currentTimeMillis() - lastMovedAt >= IDLE_AFTER_MS) { android.util.Log.i("BeaconFixMotion", "idle"); setIdle(true) }
            main.postDelayed(this, 30_000L)
        }
    }

    /** Evidence of going somewhere: leaves idle at once. */
    fun moved(why: String = "") {
        lastMovedAt = System.currentTimeMillis()
        if (_idle.value) { android.util.Log.i("BeaconFixMotion", "moving ($why)"); setIdle(false) }
    }

    private fun setIdle(on: Boolean) {
        _idle.value = on
        idleAt = null
        if (on) _status.update { it.copy(mode = MotionMode.STATIONARY, suggestedIntervalMs = calculateInterval(MotionMode.STATIONARY, it.isCharging)) }
    }

    private val sigMotion by lazy { sensorManager?.getDefaultSensor(Sensor.TYPE_SIGNIFICANT_MOTION) }
    private val sigMotionListener = object : TriggerEventListener() {
        override fun onTrigger(event: TriggerEvent?) {
            moved("significant motion")
            // one-shot: arm it again (a minute later; it keeps firing while walking and once a minute is all we need)
            main.postDelayed({ armSigMotion() }, 60_000L)
        }
    }
    private fun armSigMotion() { sigMotion?.let { runCatching { sensorManager?.requestTriggerSensor(sigMotionListener, it) } } }

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

        // Register hardware sensors if available. The accelerometer is batched in the sensor hub (5 Hz, delivered every
        // 10 s) so the CPU can sleep between batches; it was 16 Hz delivered one sample at a time, all day.
        stepSensor?.let { sensorManager?.registerListener(this, it, SensorManager.SENSOR_DELAY_NORMAL, 10_000_000) }
        accelSensor?.let { sensorManager?.registerListener(this, it, SensorManager.SENSOR_DELAY_NORMAL, 10_000_000) }
        armSigMotion()
        main.postDelayed(idleCheck, 30_000L)
    }

    fun onLocationUpdate(loc: Location) {
        val speedKmh = if (loc.hasSpeed()) loc.speed * 3.6f else 0f
        val now = System.currentTimeMillis()
        // a believable speed (not the wander of a poor fix) is going somewhere
        if (speedKmh >= 5f && (!loc.hasAccuracy() || loc.accuracy < 30f)) moved("speed")
        // while idle the fixes are other apps' (passive): one well away from where we stopped means we left
        if (!loc.hasAccuracy() || loc.accuracy < 50f) {
            val a = idleAt
            if (!_idle.value) idleAt = null
            else if (a == null) idleAt = loc.latitude to loc.longitude
            else if (org.sworrl.beaconfix.estimate.Geo.distanceM(a.first, a.second, loc.latitude, loc.longitude) > 150.0) moved("left the spot")
        }
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
                // a few steps across the room are not a trip: 8 within a minute are
                if (now - lastStepTime > 60_000L) recentSteps = 0
                recentSteps++
                lastStepTime = now
                if (recentSteps >= 8) moved("steps")
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
                if (accelVariance > 1.2f) moved("shaking")          // walking or a rough road; a hand on a phone is ≈ 0.1
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    companion object {
        const val IDLE_AFTER_MS = 3 * 60_000L
    }
}
