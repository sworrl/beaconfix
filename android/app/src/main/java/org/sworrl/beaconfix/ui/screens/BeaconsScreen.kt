package org.sworrl.beaconfix.ui.screens

import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.collector.CollectorStatus
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.ui.Chip
import org.sworrl.beaconfix.ui.EmptyState
import org.sworrl.beaconfix.ui.FixGrade
import org.sworrl.beaconfix.ui.metres
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.estimate.FitMetrics
import androidx.compose.ui.res.stringResource
import kotlin.math.roundToInt
import org.sworrl.beaconfix.ui.SecurityText
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.gradeColor
import org.sworrl.beaconfix.ui.gradeGlyph
import org.sworrl.beaconfix.ui.secName
import org.sworrl.beaconfix.ui.theme.Magenta
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.BeaconsViewModel
import org.sworrl.beaconfix.ui.vm.LiveViewModel

/** The radio security audit: every beacon graded, worst first, with the reasoning spelled out (same text as the desktop). */
@Composable
fun BeaconsScreen(vm: BeaconsViewModel = hiltViewModel(), live: LiveViewModel = hiltViewModel()) {
    val aps by vm.aps.collectAsStateWithLifecycle(); val q by vm.query.collectAsStateWithLifecycle()
    val scan by live.status.state.collectAsStateWithLifecycle()
    val ranges by vm.ranges.collectAsStateWithLifecycle()
    var filter by remember { mutableStateOf("all") }   // all | insecure | range | placed
    var open by remember { mutableStateOf<String?>(null) }
    val inRange = scan.scan.associateBy { it.bssid }
    val graded = aps.map { it to SecurityText.grade(it.security) }
        .filter { (a, g) -> when (filter) { "insecure" -> g == "critical" || g == "weak"; "range" -> a.bssid in inRange; "placed" -> a.lat != null; else -> true } }
        .sortedWith(compareBy({ (_, g) -> listOf("critical", "weak", "ok", "unknown", "strong").indexOf(g) }, { (a, _) -> -(inRange[a.bssid]?.dbm ?: -200) }, { (a, _) -> -a.lastSeen }))
    val crit = aps.count { SecurityText.grade(it.security) == "critical" }; val weak = aps.count { SecurityText.grade(it.security) == "weak" }; val strong = aps.count { SecurityText.grade(it.security) == "strong" }
    Column(Modifier.fillMaxSize()) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
            Text("Beacons", style = MaterialTheme.typography.headlineSmall)
            Text("${aps.size} known · ${inRange.size} in range · $crit insecure · $weak weak · $strong WPA3", color = Slate, style = MaterialTheme.typography.bodySmall)
            if (scan.rttHeard > 0) Text(rttLine(scan.rttHeard, scan.rttHeardAz, scan.rttAnswered, scan.rttState), color = Slate, style = MaterialTheme.typography.bodySmall)
            OutlinedTextField(q, { vm.query.value = it }, Modifier.fillMaxWidth(), label = { Text("Search name or BSSID") }, singleLine = true)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                for ((k, l) in listOf("all" to "All", "insecure" to "Insecure", "range" to "In range", "placed" to "Placed")) FilterChip(selected = filter == k, onClick = { filter = k }, label = { Text(l) })
            }
        }
        if (aps.isEmpty()) EmptyState("📶", "No beacons yet", "Turn the collector on (Home) or sync with a desktop; every network you hear lands here with its security grade.")
        LazyColumn(Modifier.fillMaxSize(), contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 12.dp, vertical = 4.dp)) {
            items(graded, key = { it.first.bssid }) { (a, g) -> BeaconRow(a, g, inRange[a.bssid]?.dbm, open == a.bssid, ranges[a.bssid]) { open = if (open == a.bssid) null else a.bssid } }
        }
    }
}

