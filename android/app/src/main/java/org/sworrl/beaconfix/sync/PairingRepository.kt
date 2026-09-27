package org.sworrl.beaconfix.sync

import android.os.Build
import kotlinx.coroutines.delay
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.Hello
import org.sworrl.beaconfix.data.api.PairRequest
import org.sworrl.beaconfix.data.db.DesktopEntity
import javax.inject.Inject
import javax.inject.Singleton

sealed class PairState {
    data object Idle : PairState()
    data class Probing(val host: String) : PairState()
    data class Found(val hello: Hello, val desktop: DesktopEntity) : PairState()
    data class WaitingApproval(val desktop: DesktopEntity, val code: String, val id: String, val polls: Int) : PairState()
    data class Paired(val desktop: DesktopEntity) : PairState()
    data class Failed(val message: String) : PairState()
}

@Singleton
class PairingRepository @Inject constructor(private val store: DesktopStore) {
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
        val started = try { api.pair(PairRequest(name, listOf("read", "control"))) } catch (e: Exception) { return PairState.Failed(e.message ?: "pair failed") }
        if (started.code() == 403) return PairState.Failed("pairing is closed on the desktop — open it there (Devices tab, tray menu, or `beaconfix --pairing 10`) and try again")
        if (started.code() == 429) return PairState.Failed("the desktop has too many pending requests")
        val p = started.body() ?: return PairState.Failed("pair failed (HTTP ${started.code()})")
        var polls = 0
        onState(PairState.WaitingApproval(d, p.code, p.id, 0))
        while (polls < 150) {                                   // ~12 min
            delay(if (polls == 0) 1500 else 5000)
            polls++
            val s = try { api.pairStatus(p.id) } catch (e: Exception) { onState(PairState.WaitingApproval(d, p.code, p.id, polls)); continue }
            if (s.code() == 404) return PairState.Failed("the request expired — pair again")
            val body = s.body() ?: continue
            when (body.status) {
                "approved" -> {
                    val token = body.token ?: return PairState.Failed("approved, but the token was already collected — revoke on the desktop and pair again")
                    store.saveToken(d.id, token)
                    val paired = d.copy(paired = true, scopes = body.scopes.joinToString(","), lastError = "", lastSeen = System.currentTimeMillis())
                    store.upsert(paired)
                    return PairState.Paired(paired)
                }
                "denied" -> return PairState.Failed("the desktop denied this device")
                else -> onState(PairState.WaitingApproval(d, p.code, p.id, polls))
            }
        }
        return PairState.Failed("timed out waiting for approval")
    }
}
