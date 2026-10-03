package org.sworrl.beaconfix.data

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.doublePreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.core.stringSetPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import javax.inject.Inject
import javax.inject.Singleton

private val Context.store: DataStore<Preferences> by preferencesDataStore("beaconfix")

@Singleton
class Prefs @Inject constructor(@ApplicationContext private val ctx: Context) {
    private object K {
        val collectorOn = booleanPreferencesKey("collector_on")
        val collectInterval = intPreferencesKey("collect_interval_s")
        val maxFixAcc = intPreferencesKey("max_fix_acc_m")
        val homePatterns = stringSetPreferencesKey("home_patterns")
        val autoSync = booleanPreferencesKey("auto_sync")
        val unmeteredOnly = booleanPreferencesKey("unmetered_only")
        val lastSyncReport = stringPreferencesKey("last_sync_report")
        val lastSyncAt = longPreferencesKey("last_sync_at")
        val throttleHintSeen = booleanPreferencesKey("throttle_hint_seen")
        val statusNotification = booleanPreferencesKey("status_notification")
        // 1.4
        val units = stringPreferencesKey("units")
        val phonePlaces = booleanPreferencesKey("phone_places")
        val phonePlacesMetered = booleanPreferencesKey("phone_places_metered")
        val pedsRadiusKm = intPreferencesKey("peds_radius_km")
        val helpAlerts = booleanPreferencesKey("help_alerts")
        val wifiAlerts = booleanPreferencesKey("wifi_alerts")
        val devAutomation = booleanPreferencesKey("dev_automation")
        val simOffline = booleanPreferencesKey("sim_offline")
        val simNoDesktop = booleanPreferencesKey("sim_no_desktop")
        val homeDirty = booleanPreferencesKey("home_dirty")
        val mapStyle = stringPreferencesKey("map_style")
        val mapPoiFilter = stringPreferencesKey("map_poi_filter")
        val lastBackupAt = longPreferencesKey("last_backup_at")
        val helpAlertState = stringPreferencesKey("help_alert_state")
        // 1.5
        val estimatorVersion = intPreferencesKey("estimator_version")
        val rttOffset = doublePreferencesKey("rtt_offset_m")
        val rttOffsetSd = doublePreferencesKey("rtt_offset_sd_m")
        // 1.6
        val estimatorCalibration = stringPreferencesKey("estimator_calibration")
        // sightings (docs/SIGHTINGS.md)
        val plateEventCursors = stringPreferencesKey("plate_event_cursors")
        val hibfWatch = stringPreferencesKey("hibf_watch")
        val lastDrivingAt = longPreferencesKey("last_driving_at")
        val registeredPlates = stringPreferencesKey("registered_plates")
        val hibfSources = stringPreferencesKey("hibf_sources")
        val webcamStills = booleanPreferencesKey("webcam_stills")
        // inspection unseen (§9.5)
        val privateInspectionActive = booleanPreferencesKey("private_inspection_active")
        val activeInspectionTarget = stringPreferencesKey("active_inspection_target")
        val activeInspectionVantageReached = booleanPreferencesKey("active_inspection_vantage_reached")
        val routingOrsKey = stringPreferencesKey("routing_ors_key")
        val routingGraphhopperKey = stringPreferencesKey("routing_graphhopper_key")
        val routingProvider = stringPreferencesKey("routing_provider")
    }
    val collectorOn: Flow<Boolean> = ctx.store.data.map { it[K.collectorOn] ?: false }
    val collectIntervalSec: Flow<Int> = ctx.store.data.map { it[K.collectInterval] ?: 60 }
    val maxFixAccM: Flow<Int> = ctx.store.data.map { it[K.maxFixAcc] ?: 60 }
    val homePatterns: Flow<Set<String>> = ctx.store.data.map { it[K.homePatterns] ?: emptySet() }
    val autoSync: Flow<Boolean> = ctx.store.data.map { it[K.autoSync] ?: true }
    val unmeteredOnly: Flow<Boolean> = ctx.store.data.map { it[K.unmeteredOnly] ?: false }
    val lastSyncReport: Flow<String> = ctx.store.data.map { it[K.lastSyncReport] ?: "" }
    val lastSyncAt: Flow<Long> = ctx.store.data.map { it[K.lastSyncAt] ?: 0L }
    val throttleHintSeen: Flow<Boolean> = ctx.store.data.map { it[K.throttleHintSeen] ?: false }
    val statusNotification: Flow<Boolean> = ctx.store.data.map { it[K.statusNotification] ?: true }
    /** auto | metric | imperial */
    val units: Flow<String> = ctx.store.data.map { it[K.units] ?: "auto" }
    /** let this phone search OpenStreetMap for help places itself when no desktop answers */
    val phonePlaces: Flow<Boolean> = ctx.store.data.map { it[K.phonePlaces] ?: true }
    /** …also over mobile data */
    val phonePlacesMetered: Flow<Boolean> = ctx.store.data.map { it[K.phonePlacesMetered] ?: true }
    /** the phone's own children's ER search radius, 50–300 km */
    val pedsRadiusKm: Flow<Int> = ctx.store.data.map { (it[K.pedsRadiusKm] ?: PEDS_RADIUS_DEFAULT).coerceIn(PEDS_RADIUS_MIN, PEDS_RADIUS_MAX) }
    val helpAlerts: Flow<Boolean> = ctx.store.data.map { it[K.helpAlerts] ?: true }
    val wifiAlerts: Flow<Boolean> = ctx.store.data.map { it[K.wifiAlerts] ?: true }
    /** lets adb test commands (forget_identity, import_*, sim_*) act on a release build */
    val devAutomation: Flow<Boolean> = ctx.store.data.map { it[K.devAutomation] ?: false }
    /** test switches: behave as if offline / as if no desktop were reachable (read through data.DevFlags) */
    val simOffline: Flow<Boolean> = ctx.store.data.map { it[K.simOffline] ?: false }
    val simNoDesktop: Flow<Boolean> = ctx.store.data.map { it[K.simNoDesktop] ?: false }
    /** home patterns were edited on the phone and still have to be pushed to a desktop */
    val homeDirty: Flow<Boolean> = ctx.store.data.map { it[K.homeDirty] ?: false }
    val mapStyle: Flow<String> = ctx.store.data.map { it[K.mapStyle] ?: "satellite" }   // the hybrid: APs read against the real world
    val mapPoiFilter: Flow<String> = ctx.store.data.map { it[K.mapPoiFilter] ?: "all" }
    val lastBackupAt: Flow<Long> = ctx.store.data.map { it[K.lastBackupAt] ?: 0L }
    /** JSON {lat, lon, at} of the last nearest-help heads-up */
    val helpAlertState: Flow<String> = ctx.store.data.map { it[K.helpAlertState] ?: "" }
    /** the estimator generation the stored AP estimates came from (estimate.Estimator.VERSION); older ones are recomputed once */
    val estimatorVersion: Flow<Int> = ctx.store.data.map { it[K.estimatorVersion] ?: 0 }