@Composable
private fun BeaconRow(a: ApEntity, grade: String, dbm: Int?, expanded: Boolean, rtt: org.sworrl.beaconfix.ranging.ApRange?, onToggle: () -> Unit) {
    Card(Modifier.fillMaxWidth().padding(vertical = 3.dp).clickable { onToggle() }) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(gradeGlyph(grade), color = gradeColor(grade), style = MaterialTheme.typography.titleMedium)
                Column(Modifier.weight(1f)) {
                    Text(a.ssid.ifEmpty { "(hidden)" }, fontWeight = FontWeight.Bold, color = if (a.home) Magenta else MaterialTheme.colorScheme.onSurface)
                    Text("${a.bssid} · ${a.band.ifEmpty { "?" }} GHz${if (a.ch > 0) " ch ${a.ch}" else ""}" + (dbm?.let { " · $it dBm" } ?: " · seen ${ago(a.lastSeen)}"), color = Slate, style = MaterialTheme.typography.bodySmall)
                }
                if (FixGrade.graded(a)) {
                    val letter = a.grade ?: ""
                    Chip(if (FixGrade.isMobile(a) || FixGrade.isRegion(a) || a.score == null) stringResource(R.string.fit_badge_letter, letter) else stringResource(R.string.fit_badge, letter, a.score.roundToInt()), FixGrade.color(letter))
                }
                Chip(secName(a.security), gradeColor(grade))
                if (a.home) Chip("HOME", Magenta)
            }
            val where = when { a.lat == null -> "no position yet — needs samples from two places"; a.posSource == "observed" -> "placed by your samples · ±${(a.acc ?: 0.0).toInt()} m" + (a.residual?.let { " · fit ${it.toInt()} m" } ?: ""); a.posSource == "placed" -> "mapped position (Apple / WiGLE) · ±${(a.acc ?: 0.0).toInt()} m"; else -> "position from the desktop · ±${(a.acc ?: 0.0).toInt()} m" }
            Text(where, color = Slate, style = MaterialTheme.typography.bodySmall)
            // the AP answered Wi-Fi RTT (802.11mc / az): the newest measured range (offset-corrected), from where it was taken
            if (rtt != null) Text(rtt.text + (if (rtt.az) " (802.11az)" else "") + " · ${ago(rtt.time)}", color = org.sworrl.beaconfix.ui.theme.Cyan, style = MaterialTheme.typography.bodySmall)
            AnimatedVisibility(expanded) {
                Column(Modifier.padding(top = 6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (i in SecurityText.forSecurity(a.security)) {
                        Text("▸ ${i.title}", fontWeight = FontWeight.Bold, color = when (i.severity) { "critical" -> gradeColor("critical"); "weak" -> gradeColor("weak"); else -> MaterialTheme.colorScheme.onSurface }, style = MaterialTheme.typography.bodySmall)
                        Text(i.nerd, style = MaterialTheme.typography.bodySmall)
                    }
                    if (FixGrade.graded(a)) FixGradeDetails(a)
                    if (a.travelling) Text("Travels with you (heard at places far apart) — never used for positioning.", color = Magenta, style = MaterialTheme.typography.bodySmall)
                    TextButton(onClick = onToggle) { Text("Less") }
                }
            }
        }
    }
}

/** The graded estimate (docs/GRADING.md): letter and score, P(error < 25 m), R95/CEP50, places, devices, score parts, flags. */
@Composable
private fun FixGradeDetails(a: ApEntity) {
    val m = FitMetrics.parse(a.fitMetrics)
    val col = FixGrade.color(a.grade)
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.fit_title), fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
            val letter = a.grade ?: ""
            Chip(if (a.score != null && !FixGrade.isMobile(a) && !FixGrade.isRegion(a)) stringResource(R.string.fit_badge, letter, a.score.roundToInt()) else stringResource(R.string.fit_badge_letter, letter), col)
            if (m?.source == "desktop") Text(stringResource(R.string.fit_by_desktop), color = Slate, style = MaterialTheme.typography.labelSmall)
        }
        when {
            FixGrade.isMobile(a) -> Text(stringResource(R.string.fit_mobile), style = MaterialTheme.typography.bodySmall)
            else -> {
                if (FixGrade.isRegion(a) && a.r95 != null) Text(stringResource(R.string.fit_region, metres(a.r95)), style = MaterialTheme.typography.bodySmall)
                a.pWithin25?.let { Text(stringResource(R.string.fit_within25, (it * 100).roundToInt()), color = col, fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall) }
                if (a.r95 != null && a.cep50 != null) Text(stringResource(R.string.fit_r95_cep50, metres(a.r95), metres(a.cep50)), style = MaterialTheme.typography.bodySmall)
            }
        }
        val places = a.vantage
        if (places != null) Text(if (a.devices != null) stringResource(R.string.fit_places_devices, places, a.devices) else stringResource(R.string.fit_places, places), color = Slate, style = MaterialTheme.typography.bodySmall)
        if (m != null) {
            val parts = listOf("P" to m.cP, "G" to m.cG, "E" to m.cE, "F" to m.cF, "S" to m.cS, "T" to m.cT, "X" to m.cX)
            if (parts.any { it.second != null }) {
                Text(stringResource(R.string.fit_components) + "  " + parts.joinToString("  ") { (k, v) -> "$k " + (if (v == null || v < 0) "—" else String.format(java.util.Locale.ROOT, "%.2f", v)) },
                    style = MaterialTheme.typography.bodySmall, fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace)
                Text(stringResource(R.string.fit_components_legend), color = Slate, style = MaterialTheme.typography.labelSmall)
            }
            val fExt = stringResource(R.string.fit_flag_extrapolated); val fAmb = stringResource(R.string.fit_flag_ambiguous)
            val fMoved = stringResource(R.string.fit_flag_moved); val fFragile = stringResource(R.string.fit_flag_fragile)
            val flags = ArrayList<String>()
            if (m.inHull == false) flags.add(fExt)
            if (m.ambiguous == true) flags.add(fAmb)
            if (m.moved == true) flags.add(fMoved)
            if (m.fragile(a.semiMajor)) flags.add(fFragile)
            if (flags.isNotEmpty()) {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) { for (f in flags) Chip(f, FixGrade.color("E")) }
                Text(stringResource(R.string.fit_flags_help), color = Slate, style = MaterialTheme.typography.labelSmall)
            }
        }
    }
}
