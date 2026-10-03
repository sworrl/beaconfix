package org.sworrl.beaconfix.widget

import android.content.Context
import android.graphics.BitmapFactory
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.glance.GlanceId
import androidx.glance.GlanceModifier
import androidx.glance.GlanceTheme
import androidx.glance.Image
import androidx.glance.ImageProvider
import androidx.glance.LocalContext
import androidx.glance.LocalSize
import androidx.glance.action.ActionParameters
import androidx.glance.action.actionStartActivity
import androidx.glance.action.clickable
import androidx.glance.appwidget.GlanceAppWidget
import androidx.glance.appwidget.GlanceAppWidgetReceiver
import androidx.glance.appwidget.SizeMode
import androidx.glance.appwidget.action.ActionCallback
import androidx.glance.appwidget.action.actionRunCallback
import androidx.glance.appwidget.cornerRadius
import androidx.glance.appwidget.provideContent
import androidx.glance.background
import androidx.glance.layout.Alignment
import androidx.glance.layout.Box
import androidx.glance.layout.Column
import androidx.glance.layout.ContentScale
import androidx.glance.layout.Row
import androidx.glance.layout.Spacer
import androidx.glance.layout.fillMaxSize
import androidx.glance.layout.fillMaxWidth
import androidx.glance.layout.padding
import androidx.glance.layout.size
import androidx.glance.layout.width
import androidx.glance.text.FontWeight
import androidx.glance.text.Text
import androidx.glance.text.TextStyle
import androidx.glance.unit.ColorProvider
import dagger.hilt.android.EntryPointAccessors
import kotlinx.coroutines.flow.first
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.collector.CollectorService

// ── shared bits ──────────────────────────────────────────────────────────────
private fun entry(ctx: Context) = EntryPointAccessors.fromApplication(ctx, WidgetEntryPoint::class.java)
private suspend fun loadState(ctx: Context): WidgetState = entry(ctx).updater().state.first()

private val Red = Color(0xFFFF4D4D); private val Orange = Color(0xFFFF9F43); private val Gold = Color(0xFFFFD166)
private val Green = Color(0xFF6CFF8A); private val Cyan = Color(0xFF35D6FF); private val Magenta = Color(0xFFFF4FD8); private val Slate = Color(0xFF9FB0C8)

private fun gradeColor(sec: String) = when (sec) { "open", "wep", "wpa1", "wpa2-tkip" -> Red; "wpa2" -> Orange; "wpa3", "wpa3-eap192" -> Green; "" -> Slate; else -> Gold }
private fun gradeGlyph(sec: String) = when (sec) { "open", "wep", "wpa1", "wpa2-tkip" -> "☠"; "wpa2" -> "⚠"; "wpa3", "wpa3-eap192" -> "🛡"; "" -> "?"; else -> "◐" }
private fun worstColor(w: String) = when (w) { "critical" -> Red; "weak" -> Orange; "strong" -> Green; "ok" -> Gold; else -> Slate }
private fun ago(ms: Long): String { if (ms <= 0) return "never"; val s = (System.currentTimeMillis() - ms) / 1000; return when { s < 60 -> "just now"; s < 3600 -> "${s / 60} min ago"; s < 86400 -> "${s / 3600} h ago"; else -> "${s / 86400} d ago" } }
private fun acc(m: Double) = if (m < 0) "" else if (m >= 1000) String.format(java.util.Locale.US, "±%.1f km", m / 1000) else "±${m.toInt()} m"

@Composable private fun title(text: String, big: Boolean = false) = Text(text, style = TextStyle(color = GlanceTheme.colors.onSurface, fontSize = if (big) 16.sp else 14.sp, fontWeight = FontWeight.Bold), maxLines = 1)
@Composable private fun body(text: String, color: Color? = null, size: Int = 12) = Text(text, style = TextStyle(color = color?.let { ColorProvider(it) } ?: GlanceTheme.colors.onSurfaceVariant, fontSize = size.sp), maxLines = 1)
@Composable private fun chip(text: String, color: Color) = Box(GlanceModifier.background(ColorProvider(color.copy(alpha = 0.22f))).cornerRadius(8.dp).padding(horizontal = 6.dp, vertical = 2.dp)) { Text(text, style = TextStyle(color = ColorProvider(color), fontSize = 10.sp, fontWeight = FontWeight.Bold)) }
@Composable private fun card(content: @Composable androidx.glance.layout.ColumnScope.() -> Unit) = Column(GlanceModifier.fillMaxSize().background(GlanceTheme.colors.widgetBackground).cornerRadius(16.dp).padding(12.dp).clickable(actionStartActivity<MainActivity>())) { content() }
@Composable private fun footer(st: WidgetState) { if (st.identityName.isNotEmpty()) body("${st.identityName} · ${ago(st.updated)}", size = 10) else body("BeaconFix · ${ago(st.updated)}", size = 10) }

