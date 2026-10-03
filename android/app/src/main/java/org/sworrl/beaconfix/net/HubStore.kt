package org.sworrl.beaconfix.net

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import java.io.IOException
import javax.inject.Inject
import javax.inject.Singleton

/** The hub this phone is enrolled with: where it is, who we are to it, and the server key pinned at enrolment. */
data class HubConfig(val url: String, val name: String, val deviceId: String, val serverPub: ByteArray, val fingerprint: String, val enrolledAt: Long) {
    override fun equals(other: Any?) = other is HubConfig && other.url == url && other.name == name && other.deviceId == deviceId && other.fingerprint == fingerprint && other.enrolledAt == enrolledAt
    override fun hashCode() = deviceId.hashCode() * 31 + url.hashCode()
}

/** What the Hub screen shows. [reachable] is null until the hub was tried; [lastError] is the newest failure (sync, live or publish). */
data class HubStatus(val config: HubConfig? = null, val lastSyncAt: Long = 0, val lastSyncText: String = "", val lastError: String = "", val lastErrorAt: Long = 0,
                     val reachable: Boolean? = null, val lastPublishAt: Long = 0, val lastContactAt: Long = 0, val loaded: Boolean = false)

/** URL rules for the hub: https only (the hub is behind TLS and WireGuard; cleartext is never used), base ends in `/`. */
object HubUrls {
    val DEFAULT: String = org.sworrl.beaconfix.BuildConfig.HUB_URL   // build property beaconfixHubUrl (app/build.gradle.kts)
    /** "beacon.example.com" → "https://beacon.example.com/"; http:// and anything unparsable are refused. */
    fun normalise(input: String): String {
        val t = input.trim()
        if (t.isEmpty()) throw IllegalArgumentException("enter the hub's https:// address")
        if (t.startsWith("http://", ignoreCase = true)) throw IllegalArgumentException("the hub is only reached over https://, never plain http")
        val withScheme = if (t.contains("://")) t else "https://$t"
        val u = withScheme.toHttpUrlOrNull() ?: throw IllegalArgumentException("not a valid address: $t")
        if (!u.isHttps) throw IllegalArgumentException("the hub is only reached over https://")
        if (u.query != null || u.fragment != null) throw IllegalArgumentException("the hub address has no ?query or #fragment")
        val s = u.newBuilder().build().toString()
        return if (s.endsWith("/")) s else "$s/"
    }
    /** `…/api/v1/<route>` → `…/api/v3/<route>` (the v1 route carried inside BFS3); other paths stay as they are. */
    fun v3(url: HttpUrl): HttpUrl {
        val p = url.encodedPath
        val i = p.indexOf("/api/v1/")
        if (i < 0) return url
        return url.newBuilder().encodedPath(p.substring(0, i) + "/api/v3/" + p.substring(i + "/api/v1/".length)).build()
    }
    /** The request target T as on the wire: path + query exactly as encoded. */
    fun target(url: HttpUrl): String = url.encodedPath + (url.encodedQuery?.let { "?$it" } ?: "")
}

/**
 * Hub enrolment on this phone. Everything lives in Keystore-wrapped EncryptedSharedPreferences (AES-256-GCM values,
 * the master key in the Android Keystore — the same scheme as the desktop tokens and the identity seed): the X25519
 * device private key (generated here, never exported; the app allows no backup or device transfer of its prefs), the
 * device id, the pinned server public key + fingerprint, the hub URL, the request-counter reservation and the sync
 * cursors. Status (last sync / error / publish) is kept alongside so the screen survives restarts.
 */
@Singleton
class HubStore @Inject constructor(@ApplicationContext private val ctx: Context) : CounterAllocator.Store {
    private val prefs: SharedPreferences by lazy {
        val key = MasterKey.Builder(ctx).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build()
        EncryptedSharedPreferences.create(ctx, FILE, key,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV, EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM)
    }
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val _status = MutableStateFlow(HubStatus())
    val status: StateFlow<HubStatus> = _status
    /** The config as last loaded — cheap to read from any thread (the location callback asks it on every fix). */
    @Volatile var cached: HubConfig? = null; private set
    @Volatile private var loaded = false

    init { scope.launch { runCatching { load() } } }

    @Synchronized private fun load() {
        val cfg = readConfig()
        cached = cfg; loaded = true
        _status.value = HubStatus(cfg, prefs.getLong(K_SYNC_AT, 0), prefs.getString(K_SYNC_TEXT, "") ?: "", prefs.getString(K_ERR, "") ?: "", prefs.getLong(K_ERR_AT, 0),
            null, prefs.getLong(K_PUB_AT, 0), prefs.getLong(K_CONTACT_AT, 0), loaded = true)
    }
    private fun readConfig(): HubConfig? {
        val id = prefs.getString(K_DEVICE, null) ?: return null
        val spk = prefs.getString(K_SPK, null)?.let { runCatching { Bfs3.unb64u(it) }.getOrNull() } ?: return null
        if (prefs.getString(K_DSK, null) == null) return null
        return HubConfig(prefs.getString(K_URL, HubUrls.DEFAULT) ?: HubUrls.DEFAULT, prefs.getString(K_NAME, "") ?: "", id, spk, prefs.getString(K_FP, "") ?: "", prefs.getLong(K_ENROLLED, 0))
    }
    fun config(): HubConfig? = if (loaded) cached else { load(); cached }
    /** The device private key — only the BFS3 client asks for it, to derive the root key. */
    internal fun deviceKey(): ByteArray? = prefs.getString(K_DSK, null)?.let { Bfs3.unb64u(it) }

