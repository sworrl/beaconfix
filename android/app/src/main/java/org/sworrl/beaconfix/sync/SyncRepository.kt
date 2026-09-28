package org.sworrl.beaconfix.sync

import kotlinx.coroutines.flow.first
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonPrimitive
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.ApiOutcome
import org.sworrl.beaconfix.data.api.ObservationDto
import org.sworrl.beaconfix.data.api.ObservationsBody
import org.sworrl.beaconfix.data.api.SyncBody
import org.sworrl.beaconfix.data.api.outcome
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.data.db.ObservationEntity
import org.sworrl.beaconfix.estimate.EstimateRepository
import java.time.Instant
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import javax.inject.Inject
import javax.inject.Singleton

data class SyncReport(val desktop: String, val ok: Boolean, val pushed: Int = 0, val pulledAps: Int = 0, val pulledObs: Int = 0, val pulledFixes: Int = 0, val refit: Int = 0, val message: String = "")

/**
 * Two-way sync with every paired desktop.
 * Push: our unsynced observations, in body-size-limited batches (the desktop caps request bodies at 4 KB).
 * Pull: the desktop's beacons (positions, security, home flags), its location/track, its places, and — with the
 * control scope — the full database export (or, when the desktop advertises the "sync" feature, incremental changes).
 * Mirror: places, nearest help, trip, track, devices and home go to the offline cache (`DesktopCache`) as they arrive.
 * Merge rules: newest observation wins (dedupe by bssid+time+position); AP positions merge by better accuracy;
 * desktop fixes are stored once per timestamp; home networks follow [Mirror.homeStep].
 */
