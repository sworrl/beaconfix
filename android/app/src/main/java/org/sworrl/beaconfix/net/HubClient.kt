package org.sworrl.beaconfix.net

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.ConnectionSpec
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.Interceptor
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import okhttp3.sse.EventSource
import okhttp3.sse.EventSourceListener
import okhttp3.sse.EventSources
import okio.Buffer
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.BeaconFixApi
import org.sworrl.beaconfix.data.api.ChangesDto
import org.sworrl.beaconfix.data.api.FixDto
import org.sworrl.beaconfix.data.api.ObservationDto
import retrofit2.Retrofit
import retrofit2.converter.kotlinx.serialization.asConverterFactory
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Query
import java.io.IOException
import java.net.ConnectException
import java.net.NoRouteToHostException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton
import javax.net.ssl.SSLException

/** The hub answered, but not in a way we can trust (a bad tag, another request's counter, an unsealed success). */
class HubSecurityException(message: String, cause: Throwable? = null) : IOException(message, cause)

/**
 * `/api/v1/db/sync` as the hub takes it from a phone: observations, fixes, anchors and plate-event records in one body
 * (docs/API.md, docs/SIGHTINGS.md §5). A null list is left out of the JSON.
 */
@Serializable data class HubSyncBody(val device: String, val kind: String, val identity: String?, val observations: List<ObservationDto>, val fixes: List<FixDto>, val anchors: List<JsonObject>?,
                                     val plateEvents: List<org.sworrl.beaconfix.data.api.PlateEventDto>? = null,
                                     val nodeDetections: List<NodeDetectionDto>? = null)

/** What the phone sends a paired desktop's /db/sync: just the node detections. */
@Serializable data class NodeSyncBody(val device: String, val identity: String?, val nodeDetections: List<NodeDetectionDto>)

/** One node_detections row for the hub (docs/API.md, /db/sync `nodeDetections`); [uid] is the merge key. */
@Serializable data class NodeDetectionDto(val uid: String, val node: String, val kind: String, val mac: String, val ssid: String,
                                          val rssi: Int, val ch: Int, val detail: String, val timeMs: Long,
                                          val lat: Double? = null, val lon: Double? = null, val acc: Double? = null,
                                          val locSource: String = "", val stored: Boolean = false)

/** The hub routes whose shapes differ from the LAN client's ([BeaconFixApi] covers the rest, same DTOs). */
interface HubApi {
    @POST("api/v1/db/sync") suspend fun sync(@Body body: HubSyncBody): retrofit2.Response<JsonObject>
    @GET("api/v1/db/changes") suspend fun changes(@Query("since") since: String, @Query("limit") limit: Int): retrofit2.Response<ChangesDto>
}

/** What a failure means for the user, in one line. */
object HubErrors {
    const val UNREACHABLE = "hub unreachable — is WireGuard on?"
    fun isUnreachable(e: Throwable): Boolean = e is UnknownHostException || e is ConnectException || e is NoRouteToHostException || e is SocketTimeoutException ||
        (e is IOException && e !is SSLException && e !is HubSecurityException && (e.message ?: "").let { it.contains("unreachable", true) || it.contains("timeout", true) || it.contains("Failed to connect", true) })
    fun describe(e: Throwable): String = when {
        e is HubSecurityException -> "hub answer rejected: ${e.message}"
        e is SSLException -> "TLS to the hub failed (${e.message?.take(80)}) — check the address and the certificate"
        isUnreachable(e) -> UNREACHABLE
        else -> (e.message ?: e.toString()).take(160)
    }
    fun ofStatus(code: Int, skewS: Long): String = when (code) {
        401 -> if (kotlin.math.abs(skewS) > Bfs3.TIME_WINDOW_S) "hub refused the request: this phone's clock is ${kotlin.math.abs(skewS) / 60} min off" else "hub refused this device (revoked, or enrolled again elsewhere) — enrol again"
        403 -> "hub: not allowed for this device"
        413 -> "hub: request too large"
        429 -> "hub: rate limited — try again in a minute"
        else -> "hub error HTTP $code"
    }
}