// ── actions ──────────────────────────────────────────────────────────────────
class RefreshAction : ActionCallback { override suspend fun onAction(context: Context, glanceId: GlanceId, parameters: ActionParameters) { runCatching { entry(context).updater().refresh() } } }
class SyncNowAction : ActionCallback { override suspend fun onAction(context: Context, glanceId: GlanceId, parameters: ActionParameters) { entry(context).syncScheduler().syncNow(); entry(context).updater().touch("sync-widget") } }
class ToggleCollectorAction : ActionCallback {
    override suspend fun onAction(context: Context, glanceId: GlanceId, parameters: ActionParameters) {
        val prefs = entry(context).prefs(); val on = !prefs.collectorOn.first()
        prefs.setCollectorOn(on); CollectorService.ensure(context, prefs)
        entry(context).updater().refresh(renderMap = false)
    }
}

// ── 1. Location ──────────────────────────────────────────────────────────────
class LocationWidget : GlanceAppWidget() {
    override val sizeMode = SizeMode.Responsive(setOf(DpSize(110.dp, 48.dp), DpSize(180.dp, 48.dp), DpSize(250.dp, 110.dp), DpSize(250.dp, 160.dp)))
    override suspend fun provideGlance(context: Context, id: GlanceId) { val st = loadState(context); provideContent { GlanceTheme { Content(st) } } }
    @Composable private fun Content(st: WidgetState) {
        val size = LocalSize.current; val tall = size.height >= 100.dp; val wide = size.width >= 180.dp
        card {
            Row(GlanceModifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Box(GlanceModifier.defaultWeight()) { title(st.place.ifEmpty { "No fix yet" }, big = tall) }
                if (st.source.isNotEmpty()) chip(when (st.source) { "gps" -> "GPS"; "wifi" -> "WI-FI"; "desktop" -> "DESKTOP"; else -> st.source.uppercase() }, when (st.source) { "gps" -> Green; "wifi" -> Cyan; else -> Gold })
            }
            if (wide || tall) body(listOfNotNull(if (st.lat != 0.0 || st.lon != 0.0) String.format(java.util.Locale.US, "%.5f, %.5f", st.lat, st.lon) else null, acc(st.acc).ifEmpty { null }).joinToString(" · ").ifEmpty { "Turn the collector on or pair a desktop" })
            if (st.atHome != null) body(if (st.atHome) "🏠 at home" else "🧭 ${st.awayText}", color = if (st.atHome) Magenta else Gold)
            if (st.nearestDevice.isNotEmpty() && (wide || tall)) body("💻 ${st.nearestDevice}", color = Gold)
            if (tall) {
                Spacer(GlanceModifier.defaultWeight())
                Row(GlanceModifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Box(GlanceModifier.defaultWeight()) { body("fix ${ago(st.fixTime)} · ${st.inRange} beacons", size = 11) }
                    Box(GlanceModifier.background(GlanceTheme.colors.primaryContainer).cornerRadius(10.dp).padding(horizontal = 8.dp, vertical = 3.dp).clickable(actionRunCallback<RefreshAction>())) { Text("↻ refresh", style = TextStyle(color = GlanceTheme.colors.onPrimaryContainer, fontSize = 11.sp)) }
                }
                footer(st)
            }
        }
    }
}
class LocationWidgetReceiver : GlanceAppWidgetReceiver() { override val glanceAppWidget = LocationWidget(); override fun onEnabled(context: Context) { super.onEnabled(context); entry(context).updater().ensurePeriodic(); entry(context).updater().refreshNow() } }

