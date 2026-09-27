package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.ui.Chip
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Magenta
import org.sworrl.beaconfix.ui.theme.Red
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.wifi.CurrentNetworkMonitor
import org.sworrl.beaconfix.wifi.CurrentWifi
import org.sworrl.beaconfix.wifi.WifiGrade
import javax.inject.Inject

data class CurrentWifiUi(val wifi: CurrentWifi? = null, val verdict: WifiGrade.Verdict? = null)

@HiltViewModel
class CurrentWifiViewModel @Inject constructor(private val monitor: CurrentNetworkMonitor, prefs: Prefs) : ViewModel() {
    init { monitor.start() }                                  // idempotent; BeaconFixApp starts it too

    val ui: StateFlow<CurrentWifiUi> = combine(monitor.current, prefs.homePatterns) { w, patterns ->
        CurrentWifiUi(w, w?.let { WifiGrade.grade(it.security, WifiGrade.isHome(it.ssid, it.bssid, patterns)) })
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), CurrentWifiUi(monitor.current.value))

    /** The name is hidden until location permission is granted; ask Android again when the card shows up. */
    fun refreshIfHidden() { if (monitor.current.value?.ssid?.isEmpty() == true) monitor.refresh() }
}

/** Home's "Connected Wi-Fi" card: the network's name, how safe it is in plain words, and a banking hint when it is not. */
@Composable
fun CurrentWifiCard(vm: CurrentWifiViewModel = hiltViewModel()) {
    val ui by vm.ui.collectAsState()
    LaunchedEffect(Unit) { vm.refreshIfHidden() }
    InfoCard(stringResource(R.string.wifi_card_title)) {
        val w = ui.wifi
        val v = ui.verdict
        if (w == null || v == null) {
            Text(stringResource(R.string.wifi_card_not_connected), color = Slate, style = MaterialTheme.typography.bodyMedium)
            return@InfoCard
        }
        val (word, color) = when (v.level) {
            WifiGrade.Level.BAD -> stringResource(R.string.wifi_card_bad) to Red
            WifiGrade.Level.OK -> stringResource(R.string.wifi_card_ok) to Green
            WifiGrade.Level.UNKNOWN -> stringResource(R.string.wifi_card_unknown) to Slate
        }
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(w.ssid.ifEmpty { stringResource(R.string.wifi_card_hidden_name) }, Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge,
                maxLines = 2, overflow = TextOverflow.Ellipsis)
            if (v.home) Chip(stringResource(R.string.wifi_card_home), Magenta)
            Chip(word, color)
        }
        Text(v.label, color = color, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite })
        if (v.level == WifiGrade.Level.BAD) {
            val advice = if (w.security == "open") R.string.wifi_card_advice_open else R.string.wifi_card_advice_weak
            Text(stringResource(advice), style = MaterialTheme.typography.bodyMedium)
        }
        if (w.frequencyMhz > 0) {
            val band = if (w.channel > 0) stringResource(R.string.wifi_card_band, w.band, w.channel) else stringResource(R.string.wifi_card_band_only, w.band)
            Text(band, color = Slate, style = MaterialTheme.typography.bodySmall)
        }
    }
}
