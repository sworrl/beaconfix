// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings.ui

import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.PlateEventEntity
import org.sworrl.beaconfix.data.db.PlateEventMediaEntity
import org.sworrl.beaconfix.sightings.Hibf
import org.sworrl.beaconfix.sightings.HibfWatcher
import org.sworrl.beaconfix.sightings.PlateEventRepository
import org.sworrl.beaconfix.sightings.PlateEvents
import org.sworrl.beaconfix.sightings.Sightings
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.KeyValue
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.compass
import org.sworrl.beaconfix.ui.map.MapFocus
import org.sworrl.beaconfix.ui.metres
import org.sworrl.beaconfix.ui.theme.Gold
import org.sworrl.beaconfix.ui.theme.Red
import org.sworrl.beaconfix.ui.theme.Slate
import java.io.File
import javax.inject.Inject

@HiltViewModel
class SightingsViewModel @Inject constructor(
    @ApplicationContext private val ctx: Context,
    private val db: AppDatabase,
    private val repo: PlateEventRepository,
    private val prefs: Prefs,
) : ViewModel() {
    val events: StateFlow<List<PlateEventEntity>?> = db.plateEvents().recent(2000).stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), null)
    val watch: StateFlow<Hibf.WatchState> = prefs.hibfWatch.map { Hibf.WatchState.decode(it) }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), Hibf.WatchState())
    val plates: StateFlow<String> = prefs.registeredPlates.map { s ->
        runCatching { Hibf.plateList(Json.parseToJsonElement(s) as? kotlinx.serialization.json.JsonObject).joinToString(", ") { it.first } }.getOrDefault("")
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), "")
    var busy by mutableStateOf(false); private set
    var note by mutableStateOf(""); private set

    fun event(uid: String): Flow<PlateEventEntity?> = db.plateEvents().watch(uid)
    fun media(e: PlateEventEntity): Flow<List<PlateEventMediaEntity>> = db.plateEvents().mediaFor(e.uid, e.cameraId)

    fun refresh() = viewModelScope.launch {
        busy = true
        val r = runCatching { repo.syncAll() }.getOrDefault(emptyList())
        note = if (r.isEmpty()) "No linked PC to ask" else r.joinToString("; ") { if (it.error.isNotEmpty()) it.error else "pushed ${it.pushed}, images ${it.uploaded}, received ${it.pulled}" }
        busy = false
    }

    val webcamStills: StateFlow<Boolean> = prefs.webcamStills.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), false)
    fun setWebcamStills(v: Boolean) = viewModelScope.launch { prefs.setWebcamStills(v) }

    fun checkPlatesNow() { HibfWatcher.checkNow(ctx); note = "Plate check queued (it waits for a network and the request floors)" }

    /** The raw record and every column from the desktop, when this phone only has the feed's copy. */
    fun fetchFull(uid: String) = viewModelScope.launch { runCatching { repo.fetchFull(uid) } }

    /** [m] decoded for display, at most [maxSide] px (local frame, or the desktop's `?as=display`, cached). */
    suspend fun image(m: PlateEventMediaEntity, maxSide: Int): ImageBitmap? = withContext(Dispatchers.IO) {
        val f = runCatching { repo.displayFile(m) }.getOrNull() ?: return@withContext null
        decode(f, maxSide)
    }

    suspend fun thumbnail(e: PlateEventEntity): ImageBitmap? = withContext(Dispatchers.IO) {
        val m = db.plateEvents().mediaForNow(e.uid, e.cameraId).firstOrNull() ?: return@withContext null
        image(m, 240)
    }

    private fun decode(f: File, maxSide: Int): ImageBitmap? = runCatching {
        val o = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(f.absolutePath, o)
        var s = 1
        while (maxOf(o.outWidth, o.outHeight) / (s * 2) >= maxSide) s *= 2
        BitmapFactory.decodeFile(f.absolutePath, BitmapFactory.Options().apply { inSampleSize = s })?.asImageBitmap()
    }.getOrNull()
}

