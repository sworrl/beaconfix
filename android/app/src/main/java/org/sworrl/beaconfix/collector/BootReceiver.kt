package org.sworrl.beaconfix.collector

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.data.Prefs
import javax.inject.Inject

/** Brings the presence service (and its permanent notification) back after a reboot or an app update. */
@AndroidEntryPoint
class BootReceiver : BroadcastReceiver() {
    @Inject lateinit var prefs: Prefs
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED && intent.action != Intent.ACTION_MY_PACKAGE_REPLACED && intent.action != Intent.ACTION_LOCKED_BOOT_COMPLETED) return
        val pending = goAsync()
        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            try { CollectorService.ensure(context, prefs) } finally { pending.finish() }
        }
    }
}
