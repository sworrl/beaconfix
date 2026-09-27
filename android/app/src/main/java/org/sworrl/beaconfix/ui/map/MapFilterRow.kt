package org.sworrl.beaconfix.ui.map

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.db.PoiEntity

/**
 * Which cached places the map draws (pure apart from [MapFilterRow]). The choice is kept in `Prefs.mapPoiFilter`.
 * Kids ER = pediatric ERs, pediatric urgent care and general ERs with a pediatrics department (tier 3); Help & medical =
 * the help categories; Kids & fun = the desktop's "kids" group.
 */
object MapFilter {
    const val ALL = "all"
    const val KIDS_ER = "kids_er"
    const val HELP = "help"
    const val KIDS = "kids"
    const val OFF = "off"

    enum class Choice(val key: String, @StringRes val label: Int) {
        ALL_PLACES(ALL, R.string.map_filter_all), KIDS_ER_PLACES(KIDS_ER, R.string.map_filter_kids_er), HELP_PLACES(HELP, R.string.map_filter_help),
        KIDS_PLACES(KIDS, R.string.map_filter_kids), NONE(OFF, R.string.map_filter_off),
    }

    val KEYS = Choice.entries.map { it.key }
    val HELP_CATS = setOf("police", "fire", "health", "urgent", "peds_er", "peds_urgent", "pharmacy", "vet")
    /** The desktop's "kids" group, for rows cached without a group. */
    private val KIDS_CATS = setOf("playground", "park", "dogpark", "pool", "splash", "zoo", "museum", "themepark", "icecream", "cinema",
        "bowling", "arcade", "trampoline", "skate", "beach", "picnic", "trail")

    /** An unknown or empty stored value means [ALL]. */
    fun normalize(key: String?): String = if (key != null && key in KEYS) key else ALL

    fun isKidsEr(p: PoiEntity) = p.cat == "peds_er" || p.cat == "peds_urgent" || (p.cat == "health" && p.peds == 3)

    fun matches(key: String?, p: PoiEntity): Boolean = when (normalize(key)) {
        OFF -> false
        KIDS_ER -> isKidsEr(p)
        HELP -> p.cat in HELP_CATS
        KIDS -> p.grp == "kids" || (p.grp.isBlank() && p.cat in KIDS_CATS)
        else -> true
    }

    fun apply(key: String?, rows: List<PoiEntity>): List<PoiEntity> = when (normalize(key)) {
        ALL -> rows
        OFF -> emptyList()
        else -> rows.filter { matches(key, it) }
    }
}

/** All · Kids ER · Help & medical · Kids & fun · Off, one selected. */
@Composable
fun MapFilterRow(selected: String?, onPick: (String) -> Unit, modifier: Modifier = Modifier) {
    val sel = MapFilter.normalize(selected)
    LazyRow(modifier, horizontalArrangement = Arrangement.spacedBy(6.dp), contentPadding = PaddingValues(end = 8.dp)) {
        items(MapFilter.Choice.entries) { c ->
            val label = stringResource(c.label)
            val cd = stringResource(R.string.map_filter_cd, label)
            FilterChip(selected = sel == c.key, onClick = { onPick(c.key) }, label = { Text(label) }, modifier = Modifier.semantics { contentDescription = cd })
        }
    }
}