// ── 2. Beacons ───────────────────────────────────────────────────────────────
class BeaconsWidget : GlanceAppWidget() {
    override val sizeMode = SizeMode.Responsive(setOf(DpSize(110.dp, 48.dp), DpSize(250.dp, 110.dp), DpSize(250.dp, 180.dp)))
    override suspend fun provideGlance(context: Context, id: GlanceId) { val st = loadState(context); provideContent { GlanceTheme { Content(st) } } }
    @Composable private fun Content(st: WidgetState) {
        val size = LocalSize.current; val tall = size.height >= 100.dp; val taller = size.height >= 170.dp
        card {
            Row(GlanceModifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Box(GlanceModifier.defaultWeight()) { title("${st.inRange} beacons in range", big = tall) }
                if (st.worst.isNotEmpty()) chip(when (st.worst) { "critical" -> "☠ INSECURE"; "weak" -> "⚠ WEAK"; "strong" -> "🛡 STRONG"; else -> "◐ OK" }, worstColor(st.worst))
            }
            val parts = ArrayList<String>()
            if (st.open > 0) parts += "${st.open} open"; if (st.wep > 0) parts += "${st.wep} WEP"; if (st.wpa1 > 0) parts += "${st.wpa1} WPA1"; if (st.tkip > 0) parts += "${st.tkip} TKIP"
            if (st.wpa2 > 0) parts += "${st.wpa2} WPA2"; if (st.wpa3 > 0) parts += "${st.wpa3} WPA3"; if (st.other > 0) parts += "${st.other} other"
            body(parts.joinToString(" · ").ifEmpty { if (st.scanTime == 0L) "no scan yet — turn the collector on" else "nothing heard" }, color = worstColor(st.worst).takeIf { st.worst == "critical" })
            if (tall) {
                val n = if (taller) 3 else 2
                for (b in st.top.take(n)) Row(GlanceModifier.fillMaxWidth().padding(top = 3.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(gradeGlyph(b.security), style = TextStyle(color = ColorProvider(gradeColor(b.security)), fontSize = 12.sp))
                    Spacer(GlanceModifier.width(6.dp))
                    Box(GlanceModifier.defaultWeight()) { Text(b.ssid.ifEmpty { "(hidden)" }, style = TextStyle(color = if (b.home) ColorProvider(Magenta) else GlanceTheme.colors.onSurface, fontSize = 12.sp), maxLines = 1) }
                    body(if (b.dbm > -100) "${b.dbm} dBm" else "", size = 11)
                }
                Spacer(GlanceModifier.defaultWeight())
                Row(GlanceModifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) { Box(GlanceModifier.defaultWeight()) { body("scan ${ago(st.scanTime)} · ${st.apsKnown} known, ${st.apsPositioned} placed", size = 10) }; footerInline(st) }
            }
        }
    }
    @Composable private fun footerInline(st: WidgetState) { if (st.identityName.isNotEmpty()) body(st.identityName, size = 10) }
}
class BeaconsWidgetReceiver : GlanceAppWidgetReceiver() { override val glanceAppWidget = BeaconsWidget(); override fun onEnabled(context: Context) { super.onEnabled(context); entry(context).updater().ensurePeriodic(); entry(context).updater().refreshNow() } }

// ── 3. Sync ──────────────────────────────────────────────────────────────────
class SyncWidget : GlanceAppWidget() {
    override val sizeMode = SizeMode.Responsive(setOf(DpSize(110.dp, 48.dp), DpSize(250.dp, 110.dp)))
    override suspend fun provideGlance(context: Context, id: GlanceId) { val st = loadState(context); provideContent { GlanceTheme { Content(st) } } }
    @Composable private fun Content(st: WidgetState) {
        val tall = LocalSize.current.height >= 100.dp
        card {
            Row(GlanceModifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Box(GlanceModifier.defaultWeight()) { title(if (st.desktopPaired) st.desktopName.ifEmpty { "Desktop" } else "No desktop paired", big = tall) }
                chip(if (st.collectorOn) "● COLLECTING" else "○ IDLE", if (st.collectorOn) Green else Slate)
            }
            body(if (st.desktopPaired) "synced ${ago(st.lastSync)} · ${st.unsynced} waiting" else "${st.unsynced} observations waiting", color = if (st.unsynced > 0) Gold else null)
            if (tall) {
                Spacer(GlanceModifier.defaultWeight())
                Row(GlanceModifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Box(GlanceModifier.background(GlanceTheme.colors.primaryContainer).cornerRadius(10.dp).padding(horizontal = 10.dp, vertical = 5.dp).clickable(actionRunCallback<SyncNowAction>())) { Text("⇅ Sync now", style = TextStyle(color = GlanceTheme.colors.onPrimaryContainer, fontSize = 12.sp, fontWeight = FontWeight.Bold)) }
                    Spacer(GlanceModifier.width(8.dp))
                    Box(GlanceModifier.background(ColorProvider((if (st.collectorOn) Green else Slate).copy(alpha = 0.22f))).cornerRadius(10.dp).padding(horizontal = 10.dp, vertical = 5.dp).clickable(actionRunCallback<ToggleCollectorAction>())) { Text(if (st.collectorOn) "■ Stop collecting" else "▶ Collect", style = TextStyle(color = ColorProvider(if (st.collectorOn) Green else Slate), fontSize = 12.sp, fontWeight = FontWeight.Bold)) }
                    Spacer(GlanceModifier.defaultWeight())
                    footer(st)
                }
            }
        }
    }
}
class SyncWidgetReceiver : GlanceAppWidgetReceiver() { override val glanceAppWidget = SyncWidget(); override fun onEnabled(context: Context) { super.onEnabled(context); entry(context).updater().ensurePeriodic(); entry(context).updater().refreshNow() } }

// ── 4. Map ───────────────────────────────────────────────────────────────────
class MapWidget : GlanceAppWidget() {
    override val sizeMode = SizeMode.Responsive(setOf(DpSize(180.dp, 110.dp), DpSize(250.dp, 180.dp), DpSize(320.dp, 260.dp)))
    override suspend fun provideGlance(context: Context, id: GlanceId) {
        val st = loadState(context)
        val bmp = st.mapPath.takeIf { it.isNotEmpty() }?.let { runCatching { BitmapFactory.decodeFile(it) }.getOrNull() }
        provideContent { GlanceTheme {
            Box(GlanceModifier.fillMaxSize().background(GlanceTheme.colors.widgetBackground).cornerRadius(16.dp).clickable(actionStartActivity<MainActivity>())) {
                if (bmp != null) Image(ImageProvider(bmp), contentDescription = "Map around the current position", modifier = GlanceModifier.fillMaxSize(), contentScale = ContentScale.Crop)
                else Box(GlanceModifier.fillMaxSize(), contentAlignment = Alignment.Center) { Column(horizontalAlignment = Alignment.CenterHorizontally) { Image(ImageProvider(R.drawable.ic_beaconfix), contentDescription = null, modifier = GlanceModifier.size(28.dp)); body(if (st.lat == 0.0 && st.lon == 0.0) "No fix yet" else "Rendering the map…") } }
                Box(GlanceModifier.fillMaxWidth().padding(8.dp), contentAlignment = Alignment.TopStart) {
                    Row(GlanceModifier.background(ColorProvider(Color(0xCC0B101A))).cornerRadius(8.dp).padding(horizontal = 8.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(st.place.ifEmpty { "BeaconFix" }, style = TextStyle(color = ColorProvider(Color.White), fontSize = 12.sp, fontWeight = FontWeight.Bold), maxLines = 1)
                        if (st.acc >= 0) { Spacer(GlanceModifier.width(6.dp)); Text(acc(st.acc), style = TextStyle(color = ColorProvider(Slate), fontSize = 10.sp)) }
                        Spacer(GlanceModifier.width(6.dp)); Text("${st.apsPositioned} placed", style = TextStyle(color = ColorProvider(Cyan), fontSize = 10.sp))
                        if (st.identityName.isNotEmpty()) { Spacer(GlanceModifier.width(6.dp)); Text(st.identityName, style = TextStyle(color = ColorProvider(Gold), fontSize = 10.sp)) }
                    }
                }
                Box(GlanceModifier.fillMaxSize().padding(8.dp), contentAlignment = Alignment.BottomEnd) {
                    Box(GlanceModifier.background(ColorProvider(Color(0xCC0B101A))).cornerRadius(8.dp).padding(horizontal = 8.dp, vertical = 4.dp).clickable(actionRunCallback<RefreshAction>())) { Text("↻ ${ago(st.mapTime)}", style = TextStyle(color = ColorProvider(Color.White), fontSize = 10.sp)) }
                }
            }
        } }
    }
}
class MapWidgetReceiver : GlanceAppWidgetReceiver() { override val glanceAppWidget = MapWidget(); override fun onEnabled(context: Context) { super.onEnabled(context); entry(context).updater().ensurePeriodic(); entry(context).updater().refreshNow() } }

/** Pin a widget from inside the app (launchers that support it show a confirmation sheet). */
object WidgetPinner {
    fun supported(ctx: Context): Boolean = android.appwidget.AppWidgetManager.getInstance(ctx).isRequestPinAppWidgetSupported
    fun pin(ctx: Context, receiver: Class<*>): Boolean = runCatching { android.appwidget.AppWidgetManager.getInstance(ctx).requestPinAppWidget(android.content.ComponentName(ctx, receiver), null, null) }.getOrDefault(false)
}
