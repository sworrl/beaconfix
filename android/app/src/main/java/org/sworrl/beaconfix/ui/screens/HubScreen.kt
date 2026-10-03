package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.net.Bfs3
import org.sworrl.beaconfix.net.HubErrors
import org.sworrl.beaconfix.net.HubStatus
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.KeyValue
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.HubViewModel

/**
 * The hub (docs/SECURE-API.md): what it does for this phone — sync, live position, every node it knows. Enrolment is
 * part of linking a PC (docs/LINKING.md): the PC hands this phone a hub invite over the authenticated link, so nothing
 * is pasted or compared by eye; a hub's own invite QR is scanned on the same Link screen ([onLink]).
 */
@Composable
fun HubScreen(onBack: () -> Unit, onLink: () -> Unit = {}, vm: HubViewModel = hiltViewModel()) {
    val st by vm.status.collectAsState(); val busy by vm.busy.collectAsState(); val msg by vm.message.collectAsState()
    val cfg = st.config
    DisposableEffect(cfg != null) { vm.live(cfg != null); onDispose { vm.live(false) } }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("‹ Back") }
            Text("Hub", style = MaterialTheme.typography.headlineSmall)
            if (busy) CircularProgressIndicator(Modifier.padding(start = 12.dp).size(20.dp), strokeWidth = 2.dp)
        }
        if (msg.isNotEmpty()) Text(msg, Modifier.padding(horizontal = 20.dp, vertical = 4.dp), style = MaterialTheme.typography.bodyMedium)
        if (cfg == null) NotEnrolled(onLink) else Enrolled(st, vm, onLink)
    }
}

@Composable
private fun NotEnrolled(onLink: () -> Unit) {
    InfoCard("Not enrolled") {
        Text("The hub holds the master database and is reached only over WireGuard. Once enrolled, this phone syncs with it " +
            "(its observations and Wi-Fi RTT ranges go up, everyone's come down), publishes its live position, and the map shows " +
            "every node. Paired desktops on the LAN are the fallback while no hub is set.", color = Slate, style = MaterialTheme.typography.bodySmall)
        Text("Link this phone to a PC that is enrolled with the hub: the PC passes on a hub invite and this phone enrols by itself. " +
            "A headless hub's invite QR (beaconfix --server --invite) can be scanned on the same screen.", style = MaterialTheme.typography.bodySmall)
        Button(onClick = onLink) { Text("Link a PC") }
    }
}

@Composable
private fun Enrolled(st: HubStatus, vm: HubViewModel, onLink: () -> Unit) {
    val cfg = st.config ?: return
    val queued by vm.queued.collectAsState(); val devices by vm.devices.collectAsState(); val streaming by vm.streaming.collectAsState()
    var editUrl by remember { mutableStateOf(false) }; var url by remember(cfg.url) { mutableStateOf(cfg.url) }
    var confirmForget by remember { mutableStateOf(false) }
    LaunchedEffect(cfg.deviceId) { if (st.reachable == null) vm.test() }
    InfoCard("Enrolled as ${cfg.name}") {
        HubReachability(st)
        KeyValue("Hub", cfg.url.removeSuffix("/"))
        KeyValue("Device id", cfg.deviceId)
        Text("Fingerprint (pinned)", color = Slate, style = MaterialTheme.typography.bodyMedium)
        Text(Bfs3.groupedFingerprint(cfg.fingerprint), fontFamily = FontFamily.Monospace)
        KeyValue("Enrolled", ago(cfg.enrolledAt))
        KeyValue("Last sync", ago(st.lastSyncAt))
        if (st.lastSyncText.isNotEmpty()) Text(st.lastSyncText, color = Slate, style = MaterialTheme.typography.bodySmall)
        KeyValue("Queued observations", "$queued")
        KeyValue("Live position sent", ago(st.lastPublishAt))
        KeyValue("Event stream", if (streaming) "open" else "polling every 15 s")
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { vm.syncNow() }) { Text("Sync now") }
            OutlinedButton(onClick = { vm.test() }) { Text("Test") }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (org.sworrl.beaconfix.BuildConfig.DEBUG) TextButton(onClick = { editUrl = !editUrl }) { Text("Change address") }
            TextButton(onClick = onLink) { Text("Enrol again (link a PC)") }
            TextButton(onClick = { confirmForget = true }) { Text("Forget hub") }
        }
        if (editUrl) {
            OutlinedTextField(url, { url = it }, Modifier.fillMaxWidth(), label = { Text("Hub address (https only)") }, singleLine = true)
            Button(onClick = { vm.setUrl(url); editUrl = false }) { Text("Save address") }
        }
    }
    InfoCard("Nodes (${devices.size})") {
        if (devices.isEmpty()) Text("No other node has reported a position yet.", color = Slate, style = MaterialTheme.typography.bodySmall)
        for (d in devices.sortedBy { it.ageS ?: Double.MAX_VALUE }) {
            val age = d.ageS?.let { a -> if (a < 90) "now" else if (a < 3600) "${(a / 60).toInt()} min" else if (a < 86400) "${(a / 3600).toInt()} h" else "${(a / 86400).toInt()} d" } ?: "?"
            KeyValue("${d.device} (${d.kind.ifEmpty { "device" }})", if (d.lat == 0.0 && d.lon == 0.0) "no position" else "±${d.acc.toInt()} m · $age${if (!d.online) " · offline" else ""}")
        }
    }
    if (confirmForget) AlertDialog(onDismissRequest = { confirmForget = false },
        title = { Text("Forget the hub?") },
        text = { Text("This phone's hub key is erased; queued data stays on the phone and goes to the LAN desktop again. Revoke ${cfg.deviceId} at the hub as well.") },
        confirmButton = { TextButton(onClick = { confirmForget = false; vm.forget() }) { Text("Forget") } },
        dismissButton = { TextButton(onClick = { confirmForget = false }) { Text("Keep") } })
}

/** Reachable / unreachable ("is WireGuard on?") / the last error, in one line. */
@Composable
fun HubReachability(st: HubStatus) {
    val (text, bad) = when {
        st.reachable == false -> (st.lastError.ifEmpty { HubErrors.UNREACHABLE } + " · data is queued") to true
        st.lastError.isNotEmpty() -> st.lastError to true
        st.reachable == true -> "reachable · last contact ${ago(st.lastContactAt)}" to false
        else -> "not tried yet" to false
    }
    Text(text, color = if (bad) MaterialTheme.colorScheme.error else Slate, style = MaterialTheme.typography.bodyMedium)
}

/** The Sync screen's hub card: what the hub sync did, and the way to the Hub screen. */
@Composable
fun HubSyncCard(onOpen: () -> Unit, vm: HubViewModel = hiltViewModel()) {
    val st by vm.status.collectAsState()
    val cfg = st.config
    InfoCard(if (cfg != null) "Hub — the sync target" else "Hub") {
        if (cfg == null) Text("No hub enrolled: syncing with paired desktops on the LAN.", color = Slate, style = MaterialTheme.typography.bodySmall)
        else {
            HubReachability(st)
            KeyValue("Hub", cfg.url.removeSuffix("/").removePrefix("https://"))
            KeyValue("Last sync", ago(st.lastSyncAt))
            if (st.lastSyncText.isNotEmpty()) Text(st.lastSyncText, color = Slate, style = MaterialTheme.typography.bodySmall)
        }
        TextButton(onClick = onOpen) { Text(if (cfg == null) "About the hub" else "Hub details") }
    }
}