    suspend fun setCollectorOn(v: Boolean) = ctx.store.edit { it[K.collectorOn] = v }
    suspend fun setCollectInterval(s: Int) = ctx.store.edit { it[K.collectInterval] = s.coerceIn(15, 900) }
    suspend fun setMaxFixAcc(m: Int) = ctx.store.edit { it[K.maxFixAcc] = m.coerceIn(10, 500) }
    suspend fun setHomePatterns(p: Set<String>) = ctx.store.edit { it[K.homePatterns] = p }
    suspend fun setAutoSync(v: Boolean) = ctx.store.edit { it[K.autoSync] = v }
    suspend fun setUnmeteredOnly(v: Boolean) = ctx.store.edit { it[K.unmeteredOnly] = v }
    suspend fun setLastSync(report: String) = ctx.store.edit { it[K.lastSyncReport] = report; it[K.lastSyncAt] = System.currentTimeMillis() }
    suspend fun setThrottleHintSeen() = ctx.store.edit { it[K.throttleHintSeen] = true }
    suspend fun setStatusNotification(v: Boolean) = ctx.store.edit { it[K.statusNotification] = v }
    suspend fun setUnits(v: String) = ctx.store.edit { it[K.units] = if (v in UNITS) v else "auto" }
    suspend fun setPhonePlaces(v: Boolean) = ctx.store.edit { it[K.phonePlaces] = v }
    suspend fun setPhonePlacesMetered(v: Boolean) = ctx.store.edit { it[K.phonePlacesMetered] = v }
    suspend fun setPedsRadiusKm(km: Int) = ctx.store.edit { it[K.pedsRadiusKm] = km.coerceIn(PEDS_RADIUS_MIN, PEDS_RADIUS_MAX) }
    suspend fun setHelpAlerts(v: Boolean) = ctx.store.edit { it[K.helpAlerts] = v }
    suspend fun setWifiAlerts(v: Boolean) = ctx.store.edit { it[K.wifiAlerts] = v }
    suspend fun setDevAutomation(v: Boolean) = ctx.store.edit { it[K.devAutomation] = v }
    suspend fun setSimOffline(v: Boolean) = ctx.store.edit { it[K.simOffline] = v }
    suspend fun setSimNoDesktop(v: Boolean) = ctx.store.edit { it[K.simNoDesktop] = v }
    suspend fun setHomeDirty(v: Boolean) = ctx.store.edit { it[K.homeDirty] = v }
    suspend fun setMapStyle(v: String) = ctx.store.edit { it[K.mapStyle] = v }
    suspend fun setMapPoiFilter(v: String) = ctx.store.edit { it[K.mapPoiFilter] = v }
    suspend fun setLastBackupAt(ms: Long) = ctx.store.edit { it[K.lastBackupAt] = ms }
    suspend fun setHelpAlertState(json: String) = ctx.store.edit { it[K.helpAlertState] = json }
    suspend fun setEstimatorVersion(v: Int) = ctx.store.edit { it[K.estimatorVersion] = v }
    /** The paired desktop's estimator calibration in this phone's frame (estimate.EstimatorCalibration JSON), "" when none yet. */
    val estimatorCalibration: Flow<String> = ctx.store.data.map { it[K.estimatorCalibration] ?: "" }
    suspend fun setEstimatorCalibration(json: String) = ctx.store.edit { it[K.estimatorCalibration] = json }
    /**
     * The last calibrated Wi-Fi RTT offset (m) and its 1-σ a desktop reported for this phone (RangingRepository), null
     * when none or stale. It is a pair offset (this phone + that desktop's responder): ranging.ApRttMath.usableOffset
     * decides whether it can stand for the phone alone when ranging other APs.
     */
    val rttOffset: Flow<Pair<Double, Double>?> = ctx.store.data.map { p -> p[K.rttOffset]?.let { it to (p[K.rttOffsetSd] ?: 0.0) } }
    suspend fun setRttOffset(v: Pair<Double, Double>?) = ctx.store.edit { if (v == null) { it.remove(K.rttOffset); it.remove(K.rttOffsetSd) } else { it[K.rttOffset] = v.first; it[K.rttOffsetSd] = v.second } }

