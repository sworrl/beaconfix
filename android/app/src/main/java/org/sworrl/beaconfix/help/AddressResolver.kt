package org.sworrl.beaconfix.help

import android.content.Context
import android.location.Address
import android.location.Geocoder
import android.os.Build
import android.util.Log
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.OkHttpClient
import okhttp3.Request
import org.sworrl.beaconfix.BuildConfig
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.DevFlags
import org.sworrl.beaconfix.estimate.Geo
import java.util.Locale
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.resume

/**
 * A street address for the dispatcher. Order: Android's Geocoder (asynchronous on API 33+; `subAdminArea` is the
 * county), then one Nominatim reverse lookup, then a saved `address` snapshot — but only one that was resolved within
 * [SAVED_NEAR_M] of the spot ([savedUsable]). A lookup happens only when the spot is more than [MOVED_M] from the last
 * resolved address; results are saved as snapshot kind `address`, source `phone`. With [resolve]'s `allowNetwork` false
 * (background refreshes) only a saved address is used. No address at all is better than the last campground's street
 * read out to 911: then the card says to read the coordinates.
 */
@Singleton
class AddressResolver @Inject constructor(@ApplicationContext private val ctx: Context, private val cache: DesktopCache) {
    private val http by lazy {
        OkHttpClient.Builder().connectTimeout(6, TimeUnit.SECONDS).readTimeout(8, TimeUnit.SECONDS).callTimeout(12, TimeUnit.SECONDS).build()
    }
    @Volatile private var lastNominatimAt = 0L

    suspend fun resolve(lat: Double, lon: Double, allowNetwork: Boolean): AddressLine? {
        if (!Origin.valid(lat, lon)) return null
        val mine = runCatching { cache.snapshotFrom(DesktopCache.PHONE, KIND) }.getOrNull()
        val mineLine = cache.decode<AddressLine>(mine)
        if (mine != null && mineLine != null && Geo.distanceM(lat, lon, mine.lat, mine.lon) <= MOVED_M) return mineLine.copy(fromCache = false)
        if (allowNetwork && !DevFlags.simOffline) {
            val found = geocoder(lat, lon) ?: nominatim(lat, lon)
            if (found != null && !found.isEmpty()) {
                runCatching { cache.putSnapshot(DesktopCache.PHONE, KIND, cache.encode(found.copy(fromCache = false)), lat, lon) }
                return found.copy(fromCache = false)
            }
        }
        // Offline (the usual case on arrival at a new site): a saved address only when it was resolved right here
        val newest = runCatching { cache.snapshotNow(KIND) }.getOrNull()
        return listOf(newest, mine).firstNotNullOfOrNull { s ->
            s?.takeIf { savedUsable(it.lat, it.lon, lat, lon) }?.let { cache.decode<AddressLine>(it) }?.takeUnless { it.isEmpty() }
        }?.copy(fromCache = true)
    }

    private suspend fun geocoder(lat: Double, lon: Double): AddressLine? {
        if (!Geocoder.isPresent()) return null
        val g = Geocoder(ctx, Locale.getDefault())
        val a: Address? = runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) withTimeoutOrNull(6000) {
                suspendCancellableCoroutine { cont ->
                    g.getFromLocation(lat, lon, 1, object : Geocoder.GeocodeListener {
                        override fun onGeocode(addresses: MutableList<Address>) { if (cont.isActive) cont.resume(addresses.firstOrNull()) }
                        override fun onError(errorMessage: String?) { if (cont.isActive) cont.resume(null) }
                    })
                }
            } else withContext(Dispatchers.IO) { @Suppress("DEPRECATION") g.getFromLocation(lat, lon, 1)?.firstOrNull() }
        }.onFailure { Log.i(TAG, "geocoder: ${it.message}") }.getOrNull()
        a ?: return null
        val street = listOfNotNull(a.subThoroughfare, a.thoroughfare).filter { it.isNotBlank() }.joinToString(" ")
        val line = street.ifEmpty { a.getAddressLine(0)?.substringBefore(',')?.trim().orEmpty() }
        return AddressLine(line = line, locality = a.locality ?: a.subLocality ?: "", county = a.subAdminArea ?: "", state = a.adminArea ?: "")
    }

    private suspend fun nominatim(lat: Double, lon: Double): AddressLine? {
        val now = System.currentTimeMillis()
        if (now - lastNominatimAt < NOMINATIM_GAP_MS) return null        // one request at a time, well under 1/s
        lastNominatimAt = now
        val url = String.format(Locale.US, "https://nominatim.openstreetmap.org/reverse?format=jsonv2&lat=%.6f&lon=%.6f&zoom=18&addressdetails=1", lat, lon)
        return withContext(Dispatchers.IO) {
            runCatching {
                val req = Request.Builder().url(url).header("User-Agent", USER_AGENT).header("Accept-Language", Locale.getDefault().toLanguageTag()).build()
                http.newCall(req).execute().use { r -> if (r.isSuccessful) parseNominatim(r.body?.string().orEmpty()) else null }
            }.onFailure { Log.i(TAG, "nominatim: ${it.message}") }.getOrNull()
        }
    }

    companion object {
        const val KIND = "address"
        const val MOVED_M = 200.0
        /** How far from where it was resolved a saved address may still be shown (marked "saved"). */
        const val SAVED_NEAR_M = 500.0

        /** A saved address resolved at ([savedLat], [savedLon]) may stand for ([lat], [lon]): known spot, within [SAVED_NEAR_M]. */
        fun savedUsable(savedLat: Double, savedLon: Double, lat: Double, lon: Double): Boolean =
            Origin.valid(savedLat, savedLon) && Geo.distanceM(savedLat, savedLon, lat, lon) <= SAVED_NEAR_M
        private const val NOMINATIM_GAP_MS = 60_000L
        private const val TAG = "BfHelp"
        val USER_AGENT = "BeaconFix-Android/${BuildConfig.VERSION_NAME} (+https://github.com/sworrl/beaconfix)"
        private val json = Json { ignoreUnknownKeys = true; isLenient = true }

        /** A Nominatim `reverse?format=jsonv2&addressdetails=1` body as an [AddressLine]; null when it has no address. */
        fun parseNominatim(body: String): AddressLine? {
            val root = runCatching { json.parseToJsonElement(body).jsonObject }.getOrNull() ?: return null
            val a = root["address"] as? JsonObject ?: return null
            fun s(vararg keys: String) = keys.firstNotNullOfOrNull { k -> runCatching { a[k]?.jsonPrimitive?.contentOrNull }.getOrNull()?.takeIf { it.isNotBlank() } } ?: ""
            val street = s("road", "pedestrian", "footway", "path", "residential", "track")
            val hn = s("house_number")
            val line = listOf(hn, street).filter { it.isNotEmpty() }.joinToString(" ")
            val out = AddressLine(line = line, locality = s("city", "town", "village", "hamlet", "municipality", "suburb"), county = s("county"), state = s("state"))
            return out.takeUnless { it.isEmpty() }
        }
    }
}

/** Nothing useful in it. */
fun AddressLine.isEmpty() = line.isBlank() && locality.isBlank() && county.isBlank() && state.isBlank()

/** "123 Main St, Town, County, ST" for sharing / reading out. */
fun AddressLine.oneLine(): String = listOf(line, locality, county, state).filter { it.isNotBlank() }.distinct().joinToString(", ")
