package org.sworrl.beaconfix.data.api

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject

@Serializable data class Hello(val name: String = "", val version: String = "", val hostname: String = "", val pairing: Boolean = false, val tls: Boolean = false, val ts: String = "", val features: List<String> = emptyList(), val identity: HelloIdentity? = null)
@Serializable data class PairIdentity(val id: String, val pub: String)
@Serializable data class SasPub(val pub: String = "", val picked: Boolean? = null)
@Serializable data class ProxBeacon(val bssid: String, val dbm: Int)
@Serializable data class Proximity(val lat: Double? = null, val lon: Double? = null, val acc: Double? = null, val ts: String = "", val source: String = "", val beacons: List<ProxBeacon> = emptyList(),
                                   /** what the phone has already measured to this desktop (docs/RANGING.md), if anything */
                                   val rangedM: Double? = null, val rangedSigmaM: Double? = null, val rangedMethod: String? = null)
@Serializable data class ProximityAnswer(val shared: Int = 0, val theirs: Int = 0, val ours: Int = 0, val strongestShared: List<String> = emptyList(), val rssiDelta: Double? = null, val distanceM: Double? = null, val verdict: String = "unknown")
@Serializable data class PairRequest(val name: String, val scopes: List<String>, val kind: String = "android", val identity: PairIdentity? = null, val sas: SasPub? = null, val proximity: Proximity? = null)
@Serializable data class PairStarted(val id: String, val code: String, val expires: String = "", val poll: String = "", val sas: SasPub? = null, val proximity: ProximityAnswer? = null)
@Serializable data class PairStatus(val status: String, val scopes: List<String> = emptyList(), val token: String? = null, val proximity: ProximityAnswer? = null, val sas: SasPub? = null)
@Serializable data class ApiError(val error: String = "")


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
    val sun: SunFull? = null, val geo: String = "", val home: HomeInfo? = null, val links: Links? = null,
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
data class ObservationDto(val bssid: String, val ssid: String = "", val dbm: Int, val lat: Double, val lon: Double, val acc: Double, val time: String, val source: String = "android", val identity: String? = null)
@Serializable data class ObservationsBody(val observations: List<ObservationDto>, val identity: String? = null, val device: String? = null)
@Serializable data class ObservationsResult(val added: Int = 0, val error: String = "")

@Serializable data class LocateAp(val macAddress: String, val signalStrength: Int)
@Serializable data class LocateBody(val wifiAccessPoints: List<LocateAp>)
@Serializable data class LatLng(val lat: Double = 0.0, val lng: Double = 0.0)
@Serializable data class LocateResult(val location: LatLng = LatLng(), val accuracy: Double = 0.0, val used: Int = 0)

@Serializable data class DbStats(val aps: Int = 0, val observations: Int = 0, val fixes: Int = 0, val pois: Int = 0, val encrypted: Boolean = false, val sizeBytes: Long = 0)

/** Incremental sync (desktop feature "sync", feature-detected via hello.features). */
@Serializable data class ChangesDto(val cursor: String = "", val aps: List<JsonObject> = emptyList(), val observations: List<JsonObject> = emptyList(), val fixes: List<JsonObject> = emptyList(), val anchors: List<JsonObject> = emptyList())
@Serializable data class SyncBody(val cursor: String, val observations: List<ObservationDto>, val identity: String? = null, val device: String? = null, val anchors: List<JsonObject>? = null)

/** The full export: tables of raw rows; we only read what we understand. */
@Serializable data class ExportDto(val aps: List<JsonObject> = emptyList(), val observations: List<JsonObject> = emptyList(), val fixes: List<JsonObject> = emptyList(), val pois: List<JsonObject> = emptyList())


// ── richer desktop views (3.4+/3.5+) ─────────────────────────────────────────
@Serializable data class SunFull(val sunrise: String = "", val sunset: String = "", val solarNoon: String = "", val civilDawn: String = "", val civilDusk: String = "",
                                 val goldenMorningEnd: String = "", val goldenEveningStart: String = "", val dayLengthSecs: Long = 0, val isDay: Boolean = true, val polarDay: Boolean = false, val polarNight: Boolean = false)
