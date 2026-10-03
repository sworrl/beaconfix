package org.sworrl.beaconfix.alpr.vision

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.CaptureResult
import android.util.Log
import androidx.camera.camera2.interop.Camera2CameraControl
import androidx.camera.camera2.interop.Camera2CameraInfo
import androidx.camera.camera2.interop.Camera2Interop
import androidx.camera.camera2.interop.CaptureRequestOptions
import androidx.camera.camera2.interop.ExperimentalCamera2Interop
import androidx.camera.core.Camera
import androidx.camera.core.ImageAnalysis
import org.sworrl.beaconfix.alpr.core.ShutterPolicy
import kotlin.math.roundToInt

/**
 * Applies [ShutterPolicy] to the bound camera through Camera2 interop: reads auto exposure's shutter / ISO from the
 * capture results, and sets a manual shutter + ISO (AE off) when the policy asks for it. Does nothing on cameras
 * without the MANUAL_SENSOR capability. Also exposes the lens / sensor geometry for gyro motion compensation.
 */
@androidx.annotation.OptIn(ExperimentalCamera2Interop::class)
class ShortShutter {
    @Volatile var enabled = true
    @Volatile var speedMps = 0f
    /** Focal length and sensor width (mm) of the bound camera (0 = unknown). */
    @Volatile var focalMm = 0f; private set
    @Volatile var sensorWidthMm = 0f; private set
    /** For the status card: "auto" or "1/1000 s · ISO 400". */
    @Volatile var describe = ""; private set

    @Volatile private var policy: ShutterPolicy? = null
    @Volatile private var control: Camera2CameraControl? = null

    /** Listens to capture results of the analysis stream (call before building the use case). */
    fun attach(b: ImageAnalysis.Builder) {
        Camera2Interop.Extender(b).setSessionCaptureCallback(object : CameraCaptureSession.CaptureCallback() {
            override fun onCaptureCompleted(session: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {
                val p = policy ?: return
                if (!enabled || p.manual) return
                if (result.get(CaptureResult.CONTROL_AE_MODE) == CaptureResult.CONTROL_AE_MODE_OFF) return
                val t = result.get(CaptureResult.SENSOR_EXPOSURE_TIME) ?: return
                val iso = result.get(CaptureResult.SENSOR_SENSITIVITY) ?: return
                p.onAutoResult(t, iso, speedMps)?.let { apply(it) }
            }
        })
    }

    fun bind(camera: Camera) {
        val info = Camera2CameraInfo.from(camera.cameraInfo)
        runCatching {
            focalMm = info.getCameraCharacteristic(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull() ?: 0f
            sensorWidthMm = info.getCameraCharacteristic(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)?.width ?: 0f
        }
        val manual = info.getCameraCharacteristic(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
            ?.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) == true
        val isoR = info.getCameraCharacteristic(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
        val expR = info.getCameraCharacteristic(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)
        if (!manual || isoR == null || expR == null) { policy = null; control = null; describe = "auto (no manual sensor control)"; return }
        control = Camera2CameraControl.from(camera.cameraControl)
        policy = ShutterPolicy(isoMin = isoR.lower, isoMax = minOf(isoR.upper, ISO_CAP).coerceAtLeast(isoR.lower), expMinNs = expR.lower, expMaxNs = expR.upper)
        describe = "auto"
    }

    /** Mean luma of an analysed frame (closes the loop while manual). */
    fun onLuma(luma: Int) {
        val p = policy ?: return
        if (!enabled) { if (p.manual) { p.reset(); apply(ShutterPolicy.Command.Auto) }; return }
        p.onLuma(luma, speedMps)?.let { apply(it) }
    }

    fun unbind() { policy?.reset(); policy = null; control = null; describe = "" }

    private fun apply(c: ShutterPolicy.Command) {
        val ctl = control ?: return
        runCatching {
            when (c) {
                is ShutterPolicy.Command.Manual -> {
                    ctl.captureRequestOptions = CaptureRequestOptions.Builder()
                        .setCaptureRequestOption(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                        .setCaptureRequestOption(CaptureRequest.SENSOR_EXPOSURE_TIME, c.exposureNs)
                        .setCaptureRequestOption(CaptureRequest.SENSOR_SENSITIVITY, c.iso)
                        .build()
                    describe = "1/${(1e9 / c.exposureNs).roundToInt()} s · ISO ${c.iso}"
                }
                ShutterPolicy.Command.Auto -> { ctl.clearCaptureRequestOptions(); describe = "auto" }
            }
        }.onFailure { Log.w("AlprShutter", "exposure", it) }
    }

    companion object {
        /** Beyond this the grain costs the recognizer more than the light gains. */
        const val ISO_CAP = 3200
    }
}

/**
 * Integrates the gyroscope between analysed frames: how far the phone turned about its x and y axes (radians), for the
 * tracker's motion compensation (a car turning a corner swings every plate across the frame between two frames).
 */
class GyroMotion(ctx: Context) : SensorEventListener {
    private val sm = ctx.getSystemService(SensorManager::class.java)
    private val gyro = sm?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
    private val acc = FloatArray(2)
    private var lastNs = 0L
    private var running = false

    fun start() { if (!running && gyro != null) running = sm.registerListener(this, gyro, SensorManager.SENSOR_DELAY_GAME) }

    fun stop() { if (running) sm?.unregisterListener(this); running = false; synchronized(acc) { acc.fill(0f); lastNs = 0L } }

    override fun onSensorChanged(e: SensorEvent) {
        synchronized(acc) {
            if (lastNs != 0L) {
                val dt = (e.timestamp - lastNs) * 1e-9f
                if (dt > 0f && dt < 0.2f) { acc[0] += e.values[0] * dt; acc[1] += e.values[1] * dt }
            }
            lastNs = e.timestamp
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    /** Rotation (rad) about the device x / y axes since the previous call. */
    fun take(): FloatArray = synchronized(acc) { acc.copyOf().also { acc.fill(0f) } }
}
