// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import android.content.Context
import android.util.Base64
import android.util.Log
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import androidx.hilt.work.HiltWorker
import dagger.assisted.Assisted
import dagger.assisted.AssistedInject
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.DevFlags
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.PlateEventDto
import org.sworrl.beaconfix.data.api.PlateEventsPush
import org.sworrl.beaconfix.data.api.PlateMediaUpload
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.data.db.PlateEventEntity
import org.sworrl.beaconfix.data.db.PlateEventMediaEntity
import java.io.File
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton

/** An image this phone made (a dash-cam frame, a webcam still), already encoded. */
class LocalImage(val bytes: ByteArray, val mime: String, val width: Int, val height: Int, val capturedMs: Long, val kind: String, val note: String = "")

/**
 * Plate events on the phone (docs/SIGHTINGS.md): the Room rows, the dash-cam frames on disk, and the exchange with the
 * paired desktops — push our passes and searches (§5 `POST /plate-events`), upload our frames (`POST …/media`, then
 * delete the local copy), pull the feed (`GET /plate-events?since=`), cache `hibfSources` and the registered plates.
 * Every merge follows §1.1 ([PlateLedger]). The hub (`/api/v3`) carries the records too ([org.sworrl.beaconfix.sync.HubSync]
 * pushes [hubPending] and pulls `db/changes` into [ingest] with [PlateDest.HUB]); the images stay queued here until a
 * LAN desktop takes them. Each destination has its own "waiting to send" flag and its own cursor.
 */
