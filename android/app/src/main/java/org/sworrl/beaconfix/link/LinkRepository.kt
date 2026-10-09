package org.sworrl.beaconfix.link

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.joinAll
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import okhttp3.Call
import okhttp3.Callback
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.DiscoveredDesktop
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.Hello
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.identity.IdentityStore
import org.sworrl.beaconfix.net.Bfs3
import org.sworrl.beaconfix.net.Bfs3Exception
import org.sworrl.beaconfix.net.HubClient
import org.sworrl.beaconfix.net.HubErrors
import org.sworrl.beaconfix.net.HubSecurityException
import org.sworrl.beaconfix.net.HubStore
import org.sworrl.beaconfix.net.HubUrls
import org.sworrl.beaconfix.sync.SyncScheduler
import java.io.IOException
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/** Where a link stands (one at a time). */
sealed class LinkPhase {
    data object Idle : LinkPhase()
    data class Contacting(val pc: String, val note: String) : LinkPhase()
    /** Both screens show [code]. QR path: the PC approves by itself; mDNS path: the user taps Link on the PC. */
    data class Confirm(val pc: String, val code: String, val viaQr: Boolean, val secondsLeft: Int) : LinkPhase()
    data class Linked(val pc: String, val code: String, val desktopId: String) : LinkPhase()
    data class Failed(val pc: String, val message: String) : LinkPhase()
}

/** The hub half of a link. */
data class HubLinkState(val phase: Phase = Phase.IDLE, val text: String = "", val fingerprint: String = "") {
    enum class Phase { IDLE, ENROLLING, ENROLLED, WAITING, FAILED }
}

/** One answer to `GET /api/v1/hello` on one of a PC's addresses. */
data class Reach(val host: String, val port: Int, val rttMs: Long, val hello: Hello)

/**
 * Linking v3 on the phone (docs/LINKING.md): scan the PC's `bflink:` QR or tap it in the mDNS list → the PC's token
 * arrives sealed to the session key → it is stored exactly like a paired desktop (every LAN feature uses it as before)
 * → the hub invite in the same payload enrols this phone with the hub, without a fingerprint prompt (the invite came
 * over the authenticated link). An unreachable hub (WireGuard off) keeps the invite and retries in the background,
 * renewing it from the PC (`POST /api/v1/link/hub-invite`) once it has expired.
 */
