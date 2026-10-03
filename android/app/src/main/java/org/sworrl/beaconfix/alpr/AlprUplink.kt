package org.sworrl.beaconfix.alpr

import android.content.Context
import android.net.ConnectivityManager
import android.util.Log
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import org.sworrl.beaconfix.alpr.core.HitBody
import org.sworrl.beaconfix.alpr.core.PendingEvent
import org.sworrl.beaconfix.alpr.core.SpillQueue
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Store-and-forward for plate events, memory first. A new event waits in memory and goes out as soon as FalconEyez
 * answers; only when it can't be sent (server out of reach, or the network policy says wait) is it spilled to
 * app-private storage, bounded by the spill cap (oldest ordinary events dropped first, ordinary events expire after
 * 24 h, hotlist events are kept until sent). An event is deleted the moment the server accepts it. Camera frames never
 * reach this class — only the crop JPEG and its meta.
 */
@Singleton
class AlprUplink @Inject constructor(
    @ApplicationContext private val ctx: Context,
    private val link: FalconLink,
    private val settings: AlprSettings,
    private val status: AlprStatus,
    private val alerts: AlprAlerts,
) {
    private val spill = SpillQueue(File(ctx.noBackupFilesDir, "alpr_spill"))
    private val mem = ArrayDeque<PendingEvent>()
    private val hits = ArrayDeque<HitBody>()
    private val wake = Channel<Unit>(Channel.CONFLATED)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var loop: Job? = null
    @Volatile private var backoffUntil = 0L
    @Volatile private var lastUnreachable = 0L
    /** Called when the server says the token is revoked (the service stops itself). */
    @Volatile var onRevoked: (() -> Unit)? = null

    init { publish() }

    /** Starts the uploader (idempotent). */
    @Synchronized fun start() {
        if (loop?.isActive == true) return
        loop = scope.launch { run() }
    }

    fun submit(e: PendingEvent) {
        synchronized(mem) { mem.addLast(e) }
        if (e.hotlist) backoffUntil = 0
        trimMemory(); publish(); wake.trySend(Unit)
    }

    fun submitHit(h: HitBody) { synchronized(hits) { hits.addLast(h); while (hits.size > 50) hits.removeFirst() }; backoffUntil = 0; wake.trySend(Unit) }

    fun kick() { backoffUntil = 0; wake.trySend(Unit) }

    /** Everything still in memory goes to disk (the service is stopping, or the server is out of reach). */
    fun spillAll() {
        val list = synchronized(mem) { mem.toList().also { mem.clear() } }
        for (e in list) runCatching { spill.put(e) }.onFailure { Log.w(TAG, "spill failed", it) }
        enforce(); publish()
    }

    fun clearSpill() { spill.clear(); publish() }

    /** Memory holds at most [MEM_EVENTS] events / [MEM_BYTES]; the oldest overflow to disk. */
    private fun trimMemory() {
        while (true) {
            val e = synchronized(mem) {
                if (mem.size <= MEM_EVENTS && mem.sumOf { it.bytes } <= MEM_BYTES) null else mem.removeFirstOrNull()
            } ?: break
            runCatching { spill.put(e) }
        }
        enforce()
    }

    private fun enforce() {
        val dropped = spill.enforce(settings.value.spillCapMb * 1024L * 1024L, MAX_AGE_MS, System.currentTimeMillis())
        if (dropped > 0) status.set { it.copy(dropped = it.dropped + dropped) }
    }

    private fun publish() {
        val (n, b) = synchronized(mem) { mem.size to mem.sumOf { it.bytes } }
        status.set { it.copy(memPending = n, memBytes = b, spillCount = spill.count(), spillBytes = spill.totalBytes()) }
    }

    private fun unmetered(): Boolean {
        val cm = ctx.getSystemService(ConnectivityManager::class.java) ?: return false
        return cm.activeNetwork != null && !cm.isActiveNetworkMetered
    }

    private fun online(): Boolean = ctx.getSystemService(ConnectivityManager::class.java)?.activeNetwork != null

    /** The next event allowed to go now: hotlist first (memory, then disk), then ordinary when the network allows. */
    private fun next(ordinaryAllowed: Boolean): Pair<PendingEvent, SpillQueue.Entry?>? {
        synchronized(mem) { mem.firstOrNull { it.hotlist }?.let { return it to null } }
        spill.list().firstOrNull { it.hotlist }?.let { en -> spill.load(en)?.let { return it to en } ?: spill.remove(en.id) }
        if (!ordinaryAllowed) return null
        synchronized(mem) { mem.firstOrNull()?.let { return it to null } }
        spill.list().firstOrNull()?.let { en -> spill.load(en)?.let { return it to en } ?: spill.remove(en.id) }
        return null
    }

    private fun done(e: PendingEvent, en: SpillQueue.Entry?) {
        if (en != null) spill.remove(en.id) else synchronized(mem) { mem.remove(e) }
    }

    /** Ordinary events that can't go now leave memory after [MEM_HOLD_MS]. */
    private fun spillStale(now: Long) {
        val stale = synchronized(mem) { mem.filter { now - it.createdMs > MEM_HOLD_MS }.also { mem.removeAll(it.toSet()) } }
        for (e in stale) runCatching { spill.put(e) }
        if (stale.isNotEmpty()) enforce()
    }

    private suspend fun run() {
        while (scope.isActive) {
            val now = System.currentTimeMillis()
            val paired = link.pairing.value.let { it.paired && !it.revoked }
            if (!paired || !online() || now < backoffUntil) {
                spillStale(now); publish()
                withTimeoutOrNull(if (now < backoffUntil) (backoffUntil - now).coerceAtMost(60_000) else 30_000) { wake.receive() }
                continue
            }
            // hits first: they are tiny and the most urgent
            val hit = synchronized(hits) { hits.firstOrNull() }
            if (hit != null) {
                when (val r = link.hit(hit)) {
                    is Call.Ok, is Call.Failed -> synchronized(hits) { hits.remove(hit) }
                    is Call.Revoked -> { revoked(); continue }
                    is Call.Busy -> { backoffUntil = now + r.retryAfterS * 1000L; continue }
                    is Call.Unreachable -> { unreachable(now); continue }
                }
                continue
            }
            val ordinaryAllowed = !settings.value.wifiOnlyBacklog || unmetered()
            val pick = next(ordinaryAllowed)
            if (pick == null) {
                spillStale(now); publish()
                withTimeoutOrNull(30_000) { wake.receive() }
                continue
            }
            val (e, en) = pick
            val r = link.uploadFrame(e)
            Log.i(TAG, "upload ${e.id} (${e.meta.reason}, ${e.jpeg.size} B): $r")
            when (r) {
                is Call.Ok -> {
                    done(e, en)
                    status.set { it.copy(uploaded = it.uploaded + 1, lastUploadMs = System.currentTimeMillis(), lastUploadError = "") }
                }
                is Call.Busy -> { backoffUntil = now + r.retryAfterS * 1000L; status.set { it.copy(lastUploadError = "server busy, retry in ${r.retryAfterS} s") } }
                is Call.Revoked -> revoked()
                is Call.Unreachable -> { unreachable(now); status.set { it.copy(lastUploadError = r.message) } }
                is Call.Failed -> {
                    status.set { it.copy(lastUploadError = "HTTP ${r.code}: ${r.message}") }
                    if (r.code in 400..499 && r.code != 408) { Log.w(TAG, "dropping ${e.id}: HTTP ${r.code}"); done(e, en); status.set { it.copy(dropped = it.dropped + 1) } }
                    else backoffUntil = now + 30_000
                }
            }
            publish()
        }
    }

    private fun unreachable(now: Long) {
        // a few seconds of grace, then nothing waits in memory for a server that isn't there
        lastUnreachable = now
        backoffUntil = now + 20_000
        spillAll()
    }

    private fun revoked() {
        spillAll()
        alerts.rePair()
        onRevoked?.invoke()
        backoffUntil = System.currentTimeMillis() + 3600_000
    }

    /** One pass for the background worker: try to drain the disk queue for up to [budgetMs]. */
    suspend fun drainOnce(budgetMs: Long = 120_000) {
        start(); kick()
        val until = System.currentTimeMillis() + budgetMs
        while (System.currentTimeMillis() < until && (spill.count() > 0 || synchronized(mem) { mem.isNotEmpty() })) {
            if (!link.pairing.value.paired || link.pairing.value.revoked) break
            if (System.currentTimeMillis() - lastUnreachable < 5_000) break
            if (settings.value.wifiOnlyBacklog && !unmetered() && spill.list().none { it.hotlist }) break
            delay(2_000)
        }
    }

    companion object {
        private const val TAG = "AlprUplink"
        const val MEM_EVENTS = 24
        const val MEM_BYTES = 32L * 1024 * 1024
        const val MEM_HOLD_MS = 60_000L
        const val MAX_AGE_MS = 24 * 3600_000L
    }
}
