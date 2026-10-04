package org.sworrl.beaconfix.alpr.ui

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.layout.Box
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.view.PreviewView
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.hilt.navigation.compose.hiltViewModel
import java.util.concurrent.Executors
import org.sworrl.beaconfix.alpr.AlprConfig
import org.sworrl.beaconfix.alpr.AlprState
import org.sworrl.beaconfix.alpr.FalconPairing
import org.sworrl.beaconfix.alpr.RecentRead
import org.sworrl.beaconfix.alpr.alprEntry
import org.sworrl.beaconfix.alpr.core.Pairing
import org.sworrl.beaconfix.alpr.core.PairingPayload
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.KeyValue
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.screens.QrScanner
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Orange
import org.sworrl.beaconfix.ui.theme.Red
import org.sworrl.beaconfix.ui.theme.Slate

/** Permissions the dash cam needs before it can start (camera; notifications for alerts; location for the geofence). */
fun alprMissingPermissions(ctx: Context): List<String> = buildList {
    fun miss(p: String) = ContextCompat.checkSelfPermission(ctx, p) != PackageManager.PERMISSION_GRANTED
    if (miss(Manifest.permission.CAMERA)) add(Manifest.permission.CAMERA)
    if (Build.VERSION.SDK_INT >= 33 && miss(Manifest.permission.POST_NOTIFICATIONS)) add(Manifest.permission.POST_NOTIFICATIONS)
    if (miss(Manifest.permission.ACCESS_FINE_LOCATION) && miss(Manifest.permission.ACCESS_COARSE_LOCATION)) { add(Manifest.permission.ACCESS_FINE_LOCATION); add(Manifest.permission.ACCESS_COARSE_LOCATION) }
}

fun bytesText(b: Long): String = when { b < 1024 -> "$b B"; b < 1024 * 1024 -> "${b / 1024} KB"; else -> "%.1f MB".format(b / 1048576.0) }

