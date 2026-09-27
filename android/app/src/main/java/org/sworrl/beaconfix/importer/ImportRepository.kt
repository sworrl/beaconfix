package org.sworrl.beaconfix.importer

import android.content.Context
import android.net.Uri
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.withContext
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.data.db.ObservationEntity
import org.sworrl.beaconfix.estimate.EstimateRepository
import org.sworrl.beaconfix.widget.WidgetUpdater
import java.io.BufferedInputStream
import javax.inject.Inject
import javax.inject.Singleton

data class ImportSummary(val format: String = "", val positions: Int = 0, val scans: Int = 0, val observations: Int = 0, val aps: Int = 0, val stops: Int = 0, val from: Long = 0, val to: Long = 0, val refit: Int = 0, val skipped: Int = 0)
data class ImportOptions(val positions: Boolean = true, val wifi: Boolean = true, val places: Boolean = true, val from: Long = 0, val to: Long = Long.MAX_VALUE)

/** Reads a file through the Storage Access Framework, parses it (streaming), then writes fixes / observations / beacons and refits. */
@Singleton
class ImportRepository @Inject constructor(@ApplicationContext private val ctx: Context, private val db: AppDatabase, private val estimates: EstimateRepository, private val widgets: WidgetUpdater) {
    val progress = MutableStateFlow("")       // human line while parsing/writing
    val parsed = MutableStateFlow<ImportResult?>(null)
    val fileName = MutableStateFlow("")
    val format = MutableStateFlow("")
    val size = MutableStateFlow(0L)

    suspend fun open(uri: Uri): Result<ImportResult> = withContext(Dispatchers.IO) { runCatching {
        val name = queryName(uri); fileName.value = name
        val len = runCatching { ctx.contentResolver.openFileDescriptor(uri, "r")?.use { it.statSize } ?: 0L }.getOrDefault(0L); size.value = len
        val head = ctx.contentResolver.openInputStream(uri)?.use { s -> val b = ByteArray(4096); val n = s.read(b); if (n > 0) String(b, 0, n, Charsets.UTF_8) else "" } ?: ""
        val fmt = Importers.detect(head, name); format.value = fmt
        if (fmt == "unknown") error("not a format BeaconFix knows (Timeline.json, Records.json, Semantic Location History, WiGLE CSV, GPX, KML, BeaconFix export)")
        progress.value = "reading $name…"
        val res = ctx.contentResolver.openInputStream(uri)?.use { s -> Importers.parse(fmt, BufferedInputStream(s, 1 shl 16)) { bytes, items -> progress.value = if (len > 0) "reading… ${bytes * 100 / len}% · $items items" else "reading… $items items" } } ?: error("cannot open that file")
        progress.value = "read ${res.positions.size} positions, ${res.scans.size} Wi-Fi scans, ${res.observations.size} observations, ${res.stops.size} places"
        parsed.value = res; res
    } }

