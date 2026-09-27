package org.sworrl.beaconfix.ui.screens

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
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.KeyValue
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.SyncViewModel

@Composable
fun SyncScreen(onPair: () -> Unit, vm: SyncViewModel = hiltViewModel()) {
    val ui by vm.ui.collectAsState()
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        InfoCard("Sync") {
            KeyValue("Observations waiting", "${ui.unsynced}")
            KeyValue("Observations total", "${ui.obs}")
            KeyValue("Beacons known", "${ui.aps}")
            KeyValue("Last sync", ago(ui.lastAt))
            if (ui.lastReport.isNotEmpty()) Text(ui.lastReport, color = Slate, style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { vm.syncNow() }, enabled = !ui.running && ui.desktops.any { it.paired }) { Text(if (ui.running) "Syncing…" else "Sync now") }
                OutlinedButton(onClick = onPair) { Text("Pair a desktop") }
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) { Text("Sync every 15 min in the background"); Switch(ui.autoSync, { vm.setAuto(it) }) }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) { Text("Only on unmetered networks"); Switch(ui.unmetered, { vm.setUnmetered(it) }) }
        }
        for (d in ui.desktops) InfoCard(d.name.ifEmpty { d.id }) {
            KeyValue("Address", "${d.host}:${d.port}${if (d.tls) " (TLS)" else ""}")
            KeyValue("Version", d.version.ifEmpty { "?" })
            KeyValue("Paired", if (d.paired) "yes · ${d.scopes}" else "no")
            KeyValue("Last sync", ago(d.lastSync))
            KeyValue("Pushed / pulled", "${d.pushedObs} obs / ${d.pulledAps} beacons")
            if (d.lastError.isNotEmpty()) Text(d.lastError, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            TextButton(onClick = { vm.forget(d) }) { Text("Forget this desktop") }
        }
    }
}
