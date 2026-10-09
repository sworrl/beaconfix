package org.sworrl.beaconfix.ui.screens

import androidx.lifecycle.compose.collectAsStateWithLifecycle
import android.content.Intent
import android.graphics.Bitmap
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.identity.Crypto
import org.sworrl.beaconfix.identity.Qr
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.KeyValue
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.IdentityViewModel

/** First launch: no identity yet. Create one or bring one over from another BeaconFix. */
@Composable
fun OnboardingScreen(initialName: String? = null, onImportHistory: (() -> Unit)? = null, onLinkPc: (() -> Unit)? = null, vm: IdentityViewModel = hiltViewModel()) {
    val busy by vm.busy.collectAsStateWithLifecycle(); val msg by vm.message.collectAsStateWithLifecycle(); val staged by vm.importBundle.collectAsStateWithLifecycle()
    var name by remember { mutableStateOf(initialName ?: "") }
    var mode by remember { mutableStateOf("choose") }     // choose | scan | paste (debug builds)
    var pass by remember { mutableStateOf("") }
    val openFile = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> uri?.let { vm.importFromFile(it) } }
    LaunchedEffect(initialName) { if (!initialName.isNullOrBlank()) vm.create(initialName) }
    if (mode == "scan") { QrScanner("Point the camera at the identity QR shown by another BeaconFix", onResult = { vm.handleScanned(it); mode = "choose" }); return }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 16.dp)) {
        Text("Who are you?", style = MaterialTheme.typography.headlineMedium, modifier = Modifier.padding(horizontal = 16.dp))
        Text("BeaconFix keeps everything you learn under one identity — a key pair that lives on each of your devices. Bring yours over from a device on this network, or make a new one.", color = Slate, modifier = Modifier.padding(16.dp))
        if (onLinkPc != null && staged.isEmpty()) InfoCard("Link to your PC") {
            Text("BeaconFix on your PC: scan its QR, or pick it from the list on this network and check the same code on both screens. Nothing to type. " +
                "Linking also enrols this phone with your hub when the PC has one.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Button(onClick = { vm.create(name); onLinkPc() }, enabled = !busy) { Text("Link a PC") }
        }
        if (staged.isNotEmpty()) InfoCard("Identity bundle received") {
            Text("Enter the passphrase or the six-word code shown where it was exported.", color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedTextField(pass, { pass = it }, Modifier.fillMaxWidth(), label = { Text("Passphrase / 6-word code") }, singleLine = true)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) { Button(onClick = { vm.importWithPassphrase(pass) }, enabled = !busy && pass.isNotBlank()) { Text("Import") }; TextButton(onClick = { vm.stageImport("") }) { Text("Cancel") } }
        } else {
            InfoCard("Create a new identity") {
                OutlinedTextField(name, { name = it }, Modifier.fillMaxWidth(), label = { Text("Your name (shown on your devices)") }, singleLine = true)
                Button(onClick = { vm.create(name) }, enabled = !busy && name.isNotBlank()) { Text("Create") }
            }
            InfoCard("Import an existing identity") {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { mode = "scan" }) { Text("Scan QR") }
                    OutlinedButton(onClick = { openFile.launch(arrayOf("*/*")) }) { Text("Open file") }
                    if (org.sworrl.beaconfix.BuildConfig.DEBUG) TextButton(onClick = { mode = "paste" }) { Text("Paste (debug)") }
                }
                if (mode == "paste" && org.sworrl.beaconfix.BuildConfig.DEBUG) { var t by remember { mutableStateOf("") }; OutlinedTextField(t, { t = it }, Modifier.fillMaxWidth(), label = { Text("BFID1:…") }, minLines = 3); Button(onClick = { vm.handleScanned(t) }, enabled = t.isNotBlank()) { Text("Use") } }
            }
        }
        if (onImportHistory != null) InfoCard("Import your history (optional)") {
            Text("Already have a Google Timeline export, Takeout, WiGLE or GPX files? BeaconFix can turn them into beacons and a track — once your identity exists, under Settings → Import.", color = Slate, style = MaterialTheme.typography.bodySmall)
        }
        if (busy) CircularProgressIndicator(Modifier.padding(16.dp))
        if (msg.isNotEmpty()) Text(msg, color = MaterialTheme.colorScheme.tertiary, modifier = Modifier.padding(16.dp))
    }
}

