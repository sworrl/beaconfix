package org.sworrl.beaconfix.ui.help

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.help.Origin
import org.sworrl.beaconfix.share.ShareText
import org.sworrl.beaconfix.ui.Intents
import org.sworrl.beaconfix.ui.compass
import org.sworrl.beaconfix.ui.km
import org.sworrl.beaconfix.ui.map.MapFocus
import org.sworrl.beaconfix.ui.metres
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Orange
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.HelpBadge
import org.sworrl.beaconfix.ui.vm.HelpRowModel

/** "12 km" / "850 m" — tenths below 10 km, whole kilometres above (unit rules live in ui/Common). */
fun distanceText(m: Double): String = if (m < 0) "" else if (m >= 10_000) km(m / 1000.0) else metres(m)

/** The badge colour: green for a confirmed ER, amber for "call ahead" / "not an ER", neutral otherwise. */
@Composable
fun badgeColor(b: HelpBadge): Color = when (b) {
    HelpBadge.ChildrensEr, HelpBadge.Er, HelpBadge.PedsDept -> Green
    is HelpBadge.ChildrensCampus, HelpBadge.ChildrensUnconfirmed, HelpBadge.ErUnconfirmed, HelpBadge.NotEr -> Orange
    HelpBadge.None -> Slate
}

@Composable
fun badgeText(b: HelpBadge): String = if (b.res == 0) "" else stringResource(b.res, *b.args.toTypedArray())

/**
 * One help place: icon, name, the confidence label as text, distance · direction · drive time · "from you", address,
 * hours with open / closed, then Call (or "No phone listed" + Search), Directions, Share and Map. TalkBack reads the
 * text as one item and offers "Call <name>" / "Directions to <name>" as actions; the emoji is not read.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun HelpPlaceRow(row: HelpRowModel, originKind: String, onMap: () -> Unit, modifier: Modifier = Modifier, guessed: Boolean = false) {
    val ctx = LocalContext.current
    val p = row.place
    val name = p.name.ifBlank { stringResource(R.string.help_title) }
    val callCd = stringResource(R.string.help_cd_call, name)
    val dirCd = stringResource(R.string.help_cd_directions, name)
    val call = { Intents.dial(ctx, p.phone); Unit }
    val go = { Intents.navigate(ctx, p.lat, p.lon, name); Unit }
    val showOnMap = { MapFocus.target.value = MapFocus.Target(p.lat, p.lon, poiKey = p.osmKey.ifBlank { null }, label = name); onMap() }
    val from = when (originKind) { Origin.PHONE -> stringResource(R.string.help_from_you); Origin.RV -> stringResource(R.string.help_from_rv); else -> "" }
    val closes = p.hours.takeIf { p.openNow == true }?.let { h ->
        org.sworrl.beaconfix.help.OpeningHours.closesAt(h, java.time.LocalDateTime.now())?.let { String.format(java.util.Locale.US, "%02d:%02d", it.hour, it.minute) }
    }

    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(
            Modifier.fillMaxWidth().semantics(mergeDescendants = true) {
                customActions = buildList {
                    if (p.phone.isNotBlank()) add(CustomAccessibilityAction(callCd) { call(); true })
                    add(CustomAccessibilityAction(dirCd) { go(); true })
                }
            },
            verticalAlignment = Alignment.Top, horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text(row.icon, style = MaterialTheme.typography.titleLarge, modifier = Modifier.clearAndSetSemantics { })
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(name, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
                val badge = badgeText(row.badge)
                if (badge.isNotEmpty()) Text(badge, color = badgeColor(row.badge), style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.Bold)
                if (guessed && row.badge != HelpBadge.NotEr) Text(stringResource(R.string.help_badge_guessed), color = Orange, style = MaterialTheme.typography.labelMedium)
                val where = listOf(
                    if (p.distM >= 0) distanceText(p.distM) + " " + compass(p.brgDeg) else "",
                    row.eta, from,
                ).filter { it.isNotBlank() }.joinToString(" · ")
                if (where.isNotEmpty()) Text(where, style = MaterialTheme.typography.bodyMedium)
                if (p.address.isNotBlank()) Text(p.address, color = Slate, style = MaterialTheme.typography.bodySmall)
                val open = when (p.openNow) {
                    true -> stringResource(R.string.help_open) + (closes?.let { " · " + stringResource(R.string.help_closes, it) } ?: "")
                    false -> stringResource(R.string.help_closed)
                    null -> ""
                }
                val hours = listOf(open, p.hours).filter { it.isNotBlank() }.joinToString(" · ")
                if (hours.isNotEmpty()) Text(hours, color = if (p.openNow == false) Orange else Slate, style = MaterialTheme.typography.bodySmall)
            }
        }
        FlowRow(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            if (p.phone.isNotBlank()) {
                FilledTonalButton(onClick = call, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = callCd }) { Text(stringResource(R.string.help_call)) }
            } else {
                Text(stringResource(R.string.help_no_phone), color = Slate, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(top = 14.dp))
                val q = stringResource(R.string.help_search_query, name)
                val searchCd = stringResource(R.string.help_cd_search, name)
                TextButton(onClick = { Intents.searchWeb(ctx, q) }, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = searchCd }) { Text(stringResource(R.string.help_search)) }
            }
            OutlinedButton(onClick = go, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = dirCd }) { Text(stringResource(R.string.help_directions)) }
            val shareCd = stringResource(R.string.help_cd_share, name)
            TextButton(onClick = { Intents.shareText(ctx, ShareText.place(name, p.lat, p.lon, p.address, p.phone), name) }, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = shareCd }) {
                Text(stringResource(R.string.help_share))
            }
            val mapCd = stringResource(R.string.help_cd_map, name)
            TextButton(onClick = showOnMap, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = mapCd }) { Text(stringResource(R.string.help_on_map)) }
        }
    }
}
