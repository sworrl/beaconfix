package org.sworrl.beaconfix.ui.screens

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.data.api.PoiDto
import org.sworrl.beaconfix.ui.EmptyState
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.compass
import org.sworrl.beaconfix.ui.metres
import org.sworrl.beaconfix.ui.theme.Red
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.LiveViewModel

private val GROUPS = listOf("all" to "All", "emergency" to "Help", "civic" to "Civic", "kids" to "Kids & fun", "food" to "Food", "services" to "Services", "camping" to "Camping", "fuel" to "Fuel & EV")
private fun groupOf(p: PoiDto): String = p.group.ifEmpty { when (p.cat) { "police", "fire", "hospital", "pharmacy", "clinic" -> "emergency"; "library", "post", "townhall", "courthouse" -> "civic"; "playground", "park", "dogpark", "museum", "zoo", "cinema" -> "kids"; "food", "cafe", "grocery" -> "food"; "fuel", "propane", "ev" -> "fuel"; "camp", "rv", "dump", "water" -> "camping"; else -> "services" } }

/** Places around the desktop's fix (OpenStreetMap via the desktop): grouped, with address, distance and one-tap call / directions. */
@Composable
fun NearbyScreen(live: LiveViewModel = hiltViewModel()) {
    val views by live.views.collectAsState()
    val ctx = LocalContext.current
    var group by remember { mutableStateOf("all") }; var q by remember { mutableStateOf("") }
    val pois = views.flatMap { it.pois }.distinctBy { it.name + it.lat + it.lon }.sortedBy { it.d }
    val shown = pois.filter { (group == "all" || groupOf(it) == group) && (q.isBlank() || it.name.contains(q, true) || it.label.contains(q, true) || it.address.contains(q, true)) }
    Column(Modifier.fillMaxSize()) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
            Text("Nearby", style = MaterialTheme.typography.headlineSmall)
            Text("${pois.size} places within reach of the desktop's fix", color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedTextField(q, { q = it }, Modifier.fillMaxWidth(), label = { Text("Search") }, singleLine = true)
            LazyRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) { items(GROUPS) { (k, l) -> FilterChip(selected = group == k, onClick = { group = k }, label = { Text(l) }) } }
        }
        if (views.isEmpty()) EmptyState("🗺", "No desktop connected", "Places come from the BeaconFix desktop's map database (OpenStreetMap). Pair one to see what is around you.")
        else if (pois.isEmpty()) EmptyState("🗺", "No places yet", "The desktop is still fetching its surroundings, or it has no precise fix. Pull to refresh in a minute.", "Refresh") { live.refresh(setOf("pois")) }
        else {
            // the nearest help: police, fire, hospital
            val help = listOf("police", "fire", "hospital").mapNotNull { c -> pois.firstOrNull { it.cat == c } }
            if (help.isNotEmpty() && (group == "all" || group == "emergency")) InfoCard("Nearest help") {
                for (p in help) Row(Modifier.fillMaxWidth().clickable { directions(ctx, p) }, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(p.icon.ifEmpty { "🚨" }); Column(Modifier.weight(1f)) { Text("${p.label}: ${p.name}", fontWeight = FontWeight.Bold); Text("${metres(p.d)} ${compass(p.brg)}" + (if (p.address.isNotEmpty()) " · ${p.address}" else ""), color = Slate, style = MaterialTheme.typography.bodySmall) }
                    if (p.phone.isNotEmpty()) TextButton(onClick = { call(ctx, p.phone) }) { Text("Call", color = Red) }
                }
                views.firstOrNull()?.trip?.locale?.emergencyNumber?.takeIf { it.isNotEmpty() }?.let { Text("Emergency number here: $it", color = Red, fontWeight = FontWeight.Bold) }
            }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 12.dp, vertical = 4.dp)) {
                items(shown, key = { it.name + it.lat + it.lon }) { p ->
                    Card(Modifier.fillMaxWidth().padding(vertical = 3.dp).clickable { directions(ctx, p) }) {
                        Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                            Text(p.icon.ifEmpty { "📍" }, style = MaterialTheme.typography.titleLarge)
                            Column(Modifier.weight(1f)) {
                                Text(p.name.ifEmpty { p.label }, fontWeight = FontWeight.Bold)
                                Text("${p.label} · ${metres(p.d)} ${compass(p.brg)}", color = Slate, style = MaterialTheme.typography.bodySmall)
                                if (p.address.isNotEmpty()) Text(p.address, style = MaterialTheme.typography.bodySmall)
                                if (p.detail.isNotEmpty()) Text(p.detail, color = Slate, style = MaterialTheme.typography.bodySmall)
                                if (p.hours.isNotEmpty()) Text("🕑 ${p.hours}", color = Slate, style = MaterialTheme.typography.bodySmall)
                                Row { if (p.phone.isNotEmpty()) TextButton(onClick = { call(ctx, p.phone) }) { Text("☎ ${p.phone}") }; if (p.website.isNotEmpty()) TextButton(onClick = { open(ctx, p.website) }) { Text("Website") }; TextButton(onClick = { directions(ctx, p) }) { Text("Directions") } }
                            }
                        }
                    }
                }
            }
        }
    }
}
private fun call(ctx: android.content.Context, phone: String) = runCatching { ctx.startActivity(Intent(Intent.ACTION_DIAL, Uri.parse("tel:" + phone.replace(" ", "")))) }
private fun open(ctx: android.content.Context, url: String) = runCatching { ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(if (url.startsWith("http")) url else "https://$url"))) }
private fun directions(ctx: android.content.Context, p: PoiDto) = runCatching { ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("geo:${p.lat},${p.lon}?q=${p.lat},${p.lon}(${Uri.encode(p.name.ifEmpty { p.label })})"))) }