@Serializable data class Links(val osm: String = "", val google: String = "", val apple: String = "")
@Serializable data class RankTier(val at: Int = 0, val name: String = "")
@Serializable data class Achievement(val key: String = "", val title: String = "", val desc: String = "", val icon: String = "", val at: String = "", val unlocked: Boolean = false)
@Serializable data class LocaleInfo(val country: String = "", val countryCode: String = "", val dialingCode: String = "", val emergencyNumber: String = "", val region: String = "", val timezone: String = "", val units: String = "")
@Serializable data class TripIdentity(val id: String = "", val name: String = "", val grouped: String = "", val linked: Int = 0, val pendingLinks: Int = 0)
@Serializable data class HomeFixDto(val lat: Double = 0.0, val lon: Double = 0.0, val time: String = "")
@Serializable data class Trip(
    val rank: String = "", val rankLevel: Int = 0, val rankCount: Int = 0, val rankAt: Int = 0, val nextRankAt: Int = 0, val rankLadder: List<RankTier> = emptyList(),
    val beaconsTotal: Int = 0, val beaconsNow: Int = 0, val locatedNow: Int = 0, val fitted: Int = 0, val bestAccuracy: Double = 0.0,
    val distanceTodayKm: Double = 0.0, val distanceTripKm: Double = 0.0, val distanceAllKm: Double = 0.0, val longestLegKm: Double = 0.0,
    val movingSecs: Long = 0, val stoppedSecs: Long = 0, val movingTripSecs: Long = 0, val stoppedTripSecs: Long = 0, val dwellSecs: Long = 0,
    val speedKmh: Double = -1.0, val heading: String = "", val headingDeg: Double = 0.0, val moving: Boolean = false,
    val stops: Int = 0, val stopsToday: Int = 0, val stopsTrip: Int = 0, val longestStaySecs: Long = 0, val longestStayPlace: String = "",
    val cities: List<String> = emptyList(), val regions: List<String> = emptyList(), val countries: List<String> = emptyList(),
    val achievements: List<Achievement> = emptyList(), val achievementsTotal: Int = 0, val achievementsUnlocked: Int = 0,
    val atHome: Boolean = false, val awayKm: Double = 0.0, val awayText: String = "", val homeFix: HomeFixDto? = null,
    val identity: TripIdentity? = null, val locale: LocaleInfo? = null, val timezone: String = "",
)
@Serializable data class TripDto(val trip: Trip = Trip(), val ts: String = "")
@Serializable data class VantageDto(val lat: Double = 0.0, val lon: Double = 0.0, val dbm: Int = 0, val device: String = "")
@Serializable data class EventDto(val id: Long = 0, val type: String = "", val time: String = "", val text: String = "", val bssid: String = "", val ssid: String = "", val dbm: Int = 0, val lat: Double? = null, val lon: Double? = null, val kind: String = "", val status: String = "",
                                  val fromLat: Double? = null, val fromLon: Double? = null, val acc: Double? = null, val prevAcc: Double? = null, val n: Int = 0, val vantage: Int = 0, val rms: Double = 0.0, val vantagePoints: List<VantageDto> = emptyList())
@Serializable data class EventsDto(val events: List<EventDto> = emptyList(), val lastEventId: Long = 0, val since: Long = 0)
/** One place from `/api/v1/pois` (and state `pois[]`). The fields after [osm] arrived with desktop 3.8 (pediatric); older desktops leave them out. */
@Serializable data class PoiDto(val name: String = "", val label: String = "", val cat: String = "", val group: String = "", val icon: String = "", val color: String = "",
                                val lat: Double = 0.0, val lon: Double = 0.0, val d: Double = 0.0, val brg: Double = 0.0, val address: String = "", val detail: String = "",
                                val phone: String = "", val hours: String = "", val website: String = "", val osm: String = "",
                                val osmType: String = "", val osmId: Long = 0L, val wheelchair: String = "", val emergency: Boolean = false, val wifi: Boolean = false,
                                /** 0 none, 1 pediatric ER, 2 children's hospital (ER not confirmed), 3 ER with pediatrics, 4 pediatric urgent care */
                                val peds: Int = 0, val er: String = "", val campusEr: String = "", val scope: String = "near",
                                val driveS: Int = 0, val driveM: Int = 0, val driveEst: Boolean = true)