/** The ALPR dash cam: on/off, live status, last reads, FalconEyez pairing and settings. */
@Composable
fun AlprScreen(onBack: () -> Unit, vm: AlprViewModel = hiltViewModel()) {
    val ctx = LocalContext.current
    val st by vm.status.state.collectAsState(); val recent by vm.status.recent.collectAsState()
    val cfg by vm.settings.config.collectAsState(); val pairing by vm.link.pairing.collectAsState()
    val busy by vm.busy.collectAsState(); val msg by vm.message.collectAsState(); val test by vm.selfTest.collectAsState()
    val incoming by AlprIncoming.payload.collectAsState()
    var scanning by remember { mutableStateOf(false) }
    var scanned by remember { mutableStateOf<PairingPayload?>(null) }
    var permNote by remember { mutableStateOf("") }
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { _ ->
        if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) { permNote = ""; vm.setEnabled(true) }
        else permNote = "The camera permission was refused. Allow it in Android Settings → Apps → BeaconFix → Permissions → Camera."
    }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri -> if (uri != null) vm.selfTest(uri) }
    fun toggle(on: Boolean) {
        if (!on) { vm.setEnabled(false); return }
        val missing = alprMissingPermissions(ctx)
        if (missing.isEmpty()) vm.setEnabled(true) else ask.launch(missing.toTypedArray())
    }

    if (scanning) {
        QrScanner("Point the camera at the pairing QR on FalconEyez (Settings → Phones → Pair a phone)", onResult = { text ->
            scanning = false
            val p = Pairing.parse(text)
            if (p == null) permNote = "That QR isn't a FalconEyez pairing code." else scanned = p
        })
        return
    }
    val confirm = scanned ?: incoming
    if (confirm != null) AlertDialog(onDismissRequest = { scanned = null; AlprIncoming.payload.value = null },
        title = { Text("Pair with ${confirm.name}?") },
        text = { Text("This phone becomes a FalconEyez camera: plate crops and their time and place are sent to\n${confirm.urls.joinToString("\n")}") },
        confirmButton = { TextButton(onClick = { vm.pair(confirm.urls, confirm.token, confirm.name); scanned = null; AlprIncoming.payload.value = null }) { Text("Pair") } },
        dismissButton = { TextButton(onClick = { scanned = null; AlprIncoming.payload.value = null }) { Text("Cancel") } })

    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("‹ Back") }
            Text("ALPR dash cam", style = MaterialTheme.typography.headlineSmall)
            if (busy) CircularProgressIndicator(Modifier.padding(start = 12.dp).size(20.dp), strokeWidth = 2.dp)
        }
        if (msg.isNotEmpty()) Text(msg, Modifier.padding(horizontal = 20.dp, vertical = 4.dp), style = MaterialTheme.typography.bodyMedium)
        if (permNote.isNotEmpty()) Text(permNote, Modifier.padding(horizontal = 20.dp, vertical = 4.dp), color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)

        LivePlateScanner(vm, cfg)

        InfoCard("Background Dash Cam (Screen Off)") {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(if (st.running) (st.paused ?: "Reading plates in background") else "Background mode: Off", fontWeight = FontWeight.SemiBold,
                        color = when { !st.running -> Slate; st.paused != null -> Orange; else -> Green })
                    Text("Keeps the camera reading plates continuously in the background with the screen off or while using other apps. Plate crops feed FalconEyez.", color = Slate, style = MaterialTheme.typography.bodySmall)
                }
                Switch(checked = st.running, onCheckedChange = { toggle(it) })
            }
            if (!pairing.paired) Text("Not paired: reads stay on this phone, nothing is sent, and there is no hotlist to match yet.", color = Orange, style = MaterialTheme.typography.bodySmall)
            if (pairing.revoked) Text("FalconEyez revoked this phone's token: pair again below.", color = Red, style = MaterialTheme.typography.bodySmall)
        }

        StatusCard(st, pairing)
        RecentCard(recent)
        FalconCard(pairing, st, vm, onScan = { scanning = true })
        SettingsCard(cfg, vm)

        InfoCard("Test with a photo") {
            Text("Runs the plate detector and reader on a picture (nothing is sent or kept).", color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedButton(onClick = { pick.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)) }, enabled = !busy) { Text("Choose a photo") }
            test?.let { t ->
                Text(t.text, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
                Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    for ((label, bmp) in t.thumbs) Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Image(bmp.asImageBitmap(), label, Modifier.height(72.dp).clip(RoundedCornerShape(6.dp)))
                        Text(label, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.labelSmall)
                    }
                }
            }
        }

        InfoCard("Privacy") {
            Text("No video is recorded. Camera frames are read in memory and released. When you pass a known ALPR or traffic camera, the one or two " +
                "frames nearest that moment (closest approach, front plate) are kept as lossless stills with the pass, sent to your PC and then " +
                "deleted from the phone (Sightings). Otherwise only a still crop " +
                "around a plate's vehicle (no scene, no faces on purpose) and its meta wait to upload — in memory, spilling to app storage only " +
                "while FalconEyez can't be reached — and are deleted the moment the server accepts them. Unsent ordinary events expire after 24 h. " +
                "Capture pauses in Maine, New Hampshire and Arkansas, where private ALPR is banned.", color = Slate, style = MaterialTheme.typography.bodySmall)
        }
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun StatusCard(st: AlprState, p: FalconPairing) {
    InfoCard("Status") {
        KeyValue("Server", when { !p.paired -> "not paired"; st.reachable == true -> "${p.serverName.ifBlank { "FalconEyez" }} · reachable"; st.reachable == false -> "${p.serverName.ifBlank { "FalconEyez" }} · out of reach"; else -> "${p.serverName.ifBlank { "FalconEyez" }} · not tried yet" })
        if (st.activeUrl.isNotEmpty()) KeyValue("Via", st.activeUrl)
        KeyValue("Camera", if (st.cameraSize.isEmpty()) "—" else "${st.cameraSize} · ${"%.1f".format(st.fps)} fps")
        if (st.latencyMs > 0) KeyValue("Per frame", "${st.latencyMs} ms (detect ${st.detectMs}, OCR ${st.ocrMs})")
        KeyValue("Frames analysed", "${st.framesAnalysed}")
        KeyValue("Plates seen / read", "${st.platesSeen} / ${st.platesRead}")
        KeyValue("Vehicles in view / events", "${st.tracks} / ${st.events}")
        if (st.burstFrames > 0) KeyValue("Burst re-scans", "${st.burstFrames}")
        if (st.shutter.isNotEmpty()) KeyValue("Shutter", st.shutter)
        KeyValue("Events", "${st.events} (${st.uploaded} sent${if (st.dropped > 0) ", ${st.dropped} dropped" else ""})")
        KeyValue("Waiting in memory", "${st.memPending} · ${bytesText(st.memBytes)}")
        KeyValue("Spilled to storage", "${st.spillCount} · ${bytesText(st.spillBytes)}")
        KeyValue("Last upload", ago(st.lastUploadMs))
        if (st.lastUploadError.isNotEmpty()) Text(st.lastUploadError, color = Orange, style = MaterialTheme.typography.bodySmall)
        KeyValue("Hotlist", "${st.hotlistSize} entries · ${ago(st.lastHotlistSyncMs)}")
        KeyValue("Thermal", st.thermal.ifEmpty { "—" } + (if (!st.headroom.isNaN()) " · headroom ${"%.2f".format(st.headroom)}" else "") + (if (st.thermalLevel > 0) " · step ${st.thermalLevel}" else ""))
        KeyValue("Charging", when (st.charging) { true -> "yes"; false -> "no"; null -> "—" })
        if (st.region.isNotEmpty()) KeyValue("State", st.region)
        if (st.modelError.isNotEmpty()) Text("Models: ${st.modelError}", color = Red, style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun RecentCard(recent: List<RecentRead>) {
    InfoCard("Last reads") {
        if (recent.isEmpty()) { Text("No plates read yet.", color = Slate, style = MaterialTheme.typography.bodySmall); return@InfoCard }
        val withThumbs = recent.filter { it.thumb != null }
        if (withThumbs.isNotEmpty()) Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            for (r in withThumbs) Card(colors = CardDefaults.cardColors(containerColor = when (r.match) { "hotlist" -> Red.copy(alpha = 0.25f); "possible" -> Orange.copy(alpha = 0.25f); else -> MaterialTheme.colorScheme.surfaceVariant })) {
                Column(Modifier.padding(6.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                    Image(r.thumb!!.asImageBitmap(), r.text, Modifier.width(120.dp).clip(RoundedCornerShape(4.dp)))
                    Text(r.text, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold)
                    Text(ago(r.atMs), color = Slate, style = MaterialTheme.typography.labelSmall)
                }
            }
        }
        for (r in recent.take(12)) KeyValue("${r.text}${when (r.match) { "hotlist" -> "  ⚠ HOTLIST"; "possible" -> "  ≈ possible"; else -> "" }}", "${"%.0f".format(r.conf * 100)} % · ${ago(r.atMs)}")
        if (recent.size > 12) Text("…and ${recent.size - 12} more (text only, this session).", color = Slate, style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun FalconCard(p: FalconPairing, st: AlprState, vm: AlprViewModel, onScan: () -> Unit) {
    var manual by remember { mutableStateOf(!p.paired) }
    var url by remember { mutableStateOf("") }
    var token by remember { mutableStateOf("") }
    var extra by remember { mutableStateOf("") }
    var confirmForget by remember { mutableStateOf(false) }
    InfoCard("FalconEyez") {
        if (p.paired) {
            KeyValue("Server", p.serverName.ifBlank { "FalconEyez" })
            if (p.cameraName.isNotBlank() || p.cameraId.isNotBlank()) KeyValue("This camera", p.cameraName.ifBlank { p.cameraId })
            KeyValue("Paired", ago(p.pairedAtMs))
            Text("Addresses (tried in order, the last one that answered first):", color = Slate, style = MaterialTheme.typography.bodySmall)
            for (u in p.urls) Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text((if (u == p.preferred) "● " else "○ ") + u, Modifier.weight(1f), fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
                if (p.urls.size > 1) TextButton(onClick = { vm.removeUrl(u) }) { Text("Remove") }
            }
            OutlinedTextField(extra, { extra = it }, Modifier.fillMaxWidth(), singleLine = true,
                label = { Text("Add a remote address (Tailscale / WireGuard / IPv6)") })
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { vm.addUrl(extra); extra = "" }, enabled = extra.isNotBlank()) { Text("Add") }
                OutlinedButton(onClick = { vm.refreshHotlist() }) { Text("Refresh hotlist") }
                if (st.memPending + st.spillCount > 0) OutlinedButton(onClick = { vm.uploadNow() }) { Text("Send now") }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = onScan) { Text("Re-pair (scan QR)") }
                TextButton(onClick = { manual = !manual }) { Text("Re-pair by hand") }
                TextButton(onClick = { confirmForget = true }) { Text("Forget") }
            }
        } else {
            Text("On FalconEyez, make a pairing code for a phone and scan its QR, or type the server address and the token.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Button(onClick = onScan) { Text("Scan pairing QR") }
        }
        if (manual) {
            OutlinedTextField(url, { url = it }, Modifier.fillMaxWidth(), singleLine = true, label = { Text("Server URL(s), comma separated") })
            OutlinedTextField(token, { token = it }, Modifier.fillMaxWidth(), singleLine = true, label = { Text("Pairing token") })
            Button(onClick = { vm.pair(url.split(',').map { it.trim() }.filter { it.isNotEmpty() }, token.trim(), "FalconEyez") }, enabled = url.isNotBlank() && token.isNotBlank()) { Text("Pair") }
        }
    }
    if (confirmForget) AlertDialog(onDismissRequest = { confirmForget = false },
        title = { Text("Forget FalconEyez?") },
        text = { Text("The dash cam stops, the token and addresses are erased, and events still waiting to upload are deleted. Remove the phone on FalconEyez as well.") },
        confirmButton = { TextButton(onClick = { confirmForget = false; vm.forget() }) { Text("Forget") } },
        dismissButton = { TextButton(onClick = { confirmForget = false }) { Text("Keep") } })
}

@Composable
private fun SettingRow(title: String, sub: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().clickable { onChange(!checked) }, verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) { Text(title); Text(sub, color = Slate, style = MaterialTheme.typography.bodySmall) }
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

@Composable
private fun SettingsCard(cfg: AlprConfig, vm: AlprViewModel) {
    InfoCard("Settings") {
        SettingRow("Only while charging", "Capture pauses when the phone is unplugged.", cfg.chargingOnly) { v -> vm.update { it.copy(chargingOnly = v) } }
        SettingRow("Upload backlog on Wi-Fi only", "Ordinary plate events wait for Wi-Fi; hotlist matches go out on any network.", cfg.wifiOnlyBacklog) { v -> vm.update { it.copy(wifiOnlyBacklog = v) } }
        SettingRow("Possible-match alerts", "Alert when a hotlist plate is among the recognizer's alternatives for a plate, or matches an unconfirmed read.", cfg.notifyPossible) { v -> vm.update { it.copy(notifyPossible = v) } }
        Text("Resolution", style = MaterialTheme.typography.titleSmall)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            for ((k, l) in listOf("max" to "12 MP", "4k" to "4K", "1080p" to "1080p")) FilterChip(selected = cfg.resolution == k, onClick = { vm.update { it.copy(resolution = k) } }, label = { Text(l) })
        }
        Text("Frames per second: ${cfg.targetFps}", style = MaterialTheme.typography.titleSmall)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            for (f in 1..4) FilterChip(selected = cfg.targetFps == f, onClick = { vm.update { it.copy(targetFps = f) } }, label = { Text("$f") })
        }
        Text("Distant plates (detector tiles across the frame)", style = MaterialTheme.typography.titleSmall)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            for ((n, l) in listOf(0 to "Off", 2 to "2", 3 to "3", 4 to "4")) FilterChip(selected = cfg.tileCols == n, onClick = { vm.update { it.copy(tileCols = n) } }, label = { Text(l) })
        }
        var cap by remember(cfg.spillCapMb) { mutableFloatStateOf(cfg.spillCapMb.toFloat()) }
        Text("Storage cap for unsent events: ${cap.toInt()} MB", style = MaterialTheme.typography.titleSmall)
        Slider(cap, { cap = it }, valueRange = 10f..AlprConfig.MAX_SPILL_MB.toFloat(), onValueChangeFinished = { vm.update { it.copy(spillCapMb = cap.toInt()) } })
        SettingRow("XNNPACK acceleration", "ONNX Runtime's XNNPACK provider for the models (try it if frames are slow).", cfg.xnnpack) { v -> vm.update { it.copy(xnnpack = v) } }
        SettingRow("Accurate OCR", "Re-read each vehicle's best frames with the larger recognizer (≈8× the work per read; downloads 5 MB once). Off when the phone runs warm.", cfg.accurateOcr) { v -> vm.update { it.copy(accurateOcr = v) } }
        SettingRow("Short shutter", "≈1/1000 s by day, at most 1/250 s at night (1/500 s at speed), with higher ISO: sharp plates on moving cars, a bit more grain.", cfg.shortShutter) { v -> vm.update { it.copy(shortShutter = v) } }
    }
}

/**
 * The entry on the Detector screen: the "Use camera as ALPR" switch and a line of status; tapping opens the ALPR screen.
 * Without the camera permission the switch opens the screen, which asks.
 */
@Composable
fun AlprDetectorCard(onOpen: () -> Unit, modifier: Modifier = Modifier) {
    val ctx = LocalContext.current
    val e = remember { ctx.alprEntry() }
    val st by e.alprStatus().state.collectAsState()
    Card(modifier.fillMaxWidth().clickable { onOpen() }, shape = RoundedCornerShape(8.dp)) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Use camera as ALPR", fontWeight = FontWeight.Bold)
                Text(when { !st.running -> "Dash cam off · feeds FalconEyez"; st.paused != null -> st.paused!!; else -> "Reading plates · ${"%.1f".format(st.fps)} fps · ${st.platesRead} read" },
                    color = Slate, style = MaterialTheme.typography.bodySmall)
            }
            Switch(checked = st.running, onCheckedChange = { on ->
                if (!on) { e.alprSettings().update { it.copy(enabled = false) }; org.sworrl.beaconfix.alpr.DashCamService.stop(ctx) }
                else if (alprMissingPermissions(ctx).isNotEmpty()) onOpen()
                else { e.alprSettings().update { it.copy(enabled = true) }; org.sworrl.beaconfix.alpr.DashCamService.start(ctx) }
            })
        }
    }
}

