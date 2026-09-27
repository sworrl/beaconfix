package org.sworrl.beaconfix.widget

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.runtime.Composable
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import androidx.glance.GlanceId
import androidx.glance.GlanceModifier
import androidx.glance.GlanceTheme
import androidx.glance.LocalSize
import androidx.glance.action.Action
import androidx.glance.action.ActionParameters
import androidx.glance.action.actionParametersOf
import androidx.glance.action.actionStartActivity
import androidx.glance.action.clickable
import androidx.glance.appwidget.GlanceAppWidget
import androidx.glance.appwidget.GlanceAppWidgetReceiver
import androidx.glance.appwidget.SizeMode
import androidx.glance.appwidget.cornerRadius
import androidx.glance.appwidget.provideContent
import androidx.glance.background
import androidx.glance.layout.Alignment
import androidx.glance.layout.Box
import androidx.glance.layout.Column
import androidx.glance.layout.Row
import androidx.glance.layout.Spacer
import androidx.glance.layout.fillMaxSize
import androidx.glance.layout.fillMaxWidth
import androidx.glance.layout.height
import androidx.glance.layout.padding
import androidx.glance.layout.width
import androidx.glance.text.FontWeight
import androidx.glance.text.Text
import androidx.glance.text.TextStyle
import dagger.hilt.android.EntryPointAccessors
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.serialization.json.Json
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.help.HelpKind
import org.sworrl.beaconfix.help.HelpPlace
import org.sworrl.beaconfix.help.HelpSnapshot
import org.sworrl.beaconfix.tile.TileModel
import org.sworrl.beaconfix.ui.Emergency
import org.sworrl.beaconfix.ui.Intents
import org.sworrl.beaconfix.ui.metres
import androidx.glance.appwidget.action.actionStartActivity as actionStartIntent

// ── the last help answer, persisted so the widget, the tiles and the notification can show it without the app ──
private val Context.helpWidgetStore: DataStore<Preferences> by preferencesDataStore("beaconfix_help_widget")
private val HELP_KEY = stringPreferencesKey("snapshot")
private val helpJson = Json { ignoreUnknownKeys = true; encodeDefaults = false; explicitNulls = false }

/** The newest [HelpSnapshot] any quick-access surface has seen (written by [WidgetUpdater.helpNow]). */
object HelpStore {
    suspend fun load(ctx: Context): HelpSnapshot = ctx.applicationContext.helpWidgetStore.data
        .map { p -> p[HELP_KEY]?.let { runCatching { helpJson.decodeFromString(HelpSnapshot.serializer(), it) }.getOrNull() } ?: HelpSnapshot() }
        .first()
    suspend fun save(ctx: Context, s: HelpSnapshot) {
        ctx.applicationContext.helpWidgetStore.edit { it[HELP_KEY] = helpJson.encodeToString(HelpSnapshot.serializer(), s) }
    }
}

private fun helpEntry(ctx: Context) = EntryPointAccessors.fromApplication(ctx.applicationContext, WidgetEntryPoint::class.java)

private val ACTION_KEY = ActionParameters.Key<String>("action")

/**
 * The Help widget: the nearest children's ER at a glance, then (as it grows) a 911 pill, Call / Go, the nearest ER,
 * urgent care ("not an ER"), police, fire, the search note and how old the answer is. GlanceTheme colours only (the
 * 911 pill is `error`/`onError`), so it follows the wallpaper and dark mode. Tapping the body opens Help; the buttons
 * go straight to the dialer (ACTION_DIAL — nothing is dialled) or a map app.
 */
class HelpWidget : GlanceAppWidget() {
    override val sizeMode = SizeMode.Responsive(setOf(TINY, ROW, MEDIUM, TALL, LARGE))

    override suspend fun provideGlance(context: Context, id: GlanceId) {
        val hs = runCatching { helpEntry(context).updater().helpNow() }.getOrElse { runCatching { HelpStore.load(context) }.getOrDefault(HelpSnapshot()) }
        val now = System.currentTimeMillis()
        provideContent { GlanceTheme { Content(hs, now) } }
    }

