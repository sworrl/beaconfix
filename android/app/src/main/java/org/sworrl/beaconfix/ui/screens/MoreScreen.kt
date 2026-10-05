package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import org.sworrl.beaconfix.ui.theme.Slate

data class MoreItem(val route: String, val icon: String, val title: String, val body: String)
val MORE = listOf(
    MoreItem("link", "🔗", "Link a PC", "Scan the QR on your PC or tap it on this network — links the PC and enrols with your hub. Nothing to type"),
    MoreItem("help?focus=", "🏥", "Help & nearest ER", "The emergency number, your position for the dispatcher, the nearest children's ER, ER, urgent care, police and fire"),
    MoreItem("trip", "🧭", "Trip", "Distances, stops, places visited, the rank ladder and milestones"),
    MoreItem("events", "📻", "Events", "What the desktop hears, live"),
    MoreItem("sightings", "📷", "Sightings", "ALPR cameras your routes passed (your plate was likely read) and searches for your plate in released Flock audit logs"),
    MoreItem("detector", "📡", "ALPR Detector", "Radar sweep, proximity geiger alerts, and audio/haptic customization for surveillance cameras"),
    MoreItem("alpr", "📹", "ALPR Dash Cam", "Local plate reader using your phone camera: offline alert matching, dash cam capture, and pass recording"),
    MoreItem("survey", "◎", "Survey", "Scan continuously and watch signal strengths"),
    MoreItem("sync", "⇅", "Sync", "Push your samples, pull the desktop's map"),
    MoreItem("hub", "🛰", "Hub", "Your BeaconFix hub over WireGuard: sync to the master database, every node's live position"),
    MoreItem("anchors", "⌖", "Anchors", "Antennas you surveyed: your computer's Wi-Fi card, your router — ground truth for ranging"),
    MoreItem("esp_nodes", "📡", "Mesh & Hardware Nodes", "LoRa mesh topology, Heltec V3 & ESP32 monitor nodes, battery telemetry, and 3D nameplates"),
    MoreItem("identity", "🪪", "Identity", "Your key pair: export, link, devices"),
    MoreItem("widgets", "▦", "Widgets", "Preview and add the home-screen widgets"),
    MoreItem("settings", "⚙", "Settings", "Collector, home networks, desktops, privacy"),
)

@Composable
fun MoreScreen(
    onGo: (String) -> Unit,
    live: org.sworrl.beaconfix.ui.vm.LiveViewModel = androidx.hilt.navigation.compose.hiltViewModel()
) {
    val doomMode by live.doomBatteryMode.collectAsState()
    var showDoomDialog by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf(false) }

    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Text("More", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.padding(horizontal = 16.dp))
        org.sworrl.beaconfix.ui.DoomBatteryCard(
            mode = doomMode,
            onClick = { showDoomDialog = true },
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp)
        )
        for (m in MORE) Card(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 5.dp).clickable { onGo(m.route) }) {
            Row(Modifier.padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(m.icon, style = MaterialTheme.typography.headlineSmall, modifier = Modifier.padding(end = 14.dp))
                Column { Text(m.title, fontWeight = FontWeight.Bold); Text(m.body, color = Slate, style = MaterialTheme.typography.bodySmall) }
            }
        }
    }

    if (showDoomDialog) {
        org.sworrl.beaconfix.ui.DoomBatteryDialog(
            currentMode = doomMode,
            onSelectMode = { mode ->
                live.setDoomBatteryMode(mode)
                showDoomDialog = false
            },
            onDismiss = { showDoomDialog = false }
        )
    }
}
