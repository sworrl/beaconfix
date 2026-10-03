// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import android.content.Context
import androidx.hilt.work.HiltWorker
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.ExistingWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import dagger.assisted.Assisted
import dagger.assisted.AssistedInject
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.sworrl.beaconfix.BuildConfig
import org.sworrl.beaconfix.data.DevFlags
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.PlateEventEntity
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The phone's HaveIBeenFlocked watch (docs/SIGHTINGS.md §4.1–4.5): a low-rate scheduled check of the registered plates
 * (the ones the desktop serves) by k-anonymity prefix — the plate never leaves the phone. The interval follows what
 * the phone did: driving 24 h, within 72 h of a Flock-network ALPR pass 12 h, of a leaky camera 3 h, else 7 days; the
 * first run checks at once (every page: the index is cumulative). Floors: ≥ 10 s between requests, ≤ 30 a day,
 * 429 / 5xx / network errors back off. Hits are `plate_search` events, alerted at once and pushed to the desktops.
 */
@Singleton
class HibfWatch @Inject constructor(
    @ApplicationContext private val ctx: Context,
    private val prefs: Prefs,
    private val repo: PlateEventRepository,
    private val db: AppDatabase,
) {
    /** The real network. Tests use [Hibf.search] with a fake [Hibf.Transport] instead. */
    val transport = Hibf.Transport { url, body ->
        val client = ApiFactory.client.newBuilder().readTimeout(30, TimeUnit.SECONDS).build()
        val req = Request.Builder().url(url).header("User-Agent", "BeaconFix/${BuildConfig.VERSION_NAME} (personal plate watch)")
            .post(body.toRequestBody("application/json".toMediaType())).build()
        client.newCall(req).execute().use { r -> Hibf.HttpResult(r.code, r.body?.string().orEmpty(), retryAfter(r.header("Retry-After"))) }
    }

    private fun retryAfter(h: String?): Long? {
        if (h.isNullOrBlank()) return null
        h.trim().toLongOrNull()?.let { return it }
        return runCatching { (java.time.ZonedDateTime.parse(h.trim(), java.time.format.DateTimeFormatter.RFC_1123_DATE_TIME).toInstant().toEpochMilli() - System.currentTimeMillis()) / 1000 }
            .getOrNull()?.coerceAtLeast(1)
    }

    suspend fun state(): Hibf.WatchState = Hibf.WatchState.decode(prefs.hibfWatch.first())

    /** The schedule's inputs from what this phone recorded. */
    suspend fun currentMode(now: Long = System.currentTimeMillis()): Hibf.Mode {
        val passes = db.plateEvents().passesSince(now - 72 * Hibf.HOUR).filter { it.cameraType == "alpr" }
        val leaky = passes.filter { it.leaky == 1 }.maxOfOrNull { it.timeMs }
        val flock = passes.filter { PassDetector.isFlock(it.operator, it.model) }.maxOfOrNull { it.timeMs }
        return Hibf.mode(now, prefs.lastDrivingAt.first().takeIf { it > 0 }, flock, leaky)
    }

    /** Check when due (or [force]); returns a one-line status. */
    suspend fun runIfDue(force: Boolean = false): String = withContext(Dispatchers.IO) {
        val now = System.currentTimeMillis()
        var st = state()
        val mode = currentMode(now)
        val plates = repo.registeredPlates()
        if (plates.isEmpty()) {
            st = st.copy(mode = mode.key, lastStatus = "no registered plates yet (they come from the desktop)")
            prefs.setHibfWatch(st.encode()); return@withContext st.lastStatus
        }
        if (DevFlags.simOffline) return@withContext "offline (simulated)"
        if ((!force && !Hibf.due(st, mode, now)) || (force && now < st.blockedUntil)) {
            st = st.copy(mode = mode.key, nextCheck = Hibf.nextCheck(st, mode)); prefs.setHibfWatch(st.encode())
            return@withContext "not due"
        }
        val limiter = Hibf.Limiter(st, { System.currentTimeMillis() }, { Thread.sleep(it) })
        // one search for every variant of every registered plate, all pages (the desktop does the same)
        val vars = Hibf.variants(plates.map { it.first })
        val outcome = Hibf.search(Hibf.prefixes(vars), transport, limiter::mayRequest, limiter::pace)
        val rows = when (outcome) { is Hibf.SearchOutcome.Ok -> outcome.rows; is Hibf.SearchOutcome.RateLimited -> outcome.rows; is Hibf.SearchOutcome.Failed -> outcome.rows; is Hibf.SearchOutcome.Capped -> outcome.rows }
        st = limiter.state
        val fresh = ArrayList<PlateEventEntity>()
        for (m in Hibf.filterResults(rows, vars)) {
            val ev = Hibf.toEvent(m, now)
            val existed = db.plateEvents().byUid(ev.uid) != null
            val stored = repo.upsertLocal(PlateEventEntity(uid = ev.uid, kind = PlateEvents.PLATE_SEARCH, plate = ev.plate, time = ev.timeLocal, timeMs = ev.timeMs,
                agency = ev.agency.ifEmpty { null }, source = PlateEvents.SRC_HIBF, sourceUrl = ev.sourceUrl, sourceName = ev.sourceName, confidence = ev.confidence,
                leaky = 1, details = ev.details, metrics = ev.metrics.toString(), raw = ev.raw.toString(), device = repo.deviceName,
                createdAt = PlateEvents.localIso(now), updatedAt = PlateEvents.localIso(now), dirty = true))
            if (!existed && !stored.notified) fresh += stored
        }
        repo.announce(fresh)
        if (fresh.isNotEmpty()) PlateEventSync.schedule(ctx)
        st = when (val o = outcome) {
            is Hibf.SearchOutcome.Ok -> st.copy(lastCheck = now, lastStatus = "ok", lastError = "", hits = st.hits + fresh.size, backoffMs = 0, blockedUntil = 0)
            is Hibf.SearchOutcome.RateLimited -> Hibf.backoff(st, now, o.retryAfterS).copy(lastStatus = "429", lastError = "rate limited", hits = st.hits + fresh.size)
            is Hibf.SearchOutcome.Failed -> Hibf.backoff(st, now, null).copy(lastStatus = if (o.code > 0) "${o.code}" else "network", lastError = o.message, hits = st.hits + fresh.size)
            is Hibf.SearchOutcome.Capped -> st.copy(lastStatus = "daily cap", blockedUntil = now + 6 * Hibf.HOUR, hits = st.hits + fresh.size)
        }
        st = st.copy(mode = mode.key, nextCheck = Hibf.nextCheck(st, mode))
        prefs.setHibfWatch(st.encode())
        "${st.lastStatus}: ${fresh.size} new"
    }

}

