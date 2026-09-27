package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import kotlinx.coroutines.delay
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.help.HelpKind
import org.sworrl.beaconfix.help.HelpMeta
import org.sworrl.beaconfix.help.HelpSnapshot
import org.sworrl.beaconfix.help.Origin
import org.sworrl.beaconfix.help.oneLine
import org.sworrl.beaconfix.share.ShareText
import org.sworrl.beaconfix.ui.Emergency
import org.sworrl.beaconfix.ui.Intents
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.durText
import org.sworrl.beaconfix.ui.help.HelpPlaceRow
import org.sworrl.beaconfix.ui.help.distanceText
import org.sworrl.beaconfix.ui.metres
import org.sworrl.beaconfix.ui.theme.Orange
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.HelpRowModel
import org.sworrl.beaconfix.ui.vm.HelpViewModel
import java.util.Locale

/**
 * "Nearest help": the emergency call first, then what a dispatcher asks (where exactly), then the children's ER, a
 * closer lower-confidence one, the nearest general ER (never hidden behind the pediatric one), urgent care ("Not an
 * ER"), police and fire, Poison Control (US), pharmacy / vet under "More", and where the data came from.
 * [focus] = "peds" scrolls the children's ER card to the top.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HelpScreen(focus: String?, onBack: () -> Unit, onMap: () -> Unit, vm: HelpViewModel = hiltViewModel()) {
    val snap by vm.snapshot.collectAsState()
    val meta by vm.meta.collectAsState()
    val ui by vm.ui.collectAsState()
    val refreshing by vm.refreshing.collectAsState()
    val list = rememberLazyListState()
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(Unit) { vm.open() }
    LaunchedEffect(Unit) { while (true) { delay(30_000); now = System.currentTimeMillis() } }
    var focused by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(focus) { if (focus == "peds" && !focused) { focused = true; list.scrollToItem(PEDS_INDEX) } }
    var showMore by rememberSaveable { mutableStateOf(false) }
    val number = ui.number.ifBlank { Emergency.number(snap.countryCode.ifBlank { null }) }
    val guessed = !snap.pediatricSupported && snap.source.startsWith("desktop:")

    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text(stringResource(R.string.help_back)) }
            Text(stringResource(R.string.help_title), style = MaterialTheme.typography.headlineSmall, modifier = Modifier.semantics { heading() })
        }
        PullToRefreshBox(isRefreshing = refreshing, onRefresh = { vm.refresh() }, modifier = Modifier.fillMaxSize()) {
            LazyColumn(Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(bottom = 24.dp)) {
                item("call") { CallBlock(number) }
                item("dispatcher") { DispatcherCard(snap, meta, now) }
                item("peds") {
                    Section(stringResource(R.string.help_peds_title)) {
                        val peds = ui.peds
                        if (peds != null) HelpPlaceRow(peds, snap.origin, onMap, guessed = guessed)
                        else Text(snap.pedsNote.ifBlank { stringResource(R.string.help_peds_none) }, style = MaterialTheme.typography.bodyLarge)
                        ui.closer?.let { c ->
                            HorizontalDivider(Modifier.padding(vertical = 4.dp))
                            Text(stringResource(R.string.help_closer), style = MaterialTheme.typography.labelLarge, color = Slate)
                            HelpPlaceRow(c, snap.origin, onMap, guessed = guessed)
                        }
                        if (peds != null && snap.pedsNote.isNotBlank()) Text(snap.pedsNote, color = Orange, style = MaterialTheme.typography.bodySmall)
                    }
                }
                item("er") {
                    Section(stringResource(R.string.help_er_title)) {
                        ui.er?.let { HelpPlaceRow(it, snap.origin, onMap) } ?: Text(stringResource(R.string.help_er_none), color = Slate)
                    }
                }
                if (ui.urgent.isNotEmpty()) item("urgent") { Rows(stringResource(R.string.help_urgent_title), ui.urgent, snap.origin, onMap) }
                if (ui.safety.isNotEmpty()) item("safety") { Rows(stringResource(R.string.help_safety_title), ui.safety, snap.origin, onMap) }
                ui.poisonControl?.let { pc -> item("poison") { PoisonCard(pc) } }
                if (ui.more.isNotEmpty()) item("more") {
                    Column(Modifier.fillMaxWidth()) {
                        TextButton(onClick = { showMore = !showMore }, modifier = Modifier.padding(horizontal = 8.dp)) {
                            Text(if (showMore) stringResource(R.string.help_less) else stringResource(R.string.help_more))
                        }
                        if (showMore) Rows(null, ui.more, snap.origin, onMap)
                    }
                }
                item("footer") { Footer(snap, meta, refreshing, now) { vm.refresh() } }
            }
        }
    }
}

/** Index of the children's ER card in the list (call, dispatcher, peds). */
private const val PEDS_INDEX = 2

