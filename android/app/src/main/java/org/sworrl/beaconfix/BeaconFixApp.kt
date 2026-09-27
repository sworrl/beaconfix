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

@HiltAndroidApp
class BeaconFixApp : Application(), Configuration.Provider {
    @Inject lateinit var workerFactory: HiltWorkerFactory
    @Inject lateinit var syncScheduler: SyncScheduler

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
        syncScheduler.ensurePeriodic()
    }

    companion object { const val CHANNEL_COLLECTOR = "collector" }
}
