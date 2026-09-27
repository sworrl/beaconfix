package org.sworrl.beaconfix.ranging

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.sqrt

/** Barometer (relative height, ~8.4 m/hPa) and moving/still from accelerometer variance (docs/RANGING.md §2). */
@Singleton
class MotionSensors @Inject constructor(@ApplicationContext private val ctx: Context) : SensorEventListener {
    private val sm by lazy { ctx.getSystemService(SensorManager::class.java) }
    @Volatile var hPa: Double? = null; private set
    @Volatile var moving: Boolean = false; private set
    @Volatile var accelStd: Double = 0.0; private set
    val hasBarometer: Boolean get() = sm?.getDefaultSensor(Sensor.TYPE_PRESSURE) != null
    private val mags = DoubleArray(64); private var idx = 0; private var filled = 0
    private var on = false

    fun start() {
        if (on) return
        val m = sm ?: return
        m.getDefaultSensor(Sensor.TYPE_PRESSURE)?.let { m.registerListener(this, it, SensorManager.SENSOR_DELAY_NORMAL) }
        m.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)?.let { m.registerListener(this, it, SensorManager.SENSOR_DELAY_UI) }
        on = true
    }
    fun stop() { if (!on) return; sm?.unregisterListener(this); on = false }

    override fun onSensorChanged(e: SensorEvent) {
        when (e.sensor.type) {
            Sensor.TYPE_PRESSURE -> { val v = e.values[0].toDouble(); hPa = hPa?.let { it * 0.8 + v * 0.2 } ?: v }
            Sensor.TYPE_ACCELEROMETER -> {
                val g = sqrt((e.values[0] * e.values[0] + e.values[1] * e.values[1] + e.values[2] * e.values[2]).toDouble())
                mags[idx] = g; idx = (idx + 1) % mags.size; if (filled < mags.size) filled++
                if (filled >= 16) {
                    var s = 0.0; var s2 = 0.0
                    for (i in 0 until filled) { s += mags[i]; s2 += mags[i] * mags[i] }
                    val mean = s / filled; val std = sqrt((s2 / filled - mean * mean).coerceAtLeast(0.0))
                    accelStd = std
                    moving = if (moving) std > 0.25 else std > 0.4       // hysteresis: walking ≈ 1–3 m/s², a hand ≈ 0.2, a table ≈ 0.02
                }
            }
        }
    }
    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
}
