package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.anchors.AnchorRepository
import org.sworrl.beaconfix.data.api.AnchorDto
import org.sworrl.beaconfix.ui.Chip
import org.sworrl.beaconfix.ui.EmptyState
import org.sworrl.beaconfix.ui.theme.Cyan
import org.sworrl.beaconfix.ui.theme.Gold
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Magenta
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.AnchorsViewModel

/** The anchors list (More → Anchors): every surveyed antenna, with edit / delete, and "Place one at my position". */
@Composable
fun AnchorsScreen(onBack: () -> Unit, onMap: () -> Unit = {}, vm: AnchorsViewModel = hiltViewModel()) {
    val anchors by vm.anchors.collectAsState(); val editing by vm.editing.collectAsState(); val message by vm.message.collectAsState()
    val desktopAnchor by vm.desktopAnchor.collectAsState()
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("‹ Back") }
            Text("Anchors", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.weight(1f))
        }
        Text("Antennas and places whose position you surveyed: your computer's Wi-Fi antenna, your router, a Bluetooth beacon. Anchors are ground truth — fixed beacons in self-location, the centre of ranging rings, the RV's own frame. Long-press the map to place one, or stand next to it and average GNSS.",
            color = Slate, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp))
        Row(Modifier.padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { vm.newAt(0.0, 0.0, 1.0, "gps-average", "wifi-ap"); vm.startAveraging() }) { Text("＋ At my position") }
            OutlinedButton(onClick = onMap) { Text("Pick on the map") }
        }
        if (message.isNotEmpty()) Text(message, color = Cyan, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(horizontal = 16.dp))
        if (anchors.isEmpty()) EmptyState("⌖", "No anchors yet", "Place your antenna: long-press the map where it is, or use the button above while standing next to it.")
        for (a in anchors) Card(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 5.dp).clickable { vm.edit(a) }) {
            Row(Modifier.padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("⌖", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.padding(end = 14.dp))
                Column(Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) { Text(a.name, fontWeight = FontWeight.Bold); if (a.ref) Chip("REF", Gold); if (a.rv) Chip("RV", Magenta); if (desktopAnchor?.id == a.id) Chip("DESKTOP", Green) }
                    Text(kindName(a.kind) + " · ±${fmt(a.accM)} m · " + (if (a.bssids.isNotEmpty()) "${a.bssids.size} BSSID${if (a.bssids.size > 1) "s" else ""} · " else "") + (a.heightM?.let { "${fmt(it)} m up · " } ?: "") + a.source + (if (a.headingAssumed == true) " · heading assumed" else ""), color = Slate, style = MaterialTheme.typography.bodySmall)
                    Text("%.6f, %.6f".format(a.lat, a.lon) + (a.alt?.let { " · ${it.toInt()} m ASL" } ?: "") + " · ${a.placedBy} ${a.placedAt.take(16).replace('T', ' ')}", color = Slate, style = MaterialTheme.typography.bodySmall)
                }
                TextButton(onClick = { vm.delete(a.id) }) { Text("Delete", color = MaterialTheme.colorScheme.error) }
            }
        }
    }
    editing?.let { AnchorEditorSheet(it, vm) }
}

fun kindName(k: String) = AnchorRepository.KINDS.firstOrNull { it.first == k }?.second ?: k
private fun fmt(v: Double) = if (v >= 10) v.toInt().toString() else String.format(java.util.Locale.US, "%.1f", v)

