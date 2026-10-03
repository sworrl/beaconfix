package org.sworrl.beaconfix.sync

import kotlinx.coroutines.CancellationException
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.sworrl.beaconfix.anchors.AnchorRepository
import org.sworrl.beaconfix.data.api.ApDto
import org.sworrl.beaconfix.data.api.ApFitDto
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.FixDto
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.estimate.EstimateRepository
import org.sworrl.beaconfix.identity.IdentityStore
import org.sworrl.beaconfix.net.HubApi
import org.sworrl.beaconfix.net.HubClient
import org.sworrl.beaconfix.net.HubErrors
import org.sworrl.beaconfix.net.HubStore
import org.sworrl.beaconfix.net.HubSyncBody
import org.sworrl.beaconfix.sightings.PlateDest
import org.sworrl.beaconfix.sightings.PlateEventRepository
import org.sworrl.beaconfix.data.db.PlateEventEntity
import org.sworrl.beaconfix.sync.SyncRepository.Companion.int
import org.sworrl.beaconfix.sync.SyncRepository.Companion.num
import org.sworrl.beaconfix.sync.SyncRepository.Companion.str
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Two-way sync with the hub (the master database), over BFS3 — the primary target whenever the phone is enrolled.
 * Push (`POST /db/sync`): unsynced observations (with Wi-Fi RTT `rangeM`/`rangeSd`), then phone fixes after the hub
 * fix cursor, then dirty anchors; batches start at [BATCH] rows and halve on `413`. Pull (`GET /db/changes`): every
 * node's beacons, observations and anchors after the stored cursor, [PAGE] rows per table per page, at most
 * [MAX_PAGES] pages a run (a first sync of a large hub continues on the next run). The cursor and the fix cursor are
 * stored after every accepted page, so an interrupted run never pulls or pushes the same rows twice. What could not
 * be sent stays queued (`synced = 0`) for the next run. Each run starts with the node heartbeat (capabilities).
 * Plate events ride along as records ([HubPlates]): pushed with their own hub flag, pulled from the same feed with their
 * own cursor, merged and announced exactly as the LAN feed's; their images still wait for a LAN desktop.
 * Later, leasing light hub jobs (docs/HUB.md) plugs in after the pull, through [org.sworrl.beaconfix.net.JobRunner].
 */
