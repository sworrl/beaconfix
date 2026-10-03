package org.sworrl.beaconfix.data.api

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.RequestBody
import okhttp3.ResponseBody
import retrofit2.Response
import retrofit2.Retrofit
import retrofit2.converter.kotlinx.serialization.asConverterFactory
import retrofit2.http.Body
import retrofit2.http.DELETE
import retrofit2.http.GET
import retrofit2.http.Header
import retrofit2.http.POST
import retrofit2.http.PUT
import retrofit2.http.Path
import retrofit2.http.Query
import retrofit2.http.Streaming
import java.util.concurrent.TimeUnit

interface BeaconFixApi {
    @GET("api/v1/hello") suspend fun hello(): Response<Hello>
    @POST("api/v1/pair") suspend fun pair(@Body body: PairRequest): Response<PairStarted>
    @GET("api/v1/pair/{id}") suspend fun pairStatus(@Path("id") id: String): Response<PairStatus>
    @POST("api/v1/pair/{id}/cancel") suspend fun pairCancel(@Path("id") id: String): Response<ResponseBody>
    @GET("api/v1/location") suspend fun location(@Header("Authorization") auth: String): Response<LocationDto>
    @GET("api/v1/aps") suspend fun aps(@Header("Authorization") auth: String): Response<ApsDto>
    @GET("api/v1/track") suspend fun track(@Header("Authorization") auth: String): Response<TrackDto>
    @GET("api/v1/pois") suspend fun pois(@Header("Authorization") auth: String): Response<PoisDto>
    @GET("api/v1/home") suspend fun home(@Header("Authorization") auth: String): Response<HomeInfo>
    @PUT("api/v1/home") suspend fun setHome(@Header("Authorization") auth: String, @Body body: HomeInfo): Response<HomeInfo>
    @POST("api/v1/locate") suspend fun locate(@Header("Authorization") auth: String, @Body body: LocateBody): Response<LocateResult>
    @GET("api/v1/db/stats") suspend fun dbStats(@Header("Authorization") auth: String): Response<DbStats>
    @POST("api/v1/db/observations") suspend fun pushObservations(@Header("Authorization") auth: String, @Body body: ObservationsBody): Response<ObservationsResult>
    @POST("api/v1/db/fixes") suspend fun pushFixes(@Header("Authorization") auth: String, @Body body: FixesBody): Response<FixesResult>
    @Streaming @GET("api/v1/db/export") suspend fun export(@Header("Authorization") auth: String): Response<ResponseBody>
    @GET("api/v1/db/changes") suspend fun changes(@Header("Authorization") auth: String, @Query("since") since: String): Response<ChangesDto>
    @POST("api/v1/db/sync") suspend fun sync(@Header("Authorization") auth: String, @Body body: SyncBody): Response<ChangesDto>
    @POST("api/v1/refresh") suspend fun refresh(@Header("Authorization") auth: String): Response<ResponseBody>
    @GET("api/v1/trip") suspend fun trip(@Header("Authorization") auth: String): Response<TripDto>
    @GET("api/v1/events") suspend fun events(@Header("Authorization") auth: String, @Query("since") since: Long): Response<EventsDto>
    @GET("api/v1/pois") suspend fun poisTyped(@Header("Authorization") auth: String): Response<PoisTyped>
    @GET("api/v1/peers") suspend fun peers(): Response<PeersDto>
    @GET("api/v1/devices/positions") suspend fun devicesPositions(@Header("Authorization") auth: String): Response<DevicesPositions>
    /** This token's own device record (desktops with the "whoami" feature): its current scopes after `--grant-control`. */
    @GET("api/v1/devices/me") suspend fun me(@Header("Authorization") auth: String): Response<DeviceMe>
    @POST("api/v1/devices/position") suspend fun devicePosition(@Header("Authorization") auth: String, @Body body: DevicePositionBody): Response<ResponseBody>
    @POST("api/v1/identity/export-request") suspend fun identityExportRequest(): Response<ResponseBody>
    // identity (spec v1)
    @GET("api/v1/identity") suspend fun identity(): Response<org.sworrl.beaconfix.identity.IdentityPublic>
    @GET("api/v1/identity/challenge") suspend fun identityChallenge(): Response<org.sworrl.beaconfix.identity.Challenge>
    @POST("api/v1/identity/auth") suspend fun identityAuth(@Body body: org.sworrl.beaconfix.identity.AuthBody): Response<org.sworrl.beaconfix.identity.AuthResult>
    @POST("api/v1/identity/link") suspend fun identityLink(@Body body: org.sworrl.beaconfix.identity.LinkStatement): Response<org.sworrl.beaconfix.identity.LinkResponse>
    @GET("api/v1/identity/export/{code}") suspend fun identityExport(@Path("code") code: String): Response<ResponseBody>
    // anchors + device ranging (docs/RANGING.md; feature-detected: a 404 means an older desktop)
    @GET("api/v1/anchors") suspend fun anchors(@Header("Authorization") auth: String): Response<List<AnchorDto>>
    @POST("api/v1/anchors") suspend fun setAnchor(@Header("Authorization") auth: String, @Body body: AnchorDto): Response<AnchorDto>
    @retrofit2.http.DELETE("api/v1/anchors/{id}") suspend fun deleteAnchor(@Header("Authorization") auth: String, @Path("id") id: String): Response<AnchorDeleted>
    @GET("api/v1/ranging/info") suspend fun rangingInfo(@Header("Authorization") auth: String): Response<RangingInfo>
    @POST("api/v1/ranging") suspend fun postRanging(@Header("Authorization") auth: String, @Body body: RangingPost): Response<DeviceRange>
    @GET("api/v1/ranging") suspend fun ranging(@Header("Authorization") auth: String): Response<RangingList>
    /** The estimator's calibration (κ, device offsets, per-band environment); a 404 means an older desktop. */
    @GET("api/v1/estimator") suspend fun estimator(@Header("Authorization") auth: String): Response<EstimatorDto>
    // help + places (desktop 3.8 adds the "pediatric" keys; treat a 404 from /emergency as an older desktop)
    @GET("api/v1/emergency") suspend fun emergency(@Header("Authorization") auth: String): Response<EmergencyDto>
    @GET("api/v1/pois") suspend fun poisFiltered(@Header("Authorization") auth: String, @Query("cat") cat: String?, @Query("group") group: String?, @Query("radius") radius: Int?): Response<PoisTyped>
    @POST("api/v1/prefetch") suspend fun prefetch(@Header("Authorization") auth: String): Response<JsonObject>
    // Flock surveillance & route heatmap
    @POST("api/v1/flock/sighting") suspend fun reportFlockSighting(@Header("Authorization") auth: String, @Body body: FlockSightingBody): Response<ResponseBody>
    /**
     * Cameras in an area, nearest first: around [lat],[lon] within [km] (the desktop's default without them: 50 km around
     * its own fix), at most [limit] (desktop default 5000, max 20000). Null parameters are left out of the request.
     */
    @GET("api/v1/flock") suspend fun flockCameras(@Header("Authorization") auth: String, @Query("lat") lat: Double? = null, @Query("lon") lon: Double? = null,
                                                  @Query("km") km: Double? = null, @Query("limit") limit: Int? = null): Response<FlockCamerasDto>
    @GET("api/v1/flock/summary") suspend fun flockSummary(@Header("Authorization") auth: String): Response<AlprSummaryDto>
    @GET("api/v1/plates") suspend fun licensePlates(@Header("Authorization") auth: String): Response<JsonObject>
    @POST("api/v1/plates") suspend fun saveLicensePlate(@Header("Authorization") auth: String, @Body plate: LicensePlateDto): Response<JsonObject>
    @DELETE("api/v1/plates/{plate}") suspend fun deleteLicensePlate(@Header("Authorization") auth: String, @Path("plate") plate: String): Response<JsonObject>
    @GET("api/v1/flock/encounters") suspend fun cameraEncounters(@Header("Authorization") auth: String, @Query("limit") limit: Int = 100): Response<JsonObject>
    @GET("api/v1/flock/audits") suspend fun plateAudits(@Header("Authorization") auth: String, @Query("limit") limit: Int = 100): Response<JsonObject>
    @POST("api/v1/flock/crossref") suspend fun crossrefOpenDatabases(@Header("Authorization") auth: String): Response<JsonObject>
    @POST("api/v1/flock/recalculate") suspend fun recalculatePasses(@Header("Authorization") auth: String): Response<JsonObject>
    @POST("api/v1/flock/sync-us") suspend fun syncNationwideUs(@Header("Authorization") auth: String): Response<JsonObject>
    @GET("api/v1/flock/sync-us") suspend fun getSyncNationwideUsStatus(@Header("Authorization") auth: String): Response<JsonObject>
    @GET("api/v1/routes/heatmap") suspend fun routeHeatmap(@Header("Authorization") auth: String): Response<RouteHeatmapDto>
    // plate events (docs/SIGHTINGS.md §5; feature-detected: a 404 means a desktop without them)
    @GET("api/v1/plate-events") suspend fun plateEvents(@Header("Authorization") auth: String, @Query("since") since: Long, @Query("limit") limit: Int? = null,
                                                       @Query("kind") kind: String? = null): Response<PlateEventsPage>
    @GET("api/v1/plate-events/{uid}") suspend fun plateEvent(@Header("Authorization") auth: String, @Path("uid") uid: String): Response<PlateEventDto>
    @POST("api/v1/plate-events") suspend fun pushPlateEvents(@Header("Authorization") auth: String, @Body body: PlateEventsPush): Response<PlateEventsAccepted>
    @POST("api/v1/plate-events/{uid}/media") suspend fun pushPlateMedia(@Header("Authorization") auth: String, @Path("uid") uid: String, @Body body: PlateMediaUpload): Response<PlateMediaUploaded>
    @Streaming @GET("api/v1/plate-events/media/{uid}") suspend fun plateMedia(@Header("Authorization") auth: String, @Path("uid") uid: String, @Query("as") asWhat: String = "display"): Response<ResponseBody>
    @GET("api/v1/plate-events/status") suspend fun plateEventsStatus(@Header("Authorization") auth: String): Response<PlateEventsStatus>
    // routing & blind-spot inspection unseen (docs/SIGHTINGS.md §8, §9)
    @POST("api/v1/route/inspect") suspend fun routeInspect(@Header("Authorization") auth: String, @Body body: JsonObject): Response<JsonObject>
    @POST("api/v1/db/import") suspend fun dbImport(@Header("Authorization") auth: String, @Query("name") name: String, @Body body: RequestBody): Response<JsonObject>
}

