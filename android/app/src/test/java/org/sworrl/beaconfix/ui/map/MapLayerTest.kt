package org.sworrl.beaconfix.ui.map

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.ui.map.PlaceSheetModel.Tier

/** The map's places layer: filters, draw order, the sheet's tier line and drive time (synthetic places only). */
class MapLayerTest {
    private fun p(key: String, cat: String, grp: String = "", peds: Int = 0, emergency: Boolean = false, er: String = "", campus: String = "",
                  lat: Double = 40.0, lon: Double = -75.0, driveS: Int = 0, driveEst: Boolean = true, oLat: Double = 0.0, oLon: Double = 0.0) =
        PoiEntity(source = "desktop:test", key = key, cat = cat, grp = grp, peds = peds, emergency = emergency, er = er, campus = campus,
            lat = lat, lon = lon, driveS = driveS, driveEst = driveEst, fetchedAt = 1, originLat = oLat, originLon = oLon)

    private val pedsEr = p("way/1", "peds_er", "civic", peds = 1, emergency = true, er = "yes")
    private val pedsUrgent = p("node/2", "peds_urgent", "civic", peds = 4)
    private val erPedsDept = p("way/3", "health", "civic", peds = 3, emergency = true)
    private val er = p("way/4", "health", "civic", emergency = true)
    private val hospital = p("way/5", "health", "civic")
    private val police = p("node/6", "police", "civic")
    private val fire = p("node/7", "fire", "civic")
    private val playground = p("node/8", "playground", "kids")
    private val parkNoGroup = p("node/9", "park")
    private val fuel = p("node/10", "fuel", "services")
    private val all = listOf(fuel, pedsEr, police, playground, er, pedsUrgent, fire, erPedsDept, hospital, parkNoGroup)

    @Test fun kidsErShowsOnlyPediatricPlaces() {
        assertEquals(setOf("way/1", "node/2", "way/3"), MapFilter.apply(MapFilter.KIDS_ER, all).map { it.key }.toSet())
    }

    @Test fun helpShowsTheHelpCategories() {
        val keys = MapFilter.apply(MapFilter.HELP, all).map { it.key }.toSet()
        assertEquals(setOf("way/1", "node/2", "way/3", "way/4", "way/5", "node/6", "node/7"), keys)
    }

    @Test fun kidsAndFunUsesTheGroupOrTheCategory() {
        assertEquals(setOf("node/8", "node/9"), MapFilter.apply(MapFilter.KIDS, all).map { it.key }.toSet())
    }

    @Test fun allOffAndUnknownFilters() {
        assertEquals(all, MapFilter.apply(MapFilter.ALL, all))
        assertTrue(MapFilter.apply(MapFilter.OFF, all).isEmpty())
        assertEquals(all, MapFilter.apply("bogus", all))
        assertEquals(all, MapFilter.apply(null, all))
        assertEquals(MapFilter.ALL, MapFilter.normalize(""))
    }

    @Test fun pediatricErsDrawLastThenErsThenPoliceAndFire() {
        val order = PlacesOverlay.drawOrder(all).map { it.key }
        assertEquals("way/1", order.last())
        assertEquals(setOf("way/3", "way/4"), order.subList(order.size - 3, order.size - 1).toSet())
        assertEquals(setOf("node/6", "node/7"), order.subList(order.size - 5, order.size - 3).toSet())
        // stable within a rank: the "everything else" rows keep their input order
        assertEquals(listOf("node/10", "node/8", "node/2", "way/5", "node/9"), order.take(5))
    }

    @Test fun aHospitalMarkedNoErIsNotDrawnAsAnEr() {
        assertFalse(PlacesOverlay.isEr(p("way/11", "health", emergency = true, er = "no")))
        assertTrue(PlacesOverlay.isEr(p("way/12", "health", er = "yes")))
    }

    @Test fun tierLineIsAlwaysWords() {
        assertEquals(Tier.PEDS_ER, PlaceSheetModel.tier(pedsEr))
        assertEquals(Tier.CAMPUS, PlaceSheetModel.tier(p("way/13", "peds_er", peds = 2, campus = "Test General Hospital")))
        assertEquals(Tier.UNCONFIRMED, PlaceSheetModel.tier(p("way/14", "peds_er", peds = 2)))
        assertEquals(Tier.PEDS_URGENT, PlaceSheetModel.tier(pedsUrgent))
        assertEquals(Tier.URGENT, PlaceSheetModel.tier(p("node/15", "urgent")))
        assertEquals(Tier.PEDS_DEPT, PlaceSheetModel.tier(erPedsDept))
        assertEquals(Tier.ER, PlaceSheetModel.tier(er))
        assertEquals(Tier.NO_ER, PlaceSheetModel.tier(p("way/16", "health", er = "no")))
        assertEquals(Tier.ER_UNKNOWN, PlaceSheetModel.tier(hospital))
        assertNull(PlaceSheetModel.tier(fuel))
    }

    @Test fun driveEstimateMatchesTheDesktop() {
        assertEquals(110, PlaceSheetModel.driveMinutes(90_000.0))   // ~1 h 50 min
        assertEquals(120, PlaceSheetModel.driveMinutes(100_000.0))  // ~2 h
        assertEquals(10, PlaceSheetModel.driveMinutes(10_000.0))    // ~10 min
        assertEquals(5, PlaceSheetModel.driveMinutes(300.0))        // at least 5 min
    }

    @Test fun etaFromThePhoneUnlessTheDesktopSearchedFromHere() {
        // desktop measured 40 min (not an estimate) from an origin 1 km from the phone: keep it
        val far = p("way/17", "peds_er", peds = 1, lat = 40.5, lon = -75.0, driveS = 2400, driveEst = false, oLat = 40.009, oLon = -75.0)
        assertEquals(PlaceSheetModel.Eta(40, false, true), PlaceSheetModel.eta(far, 40.0, -75.0))
        // phone 50 km from the desktop's origin: recompute from the phone
        val e = PlaceSheetModel.eta(far, 40.5 - 0.45, -75.0)!!
        assertTrue(e.fromYou && e.estimate && e.minutes == PlaceSheetModel.driveMinutes(50_000.0))
        // no phone fix: the desktop's time, from the RV
        assertEquals(PlaceSheetModel.Eta(40, false, false), PlaceSheetModel.eta(far, null, null))
        assertNull(PlaceSheetModel.eta(p("node/18", "fuel"), null, null))
    }

    @Test fun stylesNormalise() {
        assertEquals(TileStyles.STREETS, TileStyles.normalize(null))
        assertEquals(TileStyles.STREETS, TileStyles.normalize("sat"))
        assertEquals(TileStyles.SATELLITE, TileStyles.normalize("satellite"))
        assertEquals(listOf("streets", "dark", "topo", "satellite"), TileStyles.KEYS)
    }
}