@Singleton
class HubSync @Inject constructor(
    private val db: AppDatabase,
    private val hub: HubClient,
    private val store: HubStore,
    private val estimates: EstimateRepository,
    private val anchors: AnchorRepository,
    private val identity: IdentityStore,
    private val repo: dagger.Lazy<SyncRepository>,
    private val node: org.sworrl.beaconfix.net.HubNode,
    private val plates: PlateEventRepository,
) {
    fun enrolled() = hub.enrolled()

    private class Refused(msg: String, val unreachable: Boolean = false) : Exception(msg)

    suspend fun sync(): SyncReport {
        val label = "hub"
        val api = hub.hubApi() ?: return SyncReport(label, false, message = "not enrolled with a hub")
        val name = store.config()?.name ?: identity.deviceName
        var pushed = 0; var pushedFixes = 0; var pulledAps = 0; var pulledObs = 0
        var pushedPlates = 0; var pulledPlates = 0
        val freshPlates = ArrayList<PlateEventEntity>()
        try {
            // the node registry first (docs/HUB.md): what this phone is and can do; a hub without the route answers 404
            node.heartbeat()
            val myId = identity.currentNow()?.id
            suspend fun send(obs: List<org.sworrl.beaconfix.data.api.ObservationDto>, fixes: List<FixDto>, anch: List<JsonObject>?): Int =
                post(api, HubSyncBody(name, HubClient.KIND, myId, obs, fixes, anch)).first

            // ── push: observations ─────────────────────────────────────────
            var batch = BATCH
            for (round in 0 until 400) {
                val rows = db.observations().unsynced(batch)
                if (rows.isEmpty()) break
                when (send(rows.map { SyncRepository.dtoOf(it, myId) }, emptyList(), null)) {
                    200 -> { db.observations().markSynced(rows.map { it.id }); pushed += rows.size }
                    else -> { if (batch <= MIN_BATCH) throw Refused("hub: request too large"); batch /= 2 }
                }
            }
            // ── push: this phone's fixes (its track) ───────────────────────
            runCatching { db.fixes().backfillFromObservations() }
            batch = BATCH
            for (round in 0 until 400) {
                val rows = db.fixes().phoneFixesSince(store.fixCursor(), batch)
                if (rows.isEmpty()) break
                val dtos = rows.map { FixDto(it.lat, it.lon, it.acc, SyncRepository.iso(it.time), it.source, it.provider, it.place) }
                when (send(emptyList(), dtos, null)) {
                    200 -> { store.setFixCursor(rows.last().time); pushedFixes += rows.size }
                    else -> { if (batch <= MIN_BATCH) throw Refused("hub: request too large"); batch /= 2 }
                }
                if (rows.size < batch) break
            }
            // ── push: anchors placed / moved / deleted here ────────────────
            val dirty = db.anchors().dirty()
            if (dirty.isNotEmpty()) {
                val rows = dirty.mapNotNull { runCatching { ApiFactory.json.parseToJsonElement(it.json).jsonObject }.getOrNull() }
                if (send(emptyList(), emptyList(), rows) == 200) db.anchors().clean(dirty.map { it.id })
            }
            // ── push: plate events (records only; their images wait for a LAN desktop) ──
            pushedPlates = pushPlates(api, name, myId)

            // ── pull: everyone's changes after our cursor ──────────────────
            for (page in 0 until MAX_PAGES) {
                val since = store.pullCursor()
                val r = api.changes(since, PAGE)
                if (!r.isSuccessful) throw Refused(HubErrors.ofStatus(r.code(), hub.clockSkewS))
                val c = r.body() ?: break
                // plate events: only while their cursor is not behind this page (else the catch-up below fetches them)
                val plateStep = HubPlates.inStep(store.plateCursor(), HubPlates.cursorLong(since))
                if (plateStep && c.plateEvents.isNotEmpty()) {
                    freshPlates += plates.ingest(HubPlates.decode(c.plateEvents), PlateDest.HUB); pulledPlates += c.plateEvents.size
                }
                pulledObs += repo.get().importObservations(c.observations)
                pulledAps += repo.get().mergeAps(c.aps.mapNotNull { apOfChange(it) }, heardNow = false)
                if (c.anchors.isNotEmpty()) anchors.merge(c.anchors)
                val next = cursorText(c.cursor)
                if (next.isNotEmpty()) store.setPullCursor(next)
                if (plateStep) HubPlates.follow(store.plateCursor(), HubPlates.cursorLong(next))?.let { store.setPlateCursor(it) }
                if (!c.more || next.isEmpty() || next == since) break
            }
            pulledPlates += catchUpPlates(freshPlates)
            runCatching { plates.announce(freshPlates) }
            freshPlates.clear()                          // announced (or given up on): never twice

            val touched = db.observations().touchedSince(0).take(400)
            val refit = if (pulledObs > 0 || pushed > 0) estimates.refit(touched, force = true) else 0
            runCatching { anchors.applyToAps() }
            val platesText = if (pushedPlates + pulledPlates > 0) " · plate events: pushed $pushedPlates, pulled $pulledPlates" else ""
            val text = "pushed $pushed observations, $pushedFixes fixes · pulled $pulledAps beacons, $pulledObs observations · refit $refit$platesText"
            store.noteSync(text)
            return SyncReport(label, true, pushed, pulledAps, pulledObs, pushedFixes, refit, text)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Refused) {
            if (freshPlates.isNotEmpty()) runCatching { plates.announce(freshPlates) }
            store.noteError(e.message ?: "refused", e.unreachable)
            return SyncReport(label, false, pushed, pulledAps, pulledObs, pushedFixes, message = e.message ?: "refused")
        } catch (e: Exception) {
            if (freshPlates.isNotEmpty()) runCatching { plates.announce(freshPlates) }
            val msg = HubErrors.describe(e)
            store.noteError(msg, HubErrors.isUnreachable(e))
            return SyncReport(label, false, pushed, pulledAps, pulledObs, pushedFixes, message = msg)
        }
    }

    /** `POST db/sync` → (200 | 413, the answer). Anything else is a refusal. */
    private suspend fun post(api: HubApi, body: HubSyncBody): Pair<Int, JsonObject?> {
        val r = api.sync(body)
        if (r.isSuccessful) { store.noteContact(); return 200 to r.body() }
        if (r.code() == 413) return 413 to null
        throw Refused(HubErrors.ofStatus(r.code(), hub.clockSkewS))
    }

    /**
     * Push the plate events waiting for the hub ([HubPlates]): batches of records, halved on `413` (a single record
     * the hub will not take for its size waits for the desktop); a short count is resolved one by one. Returns how
     * many the hub took. An older hub that does not report plate events takes none: they stay queued.
     */
    private suspend fun pushPlates(api: HubApi, name: String, myId: String?): Int {
        var maxBytes = HubPlates.MAX_BYTES
        val skip = HashSet<String>()
        var pushed = 0
        for (round in 0 until 100) {
            val batch = plates.hubPending(maxBytes, skip)
            if (batch.isEmpty()) break
            val uids = batch.map { it.uid }
            val (code, answer) = post(api, HubPlates.body(name, myId, batch))
            if (code == 413) { if (batch.size == 1) skip += uids else maxBytes /= 2; continue }
            var o = HubPlates.outcome(uids, answer)
            if (o.unsupported) break
            if (o.split) {
                o = HubPlates.afterSplit(batch.map { ev ->
                    val (c1, a1) = post(api, HubPlates.body(name, myId, listOf(ev)))
                    ev.uid to if (c1 == 413) HubPlates.Outcome(refused = listOf(ev.uid)) else HubPlates.outcome(listOf(ev.uid), a1)
                })
                if (o.pushed.isEmpty()) break            // the hub stores none of them now: all stay queued
            }
            plates.markHubPushed(o.pushed + o.dropped, o.renames)
            pushed += o.pushed.size
            skip += o.refused
        }
        return pushed
    }

    /** Push only the plate events (after a live pass or a plate-search hit, without waiting for the next full sync). */
    suspend fun pushPlateEvents(): Int {
        val api = hub.hubApi() ?: return 0
        val name = store.config()?.name ?: identity.deviceName
        return try { pushPlates(api, name, identity.currentNow()?.id) }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { if (e !is Refused) store.noteError(HubErrors.describe(e), HubErrors.isUnreachable(e)); 0 }
    }

    /**
     * The plate cursor lags the feed (enrolled before the feed carried plate events, or a page whose plate events
     * failed to land): fetch the plate-only feed from it up to now, then let it follow the feed again. A hub without
     * that route leaves it for the next run. Returns the events read.
     */
    private suspend fun catchUpPlates(fresh: MutableList<PlateEventEntity>): Int {
        val main = HubPlates.cursorLong(store.pullCursor()) ?: return 0
        if (!HubPlates.behind(store.plateCursor(), main)) return 0
        val api = hub.api() ?: return 0
        var since = store.plateCursor() ?: 0L
        var n = 0
        try {
            for (page in 0 until HubPlates.MAX_CATCH_UP_PAGES) {
                val r = api.plateEvents(HubClient.AUTH, since, HubPlates.PAGE)
                if (!r.isSuccessful) break
                val body = r.body() ?: break
                fresh += plates.ingest(body.events, PlateDest.HUB); n += body.events.size
                val (next, more) = HubPlates.catchUp(since, HubPlates.pageCursor(body, since), body.more, main)
                if (next != since || store.plateCursor() == null) store.setPlateCursor(next)
                since = next
                if (!more) break
            }
        } catch (e: CancellationException) { throw e } catch (e: Exception) { /* the next run continues from the stored cursor */ }
        return n
    }

    companion object {
        const val BATCH = 500
        const val MIN_BATCH = 25
        const val PAGE = 1000
        const val MAX_PAGES = 20

        /** The feed's cursor as the hub sent it (a JSON number; "12345.0" → "12345"). */
        fun cursorText(c: String): String = c.trim().let { t -> t.toDoubleOrNull()?.takeIf { it >= 0 && it == Math.floor(it) && it < 9.0e15 }?.toLong()?.toString() ?: t }

        /**
         * A `/db/changes` beacon row (`bssid, ssid, freq, lat?, lon?, acc?, source, home, travelling, security, fit?`) in
         * the shape of an `/aps` entry, so the same merge applies: `acc` → `r`, `source` → `kind`.
         */
        fun apOfChange(o: JsonObject): ApDto? {
            val bssid = o.str("bssid")?.uppercase() ?: return null
            if (bssid.length != 17) return null
            val fit = o["fit"]?.let { f -> runCatching { ApiFactory.json.decodeFromJsonElement(ApFitDto.serializer(), f) }.getOrNull() }
            return ApDto(bssid = bssid, ssid = o.str("ssid") ?: "", freq = o.int("freq") ?: 0, kind = o.str("source") ?: "",
                lat = o.num("lat"), lon = o.num("lon"), r = o.num("acc"), status = if (o["travelling"]?.jsonPrimitive?.booleanOrNull == true) "travelling" else "",
                security = o.str("security") ?: "", home = o["home"]?.jsonPrimitive?.booleanOrNull == true, fit = fit)
        }
    }
}