private fun kindLabel(e: PlateEventEntity): String = when {
    e.kind == PlateEvents.PLATE_SEARCH -> "Plate search"
    e.cameraType == "alpr" || e.cameraType.isNullOrEmpty() -> "ALPR camera pass"
    else -> PlateEvents.typeLabel(e.cameraType).replaceFirstChar { it.uppercase() } + " pass"
}

private fun who(e: PlateEventEntity): String = when (e.kind) {
    PlateEvents.PLATE_SEARCH -> e.agency ?: "Unknown agency"
    else -> listOfNotNull(e.operator?.takeIf { it.isNotBlank() }, e.model?.takeIf { it.isNotBlank() }).joinToString(" · ").ifEmpty { e.cameraId ?: "camera" }
}

private fun openUrl(ctx: Context, url: String) = runCatching { ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }

/** More → Sightings: every plate event, newest first, from Room (works offline). */
@Composable
fun SightingsScreen(onOpen: (String) -> Unit, onBack: () -> Unit, vm: SightingsViewModel = hiltViewModel()) {
    val list by vm.events.collectAsState()
    val watch by vm.watch.collectAsState()
    val plates by vm.plates.collectAsState()
    LazyColumn(Modifier.fillMaxSize(), contentPadding = androidx.compose.foundation.layout.PaddingValues(vertical = 8.dp)) {
        item {
            Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                TextButton(onClick = onBack) { Text("‹ Back") }
                Text("Sightings", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.weight(1f))
                TextButton(onClick = { vm.refresh() }, enabled = !vm.busy) { Text(if (vm.busy) "Syncing…" else "Sync") }
            }
        }
        item {
            InfoCard("What this is") {
                Text("Camera passes: your route came within 65 m of a known plate-reading (ALPR) camera; the confidence is the chance it read your " +
                    "plate, from its viewing cone and range and your fix accuracy. Other cameras (traffic webcams, Flock PTZ video, speed cameras, CCTV) " +
                    "are listed too; they do not read plates and never alert. Plate searches: an agency searched your plate in " +
                    "Flock, found in audit logs released through public-records requests (indexed by HaveIBeenFlocked; months to years late; a " +
                    "search is not a stop). Images are your own dash-cam frames and public photos of the camera — nobody publishes Flock's own pictures.",
                    color = Slate, style = MaterialTheme.typography.bodySmall)
                Text("Plates watched: ${plates.ifEmpty { "none yet (registered on your PC)" }}", style = MaterialTheme.typography.bodySmall)
                val next = if (watch.nextCheck > System.currentTimeMillis()) " · next ${org.sworrl.beaconfix.sightings.Sightings.date(watch.nextCheck)}" else ""
                Text("Plate check: ${if (watch.lastCheck > 0) ago(watch.lastCheck) else "not yet"} · ${watch.mode.ifEmpty { "idle" }}${watch.lastStatus.let { if (it.isNotEmpty()) " · $it" else "" }}$next",
                    color = Slate, style = MaterialTheme.typography.bodySmall)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) { OutlinedButton(onClick = { vm.checkPlatesNow() }) { Text("Check my plates now") } }
                val stills by vm.webcamStills.collectAsState()
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("Keep public webcam stills", style = MaterialTheme.typography.bodyMedium)
                        Text("Off by default: some providers (WV511) forbid storing their images. On, a still is kept on this phone only and never sent.",
                            color = Slate, style = MaterialTheme.typography.bodySmall)
                    }
                    androidx.compose.material3.Switch(stills, { vm.setWebcamStills(it) })
                }
                if (vm.note.isNotEmpty()) Text(vm.note, color = Slate, style = MaterialTheme.typography.bodySmall)
            }
        }
        val l = list
        if (l != null && l.isEmpty()) item {
            org.sworrl.beaconfix.ui.EmptyState("📷", "No sightings yet", "Passes appear while the collector or the dash cam runs near known cameras, and when your PC syncs its own.")
        }
        items(l.orEmpty(), key = { it.uid }) { e -> SightingRow(e, vm) { onOpen(e.uid) } }
    }
}