@Serializable data class PoisTyped(val pois: List<PoiDto> = emptyList(), val count: Int = 0, val ts: String = "",
                                   val categories: List<PoiCategoryDto> = emptyList(), val note: String = "", val origin: OriginDto? = null, val pedsOrigin: OriginDto? = null)
/** An entry of `poiCategories` / `/pois` `categories`. [reachKm] 0 = the desktop's usual wide rule. */
@Serializable data class PoiCategoryDto(val key: String = "", val label: String = "", val icon: String = "", val color: String = "", val group: String = "", val groupLabel: String = "",
                                        val wide: Boolean = false, val reachKm: Int = 0)
/** Where a desktop answer was computed from. */
@Serializable data class OriginDto(val lat: Double = 0.0, val lon: Double = 0.0, val acc: Double? = null, val source: String = "", val time: String = "", val radiusKm: Double = 0.0)
/** One place in `/api/v1/emergency`: [d] metres and [brg] degrees from the desktop's fix (absent without a fix). */
@Serializable data class HelpPlaceDto(val name: String = "", val lat: Double = 0.0, val lon: Double = 0.0, val d: Double? = null, val brg: Double? = null,
                                      val phone: String = "", val address: String = "", val hours: String = "", val website: String = "", val osm: String = "",
                                      val tier: Int = 0, val er: String = "", val campusEr: String = "", val driveS: Int = 0, val driveM: Int = 0, val driveEst: Boolean = true,
                                      val notEr: Boolean = false,
                                      /** `urgent` only: an actual urgent care (desktop 3.8+ never puts a plain clinic there). */
                                      val urgentCare: Boolean = false)
/** `GET /api/v1/devices/me`: the calling token's device (name, kind, scopes). */
@Serializable data class DeviceMe(val name: String = "", val kind: String = "", val scopes: List<String> = emptyList())
/** `/api/v1/emergency`. The `pediatric*` keys and [origin] exist on desktops with the "pediatric" feature (3.8+); a 3.7 desktop leaves them out. */
@Serializable data class EmergencyDto(val number: String = "", val countryCode: String = "",
                                      val police: HelpPlaceDto? = null, val fire: HelpPlaceDto? = null, val hospital: HelpPlaceDto? = null, val urgent: HelpPlaceDto? = null,
                                      val pharmacy: HelpPlaceDto? = null, val vet: HelpPlaceDto? = null,
                                      val pediatric: HelpPlaceDto? = null, val pediatricCloser: HelpPlaceDto? = null, val pediatricUrgent: HelpPlaceDto? = null,
                                      val pediatricNote: String = "", val pediatricSearchKm: Int = 0, val pediatricTime: String = "", val origin: OriginDto? = null, val ts: String = "")
@Serializable data class PeerDto(val host: String = "", val port: Int = 47822, val name: String = "", val hostname: String = "", val kind: String = "", val id: String = "", val version: String = "", val pairing: Boolean = false, val features: List<String> = emptyList())
@Serializable data class PeersDto(val peers: List<PeerDto> = emptyList())
@Serializable data class HelloIdentity(val id: String = "", val name: String = "")
@Serializable data class LinkedDevice(val device: String = "", val kind: String = "", val identityId: String = "", val identityName: String = "", val lat: Double = 0.0, val lon: Double = 0.0, val acc: Double = -1.0, val time: String = "", val ageS: Double? = null, val source: String = "", val online: Boolean = true, val beacons: Int = 0)
@Serializable data class DevicesPositions(val devices: List<LinkedDevice> = emptyList())
@Serializable data class DevicePositionBody(val lat: Double, val lon: Double, val acc: Double, val time: String, val beacons: Int = 0, val source: String = "gps")


