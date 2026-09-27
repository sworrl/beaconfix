package org.sworrl.beaconfix.poi

import android.content.Context
import android.net.ConnectivityManager
import android.util.Log
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.DevFlags
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.api.OriginDto
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.PoiEntity
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

/** One finished search: where from, when (epoch ms), how many places it kept, and (pediatric) the radius. */
@Serializable data class SearchMark(val lat: Double = 0.0, val lon: Double = 0.0, val at: Long = 0, val count: Int = 0, val radiusKm: Int = 0)

/**
 * The `phoneplaces` snapshot (source `phone`): [origin]/[time] = the newest successful search of either kind, [note] =
 * what the last attempt said ("" when it went fine), [help]/[peds] = each search's own mark (what the freshness rules
 * look at). Decode it with `DesktopCache.decode<PhonePlacesState>(cache.snapshotFrom("phone", "phoneplaces"))`.
 */
@Serializable data class PhonePlacesState(
    val origin: OriginDto? = null,
    val time: String = "",
    val note: String = "",
    val help: SearchMark? = null,
    val peds: SearchMark? = null,
)

/**
 * When the phone may ask Overpass itself (plan A4). Pure, so the rules are unit-tested with a fake clock.
 *
 * - Help search: when moved more than 5 km or older than 24 h. Pediatric search: moved more than 40 km, older than
 *   30 days, or the radius changed. `force` skips these freshness checks (and the desktop one), never the others.
 * - Never: while backing off (10 min after any failure), with the fix worse than ±5 km, with phone search off, on a
 *   metered network with "use mobile data" off, while simulated offline, or with no network at all.
 * - Not needed: a desktop's cached answer less than 24 h old, searched from within 25 km (its `far` rows count for
 *   the pediatric search).
 */
object PhonePlacesPolicy {
    const val HELP_MOVE_M = 5_000.0
    const val HELP_MAX_AGE_MS = 24 * 3_600_000L
    const val PEDS_MOVE_M = 40_000.0
    const val PEDS_MAX_AGE_MS = 30 * 86_400_000L
    const val BACKOFF_MS = 10 * 60_000L
    const val GAP_MS = 5_000L
    const val MAX_ACCURACY_M = 5_000.0
    const val DESKTOP_NEAR_M = 25_000.0
    const val DESKTOP_FRESH_MS = 24 * 3_600_000L

    const val NOTE_FRESH = "fresh, skipped"
    const val NOTE_BUSY = "Overpass busy — will retry"

    /** The switches and circumstances that gate a search (read from Prefs, the network and the last phone fix). */
    data class Conditions(
        val enabled: Boolean = true,
        val allowMetered: Boolean = true,
        val metered: Boolean = false,
        val online: Boolean = true,
        val simOffline: Boolean = false,
        val accuracyM: Double? = null,
        val pedsRadiusKm: Int = 150,
    )

    /** Fresh desktop answers around the point: [help] = any, [peds] = one that includes the wide (`far`) search. */
    data class DesktopCoverage(val help: Boolean = false, val peds: Boolean = false)

    data class Plan(val help: Boolean, val peds: Boolean, val note: String) { val any get() = help || peds }

    /** Why nothing may be requested at all, or null. */
    fun blocked(now: Long, c: Conditions, busyUntil: Long): String? = when {
        c.simOffline -> "offline (simulated)"
        !c.online -> "offline"
        !c.enabled -> "phone search is off"
        c.metered && !c.allowMetered -> "on mobile data — phone search is set to Wi-Fi only"
        c.accuracyM != null && c.accuracyM > MAX_ACCURACY_M -> "location too rough (±${(c.accuracyM / 1000).toInt()} km)"
        busyUntil > now -> NOTE_BUSY
        else -> null
    }

    fun helpDue(now: Long, lat: Double, lon: Double, m: SearchMark?): Boolean =
        m == null || m.at <= 0 || now - m.at > HELP_MAX_AGE_MS || now < m.at || PedsClassifier.distanceM(m.lat, m.lon, lat, lon) > HELP_MOVE_M

    fun pedsDue(now: Long, lat: Double, lon: Double, m: SearchMark?, radiusKm: Int): Boolean =
        m == null || m.at <= 0 || now - m.at > PEDS_MAX_AGE_MS || now < m.at || m.radiusKm != radiusKm ||
            PedsClassifier.distanceM(m.lat, m.lon, lat, lon) > PEDS_MOVE_M

    /** Which desktop rows make a phone search unnecessary around [lat]/[lon]. */
    fun desktopCoverage(rows: List<PoiEntity>, lat: Double, lon: Double, now: Long): DesktopCoverage {
        var help = false; var peds = false
        for (r in rows) {
            if (!DesktopCache.isDesktop(r) || now - r.fetchedAt > DESKTOP_FRESH_MS || now < r.fetchedAt) continue
            if (r.originLat == 0.0 && r.originLon == 0.0) continue
            if (PedsClassifier.distanceM(r.originLat, r.originLon, lat, lon) > DESKTOP_NEAR_M) continue
            help = true
            if (r.scope == DesktopCache.FAR) peds = true
            if (peds) break
        }
        return DesktopCoverage(help, peds)
    }

