package org.sworrl.beaconfix.net

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.wifi.rtt.WifiRttManager
import android.os.BatteryManager
import android.os.Build
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import kotlinx.serialization.json.putJsonObject
import kotlinx.serialization.json.add
import retrofit2.http.Body
import retrofit2.http.POST
import java.util.concurrent.atomic.AtomicBoolean
import javax.inject.Inject
import javax.inject.Singleton

/** The hub's node routes (docs/HUB.md), over BFS3 like everything else. */
interface HubNodeApi {
    @POST("api/v1/nodes/heartbeat") suspend fun heartbeat(@Body body: JsonObject): retrofit2.Response<JsonObject>
}

/**
 * Light jobs the phone may run for the hub later (docs/HUB.md "Jobs": lease → compute → submit), e.g. refits while
 * charging on Wi-Fi. Not wired yet: [HubNode] reports [types] as the capabilities' `jobs` and never leases while the
 * runner is [NoJobs]. A real runner plugs in here: [eligible] gates leasing (charging, unmetered, idle), [run] turns
 * one leased job (`{"id","lease","type","key","watermark","expires"}`) into its `result` object (or throws → `error`).
 */
interface JobRunner {
    val types: List<String>
    fun eligible(state: NodeState): Boolean
    suspend fun run(job: JsonObject): JsonObject
}
object NoJobs : JobRunner {
    override val types: List<String> = emptyList()
    override fun eligible(state: NodeState) = false
    override suspend fun run(job: JsonObject): JsonObject = throw UnsupportedOperationException("this phone runs no hub jobs yet")
}

/** What the phone is and can do right now — the `capabilities` object of the heartbeat. */
data class NodeState(
    val cores: Int, val rtt: Boolean, val rttAz: Boolean, val tensor: Boolean, val nnapi: Boolean, val batteryPct: Int?, val charging: Boolean,
    val version: String, val sdk: Int, val model: String, val estimator: Int,
)

object NodeCapabilities {
    /**
     * The heartbeat's `capabilities` in the hub's vocabulary (docs/HUB.md "Node registry"): `compute` is the cores the
     * phone gives to hub jobs (0 while [runner] runs none — so the hub never counts on it), `jobs` the types it runs,
     * plus the phone's own keys (`mobile`, `rttAz`, `battery`, `cores`, …) that the hub keeps verbatim.
     */
    fun json(s: NodeState, runner: JobRunner = NoJobs): JsonObject = buildJsonObject {
        put("compute", if (runner.types.isEmpty()) 0 else s.cores)
        put("cores", s.cores)
        put("wifiScan", 1)
        put("rtt", s.rtt)
        put("rttAz", s.rttAz)
        put("mobile", true)
        put("tensor", s.tensor)
        put("nnapi", s.nnapi)
        put("version", s.version)
        putJsonArray("jobs") { runner.types.forEach { add(it) } }
        put("role", "phone")
        put("estimator", s.estimator)
        putJsonObject("battery") { s.batteryPct?.let { put("pct", it) }; put("charging", s.charging) }
        put("android", s.sdk)
        put("model", s.model)
    }

    /** Best-effort probe of this phone. `tensor` = an on-device TPU (a Google Tensor SoC); `nnapi` = NNAPI is offered (Android 8.1–14). */
    fun probe(ctx: Context): NodeState {
        val pm = ctx.packageManager
        val rtt = Build.VERSION.SDK_INT >= 28 && pm.hasSystemFeature(PackageManager.FEATURE_WIFI_RTT)
        val rttAz = rtt && Build.VERSION.SDK_INT >= 35 && runCatching {
            ctx.getSystemService(WifiRttManager::class.java)?.rttCharacteristics?.getBoolean(WifiRttManager.CHARACTERISTICS_KEY_BOOLEAN_NTB_INITIATOR) == true
        }.getOrDefault(false)
        val soc = if (Build.VERSION.SDK_INT >= 31) "${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}" else Build.HARDWARE
        val tensor = soc.contains("Google", true) || soc.contains("Tensor", true) || Build.HARDWARE.lowercase() in setOf("gs101", "gs201", "zuma", "zumapro", "laguna")
        val bat = runCatching { ctx.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)) }.getOrNull()
        val level = bat?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1; val scale = bat?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        val plugged = (bat?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
        return NodeState(Runtime.getRuntime().availableProcessors(), rtt, rttAz, tensor, Build.VERSION.SDK_INT in 27..34, if (level >= 0 && scale > 0) level * 100 / scale else null, plugged,
            org.sworrl.beaconfix.BuildConfig.VERSION_NAME, Build.VERSION.SDK_INT, (Build.MANUFACTURER + " " + Build.MODEL).trim(), org.sworrl.beaconfix.estimate.Estimator.VERSION)
    }
}

/**
 * This phone as a node of the hub: its capabilities go up with `POST /nodes/heartbeat` right after enrolment, after
 * every hub sync, and at most every [HEARTBEAT_MS] from the collector's fix feed. Job leasing is not done yet — see
 * [JobRunner]; [runner] is where it plugs in.
 */
@Singleton
class HubNode @Inject constructor(@ApplicationContext private val ctx: Context, private val hub: HubClient, private val store: HubStore) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val busy = AtomicBoolean(false)
    @Volatile private var lastAt = 0L
    @Volatile private var failedAt = 0L
    /** Jobs waiting at the hub, as its last heartbeat answer said (-1 = unknown). */
    @Volatile var pendingJobs = -1; private set
    var runner: JobRunner = NoJobs

    fun capabilities(): JsonObject = NodeCapabilities.json(NodeCapabilities.probe(ctx), runner)

    /** Send a heartbeat now (enrolment, sync); returns whether the hub took it. */
    suspend fun heartbeat(): Boolean {
        val api = hub.nodeApi() ?: return false
        return try {
            val r = api.heartbeat(buildJsonObject { put("capabilities", capabilities()) })
            if (r.isSuccessful) { lastAt = System.currentTimeMillis(); failedAt = 0; pendingJobs = r.body()?.get("pending")?.jsonPrimitive?.intOrNull ?: -1; store.noteContact(); true }
            else { failedAt = System.currentTimeMillis(); false }       // 404: a hub without the node registry — nothing to do
        } catch (e: CancellationException) { throw e } catch (e: Exception) {
            failedAt = System.currentTimeMillis(); store.noteError(HubErrors.describe(e), HubErrors.isUnreachable(e)); false
        }
    }

    /** Cheap; from any thread: a heartbeat when the last one is [HEARTBEAT_MS] old (a failure waits [RETRY_MS]). */
    fun maybeHeartbeat() {
        if (store.cached == null) return
        val now = System.currentTimeMillis()
        if (now - lastAt < HEARTBEAT_MS || (failedAt > 0 && now - failedAt < RETRY_MS)) return
        if (!busy.compareAndSet(false, true)) return
        scope.launch { try { heartbeat() } finally { busy.set(false) } }
    }

    companion object {
        const val HEARTBEAT_MS = 10 * 60_000L
        const val RETRY_MS = 5 * 60_000L
    }
}
