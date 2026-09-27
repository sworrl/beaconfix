package org.sworrl.beaconfix.ui.screens

import android.appwidget.AppWidgetHost
import android.appwidget.AppWidgetHostView
import android.appwidget.AppWidgetManager
import android.content.ComponentName
import android.content.Context
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.RemoteViews
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.foundation.layout.Row
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.widget.BeaconsWidgetReceiver
import org.sworrl.beaconfix.widget.LocationWidgetReceiver
import org.sworrl.beaconfix.widget.MapWidgetReceiver
import org.sworrl.beaconfix.widget.SyncWidgetReceiver
import org.sworrl.beaconfix.widget.WidgetPinner

private data class Gallery(val label: String, val receiver: Class<*>, val preview: Int, val heightDp: Int)

/**
 * Live previews of the four home-screen widgets. When this package may bind widgets (a launcher grants that; for
 * testing `adb shell cmd appwidget grantbind --package org.sworrl.beaconfix`) the real Glance content is hosted;
 * otherwise the static preview layouts are shown. Each card has a "Add to home screen" button.
 */
@Composable
fun WidgetGalleryScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    val host = remember { AppWidgetHost(ctx.applicationContext, HOST_ID) }
    DisposableEffect(Unit) { host.startListening(); onDispose { host.stopListening() } }
    val items = listOf(
        Gallery("Location", LocationWidgetReceiver::class.java, R.layout.widget_preview_location, 150),
        Gallery("Beacons", BeaconsWidgetReceiver::class.java, R.layout.widget_preview_beacons, 190),
        Gallery("Sync", SyncWidgetReceiver::class.java, R.layout.widget_preview_sync, 130),
        Gallery("Map", MapWidgetReceiver::class.java, R.layout.widget_preview_map, 260),
    )
    val canPin = WidgetPinner.supported(ctx)
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
        Row(Modifier.padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) { TextButton(onClick = onBack) { Text("‹ Back") }; Text("Widgets", style = MaterialTheme.typography.titleLarge) }
        Text("They refresh every 15 minutes and right after a scan, a sync, or a collector change. Resize them on the home screen for more detail.", color = Slate, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(horizontal = 16.dp))
        for (g in items) InfoCard(g.label) {
            AndroidView(modifier = Modifier.fillMaxWidth().height(g.heightDp.dp), factory = { c -> hostOrPreview(c, host, g) })
            OutlinedButton(enabled = canPin, onClick = { WidgetPinner.pin(ctx, g.receiver) }) { Text(if (canPin) "Add to home screen" else "Long-press the home screen → Widgets → BeaconFix") }
        }
    }
}

private const val HOST_ID = 0xBEAC

private fun hostOrPreview(c: Context, host: AppWidgetHost, g: Gallery): ViewGroup {
    val frame = FrameLayout(c)
    val mgr = AppWidgetManager.getInstance(c)
    val id = host.allocateAppWidgetId()
    val cn = ComponentName(c, g.receiver)
    val bound = runCatching { mgr.bindAppWidgetIdIfAllowed(id, cn) }.getOrDefault(false)
    if (bound) {
        val info = mgr.getAppWidgetInfo(id)
        val v: AppWidgetHostView = host.createView(c, id, info)
        val d = c.resources.displayMetrics.density
        v.updateAppWidgetSize(null, (info.minWidth / d).toInt(), (info.minHeight / d).toInt(), 400, g.heightDp)
        frame.addView(v, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    } else {
        host.deleteAppWidgetId(id)
        frame.addView(RemoteViews(c.packageName, g.preview).apply(c, frame), FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    }
    return frame
}