    /** `GET /plate-events` cursors per desktop: a JSON object {desktopId: seq}. */
    val plateEventCursors: Flow<String> = ctx.store.data.map { it[K.plateEventCursors] ?: "" }
    suspend fun setPlateEventCursors(json: String) = ctx.store.edit { it[K.plateEventCursors] = json }
    /** The HaveIBeenFlocked watcher's state (sightings.Hibf.WatchState JSON). */
    val hibfWatch: Flow<String> = ctx.store.data.map { it[K.hibfWatch] ?: "" }
    suspend fun setHibfWatch(json: String) = ctx.store.edit { it[K.hibfWatch] = json }
    /** The last time this phone was driving (in a vehicle, or > 20 km/h), ms; 0 never. */
    val lastDrivingAt: Flow<Long> = ctx.store.data.map { it[K.lastDrivingAt] ?: 0L }
    suspend fun setLastDrivingAt(ms: Long) = ctx.store.edit { it[K.lastDrivingAt] = ms }
    /** The registered plates the desktop serves (`GET /api/v1/plates` body as received), "" none yet. */
    val registeredPlates: Flow<String> = ctx.store.data.map { it[K.registeredPlates] ?: "" }
    suspend fun setRegisteredPlates(json: String) = ctx.store.edit { it[K.registeredPlates] = json }
    /** The desktop's leaky-agency list (`/plate-events/status` → `hibfSources`), as received. */
    val hibfSources: Flow<String> = ctx.store.data.map { it[K.hibfSources] ?: "" }
    suspend fun setHibfSources(json: String) = ctx.store.edit { it[K.hibfSources] = json }

