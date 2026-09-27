package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.BuildConfig
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.Units
import org.sworrl.beaconfix.ui.theme.Slate
import javax.inject.Inject
import kotlin.math.roundToInt

data class PlacesSettingsUi(
    val units: String = "auto",
    val phonePlaces: Boolean = true,
    val phonePlacesMetered: Boolean = true,
    val pedsRadiusKm: Int = Prefs.PEDS_RADIUS_DEFAULT,
    val helpAlerts: Boolean = true,
    val wifiAlerts: Boolean = true,
    val devAutomation: Boolean = false,
    val simOffline: Boolean = false,
    val simNoDesktop: Boolean = false,
)

@HiltViewModel
class SettingsPlacesViewModel @Inject constructor(private val prefs: Prefs) : ViewModel() {
    val ui: StateFlow<PlacesSettingsUi> = combine<Any, PlacesSettingsUi>(
        prefs.units, prefs.phonePlaces, prefs.phonePlacesMetered, prefs.pedsRadiusKm, prefs.helpAlerts,
        prefs.wifiAlerts, prefs.devAutomation, prefs.simOffline, prefs.simNoDesktop,
    ) { a -> PlacesSettingsUi(a[0] as String, a[1] as Boolean, a[2] as Boolean, a[3] as Int, a[4] as Boolean, a[5] as Boolean, a[6] as Boolean, a[7] as Boolean, a[8] as Boolean) }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), PlacesSettingsUi())

    fun setUnits(v: String) = viewModelScope.launch { prefs.setUnits(v) }
    fun setPhonePlaces(v: Boolean) = viewModelScope.launch { prefs.setPhonePlaces(v) }
    fun setPhonePlacesMetered(v: Boolean) = viewModelScope.launch { prefs.setPhonePlacesMetered(v) }
    fun setPedsRadiusKm(km: Int) = viewModelScope.launch { prefs.setPedsRadiusKm(km) }
    fun setHelpAlerts(v: Boolean) = viewModelScope.launch { prefs.setHelpAlerts(v) }
    fun setWifiAlerts(v: Boolean) = viewModelScope.launch { prefs.setWifiAlerts(v) }
    fun setDevAutomation(v: Boolean) = viewModelScope.launch {
        prefs.setDevAutomation(v)
        if (!v) { prefs.setSimOffline(false); prefs.setSimNoDesktop(false) }      // switching automation off also ends any simulation
    }
    fun setSimOffline(v: Boolean) = viewModelScope.launch { prefs.setSimOffline(v) }
    fun setSimNoDesktop(v: Boolean) = viewModelScope.launch { prefs.setSimNoDesktop(v) }
    /** Home patterns were edited on the phone: the next sync pushes them to a desktop with control access. */
    fun markHomeDirty() = viewModelScope.launch { prefs.setHomeDirty(true) }
}

/** Units, the phone's own place search and the heads-up switches (Settings). */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsPlacesSection(vm: SettingsPlacesViewModel = hiltViewModel()) {
    val ui by vm.ui.collectAsState()
    val imperialNow = when (ui.units) { "imperial" -> true; "metric" -> false; else -> Units.imperial }
    InfoCard(stringResource(R.string.a10_places_title)) {
        Text(stringResource(R.string.a10_units), style = MaterialTheme.typography.bodyMedium)
        val options = listOf("auto" to R.string.a10_units_auto, "metric" to R.string.a10_units_metric, "imperial" to R.string.a10_units_imperial)
        SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
            options.forEachIndexed { i, (key, label) ->
                SegmentedButton(selected = ui.units == key, onClick = { vm.setUnits(key) }, shape = SegmentedButtonDefaults.itemShape(i, options.size)) { Text(stringResource(label)) }
            }
        }
        Text(stringResource(R.string.a10_units_hint), color = Slate, style = MaterialTheme.typography.bodySmall)

        SwitchRow(stringResource(R.string.a10_phone_places), stringResource(R.string.a10_phone_places_hint), ui.phonePlaces) { vm.setPhonePlaces(it) }
        SwitchRow(stringResource(R.string.a10_phone_places_metered), stringResource(R.string.a10_phone_places_metered_hint), ui.phonePlacesMetered, enabled = ui.phonePlaces) { vm.setPhonePlacesMetered(it) }

        var km by remember(ui.pedsRadiusKm) { mutableFloatStateOf(ui.pedsRadiusKm.toFloat()) }
        val radius = Units.wholeKm(km.roundToInt(), imperialNow)
        Text(stringResource(R.string.a10_peds_radius, radius), style = MaterialTheme.typography.bodyMedium)
        Slider(
            value = km, onValueChange = { km = it }, onValueChangeFinished = { vm.setPedsRadiusKm(km.roundToInt()) },
            valueRange = Prefs.PEDS_RADIUS_MIN.toFloat()..Prefs.PEDS_RADIUS_MAX.toFloat(),
            steps = (Prefs.PEDS_RADIUS_MAX - Prefs.PEDS_RADIUS_MIN) / 10 - 1,
            enabled = ui.phonePlaces,
            modifier = Modifier.semantics { stateDescription = radius },
        )
        Text(stringResource(R.string.a10_peds_radius_hint), color = Slate, style = MaterialTheme.typography.bodySmall)

        SwitchRow(stringResource(R.string.a10_help_alerts), stringResource(R.string.a10_help_alerts_hint), ui.helpAlerts) { vm.setHelpAlerts(it) }
        SwitchRow(stringResource(R.string.a10_wifi_alerts), stringResource(R.string.a10_wifi_alerts_hint), ui.wifiAlerts) { vm.setWifiAlerts(it) }
    }
}

/** Settings → Developer automation: lets adb test hooks act on a release build (off by default). */
@Composable
fun DeveloperAutomationCard(vm: SettingsPlacesViewModel = hiltViewModel()) {
    val ui by vm.ui.collectAsState()
    InfoCard(stringResource(R.string.a10_dev_title)) {
        SwitchRow(stringResource(R.string.a10_dev_automation), stringResource(R.string.a10_dev_automation_hint), ui.devAutomation) { vm.setDevAutomation(it) }
        if (ui.devAutomation || BuildConfig.DEBUG) {
            SwitchRow(stringResource(R.string.a10_sim_offline), stringResource(R.string.a10_sim_offline_hint), ui.simOffline) { vm.setSimOffline(it) }
            SwitchRow(stringResource(R.string.a10_sim_no_desktop), stringResource(R.string.a10_sim_no_desktop_hint), ui.simNoDesktop) { vm.setSimNoDesktop(it) }
        }
    }
}

@Composable
private fun SwitchRow(title: String, hint: String, checked: Boolean, enabled: Boolean = true, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyMedium, color = if (enabled) MaterialTheme.colorScheme.onSurface else Slate)
            if (hint.isNotEmpty()) Text(hint, color = Slate, style = MaterialTheme.typography.bodySmall)
        }
        Switch(checked, onChange, enabled = enabled)
    }
}