/** Settings → Identity: show, export, link, forget. */
@Composable
fun IdentityScreen(onBack: () -> Unit, onLink: () -> Unit = {}, vm: IdentityViewModel = hiltViewModel()) {
    val rec by vm.identity.collectAsStateWithLifecycle(); val pending by vm.pending.collectAsStateWithLifecycle()
    val busy by vm.busy.collectAsStateWithLifecycle(); val msg by vm.message.collectAsStateWithLifecycle()
    val exportText by vm.exportText.collectAsStateWithLifecycle(); val exportCode by vm.exportCode.collectAsStateWithLifecycle()
    val offer by vm.offerText.collectAsStateWithLifecycle(); val statement by vm.statementText.collectAsStateWithLifecycle(); val staged by vm.importBundle.collectAsStateWithLifecycle()
    val incoming by vm.incomingLink.collectAsStateWithLifecycle()
    val ctx = LocalContext.current; val clip = LocalClipboardManager.current
    var mode by remember { mutableStateOf("view") }   // view | scan
    var pass by remember { mutableStateOf("") }; var newName by remember(rec?.name) { mutableStateOf(rec?.name ?: "") }
    val saveFile = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/plain")) { uri -> uri?.let { vm.writeExport(it) } }
    if (mode == "scan") { QrScanner("Scan a link QR from another BeaconFix, or an identity QR", onResult = { vm.handleScanned(it); mode = "view" }); return }
    val r = rec
    if (r == null) {
        // The record flow's first frame is always null (stateIn's initial value) — that is "still loading", not "no
        // identity" (the root shows onboarding for that). Never nest the full-screen, scrolling OnboardingScreen in
        // this screen's own verticalScroll: Compose throws on the infinite height.
        Column(Modifier.fillMaxSize().padding(vertical = 8.dp)) {
            Row(Modifier.padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) { TextButton(onClick = onBack) { Text("‹ Back") }; Text("Identity", style = MaterialTheme.typography.titleLarge) }
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
        }
        return
    }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Row(Modifier.padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) { TextButton(onClick = onBack) { Text("‹ Back") }; Text("Identity", style = MaterialTheme.typography.titleLarge) }
        if (incoming.isNotEmpty()) InfoCard("A link was opened") {
            Text(vm.describeIncoming(incoming), style = MaterialTheme.typography.bodyMedium)
            Text("Only say yes if you started this yourself, from your own other device.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { vm.confirmIncoming() }, enabled = !busy) { Text("Link") }
                OutlinedButton(onClick = { vm.dismissIncoming() }) { Text("Cancel") }
            }
        }
        InfoCard(r.name) {
            Text(Crypto.grouped(r.id), fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyLarge)
            KeyValue("Created", r.created.take(10)); KeyValue("Public key", r.pub.take(16) + "…")
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) { OutlinedTextField(newName, { newName = it }, Modifier.weight(1f), label = { Text("Name") }, singleLine = true); TextButton(onClick = { vm.rename(newName) }, enabled = newName.isNotBlank() && newName != r.name) { Text("Rename") } }
        }
        InfoCard("Devices (${r.devices.size})") {
            for (d in r.devices) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) { Column { Text(d.name); Text("${d.kind} · added ${d.added.take(10)}", color = Slate, style = MaterialTheme.typography.bodySmall) }; TextButton(onClick = { vm.forgetDevice(d.name) }) { Text("Forget") } }
        }
        InfoCard("Links (${r.links.count { it.complete }})") {
            if (r.links.isEmpty() && pending.isEmpty()) Text("No linked identities. Linking merges two identities made independently — data from both counts as yours, in every direction.", color = Slate, style = MaterialTheme.typography.bodySmall)
            for (l in r.links) Text("${Crypto.grouped(l.other(r.id))} · ${l.ts.take(10)}", fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
            for (p in pending) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) { Text("pending: ${p.name.ifEmpty { Crypto.grouped(p.id) }}"); Text("waiting for the other side to co-sign", color = Slate, style = MaterialTheme.typography.bodySmall) }
                TextButton(onClick = { vm.statementText.value = vm.statementFor(p) }) { Text("Show QR") }; TextButton(onClick = { vm.dismissPending(p.id) }) { Text("Dismiss") }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) { OutlinedButton(onClick = { vm.showOffer() }) { Text("Show my link QR") }; OutlinedButton(onClick = { mode = "scan" }) { Text("Scan") } }
            Text("To link: one side shows its link QR, the other scans it. If the shown side is a paired desktop the link completes over the LAN; otherwise the scanner shows a QR back for the first side to scan. To connect a PC (its live view, sync and the hub), use Link a PC below.", color = Slate, style = MaterialTheme.typography.bodySmall)
            if (offer.isNotEmpty()) QrBlock("My link QR", offer, onClose = { vm.clearLinkUi() })
            if (statement.isNotEmpty()) QrBlock("Link statement — scan this on the other side", statement, onClose = { vm.clearLinkUi() })
        }
        InfoCard("PCs") {
            Text("Connect this phone to BeaconFix on a PC — and through it to your hub — by scanning its QR or picking it on this network. Nothing to type.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Button(onClick = onLink) { Text("Link a PC") }
        }
        InfoCard("Export this identity") {
            Text("Moves your identity (its private key included) to another BeaconFix. Protected by a passphrase you choose, or a six-word code shown once.", color = Slate, style = MaterialTheme.typography.bodySmall)
            if (exportText.isEmpty()) {
                OutlinedTextField(pass, { pass = it }, Modifier.fillMaxWidth(), label = { Text("Passphrase (≥ 8) — or leave empty for a 6-word code") }, singleLine = true)
                Button(onClick = { vm.export(pass.ifBlank { null }) }, enabled = !busy) { Text("Export") }
            } else {
                if (exportCode.isNotEmpty()) { Text("Your one-time code — type it where you import:", color = Slate, style = MaterialTheme.typography.bodySmall); Text(exportCode, style = MaterialTheme.typography.titleLarge, fontFamily = FontFamily.Monospace) }
                QrBlock("Identity QR (scan it in the other app)", exportText, onClose = { vm.clearExport() })
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { clip.setText(AnnotatedString(exportText)) }) { Text("Copy text") }
                    OutlinedButton(onClick = { saveFile.launch("beaconfix-identity-${r.id.take(8)}.txt") }) { Text("Save file") }
                    OutlinedButton(onClick = { ctx.startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, exportText), "Share identity")) }) { Text("Share") }
                }
            }
        }
        if (staged.isNotEmpty()) InfoCard("Identity bundle received") {
            Text("Importing replaces the identity on this phone.", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            var p2 by remember { mutableStateOf("") }
            OutlinedTextField(p2, { p2 = it }, Modifier.fillMaxWidth(), label = { Text("Passphrase / 6-word code") }, singleLine = true)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) { Button(onClick = { vm.importWithPassphrase(p2) }, enabled = p2.isNotBlank()) { Text("Import") }; TextButton(onClick = { vm.stageImport("") }) { Text("Cancel") } }
        }
        InfoCard("Danger zone") { TextButton(onClick = { vm.forgetAll() }) { Text("Forget this identity on this phone", color = MaterialTheme.colorScheme.error) } }
        if (busy) CircularProgressIndicator(Modifier.padding(16.dp))
        if (msg.isNotEmpty()) Text(msg, color = MaterialTheme.colorScheme.tertiary, modifier = Modifier.padding(16.dp))
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun QrBlock(title: String, text: String, onClose: () -> Unit) {
    val bmp: Bitmap = remember(text) { Qr.encode(text, 640) }
    Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        Text(title, style = MaterialTheme.typography.labelLarge)
        Image(bmp.asImageBitmap(), contentDescription = title, modifier = Modifier.size(260.dp).padding(8.dp))
        TextButton(onClick = onClose) { Text("Hide") }
    }
}