    /** A fresh enrolment: new keys, counter from 1, cursors from the start. Written synchronously. */
    @Synchronized fun save(cfg: HubConfig, deviceSk: ByteArray) {
        val ok = prefs.edit().clear()
            .putString(K_URL, cfg.url).putString(K_NAME, cfg.name).putString(K_DEVICE, cfg.deviceId).putString(K_SPK, Bfs3.b64u(cfg.serverPub))
            .putString(K_FP, cfg.fingerprint).putLong(K_ENROLLED, cfg.enrolledAt).putString(K_DSK, Bfs3.b64u(deviceSk)).putLong(K_COUNTER, 0)
            .commit()
        if (!ok) throw IOException("could not store the hub enrolment")
        load()
    }
    @Synchronized fun setUrl(url: String) {
        val u = HubUrls.normalise(url)
        if (!prefs.edit().putString(K_URL, u).commit()) throw IOException("could not store the hub address")
        load()
    }
    @Synchronized fun forget() { prefs.edit().clear().commit(); cached = null; loaded = true; _status.value = HubStatus(loaded = true) }

    // ── counter reservation (CounterAllocator.Store): commit() returns once the value is on disk ──
    override fun reserved(): Long = prefs.getLong(K_COUNTER, 0)
    override fun reserve(upTo: Long) { if (!prefs.edit().putLong(K_COUNTER, upTo).commit()) throw IOException("could not reserve request counters") }

    // ── cursors ─────────────────────────────────────────────────────────────
    /** The hub's /db/changes cursor we have pulled up to ("0" = everything). */
    fun pullCursor(): String = prefs.getString(K_CURSOR, "0") ?: "0"
    fun setPullCursor(c: String) { prefs.edit().putString(K_CURSOR, c).apply() }
    /** The newest phone fix (time, ms) the hub has accepted. */
    fun fixCursor(): Long = prefs.getLong(K_FIX_CURSOR, 0)
    fun setFixCursor(t: Long) { prefs.edit().putLong(K_FIX_CURSOR, t).apply() }
    /**
     * The hub feed position plate events have been pulled up to (null = never: a phone enrolled before plate events
     * went over the hub catches up from 0). It follows [pullCursor] while it is not behind it (sync.HubPlates).
     */
    fun plateCursor(): Long? = if (prefs.contains(K_PLATE_CURSOR)) prefs.getLong(K_PLATE_CURSOR, 0) else null
    fun setPlateCursor(c: Long) { prefs.edit().putLong(K_PLATE_CURSOR, c).apply() }

    // ── status ──────────────────────────────────────────────────────────────
    fun noteSync(text: String) {
        val now = System.currentTimeMillis()
        prefs.edit().putLong(K_SYNC_AT, now).putString(K_SYNC_TEXT, text).putString(K_ERR, "").putLong(K_CONTACT_AT, now).apply()
        _status.update { it.copy(lastSyncAt = now, lastSyncText = text, lastError = "", reachable = true, lastContactAt = now) }
    }
    fun noteContact() {
        val now = System.currentTimeMillis()
        prefs.edit().putLong(K_CONTACT_AT, now).apply()
        _status.update { it.copy(reachable = true, lastContactAt = now, lastError = if (it.lastError == HubErrors.UNREACHABLE) "" else it.lastError) }
    }
    fun notePublish() {
        val now = System.currentTimeMillis()
        prefs.edit().putLong(K_PUB_AT, now).putLong(K_CONTACT_AT, now).apply()
        _status.update { it.copy(lastPublishAt = now, reachable = true, lastContactAt = now, lastError = if (it.lastError == HubErrors.UNREACHABLE) "" else it.lastError) }
    }
    fun noteError(msg: String, unreachable: Boolean) {
        val now = System.currentTimeMillis()
        prefs.edit().putString(K_ERR, msg).putLong(K_ERR_AT, now).apply()
        _status.update { it.copy(lastError = msg, lastErrorAt = now, reachable = if (unreachable) false else it.reachable) }
    }

    companion object {
        /** Never backed up or transferred (allowBackup=false; data_extraction_rules excludes all shared prefs): a restored copy would reuse counters. */
        const val FILE = "beaconfix_hub"
        private const val K_URL = "url"; private const val K_NAME = "name"; private const val K_DEVICE = "deviceId"; private const val K_SPK = "serverPub"
        private const val K_FP = "fingerprint"; private const val K_ENROLLED = "enrolledAt"; private const val K_DSK = "deviceSk"; private const val K_COUNTER = "counterReserved"
        private const val K_CURSOR = "pullCursor"; private const val K_FIX_CURSOR = "fixCursor"; private const val K_PLATE_CURSOR = "plateCursor"
        private const val K_SYNC_AT = "lastSyncAt"; private const val K_SYNC_TEXT = "lastSyncText"; private const val K_ERR = "lastError"; private const val K_ERR_AT = "lastErrorAt"
        private const val K_PUB_AT = "lastPublishAt"; private const val K_CONTACT_AT = "lastContactAt"
    }
}
