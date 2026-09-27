package org.sworrl.beaconfix.tile

import android.content.ComponentName
import android.content.Context
import android.os.Build
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import dagger.hilt.android.EntryPointAccessors
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.collector.CollectorService
import org.sworrl.beaconfix.widget.WidgetEntryPoint

/**
 * Quick Settings toggle for background collection — the same switch as Settings → Collector and the Sync widget.
 * A tap unlocks the phone first, flips `collectorOn` and makes sure the presence service is up.
 */
class CollectorTileService : TileService() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private fun entry() = EntryPointAccessors.fromApplication(applicationContext, WidgetEntryPoint::class.java)

    override fun onStartListening() {
        super.onStartListening()
        scope.launch { render(runCatching { entry().prefs().collectorOn.first() }.getOrDefault(false)) }
    }

    override fun onClick() {
        super.onClick()
        val run = Runnable { scope.launch { toggle() } }
        if (isLocked) unlockAndRun(run) else run.run()
    }

    override fun onDestroy() { scope.cancel(); super.onDestroy() }

    private suspend fun toggle() {
        val e = entry(); val prefs = e.prefs()
        val on = !prefs.collectorOn.first()
        prefs.setCollectorOn(on)
        CollectorService.ensure(applicationContext, prefs)
        render(on)
        runCatching { e.updater().touch("collector-tile") }
    }

    private fun render(on: Boolean) {
        val tile = qsTile ?: return
        val m = TileModel.collector(on)
        tile.label = getString(R.string.tile_collector_label)
        if (Build.VERSION.SDK_INT >= 29) tile.subtitle = m.subtitle
        tile.state = if (m.active) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
        tile.updateTile()
    }

    companion object {
        fun requestUpdate(ctx: Context) {
            runCatching { requestListeningState(ctx.applicationContext, ComponentName(ctx.applicationContext, CollectorTileService::class.java)) }
        }
    }
}
