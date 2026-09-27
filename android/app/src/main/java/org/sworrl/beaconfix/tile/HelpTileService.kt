package org.sworrl.beaconfix.tile

import android.app.PendingIntent
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.os.Build
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import dagger.hilt.android.EntryPointAccessors
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.help.HelpSnapshot
import org.sworrl.beaconfix.widget.HelpStore
import org.sworrl.beaconfix.widget.WidgetEntryPoint

/**
 * Quick Settings "Nearest help": the subtitle says what is closest ("Kids ER 38 km · ~45 min", "ER 12 km", "saved");
 * a tap unlocks the phone first (Help never shows over the lock screen) and opens the Help screen.
 * ACTIVE_TILE mode: the system binds it only when [requestUpdate] is called (after every help refresh) or on a tap.
 */
class HelpTileService : TileService() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    override fun onStartListening() {
        super.onStartListening()
        scope.launch { render(load()) }
    }

    override fun onClick() {
        super.onClick()
        val run = Runnable { openHelp() }
        if (isLocked) unlockAndRun(run) else run.run()
    }

    override fun onDestroy() { scope.cancel(); super.onDestroy() }

    private suspend fun load(): HelpSnapshot = runCatching {
        EntryPointAccessors.fromApplication(applicationContext, WidgetEntryPoint::class.java).updater().helpNow()
    }.getOrElse { runCatching { HelpStore.load(applicationContext) }.getOrDefault(HelpSnapshot()) }

    private fun render(s: HelpSnapshot) {
        val tile = qsTile ?: return
        val m = TileModel.help(s)
        tile.label = getString(R.string.tile_help_label)
        if (Build.VERSION.SDK_INT >= 29) tile.subtitle = m.subtitle
        tile.contentDescription = m.description
        tile.state = if (m.active) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
        tile.updateTile()
    }

    private fun openHelp() {
        val intent = Intent(this, MainActivity::class.java).putExtra("action", "help").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (Build.VERSION.SDK_INT >= 34) {
            startActivityAndCollapse(PendingIntent.getActivity(this, REQ, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
        } else {
            @Suppress("DEPRECATION", "StartActivityAndCollapseDeprecated") startActivityAndCollapse(intent)
        }
    }

    companion object {
        private const val REQ = 4320
        /** Ask the system to bind the tile so it re-reads the help snapshot (no-op when the tile is not in the panel). */
        fun requestUpdate(ctx: Context) {
            runCatching { requestListeningState(ctx.applicationContext, ComponentName(ctx.applicationContext, HelpTileService::class.java)) }
        }
    }
}
