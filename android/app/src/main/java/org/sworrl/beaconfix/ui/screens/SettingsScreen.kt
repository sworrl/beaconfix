package org.sworrl.beaconfix.ui.screens

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
import androidx.compose.runtime.collectAsState
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
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.SettingsViewModel

@Composable
fun SettingsScreen(onPair: () -> Unit, vm: SettingsViewModel = hiltViewModel()) {
    val ui by vm.ui.collectAsState()
    var home by remember(ui.home) { mutableStateOf(ui.home.joinToString("\n")) }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        InfoCard("Collector") {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) { Text("Collect in the background"); Switch(ui.collectorOn, { vm.setCollector(it) }) }
            Text("Scan every ${ui.interval} s", style = MaterialTheme.typography.bodyMedium)
            Slider(ui.interval.toFloat(), { vm.setInterval(it.toInt()) }, valueRange = 15f..600f, steps = 38)
            Text("Ignore fixes worse than ±${ui.maxAcc} m", style = MaterialTheme.typography.bodyMedium)
            Slider(ui.maxAcc.toFloat(), { vm.setMaxAcc(it.toInt()) }, valueRange = 10f..300f, steps = 28)
            Text("Battery: the collector holds a foreground service only while enabled; each cycle is one Wi-Fi scan and one location request.", color = Slate, style = MaterialTheme.typography.bodySmall)
        }
        InfoCard("Home networks") {
            Text("Networks that travel with you (your router, hotspot). Matched against SSID and BSSID, one glob per line. Pulled from the desktop on sync.", color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedTextField(home, { home = it }, Modifier.fillMaxWidth(), minLines = 3, placeholder = { Text("MyRouter*\nAA:BB:CC:?D:EE:F?") })
            OutlinedButton(onClick = { vm.setHome(home) }) { Text("Save") }
        }
        InfoCard("Desktops") { OutlinedButton(onClick = onPair) { Text("Pair / manage desktops") } }
        InfoCard("Privacy") {
            Text("Everything stays on this phone and on the desktops you pair with. No analytics, no crash reporting, no third-party servers. The only other network traffic is map tiles (OpenStreetMap). Tokens are stored in Android's Keystore-backed encrypted preferences; the database is in app-private storage and excluded from backups.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Text("BeaconFix Android ${BuildConfig.VERSION_NAME} · GPL-2.0-or-later", color = Slate, style = MaterialTheme.typography.bodySmall)
        }
    }
}
