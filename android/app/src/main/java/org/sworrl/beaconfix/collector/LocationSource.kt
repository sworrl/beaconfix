package org.sworrl.beaconfix.collector

import android.annotation.SuppressLint
import android.content.Context
import android.location.Location
import android.os.Looper
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.tasks.await
import kotlinx.coroutines.withTimeoutOrNull
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class LocationSource @Inject constructor(@ApplicationContext private val ctx: Context) {
    private val fused by lazy { LocationServices.getFusedLocationProviderClient(ctx) }

    @SuppressLint("MissingPermission")
    suspend fun current(timeoutMs: Long = 15_000): Location? = withTimeoutOrNull(timeoutMs) {
        runCatching { fused.getCurrentLocation(Priority.PRIORITY_HIGH_ACCURACY, null).await() }.getOrNull()
            ?: runCatching { fused.lastLocation.await() }.getOrNull()
    }

    @SuppressLint("MissingPermission")
    fun updates(intervalMs: Long): Flow<Location> = callbackFlow {
        val req = LocationRequest.Builder(Priority.PRIORITY_HIGH_ACCURACY, intervalMs).setMinUpdateDistanceMeters(2f).build()
        val cb = object : LocationCallback() { override fun onLocationResult(r: LocationResult) { r.lastLocation?.let { trySend(it) } } }
        fused.requestLocationUpdates(req, cb, Looper.getMainLooper())
        awaitClose { fused.removeLocationUpdates(cb) }
    }
}
