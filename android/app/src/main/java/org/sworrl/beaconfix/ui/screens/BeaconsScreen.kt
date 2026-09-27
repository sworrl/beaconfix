package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.grade
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.BeaconsViewModel

@Composable
fun BeaconsScreen(vm: BeaconsViewModel = hiltViewModel()) {
    val aps by vm.aps.collectAsState()
    val q by vm.query.collectAsState()
    val sort by vm.sort.collectAsState()
    Column(Modifier.fillMaxSize()) {
        OutlinedTextField(q, { vm.query.value = it }, Modifier.fillMaxWidth().padding(16.dp, 8.dp), placeholder = { Text("Filter by name or BSSID") }, singleLine = true)
        Row(Modifier.padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            AssistChip(onClick = { vm.sort.value = "seen" }, label = { Text(if (sort == "seen") "• Last seen" else "Last seen") })
            AssistChip(onClick = { vm.sort.value = "name" }, label = { Text(if (sort == "name") "• Name" else "Name") })
            AssistChip(onClick = { vm.sort.value = "acc" }, label = { Text(if (sort == "acc") "• Best fix" else "Best fix") })
            TextButton(onClick = { vm.refitAll() }) { Text("Re-fit all") }
        }
        if (aps.isEmpty()) Text("No beacons yet.", Modifier.padding(16.dp), color = Slate)
        LazyColumn { items(aps, key = { it.bssid }) { BeaconRow(it) } }
    }
}

@Composable
private fun BeaconRow(a: ApEntity) {
    var open by remember { mutableStateOf(false) }
    val g = grade(a.security)
    Card(onClick = { open = !open }, modifier = Modifier.fillMaxWidth().padding(16.dp, 4.dp)) {
        Column(Modifier.padding(12.dp)) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text(a.ssid.ifEmpty { "(hidden)" }, style = MaterialTheme.typography.titleMedium)
                Text(g.label, color = g.color, style = MaterialTheme.typography.labelLarge)
            }
            Text("${a.bssid} · ${a.band} GHz ch ${a.ch} · seen ${a.timesSeen}× · ${ago(a.lastSeen)}", color = Slate, style = MaterialTheme.typography.bodySmall)
            Text(if (a.lat != null) "±${(a.acc ?: 0.0).toInt()} m · ${a.posSource}${a.residual?.let { " · fit residual ${it.toInt()} m" } ?: ""}${a.pathExp?.let { " · n=%.1f".format(it) } ?: ""}" else "no position yet",
                 color = if (a.lat != null) MaterialTheme.colorScheme.onSurface else Slate, style = MaterialTheme.typography.bodySmall)
            if (a.home) Text("home network — travels with you, never used for positioning", color = MaterialTheme.colorScheme.tertiary, style = MaterialTheme.typography.bodySmall)
            if (open) Text(g.why, Modifier.padding(top = 6.dp), style = MaterialTheme.typography.bodySmall)
        }
    }
}
