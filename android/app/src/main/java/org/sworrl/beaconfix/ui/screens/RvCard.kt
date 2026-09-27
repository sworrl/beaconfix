package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.heightIn
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.share.RvCardModel
import org.sworrl.beaconfix.share.ShareViewModel
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.Intents
import org.sworrl.beaconfix.ui.compass
import org.sworrl.beaconfix.ui.map.MapFocus
import org.sworrl.beaconfix.ui.metres
import org.sworrl.beaconfix.ui.theme.Slate

@Composable
private fun ageText(ms: Long?): String {
    val a = ms?.let { RvCardModel.age(it) } ?: return stringResource(R.string.rv_age_now)
    return when (a.unit) {
        'm' -> stringResource(R.string.rv_age_min, a.value)
        'h' -> stringResource(R.string.rv_age_h, a.value)
        else -> stringResource(R.string.rv_age_d, a.value)
    }
}

/**
 * "Where's the RV": distance, direction and age of the RV's last known position from this phone, with Navigate back,
 * Share RV spot and Share my location; then our other devices. Reads only what is saved on the phone, so it works
 * with the desktop out of reach. Tapping the RV line shows it on the map.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun RvCard(onMap: () -> Unit, vm: ShareViewModel = hiltViewModel()) {
    val ctx = LocalContext.current
    val st by vm.rv.collectAsState()
    val sharing by vm.sharing.collectAsState()
    val rvName = stringResource(R.string.rv_name)
    InfoCard(stringResource(R.string.rv_title)) {
        val rv = st.rv
        if (rv == null) {
            Text(stringResource(R.string.rv_unknown), style = MaterialTheme.typography.bodyLarge)
            Text(stringResource(R.string.rv_unknown_hint), color = Slate, style = MaterialTheme.typography.bodySmall)
        } else {
            val seen = ageText(st.ageMs)
            val line = if (st.distM != null && st.brgDeg != null) stringResource(R.string.rv_line, metres(st.distM!!), compass(st.brgDeg!!), seen)
                       else stringResource(R.string.rv_line_no_phone, seen)
            Text(line, style = MaterialTheme.typography.bodyLarge,
                modifier = Modifier.clickable(role = Role.Button, onClickLabel = stringResource(R.string.rv_show_on_map)) { MapFocus.target.value = MapFocus.Target(rv.lat, rv.lon, label = rvName); onMap() })
            if (rv.acc > 0 || rv.name.isNotBlank()) Text(listOfNotNull(rv.name.takeIf { it.isNotBlank() }, rv.acc.takeIf { it > 0 }?.let { stringResource(R.string.rv_accuracy, metres(it)) }).joinToString(" · "),
                color = Slate, style = MaterialTheme.typography.bodySmall)
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            if (rv != null) {
                OutlinedButton(onClick = { Intents.navigate(ctx, rv.lat, rv.lon, rvName) }, modifier = Modifier.heightIn(min = 48.dp)) { Text(stringResource(R.string.rv_navigate)) }
                OutlinedButton(onClick = { vm.shareRv(ctx, rv) }, modifier = Modifier.heightIn(min = 48.dp)) { Text(stringResource(R.string.rv_share)) }
            }
            OutlinedButton(onClick = { vm.shareLocation(ctx) }, enabled = !sharing, modifier = Modifier.heightIn(min = 48.dp)) {
                if (sharing) CircularProgressIndicator(Modifier.heightIn(max = 18.dp), strokeWidth = 2.dp) else Text(stringResource(R.string.share_my_location))
            }
        }
        if (st.devices.isNotEmpty()) {
            Text(stringResource(R.string.rv_devices), style = MaterialTheme.typography.titleSmall)
            for (d in st.devices.take(6)) {
                val glyph = if (d.kind == "android" || d.kind == "phone") "📱" else "💻"
                val age = ageText(if (d.time > 0) (System.currentTimeMillis() - d.time).coerceAtLeast(0) else null)
                Text(listOfNotNull("$glyph ${d.name}", d.distM?.let { metres(it) }, age.takeIf { d.time > 0 }).joinToString(" · "), style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}
