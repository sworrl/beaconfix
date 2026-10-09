package org.sworrl.beaconfix.ui.screens

import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.BuildConfig
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.SystemHealthCard
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.SettingsViewModel

@Composable
fun SettingsScreen(
    onPair: () -> Unit,
    onIdentity: () -> Unit = {},
    onWidgets: () -> Unit = {},
    onImport: () -> Unit = {},
    onLicenses: () -> Unit = {},
    onAlpr: () -> Unit = {},
    onTrip: () -> Unit = {},
    onMap: () -> Unit = {},
    vm: SettingsViewModel = hiltViewModel()
) {
    val ctx = androidx.compose.ui.platform.LocalContext.current
    val ui by vm.ui.collectAsStateWithLifecycle()
    val placesVm: SettingsPlacesViewModel = hiltViewModel()
    var home by remember(ui.home) { mutableStateOf(ui.home.joinToString("\n")) }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        SettingsPlacesSection(placesVm)
        SystemHealthCard(compact = false)
        org.sworrl.beaconfix.backup.BackupCard(onRestore = onImport, onIdentity = onIdentity)
        val doomMode by vm.doomBatteryMode.collectAsStateWithLifecycle()
        var showDoomDialog by remember { mutableStateOf(false) }
        InfoCard("Collector") {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) { Text("Collect in the background"); Switch(ui.collectorOn, { vm.setCollector(it) }) }
            Row(
                Modifier.fillMaxWidth().padding(vertical = 4.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("DOOM battery profile", style = MaterialTheme.typography.bodyMedium, fontWeight = androidx.compose.ui.text.font.FontWeight.Bold)
                    Text(doomMode.title + " · " + doomMode.tag, style = MaterialTheme.typography.bodySmall, color = Slate)
                }
                org.sworrl.beaconfix.ui.DoomBatteryChip(
                    mode = doomMode,
                    onClick = { showDoomDialog = true }
                )
            }
            Text("Scan every ${ui.interval} s", style = MaterialTheme.typography.bodyMedium)
            Slider(ui.interval.toFloat(), { vm.setInterval(it.toInt()) }, valueRange = 15f..600f, steps = 38)
            Text("Ignore fixes worse than ±${ui.maxAcc} m", style = MaterialTheme.typography.bodyMedium)
            Slider(ui.maxAcc.toFloat(), { vm.setMaxAcc(it.toInt()) }, valueRange = 10f..300f, steps = 28)
            Text("Battery: the collector holds a foreground service only while enabled; each cycle is one Wi-Fi scan and one location request.", color = Slate, style = MaterialTheme.typography.bodySmall)
        }
        if (showDoomDialog) {
            org.sworrl.beaconfix.ui.DoomBatteryDialog(
                currentMode = doomMode,
                onSelectMode = { mode ->
                    vm.setDoomBatteryMode(mode)
                    showDoomDialog = false
                },
                onDismiss = { showDoomDialog = false }
            )
        }
        InfoCard("Home networks") {
            Text("Networks that travel with you (your router, hotspot). Matched against SSID and BSSID, one glob per line. Pulled from the desktop on sync.", color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedTextField(home, { home = it }, Modifier.fillMaxWidth(), minLines = 3, placeholder = { Text("MyRouter*\nAA:BB:CC:?D:EE:F?") })
            // edited here = the phone's list is newer than the desktop's: the next sync pushes it (needs control access)
            OutlinedButton(onClick = { vm.setHome(home); placesVm.markHomeDirty() }) { Text("Save") }
        }
        InfoCard("Avoid ALPRs routing") {
            Text("Routes around camera detection zones using OpenRouteService or GraphHopper avoid polygons. API keys are free and kept only on this phone.", color = Slate, style = MaterialTheme.typography.bodySmall)
            var orsKey by remember(ui.routingOrsKey) { mutableStateOf(ui.routingOrsKey) }
            var ghKey by remember(ui.routingGraphhopperKey) { mutableStateOf(ui.routingGraphhopperKey) }
            OutlinedTextField(
                value = orsKey,
                onValueChange = { orsKey = it; vm.setRoutingOrsKey(it) },
                label = { Text("OpenRouteService API Key") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true
            )
            OutlinedTextField(
                value = ghKey,
                onValueChange = { ghKey = it; vm.setRoutingGraphhopperKey(it) },
                label = { Text("GraphHopper API Key") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true
            )
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("Preferred provider: ${if (ui.routingProvider == "graphhopper") "GraphHopper" else "OpenRouteService"}", style = MaterialTheme.typography.bodyMedium)
                OutlinedButton(
                    onClick = { vm.setRoutingProvider("ors") },
                    enabled = ui.routingProvider != "ors"
                ) { Text("ORS") }
                OutlinedButton(
                    onClick = { vm.setRoutingProvider("graphhopper") },
                    enabled = ui.routingProvider != "graphhopper"
                ) { Text("GraphHopper") }
            }
        }
        InfoCard("ALPR Dash Cam (Phone Camera)") {
            Text("Run the camera as a local plate reader while driving. Detects plates, matches alerts offline, and captures pass evidence.", color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedButton(onClick = onAlpr) { Text("Open ALPR Dash Cam") }
        }
        InfoCard("Route Tracking & Trip Log") {
            Text("Your driving and walking routes are tracked locally and road-snapped on the Map. Daily mileage, moving time, and stops are logged in Trip.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = onTrip) { Text("View Trip Log") }
                OutlinedButton(onClick = onMap) { Text("Open Map") }
            }
        }
        InfoCard("Status in the shade") {
            val on by vm.statusNotification.collectAsStateWithLifecycle()
            Text("BeaconFix keeps one silent, permanent card in the shade while it runs: your fix, beacons in range, sync state and the range to your desktop, with Help / Scan / Pause buttons. The lock screen only shows “BeaconFix · running”. Swiping it away only puts it back.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) { Column(Modifier.weight(1f)) { Text("Hide the status notification"); Text("Not recommended: the card is the foreground service that keeps BeaconFix alive. Without it Android may stop background collection and device ranging.", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }; Switch(!on, { vm.setStatusNotification(!it) }) }
        }
        InfoCard("Import your history") {
            Text("Google Timeline exports, Takeout, WiGLE CSV, GPX/KML tracks and BeaconFix exports become fixes, beacons and stops on this phone. Nothing is fetched from Google — you export the file, the app reads it.", color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedButton(onClick = onImport) { Text("Import a file") }
        }
        InfoCard("Identity") {
            Text("Your key pair and name — the same identity across the desktop, laptop and phone. Export it, link it, or move it here.", color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedButton(onClick = onIdentity) { Text("Manage identity") }
        }
        InfoCard("PCs") { OutlinedButton(onClick = onPair) { Text("Link a PC / linked PCs") } }
        InfoCard("Home-screen widgets") {
            Text("Help · Location · Beacons · Sync · Map. They refresh every 15 minutes and after each scan or sync.", color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedButton(onClick = onWidgets) { Text("Preview the widgets") }
            val pin = org.sworrl.beaconfix.widget.WidgetPinner.supported(ctx)
            if (!pin) Text("Your launcher does not support pinning from apps — long-press the home screen → Widgets → BeaconFix.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(enabled = pin, onClick = { org.sworrl.beaconfix.widget.WidgetPinner.pin(ctx, org.sworrl.beaconfix.widget.HelpWidgetReceiver::class.java) }) { Text("Help") }
                OutlinedButton(enabled = pin, onClick = { org.sworrl.beaconfix.widget.WidgetPinner.pin(ctx, org.sworrl.beaconfix.widget.LocationWidgetReceiver::class.java) }) { Text("Location") }
                OutlinedButton(enabled = pin, onClick = { org.sworrl.beaconfix.widget.WidgetPinner.pin(ctx, org.sworrl.beaconfix.widget.BeaconsWidgetReceiver::class.java) }) { Text("Beacons") }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(enabled = pin, onClick = { org.sworrl.beaconfix.widget.WidgetPinner.pin(ctx, org.sworrl.beaconfix.widget.SyncWidgetReceiver::class.java) }) { Text("Sync") }
                OutlinedButton(enabled = pin, onClick = { org.sworrl.beaconfix.widget.WidgetPinner.pin(ctx, org.sworrl.beaconfix.widget.MapWidgetReceiver::class.java) }) { Text("Map") }
            }
        }
        DeveloperAutomationCard(placesVm)
        InfoCard("Privacy") {
            Text("Everything stays on this phone and on the desktops you pair with. No analytics, no crash reporting, no third-party servers. The only other network traffic is map tiles (OpenStreetMap) and, for Help and Places when no desktop answers, OpenStreetMap place and address lookups around you (“Find places from this phone” above turns the place search off). Tokens are stored in Android's Keystore-backed encrypted preferences; the database is in app-private storage and excluded from backups.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Text("BeaconFix Android ${BuildConfig.VERSION_NAME} · Apache-2.0", color = Slate, style = MaterialTheme.typography.bodySmall)
        }
        InfoCard("Open-source licenses") {
            Text("BeaconFix is Apache-2.0. The libraries it is built with, the ALPR models (MIT) and ONNX Runtime (MIT) keep their own licenses.", color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedButton(onClick = onLicenses) { Text("Open-source licenses") }
        }
    }
}