// ── anchors (docs/RANGING.md §4, frozen contract) ────────────────────────────
@Serializable data class RvOffset(val eastM: Double = 0.0, val northM: Double = 0.0, val upM: Double = 0.0)
@OptIn(kotlinx.serialization.ExperimentalSerializationApi::class)
@Serializable data class AnchorDto(
    val id: String = "", @kotlinx.serialization.EncodeDefault val name: String = "", @kotlinx.serialization.EncodeDefault val kind: String = "custom",
    val lat: Double = 0.0, val lon: Double = 0.0, val alt: Double? = null, val heightM: Double? = null, val floor: Int? = null,
    @kotlinx.serialization.EncodeDefault val accM: Double = 1.0, @kotlinx.serialization.EncodeDefault val bssids: List<String> = emptyList(), val ble: String? = null,
    @kotlinx.serialization.EncodeDefault val rv: Boolean = false, val rvOffset: RvOffset? = null, @kotlinx.serialization.EncodeDefault val ref: Boolean = false, val headingDeg: Double? = null,
    @kotlinx.serialization.EncodeDefault val placedBy: String = "android", @kotlinx.serialization.EncodeDefault val placedAt: String = "", @kotlinx.serialization.EncodeDefault val source: String = "map-pick",
    // output-only / sync-only
    val headingAssumed: Boolean? = null, val seq: Long? = null, val deleted: Boolean? = null, val deletedAt: String? = null,
)
@Serializable data class AnchorDeleted(val deleted: String = "")

// ── device ranging (docs/RANGING.md §7) ──────────────────────────────────────
@Serializable data class RttInfo(val bssid: String = "", val freqMHz: Int = 0, val centerFreq0MHz: Int = 0, val centerFreq1MHz: Int = 0, val bandwidthMHz: Int = 20, val channel: Int = 0, val preamble: String = "ht", val enabled: Boolean = false, val anchorId: String? = null)
@Serializable data class BleInfo(val serviceUuid: String = "", val txPower: Int = 127, val enabled: Boolean = false, val intervalMs: Int = 1000)
@Serializable data class RangingInfo(val rtt: RttInfo? = null, val ble: BleInfo? = null, val anchor: AnchorDto? = null)
@Serializable data class RttSample(val bssid: String, val distMm: Int, val stdMm: Int, val rssi: Int, val burst: Int, val n: Int, val time: Long)
// The POST DTOs below carry no defaults on purpose: ApiFactory.json does not encode defaults, and docs/API.md promises
// rtt[], ble[], wifi[], moving, ble[].txPower and fix.source in every body (only channel, baro, fix and rttState are optional).
@Serializable data class BleSample(val rssi: Int, val channel: Int? = null, val txPower: Int, val time: Long)
@Serializable data class WifiRssi(val bssid: String, val rssi: Int, val freq: Int)
@Serializable data class Baro(val hPa: Double)
@Serializable data class RangingFix(val lat: Double, val lon: Double, val acc: Double, val time: Long, val source: String)
@Serializable data class RangingPost(val device: String, val time: String, val rtt: List<RttSample>, val ble: List<BleSample>, val wifi: List<WifiRssi>,
                                     val baro: Baro? = null, val moving: Boolean, val fix: RangingFix? = null,
                                     /** why [rtt] is (not) empty: ok | doze | wifi-off | location-off | unavailable | unsupported | no-permission | no-response | not-80211mc | timeout | bad-config | no-responder | idle | away | backoff | failed:<code> (ranging/RttRanging.kt RttState) */
                                     val rttState: String? = null)
@Serializable data class RangeSamples(val rtt: Int = 0, val ble: Int = 0, val wifiDiff: Int = 0)
@Serializable data class RangeCalib(val rttOffsetM: Double? = null, val bleP0: Double? = null, val bleN: Double? = null, val bleP0Up: Double? = null)
@Serializable data class DeviceRange(val device: String = "", val distanceM: Double? = null, val sigmaM: Double? = null, val lowM: Double? = null, val highM: Double? = null,
                                     val method: List<String> = emptyList(), val bearingDeg: Double? = null, val bearingSigmaDeg: Double? = null, val dz: Double? = null,
                                     @SerialName("class") val cls: String = "unknown", val updated: String = "", val samples: RangeSamples? = null, val calib: RangeCalib? = null)
@Serializable data class RangingList(val updated: String = "", val anchor: AnchorDto? = null, val devices: List<DeviceRange> = emptyList())