data class LiveDetection(
    val box: org.sworrl.beaconfix.alpr.core.BoxF,
    val text: String,
    val conf: Float,
    val hotlist: Boolean
)

private fun performHapticClick(ctx: Context) {
    runCatching {
        if (Build.VERSION.SDK_INT >= 31) {
            val vm = ctx.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? android.os.VibratorManager
            vm?.defaultVibrator?.vibrate(android.os.VibrationEffect.createPredefined(android.os.VibrationEffect.EFFECT_CLICK))
        } else {
            @Suppress("DEPRECATION")
            val v = ctx.getSystemService(Context.VIBRATOR_SERVICE) as? android.os.Vibrator
            v?.vibrate(android.os.VibrationEffect.createOneShot(40, android.os.VibrationEffect.DEFAULT_AMPLITUDE))
        }
    }
}

@Composable
fun LivePlateScanner(vm: AlprViewModel, cfg: AlprConfig, modifier: Modifier = Modifier) {
    val ctx = LocalContext.current
    val owner = LocalLifecycleOwner.current
    var hasCameraPerm by remember { mutableStateOf(ContextCompat.checkSelfPermission(ctx, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) }
    val askCamera = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        hasCameraPerm = granted
    }

    val pipelineRef = remember { java.util.concurrent.atomic.AtomicReference<org.sworrl.beaconfix.alpr.vision.AlprPipeline?>(null) }
    val executor = remember { Executors.newSingleThreadExecutor() }

    DisposableEffect(Unit) {
        org.sworrl.beaconfix.alpr.DashCamService.uiActive = true
        onDispose {
            org.sworrl.beaconfix.alpr.DashCamService.uiActive = false
            executor.execute {
                pipelineRef.getAndSet(null)?.close()
            }
            executor.shutdown()
        }
    }

    if (!hasCameraPerm) {
        InfoCard("Live Plate Scanner") {
            Text("Point your camera at a license plate to read it in real time.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Button(onClick = { askCamera.launch(Manifest.permission.CAMERA) }, modifier = Modifier.padding(top = 8.dp)) {
                Text("Enable camera scanner")
            }
        }
        return
    }

    var detections by remember { mutableStateOf<List<LiveDetection>>(emptyList()) }
    var frameSize by remember { mutableStateOf<android.util.Size?>(null) }
    var lastScannedText by remember { mutableStateOf("") }
    var lastScannedConf by remember { mutableFloatStateOf(0f) }
    var lastScannedAt by remember { mutableLongStateOf(0L) }
    var isScannedHotlist by remember { mutableStateOf(false) }

    val textMeasurer = rememberTextMeasurer()

    Card(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 6.dp),
        shape = RoundedCornerShape(12.dp)
    ) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(300.dp)
                .clip(RoundedCornerShape(12.dp))
        ) {
            AndroidView(
                modifier = Modifier.fillMaxSize(),
                factory = { c ->
                    val previewView = PreviewView(c).apply {
                        scaleType = PreviewView.ScaleType.FILL_CENTER
                    }
                    val future = ProcessCameraProvider.getInstance(c)
                    future.addListener({
                        val provider = runCatching { future.get() }.getOrNull() ?: return@addListener
                        val preview = Preview.Builder().build().also {
                            it.surfaceProvider = previewView.surfaceProvider
                        }

                        val sel = ResolutionSelector.Builder()
                            .setResolutionStrategy(
                                ResolutionStrategy(
                                    android.util.Size(1280, 960),
                                    ResolutionStrategy.FALLBACK_RULE_CLOSEST_LOWER_THEN_HIGHER
                                )
                            ).build()

                        val ab = ImageAnalysis.Builder()
                            .setResolutionSelector(sel)
                            .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                            .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888)

                        val analysis = ab.build()
                        val planes = org.sworrl.beaconfix.alpr.vision.PlaneBuffers()

                        analysis.setAnalyzer(executor) { img ->
                            try {
                                if (pipelineRef.get() == null) {
                                    runCatching {
                                        pipelineRef.set(org.sworrl.beaconfix.alpr.vision.AlprPipeline(c, 4, cfg.xnnpack, cfg.accurateOcr))
                                    }.onFailure { android.util.Log.e("LivePlateScanner", "Failed to init pipeline", it) }
                                }
                                val p = pipelineRef.get() ?: return@setAnalyzer
                                val rot = img.imageInfo.rotationDegrees
                                val frame = org.sworrl.beaconfix.alpr.vision.YuvFrame(img, rot, planes)
                                val fw = frame.width
                                val fh = frame.height
                                frameSize = android.util.Size(fw, fh)

                                val fc = org.sworrl.beaconfix.alpr.vision.FrameContext(System.currentTimeMillis(), null, null)
                                val opt = org.sworrl.beaconfix.alpr.vision.PassOptions(tileCols = cfg.tileCols, accurateOcr = cfg.accurateOcr, makeEvents = false)
                                val outcome = p.process(frame, fc, vm.hotlist.matcher, opt)

                                val list = ArrayList<LiveDetection>()
                                val now = System.currentTimeMillis()
                                var bestPlateText = ""
                                var bestPlateConf = 0f
                                var bestHot = false

                                for (plate in outcome.plates) {
                                    val r = plate.read
                                    val readText = plate.fused.ifEmpty { r?.text.orEmpty() }
                                    val conf = r?.conf ?: 0f
                                    val isHot = plate.matches.isNotEmpty()
                                    list.add(LiveDetection(plate.det.box, readText, conf, isHot))

                                    if (readText.length >= 4 && conf >= 0.60f) {
                                        if (conf > bestPlateConf) {
                                            bestPlateText = readText
                                            bestPlateConf = conf
                                            bestHot = isHot
                                        }
                                        val kind = when {
                                            plate.matches.any { it.kind == org.sworrl.beaconfix.alpr.core.MatchKind.EXACT } -> "hotlist"
                                            plate.matches.isNotEmpty() -> "possible"
                                            else -> ""
                                        }
                                        val clBox = plate.det.box.clamp(frame.width, frame.height)
                                        val thumb = if (clBox.w > 4f && clBox.h > 4f) {
                                            runCatching {
                                                val tw = 120
                                                val th = maxOf(24, (tw * clBox.h / maxOf(1f, clBox.w)).toInt())
                                                frame.bitmap(clBox, tw, th)
                                            }.getOrNull()
                                        } else null
                                        vm.status.addRead(RecentRead(readText, conf, now, null, null, kind, thumb))
                                    }
                                }

                                detections = list
                                if (bestPlateText.isNotEmpty() && (now - lastScannedAt > 1500L || bestPlateText != lastScannedText)) {
                                    lastScannedText = bestPlateText
                                    lastScannedConf = bestPlateConf
                                    lastScannedAt = now
                                    isScannedHotlist = bestHot
                                    performHapticClick(c)
                                }
                            } catch (t: Throwable) {
                                android.util.Log.w("LivePlateScanner", "frame analysis", t)
                            } finally {
                                img.close()
                            }
                        }

                        runCatching {
                            provider.unbindAll()
                            provider.bindToLifecycle(owner, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis)
                        }.onFailure { android.util.Log.w("LivePlateScanner", "bind", it) }
                    }, ContextCompat.getMainExecutor(c))
                    previewView
                }
            )

            val activeDets = detections
            val currentFrame = frameSize
            Canvas(Modifier.fillMaxSize()) {
                val viewW = size.width
                val viewH = size.height

                if (currentFrame != null && activeDets.isNotEmpty()) {
                    val imgW = currentFrame.width.toFloat()
                    val imgH = currentFrame.height.toFloat()
                    val scale = maxOf(viewW / imgW, viewH / imgH)
                    val dx = (viewW - imgW * scale) / 2f
                    val dy = (viewH - imgH * scale) / 2f

                    for (det in activeDets) {
                        val sx = det.box.x1 * scale + dx
                        val sy = det.box.y1 * scale + dy
                        val sw = det.box.w * scale
                        val sh = det.box.h * scale

                        val boxColor = if (det.hotlist) Red else Green
                        drawRoundRect(
                            color = boxColor,
                            topLeft = Offset(sx, sy),
                            size = Size(sw, sh),
                            cornerRadius = CornerRadius(8f, 8f),
                            style = Stroke(width = 3.dp.toPx())
                        )

                        if (det.text.isNotEmpty()) {
                            val label = "${det.text} ${(det.conf * 100).toInt()}%"
                            val measured = textMeasurer.measure(
                                label,
                                style = TextStyle(
                                    fontSize = 12.sp,
                                    fontWeight = FontWeight.Bold,
                                    fontFamily = FontFamily.Monospace,
                                    color = Color.White
                                )
                            )
                            val padH = 6.dp.toPx()
                            val padV = 2.dp.toPx()
                            val pillW = measured.size.width + padH * 2
                            val pillH = measured.size.height + padV * 2
                            val pillX = sx.coerceIn(0f, (viewW - pillW).coerceAtLeast(0f))
                            val pillY = (sy - pillH - 4.dp.toPx()).coerceAtLeast(0f)

                            drawRoundRect(
                                color = if (det.hotlist) Red else Color(0xCC000000),
                                topLeft = Offset(pillX, pillY),
                                size = Size(pillW, pillH),
                                cornerRadius = CornerRadius(6f, 6f)
                            )
                            drawText(
                                measured,
                                topLeft = Offset(pillX + padH, pillY + padV)
                            )
                        }
                    }
                } else {
                    val reticleW = 200.dp.toPx()
                    val reticleH = 100.dp.toPx()
                    val rx = (viewW - reticleW) / 2f
                    val ry = (viewH - reticleH) / 2f
                    val arm = 20.dp.toPx()
                    val stroke = Stroke(width = 2.dp.toPx())
                    val reticleColor = Color(0x88FFFFFF)

                    drawLine(reticleColor, Offset(rx, ry), Offset(rx + arm, ry), stroke.width)
                    drawLine(reticleColor, Offset(rx, ry), Offset(rx, ry + arm), stroke.width)
                    drawLine(reticleColor, Offset(rx + reticleW, ry), Offset(rx + reticleW - arm, ry), stroke.width)
                    drawLine(reticleColor, Offset(rx + reticleW, ry), Offset(rx + reticleW, ry + arm), stroke.width)
                    drawLine(reticleColor, Offset(rx, ry + reticleH), Offset(rx + arm, ry + reticleH), stroke.width)
                    drawLine(reticleColor, Offset(rx, ry + reticleH), Offset(rx, ry + reticleH - arm), stroke.width)
                    drawLine(reticleColor, Offset(rx + reticleW, ry + reticleH), Offset(rx + reticleW - arm, ry + reticleH), stroke.width)
                    drawLine(reticleColor, Offset(rx + reticleW, ry + reticleH), Offset(rx + reticleW, ry + reticleH - arm), stroke.width)

                    val hintText = "Point camera at license plate"
                    val measured = textMeasurer.measure(
                        hintText,
                        style = TextStyle(fontSize = 11.sp, color = Color(0xAAFFFFFF))
                    )
                    drawText(
                        measured,
                        topLeft = Offset((viewW - measured.size.width) / 2f, ry + reticleH + 8.dp.toPx())
                    )
                }
            }

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(8.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Surface(
                    color = Color(0xCC000000),
                    shape = RoundedCornerShape(6.dp)
                ) {
                    Row(
                        modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Box(
                            Modifier
                                .size(8.dp)
                                .clip(RoundedCornerShape(4.dp))
                                .background(Green)
                        )
                        Spacer(Modifier.width(6.dp))
                        Text(
                            "LIVE SCANNER",
                            color = Color.White,
                            fontSize = 11.sp,
                            fontWeight = FontWeight.Bold
                        )
                    }
                }

                Surface(
                    color = Color(0x99000000),
                    shape = RoundedCornerShape(6.dp)
                ) {
                    Text(
                        if (cfg.tileCols > 0) "${cfg.tileCols}x tiles" else "Wide",
                        color = Color.White,
                        fontSize = 11.sp,
                        modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp)
                    )
                }
            }

            val nowTime = System.currentTimeMillis()
            if (lastScannedText.isNotEmpty() && (nowTime - lastScannedAt < 4000L)) {
                Surface(
                    color = if (isScannedHotlist) Red else Green,
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .padding(12.dp)
                ) {
                    Row(
                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 6.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            if (isScannedHotlist) "HOTLIST MATCH: " else "Scanned: ",
                            color = Color.White,
                            fontWeight = FontWeight.Bold,
                            fontSize = 13.sp
                        )
                        Text(
                            "$lastScannedText (${(lastScannedConf * 100).toInt()}%)",
                            color = Color.White,
                            fontFamily = FontFamily.Monospace,
                            fontWeight = FontWeight.Bold,
                            fontSize = 14.sp
                        )
                    }
                }
            }
        }
    }
}