    suspend fun apply(res: ImportResult, o: ImportOptions): ImportSummary = withContext(Dispatchers.IO) {
        val inRange = { t: Long -> t == 0L || (t in o.from..o.to) }
        var positions = 0; var obsN = 0; var apsN = 0; var stops = 0; var skipped = 0
        val now = System.currentTimeMillis()
        // 1. pair Wi-Fi scans with positions (creates observations); then filter by range
        val paired = if (o.wifi && res.scans.isNotEmpty()) Importers.pairScans(res) else 0
        progress.value = "paired $paired samples with positions"
        // 2. positions → fixes (thin: at most one per 30 s so a 100 MB Timeline does not become a million rows)
        if (o.positions) {
            var lastT = 0L
            for (p in res.positions.filter { inRange(it.time) }.sortedBy { it.time }) {
                if (p.time != 0L && p.time - lastT < 30_000) { skipped++; continue }
                lastT = p.time
                db.fixes().insert(FixEntity(time = if (p.time == 0L) now else p.time, lat = p.lat, lon = p.lon, acc = p.acc, source = "import", provider = p.source))
                positions++
                if (positions % 500 == 0) progress.value = "writing positions… $positions"
            }
        }
        // 3. observations + beacon rows
        if (o.wifi) {
            val known = db.aps().allBssids().toHashSet()
            val batch = ArrayList<ObservationEntity>()
            val touched = HashSet<String>()
            val apInfo = HashMap<String, ImpObservation>()
            for (ob in res.observations.filter { inRange(it.time) }) {
                if (ob.acc <= 0 || ob.acc > 150) { skipped++; continue }
                batch += ObservationEntity(bssid = ob.bssid, time = ob.time, lat = ob.lat, lon = ob.lon, acc = ob.acc, dbm = ob.dbm, freq = ob.freq, source = "import:" + res.format, synced = false)
                touched += ob.bssid
                if (ob.ssid.isNotEmpty() || ob.security.isNotEmpty()) apInfo.putIfAbsent(ob.bssid, ob)
                if (batch.size >= 500) { obsN += db.observations().insertAll(batch).count { it > 0 }; batch.clear(); progress.value = "writing observations… $obsN" }
            }
            if (batch.isNotEmpty()) obsN += db.observations().insertAll(batch).count { it > 0 }
            for (b in touched) {
                val old = db.aps().get(b); val info = apInfo[b]
                if (old == null) { db.aps().upsert(ApEntity(bssid = b, ssid = info?.ssid ?: "", freq = info?.freq ?: 0, band = org.sworrl.beaconfix.collector.ObservationRecorder.bandOf(info?.freq ?: 0), ch = org.sworrl.beaconfix.collector.ObservationRecorder.channelOf(info?.freq ?: 0), firstSeen = now, lastSeen = now, timesSeen = 1, security = info?.security ?: "")); apsN++ }
                else if (info != null && (old.ssid.isEmpty() || old.security.isEmpty())) db.aps().upsert(old.copy(ssid = old.ssid.ifEmpty { info.ssid }, security = old.security.ifEmpty { info.security }))
            }
            // desktop-export beacon notes (ssid/security only)
            for (n in res.notes.filter { it.startsWith("ap:") }) { val p = n.removePrefix("ap:").split('|'); if (p.size == 3 && p[0] !in known) { db.aps().upsert(ApEntity(bssid = p[0], ssid = p[1], security = p[2], firstSeen = now, lastSeen = now)); apsN++ } }
            progress.value = "refitting ${touched.size} beacons…"
            val refit = estimates.refit(touched.take(2000))
            widgets.touch("import")
            return@withContext ImportSummary(res.format, positions, res.scans.size, obsN, apsN, if (o.places) res.stops.count { inRange(it.start) } else 0, res.firstTime ?: 0, res.lastTime ?: 0, refit, skipped).also { progress.value = "done"; storeStops(res, o) }
        }
        ImportSummary(res.format, positions, res.scans.size, 0, 0, if (o.places) res.stops.count { inRange(it.start) } else 0, res.firstTime ?: 0, res.lastTime ?: 0, 0, skipped).also { progress.value = "done"; storeStops(res, o); widgets.touch("import") }
    }
    /** Stops become fixes with a place name (the desktop's trip log picks them up on sync). */
    private suspend fun storeStops(res: ImportResult, o: ImportOptions) { if (!o.places) return; for (s in res.stops) if (s.start == 0L || s.start in o.from..o.to) db.fixes().insert(FixEntity(time = s.start, lat = s.lat, lon = s.lon, acc = 50.0, source = "import", provider = "stop:" + res.format, place = s.name)) }
    fun clear() { parsed.value = null; progress.value = ""; fileName.value = ""; format.value = "" }
    private fun queryName(uri: Uri): String = runCatching { ctx.contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c -> if (c.moveToFirst()) c.getString(0) else null } }.getOrNull() ?: (uri.lastPathSegment ?: "file")
}