/** Runs [HibfWatch] every 30 minutes on a network; it only asks the site when the schedule says so. */
@HiltWorker
class HibfWatcher @AssistedInject constructor(@Assisted ctx: Context, @Assisted params: WorkerParameters, private val watch: HibfWatch) : CoroutineWorker(ctx, params) {
    override suspend fun doWork(): Result {
        runCatching { watch.runIfDue(inputData.getBoolean("force", false)) }
        return Result.success()
    }

    companion object {
        private val net = Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build()

        fun ensurePeriodic(ctx: Context) {
            val req = PeriodicWorkRequestBuilder<HibfWatcher>(30, TimeUnit.MINUTES).setConstraints(net).build()
            runCatching { WorkManager.getInstance(ctx).enqueueUniquePeriodicWork("hibf-watch", ExistingPeriodicWorkPolicy.KEEP, req) }
        }

        /** Re-evaluate the schedule soon (after an ALPR pass, which may shorten the interval). */
        fun nudge(ctx: Context) {
            val req = OneTimeWorkRequestBuilder<HibfWatcher>().setInitialDelay(1, TimeUnit.MINUTES).setConstraints(net).build()
            runCatching { WorkManager.getInstance(ctx).enqueueUniqueWork("hibf-nudge", ExistingWorkPolicy.KEEP, req) }
        }

        fun checkNow(ctx: Context) {
            val req = OneTimeWorkRequestBuilder<HibfWatcher>().setInputData(androidx.work.workDataOf("force" to true)).setConstraints(net).build()
            runCatching { WorkManager.getInstance(ctx).enqueueUniqueWork("hibf-now", ExistingWorkPolicy.REPLACE, req) }
        }
    }
}