@Composable
private fun SightingRow(e: PlateEventEntity, vm: SightingsViewModel, onClick: () -> Unit) {
    var thumb by remember(e.uid) { mutableStateOf<ImageBitmap?>(null) }
    LaunchedEffect(e.uid) { thumb = vm.thumbnail(e) }
    Card(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp).clickable(onClick = onClick)) {
        Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(64.dp), contentAlignment = Alignment.Center) {
                val t = thumb
                if (t != null) Image(t, contentDescription = "Image of this sighting", modifier = Modifier.size(64.dp), contentScale = ContentScale.Crop)
                else Text(if (e.kind == PlateEvents.PLATE_SEARCH) "🔍" else if (e.cameraType == "alpr") "📷" else "🎥", style = MaterialTheme.typography.headlineSmall)
            }
            Column(Modifier.weight(1f).padding(start = 12.dp)) {
                Text(kindLabel(e), fontWeight = FontWeight.Bold, color = if (e.kind == PlateEvents.PLATE_SEARCH) Red else if (e.cameraType == "alpr") Gold else Slate)
                Text(who(e), style = MaterialTheme.typography.bodyMedium)
                val bits = listOfNotNull(
                    e.time.replace('T', ' ').take(16),
                    e.plate?.takeIf { it.isNotBlank() },
                    e.distanceM?.let { metres(it) },
                    if (e.kind == PlateEvents.CAMERA_PASS) Sightings.facingText(e.facing) else null,
                    e.confidence?.let { "$it %" },
                )
                Text(bits.joinToString(" · "), color = Slate, style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}

/** One event: every field, the metrics, the images, View source, Show on map and the raw record (beaconfix://sighting/<uid>). */
@Composable
fun SightingDetailScreen(uid: String, onBack: () -> Unit, onMap: () -> Unit, vm: SightingsViewModel = hiltViewModel()) {
    val ctx = LocalContext.current
    val ev by remember(uid) { vm.event(uid) }.collectAsState(initial = null)
    LaunchedEffect(uid) { vm.fetchFull(uid) }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("‹ Back") }
            Text("Sighting", style = MaterialTheme.typography.headlineSmall)
        }
        val e = ev
        if (e == null) { Text("Not on this phone (yet). Sync with your PC to fetch it.", color = Slate, modifier = Modifier.padding(16.dp)); return@Column }
        val media by remember(e.uid, e.cameraId) { vm.media(e) }.collectAsState(initial = emptyList())
        InfoCard(kindLabel(e)) {
            Text(who(e), fontWeight = FontWeight.Bold)
            e.details?.let { Text(it) }
            Sightings.alertText(e)?.let { Text(it.second, color = Slate, style = MaterialTheme.typography.bodySmall) }
            if (e.kind == PlateEvents.CAMERA_PASS && e.cameraType != "alpr" && !e.cameraType.isNullOrEmpty())
                Text("This is a ${PlateEvents.typeLabel(e.cameraType)}: it does not read plates.", color = Slate, style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.horizontalScroll(rememberScrollState())) {
                e.sourceUrl?.takeIf { it.startsWith("http") }?.let { url -> OutlinedButton(onClick = { openUrl(ctx, url) }) { Text("View source") } }
                val lat = e.lat ?: e.cameraLat; val lon = e.lon ?: e.cameraLon
                if (lat != null && lon != null) OutlinedButton(onClick = { MapFocus.target.value = MapFocus.Target(e.cameraLat ?: lat, e.cameraLon ?: lon, 18.0, label = who(e)); onMap() }) { Text("Show on map") }
                e.cameraId?.takeIf { it.isNotBlank() }?.let { camId ->
                    OutlinedButton(onClick = {
                        val cLat = e.cameraLat ?: e.lat ?: 0.0
                        val cLon = e.cameraLon ?: e.lon ?: 0.0
                        MapFocus.target.value = MapFocus.Target(cLat, cLon, 18.0, label = who(e), inspectCamId = camId)
                        onMap()
                    }) { Text("Inspect unseen") }
                }
            }
        }
        if (media.isNotEmpty()) InfoCard("Images") {
            for (m in media) MediaView(m, vm)
        }
        InfoCard("Record") {
            for ((k, v) in fields(e)) KeyValue(k, v)
        }
        PlateEvents.metricsObject(e.metrics)?.takeIf { it.isNotEmpty() }?.let { mo ->
            InfoCard("Metrics") { for ((k, v) in mo) KeyValue(k, PlateEvents.show(v)) }
        }
        e.raw?.let { raw ->
            var open by rememberSaveable { mutableStateOf(false) }
            InfoCard("Raw record") {
                TextButton(onClick = { open = !open }) { Text(if (open) "Hide" else "Show the source record as received") }
                if (open) SelectionContainer { Text(pretty(raw), fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall) }
            }
        }
    }
}