/**
 * The BFS3 client (docs/SECURE-API.md): every request to the hub is the v1 request, sealed, sent to `/api/v3/<route>`,
 * and every answer is opened and checked before Retrofit sees it. [api] / [hubApi] are ordinary Retrofit interfaces
 * over an OkHttp client whose [Bfs3Interceptor] does the sealing, so the sync and the live code reuse the LAN client's
 * routes and DTOs unchanged (the `Authorization` argument they take is dropped: the device key authenticates).
 *
 * Transport: https only (OkHttp is limited to TLS connection specs and the URL is checked), no redirects and no silent
 * retries (a retried sealed request would reuse its counter and be refused as a replay).
 */
@Singleton
class HubClient @Inject constructor(private val store: HubStore) {
    @Volatile private var session: Bfs3Session? = null
    /** Sealing, opening and the clock (learned from the hub's `X-BF-Time` header). */
    val wire = Bfs3Wire()
    val clockSkewS: Long get() = wire.clockSkewS

    fun enrolled(): Boolean = store.cached != null || store.config() != null

    @Synchronized fun session(): Bfs3Session? {
        val cfg = store.config() ?: run { session = null; return null }
        session?.let { if (it.cfg.deviceId == cfg.deviceId) { if (it.cfg != cfg) session = Bfs3Session(cfg, it.rk, it.counters); return session } }
        val sk = store.deviceKey() ?: return null
        val rk = Bfs3.rootKeyOf(sk, cfg.serverPub)
        sk.fill(0)
        return Bfs3Session(cfg, rk, CounterAllocator(store)).also { session = it }
    }
    /** Drop the in-memory session (after forgetting or re-enrolling). */
    @Synchronized fun reset() { session?.rk?.fill(0); session = null; apis = null }

    fun nowS(): Long = wire.nowS()

    private val plain: OkHttpClient = OkHttpClient.Builder()
        .connectionSpecs(listOf(ConnectionSpec.MODERN_TLS, ConnectionSpec.COMPATIBLE_TLS))   // no CLEARTEXT spec: http:// cannot be dialled
        .connectTimeout(8, TimeUnit.SECONDS).readTimeout(30, TimeUnit.SECONDS).writeTimeout(30, TimeUnit.SECONDS)
        .retryOnConnectionFailure(false).followRedirects(false).followSslRedirects(false)
        .build()
    private val sealedClient: OkHttpClient = plain.newBuilder().addInterceptor(Bfs3Interceptor({ session() }, wire)).build()

    private class Apis(val url: String, val api: BeaconFixApi, val hub: HubApi, val node: HubNodeApi)
    @Volatile private var apis: Apis? = null
    private fun apis(): Apis? {
        val url = store.config()?.url ?: return null
        apis?.takeIf { it.url == url }?.let { return it }
        val r = Retrofit.Builder().baseUrl(url).client(sealedClient).addConverterFactory(ApiFactory.json.asConverterFactory("application/json".toMediaType())).build()
        return Apis(url, r.create(BeaconFixApi::class.java), r.create(HubApi::class.java), r.create(HubNodeApi::class.java)).also { apis = it }
    }
    /** The v1 API, carried over BFS3 to the hub; null when not enrolled. Pass [AUTH] where a route takes the bearer. */
    fun api(): BeaconFixApi? = apis()?.api
    fun hubApi(): HubApi? = apis()?.hub
    fun nodeApi(): HubNodeApi? = apis()?.node

    // ── enrolment ───────────────────────────────────────────────────────────
    @Serializable private data class EnrollBody(val inviteId: String, val name: String, val kind: String, val pub: String, val ts: Long, val mac: String)
    @Serializable private data class EnrollAnswer(val deviceId: String = "", val fingerprint: String = "", val proof: String = "")

