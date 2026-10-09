package org.sworrl.beaconfix.ui.screens

import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.QrCodeScanner
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.BuildConfig
import org.sworrl.beaconfix.link.HubLinkState
import org.sworrl.beaconfix.link.Link
import org.sworrl.beaconfix.link.LinkPhase
import org.sworrl.beaconfix.net.Bfs3
import org.sworrl.beaconfix.ui.Chip
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Red
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.LinkViewModel
import org.sworrl.beaconfix.ui.vm.LinkedRow
import org.sworrl.beaconfix.ui.vm.PcRow

/**
 * Link a PC (docs/LINKING.md) — the one way to connect this phone to a BeaconFix PC and, through it, the hub. Scan the
 * QR the PC shows, or tap the PC in the list found on this network and check that both screens show the same six
 * digits. Nothing is typed. [hintHost] (a beaconfix://pair link) only adds that address to the list; [autoLink]
 * (adb automation: debug builds / Developer automation) links with it at once.
 */
@Composable
fun LinkScreen(onBack: () -> Unit, hintHost: String? = null, hintPort: Int = 47822, autoLink: Boolean = false, vm: LinkViewModel = hiltViewModel()) {
    val phase by vm.phase.collectAsStateWithLifecycle(); val hub by vm.hub.collectAsStateWithLifecycle(); val msg by vm.message.collectAsStateWithLifecycle()
    val pcs by vm.pcs.collectAsStateWithLifecycle(); val linked by vm.linked.collectAsStateWithLifecycle(); val mdnsErr by vm.mdnsError.collectAsStateWithLifecycle()
    val hubStatus by vm.hubStatus.collectAsStateWithLifecycle()
    var scanning by remember { mutableStateOf(false) }
    DisposableEffect(Unit) { vm.browse(true); onDispose { vm.browse(false) } }
    LaunchedEffect(hintHost, hintPort, autoLink) { if (!hintHost.isNullOrBlank()) vm.hint(hintHost, hintPort, autoLink) }
    if (scanning) {
        BackHandler { scanning = false }
        Box(Modifier.fillMaxSize()) {
            QrScanner("Point the camera at the QR in BeaconFix on your PC (Link a device). A hub invite QR works too.", onResult = { scanning = false; vm.handleScanned(it) })
            TextButton(onClick = { scanning = false }, modifier = Modifier.align(Alignment.TopStart).padding(8.dp)) { Text("‹ Cancel") }
        }
        return
    }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("‹ Back") }
            Text("Link a PC", style = MaterialTheme.typography.headlineSmall)
        }
        PhaseCard(phase, onCancel = { vm.cancel() }, onDone = { vm.dismiss() })
        HubCard(hub, onRetry = { vm.retryHub() })
        // Linked to a PC but not to its hub (an app-data reset keeps neither, a forgotten hub keeps the PC): no new QR needed
        if (hubStatus.loaded && hubStatus.config == null && linked.isNotEmpty() && hub.phase != HubLinkState.Phase.ENROLLING && hub.phase != HubLinkState.Phase.WAITING)
            OutlinedButton(onClick = { vm.enrolThroughLinkedPc() }, modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp)) { Text("Join the hub through a linked PC") }
        if (msg.isNotEmpty()) Text(msg, Modifier.padding(horizontal = 20.dp, vertical = 4.dp), color = MaterialTheme.colorScheme.tertiary, style = MaterialTheme.typography.bodyMedium)

        val busy = phase is LinkPhase.Contacting || phase is LinkPhase.Confirm
        Button(onClick = { scanning = true }, enabled = !busy, modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp).height(72.dp)) {
            Icon(Icons.Default.QrCodeScanner, contentDescription = null, modifier = Modifier.size(32.dp))
            Spacer(Modifier.width(12.dp))
            Text("Scan QR", fontSize = 22.sp, fontWeight = FontWeight.Bold)
        }
        Text("On the PC: BeaconFix → Link a device, then scan the QR it shows. Or tap the PC below and confirm the same code on both screens.",
            Modifier.padding(horizontal = 20.dp), color = Slate, style = MaterialTheme.typography.bodySmall)

        InfoCard("PCs on this network") {
            if (pcs.isEmpty()) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                    Text("Searching (mDNS _beaconfix._tcp)… The PC must run BeaconFix on the same Wi-Fi.", color = Slate, style = MaterialTheme.typography.bodySmall)
                }
            }
            if (mdnsErr.isNotEmpty()) Text("$mdnsErr — scan the QR instead.", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            for (r in pcs) PcItem(r, enabled = !busy, onClick = { vm.linkWith(r) })
        }

        InfoCard("Linked PCs (${linked.size})") {
            if (linked.isEmpty()) Text("None yet.", color = Slate, style = MaterialTheme.typography.bodySmall)
            for (l in linked) LinkedItem(l, onUnlink = { vm.unlink(l.desktop.id) })
        }

        if (BuildConfig.DEBUG) DebugPaste(onUse = { vm.handleScanned(it) })
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun PhaseCard(phase: LinkPhase, onCancel: () -> Unit, onDone: () -> Unit) {
    when (phase) {
        is LinkPhase.Idle -> {}
        is LinkPhase.Contacting -> InfoCard(phase.pc) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 3.dp); Text(phase.note, Modifier.weight(1f)); TextButton(onClick = onCancel) { Text("Cancel") }
            }
        }
        is LinkPhase.Confirm -> InfoCard(if (phase.viaQr) "Linking with ${phase.pc}…" else "Confirm the same code on ${phase.pc}") {
            Text(Link.grouped(phase.code), Modifier.fillMaxWidth(), textAlign = TextAlign.Center, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold,
                fontSize = 52.sp, letterSpacing = 4.sp)
            Text(if (phase.viaQr) "${phase.pc} approves by itself — you scanned its screen. It shows the same code."
                 else "Tap Link on ${phase.pc} only if it shows this same code. Different code? Tap Reject there.",
                color = Slate, style = MaterialTheme.typography.bodyMedium)
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                Text("Waiting · ${phase.secondsLeft / 60}:${(phase.secondsLeft % 60).toString().padStart(2, '0')}", color = Slate, style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(1f))
                TextButton(onClick = onCancel) { Text("Cancel") }
            }
        }
        is LinkPhase.Linked -> InfoCard("Linked ✓ ${phase.pc} · code ${Link.grouped(phase.code)}") {
            Text("This phone now reads and controls ${phase.pc} on this network, and syncs with it.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Button(onClick = onDone) { Text("Done") }
        }
        is LinkPhase.Failed -> InfoCard("Not linked") {
            Text(phase.message, color = MaterialTheme.colorScheme.error)
            TextButton(onClick = onDone) { Text("OK") }
        }
    }
}

