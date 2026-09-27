package org.sworrl.beaconfix.sync

import kotlinx.coroutines.flow.first
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonPrimitive
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
import org.sworrl.beaconfix.data.db.FixEntity
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
 * Merge rules: newest observation wins (dedupe by bssid+time+position); AP positions merge by better accuracy.
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
    private val cache: org.sworrl.beaconfix.data.DesktopCache,
) {
    suspend fun syncAll(): List<SyncReport> {
        val out = ArrayList<SyncReport>()
        for (d in desktops.paired()) out += sync(d)
        prefs.setLastSync(out.joinToString("\n") { "${it.desktop}: " + (if (it.ok) "pushed ${it.pushed}, pulled ${it.pulledAps} beacons / ${it.pulledObs} observations, refit ${it.refit}" else "failed — ${it.message}") })
        out.forEach { widgets.note(if (it.ok) "sync ${it.desktop}: pushed ${it.pushed}, pulled ${it.pulledAps}" else "sync ${it.desktop} failed: ${it.message.take(60)}") }
        widgets.touch("sync")
        return out
    }

    suspend fun sync(d: DesktopEntity): SyncReport {
        val api = desktops.api(d)
        val auth = desktops.auth(d) ?: return fail(d, "not paired")
        var pushed = 0; var pulledAps = 0; var pulledObs = 0; var pulledFixes = 0
        try {
            val hello = api.hello()
            if (!hello.isSuccessful) return fail(d, "desktop unreachable (${hello.code()})")
            val features = hello.body()?.features ?: emptyList()
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
                        val hasPos = a.lat != null && a.lon != null && a.kind != "ring" && a.kind != "none"
                        val theirAcc = a.r ?: 100.0
                        val takePos = hasPos && old?.posSource != "anchor" && (old?.lat == null || old.posSource != "observed" || (old.acc ?: 1e9) > theirAcc)
                        db.aps().upsert((old ?: ApEntity(bssid = a.bssid, firstSeen = now)).copy(
                            ssid = a.ssid.ifEmpty { old?.ssid ?: "" }, freq = if (a.freq > 0) a.freq else old?.freq ?: 0, band = a.band.ifEmpty { old?.band ?: "" }, ch = if (a.ch > 0) a.ch else old?.ch ?: 0,
                            lastSeen = maxOf(old?.lastSeen ?: 0, now),
                            lat = if (takePos) a.lat else old?.lat, lon = if (takePos) a.lon else old?.lon, acc = if (takePos) theirAcc else old?.acc,
                            posSource = if (takePos) (if (a.kind == "wigle" || a.kind == "observed") "placed" else "desktop") else old?.posSource ?: "",
                            home = a.home || (old?.home ?: false), travelling = a.status == "travelling" || (old?.travelling ?: false),
                            security = a.security.ifEmpty { old?.security ?: "" }, rsnFlags = a.rsnFlags, wpaFlags = a.wpaFlags))
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

            // ── pull: home patterns (so the phone excludes the same networks) ──
            api.home(auth).body()?.let { h -> if (h.patterns.isNotEmpty()) prefs.setHomePatterns(h.patterns.toSet()) }

            // ── pull: the desktop's fix + track ──────────────────────────────
            val loc = api.location(auth).body()
            loc?.let { l -> if (l.valid) db.fixes().insert(FixEntity(time = parseIso(l.time), lat = l.lat, lon = l.lon, acc = l.accuracy, source = "desktop", provider = l.provider, place = l.place)) }
            api.track(auth).body()?.track?.let { t ->
                val have = HashSet<Long>()
                for (p in t) { val ts = parseIso(p.time); if (have.add(ts)) db.fixes().insert(FixEntity(time = ts, lat = p.lat, lon = p.lon, acc = p.acc, source = "desktop", provider = p.source, place = p.place)); pulledFixes++ }
            }
            // places → the offline cache (per desktop; an empty or failed answer keeps what is cached)
            runCatching { api.poisTyped(auth) }.getOrNull()?.body()?.let { p ->
                val origin = p.origin ?: loc?.takeIf { it.valid }?.let { org.sworrl.beaconfix.data.api.OriginDto(lat = it.lat, lon = it.lon) }
                cache.saveDesktopPois(d.id, p.pois, origin?.lat ?: 0.0, origin?.lon ?: 0.0)
            }

            // ── pull: the observations behind those positions (control scope) ──
            if (canControl) {
                if ("sync" in features) {
                    val ch = api.changes(auth, d.cursor)
                    if (ch.isSuccessful) ch.body()?.let { c -> pulledObs += importObservations(c.observations); if (c.anchors.isNotEmpty()) anchors.merge(c.anchors); desktops.upsert(d.copy(cursor = c.cursor)) }
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
            val refit = estimates.refit(touched)
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