    @Composable
    private fun Content(hs: HelpSnapshot, now: Long) {
        val size = LocalSize.current
        val level = when { size.height >= 170.dp -> 3; size.height >= 100.dp -> 2; size.width >= 180.dp -> 1; else -> 0 }
        val peds = hs.first(HelpKind.PEDS_ER)
        val er = hs.first(HelpKind.ER)
        val main = peds ?: er
        val openHelp = actionStartActivity<MainActivity>(actionParametersOf(ACTION_KEY to "help"))
        Column(
            GlanceModifier.fillMaxSize().background(GlanceTheme.colors.widgetBackground).cornerRadius(16.dp)
                .padding(horizontal = 10.dp, vertical = if (level == 0) 6.dp else 8.dp).clickable(openHelp),
            verticalAlignment = if (level <= 1) Alignment.CenterVertically else Alignment.Top,
        ) {
            if (level >= 2) {
                Row(GlanceModifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Box(GlanceModifier.defaultWeight()) { Title(if (main == null) "Nearest help" else (if (main === peds) "🧸 " else "") + main.name.ifBlank { "Unnamed" }) }
                    Spacer(GlanceModifier.width(6.dp))
                    EmergencyPill(hs)
                }
            }
            if (main == null) { Body(EMPTY, maxLines = if (level >= 2) 3 else 2); return@Column }
            when (level) {
                0 -> Title(TileModel.headline(hs))
                1 -> Row(GlanceModifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Column(GlanceModifier.defaultWeight()) {
                        Title((if (main === peds) "🧸 " else "") + main.name.ifBlank { "Unnamed" })
                        Body(distEta(main, hs, withEst = false))
                    }
                    if (main.phone.isNotBlank()) { Spacer(GlanceModifier.width(6.dp)); Pill("📞", dial(main.phone)) }
                }
                else -> {
                    // Glance allows at most 10 children per container (it silently drops the rest: the "saved … ago"
                    // line went missing), so the root holds groups: header, main block, extras, filler, age (≤ 6).
                    Column(GlanceModifier.fillMaxWidth()) {
                        TierLine(main)
                        Body(distEta(main, hs, withEst = true) + originText(hs))
                        Spacer(GlanceModifier.height(4.dp))
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            if (main.phone.isNotBlank()) { Pill("📞 Call", dial(main.phone)); Spacer(GlanceModifier.width(6.dp)) }
                            Pill("➜ Go", go(main))
                        }
                    }
                    Spacer(GlanceModifier.height(4.dp))
                    Column(GlanceModifier.fillMaxWidth()) {
                        if (peds != null) PlaceLine("ER", er, "No ER found nearby")
                        if (level >= 3) {
                            val urg = hs.first(HelpKind.PEDS_URGENT) ?: hs.first(HelpKind.URGENT)
                            if (urg != null) PlaceLine("Urgent care (not an ER)", urg, null)
                            PlaceLine("Police", hs.first(HelpKind.POLICE), null)
                            PlaceLine("Fire", hs.first(HelpKind.FIRE), null)
                            if (hs.pedsNote.isNotBlank()) Body(hs.pedsNote, maxLines = 2)
                        }
                    }
                    if (level >= 3) {
                        Spacer(GlanceModifier.defaultWeight())
                        Body((if (hs.stale) "saved " else "updated ") + ago(hs.fetchedAt, now), size = 10)
                    }
                }
            }
        }
    }

    @Composable private fun Title(text: String) =
        Text(text, style = TextStyle(color = GlanceTheme.colors.onSurface, fontSize = 13.sp, fontWeight = FontWeight.Bold), maxLines = 1)

    @Composable private fun Body(text: String, maxLines: Int = 1, size: Int = 11) =
        Text(text, style = TextStyle(color = GlanceTheme.colors.onSurfaceVariant, fontSize = size.sp), maxLines = maxLines)

    @Composable private fun TierLine(p: HelpPlace) {
        val t = TileModel.tierText(p)
        if (t.isNotEmpty()) Text(t, style = TextStyle(color = if (p.tier == 2 || p.notEr) GlanceTheme.colors.error else GlanceTheme.colors.primary, fontSize = 11.sp, fontWeight = FontWeight.Medium), maxLines = 1)
    }

    @Composable private fun PlaceLine(label: String, p: HelpPlace?, missing: String?) {
        if (p == null) { if (missing != null) Body("$label: $missing"); return }
        Body("$label: ${p.name.ifBlank { "unnamed" }}" + TileModel.distText(p).let { if (it.isEmpty()) "" else " · $it" })
    }

    @Composable private fun Pill(text: String, action: Action) =
        Box(GlanceModifier.background(GlanceTheme.colors.primaryContainer).cornerRadius(12.dp).padding(horizontal = 10.dp, vertical = 4.dp).clickable(action)) {
            Text(text, style = TextStyle(color = GlanceTheme.colors.onPrimaryContainer, fontSize = 12.sp, fontWeight = FontWeight.Bold), maxLines = 1)
        }

    @Composable private fun EmergencyPill(hs: HelpSnapshot) {
        val number = hs.number.ifBlank { Emergency.number(hs.countryCode.ifBlank { null }) }
        Box(GlanceModifier.background(GlanceTheme.colors.error).cornerRadius(12.dp).padding(horizontal = 10.dp, vertical = 4.dp).clickable(dial(number))) {
            Text("Call ${number.substringBefore('/').trim()}", style = TextStyle(color = GlanceTheme.colors.onError, fontSize = 12.sp, fontWeight = FontWeight.Bold), maxLines = 1)
        }
    }

    companion object {
        val TINY = DpSize(110.dp, 48.dp)
        val ROW = DpSize(180.dp, 48.dp)
        val MEDIUM = DpSize(250.dp, 110.dp)
        val TALL = DpSize(250.dp, 180.dp)
        val LARGE = DpSize(320.dp, 260.dp)
        const val EMPTY = "No help data yet — open BeaconFix with a desktop in reach"

        /** ACTION_DIAL: the dialer opens with the number filled in; the user presses call. */
        fun dial(number: String): Action = actionStartIntent(Intent(Intent.ACTION_DIAL, Uri.fromParts("tel", Intents.dialable(number), null)))
        fun go(p: HelpPlace): Action = actionStartIntent(Intent(Intent.ACTION_VIEW, Uri.parse(Intents.geoUri(p.lat, p.lon, p.name))))

        fun distEta(p: HelpPlace, hs: HelpSnapshot, withEst: Boolean): String {
            val eta = TileModel.eta(p.driveS).let { if (it.isNotEmpty() && withEst && p.driveEst) "$it (est.)" else it }
            return listOf(if (p.distM >= 0) metres(p.distM) else "", if (p.tier == 2 && !withEst) "call ahead" else eta, if (hs.stale && !withEst) "saved" else "")
                .filter { it.isNotBlank() }.joinToString(" · ")
        }
        fun originText(hs: HelpSnapshot): String = when (hs.origin) { "phone" -> " · from you"; "rv" -> " · from the RV"; else -> "" }
        fun ago(ms: Long, now: Long): String {
            if (ms <= 0) return "never"
            val s = (now - ms) / 1000
            return when { s < 60 -> "just now"; s < 3600 -> "${s / 60} min ago"; s < 86400 -> "${s / 3600} h ago"; else -> "${s / 86400} d ago" }
        }
    }
}

class HelpWidgetReceiver : GlanceAppWidgetReceiver() {
    override val glanceAppWidget: GlanceAppWidget = HelpWidget()
    override fun onEnabled(context: Context) {
        super.onEnabled(context)
        helpEntry(context).updater().ensurePeriodic(); helpEntry(context).updater().refreshNow()
    }
}