@Composable
private fun HubCard(h: HubLinkState, onRetry: () -> Unit) {
    if (h.phase == HubLinkState.Phase.IDLE && h.text.isEmpty()) return
    InfoCard("Hub") {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            when (h.phase) {
                HubLinkState.Phase.ENROLLING -> CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                HubLinkState.Phase.ENROLLED -> Text("✓", color = Green, fontWeight = FontWeight.Bold)
                HubLinkState.Phase.WAITING -> Text("…", color = MaterialTheme.colorScheme.tertiary, fontWeight = FontWeight.Bold)
                HubLinkState.Phase.FAILED -> Text("✕", color = Red, fontWeight = FontWeight.Bold)
                HubLinkState.Phase.IDLE -> {}
            }
            Text(h.text, Modifier.weight(1f), color = if (h.phase == HubLinkState.Phase.FAILED) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface)
        }
        if (h.fingerprint.isNotEmpty()) Text("Hub fingerprint ${Bfs3.groupedFingerprint(h.fingerprint)}", color = Slate, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
        if (h.phase == HubLinkState.Phase.WAITING) OutlinedButton(onClick = onRetry) { Text("Retry now") }
    }
}

@Composable
private fun PcItem(r: PcRow, enabled: Boolean, onClick: () -> Unit) {
    val pc = r.pc
    Row(Modifier.fillMaxWidth().clickable(enabled = enabled, onClick = onClick).padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Column(Modifier.weight(1f)) {
            Text(pc.txt["name"]?.takeIf { it.isNotBlank() } ?: r.reach?.hello?.pcName?.takeIf { it.isNotBlank() } ?: pc.displayName, style = MaterialTheme.typography.titleMedium)
            val host = (r.reach?.host ?: pc.host) + ":" + pc.port
            val ver = r.reach?.hello?.version?.takeIf { it.isNotEmpty() }?.let { " · v$it" } ?: ""
            Text("${pc.hostname.takeIf { it.isNotEmpty() && it != pc.displayName }?.let { "$it · " } ?: ""}$host$ver", color = Slate, style = MaterialTheme.typography.bodySmall)
            when {
                !r.checked -> Text("checking…", color = Slate, style = MaterialTheme.typography.labelSmall)
                r.reach != null -> Text("● reachable · ${r.reach.rttMs} ms", color = Green, style = MaterialTheme.typography.labelSmall)
                else -> Text("○ seen on mDNS but not answering", color = Red, style = MaterialTheme.typography.labelSmall)
            }
        }
        if (r.linkedId != null) Chip("LINKED", Green)
        OutlinedButton(onClick = onClick, enabled = enabled) { Text(if (r.linkedId != null) "Link again" else "Link") }
    }
}

