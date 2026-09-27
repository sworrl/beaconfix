package org.sworrl.beaconfix.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.BuildConfig

/**
 * Test switches for offline scenarios, readable from anywhere without injection. [simOffline] = behave as if there
 * were no network at all (no desktop, no Overpass, no Nominatim); [simNoDesktop] = as if no desktop were reachable.
 * They only take effect on a debug build or with Settings → Developer automation on; with that off they are ignored.
 */
object DevFlags {
    @Volatile var simOffline = false
    @Volatile var simNoDesktop = false

    fun desktopBlocked() = simOffline || simNoDesktop

    private var job: Job? = null

    /** Mirror the prefs (`simOffline`, `simNoDesktop`, `devAutomation`) into the flags for the life of [scope]. */
    @Synchronized
    fun bind(prefs: Prefs, scope: CoroutineScope) {
        job?.cancel()
        job = scope.launch {
            combine(prefs.simOffline, prefs.simNoDesktop, prefs.devAutomation) { off, noDesk, dev -> Triple(off, noDesk, dev) }
                .collect { (off, noDesk, dev) ->
                    val allowed = BuildConfig.DEBUG || dev
                    simOffline = allowed && off
                    simNoDesktop = allowed && noDesk
                }
        }
    }
}
