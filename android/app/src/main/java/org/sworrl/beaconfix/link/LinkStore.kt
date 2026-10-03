package org.sworrl.beaconfix.link

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.serialization.Serializable
import org.sworrl.beaconfix.data.api.ApiFactory
import javax.inject.Inject
import javax.inject.Singleton

/** What the link told us about a PC beyond the desktop row: every address it has, its identity id, the code both screens showed. */
@Serializable
data class LinkedMeta(val desktopId: String, val name: String = "", val pcId: String = "", val hostname: String = "", val hosts: List<String> = emptyList(),
                      val port: Int = 0, val code: String = "", val linkedAt: Long = 0)

/** A hub invite a link delivered that could not be used yet (hub unreachable): kept, retried, renewed from [fromDesktop]. */
@Serializable
data class PendingHub(val invite: String, val fromDesktop: String = "", val pcName: String = "", val since: Long = 0)

/**
 * Link state on this phone, in the same Keystore-wrapped EncryptedSharedPreferences scheme as the desktop tokens (a
 * pending hub invite is a secret: whoever holds it can enrol). Never backed up (allowBackup=false).
 */
@Singleton
class LinkStore @Inject constructor(@ApplicationContext private val ctx: Context) {
    private val prefs: SharedPreferences by lazy {
        val key = MasterKey.Builder(ctx).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build()
        EncryptedSharedPreferences.create(ctx, FILE, key,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV, EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM)
    }
    private val json = ApiFactory.json
    private val _meta = MutableStateFlow<Map<String, LinkedMeta>>(emptyMap())
    /** Linked PCs' metadata by desktop id (loaded on first use). */
    val meta: StateFlow<Map<String, LinkedMeta>> get() { ensureLoaded(); return _meta }
    @Volatile private var loaded = false

    @Synchronized private fun ensureLoaded() {
        if (loaded) return
        loaded = true
        _meta.value = prefs.all.filterKeys { it.startsWith(K_META) }.values.mapNotNull { v -> (v as? String)?.let { runCatching { json.decodeFromString(LinkedMeta.serializer(), it) }.getOrNull() } }
            .associateBy { it.desktopId }
    }
    fun meta(desktopId: String): LinkedMeta? { ensureLoaded(); return _meta.value[desktopId] }
    @Synchronized fun saveMeta(m: LinkedMeta) {
        ensureLoaded()
        prefs.edit().putString(K_META + m.desktopId, json.encodeToString(LinkedMeta.serializer(), m)).apply()
        _meta.value = _meta.value + (m.desktopId to m)
    }
    @Synchronized fun removeMeta(desktopId: String) {
        ensureLoaded()
        prefs.edit().remove(K_META + desktopId).apply()
        _meta.value = _meta.value - desktopId
    }

    fun pending(): PendingHub? = prefs.getString(K_PENDING, null)?.let { runCatching { json.decodeFromString(PendingHub.serializer(), it) }.getOrNull() }
    fun setPending(p: PendingHub) { prefs.edit().putString(K_PENDING, json.encodeToString(PendingHub.serializer(), p)).commit() }
    fun clearPending() { prefs.edit().remove(K_PENDING).commit() }

    companion object {
        const val FILE = "beaconfix_link"
        private const val K_META = "pc:"
        private const val K_PENDING = "pendingHub"
    }
}
