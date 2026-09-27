package org.sworrl.beaconfix.sync

import android.os.Build
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.Hello
import org.sworrl.beaconfix.data.api.PairRequest
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.ranging.RangeSession
import javax.inject.Inject
import javax.inject.Singleton

sealed class PairState {
    data object Idle : PairState()
    data class Probing(val host: String) : PairState()
    data class Found(val hello: Hello, val desktop: DesktopEntity) : PairState()
    data class WaitingApproval(val desktop: DesktopEntity, val code: String, val id: String, val polls: Int, val pictures: List<Int> = emptyList(), val proximity: org.sworrl.beaconfix.data.api.ProximityAnswer? = null, val picked: Boolean? = null) : PairState()
    data class Paired(val desktop: DesktopEntity) : PairState()
    data class Failed(val message: String) : PairState()
}

@Singleton
class PairingRepository @Inject constructor(private val store: DesktopStore, private val identity: org.sworrl.beaconfix.identity.IdentityRepository, private val identities: org.sworrl.beaconfix.identity.IdentityStore,
                                            private val scanner: org.sworrl.beaconfix.collector.WifiScanner, private val location: org.sworrl.beaconfix.collector.LocationSource, private val prefs: org.sworrl.beaconfix.data.Prefs, private val db: org.sworrl.beaconfix.data.db.AppDatabase,
                                            private val ranging: org.sworrl.beaconfix.ranging.RangingRepository) {
    @Volatile private var cancelId: Pair<DesktopEntity, String>? = null
    /** The proximity block: our fix and the strongest non-home beacons from a fresh scan. */
    private suspend fun proximity(d: DesktopEntity? = null): org.sworrl.beaconfix.data.api.Proximity {
        val ranged = d?.let { ranging.ranged(it.id)?.best }
        val scan = runCatching { scanner.scan(8000) }.getOrDefault(emptyList())
        val home = prefs.homePatterns.first()
        val beacons = scan.filter { !org.sworrl.beaconfix.collector.ObservationRecorder.isHome(it, home) }.sortedByDescending { it.dbm }.take(30).map { org.sworrl.beaconfix.data.api.ProxBeacon(it.bssid.lowercase(), it.dbm) }
        val loc = runCatching { location.current(6000) }.getOrNull()
        val fix = db.fixes().latestPhone()
        val p = when {
            loc != null -> org.sworrl.beaconfix.data.api.Proximity(loc.latitude, loc.longitude, loc.accuracy.toDouble(), SyncRepository.iso(System.currentTimeMillis()), "gps", beacons)
            fix != null -> org.sworrl.beaconfix.data.api.Proximity(fix.lat, fix.lon, fix.acc, SyncRepository.iso(fix.time), if (fix.source == "phone-wifi") "wifi" else "gps", beacons)
            else -> org.sworrl.beaconfix.data.api.Proximity(beacons = beacons)
        }
        return if (ranged != null) p.copy(rangedM = ranged.distanceM, rangedSigmaM = ranged.sigmaM, rangedMethod = RangeSession.methodName(ranged.method)) else p
    }
    /** Our own measurement overrides an "unknown"/"far" verdict: adjacent when ranged under 2 m, room under 6 m (docs/RANGING.md §5.7). */
    private fun withRange(a: org.sworrl.beaconfix.data.api.ProximityAnswer?, d: DesktopEntity): org.sworrl.beaconfix.data.api.ProximityAnswer? {
        val r = ranging.ranged(d.id)?.best ?: return a
        val cls = when { r.highM <= 2.0 -> "adjacent"; r.highM <= 6.0 -> "room"; else -> return a?.copy(distanceM = a.distanceM ?: r.distanceM) }
        return (a ?: org.sworrl.beaconfix.data.api.ProximityAnswer()).copy(distanceM = r.distanceM, verdict = cls)
    }
    suspend fun cancel() { cancelId?.let { (d, id) -> runCatching { store.api(d).pairCancel(id) } }; cancelId = null }
    /** Challenge/auth with our identity when the desktop supports it; null = not attempted (fall back to the pairing code). */
    suspend fun loginWithIdentity(d: DesktopEntity, hello: Hello): PairState? {
        if ("identity" !in hello.features || !identities.exists()) return null
        return when (val r = identity.login(d)) {
            is org.sworrl.beaconfix.identity.LoginResult.Ok -> PairState.Paired(r.desktop)
            is org.sworrl.beaconfix.identity.LoginResult.UnknownIdentity -> PairState.Failed(r.message)
            is org.sworrl.beaconfix.identity.LoginResult.Failed -> null
        }
    }

    suspend fun probe(host: String, port: Int, tls: Boolean): PairState {
        val id = "$host:$port"
        return try {
            val r = ApiFactory.create(host, port, tls).hello()
            val h = r.body() ?: return PairState.Failed("no BeaconFix at $host:$port (HTTP ${r.code()})")
            val d = (store.get(id) ?: DesktopEntity(id = id, host = host, port = port, name = h.hostname.ifEmpty { host }))
                .copy(hostname = h.hostname, version = h.version, tls = h.tls, lastSeen = System.currentTimeMillis())
            store.upsert(d)
            PairState.Found(h, d)
        } catch (e: Exception) { PairState.Failed("cannot reach $host:$port — ${e.message}") }
    }

    /** POST /pair then poll until approved/denied. Emits progress through [onState]. */
    suspend fun pair(d: DesktopEntity, onState: (PairState) -> Unit): PairState {
        val api = store.api(d)
        val name = (Build.MANUFACTURER + " " + Build.MODEL).trim().ifEmpty { "Android phone" }
        val eph = Sas.ephemeral()
        val me = identities.currentNow()
        val req = PairRequest(name, listOf("read", "control"), "android", me?.let { org.sworrl.beaconfix.data.api.PairIdentity(it.id, it.pub) }, org.sworrl.beaconfix.data.api.SasPub(org.sworrl.beaconfix.identity.Crypto.b64(eph.pub)), proximity(d))
        val started = try { api.pair(req) } catch (e: Exception) { return PairState.Failed(e.message ?: "pair failed") }
        if (started.code() == 403) return PairState.Failed("pairing is closed on the desktop — open it there (Devices tab, tray menu, or `beaconfix --pairing 10`) and try again")
        if (started.code() == 429) return PairState.Failed("the desktop has too many pending requests")
        val p = started.body() ?: return PairState.Failed("pair failed (HTTP ${started.code()})")
        // v2: the desktop answered with its ephemeral key → derive the three pictures; without it the code flow stays
        val pictures = p.sas?.pub?.takeIf { it.isNotEmpty() }?.let { runCatching { Sas.pictures(eph.priv, org.sworrl.beaconfix.identity.Crypto.unb64(it), p.id) }.getOrNull() } ?: emptyList()
        var prox = withRange(p.proximity, d)
        var polls = 0
        cancelId = d to p.id
        onState(PairState.WaitingApproval(d, p.code, p.id, 0, pictures, prox))
        while (polls < 150) {                                   // ~12 min
            delay(if (polls == 0) 1500 else 5000)
            polls++
            val s = try { api.pairStatus(p.id) } catch (e: Exception) { onState(PairState.WaitingApproval(d, p.code, p.id, polls)); continue }
            if (s.code() == 404) return PairState.Failed("the request expired — pair again")
            val body = s.body() ?: continue
            if (body.proximity != null) prox = withRange(body.proximity, d)
            when (body.status) {
                "approved" -> {
                    val token = body.token ?: return PairState.Failed("approved, but the token was already collected — revoke on the desktop and pair again")
                    cancelId = null
                    store.saveToken(d.id, token)
                    val paired = d.copy(paired = true, scopes = body.scopes.joinToString(","), lastError = "", lastSeen = System.currentTimeMillis())
                    store.upsert(paired)
                    return PairState.Paired(paired)
                }
                "denied" -> { cancelId = null; return PairState.Failed(if (body.sas?.picked == false) "wrong pictures were picked on the desktop — someone else may be pairing; try again next to it" else "the desktop denied this device") }
                else -> onState(PairState.WaitingApproval(d, p.code, p.id, polls, pictures, prox, body.sas?.picked))
            }
        }
        cancelId = null
        return PairState.Failed("timed out waiting for approval")
    }
}
