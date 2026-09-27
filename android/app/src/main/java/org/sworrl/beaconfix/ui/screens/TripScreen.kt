package org.sworrl.beaconfix.ui.screens

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.api.Trip
import org.sworrl.beaconfix.ui.Chip
import org.sworrl.beaconfix.ui.EmptyState
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.KeyValue
import org.sworrl.beaconfix.ui.durText
import org.sworrl.beaconfix.ui.km
import org.sworrl.beaconfix.ui.map.MapFocus
import org.sworrl.beaconfix.ui.theme.Gold
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Magenta
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.LiveViewModel
import org.sworrl.beaconfix.ui.vm.TripViewModel

/**
 * The trip: the desktop's trip intelligence (distances, time moving, stops, places visited, the rank ladder and
 * milestones) — live, or as last saved with "as of HH:MM" — then this phone's own day, the journal of stops (tap
 * one to see it on the map via [onMap]) and a GPX export. Everything below the stats works offline.
 */
@Composable
fun TripScreen(onMap: () -> Unit = {}, live: LiveViewModel = hiltViewModel(), vm: TripViewModel = hiltViewModel()) {
    val views by live.views.collectAsState()
    val ui by vm.ui.collectAsState()
    val msg by vm.message.collectAsState()
    val export = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/gpx+xml")) { uri -> if (uri != null) vm.exportGpx(uri) }
    // each desktop's trip (as of when it was saved, if it is not live), else the newest saved one
    val savedTitle = stringResource(R.string.a9_saved_trip)
    val unnamed = stringResource(R.string.a9_stop_unnamed)
    val trips: List<Triple<String, Trip, Long>> = views.mapNotNull { v ->
        val saved = "trip" in v.cached || (v.stale && v.error.isNotEmpty())      // from the cache, or the desktop stopped answering
        v.trip?.let { Triple(v.desktop.name, it, if (saved) v.cachedAt else 0L) }
    }
        .ifEmpty { ui.savedTrip?.let { listOf(Triple(savedTitle, it, ui.savedTripAt)) } ?: emptyList() }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("Trip", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.weight(1f))
            TextButton(onClick = { export.launch(vm.gpxFileName()) }, enabled = ui.journal.path.isNotEmpty()) { Text(stringResource(R.string.a9_export_gpx)) }
        }
        if (msg.isNotEmpty()) Text(msg, color = Slate, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(horizontal = 16.dp))
        if (views.isEmpty() && trips.isEmpty()) EmptyState("🧭", stringResource(R.string.a9_no_desktop_title), stringResource(R.string.a9_no_desktop_body))
        for (v in views) if (v.trip == null) InfoCard(v.desktop.name) { Text(v.error.ifEmpty { "Fetching…" }, color = Slate) }
        for ((name, t, asOf) in trips) TripStatsCard(name, t, asOf)
        PhoneTripCard(ui.phone)
        TripJournal(ui.journal, ui.journalAt) { s ->
            MapFocus.target.value = MapFocus.Target(s.lat, s.lon, 16.0, label = s.place.ifEmpty { unnamed })
            onMap()
        }
        for ((_, t, _) in trips) TripDetailCards(t)
    }
}

@Composable
private fun TripStatsCard(name: String, t: Trip, asOf: Long) {
    InfoCard("$name · ${t.rank.ifEmpty { "—" }}") {
        AsOfLine(asOf)
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Level ${t.rankLevel} of ${t.rankCount}", fontWeight = FontWeight.Bold); if (t.moving) Chip("MOVING ${t.speedKmh.toInt()} km/h ${t.heading}", Green) else Chip(if (t.atHome) "AT HOME" else "STOPPED", if (t.atHome) Magenta else Gold)
        }
        if (t.nextRankAt > t.rankAt) { val p = ((t.beaconsTotal - t.rankAt).toFloat() / (t.nextRankAt - t.rankAt)).coerceIn(0f, 1f); LinearProgressIndicator(progress = { p }, modifier = Modifier.fillMaxWidth()); Text("${t.beaconsTotal} beacons logged · next rank at ${t.nextRankAt}", color = Slate, style = MaterialTheme.typography.bodySmall) }
        KeyValue("Today", "${km(t.distanceTodayKm)} · ${t.stopsToday} stops")
        KeyValue("This trip", "${km(t.distanceTripKm)} · ${t.stopsTrip} stops · moving ${durText(t.movingTripSecs)} · stopped ${durText(t.stoppedTripSecs)}")
        KeyValue("All time", "${km(t.distanceAllKm)} · ${t.stops} stops · longest leg ${km(t.longestLegKm)}")
        if (t.longestStayPlace.isNotEmpty()) KeyValue("Longest stay", "${t.longestStayPlace} · ${durText(t.longestStaySecs)}")
        if (t.dwellSecs > 0) KeyValue("Here for", durText(t.dwellSecs))
        if (!t.atHome && t.awayText.isNotEmpty()) KeyValue("From home", t.awayText)
        KeyValue("Beacons", "${t.beaconsTotal} logged · ${t.locatedNow} placed now · ${t.fitted} fitted by samples")
        if (t.bestAccuracy > 0) KeyValue("Best fix", "±${t.bestAccuracy.toInt()} m")
    }
}

@Composable
private fun TripDetailCards(t: Trip) {
    InfoCard("Places visited") {
        if (t.cities.isEmpty() && t.regions.isEmpty()) Text("Nothing yet.", color = Slate)
        if (t.cities.isNotEmpty()) KeyValue("Cities (${t.cities.size})", t.cities.takeLast(8).joinToString(", "))
        if (t.regions.isNotEmpty()) KeyValue("Regions (${t.regions.size})", t.regions.joinToString(", "))
        if (t.countries.isNotEmpty()) KeyValue("Countries (${t.countries.size})", t.countries.joinToString(", "))
    }
    InfoCard("Rank ladder") {
        for (r in t.rankLadder) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { Text(r.name, fontWeight = if (r.name == t.rank) FontWeight.Bold else FontWeight.Normal, color = if (t.beaconsTotal >= r.at) MaterialTheme.colorScheme.onSurface else Slate); Text("${r.at}", color = Slate) }
    }
    InfoCard("Milestones · ${t.achievementsUnlocked}/${t.achievementsTotal}") {
        for (a in t.achievements.sortedByDescending { it.unlocked }) Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(a.icon.ifEmpty { "•" }); Column(Modifier.weight(1f)) { Text(a.title, fontWeight = FontWeight.Bold, color = if (a.unlocked) MaterialTheme.colorScheme.onSurface else Slate); Text(a.desc + (if (a.unlocked && a.at.isNotEmpty()) " · ${a.at.take(10)}" else ""), color = Slate, style = MaterialTheme.typography.bodySmall) }
            if (a.unlocked) Chip("✓", Green)
        }
    }
}
