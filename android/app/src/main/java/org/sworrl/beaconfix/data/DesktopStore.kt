package org.sworrl.beaconfix.data

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.BeaconFixApi
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.DesktopEntity
import javax.inject.Inject
import javax.inject.Singleton

/** Paired desktops: metadata in Room, bearer tokens in EncryptedSharedPreferences (AES-256-GCM, Keystore-backed key). */
@Singleton
class DesktopStore @Inject constructor(@ApplicationContext ctx: Context, private val db: AppDatabase) {
    private val prefs: SharedPreferences by lazy {
        val key = MasterKey.Builder(ctx).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build()
        EncryptedSharedPreferences.create(ctx, "beaconfix_tokens", key,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV, EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM)
    }

    fun all(): Flow<List<DesktopEntity>> = db.desktops().all()
    suspend fun paired(): List<DesktopEntity> = db.desktops().paired().filter { token(it.id) != null }
    suspend fun get(id: String) = db.desktops().get(id)
    suspend fun upsert(d: DesktopEntity) = db.desktops().upsert(d)
    suspend fun remove(id: String) { db.desktops().delete(id); prefs.edit().remove(id).apply() }

    fun token(id: String): String? = prefs.getString(id, null)
    fun saveToken(id: String, token: String) = prefs.edit().putString(id, token).apply()
    suspend fun forgetToken(id: String) {
        prefs.edit().remove(id).apply()
        db.desktops().get(id)?.let { db.desktops().upsert(it.copy(paired = false, scopes = "", lastError = "token rejected — pair again")) }
    }

    fun api(d: DesktopEntity): BeaconFixApi = ApiFactory.create(d.host, d.port, d.tls)
    fun auth(d: DesktopEntity): String? = token(d.id)?.let { ApiFactory.bearer(it) }
    fun hasScope(d: DesktopEntity, scope: String) = d.scopes.split(',').map { it.trim() }.contains(scope)
}
