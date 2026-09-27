package org.sworrl.beaconfix.ui.vm

import android.content.Context
import android.location.Geocoder
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.sworrl.beaconfix.collector.CollectorService
import org.sworrl.beaconfix.collector.CollectorStatus
import org.sworrl.beaconfix.data.DesktopLive
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.DesktopView
import org.sworrl.beaconfix.data.Peer
import org.sworrl.beaconfix.data.PeerDiscovery
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.identity.Crypto
import org.sworrl.beaconfix.identity.IdentityOps
import org.sworrl.beaconfix.identity.IdentityRecord
import org.sworrl.beaconfix.identity.IdentityRepository
import org.sworrl.beaconfix.identity.IdentityStore
import org.sworrl.beaconfix.identity.LinkOffer
import org.sworrl.beaconfix.identity.LoginResult
import org.sworrl.beaconfix.sync.SyncRepository
import javax.inject.Inject

/** The desktop's live views for every paired desktop, plus the phone's own enriched fix. */
data class PhoneFix(val fix: FixEntity? = null, val place: String = "", val countryCode: String = "", val sun: org.sworrl.beaconfix.ui.SunCalc.Times? = null, val timezone: String = java.util.TimeZone.getDefault().id)

@HiltViewModel
class LiveViewModel @Inject constructor(private val live: DesktopLive, db: AppDatabase, val status: CollectorStatus, private val prefs: Prefs, @ApplicationContext private val ctx: Context, val refits: org.sworrl.beaconfix.estimate.RefitBus, val ranging: org.sworrl.beaconfix.ranging.RangingRepository) : ViewModel() {
    val ranges: StateFlow<Map<String, org.sworrl.beaconfix.ranging.RangeSession>> = ranging.sessions
    /** local refits also appear in the ticker, as the desktop's do */
    val localRefits: StateFlow<List<org.sworrl.beaconfix.estimate.RefitEvent>> = refits.events.let { f -> MutableStateFlow<List<org.sworrl.beaconfix.estimate.RefitEvent>>(emptyList()).also { st -> viewModelScope.launch { f.collect { e -> st.value = (st.value + e).takeLast(30) } } } }
    fun replayLastRefit() { refits.last?.let { refits.emit(it.copy(time = System.currentTimeMillis())) } }
    val views: StateFlow<List<DesktopView>> = combine(live.views, kotlinx.coroutines.flow.flowOf(Unit)) { m, _ -> m.values.toList() }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    private val phoneRaw = db.fixes().latest()
    private val phoneEnriched = MutableStateFlow(PhoneFix())
    val phone: StateFlow<PhoneFix> = phoneEnriched
    val aps: StateFlow<List<ApEntity>> = db.aps().all().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val positioned: StateFlow<List<ApEntity>> = db.aps().positioned().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val phoneTrack: StateFlow<List<FixEntity>> = db.fixes().since(System.currentTimeMillis() - 24 * 3600_000L).stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val desktopTrack: StateFlow<List<FixEntity>> = db.fixes().desktopTrack().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val collectorOn: StateFlow<Boolean> = prefs.collectorOn.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), false)
    val refreshing = MutableStateFlow(false)

    init {
        viewModelScope.launch { phoneRaw.collect { f -> phoneEnriched.value = enrich(f) } }
        refresh()
    }
    fun refresh(what: Set<String> = setOf("location", "trip", "pois", "events")) = viewModelScope.launch { refreshing.value = true; try { live.refreshAll(what) } finally { refreshing.value = false } }
    fun stream(on: Boolean) { if (on) live.startStream() else live.stopStream() }
    fun toggleCollector(on: Boolean) = viewModelScope.launch { prefs.setCollectorOn(on); CollectorService.ensure(ctx, prefs) }

    private suspend fun enrich(f: FixEntity?): PhoneFix {
        if (f == null) return PhoneFix()
        val geo = withContext(Dispatchers.IO) { runCatching { @Suppress("DEPRECATION") Geocoder(ctx).getFromLocation(f.lat, f.lon, 1)?.firstOrNull() }.getOrNull() }
        val place = geo?.let { listOfNotNull(it.locality ?: it.subAdminArea, it.adminArea).filter { s -> s.isNotBlank() }.joinToString(", ") } ?: f.place
        return PhoneFix(f, place, geo?.countryCode ?: "", org.sworrl.beaconfix.ui.SunCalc.today(f.lat, f.lon))
    }
}

/** The other BeaconFix installs on this network and the two adjacency actions: "that's me" (import) and "link my identity with it". */
data class PeerRow(val peer: Peer, val relation: String)

