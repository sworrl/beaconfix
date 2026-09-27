package org.sworrl.beaconfix.widget

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.drawable.BitmapDrawable
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import androidx.glance.appwidget.updateAll
import androidx.hilt.work.HiltWorker
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.ExistingWorkPolicy
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import dagger.assisted.Assisted
import dagger.assisted.AssistedInject
import dagger.hilt.EntryPoint
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.osmdroid.tileprovider.MapTileProviderBasic
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.util.MapTileIndex
import org.sworrl.beaconfix.collector.CollectorStatus
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.identity.IdentityStore
import org.sworrl.beaconfix.sync.SyncScheduler
import java.io.File
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.PI
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tan

private val Context.widgetStore: DataStore<Preferences> by preferencesDataStore("beaconfix_widgets")
private val KEY = stringPreferencesKey("state")
private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }

/** Hilt entry point for code that Hilt does not inject (Glance widgets and their action callbacks). */
@EntryPoint @InstallIn(SingletonComponent::class)
interface WidgetEntryPoint { fun updater(): WidgetUpdater; fun syncScheduler(): SyncScheduler; fun prefs(): Prefs }

/**
 * Builds the [WidgetState] from the database, prefs, the collector's last scan and the paired desktop, renders the map
 * snapshot, and pushes every widget. Called every 15 min (WorkManager), after each scan / sync / collector change (debounced),
 * and from the widgets' own refresh buttons.
 */
