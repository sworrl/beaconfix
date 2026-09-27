package org.sworrl.beaconfix.ui.screens

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.KeyValue
import org.sworrl.beaconfix.ui.Permissions
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.metres
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.HomeViewModel

@Composable
fun HomeScreen(onPair: () -> Unit, vm: HomeViewModel = hiltViewModel()) {
    val ui by vm.ui.collectAsState()
    val ctx = LocalContext.current
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { r -> if (r.values.all { it }) vm.toggleCollector(true) }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Text("BeaconFix", style = MaterialTheme.typography.headlineMedium, modifier = Modifier.padding(horizontal = 16.dp))
        Text("Wi-Fi beacons, mapped as you move", color = Slate, modifier = Modifier.padding(horizontal = 16.dp, vertical = 2.dp))

        InfoCard("Desktop fix") {
            val f = ui.desktopFix
            when {
                ui.desktops.none { it.paired } -> { Text("No desktop paired yet.", color = Slate); Button(onClick = onPair) { Text("Pair with a BeaconFix desktop") } }
                f == null -> Text(ui.desktopError.ifEmpty { "Fetching…" }, color = Slate)
                !f.valid -> Text("The desktop has no fix yet.", color = Slate)
                else -> {
                    Text(f.place.ifEmpty { "%.5f, %.5f".format(f.lat, f.lon) }, style = MaterialTheme.typography.titleLarge)
                    KeyValue("Accuracy", "±${metres(f.accuracy)} via ${f.source}${if (f.provider.isNotEmpty()) " / ${f.provider}" else ""}")
                    f.elevation?.let { KeyValue("Elevation", "${it.toInt()} m") }
                    f.home?.let { KeyValue("Home", if (it.atHome) "at home" else it.awayText.ifEmpty { "${"%.1f".format(it.awayKm)} km away" }) }
                    f.sun?.let { KeyValue("Sun", "${it.sunrise.takeLast(8).take(5)} – ${it.sunset.takeLast(8).take(5)}") }
                    if (ui.desktopError.isNotEmpty()) Text(ui.desktopError, color = MaterialTheme.colorScheme.error)
                    Row { TextButton(onClick = { vm.refreshDesktop() }) { Text("Refresh") } }
                }
            }
        }
        InfoCard("This phone") {
            val p = ui.phoneFix
            if (p == null) Text("No position recorded yet. Turn the collector on.", color = Slate)
            else {
                KeyValue("Position", "%.5f, %.5f".format(p.lat, p.lon))
                KeyValue("Accuracy", "±${metres(p.acc)} · ${p.source}${if (p.provider.isNotEmpty()) " (${p.provider})" else ""}")
                KeyValue("When", ago(p.time))
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween, modifier = Modifier.fillMaxWidth()) {
                Column { Text("Collect beacons in the background"); Text("Foreground service: scans Wi-Fi at your GPS position", color = Slate, style = MaterialTheme.typography.bodySmall) }
                Switch(checked = ui.collectorOn, onCheckedChange = { on -> if (on && !Permissions.hasForeground(ctx)) ask.launch(Permissions.foreground()) else vm.toggleCollector(on) })
            }
        }
        InfoCard("Database") {
            KeyValue("Beacons known", "${ui.aps}")
            KeyValue("With a position", "${ui.positioned}")
            KeyValue("Observations", "${ui.obs}")
            KeyValue("Waiting to sync", "${ui.unsynced}")
            Row { OutlinedButton(onClick = onPair) { Text("Desktops") }; Spacer(Modifier.width(8.dp)) }
        }
    }
}