    fun plan(now: Long, lat: Double, lon: Double, force: Boolean, pediatric: Boolean, c: Conditions, state: PhonePlacesState?,
             busyUntil: Long, desktop: DesktopCoverage): Plan {
        blocked(now, c, busyUntil)?.let { return Plan(false, false, it) }
        if (force) return Plan(true, pediatric, "")
        val help = helpDue(now, lat, lon, state?.help) && !desktop.help
        val peds = pediatric && pedsDue(now, lat, lon, state?.peds, c.pedsRadiusKm) && !desktop.peds
        val note = when {
            help || peds -> ""
            desktop.help && (!pediatric || desktop.peds) -> "the desktop's places here are fresh, skipped"
            else -> NOTE_FRESH
        }
        return Plan(help, peds, note)
    }
}

/**
 * [PhonePlaces] over the Overpass API: the help categories only, only when asked (HelpRepository.refresh, i.e. the
 * user opened Help / Places or pulled to refresh with no desktop in reach), never from a worker. One request at a
 * time; the pediatric request waits at least 5 s after the previous one; any failure backs off 10 minutes and keeps
 * everything already cached. Results go to [DesktopCache] (source `phone`: help rows scope `near`, the wide
 * children's ER search scope `far`) plus the `phoneplaces` snapshot. Logs with tag `BfOverpass` (no coordinates).
 */
