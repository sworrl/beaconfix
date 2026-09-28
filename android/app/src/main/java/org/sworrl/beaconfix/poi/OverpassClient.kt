package org.sworrl.beaconfix.poi

import android.util.Log
import kotlinx.coroutines.delay
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import okhttp3.Call
import okhttp3.Callback
import okhttp3.FormBody
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import org.sworrl.beaconfix.BuildConfig
import java.io.IOException
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.resume

/** What one Overpass query came back with. [ok] = a usable answer ([elements] may be empty); otherwise [error] says why. */
data class OverpassResult(val ok: Boolean, val elements: List<OsmElement> = emptyList(), val error: String = "", val http: Int = 0, val mirror: String = "")

/** Where the phone's help search sends its queries (the real one is [OverpassClient]; tests use a fake). */
interface OverpassSource {
    /** POST [query] (server-side [timeoutS]); never throws except for cancellation. */
    suspend fun query(query: String, timeoutS: Int): OverpassResult
}

/**
 * Overpass API over its own OkHttp client (not the desktop API's: different hosts, longer waits). Mirrors are tried in
 * order, overpass-api.de then overpass.kumi.systems, [MIRROR_GAP_MS] apart. HTTP 429, any other non-2xx answer, an
 * empty body, a body that is not JSON, and a `remark` about a timeout / busy server / error all count as failures
 * (Overpass reports overload as HTTP 200 with a remark and no elements). The body is sent as `data=` form data, like
 * the desktop does. The client waits [WAIT_EXTRA_S] longer than the server's own `[timeout:N]`: hanging up first loses
 * the server's error remark and leaves it running the query in one of our slots.
 */
@Singleton
class OverpassClient internal constructor(
    private val http: OkHttpClient,
    private val userAgent: String,
    private val mirrors: List<String>,
    private val pause: suspend (Long) -> Unit = { delay(it) },
) : OverpassSource {
    @Inject constructor() : this(defaultClient(), USER_AGENT, MIRRORS)

    override suspend fun query(query: String, timeoutS: Int): OverpassResult =
        tryMirrors(mirrors, pause) { m -> post(m, query, timeoutS) }

    private suspend fun post(url: String, query: String, timeoutS: Int): OverpassResult {
        // the read timeout covers the server's own [timeout:N] plus queueing and the transfer
        val waitS = readTimeoutS(timeoutS).toLong()
        val client = if (waitS > READ_TIMEOUT_S) http.newBuilder().readTimeout(waitS, TimeUnit.SECONDS).build() else http
        val req = Request.Builder().url(url).header("User-Agent", userAgent).header("Accept", "application/json")
            .post(FormBody.Builder().add("data", query).build()).build()
        val call = client.newCall(req)
        return suspendCancellableCoroutine { cont ->
            cont.invokeOnCancellation { call.cancel() }
            call.enqueue(object : Callback {
                override fun onFailure(call: Call, e: IOException) {
                    if (cont.isActive) cont.resume(OverpassResult(false, error = e.message ?: e.javaClass.simpleName, mirror = host(url)))
                }
                override fun onResponse(call: Call, response: Response) {
                    val r = response.use { resp ->
                        val body = try { resp.body?.string() } catch (e: IOException) { null }
                        parse(resp.code, body).copy(mirror = host(url))
                    }
                    if (cont.isActive) cont.resume(r)
                }
            })
        }
    }

    companion object {
        const val TAG = "BfOverpass"
        const val READ_TIMEOUT_S = 30
        const val WAIT_EXTRA_S = 30
        const val MIRROR_GAP_MS = 5_000L

        /** How long to wait for an answer to a query with server-side `[timeout:timeoutS]` (the pediatric 90 s: 120 s). */
        fun readTimeoutS(timeoutS: Int) = maxOf(READ_TIMEOUT_S, timeoutS + WAIT_EXTRA_S)

        /** Each mirror in turn until one answers, [MIRROR_GAP_MS] between them (Overpass etiquette: not back to back). */
        internal suspend fun tryMirrors(mirrors: List<String>, pause: suspend (Long) -> Unit, post: suspend (String) -> OverpassResult): OverpassResult {
            var last = OverpassResult(false, error = "no Overpass mirror")
            for ((i, m) in mirrors.withIndex()) {
                if (i > 0) pause(MIRROR_GAP_MS)
                val r = post(m)
                if (r.ok) return r
                Log.w(TAG, "${host(m)}: ${r.error}")
                last = r
            }
            return last
        }
        val MIRRORS = listOf("https://overpass-api.de/api/interpreter", "https://overpass.kumi.systems/api/interpreter")
        val USER_AGENT = "BeaconFix-Android/${BuildConfig.VERSION_NAME} (+https://github.com/sworrl/beaconfix)"

        private val json = Json { ignoreUnknownKeys = true; isLenient = true }
        private val BAD_REMARK = Regex("""busy|timeout|timed out|error|out of memory|rate.?limit|too many""", RegexOption.IGNORE_CASE)

        fun defaultClient(): OkHttpClient = OkHttpClient.Builder()
            .connectTimeout(10, TimeUnit.SECONDS)
            .readTimeout(READ_TIMEOUT_S.toLong(), TimeUnit.SECONDS)
            .writeTimeout(15, TimeUnit.SECONDS)
            .retryOnConnectionFailure(true)
            .build()

        fun host(url: String) = url.substringAfter("://").substringBefore('/')

        /** Judge one answer: [http] status and [body] text → elements, or why it is unusable. */
        fun parse(http: Int, body: String?): OverpassResult {
            if (http == 429) return OverpassResult(false, error = "HTTP 429 (too many requests)", http = http)
            if (http !in 200..299) return OverpassResult(false, error = "HTTP $http", http = http)
            if (body.isNullOrBlank()) return OverpassResult(false, error = "empty answer", http = http)
            val root = runCatching { json.parseToJsonElement(body) }.getOrNull() as? JsonObject
                ?: return OverpassResult(false, error = "not a JSON answer", http = http)
            val remark = (root["remark"] as? JsonPrimitive)?.contentOrNull.orEmpty()
            if (remark.isNotBlank() && BAD_REMARK.containsMatchIn(remark)) return OverpassResult(false, error = "Overpass: ${remark.take(120)}", http = http)
            val arr = root["elements"] as? JsonArray ?: return OverpassResult(false, error = "no elements in the answer", http = http)
            return OverpassResult(true, arr.mapNotNull { OsmElement.fromOverpass(it) }, http = http)
        }
    }
}
