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
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import org.sworrl.beaconfix.sync.PairState
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.KeyValue
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.PairViewModel

@Composable
fun PairScreen(onDone: () -> Unit, vm: PairViewModel = hiltViewModel()) {
    val st by vm.state.collectAsState()
    val found by vm.found.collectAsState()
    var host by remember { mutableStateOf("") }
    var port by remember { mutableStateOf("47822") }
    var tls by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        InfoCard("Desktops on this network") {
            if (found.isEmpty()) Text("Searching for _beaconfix._tcp… (the desktop advertises itself only when avahi is installed; otherwise type its address below)", color = Slate, style = MaterialTheme.typography.bodySmall)
            for (f in found) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                Column { Text(f.name); Text("${f.host}:${f.port}${if (f.pairingOpen) " · pairing open" else ""}", color = Slate, style = MaterialTheme.typography.bodySmall) }
                TextButton(onClick = { host = f.host; port = f.port.toString(); vm.probe(f.host, f.port, false) }) { Text("Use") }
            }
        }
        InfoCard("Address") {
            OutlinedTextField(host, { host = it }, Modifier.fillMaxWidth(), label = { Text("Host or IP") }, singleLine = true)
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(port, { port = it.filter(Char::isDigit) }, Modifier.weight(1f), label = { Text("Port") }, singleLine = true)
                Checkbox(tls, { tls = it }); Text("TLS")
            }
            Button(onClick = { vm.probe(host, port.toIntOrNull() ?: 47822, tls) }, enabled = host.isNotBlank()) { Text("Connect") }
        }
        when (val s = st) {
            is PairState.Idle -> {}
            is PairState.Probing -> InfoCard("Connecting to ${s.host}…") { CircularProgressIndicator() }
            is PairState.Found -> InfoCard("Found ${s.hello.hostname}") {
                KeyValue("BeaconFix", s.hello.version); KeyValue("Pairing", if (s.hello.pairing) "open" else "closed")
                if (s.desktop.paired) Text("Already paired.", color = Slate)
                if (!s.hello.pairing) Text("Open pairing on the desktop first: Devices tab → \"Allow pairing for 10 minutes\", the tray menu, or `beaconfix --pairing 10`.", color = MaterialTheme.colorScheme.tertiary, style = MaterialTheme.typography.bodySmall)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) { Button(onClick = { vm.pair() }) { Text(if (s.desktop.paired) "Pair again" else "Pair") }; if (s.desktop.paired) OutlinedButton(onClick = onDone) { Text("Done") } }
            }
            is PairState.WaitingApproval -> InfoCard("Approve on the desktop") {
                Text(s.code, style = MaterialTheme.typography.displayMedium)
                Text("This code is shown in the desktop's notification and Devices tab. Approve it there (a known device is approved automatically). Polling… (${s.polls})", color = Slate, style = MaterialTheme.typography.bodySmall)
                CircularProgressIndicator()
            }
            is PairState.Paired -> InfoCard("Paired with ${s.desktop.name}") { Text("Scopes: ${s.desktop.scopes}"); Button(onClick = onDone) { Text("Done") } }
            is PairState.Failed -> InfoCard("Not paired") { Text(s.message, color = MaterialTheme.colorScheme.error); TextButton(onClick = { vm.reset() }) { Text("Try again") } }
        }
    }
}
