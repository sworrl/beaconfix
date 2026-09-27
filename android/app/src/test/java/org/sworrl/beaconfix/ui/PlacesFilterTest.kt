package org.sworrl.beaconfix.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.ui.places.PlaceFilters
import org.sworrl.beaconfix.ui.places.PlaceFilters.Chip
import org.sworrl.beaconfix.ui.places.PlaceFilters.Filter
import java.time.LocalDateTime

class PlacesFilterTest {
    private fun row(cat: String, name: String = "Test $cat", peds: Int = 0, grp: String = "", address: String = "", hours: String = "") =
        PoiEntity(source = "desktop:192.0.2.10:47822", key = "ll:$cat:$name", cat = cat, grp = grp, name = name, address = address, hours = hours, peds = peds,
            lat = 40.0, lon = -75.0, fetchedAt = 1L)

    private val at = LocalDateTime.of(2026, 9, 23, 10, 0)   // a Wednesday morning
    private val all = listOf(
        row("police", grp = "civic"), row("fire", grp = "civic"), row("health", "Test General Hospital", grp = "civic"), row("health", "Test Valley Medical Center", peds = 3, grp = "civic"),
        row("urgent", grp = "civic"), row("peds_er", peds = 1, grp = "civic"), row("peds_urgent", peds = 4, grp = "civic"), row("pharmacy", grp = "civic"), row("vet", grp = "civic"),
        row("library", grp = "civic"), row("playground", grp = "kids"), row("fuel", grp = "services"), row("grocery"), row("zoo"), row("townhall"),
    )
    private fun shown(f: Filter) = all.filter { PlaceFilters.matches(it, f, at) }

    @Test fun helpChipHoldsEveryHelpCategory() {
        val cats = shown(Filter(Chip.HELP)).map { it.cat }.toSet()
        assertEquals(setOf("police", "fire", "health", "urgent", "peds_er", "peds_urgent", "pharmacy", "vet"), cats)
    }

    @Test fun kidsErIsOnlyPediatricSites() {
        val names = shown(Filter(Chip.KIDS_ER)).map { it.name }
        assertEquals(listOf("Test Valley Medical Center", "Test peds_er", "Test peds_urgent"), names)
        assertTrue(shown(Filter(Chip.KIDS_ER)).all { it.cat == "peds_er" || it.cat == "peds_urgent" || it.peds == 3 })
    }

    @Test fun groupsFallBackToTheCategory() {
        assertEquals(setOf("playground", "zoo"), shown(Filter(Chip.KIDS)).map { it.cat }.toSet())
        assertEquals(setOf("fuel", "grocery"), shown(Filter(Chip.SERVICES)).map { it.cat }.toSet())
        assertTrue(shown(Filter(Chip.CIVIC)).any { it.cat == "townhall" })
        assertEquals(all.size, shown(Filter(Chip.ALL)).size)
    }

    @Test fun searchMatchesTheAddress() {
        val withAddress = row("fuel", "Test Fuel", address = "12 Test Pike, Testville")
        assertTrue(PlaceFilters.matches(withAddress, Filter(query = "test pike"), at))
        assertTrue(PlaceFilters.matches(withAddress, Filter(query = "TESTVILLE"), at))
        assertFalse(PlaceFilters.matches(withAddress, Filter(query = "elsewhere"), at))
        assertTrue(PlaceFilters.matches(withAddress, Filter(query = "  "), at))
    }

    @Test fun openNowUsesOpeningHours() {
        val open = row("pharmacy", hours = "Mo-Fr 08:00-20:00")
        val closed = row("pharmacy", hours = "Sa-Su 09:00-12:00")
        val unknown = row("pharmacy", hours = "")
        assertTrue(PlaceFilters.matches(open, Filter(openNow = true), at))
        assertFalse(PlaceFilters.matches(closed, Filter(openNow = true), at))
        assertFalse(PlaceFilters.matches(unknown, Filter(openNow = true), at))
        assertTrue(PlaceFilters.matches(open, Filter(Chip.HELP, openNow = true), at))
    }
}