@Singleton
class OverpassPhonePlaces internal constructor(
    private val overpass: OverpassSource,
    private val cache: DesktopCache,
    private val conditions: suspend (lat: Double, lon: Double) -> PhonePlacesPolicy.Conditions,
    private val clock: () -> Long,
    private val pause: suspend (Long) -> Unit,
) : PhonePlaces {
    @Inject constructor(client: OverpassClient, cache: DesktopCache, db: AppDatabase, prefs: Prefs, @ApplicationContext ctx: Context) :
        this(client, cache, liveConditions(db, prefs, ctx), System::currentTimeMillis, { delay(it) })

    private val running = Mutex()
    @Volatile private var lastRequestEnd = 0L
    @Volatile override var busyUntil: Long = 0L
        private set

    override suspend fun refreshAround(lat: Double, lon: Double, force: Boolean, pediatric: Boolean): PhonePlacesResult {
        if (!running.tryLock()) return PhonePlacesResult(false, note = "a phone search is already running", skipped = true)
        try {
            return withContext(Dispatchers.Default) { search(lat, lon, force, pediatric) }   // parsing/classifying is CPU work
        } catch (e: kotlinx.coroutines.CancellationException) {
            throw e
        } catch (e: Exception) {                                  // never throws: a bug here must not break Help
            Log.w(TAG, "phone search failed: $e")
            return PhonePlacesResult(false, note = "phone search failed")
        } finally {
            running.unlock()
        }
    }

    private suspend fun search(lat: Double, lon: Double, force: Boolean, pediatric: Boolean): PhonePlacesResult {
        val now = clock()
        val cond = conditions(lat, lon)
        val state = loadState()
        val coverage = if (force) PhonePlacesPolicy.DesktopCoverage() else PhonePlacesPolicy.desktopCoverage(cache.poisNow(), lat, lon, now)
        val plan = PhonePlacesPolicy.plan(now, lat, lon, force, pediatric, cond, state, busyUntil, coverage)
        if (!plan.any) {
            Log.i(TAG, plan.note)
            return PhonePlacesResult(false, note = plan.note, skipped = true)
        }
        var st = state ?: PhonePlacesState()
        var count = 0; var anyOk = false; var failure: String? = null
        var helpRows: List<PoiEntity>? = null

        if (plan.help) {
            gap()
            Log.i(TAG, "help request (${HelpQuery.HELP_RADIUS_M / 1000} km)${if (force) ", forced" else ""}")
            val r = overpass.query(HelpQuery.help(HelpQuery.bbox(lat, lon, HelpQuery.HELP_RADIUS_M)), HelpQuery.HELP_TIMEOUT_S)
            lastRequestEnd = clock()
            if (r.ok) {
                val at = clock()
                val hits = HelpQuery.selectHelp(r.elements, PedsClassifier.classifyAll(r.elements), lat, lon)
                val rows = hits.map { HelpQuery.toEntity(it, DesktopCache.NEAR, lat, lon, at) }
                saveNear(rows)
                helpRows = rows
                count += rows.size; anyOk = true
                st = st.copy(help = SearchMark(lat, lon, at, rows.size))
                st = saveState(st, lat, lon, at, "")
                Log.i(TAG, "help: ${r.elements.size} elements, kept ${rows.size} (${r.mirror})")
            } else failure = r.error
        }

        if (plan.peds && failure == null) {
            gap()
            val radiusKm = cond.pedsRadiusKm
            Log.i(TAG, "pediatric request ($radiusKm km)${if (force) ", forced" else ""}")
            val q = HelpQuery.pediatric(HelpQuery.bbox(lat, lon, radiusKm * 1000), HelpQuery.bbox(lat, lon, HelpQuery.URGENT_RADIUS_M))
            val r = overpass.query(q, HelpQuery.PEDS_TIMEOUT_S)
            lastRequestEnd = clock()
            if (r.ok) {
                val at = clock()
                val hits = HelpQuery.selectPediatric(r.elements, PedsClassifier.classifyAll(r.elements), lat, lon, radiusKm * 1000)
                val rows = hits.map { HelpQuery.toEntity(it, DesktopCache.FAR, lat, lon, at) }
                cache.savePhonePois(DesktopCache.FAR, rows)
                helpRows?.let { saveNear(it) }                     // a place in both answers lives in the far row
                count += rows.size; anyOk = true
                st = st.copy(peds = SearchMark(lat, lon, at, rows.size, radiusKm))
                st = saveState(st, lat, lon, at, "")
                Log.i(TAG, "pediatric: ${r.elements.size} elements, kept ${rows.size} " +
                    "(${rows.count { it.cat == "peds_er" }} pediatric ER, ${rows.count { it.cat == "peds_urgent" }} pediatric urgent care, " +
                    "${rows.count { it.cat == "health" }} ER) (${r.mirror})")
            } else failure = r.error
        }

        if (failure != null) {
            busyUntil = clock() + PhonePlacesPolicy.BACKOFF_MS
            Log.w(TAG, "search failed ($failure); backing off ${PhonePlacesPolicy.BACKOFF_MS / 60_000} min, cached places kept")
            if (st.origin != null) saveState(st, st.origin!!.lat, st.origin!!.lon, lastSuccessAt(st), PhonePlacesPolicy.NOTE_BUSY)
            return PhonePlacesResult(anyOk, count, PhonePlacesPolicy.NOTE_BUSY)
        }
        busyUntil = 0L
        return PhonePlacesResult(true, count, if (count == 0) "no help places mapped nearby" else "")
    }

    /** Overpass etiquette: at least 5 s between the end of one request and the start of the next. */
    private suspend fun gap() {
        val wait = lastRequestEnd + PhonePlacesPolicy.GAP_MS - clock()
        if (lastRequestEnd > 0 && wait > 0) pause(wait)
    }

    /** Store the help rows, minus places the phone's far scope already holds (one row per place; no replace loses it). */
    private suspend fun saveNear(rows: List<PoiEntity>) {
        val far = cache.poisNow().filter { it.source == DesktopCache.PHONE && it.scope == DesktopCache.FAR }.mapTo(HashSet()) { it.key }
        cache.savePhonePois(DesktopCache.NEAR, rows.filter { it.key !in far })
    }

    private fun lastSuccessAt(s: PhonePlacesState) = maxOf(s.help?.at ?: 0L, s.peds?.at ?: 0L)

    private suspend fun loadState(): PhonePlacesState? =
        cache.decode<PhonePlacesState>(cache.snapshotFrom(DesktopCache.PHONE, KIND))

    private suspend fun saveState(s: PhonePlacesState, lat: Double, lon: Double, at: Long, note: String): PhonePlacesState {
        val out = s.copy(origin = OriginDto(lat = lat, lon = lon, source = DesktopCache.PHONE, time = Instant.ofEpochMilli(at).toString(),
            radiusKm = HelpQuery.HELP_RADIUS_M / 1000.0), time = Instant.ofEpochMilli(at).toString(), note = note)
        cache.putSnapshot(DesktopCache.PHONE, KIND, cache.encode(out), lat, lon, at)
        return out
    }

    companion object {
        const val TAG = OverpassClient.TAG
        const val KIND = "phoneplaces"

        private fun liveConditions(db: AppDatabase, prefs: Prefs, ctx: Context): suspend (Double, Double) -> PhonePlacesPolicy.Conditions = { lat, lon ->
            val cm = ctx.getSystemService(ConnectivityManager::class.java)
            val fix = runCatching { db.fixes().lastPhone() }.getOrNull()
            PhonePlacesPolicy.Conditions(
                enabled = prefs.phonePlaces.first(),
                allowMetered = prefs.phonePlacesMetered.first(),
                metered = runCatching { cm?.isActiveNetworkMetered ?: false }.getOrDefault(false),
                online = runCatching { cm?.activeNetwork != null }.getOrDefault(true),
                simOffline = DevFlags.simOffline,
                // the accuracy of the phone fix this search is for (HelpRepository passes the last phone fix)
                accuracyM = fix?.takeIf { PedsClassifier.distanceM(it.lat, it.lon, lat, lon) < 1_000.0 }?.acc,
                pedsRadiusKm = prefs.pedsRadiusKm.first(),
            )
        }
    }
}