@Composable
private fun CallBlock(number: String) {
    val ctx = LocalContext.current
    val label = stringResource(R.string.help_call_number, number)
    Card(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp), colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Button(
                onClick = { Intents.dial(ctx, number) },
                modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp).semantics { contentDescription = label },
                colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error, contentColor = MaterialTheme.colorScheme.onError),
            ) { Text(label, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold) }
            Text(stringResource(R.string.help_call_sub), color = MaterialTheme.colorScheme.onErrorContainer, style = MaterialTheme.typography.bodyMedium)
        }
    }
}

@Composable
private fun DispatcherCard(s: HelpSnapshot, meta: HelpMeta, now: Long) {
    val ctx = LocalContext.current
    val clip = LocalClipboardManager.current
    var copied by remember { mutableStateOf(false) }
    Section(stringResource(R.string.help_dispatcher_title)) {
        if (s.origin == Origin.NONE) { Text(stringResource(R.string.help_dispatcher_none)); return@Section }
        Text(String.format(Locale.US, "%.5f, %.5f", s.originLat, s.originLon), fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
        val age = (s.originAgeMs + (now - meta.lastRunAt).coerceAtLeast(0)).let { if (it < 60_000) "now" else durText(it / 1000) + " ago" }
        Text(
            if (meta.originAccM > 0) stringResource(R.string.help_dispatcher_acc_age, metres(meta.originAccM), age) else stringResource(R.string.help_dispatcher_age, age),
            color = Slate, style = MaterialTheme.typography.bodySmall,
        )
        if (s.origin == Origin.RV) Text(stringResource(R.string.help_dispatcher_rv), color = Orange, style = MaterialTheme.typography.bodySmall)
        val a = s.address
        if (a != null) {
            if (a.line.isNotBlank()) Text(a.line, style = MaterialTheme.typography.bodyLarge)
            val place = listOf(a.locality, a.county, a.state).filter { it.isNotBlank() }.distinct().joinToString(", ")
            val saved = if (a.fromCache) " " + stringResource(R.string.help_saved) else ""
            if (place.isNotBlank() || saved.isNotBlank()) Text(place + saved, style = MaterialTheme.typography.bodyMedium)
        } else Text(stringResource(R.string.help_dispatcher_no_address), color = Slate, style = MaterialTheme.typography.bodySmall)
        // What is sent says what the screen says: the RV's position is labelled as such, an old fix carries its time,
        // and a saved address (resolved nearby, not for this exact spot) is marked
        val subject = if (s.origin == Origin.RV) ShareText.RV_POSITION else stringResource(R.string.help_share_subject)
        fun text(at: Long) = ShareText.location(
            s.originLat, s.originLon, meta.originAccM.takeIf { it > 0 }, dispatcherAddress(s.address),
            label = if (s.origin == Origin.RV) ShareText.RV_POSITION else null,
            fixAtMs = if (s.originAgeMs > 0 && meta.lastRunAt > 0) meta.lastRunAt - s.originAgeMs else 0L, nowMs = at,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { clip.setText(AnnotatedString(text(System.currentTimeMillis()))); copied = true }, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(if (copied) stringResource(R.string.help_copied) else stringResource(R.string.help_copy))
            }
            OutlinedButton(onClick = { Intents.shareText(ctx, text(System.currentTimeMillis()), subject) }, modifier = Modifier.heightIn(min = 48.dp)) { Text(stringResource(R.string.help_share)) }
        }
    }
}