@Singleton
class WidgetUpdater @Inject constructor(
    @ApplicationContext private val ctx: Context, private val db: AppDatabase, private val prefs: Prefs,
    private val status: CollectorStatus, private val desktops: DesktopStore, private val identity: IdentityStore,
    private val notifier: StatusNotifier,
    private val ranging: dagger.Lazy<org.sworrl.beaconfix.ranging.RangingRepository>,
) {
    /** last few human lines for the notification ("recent") */
    @Volatile var recent: List<String> = emptyList()
    fun note(line: String) { recent = (listOf(line) + recent).take(6) }
    fun collectorText(): String { val s = status.state.value; return when { !s.running -> "collector paused"; s.throttled -> "scanning (throttled by Android: 4 scans / 2 min)"; s.survey -> "surveying continuously"; else -> "scanning" } }
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var pending: Job? = null

    val state: Flow<WidgetState> = ctx.widgetStore.data.map { p -> p[KEY]?.let { runCatching { json.decodeFromString<WidgetState>(it) }.getOrNull() } ?: WidgetState() }
    suspend fun current(): WidgetState = state.first()

    /** Something changed (a scan, a sync, the collector flipped): refresh soon, coalescing bursts. */
    fun touch(@Suppress("UNUSED_PARAMETER") reason: String) {
        pending?.cancel()
        pending = scope.launch { delay(2500); runCatching { refresh() } }
    }

    fun ensurePeriodic() {
        val req = PeriodicWorkRequestBuilder<WidgetWorker>(15, TimeUnit.MINUTES).build()
        WorkManager.getInstance(ctx).enqueueUniquePeriodicWork("beaconfix-widgets", ExistingPeriodicWorkPolicy.KEEP, req)
    }
    fun refreshNow() { WorkManager.getInstance(ctx).enqueueUniqueWork("beaconfix-widgets-now", ExistingWorkPolicy.REPLACE, OneTimeWorkRequestBuilder<WidgetWorker>().build()) }

    suspend fun refresh(renderMap: Boolean = true): WidgetState {
        val old = current()
        val rec = identity.currentNow()
        val phoneFix = db.fixes().latestPhone()
        val paired = desktops.paired().firstOrNull()
        // the desktop's view of things, when it answers quickly
        var deskPlace = ""; var deskLat = 0.0; var deskLon = 0.0; var deskAcc = -1.0; var deskTime = 0L; var atHome: Boolean? = null; var away = ""
        var nearestDevice = ""
        if (paired != null) withTimeoutOrNull(4000) {
            runCatching { desktops.api(paired).location(desktops.auth(paired)!!) }.getOrNull()?.body()?.let { l ->
                if (l.valid) { deskPlace = l.place; deskLat = l.lat; deskLon = l.lon; deskAcc = l.accuracy; deskTime = System.currentTimeMillis() - ((l.ageS ?: 0.0) * 1000).toLong()
                    l.home?.let { h -> atHome = h.atHome; away = if (h.atHome) "at home" else h.awayText.ifEmpty { String.format(java.util.Locale.US, "%.1f km from home", h.awayKm) } } }
            }
        }
        // choose the position to show: a fresh phone fix beats the desktop, the desktop beats a stale phone fix
        if (paired != null) withTimeoutOrNull(3000) {
            val auth = desktops.auth(paired)!!
            val devs = runCatching { desktops.api(paired).devicesPositions(auth) }.getOrNull()?.takeIf { it.isSuccessful }?.body()?.devices ?: emptyList()
            val ref = phoneFix?.takeIf { System.currentTimeMillis() - it.time < 20 * 60_000L }?.let { it.lat to it.lon } ?: (if (deskTime > 0) deskLat to deskLon else null)
            val cands = (devs.filter { it.lat != 0.0 || it.lon != 0.0 && it.kind != "android" } + (if (deskTime > 0) listOf(org.sworrl.beaconfix.data.api.LinkedDevice(paired.name, "desktop", "", "", deskLat, deskLon, deskAcc)) else emptyList()))
            if (ref != null) cands.filter { it.device != identity.deviceName }.map { d -> d to distanceM(ref.first, ref.second, d.lat, d.lon) }.minByOrNull { it.second }?.let { (d, dist) ->
                val brg = Math.toDegrees(Math.atan2(Math.sin(Math.toRadians(d.lon - ref.second)) * Math.cos(Math.toRadians(d.lat)), Math.cos(Math.toRadians(ref.first)) * Math.sin(Math.toRadians(d.lat)) - Math.sin(Math.toRadians(ref.first)) * Math.cos(Math.toRadians(d.lat)) * Math.cos(Math.toRadians(d.lon - ref.second))))
                val dir = listOf("N","NE","E","SE","S","SW","W","NW")[((((brg % 360) + 360) % 360) / 45).toInt() % 8]
                nearestDevice = "${d.identityName.ifEmpty { d.device }} " + (if (dist < 1000) "${dist.toInt()} m" else String.format(java.util.Locale.US, "%.1f km", dist / 1000)) + " $dir"
            }
        }
        val phoneFresh = phoneFix != null && System.currentTimeMillis() - phoneFix.time < 20 * 60_000L
        val usePhone = phoneFix != null && (phoneFresh || deskTime == 0L)
        val lat = if (usePhone) phoneFix!!.lat else deskLat; val lon = if (usePhone) phoneFix!!.lon else deskLon
        val acc = if (usePhone) phoneFix!!.acc else deskAcc
        val source = if (usePhone) (if (phoneFix!!.source == "phone-wifi") "wifi" else "gps") else if (deskTime > 0) "desktop" else ""
        val fixTime = if (usePhone) phoneFix!!.time else deskTime
        val place = if (usePhone && phoneFix!!.place.isNotEmpty()) phoneFix.place else deskPlace.ifEmpty { if (lat != 0.0 || lon != 0.0) String.format(java.util.Locale.US, "%.5f, %.5f", lat, lon) else "" }
        // beacons: the collector's last scan (in this process) or, failing that, the most recently seen rows
        val scan = status.state.value.scan
        val apsAll = db.aps().all().first()
        val byBssid = apsAll.associateBy { it.bssid }
        val heard = if (scan.isNotEmpty()) scan.map { s -> TopBeacon(s.ssid, s.bssid, s.dbm, byBssid[s.bssid]?.security?.ifEmpty { org.sworrl.beaconfix.collector.ObservationRecorder.securityOf(s.capabilities) } ?: org.sworrl.beaconfix.collector.ObservationRecorder.securityOf(s.capabilities), byBssid[s.bssid]?.home ?: false) }
                    else apsAll.filter { System.currentTimeMillis() - it.lastSeen < 15 * 60_000L }.map { TopBeacon(it.ssid, it.bssid, -100, it.security, it.home) }
        var open = 0; var wep = 0; var wpa1 = 0; var tkip = 0; var wpa2 = 0; var wpa3 = 0; var other = 0
        for (b in heard) when (b.security) { "open" -> open++; "wep" -> wep++; "wpa1" -> wpa1++; "wpa2-tkip" -> tkip++; "wpa2" -> wpa2++; "wpa3", "wpa3-eap192" -> wpa3++; else -> other++ }
        val worst = when { open + wep + wpa1 + tkip > 0 -> "critical"; wpa2 > 0 -> "weak"; other > 0 -> "ok"; wpa3 > 0 -> "strong"; else -> "" }
        val top = heard.sortedByDescending { it.dbm }.take(3)
        val unsynced = db.observations().unsyncedCount().first()
        val collectorOn = prefs.collectorOn.first()
        val positioned = db.aps().positionedCount().first()
        var st = WidgetState(
            updated = System.currentTimeMillis(), identityName = rec?.name ?: "", identityId = rec?.id ?: "",
            place = place, lat = lat, lon = lon, acc = acc, source = source, fixTime = fixTime, atHome = atHome, awayText = away,
            inRange = heard.size, scanTime = if (scan.isNotEmpty()) status.state.value.lastScanAt else 0,
            open = open, wep = wep, wpa1 = wpa1, tkip = tkip, wpa2 = wpa2, wpa3 = wpa3, other = other, worst = worst, top = top,
            desktopName = paired?.name ?: "", desktopPaired = paired != null, lastSync = paired?.lastSync ?: prefs.lastSyncAt.first(), unsynced = unsynced, collectorOn = collectorOn,
            apsKnown = apsAll.size, apsPositioned = positioned, mapPath = old.mapPath, mapTime = old.mapTime, nearestDevice = nearestDevice,
            rangeLine = runCatching { ranging.get().line() }.getOrDefault(""),
        )
        // a measured range beats a difference of two fixes for the widget's nearest-device line
        if (st.rangeLine.isNotEmpty()) st = st.copy(nearestDevice = st.rangeLine.substringBefore(" ("))
        if (renderMap && (lat != 0.0 || lon != 0.0)) {
            val moved = old.mapTime == 0L || distanceM(old.lat, old.lon, lat, lon) > 60 || System.currentTimeMillis() - old.mapTime > 15 * 60_000L
            if (moved) runCatching { renderMap(lat, lon, acc) }.getOrNull()?.let { st = st.copy(mapPath = it, mapTime = System.currentTimeMillis()) }
        }
        ctx.widgetStore.edit { it[KEY] = json.encodeToString(st) }
        runCatching { LocationWidget().updateAll(ctx); BeaconsWidget().updateAll(ctx); SyncWidget().updateAll(ctx); MapWidget().updateAll(ctx) }
        runCatching { notifier.post(st, status.state.value.presence, collectorText(), recent) }
        return st
    }

    // ── static map snapshot: OSM tiles through osmdroid's cache, beacons and the fix drawn on top ──
    private suspend fun renderMap(lat: Double, lon: Double, acc: Double, w: Int = 640, h: Int = 480, z: Int = 15): String? = withContext(Dispatchers.Main) {
        val provider = MapTileProviderBasic(ctx, TileSourceFactory.MAPNIK)
        try {
            val n = 2.0.pow(z)
            val cx = (lon + 180.0) / 360.0 * n
            val cy = (1.0 - ln(tan(Math.toRadians(lat)) + 1 / cos(Math.toRadians(lat))) / PI) / 2.0 * n
            val px0 = cx * 256 - w / 2.0; val py0 = cy * 256 - h / 2.0
            val tx0 = floor(px0 / 256).toInt(); val ty0 = floor(py0 / 256).toInt()
            val tx1 = floor((px0 + w) / 256).toInt(); val ty1 = floor((py0 + h) / 256).toInt()
            val need = ArrayList<Long>(); for (x in tx0..tx1) for (y in ty0..ty1) need += MapTileIndex.getTileIndex(z, x, y)
            val got = HashMap<Long, android.graphics.drawable.Drawable>()
            var waited = 0
            while (got.size < need.size && waited < 10_000) {
                for (t in need) if (t !in got) provider.getMapTile(t)?.let { got[t] = it }
                if (got.size < need.size) { delay(400); waited += 400 }
            }
            if (got.isEmpty()) return@withContext null
            val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
            val c = Canvas(bmp); c.drawColor(Color.parseColor("#0B101A"))
            for (t in need) {
                val d = got[t] ?: continue
                val x = (MapTileIndex.getX(t) * 256 - px0).toInt(); val y = (MapTileIndex.getY(t) * 256 - py0).toInt()
                val b = (d as? BitmapDrawable)?.bitmap
                if (b != null) c.drawBitmap(b, null, android.graphics.Rect(x, y, x + 256, y + 256), null) else { d.setBounds(x, y, x + 256, y + 256); d.draw(c) }
            }
            // dark scrim so markers pop
            c.drawColor(Color.parseColor("#33000000"))
            val mpp = 156543.03 * cos(Math.toRadians(lat)) / n
            val p = Paint(Paint.ANTI_ALIAS_FLAG)
            fun toXY(la: Double, lo: Double): Pair<Float, Float> {
                val x = (lo + 180.0) / 360.0 * n * 256 - px0
                val y = (1.0 - ln(tan(Math.toRadians(la)) + 1 / cos(Math.toRadians(la))) / PI) / 2.0 * n * 256 - py0
                return x.toFloat() to y.toFloat()
            }
            for (a in db.aps().positionedNow()) {
                val (x, y) = toXY(a.lat!!, a.lon!!)
                if (x < -20 || y < -20 || x > w + 20 || y > h + 20) continue
                val col = when { a.home -> "#FF4FD8"; a.security == "open" || a.security == "wep" || a.security == "wpa1" || a.security == "wpa2-tkip" -> "#FF4D4D"; a.posSource == "observed" -> "#35D6FF"; a.posSource == "placed" -> "#FFD166"; else -> "#9FB0C8" }
                val r = ((a.acc ?: 50.0) / mpp).toFloat().coerceIn(4f, 120f)
                p.style = Paint.Style.FILL; p.color = Color.parseColor("#22" + col.drop(1)); c.drawCircle(x, y, r, p)
                p.style = Paint.Style.STROKE; p.strokeWidth = 1.5f; p.color = Color.parseColor(col); c.drawCircle(x, y, r, p)
                p.style = Paint.Style.FILL; c.drawCircle(x, y, 4f, p)
            }
            val (fx, fy) = toXY(lat, lon)
            if (acc > 0) { p.style = Paint.Style.FILL; p.color = Color.parseColor("#2235D6FF"); c.drawCircle(fx, fy, (acc / mpp).toFloat().coerceIn(6f, 200f), p) }
            p.style = Paint.Style.STROKE; p.strokeWidth = 3f; p.color = Color.WHITE; c.drawCircle(fx, fy, 11f, p)
            p.style = Paint.Style.FILL; p.color = Color.parseColor("#FF4D4D"); c.drawCircle(fx, fy, 5f, p)
            // scale bar + attribution
            val barM = listOf(50, 100, 200, 500, 1000, 2000).first { it / mpp > 60 || it == 2000 }
            val barPx = (barM / mpp).toFloat()
            p.style = Paint.Style.FILL; p.color = Color.parseColor("#AA000000"); c.drawRect(8f, h - 26f, 20f + barPx, h - 6f, p)
            p.color = Color.WHITE; c.drawRect(14f, h - 14f, 14f + barPx, h - 11f, p); p.textSize = 11f; c.drawText(if (barM >= 1000) "${barM / 1000} km" else "$barM m", 14f, h - 16f, p)
            p.textSize = 10f; p.color = Color.parseColor("#CCFFFFFF"); c.drawText("© OpenStreetMap contributors", w - 150f, h - 8f, p)
            val dir = File(ctx.filesDir, "widget").apply { mkdirs() }
            val f = File(dir, "map.png")
            f.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 90, it) }
            f.absolutePath
        } finally { provider.detach() }
    }

    companion object {
        fun distanceM(la1: Double, lo1: Double, la2: Double, lo2: Double): Double {
            val d2r = PI / 180; val dLa = (la2 - la1) * d2r; val dLo = (lo2 - lo1) * d2r
            val a = sin(dLa / 2) * sin(dLa / 2) + cos(la1 * d2r) * cos(la2 * d2r) * sin(dLo / 2) * sin(dLo / 2)
            return 2 * 6371000 * atan2(sqrt(a), sqrt(1 - a))
        }
    }
}

@HiltWorker
class WidgetWorker @AssistedInject constructor(@Assisted ctx: Context, @Assisted params: WorkerParameters, private val updater: WidgetUpdater, private val prefs: Prefs, private val status: CollectorStatus,
                                              private val recorder: org.sworrl.beaconfix.collector.ObservationRecorder) : CoroutineWorker(ctx, params) {
    override suspend fun doWork(): Result {
        // with the status notification hidden there is no foreground service: this 15-min job is all the background collection Android allows
        if (prefs.collectorOn.first() && !status.state.value.presence) runCatching { recorder.scanAndRecord(fresh = true) }
        runCatching { updater.refresh() }
        return Result.success()
    }
}