/**
 * "Place an antenna here": name, kind, BSSIDs from the current scan grouped by physical transmitter, height above the floor,
 * accuracy, "moves with the RV", the RV reference flag, and a precise position from averaged GNSS fixes.
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun AnchorEditorSheet(a: AnchorDto, vm: AnchorsViewModel) {
    val sheet = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val scan by vm.scan.collectAsState(); val scanning by vm.scanning.collectAsState(); val averaging by vm.averaging.collectAsState()
    var name by remember(a.id) { mutableStateOf(a.name) }
    var height by remember(a.id) { mutableStateOf(a.heightM?.let { fmt(it) } ?: "") }
    var acc by remember(a.id) { mutableStateOf(fmt(a.accM)) }
    var latStr by remember(a.id) { mutableStateOf(if (a.lat != 0.0) String.format(java.util.Locale.US, "%.8f", a.lat) else "") }
    var lonStr by remember(a.id) { mutableStateOf(if (a.lon != 0.0) String.format(java.util.Locale.US, "%.8f", a.lon) else "") }
    var altStr by remember(a.id) { mutableStateOf(a.alt?.let { String.format(java.util.Locale.US, "%.2f", it) } ?: "") }
    var pasteCoords by remember(a.id) { mutableStateOf("") }
    var showAll by remember { mutableStateOf(false) }
    LaunchedEffect(a.id) { if (scan.isEmpty()) vm.rescan(fresh = false) }
    val groups = remember(scan) { AnchorRepository.groups(scan) }
    val chosen = a.bssids.toSet()

    fun parseCoordString(raw: String) {
        val parts = raw.trim().split(Regex("[,;\\s]+")).filter { it.isNotEmpty() }
        if (parts.size >= 2) {
            val pLat = parts[0].toDoubleOrNull()
            val pLon = parts[1].toDoubleOrNull()
            if (pLat != null && pLon != null && pLat in -90.0..90.0 && pLon in -180.0..180.0) {
                latStr = String.format(java.util.Locale.US, "%.8f", pLat)
                lonStr = String.format(java.util.Locale.US, "%.8f", pLon)
                if (parts.size >= 3) {
                    parts[2].toDoubleOrNull()?.let { altStr = String.format(java.util.Locale.US, "%.2f", it) }
                }
            }
        }
    }

    fun commit(): AnchorDto {
        val parsedLat = latStr.trim().toDoubleOrNull() ?: a.lat
        val parsedLon = lonStr.trim().toDoubleOrNull() ?: a.lon
        val parsedAlt = altStr.trim().toDoubleOrNull() ?: a.alt
        return a.copy(
            name = name,
            lat = parsedLat,
            lon = parsedLon,
            alt = parsedAlt,
            heightM = height.trim().toDoubleOrNull(),
            accM = acc.trim().toDoubleOrNull() ?: a.accM,
            source = if (parsedLat != a.lat || parsedLon != a.lon) "manual-coords" else a.source
        )
    }

    ModalBottomSheet(onDismissRequest = { vm.clearAveraging(); vm.edit(null) }, sheetState = sheet) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(horizontal = 16.dp).padding(bottom = 24.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(if (a.id.isEmpty()) "Place an antenna / fixed point" else "Edit anchor", style = MaterialTheme.typography.titleLarge)
            val curLat = latStr.toDoubleOrNull() ?: a.lat
            val curLon = lonStr.toDoubleOrNull() ?: a.lon
            Text(if (curLat == 0.0 && curLon == 0.0) "No position yet — enter detailed coords below, average GNSS, or pick on map" else "%.8f, %.8f · ±${fmt(acc.toDoubleOrNull() ?: a.accM)} m · ${a.source}".format(curLat, curLon), color = Slate, style = MaterialTheme.typography.bodySmall)

            OutlinedTextField(name, { name = it }, Modifier.fillMaxWidth(), label = { Text("Name") }, placeholder = { Text("Fixed Anchor Node, Wi-Fi antenna…") }, singleLine = true)

            // ── Fixed Known Reference Point card ──
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                Text("Fixed Known Point (Anchor)", fontWeight = FontWeight.Bold)
                                if (a.ref) Chip("FIXED REF", Gold)
                            }
                            Text("Ground truth anchor for multilateration and triangulation. Other nodes and ranging rings calculate relative to this surveyed point.", color = Slate, style = MaterialTheme.typography.bodySmall)
                        }
                        Switch(a.ref, { isRef ->
                            vm.update(a.copy(ref = isRef, kind = if (isRef && (a.kind == "custom" || a.kind.isEmpty())) "fixed-point" else a.kind))
                        })
                    }
                }
            }

            Text("What is there", style = MaterialTheme.typography.labelLarge)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                for ((k, label) in AnchorRepository.KINDS) FilterChip(selected = a.kind == k, onClick = { vm.update(a.copy(kind = k)) }, label = { Text(label) })
            }

            // ── Detailed Coordinates Editor ──
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("Detailed Coordinates (Editor)", style = MaterialTheme.typography.labelLarge)
                    Text("Enter precise surveyed coordinates (up to 8 decimals) or paste from maps / GIS.", color = Slate, style = MaterialTheme.typography.bodySmall)

                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedTextField(latStr, { latStr = it }, Modifier.weight(1f), label = { Text("Latitude") }, placeholder = { Text("e.g. 37.7749295") }, singleLine = true)
                        OutlinedTextField(lonStr, { lonStr = it }, Modifier.weight(1f), label = { Text("Longitude") }, placeholder = { Text("-122.4194155") }, singleLine = true)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedTextField(altStr, { altStr = it }, Modifier.weight(1f), label = { Text("Altitude (m ASL)") }, placeholder = { Text("e.g. 120.5") }, singleLine = true)
                        OutlinedTextField(acc, { acc = it }, Modifier.weight(1f), label = { Text("Accuracy ±m") }, placeholder = { Text("1.0") }, singleLine = true)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                        OutlinedTextField(pasteCoords, { pasteCoords = it }, Modifier.weight(1f), label = { Text("Paste 'lat, lon'") }, placeholder = { Text("37.774929, -122.419415") }, singleLine = true)
                        OutlinedButton(onClick = { parseCoordString(pasteCoords); pasteCoords = "" }, enabled = pasteCoords.isNotBlank()) { Text("Apply") }
                    }
                }
            }

            // ── BSSIDs: the current scan grouped by box ──
            Row(verticalAlignment = Alignment.CenterVertically) { Text("Radios this anchor transmits", style = MaterialTheme.typography.labelLarge, modifier = Modifier.weight(1f)); TextButton(onClick = { vm.rescan(true) }) { Text(if (scanning) "scanning…" else "Rescan") } }
            if (chosen.isNotEmpty()) Text(chosen.joinToString("  "), color = Cyan, style = MaterialTheme.typography.bodySmall)
            val visible = if (showAll) groups else groups.take(6)
            for (g in visible) {
                val all = g.bssids.all { it in chosen }; val some = g.bssids.any { it in chosen }
                Column(Modifier.fillMaxWidth().clickable { vm.update(a.copy(bssids = if (all) a.bssids - g.bssids.toSet() else (a.bssids + g.bssids).distinct())) }) {
                    Row(verticalAlignment = Alignment.CenterVertically) { Checkbox(checked = all, onCheckedChange = null); Column(Modifier.weight(1f)) { Text(g.title, fontWeight = if (some) FontWeight.Bold else FontWeight.Normal); Text(g.subtitle, color = Slate, style = MaterialTheme.typography.bodySmall) } }
                    if (some && !all) FlowRow(Modifier.padding(start = 40.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) { for (b in g.bssids) FilterChip(selected = b in chosen, onClick = { vm.update(a.copy(bssids = if (b in chosen) a.bssids - b else a.bssids + b)) }, label = { Text(b, style = MaterialTheme.typography.labelSmall) }) }
                }
            }
            if (groups.size > 6 && !showAll) TextButton(onClick = { showAll = true }) { Text("Show all ${groups.size} transmitters") }
            if (groups.isEmpty()) Text(if (scanning) "Scanning…" else "No scan results yet — Rescan (location must be on).", color = Slate, style = MaterialTheme.typography.bodySmall)

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(height, { height = it }, Modifier.weight(1f), label = { Text("Height above floor (m)") }, singleLine = true)
            }
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) { Column(Modifier.weight(1f)) { Text("Moves with the RV"); Text("Kept at its offset from the reference anchor when the RV moves.", color = Slate, style = MaterialTheme.typography.bodySmall) }; Switch(a.rv, { vm.update(a.copy(rv = it)) }) }

            // ── precise position: average GNSS ──
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("Average GNSS at this spot", style = MaterialTheme.typography.labelLarge)
                    Text("Stand next to the antenna and hold still: fixes are averaged for up to a minute and running accuracy is shown.", color = Slate, style = MaterialTheme.typography.bodySmall)
                    val av = averaging
                    if (av == null) OutlinedButton(onClick = { vm.startAveraging() }) { Text("Start averaging") }
                    else {
                        Text(if (av.n == 0) (av.error.ifEmpty { "waiting for the first fix…" }) else "${av.n} fixes · ±${fmt(av.acc)} m · ${av.elapsedS} s" + (av.alt?.let { " · ${it.toInt()} m ASL" } ?: ""), color = if (av.n > 0) Green else Slate)
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            if (!av.done) OutlinedButton(onClick = {
                                vm.stopAveraging()?.let { r ->
                                    vm.update(a.copy(lat = r.lat, lon = r.lon, accM = r.acc, alt = r.alt, source = "gps-average"))
                                    latStr = String.format(java.util.Locale.US, "%.8f", r.lat)
                                    lonStr = String.format(java.util.Locale.US, "%.8f", r.lon)
                                    r.alt?.let { altStr = String.format(java.util.Locale.US, "%.2f", it) }
                                    acc = fmt(r.acc)
                                }
                            }) { Text("Stop & use") }
                            else if (av.n > 0) OutlinedButton(onClick = {
                                vm.update(a.copy(lat = av.lat, lon = av.lon, accM = av.acc, alt = av.alt, source = "gps-average"))
                                latStr = String.format(java.util.Locale.US, "%.8f", av.lat)
                                lonStr = String.format(java.util.Locale.US, "%.8f", av.lon)
                                av.alt?.let { altStr = String.format(java.util.Locale.US, "%.2f", it) }
                                acc = fmt(av.acc)
                                vm.clearAveraging()
                            }) { Text("Use ±${fmt(av.acc)} m") }
                            TextButton(onClick = { vm.clearAveraging() }) { Text("Discard") }
                        }
                    }
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                val canSave = (latStr.toDoubleOrNull() ?: a.lat) != 0.0 || (lonStr.toDoubleOrNull() ?: a.lon) != 0.0
                Button(onClick = { vm.save(commit()) }, enabled = canSave) { Text("Save anchor") }
                if (a.id.isNotEmpty()) OutlinedButton(onClick = { vm.delete(a.id) }) { Text("Delete") }
                Spacer(Modifier.weight(1f)); TextButton(onClick = { vm.clearAveraging(); vm.edit(null) }) { Text("Cancel") }
            }
            Spacer(Modifier.height(8.dp))
        }
    }
}
