package org.sworrl.beaconfix.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.help.Origin
import org.sworrl.beaconfix.ui.places.PlaceFilters.Filter
import org.sworrl.beaconfix.ui.places.placeBadges
import org.sworrl.beaconfix.ui.vm.PlacesEmpty
import org.sworrl.beaconfix.ui.vm.PlacesModel
import org.sworrl.beaconfix.R

class PlacesViewModelTest {
    private val now = 1_790_000_000_000L
    private val desk = Origin.Fix(40.0, -75.0, 10.0, now - 5 * 60_000L)

    private fun row(name: String, lat: Double, lon: Double, cat: String = "fuel", fetchedAt: Long = now - 3_600_000L, source: String = "desktop:192.0.2.10:47822") =
        PoiEntity(source = source, key = "ll:$name", cat = cat, name = name, lat = lat, lon = lon, fetchedAt = fetchedAt, originLat = 40.0, originLon = -75.0)

    // A is next to the RV, B next to the phone 10 km north of it
    private val a = row("A near the RV", 40.001, -75.0)
    private val b = row("B near the phone", 40.091, -75.0)
    private val c = row("C between", 40.05, -75.0)

    @Test fun orderFollowsThePhoneWhenItsFixIsFresh() {
        val phone = Origin.Fix(40.09, -75.0, 12.0, now - 60_000L)
        val o = Origin.choose(phone, desk, now)
        assertEquals(Origin.PHONE, o.kind)
        val list = PlacesModel.build(listOf(a, c, b), o, Filter(), now)
        assertEquals(listOf("B near the phone", "C between", "A near the RV"), list.map { it.row.name })
        assertEquals(111.0, list.first().distM, 20.0)
    }

    @Test fun anOldPhoneFixMeasuresFromTheRv() {
        val stale = Origin.Fix(40.09, -75.0, 12.0, now - 3_600_000L)
        val list = PlacesModel.build(listOf(b, c, a), Origin.choose(stale, desk, now), Filter(), now)
        assertEquals(listOf("A near the RV", "C between", "B near the phone"), list.map { it.row.name })
    }

    @Test fun withNoPositionEachRowUsesItsSearchOrigin() {
        val list = PlacesModel.build(listOf(b, a), Origin.choose(null, null, now), Filter(), now)
        assertEquals("A near the RV", list.first().row.name)
        assertTrue(list.all { it.distM >= 0 })
    }

    @Test fun statusSaysHowOldAndHowFar() {
        val phone = Origin.Fix(40.09, -75.0, 12.0, now - 60_000L)
        val s = PlacesModel.status(listOf(a, b), Origin.choose(phone, desk, now), "Testville")
        assertEquals(now - 3_600_000L, s.newest)
        assertEquals(10_000.0, s.distM, 200.0)
        assertEquals("Testville", s.near)
        assertEquals(0L, PlacesModel.status(emptyList(), Origin.choose(phone, desk, now)).newest)
        assertTrue(PlacesModel.status(listOf(row("P", 40.0, -75.0, source = "phone")), Origin.choose(phone, desk, now)).fromPhone)
    }

    @Test fun emptyStatesAreToldApart() {
        assertEquals(PlacesEmpty.UNPAIRED, PlacesModel.empty(paired = false, reachable = false, total = 0, shown = 0))
        assertEquals(PlacesEmpty.UNREACHABLE, PlacesModel.empty(paired = true, reachable = false, total = 0, shown = 0))
        assertEquals(PlacesEmpty.FETCHING, PlacesModel.empty(paired = true, reachable = true, total = 0, shown = 0))
        assertEquals(PlacesEmpty.FILTERED, PlacesModel.empty(paired = true, reachable = false, total = 5, shown = 0))
        assertEquals(PlacesEmpty.NONE, PlacesModel.empty(paired = false, reachable = false, total = 5, shown = 2))
    }

    @Test fun rvFixPrefersTheNewest() {
        val older = Origin.Fix(40.5, -75.5, 10.0, now - 86_400_000L)
        assertEquals(older, PlacesModel.rvFix(emptyList(), older))
        assertEquals(null, PlacesModel.rvFix(emptyList(), null))
    }

    @Test fun badges() {
        val kids = PlacesModel.build(listOf(row("K", 40.0, -75.0, cat = "peds_urgent").copy(peds = 4, hours = "24/7")), Origin.choose(null, desk, now), Filter(), now).single()
        val res = placeBadges(kids).map { it.res }
        assertTrue(R.string.places_badge_kids_urgent in res)
        assertTrue("urgent care never gets an ER badge", R.string.places_badge_er !in res)
        assertTrue(R.string.places_open in res)
        val er = PlacesModel.build(listOf(row("E", 40.0, -75.0, cat = "health").copy(emergency = true, er = "yes", wheelchair = "yes")), Origin.choose(null, desk, now), Filter(), now).single()
        assertTrue(placeBadges(er).map { it.res }.containsAll(listOf(R.string.places_badge_er, R.string.places_badge_wheelchair)))
        val noEr = PlacesModel.build(listOf(row("N", 40.0, -75.0, cat = "health").copy(er = "no")), Origin.choose(null, desk, now), Filter(), now).single()
        assertEquals(listOf(R.string.places_badge_no_er), placeBadges(noEr).map { it.res })
    }
}