object ApiFactory {
    val json = Json { ignoreUnknownKeys = true; isLenient = true; explicitNulls = false; coerceInputValues = true }

    val client: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(5, TimeUnit.SECONDS)
        .readTimeout(20, TimeUnit.SECONDS)
        .writeTimeout(20, TimeUnit.SECONDS)
        .retryOnConnectionFailure(true)
        .build()

    fun baseUrl(host: String, port: Int, tls: Boolean): String {
        val h = if (host.contains(':') && !host.startsWith("[")) "[$host]" else host
        return (if (tls) "https" else "http") + "://$h:$port/"
    }

    fun create(host: String, port: Int, tls: Boolean): BeaconFixApi =
        Retrofit.Builder()
            .baseUrl(baseUrl(host, port, tls))
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(BeaconFixApi::class.java)

    /** The same client with room for a 40 MB media upload / download on a slow link. */
    val bulkClient: OkHttpClient by lazy { client.newBuilder().readTimeout(120, TimeUnit.SECONDS).writeTimeout(180, TimeUnit.SECONDS).build() }

    fun createBulk(host: String, port: Int, tls: Boolean): BeaconFixApi =
        Retrofit.Builder()
            .baseUrl(baseUrl(host, port, tls))
            .client(bulkClient)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(BeaconFixApi::class.java)

    fun bearer(token: String) = "Bearer $token"
}

/** Turn an HTTP status into what the UI should do. */
sealed class ApiOutcome {
    data object Unauthorized : ApiOutcome()      // 401: forget the token, pair again
    data object Forbidden : ApiOutcome()         // 403: not a known device / missing scope
    data object RateLimited : ApiOutcome()       // 429
    data class Failed(val code: Int, val message: String) : ApiOutcome()
    data object Ok : ApiOutcome()
}
fun <T> Response<T>.outcome(): ApiOutcome = when {
    isSuccessful -> ApiOutcome.Ok
    code() == 401 -> ApiOutcome.Unauthorized
    code() == 403 -> ApiOutcome.Forbidden
    code() == 429 -> ApiOutcome.RateLimited
    else -> ApiOutcome.Failed(code(), errorBody()?.string()?.take(200) ?: message())
}
