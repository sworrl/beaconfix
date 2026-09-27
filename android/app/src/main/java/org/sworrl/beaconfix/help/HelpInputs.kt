package org.sworrl.beaconfix.help

import android.content.Context
import android.telephony.TelephonyManager
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeoutOrNull
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.DesktopLive
import org.sworrl.beaconfix.data.DesktopView
import org.sworrl.beaconfix.data.DevFlags
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.api.LocationDto
import org.sworrl.beaconfix.data.api.TripDto
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.data.db.SnapshotEntity
import org.sworrl.beaconfix.poi.PhonePlaces
import org.sworrl.beaconfix.widget.WidgetUpdater
import java.util.Locale
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The places a live desktop view holds, as cache rows (source `desktop:<id>`, fetched when the view was). They are
 * merged with DesktopCache so a fresh answer shows even before it has been written to Room; nothing when DevFlags block
 * the desktop.
 */
object LivePlaces {
    fun rows(views: Collection<DesktopView>): List<PoiEntity> = if (DevFlags.desktopBlocked()) emptyList() else
        views.filter { it.fetched > 0 && it.pois.isNotEmpty() }.flatMap { v ->
            val l = v.location?.takeIf { it.valid }
            v.pois.map { DesktopCache.toEntity(DesktopCache.desktopSource(v.desktop.id), it, l?.lat ?: 0.0, l?.lon ?: 0.0, v.fetched) }
        }
}

/** Everything [HelpRepository] reads or asks for, behind one seam so the ranking and failure rules are JVM-testable. */
interface HelpInputs {
    fun now(): Long
    /** Ask every paired desktop for emergency, places and location (bounded to a few seconds); true when one answered. */
    suspend fun refreshDesktops(): Boolean
    /** The newest `emergency` snapshot from any desktop. */
    suspend fun emergency(): SnapshotEntity?
    /** The newest `hello` snapshot (its `features` say whether the desktop has "pediatric"). */
    suspend fun hello(): SnapshotEntity?
    /** The phone's own search note (`phoneplaces` snapshot: {origin, time, note}). */
    suspend fun phoneSearch(): SnapshotEntity?
    /** Every cached place (all desktops and the phone), deduped by key. */
    suspend fun places(): List<PoiEntity>
    suspend fun phoneFix(): FixEntity?
    /** The newest RV (desktop) position this phone knows. */
    suspend fun desktopFix(): FixEntity?
    suspend fun phonePlacesOn(): Boolean
    suspend fun pedsRadiusKm(): Int
    /** A country code when no desktop said one (trip locale, then the phone's network / SIM / locale). */
    suspend fun countryCode(): String
    suspend fun address(lat: Double, lon: Double, allowNetwork: Boolean): AddressLine?
    val phone: PhonePlaces
    fun touchWidgets()
}

/** The real inputs: Room (via DesktopCache and FixDao), the live desktop views, prefs, the phone search and the geocoder. */
@Singleton
class AndroidHelpInputs @Inject constructor(
    @ApplicationContext private val ctx: Context,
    private val live: DesktopLive,
    private val cache: DesktopCache,
    private val db: AppDatabase,
    private val prefs: Prefs,
    private val addresses: AddressResolver,
    override val phone: PhonePlaces,
    private val widgets: dagger.Lazy<WidgetUpdater>,
) : HelpInputs {
    override fun now() = System.currentTimeMillis()

    override suspend fun refreshDesktops(): Boolean {
        if (DevFlags.desktopBlocked()) return false
        val start = now()
        withTimeoutOrNull(6000) { live.refreshAll(setOf("emergency", "pois", "location")) }
        return live.views.value.values.any { it.error.isEmpty() && it.fetched >= start }
    }

    override suspend fun emergency() = cache.snapshotNow("emergency")
    override suspend fun hello() = cache.snapshotNow("hello")
    override suspend fun phoneSearch() = cache.snapshotNow("phoneplaces")

    /** The cache, plus what the live views hold that may not have been written to it yet. */
    override suspend fun places(): List<PoiEntity> = DesktopCache.dedupe(cache.poisNow() + LivePlaces.rows(live.views.value.values))

    override suspend fun phoneFix() = db.fixes().lastPhone()

    override suspend fun desktopFix(): FixEntity? {
        val cands = ArrayList<FixEntity>()
        db.fixes().lastDesktop()?.let { cands += it }
        fun LocationDto.fix(at: Long) = FixEntity(time = at - ((ageS ?: 0.0) * 1000).toLong(), lat = lat, lon = lon, acc = accuracy, source = "desktop", place = place)
        if (!DevFlags.desktopBlocked()) live.views.value.values.forEach { v -> v.location?.takeIf { it.valid && v.fetched > 0 }?.let { cands += it.fix(v.fetched) } }
        cache.snapshotNow("location")?.let { s -> cache.decode<LocationDto>(s)?.takeIf { it.valid }?.let { cands += it.fix(s.fetchedAt) } }
        return cands.filter { Origin.valid(it.lat, it.lon) }.maxByOrNull { it.time }
    }

    override suspend fun phonePlacesOn() = prefs.phonePlaces.first()
    override suspend fun pedsRadiusKm() = prefs.pedsRadiusKm.first()

    override suspend fun countryCode(): String {
        val trip = cache.decode<TripDto>(cache.snapshotNow("trip"))?.trip?.locale?.countryCode
            ?: live.views.value.values.firstNotNullOfOrNull { it.trip?.locale?.countryCode?.takeIf { c -> c.isNotBlank() } }
        if (!trip.isNullOrBlank()) return trip.uppercase(Locale.ROOT)
        val tm = ctx.getSystemService(TelephonyManager::class.java)
        val net = runCatching { tm?.networkCountryIso }.getOrNull()?.takeIf { it.isNotBlank() }
            ?: runCatching { tm?.simCountryIso }.getOrNull()?.takeIf { it.isNotBlank() }
            ?: Locale.getDefault().country
        return net.uppercase(Locale.ROOT)
    }

    override suspend fun address(lat: Double, lon: Double, allowNetwork: Boolean) = addresses.resolve(lat, lon, allowNetwork)

    override fun touchWidgets() { runCatching { widgets.get().touch("help") } }
}
