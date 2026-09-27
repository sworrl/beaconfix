package org.sworrl.beaconfix.data

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import javax.inject.Inject
import javax.inject.Singleton

data class DiscoveredDesktop(val host: String, val port: Int, val name: String, val pairingOpen: Boolean)

/** Finds BeaconFix desktops advertising `_beaconfix._tcp` (TXT: v=1 pair=0|1). */
@Singleton
class Discovery @Inject constructor(@ApplicationContext private val ctx: Context) {
    fun discover(): Flow<List<DiscoveredDesktop>> = callbackFlow {
        val nsd = ctx.getSystemService(Context.NSD_SERVICE) as NsdManager
        val found = LinkedHashMap<String, DiscoveredDesktop>()
        val listener = object : NsdManager.DiscoveryListener {
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) { close() }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            override fun onDiscoveryStarted(serviceType: String) {}
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onServiceFound(info: NsdServiceInfo) {
                @Suppress("DEPRECATION")
                nsd.resolveService(info, object : NsdManager.ResolveListener {
                    override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {}
                    override fun onServiceResolved(si: NsdServiceInfo) {
                        val host = si.host?.hostAddress ?: return
                        val pair = si.attributes["pair"]?.let { String(it) } == "1"
                        found[si.serviceName] = DiscoveredDesktop(host, si.port, si.serviceName, pair)
                        trySend(found.values.toList())
                    }
                })
            }
            override fun onServiceLost(info: NsdServiceInfo) { found.remove(info.serviceName); trySend(found.values.toList()) }
        }
        try { nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, listener) } catch (e: Exception) { close(e) }
        awaitClose { runCatching { nsd.stopServiceDiscovery(listener) } }
    }
    companion object { const val SERVICE_TYPE = "_beaconfix._tcp." }
}
