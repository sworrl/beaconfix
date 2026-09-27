package org.sworrl.beaconfix.ui.places

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
import androidx.compose.ui.unit.dp
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.help.OpeningHours
import java.time.LocalDateTime
import java.util.Locale

/**
 * The Places filters (pure apart from [PlaceFilterRow]). Chips: All · Help & medical · Kids ER · Civic · Kids & fun ·
 * Services, plus an "Open now" toggle that combines with any of them. Keys are the desktop's categories and groups
 * (C1); a row without a group (an older cache) is grouped by its category.
 */
object PlaceFilters {
    enum class Chip(@StringRes val label: Int) {
        ALL(R.string.places_chip_all), HELP(R.string.places_chip_help), KIDS_ER(R.string.places_chip_kids_er),
        CIVIC(R.string.places_chip_civic), KIDS(R.string.places_chip_kids), SERVICES(R.string.places_chip_services),
    }

    /** What the filter row shows: one chip, the open-now toggle and the search text. */
    data class Filter(val chip: Chip = Chip.ALL, val openNow: Boolean = false, val query: String = "")

    val HELP_CATS = setOf("police", "fire", "health", "urgent", "peds_er", "peds_urgent", "pharmacy", "vet")
    private val CIVIC_CATS = HELP_CATS + setOf("dentist", "library", "townhall", "court", "dmv", "school", "community", "post")
    private val KIDS_CATS = setOf("playground", "park", "dogpark", "pool", "splash", "zoo", "museum", "themepark", "icecream", "cinema",
        "bowling", "arcade", "trampoline", "skate", "beach", "picnic", "trail")

    /** civic | kids | services — the row's own group, else the one its category belongs to on the desktop. */
    fun groupOf(r: PoiEntity): String = r.grp.ifBlank { if (r.cat in CIVIC_CATS) "civic" else if (r.cat in KIDS_CATS) "kids" else "services" }

    fun isKidsEr(r: PoiEntity) = r.cat == "peds_er" || r.cat == "peds_urgent" || (r.cat == "health" && r.peds == 3)

    fun inChip(r: PoiEntity, chip: Chip): Boolean = when (chip) {
        Chip.ALL -> true
        Chip.HELP -> r.cat in HELP_CATS
        Chip.KIDS_ER -> isKidsEr(r)
        Chip.CIVIC -> groupOf(r) == "civic"
        Chip.KIDS -> groupOf(r) == "kids"
        Chip.SERVICES -> groupOf(r) == "services"
    }

    /** Case-insensitive match on name, kind, address, detail and category. */
    fun matchesQuery(r: PoiEntity, q: String): Boolean {
        val t = q.trim().lowercase(Locale.ROOT)
        if (t.isEmpty()) return true
        return listOf(r.name, r.label, r.address, r.detail, r.cat).any { it.lowercase(Locale.ROOT).contains(t) }
    }

    fun matches(r: PoiEntity, f: Filter, at: LocalDateTime): Boolean =
        inChip(r, f.chip) && matchesQuery(r, f.query) && (!f.openNow || OpeningHours.isOpen(r.hours, at) == true)
}

/** The chip row: the six chips, then "Open now". */
@Composable
fun PlaceFilterRow(filter: PlaceFilters.Filter, onChange: (PlaceFilters.Filter) -> Unit, modifier: Modifier = Modifier) {
    LazyRow(modifier, horizontalArrangement = Arrangement.spacedBy(6.dp), contentPadding = PaddingValues(horizontal = 12.dp)) {
        items(PlaceFilters.Chip.entries) { c ->
            FilterChip(selected = filter.chip == c, onClick = { onChange(filter.copy(chip = c)) }, label = { Text(stringResource(c.label)) })
        }
        item {
            FilterChip(selected = filter.openNow, onClick = { onChange(filter.copy(openNow = !filter.openNow)) }, label = { Text(stringResource(R.string.places_chip_open)) })
        }
    }
}