    /**
     * Enrol with [invite] at [url] under [name] (docs/SECURE-API.md "Enrolment"): a new X25519 key pair is generated
     * here, the MAC proves we hold the invite, and the hub's answer must carry our device id, the invite's
     * fingerprint and a proof only the holder of the invite's server key can compute. Nothing is stored unless all
     * three check out. Throws with a user-readable message.
     */
    suspend fun enroll(invite: Bfs3.Invite, url: String, name: String): HubConfig = withContext(Dispatchers.IO) {
        val base = HubUrls.normalise(url)
        val dsk = Bfs3.newPrivateKey(); val dpk = Bfs3.publicKey(dsk)
        val pub = Bfs3.b64u(dpk)
        val nm = name.trim().take(64).ifEmpty { "Android phone" }
        var code: Int; var text: String; var attempt = 0
        while (true) {
            val ts = nowS()
            val body = EnrollBody(invite.inviteId, nm, KIND, pub, ts, Bfs3.enrollMac(invite.secret, invite.inviteId, nm, KIND, pub, ts))
            val req = Request.Builder().url(base.toHttpUrl().resolve("api/v3/enroll")!!)
                .post(ApiFactory.json.encodeToString(EnrollBody.serializer(), body).toRequestBody("application/json".toMediaType())).build()
            val skewBefore = clockSkewS
            val got = try { plain.newCall(req).execute().use { r -> wire.learnClock(r); r.code to (r.body?.string() ?: "") } }
                      catch (e: IOException) { throw IOException(HubErrors.describe(e), e) }
            code = got.first; text = got.second
            // refused while our clock is off: the invite is still unused, so try once more in the hub's time
            if (code == 403 && attempt++ == 0 && clockSkewS != skewBefore && kotlin.math.abs(clockSkewS) > 60) continue
            break
        }
        if (code == 403) throw IOException("enrolment refused — the invite was already used or expired, or this phone's clock is off" + if (kotlin.math.abs(clockSkewS) > Bfs3.TIME_WINDOW_S) " (by ${kotlin.math.abs(clockSkewS) / 60} min)" else "")
        if (code !in 200..299) throw IOException("enrolment failed: HTTP $code")
        val a = runCatching { ApiFactory.json.decodeFromString(EnrollAnswer.serializer(), text) }.getOrNull() ?: throw IOException("enrolment failed: unreadable answer")
        val id = Bfs3.deviceId(dpk)
        if (a.deviceId != id) throw HubSecurityException("the hub named another device id (${a.deviceId.take(32)})")
        if (!Bfs3.constantTimeEquals(a.fingerprint.lowercase(), invite.fingerprint)) throw HubSecurityException("the hub's fingerprint is not the invite's — not enrolled")
        val rk = Bfs3.rootKeyOf(dsk, invite.serverPub)
        val ok = Bfs3.checkProof(rk, id, a.proof)
        rk.fill(0)
        if (!ok) throw HubSecurityException("the hub could not prove it holds the invite's key — not enrolled")
        val cfg = HubConfig(base, nm, id, invite.serverPub, invite.fingerprint, System.currentTimeMillis())
        reset()
        store.save(cfg, dsk)
        dsk.fill(0)
        cfg
    }

