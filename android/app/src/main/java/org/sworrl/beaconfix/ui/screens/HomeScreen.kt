package org.sworrl.beaconfix.ui.screens

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.ui.Chip
import org.sworrl.beaconfix.ui.EmptyState
import org.sworrl.beaconfix.ui.Emergency
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.KeyValue
import org.sworrl.beaconfix.ui.Permissions
import org.sworrl.beaconfix.ui.SystemHealthCard
import org.sworrl.beaconfix.ui.help.HomeHelpCard
import org.sworrl.beaconfix.ui.hasRanging
import org.sworrl.beaconfix.ui.ranging
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.hhmm
import org.sworrl.beaconfix.ui.km
import org.sworrl.beaconfix.ui.metres
import org.sworrl.beaconfix.ui.theme.Cyan
import org.sworrl.beaconfix.ui.theme.Gold
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Magenta
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.HomeViewModel
import org.sworrl.beaconfix.ui.vm.LiveViewModel
import org.sworrl.beaconfix.widget.WidgetUpdater

@Composable
fun HomeScreen(onPair: () -> Unit, onIdentity: () -> Unit = {}, onHelp: () -> Unit = {}, onMap: () -> Unit = {}, vm: HomeViewModel = hiltViewModel(), live: LiveViewModel = hiltViewModel()) {
    val ui by vm.ui.collectAsState(); val views by live.views.collectAsState(); val phone by live.phone.collectAsState()
    val refreshing by live.refreshing.collectAsState()
    val ctx = LocalContext.current
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { r -> if (r.values.all { it }) vm.toggleCollector(true) }
    val askRanging = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { live.ranging.foreground(true) }
    val ranges by live.ranges.collectAsState()
    DisposableEffect(Unit) { live.stream(true); onDispose { live.stream(false) } }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) { Text("BeaconFix", style = MaterialTheme.typography.headlineMedium); Text("Wi-Fi beacons, mapped as you move", color = Slate) }
            TextButton(onClick = { live.refresh() }) { Text(if (refreshing) "…" else "↻") }
        }
        SystemHealthCard(compact = true)
        HomeHelpCard(onOpen = onHelp)
        RvCard(onMap = onMap)

        // ── this phone ──────────────────────────────────────────────────
        val f = phone.fix
        InfoCard("This phone") {
            if (f == null) EmptyState("📡", "No position yet", "Turn the collector on: it takes a GPS fix, scans the beacons around you and records both.", "Turn the collector on") { if (!Permissions.hasForeground(ctx)) ask.launch(Permissions.foreground()) else vm.toggleCollector(true) }
            else {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(phone.place.ifEmpty { "%.5f, %.5f".format(f.lat, f.lon) }, style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
                    Chip(if (f.source == "phone-wifi") "WI-FI" else "GPS", if (f.source == "phone-wifi") Cyan else Green)
                }
                KeyValue("Position", "%.5f, %.5f".format(f.lat, f.lon))
                KeyValue("Accuracy", "±${metres(f.acc)}" + (if (f.provider.isNotEmpty()) " · ${f.provider}" else ""))
                phone.sun?.let { KeyValue("Sun", "${it.sunrise} – ${it.sunset} · ${it.dayLengthSecs / 3600} h ${it.dayLengthSecs % 3600 / 60} min of daylight") }
                KeyValue("Time zone", phone.timezone)
                KeyValue("Emergency", Emergency.number(phone.countryCode))
                views.firstOrNull()?.location?.home?.let { h -> if (h.homeLat != null && h.homeLon != null) { val d = WidgetUpdater.distanceM(f.lat, f.lon, h.homeLat, h.homeLon) / 1000; KeyValue("Home", if (d < 0.3) "at home" else "${km(d)} from home") } }
                KeyValue("When", ago(f.time))
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween, modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.weight(1f)) { Text("Collect beacons in the background"); Text("Foreground service: scans Wi-Fi at your GPS position", color = Slate, style = MaterialTheme.typography.bodySmall) }
                Switch(checked = ui.collectorOn, onCheckedChange = { on -> if (on && !Permissions.hasForeground(ctx)) ask.launch(Permissions.foreground()) else vm.toggleCollector(on) })
            }
        }

        // ── each connected desktop ──────────────────────────────────────
        if (views.isEmpty()) InfoCard("Desktops") { EmptyState("🖥", "No PC linked", "Link this phone to BeaconFix on your PC: scan the QR it shows, or pick it on this network. Nothing to type.", "Link a PC") { onPair() } }
        for (v in views) InfoCard(v.desktop.name.ifEmpty { v.desktop.hostname }) {
            val l = v.location
            if (l == null) Text(v.error.ifEmpty { "Fetching…" }, color = if (v.error.isEmpty()) Slate else MaterialTheme.colorScheme.error)
            else if (!l.valid) Text("The desktop has no fix yet.", color = Slate)
            else {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(l.place.ifEmpty { "%.5f, %.5f".format(l.lat, l.lon) }, style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
                    Chip(when (l.source) { "starlink" -> "GPS"; "wifi" -> if (l.provider == "apple") "APPLE WI-FI" else if (l.provider == "internal") "OWN MAP" else "WI-FI"; "ip" -> "IP"; else -> l.source.uppercase() }, when (l.source) { "starlink" -> Green; "wifi" -> Cyan; else -> Gold })
                    if (v.streaming) Chip("LIVE", Magenta)
                }
                KeyValue("Position", "%.5f, %.5f · ±${metres(l.accuracy)}".format(l.lat, l.lon))
                l.elevation?.let { KeyValue("Elevation", "${it.toInt()} m · ${(it * 3.28084).toInt()} ft") }
                l.sun?.let { KeyValue("Sun", "${hhmm(it.sunrise)} – ${hhmm(it.sunset)}" + (if (it.goldenEveningStart.isNotEmpty()) " · golden ${hhmm(it.goldenEveningStart)}" else "")) }
                l.home?.let { KeyValue("Home", if (it.atHome) "🏠 at home" else it.awayText.ifEmpty { "${km(it.awayKm)} away" }) }
                v.trip?.locale?.let { loc -> if (loc.timezone.isNotEmpty()) KeyValue("Time zone", loc.timezone); if (loc.emergencyNumber.isNotEmpty()) KeyValue("Emergency", "${loc.emergencyNumber} (${loc.country.ifEmpty { loc.countryCode }})") }
                v.trip?.let { t -> KeyValue("Beacons", "${t.beaconsNow} in range · ${t.locatedNow} placed · ${t.beaconsTotal} logged"); if (t.rank.isNotEmpty()) KeyValue("Rank", "${t.rank} · level ${t.rankLevel}/${t.rankCount}") }
                KeyValue("Fix", (l.ageS?.let { ago(System.currentTimeMillis() - (it * 1000).toLong()) } ?: hhmm(l.time)) + " · ${l.provider.ifEmpty { l.source }}")
                if (v.error.isNotEmpty()) Text(v.error, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            }
            // ── device ranging: how far this phone is from that desktop, measured (docs/RANGING.md) ──
            val rs = ranges[v.desktop.id]; val best = rs?.best
            if (best != null) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(rs.line, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
                    Chip(best.cls.uppercase(), when (best.cls) { "adjacent" -> Green; "room" -> Cyan; "near" -> Gold; "far" -> MaterialTheme.colorScheme.error; else -> Slate })
                }
                KeyValue("Range", "${org.sworrl.beaconfix.ranging.RangeSession.fmtM(best.lowM)} – ${org.sworrl.beaconfix.ranging.RangeSession.fmtM(best.highM)} (68 %) · " + best.method.joinToString(" + ") + (best.bearingDeg?.let { " · bearing ${it.toInt()}°" } ?: "") + (if (rs.remote != null && System.currentTimeMillis() - rs.remoteAt < 20_000) " · fused by the desktop" else " · fused here"))
                rs.remote?.samples?.let { KeyValue("Samples", "RTT ${it.rtt} · BLE ${it.ble} · Wi-Fi diff ${it.wifiDiff}" + (rs.remote.calib?.rttOffsetM?.let { c -> " · RTT offset ${"%.2f".format(c)} m" } ?: "")) }
                rttLine(rs)?.let { KeyValue("Wi-Fi RTT", it) }
                if (rs.remote?.calib?.rttStale == true) Text("RTT calibration out of date" + (rs.remote?.calib?.rttStaleByM?.let { " (off by ~${"%.1f".format(it)} m)" } ?: "") +
                    " — calibrate again at a known distance: beaconfix --ranging-calibrate", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            } else if (rs != null) {
                val why = when {
                    !Permissions.hasRanging(ctx) -> "needs the Nearby devices permission"
                    rs.supported == false -> "the desktop (${v.desktop.version}) has no ranging endpoint yet"
                    rs.info?.rtt?.enabled != true && rs.identityId.isEmpty() -> "waiting for the desktop's ranging info"
                    rs.bleError.isNotEmpty() -> "BLE: ${rs.bleError}"
                    else -> "measuring… (RTT ${rs.rttCount}, BLE ${rs.bleCount} samples, ${rs.posts} posts)"
                }
                KeyValue("Range", why)
                rttLine(rs)?.let { KeyValue("Wi-Fi RTT", it) }       // its own line: BLE samples must not hide why RTT is silent
                if (!Permissions.hasRanging(ctx)) OutlinedButton(onClick = { askRanging.launch(Permissions.ranging()) }) { Text("Enable device ranging") }
            }
        }

        InfoCard("On this phone") {
            KeyValue("Beacons known", "${ui.aps}"); KeyValue("With a position", "${ui.positioned}"); KeyValue("Observations", "${ui.obs}"); KeyValue("Waiting to sync", "${ui.unsynced}")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) { OutlinedButton(onClick = onPair) { Text("Link a PC") }; OutlinedButton(onClick = onIdentity) { Text("Identity") } }
        }
        CurrentWifiCard()
    }
}

/** Why Wi-Fi RTT is (not) measuring, in words (RttRanging.state); null when there is nothing to say. */
private fun rttLine(rs: org.sworrl.beaconfix.ranging.RangeSession): String? = when (rs.rttState) {
    "" -> null
    org.sworrl.beaconfix.ranging.RttState.OK -> "measuring · ${rs.rttCount} bursts"
    org.sworrl.beaconfix.ranging.RttState.DOZE -> "paused: the phone is in Doze, which switches RTT off. Unlock it or put it on a charger to measure."
    org.sworrl.beaconfix.ranging.RttState.IDLE -> "off: open the app or turn the collector on"
    org.sworrl.beaconfix.ranging.RttState.NO_RESPONDER -> rs.rttError.ifEmpty { null }
    else -> rs.rttError.ifEmpty { rs.rttState } + if (rs.rttCount > 0) " · ${rs.rttCount} bursts so far" else ""
}