@Singleton
class PlateEventRepository @Inject constructor(
    @ApplicationContext private val ctx: Context,
    private val db: AppDatabase,
    private val desktops: DesktopStore,
    private val prefs: Prefs,
    private val alerts: SightingAlerts,
    private val identity: org.sworrl.beaconfix.identity.IdentityStore,
) {
    private val dao get() = db.plateEvents()
    private val lock = Mutex()
    private val ledger = PlateLedger(object : PlateRows {
        override suspend fun byUid(uid: String) = dao.byUid(uid)
        override suspend fun passNear(cameraId: String, fromMs: Long, toMs: Long, atMs: Long) = dao.passNear(cameraId, fromMs, toMs, atMs)
        override suspend fun insert(e: PlateEventEntity) = dao.insert(e)
        override suspend fun update(e: PlateEventEntity) = dao.update(e)
        override suspend fun delete(uid: String) = dao.delete(uid)
        override suspend fun moveMedia(from: String, to: String) = dao.moveMedia(from, to)
        override suspend fun insertMedia(m: PlateEventMediaEntity) = dao.insertMedia(m)
    })
    private val mediaDir: File get() = File(ctx.noBackupFilesDir, "plate_media").apply { mkdirs() }
    private val displayDir: File get() = File(ctx.cacheDir, "plate_media_display").apply { mkdirs() }
    private val lastQuickPull = HashMap<String, Long>()

    val deviceName: String get() = identity.deviceName

    // ── local writes ──
    /** Store [e] merged per §1.1 with what is there (same uid, or the same camera within ±10 min); returns the stored row. */
    suspend fun upsertLocal(e: PlateEventEntity): PlateEventEntity = lock.withLock { ledger.upsertLocal(e) }

    /** A pass (or a search) this phone recorded, with its images; alerts per §6 and schedules the push. */
    suspend fun recordLocal(e: PlateEventEntity, images: List<LocalImage> = emptyList()): PlateEventEntity {
        val stored = upsertLocal(e)
        for (img in images) runCatching { saveImage(stored.uid, stored.cameraId, img) }.onFailure { Log.w(TAG, "image", it) }
        if (!stored.notified) {
            if (alerts.alertable(stored)) alerts.alert(stored)
            dao.markNotified(listOf(stored.uid))
        }
        trimCache()
        PlateEventSync.schedule(ctx)
        return stored
    }

    private suspend fun saveImage(eventUid: String, cameraId: String?, img: LocalImage) = withContext(Dispatchers.IO) {
        val uid = Hibf.sha256Hex(img.bytes).take(32)
        val ext = when (img.mime) { "image/webp" -> "webp"; "image/png" -> "png"; "image/jpeg" -> "jpg"; else -> "bin" }
        val f = File(mediaDir, "$uid.$ext")
        if (!f.exists()) { val tmp = File(mediaDir, "$uid.tmp"); tmp.writeBytes(img.bytes); tmp.renameTo(f) }
        dao.insertMedia(PlateEventMediaEntity(uid = uid, eventUid = eventUid, cameraId = if (img.kind == "camera_photo") cameraId else null, kind = img.kind, mime = img.mime,
            path = f.absolutePath, width = img.width, height = img.height, bytes = img.bytes.size.toLong(), capturedAt = PlateEvents.localIso(img.capturedMs),
            createdAt = PlateEvents.localIso(System.currentTimeMillis()), attribution = img.note.ifEmpty { null }))
    }

    /** §3.3: the phone's frames are kept until the desktop has them, at most [CACHE_CAP] bytes, oldest deleted first. */
    suspend fun trimCache(cap: Long = CACHE_CAP) = withContext(Dispatchers.IO) {
        val rows = dao.localFiles()
        val files = rows.mapNotNull { r -> r.path?.let { File(it) }?.takeIf { it.exists() }?.let { r to it } }.sortedBy { it.second.lastModified() }
        var total = files.sumOf { it.second.length() }
        for ((r, f) in files) {
            if (total <= cap) break
            total -= f.length(); f.delete()
            if (r.remote) dao.updateMedia(r.copy(path = null)) else dao.deleteMedia(r.uid)
        }
        // rows whose file is gone (cleared storage) and that the desktop never got
        for (r in rows) if (r.path != null && !File(r.path).exists()) { if (r.remote) dao.updateMedia(r.copy(path = null)) else dao.deleteMedia(r.uid) }
    }

    // ── desktop exchange ──
    data class SyncResult(val pushed: Int = 0, val uploaded: Int = 0, val pulled: Int = 0, val error: String = "")

    /** Every paired desktop: push, upload, pull. Hub enrolment does not matter here (media go to the nodes). */
    suspend fun syncAll(): List<SyncResult> {
        if (DevFlags.desktopBlocked()) return emptyList()
        return desktops.paired().map { d -> runCatching { sync(d) }.getOrElse { if (it is CancellationException) throw it; SyncResult(error = it.message ?: "failed") } }
    }

    /** While a screen talks to [d] anyway (DesktopLive): at most once a minute, pull the feed (and push what is waiting). */
    fun quickSync(d: DesktopEntity) {
        val now = System.currentTimeMillis()
        synchronized(lastQuickPull) {
            if (now - (lastQuickPull[d.id] ?: 0L) < 60_000L) return
            lastQuickPull[d.id] = now
        }
        bg.launch { runCatching { sync(d, statusToo = now - lastLists > 30 * 60_000L).also { if (it.error.isEmpty()) lastLists = now } } }
    }
    private val bg = kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.SupervisorJob() + Dispatchers.IO)
    @Volatile private var lastLists = 0L

    suspend fun sync(d: DesktopEntity, statusToo: Boolean = true): SyncResult {
        val auth = desktops.auth(d) ?: return SyncResult(error = "not paired")
        val api = desktops.api(d)
        val canControl = desktops.hasScope(d, "control")
        var pushed = 0; var uploaded = 0; var pulled = 0
        // ── push our events (control scope) ──
        if (canControl) {
            val tried = HashSet<String>()
            while (true) {
                val batch = sizeLimited(dao.dirty(500).filter { it.uid !in tried }.take(100).map { PlateEvents.toDto(it) })
                if (batch.isEmpty()) break
                batch.forEach { tried += it.uid }
                val r = api.pushPlateEvents(auth, PlateEventsPush(batch, deviceName))
                if (r.code() == 404) return SyncResult(error = "this desktop has no plate events yet")
                if (r.code() == 401) { desktops.forgetToken(d.id); return SyncResult(error = "token rejected") }
                if (!r.isSuccessful) break
                val uids = r.body()?.uids.orEmpty()
                lock.withLock {
                    // uids[i] is where event i landed (a merge keeps the desktop's uid); null = rejected, kept and retried later
                    batch.forEachIndexed { i, sent -> uids.getOrNull(i)?.takeIf { it.isNotEmpty() && it != sent.uid }?.let { ledger.rename(sent.uid, it) } }
                    dao.clean(batch.mapIndexedNotNull { i, sent -> if (i < uids.size) uids[i]?.takeIf { it.isNotEmpty() } else sent.uid })
                }
                pushed += batch.size
            }
            // ── upload our frames once their event is on the desktop ──
            val bulk = ApiFactory.createBulk(d.host, d.port, d.tls)
            for (m in dao.pendingUploads()) {
                val ev = m.eventUid?.let { dao.byUid(it) } ?: continue
                if (ev.dirty) continue
                val f = m.path?.let { File(it) }?.takeIf { it.exists() } ?: continue
                if (f.length() > MAX_UPLOAD) continue
                val body = withContext(Dispatchers.IO) { PlateMediaUpload(m.kind, m.mime, Base64.encodeToString(f.readBytes(), Base64.NO_WRAP), m.width ?: 0, m.height ?: 0, m.capturedAt.orEmpty()) }
                val r = runCatching { bulk.pushPlateMedia(auth, ev.uid, body) }.getOrNull() ?: break
                // 400 / 404 / 413 / 422: this image or its event is refused (kept, retried next sync); anything else: stop for now
                if (!r.isSuccessful) { if (r.code() in setOf(400, 404, 413, 422)) continue else break }
                val newUid = r.body()?.uid?.takeIf { it.isNotEmpty() } ?: m.uid
                lock.withLock {
                    if (newUid != m.uid && dao.mediaByUid(newUid) != null) dao.deleteMedia(m.uid)
                    else dao.updateMedia(m.copy(uid = newUid, path = null, remote = true))
                }
                withContext(Dispatchers.IO) { f.delete() }
                uploaded++
            }
        }
        // ── pull the feed ──
        val cursors = cursors()
        var since = cursors[d.id] ?: 0L
        val fresh = ArrayList<PlateEventEntity>()
        for (page in 0 until 50) {
            val r = runCatching { api.plateEvents(auth, since, 500) }.getOrNull() ?: break
            if (!r.isSuccessful) break
            val body = r.body() ?: break
            fresh += ingest(body.events)
            pulled += body.events.size
            val next = (body.cursor as? JsonPrimitive)?.contentOrNull?.toDoubleOrNull()?.toLong() ?: body.events.maxOfOrNull { it.seq ?: 0L } ?: since
            if (next <= since && body.more) break
            since = maxOf(since, next)
            if (!body.more) break
        }
        if (since != (cursors[d.id] ?: 0L)) saveCursor(d.id, since)
        announce(fresh)
        if (statusToo) refreshDesktopLists(d, auth)
        return SyncResult(pushed, uploaded, pulled)
    }

    /** `/plate-events/status` → the leaky-agency list; `/plates` → the registered plates (both cached in Prefs). */
    suspend fun refreshDesktopLists(d: DesktopEntity, auth: String = desktops.auth(d).orEmpty()) {
        val api = desktops.api(d)
        runCatching { api.plateEventsStatus(auth) }.getOrNull()?.takeIf { it.isSuccessful }?.body()?.hibfSources?.let { s ->
            prefs.setHibfSources(buildJsonObject { put("fetched", System.currentTimeMillis()); put("desktop", d.id); put("agencies", s) }.toString())
        }
        runCatching { api.licensePlates(auth) }.getOrNull()?.takeIf { it.isSuccessful }?.body()?.let { prefs.setRegisteredPlates(it.toString()) }
    }

    /** The registered plates (display, normalized) the desktop serves; empty until a desktop answered once. */
    suspend fun registeredPlates(): List<Pair<String, String>> =
        runCatching { Hibf.plateList(ApiFactory.json.parseToJsonElement(prefs.registeredPlates.first()).jsonObject) }.getOrDefault(emptyList())

    /** The plate a pass is recorded for: the desktop's active plate. */
    suspend fun activePlate(): String? = runCatching { Hibf.activePlate(ApiFactory.json.parseToJsonElement(prefs.registeredPlates.first()).jsonObject) }.getOrNull()

    /** The desktop's leaky-agency list (§4.4), as cached from `/plate-events/status`. */
    suspend fun leakyAgencies(): kotlinx.serialization.json.JsonArray =
        runCatching { ApiFactory.json.parseToJsonElement(prefs.hibfSources.first()).jsonObject["agencies"] as? kotlinx.serialization.json.JsonArray }.getOrNull()
            ?: kotlinx.serialization.json.JsonArray(emptyList())

    /** A feed's events into Room (merged per §1.1, see [PlateLedger.ingest]); returns the rows that are new to this phone. */
    suspend fun ingest(list: List<PlateEventDto>, dest: PlateDest = PlateDest.LAN): List<PlateEventEntity> = lock.withLock { ledger.ingest(list, dest) }

    // ── the hub (records only) ──
    /** The next records waiting for the hub, oldest first, cut at [maxBytes] of JSON (at least one when any wait). */
    suspend fun hubPending(maxBytes: Int = MAX_PUSH_BYTES, skip: Set<String> = emptySet()): List<PlateEventDto> =
        sizeLimited(dao.hubDirty(500).filter { it.uid !in skip }.take(100).map { PlateEvents.toDto(it) }, maxBytes)

    /** The hub took these records: clear their hub flag (the desktop's flag and every image stay as they are). */
    suspend fun markHubPushed(uids: List<String>, renames: List<Pair<String, String>> = emptyList()) {
        if (uids.isEmpty() && renames.isEmpty()) return
        lock.withLock {
            for ((from, to) in renames) ledger.rename(from, to)
            val renamed = renames.toMap()
            dao.hubClean(uids.map { renamed[it] ?: it })
        }
    }

    /** §6 for events that arrived together: recent live ALPR passes and new searches alert, the rest is one summary. */
    suspend fun announce(fresh: List<PlateEventEntity>) {
        if (fresh.isEmpty()) return
        val plan = Sightings.plan(fresh, System.currentTimeMillis())
        for (e in plan.single) alerts.alert(e, sound = false)
        alerts.summary(plan.summaryPasses, plan.summarySearches, plan.summarySince)
        dao.markNotified(fresh.map { it.uid })
    }

    /** `GET /plate-events/<uid>`: the full event, `raw` included (Event detail). */
    suspend fun fetchFull(uid: String): Boolean {
        for (d in desktops.paired()) {
            val auth = desktops.auth(d) ?: continue
            val r = runCatching { desktops.api(d).plateEvent(auth, uid) }.getOrNull() ?: continue
            if (!r.isSuccessful) continue
            r.body()?.let { ingest(listOf(it)) }
            return true
        }
        return false
    }

    /**
     * The file to show for [m]: our own frame, else a cached copy of the desktop's `?as=display` rendering (JPEG / PNG;
     * Android cannot decode JPEG XL), fetched once from the first desktop that has it. Null offline with nothing cached.
     */
    suspend fun displayFile(m: PlateEventMediaEntity): File? = withContext(Dispatchers.IO) {
        m.path?.let { File(it) }?.takeIf { it.exists() }?.let { return@withContext it }
        val f = File(displayDir, m.uid.filter { it.isLetterOrDigit() })
        if (f.exists() && f.length() > 0) return@withContext f
        if (DevFlags.desktopBlocked()) return@withContext null
        for (d in desktops.paired()) {
            val auth = desktops.auth(d) ?: continue
            val r = runCatching { ApiFactory.createBulk(d.host, d.port, d.tls).plateMedia(auth, m.uid, "display") }.getOrNull() ?: continue
            if (!r.isSuccessful) { r.errorBody()?.close(); continue }
            val ok = r.body()?.use { b -> val tmp = File(displayDir, f.name + ".tmp"); tmp.outputStream().use { b.byteStream().copyTo(it) }; tmp.renameTo(f) } ?: false
            if (ok) { trimDisplayCache(); return@withContext f }
        }
        null
    }

    private fun trimDisplayCache(cap: Long = 100L * 1024 * 1024) {
        val files = displayDir.listFiles()?.sortedBy { it.lastModified() } ?: return
        var total = files.sumOf { it.length() }
        for (f in files) { if (total <= cap) break; total -= f.length(); f.delete() }
    }

    private suspend fun cursors(): Map<String, Long> = runCatching {
        ApiFactory.json.parseToJsonElement(prefs.plateEventCursors.first()).jsonObject.mapValues { it.value.jsonPrimitive.contentOrNull?.toLongOrNull() ?: 0L }
    }.getOrDefault(emptyMap())

    private suspend fun saveCursor(id: String, seq: Long) {
        val m = cursors() + (id to seq)
        prefs.setPlateEventCursors(JsonObject(m.mapValues { JsonPrimitive(it.value) }).toString())
    }

    companion object {
        private const val TAG = "PlateEvents"
        /** §3.3: the phone's frame cache. */
        const val CACHE_CAP = 300L * 1024 * 1024
        /** §5: the media body limit is 40 MB of JSON; base64 makes 4 bytes of 3, so files above ~29 MB are never sent. */
        const val MAX_UPLOAD = 29L * 1024 * 1024
        /** One push request: well under the desktop's sync-sized body limit (assumed 1 MB, as `/db/sync`). */
        const val MAX_PUSH_BYTES = 512 * 1024

        /** The events of one push, cut at [MAX_PUSH_BYTES] of JSON (at least one). */
        fun sizeLimited(list: List<PlateEventDto>, limit: Int = MAX_PUSH_BYTES): List<PlateEventDto> {
            var size = 64; val out = ArrayList<PlateEventDto>()
            for (e in list) {
                size += ApiFactory.json.encodeToString(PlateEventDto.serializer(), e).length + 1
                if (size > limit && out.isNotEmpty()) break
                out += e
            }
            return out
        }
    }
}

/**
 * As soon as a network is up after a live pass or a hit: pushes the records to the hub when enrolled (away from home
 * that is the only way out), then pushes and pulls with every paired desktop (records and images).
 */
@HiltWorker
class PlateEventSync @AssistedInject constructor(@Assisted ctx: Context, @Assisted params: WorkerParameters, private val repo: PlateEventRepository,
                                                 private val hub: org.sworrl.beaconfix.sync.HubSync) : CoroutineWorker(ctx, params) {
    override suspend fun doWork(): Result {
        if (hub.enrolled()) hub.pushPlateEvents()
        val r = repo.syncAll()
        return if (r.any { it.error.isNotEmpty() && it.pushed == 0 } && runAttemptCount < 3) Result.retry() else Result.success()
    }

    companion object {
        fun schedule(ctx: Context, delayS: Long = 20) {
            val req = OneTimeWorkRequestBuilder<PlateEventSync>().setInitialDelay(delayS, TimeUnit.SECONDS)
                .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build()).build()
            runCatching { WorkManager.getInstance(ctx).enqueueUniqueWork("plate-events-sync", ExistingWorkPolicy.KEEP, req) }
        }
    }
}
