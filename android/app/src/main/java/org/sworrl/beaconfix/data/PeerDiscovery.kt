package org.sworrl.beaconfix.data

import android.content.Context
import android.net.wifi.WifiManager
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import okhttp3.OkHttpClient
import okhttp3.Request
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.Hello
import org.sworrl.beaconfix.identity.IdentityStore
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton

/** A BeaconFix on this network, however we found it (mDNS, the desktop's peers list, or a subnet probe). */
data class Peer(
    val host: String, val port: Int, val hostname: String, val version: String, val kind: String,
    val identityId: String, val identityName: String, val pairingOpen: Boolean, val features: List<String>,
    val via: String,                    // mdns | peers | probe
) {
    val key get() = "$host:$port"
    /** same | different | none — relative to our own identity and its linked set. */
    fun relation(ownId: String?, linked: Set<String>): String = when { identityId.isEmpty() -> "none"; identityId == ownId || identityId in linked -> "same"; else -> "different" }
}

/**
 * Finds the other BeaconFix installs around us. mDNS (`_beaconfix._tcp`) is continuous; the desktop's `/api/v1/peers`
 * (when it has it) adds what it sees; "Scan the network" probes every host of the phone's /24 for `/api/v1/hello`
 * with short timeouts, 32 at a time.
 */
@Singleton
class PeerDiscovery @Inject constructor(@ApplicationContext private val ctx: Context, private val discovery: Discovery, private val identity: IdentityStore) {
    private val _peers = MutableStateFlow<Map<String, Peer>>(emptyMap())
    val peers: StateFlow<Map<String, Peer>> = _peers
    val scanning = MutableStateFlow(false)
    val scanNote = MutableStateFlow("")
    private val quick = OkHttpClient.Builder().connectTimeout(450, TimeUnit.MILLISECONDS).readTimeout(900, TimeUnit.MILLISECONDS).build()

    /** Keep the mDNS view current (call from a screen's lifecycle). */
    fun mdns(): Flow<List<DiscoveredDesktop>> = discovery.discover()
    suspend fun noteMdns(list: List<DiscoveredDesktop>) { for (d in list) probeOne(d.host, d.port, "mdns")?.let { add(it) } }

    private fun add(p: Peer) { _peers.value = _peers.value + (p.key to p) }

    /** hello → Peer (null when it is not a BeaconFix). */
    suspend fun probeOne(host: String, port: Int, via: String): Peer? = withContext(Dispatchers.IO) {
        val req = Request.Builder().url(ApiFactory.baseUrl(host, port, false) + "api/v1/hello").build()
        val body = runCatching { quick.newCall(req).execute().use { r -> if (r.isSuccessful) r.body?.string() else null } }.getOrNull() ?: return@withContext null
        val h = runCatching { ApiFactory.json.decodeFromString(Hello.serializer(), body) }.getOrNull() ?: return@withContext null
        if (h.name != "BeaconFix" && h.version.isEmpty()) return@withContext null
        var id = h.identity?.id ?: ""; var name = h.identity?.name ?: ""
        if (id.isEmpty() && "identity" in h.features) runCatching { ApiFactory.create(host, port, false).identity().body() }.getOrNull()?.let { id = it.id; name = it.name }
        Peer(host, port, h.hostname, h.version, "desktop", id, name, h.pairing, h.features, via)
    }

    /** Ask a desktop that has the peers feature what it sees. */
    suspend fun fromPeersEndpoint(host: String, port: Int) {
        runCatching { ApiFactory.create(host, port, false).peers().body()?.peers }.getOrNull()?.forEach { p ->
            add(Peer(p.host.ifEmpty { host }, p.port, p.hostname, p.version, p.kind.ifEmpty { "desktop" }, p.id, p.name, p.pairing, p.features, "peers"))
        }
    }

    /** Probe the whole /24 the phone sits on (Wi-Fi). ~250 hosts, 32 in flight, ≤ 1.5 s each → a few seconds. */
    suspend fun scanSubnet(port: Int = 47822): Int {
        if (scanning.value) return 0
        scanning.value = true; scanNote.value = ""
        try {
            val ip = wifiIpv4() ?: run { scanNote.value = "not on Wi-Fi"; return 0 }
            val base = ip.substringBeforeLast('.')
            val sem = Semaphore(32)
            val found = coroutineScope {
                (1..254).map { n -> async(Dispatchers.IO) { sem.withPermit { withTimeoutOrNull(1500) { probeOne("$base.$n", port, "probe") } } } }.awaitAll().filterNotNull()
            }
            found.forEach { add(it) }
            // what a found desktop knows about
            for (p in found) if ("peers" in p.features) fromPeersEndpoint(p.host, p.port)
            scanNote.value = if (found.isEmpty()) "no other BeaconFix answered on $base.0/24" else "${found.size} found on $base.0/24"
            return found.size
        } finally { scanning.value = false }
    }

    suspend fun relationOf(p: Peer): String { val rec = identity.currentNow(); return p.relation(rec?.id, identity.linkedIds()) }

    private fun wifiIpv4(): String? = try {
        @Suppress("DEPRECATION") val i = (ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager).connectionInfo.ipAddress
        if (i == 0) null else "${i and 0xff}.${i shr 8 and 0xff}.${i shr 16 and 0xff}.${i shr 24 and 0xff}"
    } catch (e: Exception) { null }
}
