package org.sworrl.beaconfix.data

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.ext.SdkExtensions
import androidx.annotation.RequiresApi
import androidx.annotation.RequiresExtension
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import org.sworrl.beaconfix.link.Link
import java.net.Inet4Address
import java.net.InetAddress
import java.util.concurrent.Executor
import java.util.concurrent.Executors
import javax.inject.Inject
import javax.inject.Singleton

/**
 * A BeaconFix PC advertising `_beaconfix._tcp`. [host] is the best address to dial (IPv4 first), [hosts] every
 * candidate (the resolved addresses plus the TXT `addr=` list the PC publishes), [txt] the TXT record
 * (docs/LINKING.md "Discovery": name, id, link, api, port; older PCs: v, host, kind, pair, features, addr).
 */
data class DiscoveredDesktop(val host: String, val port: Int, val name: String, val pairingOpen: Boolean,
                             val hosts: List<String> = listOf(host), val txt: Map<String, String> = emptyMap(), val service: String = name) {
    /** The PC's own name: TXT name (spec), else its hostname, else the service instance name. */
    val displayName: String get() = txt["name"]?.takeIf { it.isNotBlank() } ?: txt["host"]?.takeIf { it.isNotBlank() } ?: service.removePrefix("BeaconFix on ")
    val hostname: String get() = txt["host"].orEmpty()
    val identityId: String get() = txt["id"].orEmpty()
    val supportsLink: Boolean get() = txt["link"] == "1" || (txt["api"]?.toIntOrNull() ?: 0) >= 3
}

/**
 * Finds BeaconFix PCs advertising `_beaconfix._tcp` with [NsdManager].
 *
 * What made the old discovery miss PCs, and what this does instead:
 * - Every found service was resolved at once with the legacy `resolveService`, which allows ONE resolve in flight:
 *   the others failed with FAILURE_ALREADY_ACTIVE and were never retried. Android 14+ uses
 *   `registerServiceInfoCallback` (no such limit, and it follows address changes); older versions resolve one at a time.
 * - Only `host` was read — on Android 14+ that may be an IPv6 link-local address (`fe80::…%wlan0`) that an HTTP URL
 *   cannot dial. Every address is collected now, IPv4 preferred, plus the PC's own TXT `addr=` list.
 * - A failed start closed the flow silently; now the error is reported ([onError]) and the list stays empty.
 * - A Wi-Fi multicast lock is held while browsing (some chipsets drop multicast in power save).
 */
@Singleton
class Discovery @Inject constructor(@ApplicationContext private val ctx: Context) {
    fun discover(onError: (String) -> Unit = {}): Flow<List<DiscoveredDesktop>> = callbackFlow {
        val nsd = ctx.getSystemService(Context.NSD_SERVICE) as NsdManager
        val found = LinkedHashMap<String, DiscoveredDesktop>()
        val lock = runCatching {
            (ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager).createMulticastLock("beaconfix-mdns").apply { setReferenceCounted(false); acquire() }
        }.getOrNull()
        fun publish() { trySend(found.values.toList()) }
        fun put(si: NsdServiceInfo, addresses: List<InetAddress>) = synchronized(found) {
            val txt = si.attributes.orEmpty().mapValues { (_, v) -> v?.let { String(it, Charsets.UTF_8) }.orEmpty() }
            val resolved = addresses.mapNotNull { a -> a.hostAddress?.substringBefore('%')?.takeIf { (a is Inet4Address || !a.isLinkLocalAddress) && !a.isLoopbackAddress } }
            val fromTxt = txt["addr"].orEmpty().split(',').map { it.trim() }.filter { it.isNotEmpty() && Link.validHost(it) }
            val hosts = Link.orderHosts(resolved + fromTxt).filter { Link.validHost(it) }
            if (hosts.isEmpty()) return@synchronized
            val port = txt["port"]?.toIntOrNull()?.takeIf { it in 1..65535 } ?: si.port
            found[si.serviceName] = DiscoveredDesktop(hosts.first(), port, si.serviceName, txt["pair"] == "1", hosts, txt, si.serviceName)
            publish()
        }
        val pool = Executors.newSingleThreadExecutor()
        val exec: Executor = pool
        // Android 14+: one ServiceInfoCallback per service (no one-at-a-time limit, follows address changes)
        val watcher: Any? = if (Build.VERSION.SDK_INT >= 34 && SdkExtensions.getExtensionVersion(Build.VERSION_CODES.TIRAMISU) >= 7)
            InfoWatcher(nsd, exec, { si, addrs -> put(si, addrs) }, { name -> synchronized(found) { if (found.remove(name) != null) publish() } }) else null
        val queue = ArrayDeque<NsdServiceInfo>(); var resolving = false   // older: one legacy resolve at a time

        fun resolveNextLegacy() {
            synchronized(queue) { if (resolving) return; val next = queue.removeFirstOrNull() ?: return; resolving = true
                @Suppress("DEPRECATION")
                runCatching {
                    nsd.resolveService(next, object : NsdManager.ResolveListener {
                        override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                            synchronized(queue) { resolving = false; if (errorCode == NsdManager.FAILURE_ALREADY_ACTIVE) queue.addLast(serviceInfo) }
                            resolveNextLegacy()
                        }
                        override fun onServiceResolved(si: NsdServiceInfo) {
                            @Suppress("DEPRECATION") put(si, listOfNotNull(si.host))
                            synchronized(queue) { resolving = false }
                            resolveNextLegacy()
                        }
                    })
                }.onFailure { resolving = false }
            }
        }

