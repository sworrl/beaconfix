package org.sworrl.beaconfix.ui.screens

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.Permissions
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.theme.Cyan
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.SurveyViewModel

@Composable
fun SurveyScreen(vm: SurveyViewModel = hiltViewModel()) {
    val st by vm.status.state.collectAsState()
    val hintSeen by vm.throttleHintSeen.collectAsState()
    val ctx = LocalContext.current
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { r -> if (r.values.all { it }) vm.surveyOn() }
    DisposableEffect(Unit) { onDispose { vm.surveyOff() } }
    Column(Modifier.fillMaxSize()) {
        InfoCard("Survey mode") {
            Text(if (st.running && st.survey) "Scanning continuously. Walk around: every scan at a GPS fix becomes an observation, and each beacon's position is re-fitted as you go."
                 else "Runs the collector as fast as the OS allows while this tab is open.", color = Slate, style = MaterialTheme.typography.bodySmall)
            if (vm.throttled() && !hintSeen) {
                Text("Android limits foreground apps to 4 Wi-Fi scans per 2 minutes. For a real survey turn off Developer options → Networking → \"Wi-Fi scan throttling\".", color = MaterialTheme.colorScheme.tertiary, style = MaterialTheme.typography.bodySmall)
                TextButton(onClick = { vm.dismissHint() }) { Text("Got it") }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { if (Permissions.hasForeground(ctx)) vm.surveyOn() else ask.launch(Permissions.foreground()) }, enabled = !(st.running && st.survey)) { Text("Start survey") }
                TextButton(onClick = { vm.surveyOff() }, enabled = st.survey) { Text("Pause") }
            }
            Text("Last scan ${ago(st.lastScanAt)} · ${st.apsInScan} beacons · fix ${st.lastFixSource.ifEmpty { "—" }} · recorded ${st.recordedTotal}" + if (st.error.isNotEmpty()) " · ${st.error}" else "", color = Slate, style = MaterialTheme.typography.bodySmall)
        }
        LazyColumn {
            items(st.scan.sortedByDescending { it.dbm }, key = { it.bssid }) { s ->
                Card(Modifier.fillMaxWidth().padding(16.dp, 4.dp)) {
                    Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(s.ssid.ifEmpty { "(hidden)" }, style = MaterialTheme.typography.titleSmall)
                            Text("${s.bssid} · ${s.freq} MHz · ${s.dbm} dBm", color = Slate, style = MaterialTheme.typography.bodySmall)
                        }
                        Sparkline(st.history[s.bssid] ?: emptyList(), Modifier.width(96.dp).height(28.dp))
                    }
                }
            }
        }
    }
}

@Composable
fun Sparkline(values: List<Int>, modifier: Modifier = Modifier, color: Color = Cyan) {
    Canvas(modifier) {
        if (values.size < 2) return@Canvas
        val lo = -100f; val hi = -30f
        val stepX = size.width / (values.size - 1)
        var prev: Offset? = null
        values.forEachIndexed { i, v ->
            val y = size.height - ((v.toFloat() - lo) / (hi - lo)).coerceIn(0f, 1f) * size.height
            val p = Offset(i * stepX, y)
            prev?.let { drawLine(color, it, p, strokeWidth = 2f) }
            prev = p
        }
    }
}
