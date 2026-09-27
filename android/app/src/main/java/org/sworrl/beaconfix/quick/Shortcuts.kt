package org.sworrl.beaconfix.quick

import android.content.Context
import android.content.Intent
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat
import dagger.hilt.android.EntryPointAccessors
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.help.HelpKind
import org.sworrl.beaconfix.help.HelpPlace
import org.sworrl.beaconfix.help.HelpRepository
import org.sworrl.beaconfix.help.HelpSnapshot
import org.sworrl.beaconfix.widget.HelpStore
import org.sworrl.beaconfix.widget.WidgetEntryPoint

/**
 * Launcher shortcuts (long-press the app icon): Help now · Kids ER: <name> · Share my location · Find the RV.
 * Dynamic only (no static shortcuts.xml: the debug build's `.debug` package would break its explicit intents).
 * Each is an explicit MainActivity intent with an `action` extra; the list is re-published only when its content
 * changes (the children's ER name), so there is no churn against the launcher's rate limit.
 */
object Shortcuts {
    const val HELP = "help"
    const val KIDS_ER = "kids_er"
    const val SHARE = "share_location"
    const val RV = "find_rv"

    /** One shortcut, independent of Android (the content the launcher shows). */
    data class Spec(val id: String, val shortLabel: String, val longLabel: String, val action: String, val icon: Int)

    private var job: Job? = null

    /** Publish now and again whenever the nearest children's ER changes, for the life of [scope]. */
    @Synchronized
    fun install(ctx: Context, help: HelpRepository, scope: CoroutineScope) {
        val app = ctx.applicationContext
        job?.cancel()
        job = scope.launch {
            help.snapshot
                .map { live -> if (live.fetchedAt > 0) live else runCatching { HelpStore.load(app) }.getOrDefault(HelpSnapshot()) }
                .map { specs(app, it.first(HelpKind.PEDS_ER)) }
                .distinctUntilChanged()
                .collect { runCatching { publish(app, it) } }
        }
    }

    fun specs(ctx: Context, kids: HelpPlace?): List<Spec> = buildList {
        add(Spec(HELP, ctx.getString(R.string.shortcut_help_short), ctx.getString(R.string.shortcut_help_long), "help", R.drawable.ic_shortcut_help))
        if (kids != null) add(kidsSpec(ctx, kids))
        add(Spec(SHARE, ctx.getString(R.string.shortcut_share_short), ctx.getString(R.string.shortcut_share_long), "share_location", R.drawable.ic_shortcut_share))
        add(Spec(RV, ctx.getString(R.string.shortcut_rv_short), ctx.getString(R.string.shortcut_rv_long), "find_rv", R.drawable.ic_shortcut_rv))
    }

    private fun kidsSpec(ctx: Context, p: HelpPlace): Spec {
        val prefix = if (p.tier == 2) ctx.getString(R.string.shortcut_kids_hospital_prefix) else ctx.getString(R.string.shortcut_kids_er_long)
        val name = p.name.ifBlank { ctx.getString(R.string.shortcut_kids_er_short) }
        return Spec(KIDS_ER, ctx.getString(R.string.shortcut_kids_er_short), "$prefix: $name".take(60), "help_peds", R.drawable.ic_shortcut_kids_er)
    }

    private fun info(ctx: Context, s: Spec, rank: Int = 0): ShortcutInfoCompat =
        ShortcutInfoCompat.Builder(ctx, s.id)
            .setShortLabel(s.shortLabel).setLongLabel(s.longLabel).setRank(rank)
            .setIcon(IconCompat.createWithResource(ctx, s.icon))
            .setIntent(Intent(Intent.ACTION_MAIN).setClass(ctx, MainActivity::class.java).putExtra("action", s.action).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP))
            .build()

    private fun publish(ctx: Context, specs: List<Spec>) {
        val max = ShortcutManagerCompat.getMaxShortcutCountPerActivity(ctx).takeIf { it > 0 } ?: 4
        val wanted = specs.take(max)
        val current = ShortcutManagerCompat.getDynamicShortcuts(ctx).associate { it.id to Triple(it.shortLabel.toString(), it.longLabel?.toString(), it.rank) }
        val next = wanted.withIndex().associate { (i, w) -> w.id to Triple(w.shortLabel, w.longLabel, i) }
        if (current == next) return
        ShortcutManagerCompat.setDynamicShortcuts(ctx, wanted.mapIndexed { i, w -> info(ctx, w, i) })
    }

    /** "Pin the children's ER to the home screen": a pinned `kids_er` shortcut (disabled until one is known). */
    @Composable
    fun PinKidsErButton() {
        val ctx = LocalContext.current
        val help = remember { EntryPointAccessors.fromApplication(ctx.applicationContext, WidgetEntryPoint::class.java).help() }
        val live by help.snapshot.collectAsState()
        val stored by produceState(HelpSnapshot()) { value = runCatching { HelpStore.load(ctx) }.getOrDefault(HelpSnapshot()) }
        val kids = (if (live.fetchedAt >= stored.fetchedAt) live else stored).first(HelpKind.PEDS_ER)
        val supported = remember { ShortcutManagerCompat.isRequestPinShortcutSupported(ctx) }
        OutlinedButton(enabled = supported && kids != null, onClick = {
            kids?.let { runCatching { ShortcutManagerCompat.requestPinShortcut(ctx, info(ctx, kidsSpec(ctx, it)), null) } }
        }) {
            Text(when {
                !supported -> ctx.getString(R.string.shortcut_pin_unsupported)
                kids == null -> ctx.getString(R.string.shortcut_pin_none)
                else -> ctx.getString(R.string.shortcut_pin_kids_er)
            })
        }
    }
}

/** Top-level alias so screens can call `PinKidsErButton()` directly. */
@Composable
fun PinKidsErButton() = Shortcuts.PinKidsErButton()
