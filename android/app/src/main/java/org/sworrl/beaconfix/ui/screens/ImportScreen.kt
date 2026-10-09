package org.sworrl.beaconfix.ui.screens

import androidx.lifecycle.compose.collectAsStateWithLifecycle
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.material3.LinearProgressIndicator
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
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.importer.ImportOptions
import org.sworrl.beaconfix.importer.ImportRepository
import org.sworrl.beaconfix.importer.ImportSummary
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.KeyValue
import org.sworrl.beaconfix.ui.theme.Slate
import java.text.DateFormat
import java.util.Date
import javax.inject.Inject

@HiltViewModel
class ImportViewModel @Inject constructor(val repo: ImportRepository) : ViewModel() {
    val busy = MutableStateFlow(false); val error = MutableStateFlow(""); val summary = MutableStateFlow<ImportSummary?>(null)
    fun open(uri: Uri) = viewModelScope.launch { busy.value = true; error.value = ""; summary.value = null; repo.open(uri).onFailure { error.value = it.message ?: "failed" }; busy.value = false }
    fun apply(o: ImportOptions) = viewModelScope.launch { val r = repo.parsed.value ?: return@launch; busy.value = true; try { summary.value = repo.apply(r, o) } catch (e: Exception) { error.value = e.message ?: "failed" } finally { busy.value = false } }
    fun reset() { repo.clear(); summary.value = null; error.value = "" }
}

/** Settings → Import, and the target of "Share → BeaconFix" for JSON/CSV/GPX/KML files. */
@Composable
fun ImportScreen(onBack: () -> Unit, incoming: Uri? = null, vm: ImportViewModel = hiltViewModel()) {
    val busy by vm.busy.collectAsStateWithLifecycle(); val err by vm.error.collectAsStateWithLifecycle(); val summary by vm.summary.collectAsStateWithLifecycle()
    val parsed by vm.repo.parsed.collectAsStateWithLifecycle(); val progress by vm.repo.progress.collectAsStateWithLifecycle(); val name by vm.repo.fileName.collectAsStateWithLifecycle(); val fmt by vm.repo.format.collectAsStateWithLifecycle()
    var positions by remember { mutableStateOf(true) }; var wifi by remember { mutableStateOf(true) }; var places by remember { mutableStateOf(true) }
    var from by remember { mutableStateOf("") }; var to by remember { mutableStateOf("") }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> uri?.let { vm.open(it) } }
    LaunchedEffect(incoming) { if (incoming != null) vm.open(incoming) }
    val df = DateFormat.getDateInstance(DateFormat.MEDIUM)
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Row(Modifier.padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) { TextButton(onClick = onBack) { Text("‹ Back") }; Text("Import your history", style = MaterialTheme.typography.titleLarge) }
        InfoCard("What this reads") {
            Text("• Google Timeline — Maps → your picture → Timeline → ⋯ → Export Timeline data → Timeline.json (positions, Wi-Fi scans, visits)\n• Google Takeout — Records.json and Semantic Location History\n• WiGLE WiFi CSV exports (beacons with positions and security)\n• GPX / KML tracks\n• BeaconFix desktop exports", style = MaterialTheme.typography.bodySmall)
            Text("Nothing talks to Google: you export the file on this phone or your computer, BeaconFix reads it locally, and the results sync to your desktops like your own scans.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Button(onClick = { pick.launch(arrayOf("*/*")) }, enabled = !busy) { Text("Choose a file") }
        }
        if (busy) InfoCard("Working") { LinearProgressIndicator(Modifier.fillMaxWidth()); Text(progress, color = Slate, style = MaterialTheme.typography.bodySmall) }
        if (err.isNotEmpty()) InfoCard("Could not import") { Text(err, color = MaterialTheme.colorScheme.error); TextButton(onClick = { vm.reset() }) { Text("Try another file") } }
        val p = parsed
        if (p != null && summary == null && !busy) InfoCard("$name · ${fmt}") {
            Text(progress, color = Slate, style = MaterialTheme.typography.bodySmall)
            p.firstTime?.let { f -> p.lastTime?.let { l -> KeyValue("Spans", "${df.format(Date(f))} – ${df.format(Date(l))}") } }
            Row(verticalAlignment = Alignment.CenterVertically) { Checkbox(positions, { positions = it }); Text("Positions & track (${p.positions.size})") }
            Row(verticalAlignment = Alignment.CenterVertically) { Checkbox(wifi, { wifi = it }); Text("Wi-Fi scans → beacon samples (${p.scans.size} scans, ${p.observations.size} samples)") }
            Row(verticalAlignment = Alignment.CenterVertically) { Checkbox(places, { places = it }); Text("Places / stops (${p.stops.size})") }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) { OutlinedTextField(from, { from = it }, Modifier.weight(1f), label = { Text("From (YYYY-MM-DD)") }, singleLine = true); OutlinedTextField(to, { to = it }, Modifier.weight(1f), label = { Text("To") }, singleLine = true) }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { vm.apply(ImportOptions(positions, wifi, places, day(from) ?: 0L, day(to)?.plus(86_399_999L) ?: Long.MAX_VALUE)) }) { Text("Import") }
                TextButton(onClick = { vm.reset() }) { Text("Cancel") }
            }
        }
        summary?.let { s -> InfoCard("Imported") {
            KeyValue("Format", s.format); KeyValue("Positions", "${s.positions}"); KeyValue("Wi-Fi scans", "${s.scans}"); KeyValue("Beacon samples", "${s.observations}"); KeyValue("New beacons", "${s.aps}"); KeyValue("Places", "${s.stops}"); KeyValue("Beacons re-fitted", "${s.refit}")
            if (s.from > 0) KeyValue("Spans", "${df.format(Date(s.from))} – ${df.format(Date(s.to))}")
            if (s.skipped > 0) Text("${s.skipped} rows skipped (too dense or too imprecise)", color = Slate, style = MaterialTheme.typography.bodySmall)
            Text("Everything is queued for the next sync, so your desktops get it too.", color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedButton(onClick = { vm.reset() }) { Text("Import another") }
        } }
    }
}
private fun day(s: String): Long? = runCatching { java.time.LocalDate.parse(s.trim()).atStartOfDay(java.time.ZoneId.systemDefault()).toInstant().toEpochMilli() }.getOrNull()