@Composable
private fun MediaView(m: PlateEventMediaEntity, vm: SightingsViewModel) {
    var img by remember(m.uid) { mutableStateOf<ImageBitmap?>(null) }
    LaunchedEffect(m.uid) { img = vm.image(m, 1600) }
    Column(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        val i = img
        if (i != null) Image(i, contentDescription = m.kind, modifier = Modifier.fillMaxWidth().heightIn(max = 360.dp), contentScale = ContentScale.Fit)
        else Text("Image not on this phone (needs your PC on the network)", color = Slate, style = MaterialTheme.typography.bodySmall)
        val label = when (m.kind) { "dashcam" -> "Your dash cam"; "webcam" -> "Public webcam still (on this phone only)"; "camera_photo" -> "Photo of the camera"; else -> m.kind }
        Text(listOfNotNull(label, m.capturedAt?.replace('T', ' ')?.take(19), m.width?.let { "${it}×${m.height}" }, m.attribution, m.license).joinToString(" · "),
            color = Slate, style = MaterialTheme.typography.bodySmall)
    }
}

private fun fields(e: PlateEventEntity): List<Pair<String, String>> {
    fun d(v: Double?, unit: String = "", n: Int = 1) = v?.let { "%.${n}f".format(java.util.Locale.US, it) + unit }
    return listOf(
        "uid" to e.uid, "kind" to e.kind, "plate" to e.plate, "time" to e.time,
        "position" to if (e.lat != null && e.lon != null) "%.6f, %.6f".format(java.util.Locale.US, e.lat, e.lon) else null,
        "accuracy" to d(e.acc, " m"), "camera" to e.cameraId,
        "camera position" to if (e.cameraLat != null && e.cameraLon != null) "%.6f, %.6f".format(java.util.Locale.US, e.cameraLat, e.cameraLon) else null,
        "distance" to e.distanceM?.let { metres(it) }, "speed" to d(e.speedKmh, " km/h"),
        "heading" to e.headingDeg?.let { "${d(it, "°", 0)} ${compass(it)}" }, "camera → you" to e.approachBearingDeg?.let { "${d(it, "°", 0)} ${compass(it)}" },
        "camera faces" to e.cameraDirDeg?.let { "${d(it, "°", 0)} ${compass(it)}" }, "facing" to Sightings.facingText(e.facing).takeIf { e.kind == PlateEvents.CAMERA_PASS },
        "operator" to e.operator, "agency" to e.agency, "model" to e.model, "camera type" to e.cameraType, "source" to e.source, "source name" to e.sourceName,
        "source url" to e.sourceUrl, "confidence" to e.confidence?.let { "$it %" }, "leaky agency" to if (e.leaky == 1) "yes — publishes its audit logs" else null,
        "recorded by" to e.device?.ifEmpty { "the PC" }, "created" to e.createdAt, "updated" to e.updatedAt, "feed seq" to e.seq.takeIf { it > 0 }?.toString(),
        "waiting to send" to listOfNotNull("the PC".takeIf { e.dirty }, "the hub".takeIf { e.hubDirty }).joinToString(" and ").ifEmpty { null },
    ).mapNotNull { (k, v) -> v?.takeIf { it.isNotBlank() }?.let { k to it } }
}

private val prettyJson = Json { prettyPrint = true }
private fun pretty(s: String): String = runCatching { prettyJson.encodeToString(JsonElement.serializer(), Json.parseToJsonElement(s)) }.getOrDefault(s)