        val listener = object : NsdManager.DiscoveryListener {
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) { onError("mDNS discovery could not start (error $errorCode)") }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            override fun onDiscoveryStarted(serviceType: String) {}
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onServiceFound(info: NsdServiceInfo) {
                if (Build.VERSION.SDK_INT >= 34 && SdkExtensions.getExtensionVersion(Build.VERSION_CODES.TIRAMISU) >= 7) (watcher as InfoWatcher).watch(info)
                else { synchronized(queue) { queue.addLast(info) }; resolveNextLegacy() }
            }
            override fun onServiceLost(info: NsdServiceInfo) {
                if (Build.VERSION.SDK_INT >= 34 && SdkExtensions.getExtensionVersion(Build.VERSION_CODES.TIRAMISU) >= 7) (watcher as InfoWatcher).unwatch(info.serviceName)
                synchronized(found) { if (found.remove(info.serviceName) != null) publish() }
            }
        }
        try { nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, listener) }
        catch (e: Exception) { onError("mDNS discovery unavailable: ${e.message}") }
        awaitClose {
            runCatching { nsd.stopServiceDiscovery(listener) }
            if (Build.VERSION.SDK_INT >= 34 && SdkExtensions.getExtensionVersion(Build.VERSION_CODES.TIRAMISU) >= 7) (watcher as InfoWatcher).close()
            runCatching { lock?.release() }
            pool.shutdown()
        }
    }

    /** Without the trailing dot: the form NsdManager documents (Android 14's mDNS stack is strict about it). */
    companion object { const val SERVICE_TYPE = "_beaconfix._tcp" }
}

/** Android 14+ resolution: a ServiceInfoCallback per service, every address it has, updates as they change. */
@RequiresApi(34) @RequiresExtension(extension = Build.VERSION_CODES.TIRAMISU, version = 7)
private class InfoWatcher(private val nsd: NsdManager, private val exec: Executor,
                          private val onUpdate: (NsdServiceInfo, List<InetAddress>) -> Unit, private val onLost: (String) -> Unit) {
    private val callbacks = HashMap<String, NsdManager.ServiceInfoCallback>()

    fun watch(info: NsdServiceInfo) {
        val name = info.serviceName
        val cb = object : NsdManager.ServiceInfoCallback {
            override fun onServiceInfoCallbackRegistrationFailed(errorCode: Int) { synchronized(callbacks) { callbacks.remove(name) } }
            override fun onServiceUpdated(si: NsdServiceInfo) = onUpdate(si, si.hostAddresses)
            override fun onServiceLost() = onLost(name)
            override fun onServiceInfoCallbackUnregistered() {}
        }
        synchronized(callbacks) { if (callbacks.containsKey(name)) return; callbacks[name] = cb }
        runCatching { nsd.registerServiceInfoCallback(info, exec, cb) }.onFailure { synchronized(callbacks) { callbacks.remove(name) } }
    }
    fun unwatch(name: String) {
        val cb = synchronized(callbacks) { callbacks.remove(name) } ?: return
        runCatching { nsd.unregisterServiceInfoCallback(cb) }
    }
    fun close() { for (name in synchronized(callbacks) { callbacks.keys.toList() }) unwatch(name) }
}
