package org.sworrl.beaconfix.alpr.core

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.time.Instant
import java.time.OffsetDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/** The `meta` part of `POST /api/mobile/frames` (FalconEyez mobile contract). */
@Serializable
data class FrameMeta(
    @SerialName("captured_at") val capturedAt: String,
    val lat: Double? = null,
    val lon: Double? = null,
    @SerialName("accuracy_m") val accuracyM: Double? = null,
    @SerialName("speed_mps") val speedMps: Double? = null,
    @SerialName("heading_deg") val headingDeg: Double? = null,
    @SerialName("altitude_m") val altitudeM: Double? = null,
    @SerialName("region_code") val regionCode: String? = null,
    /** [x1,y1,x2,y2] of the uploaded crop, normalised in the full frame. */
    @SerialName("crop_box") val cropBox: List<Double> = emptyList(),
    /** Full frame [w,h] in pixels (upright). */
    @SerialName("frame_size") val frameSize: List<Int> = emptyList(),
    /** "plate" | "hotlist". */
    val reason: String = REASON_PLATE,
    @SerialName("on_device") val onDevice: OnDevice = OnDevice(),
) {
    companion object {
        const val REASON_PLATE = "plate"
        const val REASON_HOTLIST = "hotlist"
    }
}

@Serializable data class OnDevice(val plates: List<OnDevicePlate> = emptyList())

/** [box] is normalised in the crop. */
@Serializable data class OnDevicePlate(val box: List<Double>, val text: String, val conf: Double, val slots: List<List<SlotDto>> = emptyList())

@Serializable data class SlotDto(val c: String, val p: Double)

/** `POST /api/mobile/hit`. */
@Serializable
data class HitBody(
    /** Sent as a JSON number when numeric (the server's ids are int64). */
    @SerialName("entry_id") @Serializable(with = LenientString::class) val entryId: String, val plate: String, val conf: Double,
    @SerialName("captured_at") val capturedAt: String, val lat: Double? = null, val lon: Double? = null,
)

@Serializable data class HelloBody(@SerialName("device_name") val deviceName: String, @SerialName("device_model") val deviceModel: String, @SerialName("app_version") val appVersion: String)

@Serializable
data class HelloReply(
    @SerialName("device_id") @Serializable(with = LenientString::class) val deviceId: String = "",
    @SerialName("camera_id") @Serializable(with = LenientString::class) val cameraId: String = "",
    @SerialName("camera_name") val cameraName: String = "", @SerialName("server_name") val serverName: String = "",
)

/** `accepted:false` with a [reason] (e.g. the server's own geofence) is final: the event is not retried. */
@Serializable data class FramesReply(
    val accepted: Boolean = false,
    @SerialName("frame_id") @Serializable(with = LenientString::class) val frameId: String = "",
    @SerialName("camera_id") @Serializable(with = LenientString::class) val cameraId: String = "",
    val reason: String = "",
)

/** A plate event waiting to upload: the crop JPEG and its meta. Lives in memory; spilled to disk only while unsendable. */
class PendingEvent(val id: String, val createdMs: Long, val meta: FrameMeta, val jpeg: ByteArray) {
    val hotlist get() = meta.reason == FrameMeta.REASON_HOTLIST
    val bytes get() = jpeg.size.toLong() + 1024
}

object AlprJson {
    val json = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false; coerceInputValues = true }

    private val RFC3339 = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss.SSSXXX")

    /** RFC 3339 with the local offset, e.g. 2026-10-02T14:03:11.250-05:00. */
    fun rfc3339(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String = OffsetDateTime.ofInstant(Instant.ofEpochMilli(ms), zone).format(RFC3339)

    fun newId(): String = java.util.UUID.randomUUID().toString().replace("-", "").take(16)
}
