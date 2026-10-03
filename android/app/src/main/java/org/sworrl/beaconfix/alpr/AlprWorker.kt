package org.sworrl.beaconfix.alpr

import android.content.Context
import androidx.hilt.work.HiltWorker
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import dagger.assisted.Assisted
import dagger.assisted.AssistedInject
import java.util.concurrent.TimeUnit

/**
 * Every 15 min with a network: refresh the hotlist cache and send what the dash cam left on disk (after it stopped, or
 * while the RV was out of reach). Does nothing when not paired.
 */
@HiltWorker
class AlprWorker @AssistedInject constructor(
    @Assisted ctx: Context, @Assisted params: WorkerParameters,
    private val link: FalconLink, private val hotlist: HotlistStore, private val uplink: AlprUplink,
) : CoroutineWorker(ctx, params) {
    override suspend fun doWork(): Result {
        val p = link.pairing.value
        if (!p.paired || p.revoked) return Result.success()
        runCatching { hotlist.refresh() }
        runCatching { uplink.drainOnce() }
        return Result.success()
    }

    companion object {
        private const val NAME = "alpr-periodic"
        fun schedule(ctx: Context) {
            val req = PeriodicWorkRequestBuilder<AlprWorker>(15, TimeUnit.MINUTES)
                .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build()).build()
            WorkManager.getInstance(ctx).enqueueUniquePeriodicWork(NAME, ExistingPeriodicWorkPolicy.KEEP, req)
        }
        fun cancel(ctx: Context) { WorkManager.getInstance(ctx).cancelUniqueWork(NAME) }
    }
}
