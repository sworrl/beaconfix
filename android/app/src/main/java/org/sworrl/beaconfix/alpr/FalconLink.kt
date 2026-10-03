package org.sworrl.beaconfix.alpr

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import android.util.Log
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.MultipartBody
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import org.sworrl.beaconfix.BuildConfig
import org.sworrl.beaconfix.alpr.core.AlprJson
import org.sworrl.beaconfix.alpr.core.FrameMeta
import org.sworrl.beaconfix.alpr.core.FramesReply
import org.sworrl.beaconfix.alpr.core.HelloBody
import org.sworrl.beaconfix.alpr.core.HelloReply
import org.sworrl.beaconfix.alpr.core.HitBody
import org.sworrl.beaconfix.alpr.core.Hotlist
import org.sworrl.beaconfix.alpr.core.Pairing
import org.sworrl.beaconfix.alpr.core.PendingEvent
import java.io.IOException
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton

/** What this phone knows about its FalconEyez server. */
data class FalconPairing(
    val urls: List<String> = emptyList(),
    val serverName: String = "",
    val deviceId: String = "",
    val cameraId: String = "",
    val cameraName: String = "",
    /** The URL that answered last; tried first. */
    val preferred: String = "",
    /** The server answered 401: the token was revoked, re-pair. */
    val revoked: Boolean = false,
    val pairedAtMs: Long = 0,
) {
    val paired get() = urls.isNotEmpty()
}

/** The outcome of one HTTP exchange with FalconEyez. */
sealed interface Call<out T> {
    data class Ok<T>(val value: T, val url: String) : Call<T>
    /** 429 / 503: come back after [retryAfterS]. */
    data class Busy(val retryAfterS: Int) : Call<Nothing>
    /** 401: token revoked. */
    data object Revoked : Call<Nothing>
    /** Any other HTTP status (the server is reachable). */
    data class Failed(val code: Int, val message: String) : Call<Nothing>
    /** No URL answered. */
    data class Unreachable(val message: String) : Call<Nothing>
}

/**
 * Pairing with FalconEyez and its mobile API (`/api/mobile/…`, Bearer token). The URL list and token are kept in
 * EncryptedSharedPreferences (AES-256-GCM, Keystore key), like the desktops' tokens. URLs are tried in order, the one
 * that answered last first; a LAN address times out in 3 s so a remote one (Tailscale / WireGuard / IPv6) is tried next.
 */
@Singleton
class FalconLink @Inject constructor(@ApplicationContext private val ctx: Context, private val status: AlprStatus) {
    private val sp: SharedPreferences by lazy {
        runCatching {
            val key = MasterKey.Builder(ctx).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build()
            EncryptedSharedPreferences.create(ctx, "alpr_falcon", key,
                EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV, EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM)
        }.getOrElse { Log.w(TAG, "encrypted prefs unavailable", it); ctx.getSharedPreferences("alpr_falcon_fallback", Context.MODE_PRIVATE) }
    }
    private val state = MutableStateFlow(FalconPairing())
    val pairing: StateFlow<FalconPairing> get() { ensureLoaded(); return state.asStateFlow() }
    @Volatile private var loaded = false

    private fun ensureLoaded() {
        if (loaded) return
        synchronized(this) {
            if (loaded) return
            state.value = FalconPairing(
                urls = sp.getString("urls", "").orEmpty().split('\n').filter { it.isNotBlank() },
                serverName = sp.getString("server_name", "").orEmpty(), deviceId = sp.getString("device_id", "").orEmpty(),
                cameraId = sp.getString("camera_id", "").orEmpty(), cameraName = sp.getString("camera_name", "").orEmpty(),
                preferred = sp.getString("preferred", "").orEmpty(), revoked = sp.getBoolean("revoked", false), pairedAtMs = sp.getLong("paired_at", 0),
            )
            loaded = true
        }
    }

    private fun token() = sp.getString("token", "").orEmpty()

    private fun save(p: FalconPairing, token: String? = null) {
        sp.edit().putString("urls", p.urls.joinToString("\n")).putString("server_name", p.serverName).putString("device_id", p.deviceId)
            .putString("camera_id", p.cameraId).putString("camera_name", p.cameraName).putString("preferred", p.preferred)
            .putBoolean("revoked", p.revoked).putLong("paired_at", p.pairedAtMs)
            .apply { if (token != null) putString("token", token) }.apply()
        state.value = p
    }

    val client: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(3, TimeUnit.SECONDS).readTimeout(20, TimeUnit.SECONDS).writeTimeout(30, TimeUnit.SECONDS)
        .retryOnConnectionFailure(false).build()

