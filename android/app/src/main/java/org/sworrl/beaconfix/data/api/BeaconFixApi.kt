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
