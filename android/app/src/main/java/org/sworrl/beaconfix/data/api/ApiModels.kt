package org.sworrl.beaconfix.data.api

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject

@Serializable data class Hello(val name: String = "", val version: String = "", val hostname: String = "", val pairing: Boolean = false, val tls: Boolean = false, val ts: String = "", val features: List<String> = emptyList())
@Serializable data class PairRequest(val name: String, val scopes: List<String>)
@Serializable data class PairStarted(val id: String, val code: String, val expires: String = "", val poll: String = "")
@Serializable data class PairStatus(val status: String, val scopes: List<String> = emptyList(), val token: String? = null)
@Serializable data class ApiError(val error: String = "")

@Serializable
data class Sun(val sunrise: String = "", val sunset: String = "", val solarNoon: String = "", val dayLength: Double = 0.0)

@Serializable
data class HomeInfo(
    val atHome: Boolean = false, val awayKm: Double = 0.0, val awayText: String = "",
    val homeLat: Double? = null, val homeLon: Double? = null, val homeTime: String = "",
    val patterns: List<String> = emptyList(),
)

@Serializable
data class LocationDto(
    val valid: Boolean = false, val lat: Double = 0.0, val lon: Double = 0.0, val accuracy: Double = -1.0,
    val source: String = "", val provider: String = "", val place: String = "",
    val city: String = "", val region: String = "", val country: String = "",
    val elevation: Double? = null, val time: String = "", @SerialName("age_s") val ageS: Double? = null,
    val sun: Sun? = null, val geo: String = "", val home: HomeInfo? = null,
)

@Serializable
data class ApDto(
    val bssid: String = "", val ssid: String = "", val dbm: Int = -100, val freq: Int = 0, val ch: Int = 0, val band: String = "",
    val kind: String = "", val lat: Double? = null, val lon: Double? = null, val r: Double? = null, val status: String = "",
    val security: String = "", val rsnFlags: Int = 0, val wpaFlags: Int = 0, val home: Boolean = false, val insecure: Boolean = false,
)
@Serializable data class ApsDto(val aps: List<ApDto> = emptyList())
@Serializable data class PoisDto(val pois: List<JsonObject> = emptyList())
@Serializable data class TrackPoint(val lat: Double = 0.0, val lon: Double = 0.0, val acc: Double = 0.0, val source: String = "", val time: String = "", val place: String = "")
@Serializable data class TrackDto(val track: List<TrackPoint> = emptyList())

@Serializable
data class ObservationDto(val bssid: String, val ssid: String = "", val dbm: Int, val lat: Double, val lon: Double, val acc: Double, val time: String, val source: String = "android")
@Serializable data class ObservationsBody(val observations: List<ObservationDto>)
@Serializable data class ObservationsResult(val added: Int = 0, val error: String = "")

@Serializable data class LocateAp(val macAddress: String, val signalStrength: Int)
@Serializable data class LocateBody(val wifiAccessPoints: List<LocateAp>)
@Serializable data class LatLng(val lat: Double = 0.0, val lng: Double = 0.0)
@Serializable data class LocateResult(val location: LatLng = LatLng(), val accuracy: Double = 0.0, val used: Int = 0)

@Serializable data class DbStats(val aps: Int = 0, val observations: Int = 0, val fixes: Int = 0, val pois: Int = 0, val encrypted: Boolean = false, val sizeBytes: Long = 0)

/** Incremental sync (desktop feature "sync", feature-detected via hello.features). */
@Serializable data class ChangesDto(val cursor: String = "", val aps: List<JsonObject> = emptyList(), val observations: List<JsonObject> = emptyList(), val fixes: List<JsonObject> = emptyList())
@Serializable data class SyncBody(val cursor: String, val observations: List<ObservationDto>)

/** The full export: tables of raw rows; we only read what we understand. */
@Serializable data class ExportDto(val aps: List<JsonObject> = emptyList(), val observations: List<JsonObject> = emptyList(), val fixes: List<JsonObject> = emptyList(), val pois: List<JsonObject> = emptyList())