    /** Pair: `POST {url}/api/mobile/hello` on each URL in order until one answers. Stores everything on success. */
    suspend fun pair(urls: List<String>, token: String, name: String): Call<HelloReply> = withContext(Dispatchers.IO) {
        val list = urls.mapNotNull { Pairing.normalizeUrl(it) }.distinct()
        if (list.isEmpty() || token.isBlank()) return@withContext Call.Failed(0, "a server URL and the pairing token are needed")
        val body = AlprJson.json.encodeToString(HelloBody.serializer(), HelloBody(deviceName(), "${Build.MANUFACTURER} ${Build.MODEL}", BuildConfig.VERSION_NAME))
            .toRequestBody(JSON)
        val r = exchange(list, token, { url -> Request.Builder().url("$url/api/mobile/hello").post(body) }) { resp ->
            AlprJson.json.decodeFromString(HelloReply.serializer(), resp.body?.string().orEmpty().ifBlank { "{}" })
        }
        if (r is Call.Ok) {
            ensureLoaded()
            save(FalconPairing(list, r.value.serverName.ifBlank { name }, r.value.deviceId, r.value.cameraId, r.value.cameraName,
                preferred = r.url, revoked = false, pairedAtMs = System.currentTimeMillis()), token.trim())
        }
        r
    }

    /** Adds a URL (a remote route: Tailscale / WireGuard / IPv6) after the existing ones. */
    fun addUrl(raw: String): Boolean {
        val u = Pairing.normalizeUrl(raw) ?: return false
        ensureLoaded(); val p = state.value
        if (u in p.urls) return true
        save(p.copy(urls = p.urls + u)); return true
    }

    fun removeUrl(u: String) { ensureLoaded(); val p = state.value; save(p.copy(urls = p.urls - u, preferred = if (p.preferred == u) "" else p.preferred)) }

    fun forget() { sp.edit().clear().apply(); state.value = FalconPairing() }

    private fun markRevoked() { ensureLoaded(); if (!state.value.revoked) save(state.value.copy(revoked = true)) }

    /** `POST /api/mobile/frames`: the crop + meta (no scene image, ever). */
    suspend fun uploadFrame(e: PendingEvent): Call<FramesReply> {
        val meta = AlprJson.json.encodeToString(FrameMeta.serializer(), e.meta)
        return authed({ url ->
            val body = MultipartBody.Builder().setType(MultipartBody.FORM)
                .addFormDataPart("image", "${e.id}.jpg", e.jpeg.toRequestBody(JPEG))
                .addFormDataPart("meta", null, meta.toRequestBody(JSON))
                .build()
            Request.Builder().url("$url/api/mobile/frames").post(body)
        }) { AlprJson.json.decodeFromString(FramesReply.serializer(), it.ifBlank { "{}" }) }
    }

    suspend fun hotlist(): Call<Hotlist> = authed({ url -> Request.Builder().url("$url/api/mobile/hotlist").get() }) {
        AlprJson.json.decodeFromString(Hotlist.serializer(), it)
    }

    suspend fun hit(b: HitBody): Call<Unit> {
        val body = AlprJson.json.encodeToString(HitBody.serializer(), b)
        return authed({ url -> Request.Builder().url("$url/api/mobile/hit").post(body.toRequestBody(JSON)) }) { }
    }

    // ---- plumbing ----

    private suspend fun <T> authed(build: (String) -> Request.Builder, parse: (String) -> T): Call<T> = withContext(Dispatchers.IO) {
        ensureLoaded(); val p = state.value
        if (!p.paired) return@withContext Call.Unreachable("not paired")
        if (p.revoked) return@withContext Call.Revoked
        val order = listOfNotNull(p.preferred.takeIf { it in p.urls }) + p.urls.filter { it != p.preferred }
        val r = exchange(order, token(), build) { resp -> parse(resp.body?.string().orEmpty()) }
        when (r) {
            is Call.Ok -> if (r.url != state.value.preferred) save(state.value.copy(preferred = r.url))
            is Call.Revoked -> markRevoked()
            else -> {}
        }
        status.set { it.copy(reachable = r !is Call.Unreachable, activeUrl = (r as? Call.Ok<*>)?.url ?: if (r is Call.Unreachable) "" else it.activeUrl) }
        r
    }

    private fun <T> exchange(urls: List<String>, token: String, build: (String) -> Request.Builder, reader: (Response) -> T): Call<T> {
        var last = "no URL"
        for (u in urls) {
            try {
                client.newCall(build(u).header("Authorization", "Bearer $token").header("User-Agent", "BeaconFix-ALPR/${BuildConfig.VERSION_NAME}").build()).execute().use { resp ->
                    return when {
                        resp.isSuccessful -> try { Call.Ok(reader(resp), u) } catch (e: Exception) { Call.Failed(resp.code, "bad reply: ${e.message}") }
                        resp.code == 401 -> Call.Revoked
                        resp.code == 429 || resp.code == 503 -> Call.Busy(resp.header("Retry-After")?.trim()?.toIntOrNull()?.coerceIn(1, 600) ?: 30)
                        else -> Call.Failed(resp.code, resp.message.ifBlank { "HTTP ${resp.code}" })
                    }
                }
            } catch (e: IOException) {
                last = "$u: ${e.javaClass.simpleName} ${e.message.orEmpty()}".trim()
            } catch (e: IllegalArgumentException) {
                last = "$u: ${e.message}"
            }
        }
        return Call.Unreachable(last)
    }

    private fun deviceName(): String =
        runCatching { android.provider.Settings.Global.getString(ctx.contentResolver, "device_name") }.getOrNull()?.takeIf { it.isNotBlank() } ?: Build.MODEL

    companion object {
        private const val TAG = "AlprLink"
        private val JSON = "application/json".toMediaType()
        private val JPEG = "image/jpeg".toMediaType()
    }
}