    /** Keep a public webcam's still with a live pass (docs/SIGHTINGS.md §2.0). Off by default: providers such as WV511 forbid storing their images; on, stills stay on this phone. */
    val webcamStills: Flow<Boolean> = ctx.store.data.map { it[K.webcamStills] ?: false }
    suspend fun setWebcamStills(v: Boolean) = ctx.store.edit { it[K.webcamStills] = v }

    /** Private inspection mode (docs/SIGHTINGS.md §9.5): no fixes, observations or passes recorded; dashcam paused; no sync. */
    val privateInspectionActive: Flow<Boolean> = ctx.store.data.map { it[K.privateInspectionActive] ?: false }
    suspend fun setPrivateInspectionActive(v: Boolean) = ctx.store.edit { it[K.privateInspectionActive] = v }

    /** JSON serialization of active inspection plan/target for tracking and live guard alerts. */
    val activeInspectionTarget: Flow<String> = ctx.store.data.map { it[K.activeInspectionTarget] ?: "" }
    suspend fun setActiveInspectionTarget(json: String) = ctx.store.edit { it[K.activeInspectionTarget] = json }

    /** Whether the user reached the vantage point during current private inspection. */
    val activeInspectionVantageReached: Flow<Boolean> = ctx.store.data.map { it[K.activeInspectionVantageReached] ?: false }
    suspend fun setActiveInspectionVantageReached(v: Boolean) = ctx.store.edit { it[K.activeInspectionVantageReached] = v }

    /** User's OpenRouteService API key for avoiding ALPRs. */
    val routingOrsKey: Flow<String> = ctx.store.data.map { it[K.routingOrsKey] ?: "" }
    suspend fun setRoutingOrsKey(k: String) = ctx.store.edit { it[K.routingOrsKey] = k }

    /** User's GraphHopper API key for avoiding ALPRs. */
    val routingGraphhopperKey: Flow<String> = ctx.store.data.map { it[K.routingGraphhopperKey] ?: "" }
    suspend fun setRoutingGraphhopperKey(k: String) = ctx.store.edit { it[K.routingGraphhopperKey] = k }

    /** Routing provider ("ors" or "graphhopper"). */
    val routingProvider: Flow<String> = ctx.store.data.map { it[K.routingProvider] ?: "ors" }
    suspend fun setRoutingProvider(p: String) = ctx.store.edit { it[K.routingProvider] = p }

    companion object {
        val UNITS = setOf("auto", "metric", "imperial")
        const val PEDS_RADIUS_DEFAULT = 150
        const val PEDS_RADIUS_MIN = 50
        const val PEDS_RADIUS_MAX = 300
    }
}