    // ── event stream (GET /api/v3/stream) ───────────────────────────────────
    /**
     * Open the hub's sealed event stream. Each `data:` line is one event sealed under this request's counter and its
     * sequence number (1, 2, …); [onEvent] gets the decoded JSON. A line that does not open closes the stream (a
     * dropped or forged event would desynchronise the sequence) — the caller reopens with a new counter.
     */
    fun openStream(onEvent: (JsonObject) -> Unit, onOpen: () -> Unit, onClosed: (String?) -> Unit): EventSource? {
        val emit = onEvent; val opened = onOpen; val closed = onClosed
        val s = session() ?: return null
        val url = HubUrls.v3(s.cfg.url.toHttpUrl().resolve("api/v1/stream")!!)
        val sealed = wire.seal(s, "GET", url, ByteArray(0))
        val req = wire.headers(Request.Builder().url(url), s, sealed).header(Bfs3.H_SEALED, Bfs3.b64u(sealed.bytes)).header("Accept", "text/event-stream").get().build()
        val client = plain.newBuilder().readTimeout(0, TimeUnit.MILLISECONDS).build()
        var seq = 0L
        return EventSources.createFactory(client).newEventSource(req, object : EventSourceListener() {
            override fun onOpen(eventSource: EventSource, response: Response) {
                wire.learnClock(response)
                // the hub names the counter its events are sealed under: it must be this request's
                if (response.header(Bfs3.H_COUNTER)?.trim()?.toLongOrNull()?.let { it != sealed.counter } == true) { eventSource.cancel(); closed("stream for another request"); return }
                opened()
            }
            override fun onEvent(eventSource: EventSource, id: String?, type: String?, data: String) {
                val pt = try { Bfs3.openEvent(s.rk, s.cfg.deviceId, sealed.counter, seq + 1, data) } catch (e: Bfs3Exception) {
                    if (type == "ping") return                     // an unsealed keep-alive carries nothing
                    eventSource.cancel(); closed("stream event failed authentication"); return
                }
                seq++
                val o = runCatching { ApiFactory.json.parseToJsonElement(String(pt, Charsets.UTF_8)).jsonObject }.getOrNull() ?: return
                // the hub repeats the sequence number inside: {"type", "seq", "data"}
                if (o["seq"]?.let { runCatching { it.jsonPrimitive.content.toDouble().toLong() }.getOrNull() }?.let { it != seq } == true) { eventSource.cancel(); closed("stream out of sequence"); return }
                emit(o)
            }
            override fun onClosed(eventSource: EventSource) = closed(null)
            override fun onFailure(eventSource: EventSource, t: Throwable?, response: Response?) {
                response?.let { wire.learnClock(it) }
                closed(t?.let { HubErrors.describe(it) } ?: response?.let { HubErrors.ofStatus(it.code, clockSkewS) })
            }
        })
    }

    companion object {
        const val KIND = "android"
        /** What to pass where a [BeaconFixApi] route takes the bearer: the interceptor drops it. */
        const val AUTH = "BFS3"
        /** Set on an error response the hub did not seal (its body is untrusted). */
        const val UNSEALED = "X-BF-Unsealed"
    }
}

/** One enrolment's live state: the root key (derived once, in memory only) and the counter allocator. */
class Bfs3Session(val cfg: HubConfig, internal val rk: ByteArray, val counters: CounterAllocator)

/** The request/response half of BFS3 on OkHttp objects, plus the clock the requests are stamped with. */
class Bfs3Wire(private val clock: () -> Long = System::currentTimeMillis) {
    /** Learned from the hub's `X-BF-Time` (else `Date`) header when this phone's clock is off by more than 30 s (requests then carry hub time). */
    @Volatile var clockSkewS = 0L; private set
    fun nowS(): Long = clock() / 1000 + clockSkewS

    class Sealed(val counter: Long, val ts: Long, val nonce: ByteArray, val bytes: ByteArray)
    companion object { const val UNAUTH_SKIP = 256L }
    fun seal(s: Bfs3Session, method: String, url: HttpUrl, plaintext: ByteArray): Sealed {
        val c = s.counters.next()            // reserved on disk before it is used
        val ts = nowS(); val nonce = Bfs3.random(12)
        return Sealed(c, ts, nonce, Bfs3.sealRequest(s.rk, s.cfg.deviceId, method, HubUrls.target(url), c, ts, nonce, plaintext))
    }
    fun headers(b: Request.Builder, s: Bfs3Session, sealed: Sealed): Request.Builder = b
        .removeHeader("Authorization")
        .header(Bfs3.H_DEVICE, s.cfg.deviceId).header(Bfs3.H_COUNTER, sealed.counter.toString()).header(Bfs3.H_TIME, sealed.ts.toString())
        .header(Bfs3.H_NONCE, Bfs3.b64u(sealed.nonce))

