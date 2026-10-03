package org.sworrl.beaconfix.car

import android.content.Intent
import android.net.Uri
import androidx.car.app.CarContext
import androidx.car.app.Screen
import androidx.car.app.model.Action
import androidx.car.app.model.ActionStrip
import androidx.car.app.model.CarColor
import androidx.car.app.model.CarLocation
import androidx.car.app.model.ItemList
import androidx.car.app.model.Metadata
import androidx.car.app.model.Place
import androidx.car.app.model.PlaceListMapTemplate
import androidx.car.app.model.PlaceMarker
import androidx.car.app.model.Row
import androidx.car.app.model.Template
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import dagger.hilt.EntryPoint
import dagger.hilt.InstallIn
import dagger.hilt.android.EntryPointAccessors
import dagger.hilt.components.SingletonComponent
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.data.DesktopLive
import org.sworrl.beaconfix.data.api.FlockCameraDto
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.detector.DetectionType
import org.sworrl.beaconfix.detector.DetectorAlertManager
import org.sworrl.beaconfix.estimate.Geo
import kotlin.math.roundToInt

@EntryPoint
@InstallIn(SingletonComponent::class)
interface CarEntryPoint {
    fun database(): AppDatabase
    fun live(): DesktopLive
    fun alertManager(): DetectorAlertManager
}

class BeaconFixCarScreen(carContext: CarContext) : Screen(carContext) {

    private val scope = CoroutineScope(Dispatchers.Main + Job())
    private val entry = EntryPointAccessors.fromApplication(
        carContext.applicationContext,
        CarEntryPoint::class.java
    )
    private val db = entry.database()
    private val live = entry.live()
    private val alertManager = entry.alertManager()

    private var isAlertMuted = false
    private var lastLat: Double = 0.0
    private var camAt = 0L; private var camLat = 0.0; private var camLon = 0.0   // where/when the car last fetched cameras
    private var lastLon: Double = 0.0
    private var nearestCameraDistanceM: Double? = null
    private var nearbyCameras: List<Pair<FlockCameraDto, Double>> = emptyList()
    private var activeApsCount: Int = 0
    private var isSyncingUs = false

    init {
        lifecycle.addObserver(object : DefaultLifecycleObserver {
            override fun onStart(owner: LifecycleOwner) {
                observeData()
            }
        })
    }

    private fun observeData() {
        scope.launch {
            try {
                db.fixes().latest().collectLatest { fix ->
                    if (fix != null && (fix.lat != 0.0 || fix.lon != 0.0)) {
                        lastLat = fix.lat
                        lastLon = fix.lon

                        // cameras for 50 km around: re-asked after 5 km or 5 min, not on every 4 s fix
                        val now = System.currentTimeMillis()
                        if (camAt == 0L || now - camAt > 300_000 || Geo.distanceM(camLat, camLon, fix.lat, fix.lon) > 5_000) {
                            if (live.refreshAll(setOf("flock"), fix.lat to fix.lon, flockKey = "car")) { camAt = now; camLat = fix.lat; camLon = fix.lon }
                        }

                        val views = live.views.value.values
                        val cams = views.flatMap { it.flockCameras }.distinctBy { it.id }

                        val sortedWithDist = cams.mapNotNull { cam ->
                            if (cam.lat != 0.0 && cam.lon != 0.0) {
                                val d = Geo.distanceM(fix.lat, fix.lon, cam.lat, cam.lon)
                                cam to d
                            } else null
                        }.sortedBy { it.second }

                        nearbyCameras = sortedWithDist.take(6)

                        val closest = sortedWithDist.firstOrNull()
                        if (closest != null && closest.second < 2500.0) {
                            nearestCameraDistanceM = closest.second
                            if (closest.second < 800.0 && !isAlertMuted) {
                                alertManager.triggerAlert(DetectionType.ALPR_FLOCK)
                            }
                        } else {
                            nearestCameraDistanceM = null
                        }
                        invalidate()
                    }
                }
            } catch (_: Exception) {}
        }

        scope.launch {
            try {
                db.aps().count().collectLatest { count ->
                    activeApsCount = count
                    invalidate()
                }
            } catch (_: Exception) {}
        }
    }

