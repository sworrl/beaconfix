package org.sworrl.beaconfix.ui.screens

import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.ui.Chip
import org.sworrl.beaconfix.ui.EmptyState
import org.sworrl.beaconfix.ui.hhmm
import org.sworrl.beaconfix.ui.theme.Cyan
import org.sworrl.beaconfix.ui.theme.Gold
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Magenta
import org.sworrl.beaconfix.ui.theme.Orange
import org.sworrl.beaconfix.ui.theme.Red
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.LiveViewModel

private fun glyph(t: String) = when (t) { "ap_new" -> "◎"; "ap_lost" -> "◌"; "ap_up" -> "▲"; "ap_down" -> "▼"; "ap_placed" -> "◆"; "ap_refit" -> "🎯"; "fix" -> "⌖"; "stop" -> "🚩"; "home" -> "🏠"; "achievement" -> "★"; "region" -> "🗺"; "prefetch" -> "▦"; "ap_insecure" -> "☠"; "error" -> "!"; else -> "•" }
private fun colour(t: String): Color = when (t) { "ap_new" -> Cyan; "ap_lost" -> Slate; "ap_up" -> Green; "ap_down" -> Orange; "ap_placed", "achievement", "ap_refit" -> Gold; "fix", "stop" -> Cyan; "home" -> Magenta; "ap_insecure", "error" -> Red; else -> Slate }

/** The desktop's live event ticker (what it hears, places, fixes, stops), streamed over SSE while this screen is open. */
@Composable
fun EventsScreen(live: LiveViewModel = hiltViewModel()) {
    val views by live.views.collectAsStateWithLifecycle()
    DisposableEffect(Unit) { live.stream(true); onDispose { live.stream(false) } }
    val events = views.flatMap { v -> v.events.map { v.desktop.name to it } }.sortedByDescending { it.second.id }
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Events", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.weight(1f))
            if (views.any { it.streaming }) Chip("LIVE", Green) else if (views.isNotEmpty()) Chip("POLLED", Slate)
        }
        if (views.isEmpty()) EmptyState("📻", "No desktop connected", "Events are what the desktop hears as it scans: beacons appearing and fading, fixes, stops, milestones.")
        else if (events.isEmpty()) EmptyState("📻", "Quiet so far", "The desktop scans every 45 s; new beacons, fixes and stops will show up here as they happen.")
        LazyColumn(Modifier.fillMaxSize(), contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 12.dp, vertical = 4.dp)) {
            items(events, key = { it.first + it.second.id }) { (who, e) ->
                Card(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
                    Row(Modifier.padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        Text(glyph(e.type), color = colour(e.type), style = MaterialTheme.typography.titleMedium)
                        Column(Modifier.weight(1f)) { Text(e.text, style = MaterialTheme.typography.bodyMedium); Text("${hhmm(e.time)} · $who" + (if (e.dbm != 0) " · ${e.dbm} dBm" else ""), color = Slate, style = MaterialTheme.typography.bodySmall) }
                    }
                }
            }
        }
    }
}
