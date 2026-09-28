package org.sworrl.beaconfix.ui.help

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.ui.Emergency
import org.sworrl.beaconfix.ui.Intents
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.theme.Orange
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.HelpBadge
import org.sworrl.beaconfix.ui.vm.HelpRowModel
import org.sworrl.beaconfix.ui.vm.HelpViewModel

/**
 * One-glance nearest help for the Places header: "🧸 name · distance · ETA" (or the nearest ER when no children's ER
 * is known) plus a "Call 911" button. Tapping the card ([onOpen]) opens the Help screen. Refreshes only from a desktop.
 */
@Composable
fun HelpSummaryCard(onOpen: () -> Unit, modifier: Modifier = Modifier) {
    val vm: HelpViewModel = hiltViewModel()
    LaunchedEffect(Unit) { vm.refreshIfStale() }
    HelpCardBody(vm, onOpen, modifier, withEr = false)
}

/** The Home screen's help card: the children's ER line and the nearest-ER line, plus "Call 911". */
@Composable
fun HomeHelpCard(onOpen: () -> Unit) {
    val vm: HelpViewModel = hiltViewModel()
    LaunchedEffect(Unit) { vm.refreshIfStale() }
    HelpCardBody(vm, onOpen, Modifier.padding(horizontal = 16.dp, vertical = 6.dp), withEr = true)
}

@Composable
private fun HelpCardBody(vm: HelpViewModel, onOpen: () -> Unit, modifier: Modifier, withEr: Boolean) {
    val ctx = LocalContext.current
    val ui by vm.ui.collectAsState()
    val snap by vm.snapshot.collectAsState()
    val number = Emergency.accept(ui.number, snap.countryCode.ifBlank { null })
    val callLabel = stringResource(R.string.help_call_number, number)
    val first = ui.peds ?: ui.er
    OutlinedCard(onClick = onOpen, modifier = modifier.fillMaxWidth()) {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(stringResource(R.string.help_card_title), style = MaterialTheme.typography.labelLarge, color = Slate)
                if (first == null) Text(stringResource(R.string.help_card_empty), style = MaterialTheme.typography.bodyMedium)
                else {
                    CardLine(first, er = first === ui.er, bold = true)
                    if (withEr && first !== ui.er) ui.er?.let { CardLine(it, er = true, bold = false) }
                }
                if (snap.stale && snap.fetchedAt > 0) Text(stringResource(R.string.help_card_saved, ago(snap.fetchedAt)), color = Orange, style = MaterialTheme.typography.labelSmall)
            }
            Button(
                onClick = { Intents.dial(ctx, number) },
                modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = callLabel },
                colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error, contentColor = MaterialTheme.colorScheme.onError),
            ) { Text(callLabel, fontWeight = FontWeight.Bold) }
        }
    }
}

/**
 * "🧸 name · 38 km · ~45 min (est.)" and, under it, the confidence text whenever it is not a plain confirmed ER
 * ("Children's hospital — ER not confirmed, call ahead", "Not an ER", …; plan §7: never without it). TalkBack hears
 * both, without the emoji.
 */
@Composable
private fun CardLine(r: HelpRowModel, er: Boolean, bold: Boolean) {
    val name = if (er) stringResource(R.string.help_card_er, r.place.name) else r.place.name
    val line = listOf(name, distanceText(r.place.distM), r.eta).filter { it.isNotBlank() }.joinToString(" · ")
    val tier = if (cardShowsBadge(r.badge)) badgeText(r.badge) else ""
    val spoken = if (tier.isEmpty()) line else "$line. $tier"
    Column(Modifier.semantics(mergeDescendants = true) { contentDescription = spoken }) {
        Text(
            r.icon + " " + line,
            style = if (bold) MaterialTheme.typography.bodyMedium else MaterialTheme.typography.bodySmall,
            fontWeight = if (bold) FontWeight.Bold else null, maxLines = 2, overflow = TextOverflow.Ellipsis,
        )
        if (tier.isNotEmpty()) Text(tier, color = badgeColor(r.badge), style = MaterialTheme.typography.labelSmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
}

/** The card spells out every confidence level except the plain ones (a confirmed children's ER, a confirmed ER). */
internal fun cardShowsBadge(b: HelpBadge): Boolean = b != HelpBadge.None && b != HelpBadge.Er && b != HelpBadge.ChildrensEr