    override fun onGetTemplate(): Template {
        val itemListBuilder = ItemList.Builder()

        val dist = nearestCameraDistanceM
        val plate = live.views.value.values.firstNotNullOfOrNull { it.alprSummary?.activePlate }   // the desktop's registered plate
        val plateText = plate?.let { p -> listOf(p.state, p.displayPlate.ifBlank { p.plate }).filter { it.isNotBlank() }.joinToString(" ") }
        val title = if (dist != null && dist < 1200.0) {
            val distFt = (dist * 3.28084).roundToInt()
            "⚠️ ALPR: $distFt FT" + (plateText?.let { " · $it" } ?: "")
        } else {
            "🛡️ Radar Armed" + (plateText?.let { " · $it" } ?: "")
        }

        if (nearbyCameras.isNotEmpty()) {
            for ((idx, pair) in nearbyCameras.withIndex()) {
                val (cam, d) = pair
                val distFt = (d * 3.28084).roundToInt()
                val distStr = if (d < 1609.34) "$distFt ft" else "%.1f mi".format(d / 1609.34)
                val dirStr = cam.direction.ifEmpty { "Omni" }
                val modelStr = cam.model.ifEmpty { "Flock Falcon" }

                val loc = CarLocation.create(cam.lat, cam.lon)
                val marker = PlaceMarker.Builder()
                    .setColor(if (cam.vetted) CarColor.RED else CarColor.YELLOW)
                    .setLabel((idx + 1).toString())
                    .build()
                val place = Place.Builder(loc).setMarker(marker).build()
                val meta = Metadata.Builder().setPlace(place).build()

                val passCount = cam.passCount.coerceAtLeast(1)
                val statusStr = if (cam.vetted) "Vetted ALPR" else "Candidate ALPR"
                val operatorStr = cam.operatorName.ifEmpty { "operator unknown" }

                val row = Row.Builder()
                    .setTitle("$modelStr · $distStr ($dirStr)")
                    .addText("Encounter #$passCount · $statusStr · $operatorStr")
                    .addText(plate?.let { p -> listOf(plateText, p.vehicleDesc).filter { !it.isNullOrBlank() }.joinToString(" · ") } ?: "No plate set")
                    .setMetadata(meta)
                    .setOnClickListener {
                        val navUri = Uri.parse("geo:${cam.lat},${cam.lon}?q=${cam.lat},${cam.lon}($modelStr)")
                        val navIntent = Intent(CarContext.ACTION_NAVIGATE, navUri)
                        try {
                            carContext.startCarApp(navIntent)
                        } catch (_: Exception) {}
                    }
                    .build()

                itemListBuilder.addItem(row)
            }
        } else {
            itemListBuilder.setNoItemsMessage(
                "All Clear: No ALPR cameras detected within 1.5 miles." + (plateText?.let { " Watching for $it." } ?: "")
            )
        }

        val actionStripBuilder = ActionStrip.Builder()
            .addAction(
                Action.Builder()
                    .setTitle(if (isAlertMuted) "🔊 Unmute" else "🔇 Mute")
                    .setOnClickListener {
                        isAlertMuted = !isAlertMuted
                        invalidate()
                    }
                    .build()
            )
            .addAction(
                Action.Builder()
                    .setTitle(if (isSyncingUs) "Syncing..." else "Sync US")
                    .setOnClickListener {
                        if (!isSyncingUs) {
                            isSyncingUs = true
                            invalidate()
                            scope.launch {
                                try {
                                    live.syncNationwideUs()
                                    if (lastLat != 0.0 && lastLon != 0.0) {
                                        live.refreshAll(setOf("flock"), lastLat to lastLon, flockKey = "car")
                                    }
                                } finally {
                                    isSyncingUs = false
                                    invalidate()
                                }
                            }
                        }
                    }
                    .build()
            )
            .addAction(
                Action.Builder()
                    .setTitle("Scan")
                    .setOnClickListener {
                        scope.launch {
                            if (lastLat != 0.0 && lastLon != 0.0) {
                                live.refreshAll(setOf("flock"), lastLat to lastLon, flockKey = "car")
                            }
                            invalidate()
                        }
                    }
                    .build()
            )

        return PlaceListMapTemplate.Builder()
            .setTitle(title)
            .setHeaderAction(Action.APP_ICON)
            .setCurrentLocationEnabled(true)
            .setActionStrip(actionStripBuilder.build())
            .setItemList(itemListBuilder.build())
            .build()
    }
}
