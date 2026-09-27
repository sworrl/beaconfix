package org.sworrl.beaconfix.sync

import android.content.Context
import androidx.hilt.work.HiltWorker
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.ExistingWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import dagger.assisted.Assisted
import dagger.assisted.AssistedInject
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.first
import org.sworrl.beaconfix.data.Prefs
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton

@HiltWorker
class SyncWorker @AssistedInject constructor(
    @Assisted ctx: Context, @Assisted params: WorkerParameters,
    private val sync: SyncRepository, private val prefs: Prefs,
) : CoroutineWorker(ctx, params) {
    override suspend fun doWork(): Result {
        if (!prefs.autoSync.first() && !inputData.getBoolean("manual", false)) return Result.success()
        val reports = sync.syncAll()
        return if (reports.all { it.ok } || reports.isEmpty()) Result.success() else Result.retry()
    }
}

@Singleton
class SyncScheduler @Inject constructor(@ApplicationContext private val ctx: Context) {
    fun ensurePeriodic(unmeteredOnly: Boolean = false) {
        val c = Constraints.Builder().setRequiredNetworkType(if (unmeteredOnly) NetworkType.UNMETERED else NetworkType.CONNECTED).build()
        val req = PeriodicWorkRequestBuilder<SyncWorker>(15, TimeUnit.MINUTES).setConstraints(c).build()
        WorkManager.getInstance(ctx).enqueueUniquePeriodicWork("beaconfix-sync", ExistingPeriodicWorkPolicy.UPDATE, req)
    }
    fun syncNow() {
        val req = OneTimeWorkRequestBuilder<SyncWorker>().setInputData(androidx.work.workDataOf("manual" to true)).build()
        WorkManager.getInstance(ctx).enqueueUniqueWork("beaconfix-sync-now", ExistingWorkPolicy.REPLACE, req)
    }
}