@Singleton
class LinkRepository @Inject constructor(
    @ApplicationContext private val ctx: Context,
    private val desktops: DesktopStore,
    private val links: LinkStore,
    private val identity: IdentityStore,
    private val hub: HubClient,
    private val hubStore: HubStore,
    private val syncScheduler: SyncScheduler,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val _phase = MutableStateFlow<LinkPhase>(LinkPhase.Idle)
    val phase: StateFlow<LinkPhase> = _phase
    private val _hub = MutableStateFlow(HubLinkState())
    val hubState: StateFlow<HubLinkState> = _hub
    private var job: Job? = null
    private val hubLock = Mutex()
    private var retryJob: Job? = null
    @Volatile private var started = false

    private val http: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(4, TimeUnit.SECONDS).readTimeout(10, TimeUnit.SECONDS).writeTimeout(10, TimeUnit.SECONDS)
        .retryOnConnectionFailure(false).followRedirects(false).build()      // a retried POST /link would open a second session
    private val quick: OkHttpClient = http.newBuilder().connectTimeout(1500, TimeUnit.MILLISECONDS).readTimeout(2500, TimeUnit.MILLISECONDS).build()
    private val jsonType = "application/json".toMediaType()

    val deviceName: String get() = identity.deviceName

    // ── lifecycle ───────────────────────────────────────────────────────────
    /** At app start: resume a pending hub enrolment, and retry it whenever the network changes (WireGuard coming up). */
    fun start() {
        if (started) return
        started = true
        scope.launch { if (links.pending() != null) { tryPendingHub(); ensureRetry() } else refreshHubState() }
        runCatching {
            val cm = ctx.getSystemService(ConnectivityManager::class.java)
            cm.registerDefaultNetworkCallback(object : ConnectivityManager.NetworkCallback() {
                override fun onAvailable(network: Network) = kick()
                override fun onCapabilitiesChanged(network: Network, caps: NetworkCapabilities) { if (caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) kick() }
            })
        }
    }
    @Volatile private var lastKick = 0L
    private fun kick() {
        val now = System.currentTimeMillis()
        if (now - lastKick < 5000) return
        lastKick = now
        scope.launch { delay(1500); if (links.pending() != null) tryPendingHub() }
    }
    private fun refreshHubState() {
        val cfg = hubStore.config()
        if (_hub.value.phase == HubLinkState.Phase.IDLE && cfg != null) _hub.value = HubLinkState(HubLinkState.Phase.ENROLLED, "Enrolled with the hub as ${cfg.name}", cfg.fingerprint)
        links.pending()?.let { _hub.value = HubLinkState(HubLinkState.Phase.WAITING, WAITING_TEXT, runCatching { Bfs3.parseInvite(it.invite).fingerprint }.getOrDefault("")) }
    }
    fun hubSnapshot() { scope.launch { refreshHubState() } }

    fun reset() { if (_phase.value !is LinkPhase.Contacting && _phase.value !is LinkPhase.Confirm) _phase.value = LinkPhase.Idle }
    fun cancel() { job?.cancel(); job = null; _phase.value = LinkPhase.Idle }

    // ── reachability ────────────────────────────────────────────────────────
    /** `GET /api/v1/hello` on every address at once; the first BeaconFix to answer wins (null: none did). */
    suspend fun reach(hosts: List<String>, port: Int): Reach? = coroutineScope {
        if (hosts.isEmpty()) return@coroutineScope null
        val first = CompletableDeferred<Reach?>()
        val jobs = hosts.distinct().map { h -> launch(Dispatchers.IO) { hello(h, port)?.let { first.complete(it) } } }
        val all = launch { jobs.joinAll(); first.complete(null) }
        val r = first.await()
        jobs.forEach { it.cancel() }; all.cancel()
        r
    }
    private suspend fun hello(host: String, port: Int): Reach? {
        val t0 = System.nanoTime()
        val req = Request.Builder().url(ApiFactory.baseUrl(host, port, false) + "api/v1/hello").build()
        val body = try { quick.newCall(req).await().use { r -> if (r.isSuccessful) r.body?.string() else null } } catch (e: CancellationException) { throw e } catch (e: Exception) { null } ?: return null
        val h = runCatching { ApiFactory.json.decodeFromString(Hello.serializer(), body) }.getOrNull() ?: return null
        if (h.name != "BeaconFix" && h.version.isEmpty()) return null
        return Reach(host, port, (System.nanoTime() - t0) / 1_000_000, h)
    }

    // ── the two ways in ─────────────────────────────────────────────────────
    /** QR path: the MAC over the QR's secret proves we saw the PC's screen, so the PC approves at once. */
    fun linkWithQr(qr: LinkQr) = launchLink(qr.name.ifEmpty { "the PC" }) { runQr(qr) }
    /** mDNS path: the PC asks its user to confirm the same six digits. */
    fun linkWithPc(pc: DiscoveredDesktop) = launchLink(pc.displayName) { runMdns(pc.displayName, pc.hosts, pc.port) }
    /** A hub invite scanned directly (headless hubs): enrol, showing the fingerprint as information only. */
    fun enrolScannedInvite(text: String) { scope.launch { enrolHub(text, fromDesktop = "", pcName = "", fetchedFresh = false) } }

    /**
     * Enrol with the hub through a PC this phone is already linked to, without a new QR: the PC asks its hub for an
     * invite (`POST /api/v1/link/hub-invite`). For a phone that kept its PC link but lost its hub one (an app-data
     * reset, a forgotten hub). An unreachable hub keeps the invite and finishes when WireGuard is up.
     */
    fun enrolThroughLinkedPc() {
        scope.launch {
            if (hubStore.config() != null) { refreshHubState(); return@launch }
            val pcs = desktops.paired()
            if (pcs.isEmpty()) { _hub.value = HubLinkState(HubLinkState.Phase.FAILED, "No linked PC can invite this phone to a hub. Link a PC first."); return@launch }
            _hub.value = HubLinkState(HubLinkState.Phase.ENROLLING, "Asking ${pcs.first().name.ifEmpty { "the PC" }} for a hub invite…")
            var noHub = 0
            for (d in pcs) {
                val f = freshInvite(d.id)
                f.invite?.let { enrolHub(it, d.id, d.name, fetchedFresh = true); return@launch }
                if (f.noHub) noHub++
            }
            _hub.value = HubLinkState(HubLinkState.Phase.FAILED,
                if (noHub == pcs.size) "None of the linked PCs is enrolled with a hub." else "The linked PCs didn't answer, or couldn't reach their hub. Try again on their network.")
        }
    }

    private fun launchLink(pc: String, block: suspend () -> Unit) {
        job?.cancel()
        _phase.value = LinkPhase.Contacting(pc, "Looking for $pc on this network…")
        job = scope.launch {
            try { block() }
            catch (e: CancellationException) { throw e }
            catch (e: Bfs3Exception) { _phase.value = LinkPhase.Failed(pc, "The link did not authenticate (${e.message}) — nothing was stored. Try again.") }
            catch (e: Exception) { _phase.value = LinkPhase.Failed(pc, e.message ?: e.toString()) }
        }
    }

    /** QR path: sid + pub + mac. mDNS path: only the commitment to our key; the key itself follows in [reveal]. */
    @Serializable private data class StartBody(val sid: String? = null, val name: String, val kind: String, val pub: String? = null, val mac: String? = null, val commit: String? = null)
    @Serializable private data class RevealBody(val pub: String)

    private suspend fun runQr(qr: LinkQr) {
        val pc = qr.name.ifEmpty { "the PC" }
        if (qr.expired(System.currentTimeMillis() / 1000)) throw IOException("This QR has expired — $pc shows a fresh one; scan that.")
        val reach = reach(qr.hosts, qr.port) ?: throw IOException("Can't reach $pc at ${qr.hosts.joinToString(", ")} port ${qr.port}. Is this phone on the same Wi-Fi as the PC?")
        val sk = Bfs3.newPrivateKey(); val pub = Bfs3.b64u(Bfs3.publicKey(sk))
        val name = deviceName
        val body = StartBody(qr.sid, name, Link.KIND, pub, Link.mac(qr.k, qr.sid, name, Link.KIND, pub))
        val started = start(reach, body, pc)
        // the PC's key must be the one on its screen — otherwise someone else answered
        if (!started.pub.contentEquals(qr.pcPub)) throw Bfs3Exception("the PC answered with another key than its QR shows")
        if (started.sid != qr.sid) throw Bfs3Exception("the PC answered for another session")
        finish(reach, sk, started.sid, started.pub, pc, viaQr = true, qrHub = qr.hub, fallbackPort = qr.port)
    }

    private suspend fun runMdns(pc: String, hosts: List<String>, port: Int) {
        val reach = reach(hosts, port) ?: throw IOException("$pc does not answer at ${hosts.joinToString(", ")} port $port.")
        val sk = Bfs3.newPrivateKey(); val pub = Bfs3.b64u(Bfs3.publicKey(sk))
        val name = deviceName
        // commit to our key before seeing the PC's (docs/LINKING.md): a man in the middle cannot then pick a key that makes the codes match
        val started = start(reach, StartBody(null, name, Link.KIND, commit = Link.commit(name, Link.KIND, pub)), pc)
        reveal(reach, started.sid, pub, pc)
        finish(reach, sk, started.sid, started.pub, pc, viaQr = false, qrHub = null, fallbackPort = port)
    }

    private class Started(val sid: String, val pub: ByteArray, val status: String)

    private suspend fun start(reach: Reach, body: StartBody, pc: String): Started {
        _phase.value = LinkPhase.Contacting(pc, "Asking $pc to link…")
        val req = Request.Builder().url(ApiFactory.baseUrl(reach.host, reach.port, false) + "api/v1/link")
            .post(ApiFactory.json.encodeToString(StartBody.serializer(), body).toRequestBody(jsonType)).build()
        val (code, text) = try { http.newCall(req).await().use { it.code to (it.body?.string() ?: "") } }
                           catch (e: CancellationException) { throw e } catch (e: IOException) { throw IOException("$pc stopped answering (${e.message})") }
        when (code) {
            200, 201, 202 -> {}
            404, 405 -> throw IOException(if (code == 404 && body.sid != null && serverError(text)?.contains("session") == true) "That QR has expired on $pc — scan the fresh one it shows."
                                          else "$pc runs a BeaconFix without linking — update it there, then try again.")
            403 -> throw IOException(if (body.sid != null) "$pc refused the QR (${serverError(text) ?: "expired or already used"}) — scan the fresh one it shows." else "$pc refused the link request" + (serverError(text)?.let { ": $it" } ?: "."))
            409, 410 -> throw IOException("That QR was already used — scan the fresh one $pc shows.")
            429 -> throw IOException("$pc has too many link requests waiting — answer or reject them there, then try again.")
            else -> throw IOException("$pc answered HTTP $code" + (serverError(text)?.let { ": $it" } ?: ""))
        }
        val o = runCatching { ApiFactory.json.parseToJsonElement(text).jsonObject }.getOrNull() ?: throw IOException("$pc sent an unreadable answer")
        val sid = o.s("sid")?.lowercase()?.takeIf { it.matches(Regex("[0-9a-f]{8,64}")) } ?: throw IOException("$pc sent no session id")
        val pcPub = Bfs3.unb64u(o.s("pub") ?: throw IOException("$pc sent no key"))
        if (pcPub.size != 32) throw Bfs3Exception("the PC's key is not 32 bytes")
        return Started(sid, pcPub, o.s("status") ?: "pending")
    }

    /** mDNS path, step 2: `POST /api/v1/link/<sid> {"pub"}` — the key we committed to. */
    private suspend fun reveal(reach: Reach, sid: String, pub: String, pc: String) {
        val req = Request.Builder().url(ApiFactory.baseUrl(reach.host, reach.port, false) + "api/v1/link/$sid")
            .post(ApiFactory.json.encodeToString(RevealBody.serializer(), RevealBody(pub)).toRequestBody(jsonType)).build()
        val (code, text) = try { http.newCall(req).await().use { it.code to (it.body?.string() ?: "") } }
                           catch (e: CancellationException) { throw e } catch (e: IOException) { throw IOException("$pc stopped answering (${e.message})") }
        when (code) {
            200, 201, 202, 409 -> {}                       // 409: already sent (a repeated request) — the poll tells the rest
            403 -> throw IOException("$pc refused our key: ${serverError(text) ?: "it does not match the commitment"} — try again.")
            404 -> throw IOException("The link request expired on $pc — try again.")
            429 -> throw IOException("$pc is rate limiting link requests — wait a minute.")
            else -> throw IOException("$pc answered HTTP $code" + (serverError(text)?.let { ": $it" } ?: ""))
        }
    }

    private suspend fun finish(reach: Reach, sk: ByteArray, sid: String, pcPub: ByteArray, pc: String, viaQr: Boolean, qrHub: String?, fallbackPort: Int) {
        val shared = Bfs3.x25519(sk, pcPub)
        sk.fill(0)
        val code = Link.code(shared, sid)
        val deadline = System.currentTimeMillis() + Link.POLL_LIMIT_MS
        _phase.value = LinkPhase.Confirm(pc, code, viaQr, (Link.POLL_LIMIT_MS / 1000).toInt())
        var firstPoll = true
        while (true) {
            val left = deadline - System.currentTimeMillis()
            if (left <= 0) throw IOException(if (viaQr) "$pc did not finish the link in time — try again." else "Nobody confirmed the code on $pc in time — try again and tap Link there.")
            delay(if (firstPoll && viaQr) 300 else Link.POLL_MS)
            firstPoll = false
            _phase.value = LinkPhase.Confirm(pc, code, viaQr, (left / 1000).toInt())
            val req = Request.Builder().url(ApiFactory.baseUrl(reach.host, reach.port, false) + "api/v1/link/$sid").build()
            val (status, text) = try { http.newCall(req).await().use { it.code to (it.body?.string() ?: "") } }
                                 catch (e: CancellationException) { throw e } catch (e: IOException) { continue }    // a dropped poll: keep going
            if (status == 404 || status == 410) throw IOException("The link request expired on $pc — try again.")
            if (status !in 200..299) continue
            val o = runCatching { ApiFactory.json.parseToJsonElement(text).jsonObject }.getOrNull() ?: continue
            when (o.s("status")) {
                "pending" -> continue
                "denied" -> throw IOException("$pc rejected the link" + (o.s("reason")?.takeIf { it.isNotBlank() }?.let { ": $it" } ?: "."))
                "approved" -> {
                    val sealed = o.s("sealed") ?: throw IOException("$pc approved, but the token was already collected — link again.")
                    val payload = Link.parsePayload(Link.openPayload(shared, sid, sealed), fallbackPort)
                    shared.fill(0)
                    val d = store(reach, payload, pc, code)
                    _phase.value = LinkPhase.Linked(payload.pc.name.ifEmpty { pc }, code, d.id)
                    syncScheduler.syncNow()
                    val invite = payload.hub ?: qrHub
                    if (invite != null) enrolHub(invite, d.id, payload.pc.name.ifEmpty { pc }, fetchedFresh = false)
                    else if (hubStore.config() == null) _hub.value = HubLinkState(HubLinkState.Phase.IDLE, "$pc has no hub to share — linked on this network only.")
                    return
                }
                else -> continue
            }
        }
    }

    /** The PC becomes a paired desktop exactly as before: Room row + token in the Keystore-backed store. */
    private suspend fun store(reach: Reach, p: LinkPayload, pc: String, code: String): DesktopEntity {
        val id = "${reach.host}:${reach.port}"
        val now = System.currentTimeMillis()
        val h = reach.hello
        val name = p.pc.name.ifEmpty { pc }
        val prev = desktops.get(id)
        val d = (prev ?: DesktopEntity(id = id, host = reach.host, port = reach.port, name = name)).copy(
            host = reach.host, port = reach.port, name = name, hostname = h.hostname.ifEmpty { prev?.hostname.orEmpty() }, version = h.version, tls = false,
            scopes = p.scopes.joinToString(","), paired = true, lastSeen = now, lastError = "")
        desktops.saveToken(id, p.token)
        desktops.upsert(d)
        // the same PC linked or paired before under another address: one row per PC (else it would be synced twice)
        val hosts = (p.pc.hosts + reach.host).distinct()
        for (other in desktops.paired()) if (other.id != id && samePc(other, d, p.pc.id, hosts)) { desktops.remove(other.id); links.removeMeta(other.id) }
        links.saveMeta(LinkedMeta(id, name, p.pc.id.ifEmpty { h.identity?.id.orEmpty() }, d.hostname, Link.orderHosts(hosts), p.pc.port, code, now))
        return d
    }
    private fun samePc(other: DesktopEntity, d: DesktopEntity, pcId: String, hosts: List<String>): Boolean {
        val m = links.meta(other.id)
        return (pcId.isNotEmpty() && m?.pcId == pcId) || (d.hostname.isNotEmpty() && other.hostname == d.hostname) || (other.port == d.port && other.host in hosts)
    }

    /** Forget a linked PC on this phone (revoke it on the PC under Devices too). */
    suspend fun unlink(desktopId: String) { desktops.remove(desktopId); links.removeMeta(desktopId) }

    /**
     * A linked PC that moved (DHCP gave it another address): keep its row and token, change the address. Called with
     * what mDNS / a probe found for it.
     */
    suspend fun relocate(desktopId: String, host: String, port: Int) {
        val d = desktops.get(desktopId) ?: return
        if (d.host == host && d.port == port) return
        desktops.upsert(d.copy(host = host, port = port, lastSeen = System.currentTimeMillis()))
        links.meta(desktopId)?.let { m -> links.saveMeta(m.copy(hosts = Link.orderHosts(listOf(host) + m.hosts), port = port)) }
    }

    // ── the hub half ────────────────────────────────────────────────────────
    private suspend fun enrolHub(inviteText: String, fromDesktop: String, pcName: String, fetchedFresh: Boolean): Unit =
        hubLock.withLock { enrolLocked(inviteText, fromDesktop, pcName, fetchedFresh) }

    private suspend fun enrolLocked(inviteText: String, fromDesktop: String, pcName: String, fetchedFresh: Boolean) {
        val inv = try { Bfs3.parseInvite(inviteText) } catch (e: Bfs3Exception) { _hub.value = HubLinkState(HubLinkState.Phase.FAILED, "The hub invite is unusable: ${e.message}"); links.clearPending(); return }
        val cur = hubStore.config()
        if (cur != null && Bfs3.constantTimeEquals(cur.fingerprint, inv.fingerprint)) {
            links.clearPending()
            _hub.value = HubLinkState(HubLinkState.Phase.ENROLLED, "Already enrolled with this hub as ${cur.name}", cur.fingerprint)
            return
        }
        val nowS = hub.nowS()
        if (inv.expired(nowS)) {
            val fresh = if (!fetchedFresh && fromDesktop.isNotEmpty()) freshInvite(fromDesktop) else null
            fresh?.invite?.let { return enrolLocked(it, fromDesktop, pcName, fetchedFresh = true) }
            if (fresh?.noHub == true) { noHubAnyMore(pcName, inv.fingerprint); return }
            if (fromDesktop.isNotEmpty()) {
                links.setPending(PendingHub(inviteText, fromDesktop, pcName, System.currentTimeMillis()))
                _hub.value = HubLinkState(HubLinkState.Phase.WAITING, "The hub invite expired — a fresh one is fetched from ${pcName.ifEmpty { "the PC" }} when this phone is back on its network.", inv.fingerprint)
                ensureRetry()
            } else {
                links.clearPending()
                _hub.value = HubLinkState(HubLinkState.Phase.FAILED, "This hub invite has expired — ask the hub for a new one.", inv.fingerprint)
            }
            return
        }
        _hub.value = HubLinkState(HubLinkState.Phase.ENROLLING, "Enrolling with the hub…", inv.fingerprint)
        val url = try { HubUrls.normalise(inv.url.ifBlank { cur?.url ?: HubUrls.DEFAULT }) } catch (e: IllegalArgumentException) {
            _hub.value = HubLinkState(HubLinkState.Phase.FAILED, "The invite names an unusable hub address: ${e.message}", inv.fingerprint); links.clearPending(); return
        }
        try {
            val cfg = hub.enroll(inv, url, deviceName)
            links.clearPending()
            _hub.value = HubLinkState(HubLinkState.Phase.ENROLLED, "Enrolled with the hub as ${cfg.name}", cfg.fingerprint)
            syncScheduler.syncNow()
        } catch (e: CancellationException) { throw e
        } catch (e: HubSecurityException) {
            links.clearPending()
            _hub.value = HubLinkState(HubLinkState.Phase.FAILED, "Hub enrolment rejected: ${e.message}", inv.fingerprint)
        } catch (e: IOException) {
            val msg = e.message.orEmpty()
            when {
                msg == HubErrors.UNREACHABLE || HubErrors.isUnreachable(e) || (e.cause?.let { HubErrors.isUnreachable(it) } == true) -> {
                    links.setPending(PendingHub(inviteText, fromDesktop, pcName, links.pending()?.since ?: System.currentTimeMillis()))
                    _hub.value = HubLinkState(HubLinkState.Phase.WAITING, WAITING_TEXT, inv.fingerprint)
                    ensureRetry()
                }
                msg.startsWith("enrolment refused") && !fetchedFresh && fromDesktop.isNotEmpty() -> {
                    val fresh = freshInvite(fromDesktop)
                    fresh.invite?.let { return enrolLocked(it, fromDesktop, pcName, fetchedFresh = true) }
                    if (fresh.noHub) { noHubAnyMore(pcName, inv.fingerprint); return }
                    links.setPending(PendingHub(inviteText, fromDesktop, pcName, System.currentTimeMillis()))
                    _hub.value = HubLinkState(HubLinkState.Phase.WAITING, "The hub refused the invite (used or expired); a fresh one is fetched from ${pcName.ifEmpty { "the PC" }} when it is reachable.", inv.fingerprint)
                    ensureRetry()
                }
                else -> { links.clearPending(); _hub.value = HubLinkState(HubLinkState.Phase.FAILED, "Hub enrolment failed: $msg", inv.fingerprint) }
            }
        } catch (e: Exception) {
            _hub.value = HubLinkState(HubLinkState.Phase.FAILED, "Hub enrolment failed: ${e.message ?: e}", inv.fingerprint)
        }
    }

    private fun noHubAnyMore(pcName: String, fp: String) {
        links.clearPending()
        _hub.value = HubLinkState(HubLinkState.Phase.FAILED, "${pcName.ifEmpty { "The PC" }} is no longer enrolled with a hub — enrol it there, then link again.", fp)
    }

    /** [invite] a fresh one; [noHub] the PC said it has no hub (409) — no point asking again. */
    private class Fresh(val invite: String?, val noHub: Boolean = false)

    /** `POST /api/v1/link/hub-invite` with the PC's token (control scope) → a fresh `bfs3:` invite (409: no hub, 503: hub unreachable from the PC). */
    private suspend fun freshInvite(desktopId: String): Fresh {
        val d = desktops.get(desktopId) ?: return Fresh(null)
        val token = desktops.token(desktopId) ?: return Fresh(null)
        val hosts = Link.orderHosts(listOf(d.host) + (links.meta(desktopId)?.hosts ?: emptyList()))
        val body = """{"name":${JsonPrimitive(deviceName)},"kind":"${Link.KIND}"}""".toRequestBody(jsonType)
        for (h in hosts) {
            val req = Request.Builder().url(ApiFactory.baseUrl(h, d.port, d.tls) + "api/v1/link/hub-invite").header("Authorization", ApiFactory.bearer(token)).post(body).build()
            val r = try { http.newCall(req).await().use { it.code to (it.body?.string() ?: "") } } catch (e: CancellationException) { throw e } catch (e: IOException) { continue }
            if (r.first == 409) return Fresh(null, noHub = true)
            if (r.first !in 200..299) continue
            val inv = runCatching { ApiFactory.json.parseToJsonElement(r.second).jsonObject.s("hub") }.getOrNull()
            if (inv != null && inv.startsWith(Bfs3.PREFIX, ignoreCase = true)) return Fresh(inv)
        }
        return Fresh(null)
    }

    /** Try the kept invite now (the "Retry" button, a network change, the background loop). */
    suspend fun tryPendingHub() {
        val p = links.pending() ?: return
        enrolHub(p.invite, p.fromDesktop, p.pcName, fetchedFresh = false)
    }
    fun retryHubNow() { scope.launch { tryPendingHub() } }

    private fun ensureRetry() {
        if (retryJob?.isActive == true) return
        retryJob = scope.launch {
            var wait = 20_000L
            while (links.pending() != null) {
                delay(wait)
                runCatching { tryPendingHub() }
                wait = (wait * 2).coerceAtMost(5 * 60_000L)
            }
        }
    }

    companion object {
        const val WAITING_TEXT = "Turn on WireGuard to finish linking to the hub — this phone keeps trying in the background."
        private fun JsonObject.s(k: String): String? = (this[k] as? JsonPrimitive)?.takeIf { it.isString }?.contentOrNull
        /** The `error` text of a JSON error answer, if any. */
        private fun serverError(text: String): String? = runCatching { ApiFactory.json.parseToJsonElement(text).jsonObject.s("error") }.getOrNull()?.takeIf { it.isNotBlank() }?.take(160)
    }
}

/** OkHttp's async call as a cancellable suspend function (cancelling the coroutine cancels the request). */
internal suspend fun Call.await(): Response = suspendCancellableCoroutine { cont ->
    cont.invokeOnCancellation { runCatching { cancel() } }
    enqueue(object : Callback {
        override fun onFailure(call: Call, e: IOException) { if (cont.isActive) cont.resumeWithException(e) }
        override fun onResponse(call: Call, response: Response) { if (cont.isActive) cont.resume(response) else response.close() }
    })
}
