package org.sworrl.beaconfix.data.api

import kotlinx.serialization.json.Json
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.ResponseBody
import retrofit2.Response
import retrofit2.Retrofit
import retrofit2.converter.kotlinx.serialization.asConverterFactory
import retrofit2.http.Body
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
    @Streaming @GET("api/v1/db/export") suspend fun export(@Header("Authorization") auth: String): Response<ResponseBody>
    @GET("api/v1/db/changes") suspend fun changes(@Header("Authorization") auth: String, @Query("since") since: String): Response<ChangesDto>
    @POST("api/v1/db/sync") suspend fun sync(@Header("Authorization") auth: String, @Body body: SyncBody): Response<ChangesDto>
    @POST("api/v1/refresh") suspend fun refresh(@Header("Authorization") auth: String): Response<ResponseBody>
    @GET("api/v1/trip") suspend fun trip(@Header("Authorization") auth: String): Response<TripDto>
    @GET("api/v1/events") suspend fun events(@Header("Authorization") auth: String, @Query("since") since: Long): Response<EventsDto>
    @GET("api/v1/pois") suspend fun poisTyped(@Header("Authorization") auth: String): Response<PoisTyped>
    @GET("api/v1/peers") suspend fun peers(): Response<PeersDto>
    @GET("api/v1/devices/positions") suspend fun devicesPositions(@Header("Authorization") auth: String): Response<DevicesPositions>
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
