// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import android.content.Context
import android.util.Log
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.sworrl.beaconfix.data.api.FlockCameraDto
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Persistent offline disk and memory cache for known ALPR & Flock cameras.
 * Ensures the phone retains cameras when disconnected from the desktop LAN or traveling on cellular.
 */
@Singleton
class FlockCameraCache @Inject constructor(
    @ApplicationContext private val context: Context
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val mutex = Mutex()
    private val cacheFile by lazy { File(context.filesDir, "flock_cameras_cache.json") }
    private val json = Json { ignoreUnknownKeys = true; isLenient = true; encodeDefaults = true }

    @Volatile
    private var inMemory: Map<String, FlockCameraDto> = emptyMap()

    init {
        scope.launch {
            loadFromDisk()
        }
    }

    suspend fun getCameras(): List<FlockCameraDto> {
        return inMemory.values.toList()
    }

    suspend fun getPassCameras(): List<PassCamera> {
        val list = inMemory.values.toList()
        return withContext(Dispatchers.Default) {
            list.filter { !it.stale }.map { PlateEvents.camera(it) }
        }
    }

    suspend fun updateCameras(cameras: List<FlockCameraDto>) {
        if (cameras.isEmpty()) return
        mutex.withLock {
            val updated = inMemory.toMutableMap()
            var changed = false
            for (c in cameras) {
                if (c.id.isNotBlank()) {
                    val prev = updated[c.id]
                    if (prev == null || prev != c) {
                        updated[c.id] = c
                        changed = true
                    }
                }
            }
            if (changed) {
                inMemory = updated
                saveToDisk(updated.values.toList())
            }
        }
    }

    private suspend fun loadFromDisk() = mutex.withLock {
        withContext(Dispatchers.IO) {
            try {
                if (cacheFile.exists()) {
                    val text = cacheFile.readText(Charsets.UTF_8)
                    if (text.isNotBlank()) {
                        val list: List<FlockCameraDto> = json.decodeFromString(text)
                        inMemory = list.associateBy { it.id }
                        Log.i(TAG, "Loaded ${list.size} offline cameras from cache")
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "Error loading camera cache", e)
            }
        }
    }

    private suspend fun saveToDisk(cameras: List<FlockCameraDto>) = withContext(Dispatchers.IO) {
        try {
            val text = json.encodeToString(cameras)
            val tmp = File(context.filesDir, "flock_cameras_cache.json.tmp")
            tmp.writeText(text, Charsets.UTF_8)
            tmp.renameTo(cacheFile)
            Log.d(TAG, "Saved ${cameras.size} cameras to offline cache")
        } catch (e: Exception) {
            Log.w(TAG, "Error saving camera cache", e)
        }
    }

    companion object {
        private const val TAG = "FlockCameraCache"
    }
}