@Composable
private fun LinkedItem(l: LinkedRow, onUnlink: () -> Unit) {
    var confirm by remember { mutableStateOf(false) }
    val d = l.desktop
    Row(Modifier.fillMaxWidth().padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Column(Modifier.weight(1f)) {
            Text(d.name.ifEmpty { d.hostname.ifEmpty { d.host } }, style = MaterialTheme.typography.titleMedium)
            Text("${d.host}:${d.port}${l.meta?.code?.takeIf { it.isNotEmpty() }?.let { " · code ${Link.grouped(it)}" } ?: ""}", color = Slate, style = MaterialTheme.typography.bodySmall)
            val status = when {
                !l.checked -> "checking…" to Slate
                l.reach != null -> "● online · ${l.reach.rttMs} ms" to Green
                else -> "○ not on this network right now" to Slate
            }
            Text(status.first, color = status.second, style = MaterialTheme.typography.labelSmall)
            Text("linked ${ago(l.meta?.linkedAt ?: 0)} · last sync ${ago(d.lastSync)}", color = Slate, style = MaterialTheme.typography.labelSmall)
            if (d.lastError.isNotEmpty()) Text(d.lastError, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.labelSmall)
        }
        TextButton(onClick = { confirm = true }) { Text("Unlink") }
    }
    if (confirm) AlertDialog(onDismissRequest = { confirm = false },
        title = { Text("Unlink ${d.name}?") },
        text = { Text("This phone forgets its token for this PC. Remove the phone on the PC under Devices as well.") },
        confirmButton = { TextButton(onClick = { confirm = false; onUnlink() }) { Text("Unlink") } },
        dismissButton = { TextButton(onClick = { confirm = false }) { Text("Keep") } })
}

/** Debug builds only: paste a bflink:/bfs3: text (an emulator has no camera pointed at a PC). */
@Composable
private fun DebugPaste(onUse: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }; var text by remember { mutableStateOf("") }
    TextButton(onClick = { open = !open }, modifier = Modifier.padding(horizontal = 12.dp)) { Text("Debug: paste a link text", color = Slate) }
    if (open) Column(Modifier.padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        OutlinedTextField(text, { text = it }, Modifier.fillMaxWidth(), label = { Text("bflink:… or bfs3:…") }, minLines = 2)
        OutlinedButton(onClick = { onUse(text); text = ""; open = false }, enabled = text.isNotBlank()) { Text("Use") }
    }
}