@HiltViewModel
class PeersViewModel @Inject constructor(
    private val peers: PeerDiscovery, private val identity: IdentityStore, private val repo: IdentityRepository, private val desktops: DesktopStore, private val sync: SyncRepository,
) : ViewModel() {
    val rows: StateFlow<List<PeerRow>> = combine(peers.peers, identity.current) { m, rec ->
        val linked = rec?.let { IdentityOps.linkedSet(it) } ?: emptySet()
        // one row per (host, identity): a test instance on another port of the same box is noise — keep the default port
        val dedup = m.values.groupBy { (it.host to it.identityId.ifEmpty { it.key })}.values.map { g -> g.minByOrNull { if (it.port == 47822) 0 else it.port } ?: g.first() }
        dedup.map { PeerRow(it, it.relation(rec?.id, linked)) }.sortedWith(compareBy({ it.relation != "same" }, { it.relation != "different" }, { it.peer.hostname })) }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val scanning: StateFlow<Boolean> = peers.scanning
    val note: StateFlow<String> = peers.scanNote
    val busy = MutableStateFlow(""); val message = MutableStateFlow("")
    /** import step: a peer whose bundle is held under a code → we need the code (+ passphrase) */
    val importFrom = MutableStateFlow<Peer?>(null)
    val importHint = MutableStateFlow("")

    init { viewModelScope.launch { runCatching { peers.mdns().collect { peers.noteMdns(it) } } } }
    fun scan() = viewModelScope.launch { runCatching { peers.scanSubnet() } }
    fun probe(host: String) = viewModelScope.launch { peers.probeOne(host.trim(), 47822, "manual")?.let { } ?: run { message.value = "no BeaconFix at $host" } }

    /** "That's me": ask the peer to hold its identity bundle under a code (if it can), otherwise tell the user what to press there. */
    fun thatsMe(p: Peer) = viewModelScope.launch {
        importFrom.value = p; importHint.value = ""
        if ("export-request" in p.features) {
            val r = runCatching { ApiFactory.create(p.host, p.port, false).identityExportRequest() }.getOrNull()
            importHint.value = if (r?.isSuccessful == true) "${p.hostname} is now showing a 6-digit code and a six-word passphrase — enter both here." else "${p.hostname} did not accept the request (it may need you to confirm there). Open Identity → Export → LAN code on it and enter the code here."
        } else importHint.value = "On ${p.hostname.ifEmpty { p.host }}: Identity → Export → \"hold for the LAN\" (or `beaconfix --identity-export --words`), then enter its 6-digit code and passphrase here."
    }
    fun importWith(code: String, pass: String) = viewModelScope.launch {
        val p = importFrom.value ?: return@launch
        busy.value = p.key
        try { val b = repo.fetchBundle(p.host, p.port, false, code); val r = identity.import(b, pass); message.value = "you are now ${r.name} (${Crypto.grouped(r.id)})"; importFrom.value = null; loginTo(p) }
        catch (e: Exception) { message.value = e.message ?: "import failed" } finally { busy.value = "" }
    }
    fun cancelImport() { importFrom.value = null }

    /** "Link my identity with it": phone signs first, posts the half statement, then polls the peer until the link shows. */
    fun linkWith(p: Peer) = viewModelScope.launch {
        busy.value = p.key
        try {
            val remote = ApiFactory.create(p.host, p.port, false).identity().body() ?: error("${p.hostname} has no identity")
            val offer = LinkOffer(id = remote.id, name = remote.name, pub = remote.pub, ts = org.sworrl.beaconfix.identity.isoNow())
            val half = identity.complete(offer)
            val d = desktops.get(p.key) ?: DesktopEntity(id = p.key, host = p.host, port = p.port, name = p.hostname.ifEmpty { p.host }, hostname = p.hostname, version = p.version)
            desktops.upsert(d)
            val r = repo.sendLink(d, half, Crypto.unb64(remote.pub))
            if (r.isSuccess) { message.value = "linked with ${remote.name}"; loginTo(p); return@launch }
            // the desktop may have parked it for the user to confirm: poll its public identity for the link
            message.value = "waiting for ${p.hostname} to confirm the link (${r.exceptionOrNull()?.message?.take(80) ?: ""})"
            val me = identity.currentNow() ?: return@launch
            repeat(60) {
                kotlinx.coroutines.delay(5000)
                val pub = runCatching { ApiFactory.create(p.host, p.port, false).identity().body() }.getOrNull() ?: return@repeat
                val st = pub.links.firstOrNull { (it.a == me.id || it.b == me.id) && it.complete }
                if (st != null && identity.addLink(st, null, Crypto.unb64(remote.pub))) { message.value = "linked with ${remote.name}"; loginTo(p); return@launch }
            }
            message.value = "no confirmation from ${p.hostname} yet — it is listed there under pending links"
        } catch (e: Exception) { message.value = e.message ?: "link failed" } finally { busy.value = "" }
    }

    /** After import/link: sign in with the identity so the desktop is paired without a code, then sync. */
    private suspend fun loginTo(p: Peer) {
        if ("identity" !in p.features) return
        val d = desktops.get(p.key) ?: DesktopEntity(id = p.key, host = p.host, port = p.port, name = p.hostname.ifEmpty { p.host }, hostname = p.hostname, version = p.version)
        desktops.upsert(d)
        when (val r = repo.login(d)) { is LoginResult.Ok -> { message.value += " · signed in to ${p.hostname}"; runCatching { sync.syncAll() } }; is LoginResult.UnknownIdentity -> message.value += " · ${r.message}"; is LoginResult.Failed -> message.value += " · sign-in: ${r.message}" }
    }
    /** Sign in to a same-identity peer that we have not paired with yet. */
    fun signIn(p: Peer) = viewModelScope.launch { busy.value = p.key; try { loginTo(p) } finally { busy.value = "" } }
}