@Singleton
class SyncRepository @Inject constructor(
    private val db: AppDatabase,
    private val desktops: DesktopStore,
    private val estimates: EstimateRepository,
    private val prefs: Prefs,
    private val identity: org.sworrl.beaconfix.identity.IdentityStore,
    private val widgets: org.sworrl.beaconfix.widget.WidgetUpdater,
    private val anchors: org.sworrl.beaconfix.anchors.AnchorRepository,
    private val cache: DesktopCache,
) {
    suspend fun syncAll(): List<SyncReport> {
        val paired = desktops.paired()
        if (org.sworrl.beaconfix.data.DevFlags.desktopBlocked()) return paired.map { SyncReport(it.name, false, message = "offline (simulated)") }
        val out = ArrayList<SyncReport>()
        // the home list as it stands now: pushed where we may, never overwritten by a pull before that (Mirror.homeStep)
        val home = HomeRun(prefs.homeDirty.first(), prefs.homePatterns.first())
        for (d in paired) out += sync(d, home)
        if (home.pushed && Mirror.mayClearDirty(home.local, prefs.homePatterns.first())) prefs.setHomeDirty(false)
        runCatching { db.fixes().dedupeDesktop() }     // older builds stored the same desktop fix on every sync
        prefs.setLastSync(out.joinToString("\n") { "${it.desktop}: " + (if (it.ok) "pushed ${it.pushed}, pulled ${it.pulledAps} beacons / ${it.pulledObs} observations, refit ${it.refit}" else "failed — ${it.message}") })
        out.forEach { widgets.note(if (it.ok) "sync ${it.desktop}: pushed ${it.pushed}, pulled ${it.pulledAps}" else "sync ${it.desktop} failed: ${it.message.take(60)}") }
        widgets.touch("sync")
        return out
    }

    /** One sync run's view of the home-network list: [dirty] = edited on this phone since the last push. */
    class HomeRun(val dirty: Boolean, val local: Set<String>) { var pushed = false }

    suspend fun sync(desktop: DesktopEntity, home: HomeRun? = null): SyncReport {
        var d = desktop                                  // every upsert below builds on the newest row (scopes, cursor)
        val api = desktops.api(d)
        val auth = desktops.auth(d) ?: return fail(d, "not paired")
        var pushed = 0; var pulledAps = 0; var pulledObs = 0; var pulledFixes = 0
        try {
            val hello = api.hello()
            if (!hello.isSuccessful) return fail(d, "desktop unreachable (${hello.code()})")
            val features = hello.body()?.features ?: emptyList()
            // The scopes stored at pairing go stale: `beaconfix --grant-control` upgrades a token on the desktop. Ask it
            // what this token may do now (desktops with the "whoami" feature), so push, backup send and prefetch light up.
            if ("whoami" in features) runCatching { Mirror.bodyOf { api.me(auth) } }.getOrNull()?.let { me ->
                val sc = Mirror.scopesText(me.scopes)
                if (sc.isNotEmpty() && sc != d.scopes) { d = d.copy(scopes = sc); desktops.upsert(d) }
            }
            val canControl = desktops.hasScope(d, "control")

            // ── push ────────────────────────────────────────────────────────
            if (canControl) {
                while (true) {
                    val batch = db.observations().unsynced(200)
                    if (batch.isEmpty()) break
                    val chunk = sizeLimited(batch)
                    val myId = identity.currentNow()?.id
                    val body = ObservationsBody(chunk.map { ObservationDto(it.bssid, "", it.dbm, it.lat, it.lon, it.acc, iso(it.time), "android", myId) }, myId, identity.deviceName)
                    val r = api.pushObservations(auth, body)
                    when (r.outcome()) {
                        ApiOutcome.Ok -> { db.observations().markSynced(chunk.map { it.id }); pushed += chunk.size }
                        ApiOutcome.Unauthorized -> { desktops.forgetToken(d.id); return fail(d, "token rejected — pair again") }
                        ApiOutcome.RateLimited -> break
                        else -> return fail(d, "push failed: ${(r.outcome() as? ApiOutcome.Failed)?.message ?: r.code()}")
                    }
                }
            }

            // ── pull: beacons the desktop knows ──────────────────────────────
            val aps = api.aps(auth)
            when (aps.outcome()) {
                ApiOutcome.Ok -> {
                    val list = aps.body()?.aps ?: emptyList()
                    val now = System.currentTimeMillis()
                    for (a in list) {
                        if (a.bssid.length != 17) continue
                        val old = db.aps().get(a.bssid)
                        val hasPos = a.lat != null && a.lon != null && a.kind != "ring" && a.kind != "none" && a.kind != "mobile"
                        val theirAcc = a.r ?: 100.0
                        val fit = Mirror.fitOfPosition(a)       // the desktop's fit, when it is what placed this position
                        // compare like with like: R95 where both sides have one (graded fits), else the accuracy radius
                        val theirR95 = fit?.r95 ?: theirAcc * 2.45
                        val ourR95 = old?.r95 ?: (old?.acc ?: 1e9) * 2.45
                        val takePos = hasPos && old?.posSource != "anchor" && (old?.lat == null || old.posSource != "observed" || ourR95 > theirR95)
                        val base = (old ?: ApEntity(bssid = a.bssid, firstSeen = now)).copy(
                            ssid = a.ssid.ifEmpty { old?.ssid ?: "" }, freq = if (a.freq > 0) a.freq else old?.freq ?: 0, band = a.band.ifEmpty { old?.band ?: "" }, ch = if (a.ch > 0) a.ch else old?.ch ?: 0,
                            lastSeen = maxOf(old?.lastSeen ?: 0, now),
                            lat = if (takePos) a.lat else old?.lat, lon = if (takePos) a.lon else old?.lon, acc = if (takePos) theirAcc else old?.acc,
                            posSource = if (takePos) (if (a.kind == "wigle" || a.kind == "observed") "placed" else "desktop") else old?.posSource ?: "",
                            home = a.home || (old?.home ?: false), travelling = a.status == "travelling" || (old?.travelling ?: false),
                            security = a.security.ifEmpty { old?.security ?: "" }, rsnFlags = a.rsnFlags, wpaFlags = a.wpaFlags)
                        // the desktop's fit becomes the row's grade along with its position (none sent: the old grade no longer applies)
                        db.aps().upsert(if (takePos) Mirror.withDesktopFit(base, fit, now) else base)
                        pulledAps++
                    }
                }
                ApiOutcome.Unauthorized -> { desktops.forgetToken(d.id); return fail(d, "token rejected — pair again") }
                else -> {}
            }

            // ── anchors: push ours, merge theirs (desktops with the endpoints; older ones keep them local) ──
            if ("anchors" in features) {
                anchors.forgetNoEndpoint(d.id)
                if (canControl) runCatching { anchors.push() }
                runCatching { api.anchors(auth) }.getOrNull()?.takeIf { it.isSuccessful }?.body()?.let { anchors.mergeDtos(it) }
            }

            // ── home patterns: push the phone's edit (control scope), else pull (so the phone excludes the same networks) ──
            val src = DesktopCache.desktopSource(d.id)
            val run = home ?: HomeRun(prefs.homeDirty.first(), prefs.homePatterns.first())
            when (Mirror.homeStep(run.dirty, canControl)) {
                Mirror.HomeStep.PUSH -> {
                    val r = Mirror.rawPut(d, auth, "api/v1/home", Mirror.homeBody(run.local))
                    if (r.ok) { run.pushed = true; runCatching { cache.putSnapshot(src, Mirror.HOME, r.body) } }
                    if (home == null && r.ok && Mirror.mayClearDirty(run.local, prefs.homePatterns.first())) prefs.setHomeDirty(false)
                }
                Mirror.HomeStep.KEEP -> {}      // edited here, and this desktop may not take it: leave both lists alone
                Mirror.HomeStep.PULL -> Mirror.bodyOf { api.home(auth) }?.let { h ->
                    val mine = prefs.homePatterns.first()
                    val next = Mirror.pulled(mine, h.patterns)
                    if (next != mine) prefs.setHomePatterns(next)
                    runCatching { cache.putSnapshot(src, Mirror.HOME, cache.encode(h)) }
                }
            }

            // ── pull: the desktop's fix + track (stops) — each stored once (Mirror.newDesktopFixes) ──
            val loc = Mirror.bodyOf { api.location(auth) }
            val at = loc?.takeIf { it.valid }?.let { it.lat to it.lon } ?: (0.0 to 0.0)
            loc?.let { l ->
                Mirror.newLocationFix(l, { db.fixes().existsDesktopAt(it) })?.let { db.fixes().insert(it) }
                runCatching { cache.putSnapshot(src, Mirror.LOCATION, cache.encode(l), at.first, at.second) }
            }
            Mirror.rawGet(d, auth, "api/v1/track").takeIf { it.ok }?.let { r ->
                for (f in Mirror.newDesktopFixes(Mirror.trackPoints(r.body), { db.fixes().existsDesktopAt(it) })) { db.fixes().insert(f); pulledFixes++ }
                runCatching { cache.putSnapshot(src, Mirror.TRACK, r.body, at.first, at.second) }
            }
            // places → the offline cache (per desktop; an empty or failed answer keeps what is cached)
            runCatching { api.poisTyped(auth) }.getOrNull()?.body()?.let { p ->
                val origin = p.origin?.takeIf { it.lat != 0.0 || it.lon != 0.0 } ?: loc?.takeIf { it.valid }?.let { org.sworrl.beaconfix.data.api.OriginDto(lat = it.lat, lon = it.lon) }
                cache.saveDesktopPois(d.id, p.pois, origin?.lat ?: 0.0, origin?.lon ?: 0.0)
            }
            // nearest help, the trip and our other devices → snapshots, so Help, Trip and "where's the RV" work offline
            Mirror.bodyOf { api.emergency(auth) }?.let { e ->       // a 404 = a desktop older than /emergency: skipped
                val (la, lo) = Mirror.emergencyAt(e, loc)
                runCatching { cache.putSnapshot(src, Mirror.EMERGENCY, cache.encode(e), la, lo) }
            }
            Mirror.bodyOf { api.trip(auth) }?.let { t -> runCatching { cache.putSnapshot(src, Mirror.TRIP, cache.encode(t), at.first, at.second) } }
            Mirror.bodyOf { api.devicesPositions(auth) }?.let { dv -> runCatching { cache.putSnapshot(src, Mirror.DEVICES, cache.encode(dv), at.first, at.second) } }

            // ── pull: the observations behind those positions (control scope) ──
            if (canControl) {
                if ("sync" in features) {
                    val ch = api.changes(auth, d.cursor)
                    if (ch.isSuccessful) ch.body()?.let { c -> pulledObs += importObservations(c.observations); if (c.anchors.isNotEmpty()) anchors.merge(c.anchors); d = d.copy(cursor = c.cursor); desktops.upsert(d) }
                } else if (d.pulledAps == 0L || System.currentTimeMillis() - d.lastSync > 6 * 3600_000L) {
                    // full export: streamed, parsed once; cheap enough at a few MB and only every 6 h
                    val ex = api.export(auth)
                    if (ex.isSuccessful) ex.body()?.use { body ->
                        val dump = ApiFactory.json.decodeFromString(org.sworrl.beaconfix.data.api.ExportDto.serializer(), body.string())
                        pulledObs += importObservations(dump.observations)
                    }
                }
            }
            val touched = db.observations().touchedSince(0).take(400)   // refit what we have data for (bounded)
            val refit = estimates.refit(touched, force = true)
            runCatching { anchors.applyToAps() }
            desktops.upsert(d.copy(lastSync = System.currentTimeMillis(), lastError = "", pushedObs = d.pushedObs + pushed, pulledAps = d.pulledAps + pulledAps, hostname = hello.body()?.hostname ?: d.hostname, version = hello.body()?.version ?: d.version))
            return SyncReport(d.name, true, pushed, pulledAps, pulledObs, pulledFixes, refit)
        } catch (e: Exception) {
            return fail(d, e.message ?: e.toString())
        }
    }

    /** Desktop rows → local observations, skipping duplicates; returns the number added. */
    suspend fun importObservations(rows: List<JsonObject>): Int {
        var n = 0
        val batch = ArrayList<ObservationEntity>()
        for (o in rows) {
            val bssid = o.str("bssid")?.uppercase() ?: continue
            val lat = o.num("lat") ?: continue; val lon = o.num("lon") ?: continue
            val acc = o.num("acc") ?: 100.0; if (acc <= 0 || acc > 2000) continue
            val t = o.str("time")?.let { parseIso(it) } ?: continue
            if (db.observations().duplicates(bssid, t, lat, lon) > 0) continue
            batch += ObservationEntity(bssid = bssid, time = t, lat = lat, lon = lon, acc = acc, dbm = o.int("dbm") ?: -80, source = o.str("fix_source") ?: o.str("source") ?: "desktop", synced = true, remote = true)
            if (batch.size >= 500) { n += db.observations().insertAll(batch).count { it > 0 }; batch.clear() }
        }
        if (batch.isNotEmpty()) n += db.observations().insertAll(batch).count { it > 0 }
        return n
    }

    private suspend fun fail(d: DesktopEntity, msg: String): SyncReport { desktops.upsert(d.copy(lastError = msg)); return SyncReport(d.name, false, message = msg) }

    companion object {
        /** The desktop refuses bodies over 4 KB: ~28 observations per request. */
        fun sizeLimited(rows: List<ObservationEntity>, limitBytes: Int = 3600): List<ObservationEntity> {
            var size = 30; val out = ArrayList<ObservationEntity>()
            for (r in rows) { size += 120; if (size > limitBytes) break; out += r }
            return out.ifEmpty { rows.take(1) }
        }
        private val fmt: DateTimeFormatter = DateTimeFormatter.ISO_LOCAL_DATE_TIME
        fun iso(ms: Long): String = LocalDateTime.ofInstant(Instant.ofEpochMilli(ms), ZoneId.systemDefault()).withNano(0).format(fmt)
        fun parseIso(s: String): Long = runCatching { LocalDateTime.parse(s.take(19), fmt).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli() }.getOrElse { System.currentTimeMillis() }
        fun JsonObject.str(k: String) = this[k]?.jsonPrimitive?.contentOrNull
        fun JsonObject.num(k: String) = this[k]?.jsonPrimitive?.doubleOrNull
        fun JsonObject.int(k: String) = this[k]?.jsonPrimitive?.intOrNull
        fun JsonObject.long(k: String) = this[k]?.jsonPrimitive?.contentOrNull?.toLongOrNull()
        @Suppress("unused") fun JsonObject.bool(k: String) = this[k]?.jsonPrimitive?.booleanOrNull
    }
}
