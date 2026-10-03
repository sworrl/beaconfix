package org.sworrl.beaconfix.backup

import android.content.Context
import android.database.Cursor
import android.net.Uri
import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.sqlite.db.SimpleSQLiteQuery
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody.Companion.asRequestBody
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.DevFlags
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.BeaconFixApi
import org.sworrl.beaconfix.data.db.AnchorEntity
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.data.db.ObservationEntity
import org.sworrl.beaconfix.identity.IdentityStore
import retrofit2.Retrofit
import retrofit2.converter.kotlinx.serialization.asConverterFactory
import java.io.File
import java.io.OutputStreamWriter
import java.io.Writer
import java.time.LocalDate
import java.util.concurrent.TimeUnit
import javax.inject.Inject

/**
 * Phone backup: "Save backup…" writes [BackupWriter]'s file through the system file picker; "Send to the RV" uploads
 * the same file to a paired desktop's `POST /api/v1/db/import` (control scope), which merges it into its map database
 * (rows it already has are skipped). Restoring goes through the existing Import screen.
 */
@HiltViewModel
class BackupViewModel @Inject constructor(
    @ApplicationContext private val app: Context,
    private val db: AppDatabase,
    private val prefs: Prefs,
    private val store: DesktopStore,
    private val identity: IdentityStore,
) : ViewModel() {
    val busy = MutableStateFlow(false)
    val message = MutableStateFlow("")
    val lastBackupAt: StateFlow<Long> = prefs.lastBackupAt.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0L)
    /** Paired desktops this phone may send the backup to (control scope). */
    val targets: StateFlow<List<DesktopEntity>> = store.all()
        .map { list -> list.filter { it.paired && store.hasScope(it, "control") && store.token(it.id) != null } }
        .flowOn(Dispatchers.IO)
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    fun suggestedFileName(): String = "beaconfix-phone-${LocalDate.now()}.json"

    fun save(uri: Uri) = job {
        try {
            val counts = withContext(Dispatchers.IO) {
                val os = runCatching { app.contentResolver.openOutputStream(uri, "wt") }.getOrNull() ?: app.contentResolver.openOutputStream(uri) ?: error("cannot open the file")
                OutputStreamWriter(os, Charsets.UTF_8).buffered(1 shl 16).use { writeAll(it) }
            }
            prefs.setLastBackupAt(System.currentTimeMillis())
            Log.i(TAG, "saved backup: $counts")
            message.value = app.getString(R.string.backup_saved, counts.aps, counts.observations, counts.fixes)
        } catch (e: CancellationException) { throw e } catch (e: Exception) {
            Log.w(TAG, "save failed", e)
            message.value = app.getString(R.string.backup_failed, e.message ?: e.javaClass.simpleName)
        }
    }

    fun sendTo(d: DesktopEntity) = job {
        if (DevFlags.desktopBlocked()) { message.value = app.getString(R.string.backup_send_unreachable, d.name); return@job }
        val auth = store.auth(d)
        if (auth == null) { message.value = app.getString(R.string.backup_send_needs_control); return@job }
        val tmp = File(app.cacheDir, "backup-upload.json")
        try {
            val counts = withContext(Dispatchers.IO) { tmp.writer(Charsets.UTF_8).buffered(1 shl 16).use { writeAll(it) } }
            val name = "phone-${LocalDate.now()}.json"
            val r = withContext(Dispatchers.IO) { uploadApi(d).dbImport(auth, name, tmp.asRequestBody("application/json".toMediaType())) }
            val body: JsonObject? = r.body() ?: r.errorBody()?.string()?.let { s -> runCatching { ApiFactory.json.parseToJsonElement(s) as? JsonObject }.getOrNull() }
            val ok = r.isSuccessful && body?.get("ok")?.jsonPrimitive?.booleanOrNull != false
            Log.i(TAG, "send to ${d.name}: HTTP ${r.code()} ok=$ok ($counts)")
            message.value = when {
                ok -> app.getString(R.string.backup_sent, d.name, body?.get("observations")?.jsonPrimitive?.intOrNull ?: 0)
                r.code() == 401 || r.code() == 403 -> app.getString(R.string.backup_send_needs_control)
                r.code() == 409 -> app.getString(R.string.backup_send_busy, d.name)
                r.code() == 413 -> app.getString(R.string.backup_send_too_big)
                else -> app.getString(R.string.backup_send_failed, d.name, body?.get("error")?.jsonPrimitive?.contentOrNull ?: "HTTP ${r.code()}")
            }
        } catch (e: CancellationException) { throw e } catch (e: Exception) {
            Log.w(TAG, "send failed", e)
            message.value = app.getString(R.string.backup_send_unreachable, d.name)
        } finally {
            withContext(Dispatchers.IO) { tmp.delete() }
        }
    }

    /** One job at a time; [busy] while it runs. */
    private fun job(block: suspend () -> Unit) {
        if (busy.value) return
        busy.value = true; message.value = ""
        viewModelScope.launch { try { block() } finally { busy.value = false } }
    }

    /** A large upload and a desktop that merges before it answers: longer timeouts than ApiFactory's, and no silent re-send. */
    private fun uploadApi(d: DesktopEntity): BeaconFixApi {
        val client = ApiFactory.client.newBuilder().writeTimeout(5, TimeUnit.MINUTES).readTimeout(5, TimeUnit.MINUTES).retryOnConnectionFailure(false).build()
        return Retrofit.Builder().baseUrl(ApiFactory.baseUrl(d.host, d.port, d.tls)).client(client)
            .addConverterFactory(ApiFactory.json.asConverterFactory("application/json".toMediaType())).build().create(BeaconFixApi::class.java)
    }

    private fun writeAll(w: Writer): BackupWriter.Counts =
        BackupWriter.write(w, rows(SQL_APS, ::ap), rows(SQL_OBS, ::observation), rows(SQL_FIXES, ::fix), rows(SQL_ANCHORS, ::anchor), identity.deviceName)

    /** Rows streamed from one cursor (the tables can hold hundreds of thousands of observations). */
    private fun <T> rows(sql: String, map: (Cursor) -> T): Sequence<T> = sequence {
        db.query(SimpleSQLiteQuery(sql)).use { c -> while (c.moveToNext()) yield(map(c)) }
    }

    private fun Cursor.s(col: String) = getColumnIndex(col).let { if (it < 0 || isNull(it)) "" else getString(it) }
    private fun Cursor.d(col: String) = getColumnIndex(col).let { if (it < 0 || isNull(it)) null else getDouble(it) }
    private fun Cursor.l(col: String) = getColumnIndex(col).let { if (it < 0 || isNull(it)) 0L else getLong(it) }
    private fun Cursor.b(col: String) = l(col) != 0L

    private fun ap(c: Cursor) = ApEntity(bssid = c.s("bssid"), ssid = c.s("ssid"), freq = c.l("freq").toInt(), band = c.s("band"), ch = c.l("ch").toInt(),
        firstSeen = c.l("firstSeen"), lastSeen = c.l("lastSeen"), timesSeen = c.l("timesSeen").toInt(), lat = c.d("lat"), lon = c.d("lon"), acc = c.d("acc"),
        posSource = c.s("posSource"), security = c.s("security"))
    private fun observation(c: Cursor) = ObservationEntity(id = c.l("id"), bssid = c.s("bssid"), time = c.l("time"), lat = c.d("lat") ?: 0.0, lon = c.d("lon") ?: 0.0,
        acc = c.d("acc") ?: 0.0, dbm = c.l("dbm").toInt(), freq = c.l("freq").toInt(), source = c.s("source"), synced = c.b("synced"), remote = c.b("remote"),
        rangeM = c.d("rangeM"), rangeSd = c.d("rangeSd"))
    private fun fix(c: Cursor) = FixEntity(id = c.l("id"), time = c.l("time"), lat = c.d("lat") ?: 0.0, lon = c.d("lon") ?: 0.0, acc = c.d("acc") ?: 0.0,
        source = c.s("source"), provider = c.s("provider"), place = c.s("place"))
    private fun anchor(c: Cursor) = AnchorEntity(id = c.s("id"), json = c.s("json"), name = c.s("name"), kind = c.s("kind"), lat = c.d("lat") ?: 0.0, lon = c.d("lon") ?: 0.0,
        rv = c.b("rv"), ref = c.b("ref"), deleted = c.b("deleted"), placedAt = c.s("placedAt"), seq = c.l("seq"), dirty = c.b("dirty"))

    private companion object {
        const val TAG = "BfBackup"
        const val SQL_APS = "SELECT bssid, ssid, freq, band, ch, firstSeen, lastSeen, timesSeen, lat, lon, acc, posSource, security FROM aps ORDER BY bssid"
        const val SQL_OBS = "SELECT id, bssid, time, lat, lon, acc, dbm, freq, source, synced, remote, rangeM, rangeSd FROM observations WHERE remote = 0 ORDER BY id"
        const val SQL_FIXES = "SELECT id, time, lat, lon, acc, source, provider, place FROM fixes WHERE source <> 'desktop' ORDER BY id"
        const val SQL_ANCHORS = "SELECT * FROM anchors ORDER BY id"
    }
}
