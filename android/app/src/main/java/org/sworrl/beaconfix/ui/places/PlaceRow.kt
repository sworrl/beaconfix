package org.sworrl.beaconfix.ui.places

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
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
import org.sworrl.beaconfix.share.ShareText
import org.sworrl.beaconfix.ui.Chip
import org.sworrl.beaconfix.ui.Intents
import org.sworrl.beaconfix.ui.compass
import org.sworrl.beaconfix.ui.help.distanceText
import org.sworrl.beaconfix.ui.map.MapFocus
import org.sworrl.beaconfix.ui.theme.Cyan
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Magenta
import org.sworrl.beaconfix.ui.theme.Orange
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.PlaceUi

/** A badge on a place row: text plus a colour (the text alone must carry the meaning). */
data class PlaceBadge(val res: Int, val color: Color, val arg: String? = null)

/** ER · No ER · the Kids ER tier · wheelchair · Wi-Fi · Open / Closed / closes 17:00 (pure apart from colours). */
fun placeBadges(p: PlaceUi): List<PlaceBadge> {
    val r = p.row
    val out = ArrayList<PlaceBadge>()
    when (r.peds) {
        1 -> out += PlaceBadge(R.string.places_badge_kids_er, Magenta)
        2 -> out += PlaceBadge(if (r.campus.isNotBlank()) R.string.places_badge_kids_campus else R.string.places_badge_kids_unconfirmed, Orange)
        3 -> out += PlaceBadge(R.string.places_badge_peds_dept, Magenta)
        4 -> out += PlaceBadge(R.string.places_badge_kids_urgent, Orange)
    }
    if (r.er == "no") out += PlaceBadge(R.string.places_badge_no_er, Orange)
    else if ((r.emergency || r.er == "yes") && r.peds != 4 && r.cat != "urgent") out += PlaceBadge(R.string.places_badge_er, Green)
    when (r.wheelchair) { "yes", "designated" -> out += PlaceBadge(R.string.places_badge_wheelchair, Cyan); "limited" -> out += PlaceBadge(R.string.places_badge_wheelchair_limited, Cyan) }
    if (r.wifi) out += PlaceBadge(R.string.places_badge_wifi, Cyan)
    when (p.openNow) {
        true -> out += if (p.closes != null) PlaceBadge(R.string.places_closes, Green, p.closes) else PlaceBadge(R.string.places_open, Green)
        false -> out += PlaceBadge(R.string.places_closed, Orange)
        null -> Unit
    }
    return out
}

/**
 * One place: icon, name, kind · distance · direction, badges, detail, address, hours, then Call / Directions / Share /
 * Website / Map. TalkBack reads the text as one item with "Call …" and "Directions to …" actions; the emoji is skipped.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun PlaceRow(p: PlaceUi, onMap: () -> Unit, modifier: Modifier = Modifier) {
    val ctx = LocalContext.current
    val r = p.row
    val name = r.name.ifBlank { r.label.ifBlank { r.cat } }
    val callCd = stringResource(R.string.places_cd_call, name)
    val dirCd = stringResource(R.string.places_cd_directions, name)
    val call = { Intents.dial(ctx, r.phone); Unit }
    val go = { Intents.navigate(ctx, r.lat, r.lon, name); Unit }
    Card(modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 3.dp)) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(
                Modifier.fillMaxWidth().semantics(mergeDescendants = true) {
                    customActions = buildList {
                        if (r.phone.isNotBlank()) add(CustomAccessibilityAction(callCd) { call(); true })
                        add(CustomAccessibilityAction(dirCd) { go(); true })
                    }
                },
                verticalAlignment = Alignment.Top, horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Text(r.icon.ifEmpty { "📍" }, style = MaterialTheme.typography.titleLarge, modifier = Modifier.clearAndSetSemantics { })
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(name, fontWeight = FontWeight.Bold, style = MaterialTheme.typography.titleSmall)
                    val where = listOf(r.label, if (p.distM >= 0) distanceText(p.distM) + " " + compass(p.brgDeg) else "").filter { it.isNotBlank() }.joinToString(" · ")
                    if (where.isNotEmpty()) Text(where, color = Slate, style = MaterialTheme.typography.bodySmall)
                    val badges = placeBadges(p)
                    if (badges.isNotEmpty()) FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        badges.forEach { b -> Chip(if (b.arg != null) stringResource(b.res, b.arg) else stringResource(b.res), b.color) }
                    }
                    if (r.detail.isNotBlank()) Text(r.detail, color = Slate, style = MaterialTheme.typography.bodySmall)
                    if (r.address.isNotBlank()) Text(r.address, style = MaterialTheme.typography.bodySmall)
                    if (r.hours.isNotBlank()) Text("🕑 " + r.hours, color = Slate, style = MaterialTheme.typography.bodySmall)
                }
            }
            FlowRow(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                if (r.phone.isNotBlank()) TextButton(onClick = call, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = callCd }) { Text(stringResource(R.string.places_call)) }
                TextButton(onClick = go, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = dirCd }) { Text(stringResource(R.string.places_directions)) }
                val shareCd = stringResource(R.string.places_cd_share, name)
                TextButton(onClick = { Intents.shareText(ctx, ShareText.place(name, r.lat, r.lon, r.address, r.phone), name) }, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = shareCd }) {
                    Text(stringResource(R.string.places_share))
                }
                if (r.website.isNotBlank()) {
                    val webCd = stringResource(R.string.places_cd_website, name)
                    TextButton(onClick = { Intents.web(ctx, r.website) }, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = webCd }) { Text(stringResource(R.string.places_website)) }
                }
                val mapCd = stringResource(R.string.places_cd_map, name)
                TextButton(
                    onClick = { MapFocus.target.value = MapFocus.Target(r.lat, r.lon, poiKey = r.key, label = name); onMap() },
                    modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = mapCd },
                ) { Text(stringResource(R.string.places_map)) }
            }
        }
    }
}
