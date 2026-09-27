package org.sworrl.beaconfix.data

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.core.stringSetPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import javax.inject.Inject
import javax.inject.Singleton

private val Context.store: DataStore<Preferences> by preferencesDataStore("beaconfix")

@Singleton
class Prefs @Inject constructor(@ApplicationContext private val ctx: Context) {
    private object K {
        val collectorOn = booleanPreferencesKey("collector_on")
        val collectInterval = intPreferencesKey("collect_interval_s")
        val maxFixAcc = intPreferencesKey("max_fix_acc_m")
        val homePatterns = stringSetPreferencesKey("home_patterns")
        val autoSync = booleanPreferencesKey("auto_sync")
        val unmeteredOnly = booleanPreferencesKey("unmetered_only")
        val lastSyncReport = stringPreferencesKey("last_sync_report")
        val lastSyncAt = longPreferencesKey("last_sync_at")
        val throttleHintSeen = booleanPreferencesKey("throttle_hint_seen")
    }
    val collectorOn: Flow<Boolean> = ctx.store.data.map { it[K.collectorOn] ?: false }
    val collectIntervalSec: Flow<Int> = ctx.store.data.map { it[K.collectInterval] ?: 60 }
    val maxFixAccM: Flow<Int> = ctx.store.data.map { it[K.maxFixAcc] ?: 60 }
    val homePatterns: Flow<Set<String>> = ctx.store.data.map { it[K.homePatterns] ?: emptySet() }
    val autoSync: Flow<Boolean> = ctx.store.data.map { it[K.autoSync] ?: true }
    val unmeteredOnly: Flow<Boolean> = ctx.store.data.map { it[K.unmeteredOnly] ?: false }
    val lastSyncReport: Flow<String> = ctx.store.data.map { it[K.lastSyncReport] ?: "" }
    val lastSyncAt: Flow<Long> = ctx.store.data.map { it[K.lastSyncAt] ?: 0L }
    val throttleHintSeen: Flow<Boolean> = ctx.store.data.map { it[K.throttleHintSeen] ?: false }

    suspend fun setCollectorOn(v: Boolean) = ctx.store.edit { it[K.collectorOn] = v }
    suspend fun setCollectInterval(s: Int) = ctx.store.edit { it[K.collectInterval] = s.coerceIn(15, 900) }
    suspend fun setMaxFixAcc(m: Int) = ctx.store.edit { it[K.maxFixAcc] = m.coerceIn(10, 500) }
    suspend fun setHomePatterns(p: Set<String>) = ctx.store.edit { it[K.homePatterns] = p }
    suspend fun setAutoSync(v: Boolean) = ctx.store.edit { it[K.autoSync] = v }
    suspend fun setUnmeteredOnly(v: Boolean) = ctx.store.edit { it[K.unmeteredOnly] = v }
    suspend fun setLastSync(report: String) = ctx.store.edit { it[K.lastSyncReport] = report; it[K.lastSyncAt] = System.currentTimeMillis() }
    suspend fun setThrottleHintSeen() = ctx.store.edit { it[K.throttleHintSeen] = true }
}