    fun learnClock(r: Response) {
        val date = r.header(Bfs3.H_SERVER_TIME)?.trim()?.toLongOrNull()?.let { it * 1000 } ?: r.headers.getDate("Date")?.time ?: return
        val skew = (date - clock()) / 1000
        clockSkewS = if (kotlin.math.abs(skew) > 30) skew else 0L
    }

    /** Open the hub's answer to request [c]: sealed → plaintext JSON; unsealed → only an error passes (marked, untrusted). */
    fun open(s: Bfs3Session, c: Long, r: Response): Response {
        val raw = r.body?.use { it.bytes() } ?: ByteArray(0)
        val nonce = r.header(Bfs3.H_NONCE)
        if (nonce == null && r.header(Bfs3.H_COUNTER) == null) {      // unauthenticated (docs/SECURE-API.md): trust only the status
            if (r.isSuccessful) throw HubSecurityException("an unsealed ${r.code} answer")
            if (r.code == 401) runCatching { s.counters.skip(UNAUTH_SKIP) }   // in case the hub saw counters we lost track of
            return r.newBuilder().header(HubClient.UNSEALED, "1").body(raw.toResponseBody("application/json".toMediaType())).build()
        }
        val pt = try { Bfs3.openResponseFor(s.rk, s.cfg.deviceId, c, r.code, r.header(Bfs3.H_COUNTER), nonce, raw) }
                 catch (e: Bfs3Exception) { throw HubSecurityException(e.message ?: "authentication failed", e) }
        return r.newBuilder().removeHeader("Content-Length").removeHeader("Content-Type").header("Content-Type", "application/json")
            .body(pt.toResponseBody("application/json".toMediaType())).build()
    }
}

/**
 * Seals every request to the hub and opens every answer. An application interceptor: it runs once per call, never
 * per retry, so a counter is never sent twice. A request with an empty plaintext (GET, DELETE, a bodiless POST) sends
 * no body; its sealed bytes (the tag) go in [Bfs3.H_SEALED].
 */
class Bfs3Interceptor(private val session: () -> Bfs3Session?, private val wire: Bfs3Wire) : Interceptor {
    override fun intercept(chain: Interceptor.Chain): Response {
        val req = chain.request()
        if (!req.isHttps) throw HubSecurityException("the hub is only reached over https")
        val s = session() ?: throw IOException("not enrolled with a hub")
        val url = HubUrls.v3(req.url)
        val plaintext = req.body?.let { b -> Buffer().also { b.writeTo(it) }.readByteArray() } ?: ByteArray(0)
        val sealed = wire.seal(s, req.method, url, plaintext)
        val b = wire.headers(req.newBuilder().url(url), s, sealed)
        when {
            plaintext.isNotEmpty() -> b.method(req.method, sealed.bytes.toRequestBody(Bfs3.SEALED_TYPE.toMediaType()))
            // an empty plaintext sends no body: the sealed bytes (the tag) go in X-BF-Seal (spec rev. 2)
            req.method == "GET" || req.method == "HEAD" || req.method == "DELETE" -> b.method(req.method, null).header(Bfs3.H_SEALED, Bfs3.b64u(sealed.bytes))
            else -> b.method(req.method, ByteArray(0).toRequestBody(null)).header(Bfs3.H_SEALED, Bfs3.b64u(sealed.bytes))   // POST/PUT need a (zero-length) body
        }
        val r = chain.proceed(b.build())
        wire.learnClock(r)
        return wire.open(s, sealed.counter, r)
    }
}