/** The address line for the share text: a saved one is marked, since it was resolved near here, not for this spot. */
internal fun dispatcherAddress(a: org.sworrl.beaconfix.help.AddressLine?): String? {
    if (a == null) return null
    val line = a.oneLine().takeIf { it.isNotBlank() } ?: return null
    return if (a.fromCache) "Saved address nearby (check it): $line" else line
}

@Composable
private fun PoisonCard(number: String) {
    val ctx = LocalContext.current
    val cd = stringResource(R.string.help_cd_call, stringResource(R.string.help_poison_title))
    Section(stringResource(R.string.help_poison_title)) {
        Text(stringResource(R.string.help_poison_body), style = MaterialTheme.typography.bodyMedium)
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(number, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            Button(onClick = { Intents.dial(ctx, number) }, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = cd }) { Text(stringResource(R.string.help_call)) }
        }
    }
}

@Composable
private fun Rows(title: String?, rows: List<HelpRowModel>, origin: String, onMap: () -> Unit) {
    val body: @Composable () -> Unit = {
        rows.forEachIndexed { i, r ->
            if (i > 0) HorizontalDivider(Modifier.padding(vertical = 4.dp))
            HelpPlaceRow(r, origin, onMap)
        }
    }
    if (title != null) Section(title) { body() } else Card(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp)) { Column(Modifier.padding(16.dp)) { body() } }
}

@Composable
private fun Footer(s: HelpSnapshot, meta: HelpMeta, refreshing: Boolean, now: Long, onRefresh: () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        val status = when {
            refreshing -> stringResource(R.string.help_refreshing)
            s.fetchedAt <= 0 -> stringResource(R.string.help_source_none)
            s.source.startsWith("desktop:") && meta.dataDistM >= 0 -> stringResource(R.string.help_source_desktop_far, agoAt(s.fetchedAt, now), distanceText(meta.dataDistM))
            s.source.startsWith("desktop:") -> stringResource(R.string.help_source_desktop, agoAt(s.fetchedAt, now))
            else -> stringResource(R.string.help_source_phone, agoAt(s.fetchedAt, now))
        }
        Text(status, style = MaterialTheme.typography.bodySmall, modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite })
        if (s.stale) Text(stringResource(R.string.help_stale), color = Orange, style = MaterialTheme.typography.bodySmall)
        if (s.pedsNote.isNotBlank() && s.first(HelpKind.PEDS_ER) == null && !s.pedsNote.startsWith("No pediatric ER")) Text(s.pedsNote, color = Slate, style = MaterialTheme.typography.bodySmall)
        if (meta.phoneNote.isNotBlank() && !s.source.startsWith("desktop:")) Text(meta.phoneNote, color = Slate, style = MaterialTheme.typography.bodySmall)
        Text(stringResource(R.string.help_osm_note), color = Slate, style = MaterialTheme.typography.bodySmall)
        if (s.lastError.isNotBlank()) Text(stringResource(R.string.help_error, s.lastError), color = Orange, style = MaterialTheme.typography.bodySmall)
        TextButton(onClick = onRefresh, enabled = !refreshing) { Text(stringResource(R.string.help_refresh)) }
    }
}

/** "just now" / "5 min ago" relative to [now] (the screen's ticking clock). */
private fun agoAt(ms: Long, now: Long): String {
    val s = (now - ms) / 1000
    return if (s < 60) "just now" else ago(ms)
}

@Composable
private fun Section(title: String, content: @Composable () -> Unit) {
    Card(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp)) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() })
            content()
        }
    }
}
