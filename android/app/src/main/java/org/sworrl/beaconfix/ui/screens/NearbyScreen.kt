package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.ui.EmptyState
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.help.HelpSummaryCard
import org.sworrl.beaconfix.ui.help.distanceText
import org.sworrl.beaconfix.ui.places.PlaceFilterRow
import org.sworrl.beaconfix.ui.places.PlaceRow
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.PlacesEmpty
import org.sworrl.beaconfix.ui.vm.PlacesModel
import org.sworrl.beaconfix.ui.vm.PlacesStatus
import org.sworrl.beaconfix.ui.vm.PlacesViewModel

/**
 * Places around you from every paired desktop and this phone, kept offline: nearest help on top, filter chips, search,
 * and one card per place with call / directions / share / website / map. Pull to refresh; with no desktop in reach
 * "Search from this phone" lets the phone look for help places itself.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun NearbyScreen(onHelp: () -> Unit = {}, onMap: () -> Unit = {}) {
    val vm: PlacesViewModel = hiltViewModel()
    val places by vm.places.collectAsState()
    val filter by vm.filter.collectAsState()
    val total by vm.total.collectAsState()
    val status by vm.status.collectAsState()
    val paired by vm.paired.collectAsState()
    val reachable by vm.reachable.collectAsState()
    val refreshing by vm.refreshing.collectAsState()
    val searching by vm.phoneSearching.collectAsState()
    val empty = PlacesModel.empty(paired, reachable, total, places.size)

    Column(Modifier.fillMaxSize()) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
            Text(stringResource(R.string.places_title), style = MaterialTheme.typography.headlineSmall)
            if (total > 0) Text(LocalContext.current.resources.getQuantityString(R.plurals.places_count, total, total), color = Slate, style = MaterialTheme.typography.bodySmall)
        }
        PullToRefreshBox(isRefreshing = refreshing, onRefresh = { vm.refresh() }, modifier = Modifier.fillMaxSize()) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 16.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                item("help") { HelpSummaryCard(onOpen = onHelp, modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) }
                item("status") { StatusBanner(status, reachable, searching) { vm.searchFromPhone() } }
                item("search") {
                    OutlinedTextField(filter.query, { vm.setFilter(filter.copy(query = it)) }, Modifier.fillMaxWidth().padding(horizontal = 16.dp),
                        label = { Text(stringResource(R.string.places_search)) }, singleLine = true)
                }
                item("chips") { PlaceFilterRow(filter, { vm.setFilter(it) }, Modifier.padding(vertical = 4.dp)) }
                when (empty) {
                    PlacesEmpty.NONE -> items(places, key = { it.row.source + "|" + it.row.key }) { p -> PlaceRow(p, onMap) }
                    PlacesEmpty.FILTERED -> item("empty") { EmptyState("🔎", stringResource(R.string.places_empty_filter_title), stringResource(R.string.places_empty_filter_body)) }
                    PlacesEmpty.UNPAIRED -> item("empty") {
                        EmptyState("🗺", stringResource(R.string.places_empty_unpaired_title), stringResource(R.string.places_empty_unpaired_body),
                            stringResource(R.string.places_search_phone)) { vm.searchFromPhone() }
                    }
                    PlacesEmpty.UNREACHABLE -> item("empty") {
                        EmptyState("📡", stringResource(R.string.places_empty_unreachable_title), stringResource(R.string.places_empty_unreachable_body),
                            stringResource(R.string.places_search_phone)) { vm.searchFromPhone() }
                    }
                    PlacesEmpty.FETCHING -> item("empty") {
                        EmptyState("🗺", stringResource(R.string.places_empty_fetching_title), stringResource(R.string.places_empty_fetching_body),
                            stringResource(R.string.places_refresh)) { vm.refresh() }
                    }
                }
            }
        }
    }
}

/** "Updated just now" or "Saved 2 h ago near <place> · 12 km from here", plus "Search from this phone" when no desktop answers. */
@Composable
private fun StatusBanner(s: PlacesStatus, reachable: Boolean, searching: Boolean, onSearch: () -> Unit) {
    if (s.newest <= 0 && reachable) return
    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 2.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        if (s.newest > 0) {
            val fresh = System.currentTimeMillis() - s.newest < 120_000L
            val base = when {
                fresh -> stringResource(R.string.places_updated_now)
                s.fromPhone -> stringResource(R.string.places_from_phone, ago(s.newest))
                s.near.isNotBlank() -> stringResource(R.string.places_saved_near, ago(s.newest), s.near)
                else -> stringResource(R.string.places_saved, ago(s.newest))
            }
            val far = if (!fresh && s.distM >= 1000) " · " + stringResource(R.string.places_from_here, distanceText(s.distM)) else ""
            Text(base + far, color = Slate, style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(1f).semantics { liveRegion = LiveRegionMode.Polite })
        } else Spacer(Modifier.weight(1f))
        if (!reachable) OutlinedButton(onClick = onSearch, enabled = !searching) {
            Text(if (searching) stringResource(R.string.places_searching) else stringResource(R.string.places_search_phone))
        }
    }
}
