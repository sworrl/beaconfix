package org.sworrl.beaconfix

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import dagger.hilt.android.AndroidEntryPoint
import android.content.Intent
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.nav.IncomingLink
import org.sworrl.beaconfix.ui.BeaconFixRoot
import org.sworrl.beaconfix.ui.map.MapFocus
import org.sworrl.beaconfix.ui.theme.BeaconFixTheme

@AndroidEntryPoint
class MainActivity : ComponentActivity() {
    @javax.inject.Inject lateinit var ranging: org.sworrl.beaconfix.ranging.RangingRepository
    override fun onStart() { super.onStart(); ranging.foreground(true) }
    /** Test hook: `--es rtt_bssid 4E:… --ei rtt_freq 5180 --ei rtt_bw 80 --ei rtt_center 5210 --es rtt_preamble vht` ranges that responder before the desktop serves /ranging/info. */
    private fun rttHook(i: Intent) { i.getStringExtra("rtt_bssid")?.let { b -> ranging.overrideRtt(if (b == "off") null else org.sworrl.beaconfix.data.api.RttInfo(bssid = b.uppercase(), freqMHz = i.getIntExtra("rtt_freq", 2412), centerFreq0MHz = i.getIntExtra("rtt_center", 0), bandwidthMHz = i.getIntExtra("rtt_bw", 20), preamble = i.getStringExtra("rtt_preamble") ?: "ht", enabled = true)) } }
    override fun onStop() { super.onStop(); ranging.foreground(false) }
    override fun onCreate(savedInstanceState: Bundle?) {
        installSplashScreen()
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        // Automation hooks (adb / tests): `am start -n org.sworrl.beaconfix/.MainActivity --es pair_host <host> [--ei pair_port 47822] [--es action sync|scan|identity|…]`
        launch.value = incoming(intent, LaunchArgs.from(intent)); rttHook(intent); simHook(intent)
        // Screenshot automation over adb only (`--ez show_when_locked true`): lets the activity draw over the lock screen for this launch.
        if (intent.getBooleanExtra("show_when_locked", false) && android.os.Build.VERSION.SDK_INT >= 27) { setShowWhenLocked(true); setTurnScreenOn(true) }
        setContent { val args by launch; BeaconFixTheme { BeaconFixRoot(args) } }
    }
    @javax.inject.Inject lateinit var prefs: org.sworrl.beaconfix.data.Prefs
    /**
     * Test hook: `--ez sim_offline true|false --ez sim_no_desktop true|false` sets the offline-simulation prefs (DevFlags).
     * The activity is exported, so this is honoured only on a debug build or with Settings → Developer automation on.
     */
    private fun simHook(i: Intent) {
        if (!i.hasExtra("sim_offline") && !i.hasExtra("sim_no_desktop")) return
        lifecycleScope.launch {
            if (!BuildConfig.DEBUG && !prefs.devAutomation.first()) return@launch
            if (i.hasExtra("sim_offline")) prefs.setSimOffline(i.getBooleanExtra("sim_offline", false))
            if (i.hasExtra("sim_no_desktop")) prefs.setSimNoDesktop(i.getBooleanExtra("sim_no_desktop", false))
        }
    }
    /**
     * Positions other apps hand us: Share → BeaconFix with plain text (a maps link, geo: URI or "lat, lon") and
     * `beaconfix://map?lat=&lon=&label=` open the map on that spot; `beaconfix://help` opens Help. Files still go to Import.
     */
    private fun incoming(i: Intent, args: LaunchArgs): LaunchArgs {
        val data = i.data
        if (i.action == Intent.ACTION_VIEW && data?.scheme.equals("beaconfix", ignoreCase = true)) when (data?.host?.lowercase()) {
            "help" -> return args.copy(action = "help")
            "map" -> return focusOn(IncomingLink.parse(data.toString()), args)
        }
        if (i.action == Intent.ACTION_SEND && args.file == null && (i.type ?: "text/plain").startsWith("text/")) {
            val text = i.getCharSequenceExtra(Intent.EXTRA_TEXT)?.toString().orEmpty()
            return focusOn(IncomingLink.parse(text), args)
        }
        return args
    }
    private fun focusOn(p: IncomingLink.Parsed?, args: LaunchArgs): LaunchArgs {
        if (p == null) { android.widget.Toast.makeText(this, "No location in shared text", android.widget.Toast.LENGTH_SHORT).show(); return args }
        MapFocus.target.value = MapFocus.Target(p.lat, p.lon, label = p.label.ifBlank { "Shared place" })
        return args.copy(action = "map")
    }
    override fun onResume() { super.onResume(); lifecycleScope.launch { org.sworrl.beaconfix.collector.CollectorService.ensure(this@MainActivity, prefs) } }
    private val launch = mutableStateOf(LaunchArgs())
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent); setIntent(intent); launch.value = incoming(intent, LaunchArgs.from(intent)); rttHook(intent); simHook(intent)
        if (intent.getBooleanExtra("show_when_locked", false) && android.os.Build.VERSION.SDK_INT >= 27) { setShowWhenLocked(true); setTurnScreenOn(true) }
    }
}

data class LaunchArgs(val pairHost: String? = null, val pairPort: Int = 47822, val action: String? = null, val identityName: String? = null,
                      val importHost: String? = null, val importCode: String? = null, val importPass: String? = null, val linkPayload: String? = null, val seq: Long = 0,
                      val file: android.net.Uri? = null, val importDryRun: Boolean = false) {
    companion object {
        fun from(i: Intent): LaunchArgs {
            // a file shared or opened with BeaconFix (Share → BeaconFix, or "Open with")
            val data = i.data
            val scheme = data?.scheme ?: ""
            // QR / link URIs → the payload text the identity code understands
            var payload: String? = i.getStringExtra("link_payload")
            var pairHost: String? = i.getStringExtra("pair_host"); var pairPort = i.getIntExtra("pair_port", 47822)
            if (i.action == Intent.ACTION_VIEW && data != null) {
                when (scheme.lowercase()) {
                    "beaconfix" -> when (data.host) {
                        "link" -> payload = "BFLNK1:" + data.path.orEmpty().trimStart('/')
                        "statement" -> payload = "BFLINK1:" + data.path.orEmpty().trimStart('/')
                        "identity" -> payload = "BFID1:" + data.path.orEmpty().trimStart('/')
                        "pair" -> { pairHost = data.getQueryParameter("host"); pairPort = data.getQueryParameter("port")?.toIntOrNull() ?: 47822 }
                    }
                    "bflnk1", "bflink1", "bfid1" -> payload = scheme.uppercase() + ":" + (data.schemeSpecificPart ?: "").trimStart('/')
                }
            }
            val file: android.net.Uri? = when {
                i.action == Intent.ACTION_SEND -> @Suppress("DEPRECATION") (i.getParcelableExtra(Intent.EXTRA_STREAM) as? android.net.Uri)
                i.action == Intent.ACTION_VIEW && (scheme == "content" || scheme == "file") -> data
                else -> null
            }
            return LaunchArgs(pairHost, pairPort, i.getStringExtra("action"), i.getStringExtra("identity_name"),
                i.getStringExtra("import_host"), i.getStringExtra("import_code"), i.getStringExtra("import_pass"), payload, System.nanoTime(), file, i.getBooleanExtra("import_dry_run", false))
        }
    }
}
