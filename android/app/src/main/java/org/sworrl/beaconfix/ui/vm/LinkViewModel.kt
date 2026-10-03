package org.sworrl.beaconfix.ui.vm

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.Discovery
import org.sworrl.beaconfix.data.DiscoveredDesktop
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.link.HubLinkState
import org.sworrl.beaconfix.link.Link
import org.sworrl.beaconfix.link.LinkPhase
import org.sworrl.beaconfix.link.LinkRepository
import org.sworrl.beaconfix.link.LinkStore
import org.sworrl.beaconfix.link.LinkedMeta
import org.sworrl.beaconfix.link.Reach
import org.sworrl.beaconfix.link.Scanned
import org.sworrl.beaconfix.net.Bfs3
import org.sworrl.beaconfix.net.HubStatus
import org.sworrl.beaconfix.net.HubStore
import javax.inject.Inject

/** A PC on this network: what mDNS said, whether it answers (and how fast), and which linked PC it is, if any. */
data class PcRow(val pc: DiscoveredDesktop, val reach: Reach?, val checked: Boolean, val linkedId: String?)
/** A linked PC: its desktop row, what the link stored, and whether it answers right now. */
data class LinkedRow(val desktop: DesktopEntity, val meta: LinkedMeta?, val reach: Reach?, val checked: Boolean)

/** The Link a PC screen: mDNS list with reachability, the scanner's results, the link and hub progress, linked PCs. */
@HiltViewModel
class LinkViewModel @Inject constructor(
    private val repo: LinkRepository, private val discovery: Discovery, private val desktops: DesktopStore, private val links: LinkStore, hubStore: HubStore,
) : ViewModel() {
    val phase: StateFlow<LinkPhase> = repo.phase
    val hub: StateFlow<HubLinkState> = repo.hubState
    val hubStatus: StateFlow<HubStatus> = hubStore.status
    val message = MutableStateFlow("")
    val mdnsError = MutableStateFlow("")
    private val found = MutableStateFlow<List<DiscoveredDesktop>>(emptyList())
    private val extra = MutableStateFlow<List<DiscoveredDesktop>>(emptyList())     // an address handed in by a beaconfix://pair link
    private val reaches = MutableStateFlow<Map<String, Reach?>>(emptyMap())       // "host:port"-key of a PC / desktop id → last probe
    private val paired = desktops.all()

    val pcs: StateFlow<List<PcRow>> = combine(found, extra, reaches, paired) { f, x, r, ds ->
        (f + x.filter { e -> f.none { it.hosts.contains(e.host) && it.port == e.port } }).map { pc ->
            val key = pcKey(pc)
            PcRow(pc, r[key], r.containsKey(key), linkedFor(pc, ds.filter { it.paired }))
        }.sortedWith(compareBy({ it.reach == null }, { it.pc.displayName.lowercase() }))
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    val linked: StateFlow<List<LinkedRow>> = combine(paired, links.meta, reaches) { ds, m, r ->
        ds.filter { it.paired }.map { LinkedRow(it, m[it.id], r[it.id], r.containsKey(it.id)) }.sortedByDescending { it.meta?.linkedAt ?: it.desktop.lastSeen }
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    private var browseJob: Job? = null
    private var probeJob: Job? = null

    init { repo.hubSnapshot() }

    private fun pcKey(pc: DiscoveredDesktop) = "pc:" + pc.service + "@" + pc.port
    private fun linkedFor(pc: DiscoveredDesktop, ds: List<DesktopEntity>): String? = ds.firstOrNull { d ->
        val m = links.meta(d.id)
        (d.port == pc.port && d.host in pc.hosts) || (pc.identityId.isNotEmpty() && m?.pcId == pc.identityId) ||
            (pc.hostname.isNotEmpty() && pc.hostname.equals(d.hostname, true)) || (m != null && m.hosts.any { it in pc.hosts } && m.port == pc.port)
    }?.id

    /** While the screen is visible: browse mDNS and re-probe every PC (found and linked) every few seconds. */
    fun browse(on: Boolean) {
        browseJob?.cancel(); probeJob?.cancel()
        if (!on) return
        mdnsError.value = ""
        browseJob = viewModelScope.launch { runCatching { discovery.discover { mdnsError.value = it }.collect { found.value = it } } }
        probeJob = viewModelScope.launch {
            while (true) { probeAll(); delay(8000) }
        }
    }

    private suspend fun probeAll() {
        val ds = paired.first().filter { it.paired }
        val pcList = found.value + extra.value
        val pcResults = pcList.map { pc -> viewModelScope.async { pcKey(pc) to repo.reach(pc.hosts, pc.port) } }
        // a linked PC is tried at its own address first; only when that is silent at the others it had (DHCP moves)
        val dResults = ds.map { d -> viewModelScope.async { d.id to (repo.reach(listOf(d.host), d.port) ?: repo.reach((links.meta(d.id)?.hosts ?: emptyList()) - d.host, d.port)) } }
        val all = (pcResults + dResults).awaitAll().toMap()
        reaches.value = all
        for (d in ds) {
            val r = all[d.id]
            val moved = (r ?: pcList.firstOrNull { linkedFor(it, listOf(d)) == d.id }?.let { all[pcKey(it)] })?.takeIf { it.host != d.host || it.port != d.port } ?: continue
            repo.relocate(d.id, moved.host, moved.port)
        }
    }

    /** The one scanner: `bflink:` links, `bfs3:` invites go straight to the hub (fingerprint shown as information only). */
    fun handleScanned(text: String) {
        when (val s = Link.classify(text)) {
            is Scanned.Link -> { message.value = ""; repo.linkWithQr(s.qr) }
            is Scanned.HubInvite -> { message.value = "Hub invite read · hub fingerprint ${Bfs3.groupedFingerprint(s.invite.fingerprint)} (for information)"; repo.enrolScannedInvite(s.text) }
            is Scanned.Bad -> message.value = s.message
        }
    }

    fun linkWith(row: PcRow) {
        message.value = ""
        // a row without a TXT name (an address from a link) takes the name the PC gives in /hello
        val name = row.reach?.hello?.pcName?.takeIf { it.isNotBlank() }
        repo.linkWithPc(if (row.pc.txt["name"].isNullOrBlank() && name != null) row.pc.copy(txt = row.pc.txt + ("name" to name)) else row.pc)
    }

    /** An address from a beaconfix://pair link becomes a row (tapping it links with the code check); [auto] (adb automation) links at once. */
    fun hint(host: String, port: Int, auto: Boolean) {
        val pc = DiscoveredDesktop(host, port, host, false, listOf(host), emptyMap(), "BeaconFix at $host")
        if (extra.value.none { it.host == host && it.port == port }) extra.value = extra.value + pc
        viewModelScope.launch { reaches.value = reaches.value + (pcKey(pc) to repo.reach(listOf(host), port)) }
        if (auto) repo.linkWithPc(pc)
    }

    fun cancel() = repo.cancel()
    fun dismiss() = repo.reset()
    fun retryHub() = repo.retryHubNow()
    fun unlink(id: String) = viewModelScope.launch { repo.unlink(id); message.value = "Unlinked on this phone — remove it on the PC under Devices too." }
    override fun onCleared() { browseJob?.cancel(); probeJob?.cancel() }
}
