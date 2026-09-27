package org.sworrl.beaconfix.ui.vm

import androidx.annotation.StringRes
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.help.DriveEstimate
import org.sworrl.beaconfix.help.HelpKind
import org.sworrl.beaconfix.help.HelpMeta
import org.sworrl.beaconfix.help.HelpPlace
import org.sworrl.beaconfix.help.HelpRepository
import org.sworrl.beaconfix.help.HelpSnapshot
import javax.inject.Inject

/** The confidence label every help row carries as text (never colour or an icon alone). */
sealed interface HelpBadge {
    /** tier 1 */ data object ChildrensEr : HelpBadge
    /** tier 2 with an ER on the same campus */ data class ChildrensCampus(val campus: String) : HelpBadge
    /** tier 2 */ data object ChildrensUnconfirmed : HelpBadge
    /** tier 3: a general ER with a pediatrics department */ data object PedsDept : HelpBadge
    /** urgent care, pediatric or not */ data object NotEr : HelpBadge
    /** a general hospital with a tagged ER */ data object Er : HelpBadge
    /** a general hospital, ER not tagged */ data object ErUnconfirmed : HelpBadge
    /** police, fire, pharmacy, vet */ data object None : HelpBadge

    /** The string resource for this label (format arg: [ChildrensCampus.campus]). */
    @get:StringRes val res: Int get() = when (this) {
        ChildrensEr -> R.string.help_badge_childrens_er
        is ChildrensCampus -> R.string.help_badge_campus
        ChildrensUnconfirmed -> R.string.help_badge_unconfirmed
        PedsDept -> R.string.help_badge_peds_dept
        NotEr -> R.string.help_badge_not_er
        Er -> R.string.help_badge_er
        ErUnconfirmed -> R.string.help_badge_er_unconfirmed
        None -> 0
    }
    val args: List<String> get() = if (this is ChildrensCampus) listOf(campus) else emptyList()
}

/** One place on the Help screen, ready to draw. [eta]: "~15 min (est.)" or "". */
data class HelpRowModel(val kind: String, val place: HelpPlace, val icon: String, val badge: HelpBadge, val eta: String) {
    val notEr get() = badge == HelpBadge.NotEr
}

/** The Help screen top to bottom (A2 spec order). The ER row is always the general ER, never a pediatric pick. */
data class HelpUi(
    val number: String = "",
    val peds: HelpRowModel? = null,
    val closer: HelpRowModel? = null,
    val er: HelpRowModel? = null,
    val urgent: List<HelpRowModel> = emptyList(),
    val safety: List<HelpRowModel> = emptyList(),
    val more: List<HelpRowModel> = emptyList(),
    val poisonControl: String? = null,
) {
    val isEmpty get() = peds == null && closer == null && er == null && urgent.isEmpty() && safety.isEmpty() && more.isEmpty()
}

/** Snapshot → rows (pure, JVM-tested). */
object HelpRows {
    fun icon(kind: String) = when (kind) {
        HelpKind.PEDS_ER, HelpKind.PEDS_CLOSER -> "🧸"; HelpKind.PEDS_URGENT -> "🩹"; HelpKind.ER -> "🏥"; HelpKind.URGENT -> "🩺"
        HelpKind.POLICE -> "🚔"; HelpKind.FIRE -> "🚒"; HelpKind.PHARMACY -> "💊"; HelpKind.VET -> "🐾"; else -> "📍"
    }

    fun badge(p: HelpPlace): HelpBadge = when {
        p.kind == HelpKind.PEDS_URGENT || p.kind == HelpKind.URGENT || p.notEr || p.tier == 4 -> HelpBadge.NotEr
        p.tier == 1 -> HelpBadge.ChildrensEr
        p.tier == 2 && p.campus.isNotBlank() -> HelpBadge.ChildrensCampus(p.campus)
        p.tier == 2 -> HelpBadge.ChildrensUnconfirmed
        p.tier == 3 -> HelpBadge.PedsDept
        p.kind == HelpKind.ER -> if (p.er == "yes") HelpBadge.Er else HelpBadge.ErUnconfirmed
        else -> HelpBadge.None
    }

    fun row(p: HelpPlace) = HelpRowModel(p.kind, p, icon(p.kind), badge(p), if (p.driveS > 0) DriveEstimate.text(p.driveS, p.driveEst) else "")

    fun map(s: HelpSnapshot): HelpUi {
        fun one(kind: String) = s.places.firstOrNull { it.kind == kind }?.let { row(it) }
        // a malformed snapshot could put a pediatric pick under "er": the ER row is only ever a general ER
        val er = s.places.firstOrNull { it.kind == HelpKind.ER && it.tier != 1 && it.tier != 2 }?.let { row(it) }
        return HelpUi(
            number = s.number,
            peds = one(HelpKind.PEDS_ER), closer = one(HelpKind.PEDS_CLOSER), er = er,
            urgent = listOfNotNull(one(HelpKind.PEDS_URGENT), one(HelpKind.URGENT)),
            safety = listOfNotNull(one(HelpKind.POLICE), one(HelpKind.FIRE)),
            more = listOfNotNull(one(HelpKind.PHARMACY), one(HelpKind.VET)),
            poisonControl = s.poisonControl,
        )
    }
}

@HiltViewModel
class HelpViewModel @Inject constructor(private val help: HelpRepository) : ViewModel() {
    val snapshot: StateFlow<HelpSnapshot> = help.snapshot
    val meta: StateFlow<HelpMeta> = help.meta
    val refreshing: StateFlow<Boolean> = help.refreshing
    val ui: StateFlow<HelpUi> = help.snapshot.map { HelpRows.map(it) }.stateIn(viewModelScope, SharingStarted.Eagerly, HelpRows.map(help.snapshot.value))

    /** The Help screen opened: desktop first; the phone may search itself when no desktop answers. */
    fun open() { viewModelScope.launch { help.refresh(force = false) } }
    /** Pull-to-refresh / the Refresh button. */
    fun refresh() { viewModelScope.launch { help.refresh(force = true) } }
    /** Cards on Home / Places: cheap, desktop only. */
    fun refreshIfStale() { viewModelScope.launch { help.refreshIfStale() } }
}
