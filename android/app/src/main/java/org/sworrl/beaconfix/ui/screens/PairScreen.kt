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
fun PairScreen(onDone: () -> Unit, autoHost: String? = null, autoPort: Int = 47822, vm: PairViewModel = hiltViewModel()) {
    val st by vm.state.collectAsState()
    val found by vm.found.collectAsState()
    val idNote by vm.identityNote.collectAsState()
    var host by remember { mutableStateOf(autoHost ?: "") }
    var port by remember { mutableStateOf(autoPort.toString()) }
    androidx.compose.runtime.LaunchedEffect(autoHost) { if (!autoHost.isNullOrBlank()) vm.probe(autoHost, autoPort, false) }
    androidx.compose.runtime.LaunchedEffect(st) { if (!autoHost.isNullOrBlank() && st is PairState.Found && !(st as PairState.Found).desktop.paired) vm.pair() }
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
                if ("identity" in s.hello.features) Text("This desktop supports identity login: if it is yours (same or linked identity) no code is needed.", color = Slate, style = MaterialTheme.typography.bodySmall)
                if (idNote.isNotEmpty()) Text(idNote, color = MaterialTheme.colorScheme.tertiary, style = MaterialTheme.typography.bodySmall)
                if (!s.hello.pairing) Text("Open pairing on the desktop first: Devices tab → \"Allow pairing for 10 minutes\", the tray menu, or `beaconfix --pairing 10`.", color = MaterialTheme.colorScheme.tertiary, style = MaterialTheme.typography.bodySmall)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) { Button(onClick = { vm.pair() }) { Text(if (s.desktop.paired) "Pair again" else "Pair") }; if (s.desktop.paired) OutlinedButton(onClick = onDone) { Text("Done") } }
            }
            is PairState.WaitingApproval -> {
                s.proximity?.let { p -> InfoCard("Are we next to each other?") {
                    val (label, col) = when (p.verdict) { "adjacent" -> "ADJACENT" to org.sworrl.beaconfix.ui.theme.Green; "room" -> "SAME ROOM" to org.sworrl.beaconfix.ui.theme.Cyan; "near" -> "NEAR" to org.sworrl.beaconfix.ui.theme.Gold; "far" -> "FAR" to org.sworrl.beaconfix.ui.theme.Red; else -> "UNKNOWN" to Slate }
                    vm.ranged(s.desktop.id)?.let { r -> KeyValue("Measured range", "${org.sworrl.beaconfix.ranging.RangeSession.fmtM(r.distanceM)} (${org.sworrl.beaconfix.ranging.RangeSession.methodName(r.method)} ±${org.sworrl.beaconfix.ranging.RangeSession.fmtM(r.sigmaM)}) — adjacent means under 2 m") }
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) { org.sworrl.beaconfix.ui.Chip(label, col); Text("${s.desktop.name} hears ${p.shared} of the ${p.theirs.coerceAtLeast(p.shared)} beacons this phone hears", style = MaterialTheme.typography.bodyMedium) }
                    if (p.strongestShared.isNotEmpty()) KeyValue("Strongest shared", p.strongestShared.joinToString(", "))
                    p.distanceM?.let { KeyValue("Distance between fixes", org.sworrl.beaconfix.ui.metres(it)) }
                    p.rssiDelta?.let { KeyValue("Signal difference", "${it.toInt()} dB median") }
                    when (p.verdict) {
                        "far" -> Text("These two devices do not look like they are in the same place: no shared beacons and/or the fixes are far apart. The desktop will not approve unless its owner overrides it. If you ARE next to it, wait for a Wi-Fi scan and try again.", color = org.sworrl.beaconfix.ui.theme.Red, style = MaterialTheme.typography.bodySmall)
                        "unknown" -> Text("Not enough radio evidence yet (no scan or no fix on one side). The desktop shows the code and pictures anyway.", color = Slate, style = MaterialTheme.typography.bodySmall)
                        else -> Text("Devices that hear the same beacons at similar strength are next to each other.", color = Slate, style = MaterialTheme.typography.bodySmall)
                    }
                } }
                InfoCard(if (s.pictures.isNotEmpty()) "Tap the same three pictures on ${s.desktop.name}" else "Approve on the desktop") {
                    if (s.pictures.isNotEmpty()) {
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) { for (i in s.pictures) Column(horizontalAlignment = Alignment.CenterHorizontally) { Text(org.sworrl.beaconfix.sync.Sas.EMOJI[i], style = MaterialTheme.typography.displayLarge); Text(org.sworrl.beaconfix.sync.Sas.ICONS[i], color = Slate, style = MaterialTheme.typography.labelSmall) } }
                        Text("The desktop shows three sets of pictures; only this one is right. Picking a wrong set denies the request.", color = Slate, style = MaterialTheme.typography.bodySmall)
                        Text("Fallback code: ${s.code}", color = Slate, style = MaterialTheme.typography.bodyMedium)
                    } else {
                        Text(s.code, style = MaterialTheme.typography.displayMedium)
                        Text("This code is shown in the desktop's notification and Devices tab. Approve it there (a known device is approved automatically).", color = Slate, style = MaterialTheme.typography.bodySmall)
                    }
                    if (s.picked == true) Text("Pictures picked — finishing…", color = org.sworrl.beaconfix.ui.theme.Green)
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) { CircularProgressIndicator(); Text("Waiting… (${s.polls})", color = Slate, style = MaterialTheme.typography.bodySmall); TextButton(onClick = { vm.cancel() }) { Text("Cancel") } }
                }
            }
            is PairState.Paired -> InfoCard("Paired with ${s.desktop.name}") { Text("Scopes: ${s.desktop.scopes}"); Button(onClick = onDone) { Text("Done") } }
            is PairState.Failed -> InfoCard("Not paired") { Text(s.message, color = MaterialTheme.colorScheme.error); TextButton(onClick = { vm.reset() }) { Text("Try again") } }
        }
    }
}
