package org.sworrl.beaconfix

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import androidx.hilt.work.HiltWorkerFactory
import androidx.work.Configuration
import dagger.hilt.android.HiltAndroidApp
import org.osmdroid.config.Configuration as OsmConfig
import org.sworrl.beaconfix.sync.SyncScheduler
import javax.inject.Inject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

@HiltAndroidApp
class BeaconFixApp : Application(), Configuration.Provider {
    @Inject lateinit var workerFactory: HiltWorkerFactory
    @Inject lateinit var syncScheduler: SyncScheduler
    @Inject lateinit var widgets: org.sworrl.beaconfix.widget.WidgetUpdater
    @Inject lateinit var notifier: org.sworrl.beaconfix.widget.StatusNotifier
    @Inject lateinit var prefs: org.sworrl.beaconfix.data.Prefs
    @Inject lateinit var cache: org.sworrl.beaconfix.data.DesktopCache
    @Inject lateinit var help: org.sworrl.beaconfix.help.HelpRepository
    @Inject lateinit var helpAlerts: org.sworrl.beaconfix.notify.HelpAlerts
    @Inject lateinit var wifiMonitor: org.sworrl.beaconfix.wifi.CurrentNetworkMonitor
    @Inject lateinit var estimates: org.sworrl.beaconfix.estimate.EstimateRepository

    /** Lives as long as the process: pref mirrors, the help heads-up, launcher shortcuts. */
    private val appScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    override val workManagerConfiguration: Configuration
        get() = Configuration.Builder().setWorkerFactory(workerFactory).build()

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_COLLECTOR, getString(R.string.notif_channel_collector), NotificationManager.IMPORTANCE_LOW)
                .apply { description = getString(R.string.notif_channel_collector_desc) }
        )
        // osmdroid: identify ourselves to tile servers (OSM's policy) and cache under our own dir.
        OsmConfig.getInstance().apply {
            userAgentValue = "BeaconFix-Android/${BuildConfig.VERSION_NAME} (+https://github.com/sworrl/beaconfix)"
            osmdroidBasePath = getExternalFilesDir(null) ?: filesDir
            osmdroidTileCache = java.io.File(osmdroidBasePath, "tiles")
        }
        notifier.ensureChannel()
        syncScheduler.ensurePeriodic()
        widgets.ensurePeriodic()
        widgets.touch("start")
        kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.Dispatchers.Main).launch { org.sworrl.beaconfix.collector.CollectorService.ensure(this@BeaconFixApp, prefs) }
        org.sworrl.beaconfix.data.DevFlags.bind(prefs, appScope); org.sworrl.beaconfix.ui.Units.bind(prefs, appScope, cache)
        helpAlerts.ensureChannel(this); helpAlerts.start(appScope)
        org.sworrl.beaconfix.quick.Shortcuts.install(this, help, appScope)
        wifiMonitor.start()
        // a new estimator generation (1.5: graded fits) recomputes every stored estimate once, in the background
        appScope.launch {
            if (prefs.estimatorVersion.first() < org.sworrl.beaconfix.estimate.Estimator.VERSION)
                runCatching { estimates.refitAll(announce = false) }.onSuccess { prefs.setEstimatorVersion(org.sworrl.beaconfix.estimate.Estimator.VERSION) }
        }
    }

    companion object { const val CHANNEL_COLLECTOR = "collector" }
}
