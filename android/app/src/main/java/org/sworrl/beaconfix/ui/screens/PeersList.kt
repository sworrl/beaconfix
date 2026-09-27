package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
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
import org.sworrl.beaconfix.ui.Chip
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.theme.Cyan
import org.sworrl.beaconfix.ui.theme.Gold
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.PeersViewModel

/**
 * "BeaconFix devices on this network": what mDNS, the desktops' peer lists and a subnet probe found, with the two
 * adjacency actions. Shown on first launch and on the Identity screen. [haveIdentity] switches the offers.
 */
@Composable
fun PeersList(haveIdentity: Boolean, vm: PeersViewModel = hiltViewModel()) {
    val rows by vm.rows.collectAsState(); val scanning by vm.scanning.collectAsState(); val note by vm.note.collectAsState()
    val busy by vm.busy.collectAsState(); val msg by vm.message.collectAsState()
    val importFrom by vm.importFrom.collectAsState(); val hint by vm.importHint.collectAsState()
    var manual by remember { mutableStateOf("") }
    InfoCard("BeaconFix devices on this network") {
        Text("Devices on your own network are usually yours. Pick one to bring its identity here, or to link the two.", color = Slate, style = MaterialTheme.typography.bodySmall)
        if (rows.isEmpty()) Text(if (scanning) "Scanning…" else "Nothing announced itself yet. Tap Scan the network, or type an address.", color = Slate, style = MaterialTheme.typography.bodySmall)
        for (r in rows) {
            val p = r.peer
            Column(Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Column(Modifier.weight(1f)) {
                        Text(p.identityName.ifEmpty { p.hostname.ifEmpty { p.host } }, style = MaterialTheme.typography.titleSmall)
                        Text("${p.hostname.ifEmpty { p.host }} · ${p.host}:${p.port} · v${p.version.ifEmpty { "?" }} · via ${p.via}", color = Slate, style = MaterialTheme.typography.bodySmall)
                    }
                    when (r.relation) { "same" -> Chip("SAME IDENTITY", Green); "different" -> Chip("DIFFERENT IDENTITY", Gold); else -> Chip("NO IDENTITY", Slate) }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (p.identityId.isNotEmpty() && r.relation != "same") Button(enabled = busy.isEmpty(), onClick = { vm.thatsMe(p) }) { Text("That's me — import") }
                    if (haveIdentity && r.relation == "different") OutlinedButton(enabled = busy.isEmpty(), onClick = { vm.linkWith(p) }) { Text("Link my identity with it") }
                    if (haveIdentity && r.relation == "same" && "identity" in p.features) OutlinedButton(enabled = busy.isEmpty(), onClick = { vm.signIn(p) }) { Text("Sign in") }
                    if (busy == p.key) CircularProgressIndicator(Modifier.padding(4.dp))
                }
            }
        }
        importFrom?.let { p ->
            var code by remember { mutableStateOf("") }; var pass by remember { mutableStateOf("") }
            Text("Import from ${p.identityName.ifEmpty { p.hostname }}", style = MaterialTheme.typography.titleSmall)
            Text(hint, color = Cyan, style = MaterialTheme.typography.bodySmall)
            OutlinedTextField(code, { code = it.filter(Char::isDigit).take(6) }, Modifier.fillMaxWidth(), label = { Text("6-digit code") }, singleLine = true)
            OutlinedTextField(pass, { pass = it }, Modifier.fillMaxWidth(), label = { Text("Passphrase / six-word code") }, singleLine = true)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) { Button(onClick = { vm.importWith(code, pass) }, enabled = code.length == 6 && pass.isNotBlank() && busy.isEmpty()) { Text("Import") }; TextButton(onClick = { vm.cancelImport() }) { Text("Cancel") } }
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { vm.scan() }, enabled = !scanning) { Text(if (scanning) "Scanning…" else "Scan the network") }
            OutlinedTextField(manual, { manual = it }, Modifier.weight(1f), label = { Text("or an address") }, singleLine = true)
            TextButton(onClick = { vm.probe(manual) }, enabled = manual.isNotBlank()) { Text("Add") }
        }
        if (note.isNotEmpty()) Text(note, color = Slate, style = MaterialTheme.typography.bodySmall)
        if (msg.isNotEmpty()) Text(msg, color = MaterialTheme.colorScheme.tertiary, style = MaterialTheme.typography.bodySmall)
    }
}
