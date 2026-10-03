package org.sworrl.beaconfix.alpr

import android.app.PendingIntent
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.os.Build
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.R

/** Quick Settings toggle for the ALPR dash cam. Off → stops the service; on → starts it through [AlprLaunchActivity]. */
class AlprTileService : TileService() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var watch: Job? = null

    override fun onStartListening() {
        super.onStartListening()
        watch?.cancel()
        watch = scope.launch { alprEntry().alprStatus().state.collect { render(it) } }
    }

    override fun onStopListening() { watch?.cancel(); watch = null; super.onStopListening() }

    @android.annotation.SuppressLint("StartActivityAndCollapseDeprecated")   // the Intent overload is only used below API 34
    override fun onClick() {
        super.onClick()
        val e = alprEntry()
        if (e.alprStatus().state.value.running) {
            e.alprSettings().update { it.copy(enabled = false) }
            DashCamService.stop(applicationContext)
            return
        }
        val i = Intent(this, AlprLaunchActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (Build.VERSION.SDK_INT >= 34) startActivityAndCollapse(PendingIntent.getActivity(this, 7330, i, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
        else @Suppress("DEPRECATION") startActivityAndCollapse(i)
    }

    override fun onDestroy() { scope.cancel(); super.onDestroy() }

    private fun render(s: AlprState) {
        val tile = qsTile ?: return
        tile.label = getString(R.string.alpr_tile_label)
        if (Build.VERSION.SDK_INT >= 29) tile.subtitle = when {
            !s.running -> "Off"
            s.paused != null -> "Paused"
            else -> "%.1f fps".format(s.fps)
        }
        tile.state = if (s.running) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
        tile.updateTile()
    }

    companion object {
        fun requestUpdate(ctx: Context) {
            runCatching { requestListeningState(ctx.applicationContext, ComponentName(ctx.applicationContext, AlprTileService::class.java)) }
        }
    }
}
