package org.sworrl.beaconfix.sightings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.db.PlateEventEntity
import java.time.ZoneOffset

/** §6: what alerts, and how it is worded. */
class SightingsTest {
    private val now = 1_760_000_000_000L
    private fun pass(uid: String, ageH: Double, source: String = PlateEvents.SRC_PHONE_LIVE, type: String = "alpr", facing: Int? = 1) = PlateEventEntity(
        uid = uid, kind = PlateEvents.CAMERA_PASS, time = "", timeMs = now - (ageH * 3_600_000).toLong(), cameraId = "osm:node/1", distanceM = 21.4,
        operator = "Flock Safety", model = "Falcon", cameraType = type, source = source, confidence = 95, facing = facing)
    private fun search(uid: String) = PlateEventEntity(uid = uid, kind = PlateEvents.PLATE_SEARCH, time = "", timeMs = 1_730_462_400_000L, agency = "Chehalis WA PD",
        source = PlateEvents.SRC_HIBF, confidence = 100, metrics = """{"reason":"stolen vehicle","case_number":"24-1234"}""")

    @Test fun passWording() {
        val (title, body) = Sightings.alertText(pass("a", 0.1), ZoneOffset.UTC)!!
        assertEquals("Passed an ALPR camera · Flock Safety Falcon · 21 m", title)
        assertEquals("Your plate was likely read (camera faced you). Confidence 95 %.", body)
        assertEquals("Your plate was likely read (facing unknown). Confidence 95 %.", Sightings.alertText(pass("b", 0.1, facing = null))!!.second)
        assertEquals("Your plate was likely read (camera faced away). Confidence 95 %.", Sightings.alertText(pass("c", 0.1, facing = 0))!!.second)
        assertNull("a traffic camera never alerts", Sightings.alertText(pass("d", 0.1, type = "camera")))
    }

    @Test fun searchWording() {
        val (title, body) = Sightings.alertText(search("s"), ZoneOffset.UTC)!!
        assertEquals("Your plate was searched in Flock · Chehalis WA PD", title)
        assertEquals("Nov 1, 2024: stolen vehicle (case 24-1234). From a released Flock audit log.", body)
    }

    @Test fun backfillIsOneSummaryNotAFlood() {
        val fresh = listOf(pass("live", 0.5), pass("old-live", 30.0), search("s1"), pass("traffic", 0.2, type = "camera")) +
            (1..40).map { pass("bf$it", it.toDouble(), source = PlateEvents.SRC_BACKFILL) }
        val plan = Sightings.plan(fresh, now)
        assertEquals(listOf("live", "s1"), plan.single.map { it.uid })
        assertEquals(41, plan.summaryPasses)                      // 40 backfill + 1 older than 24 h; the traffic camera is not counted
        assertEquals(0, plan.summarySearches)
        // more than five new searches: five alerts, the rest summarised
        val many = Sightings.plan((1..8).map { search("s$it") }, now)
        assertEquals(5, many.single.size); assertEquals(3, many.summarySearches)
        // already alerted rows stay quiet
        assertTrue(Sightings.plan(listOf(pass("n", 0.1).copy(notified = true)), now).single.isEmpty())
    }

    @Test fun summaryTitleAndDeepLink() {
        assertEquals("Backfill: 12 ALPR camera passes, 1 plate search since Nov 1, 2024", Sightings.summaryTitle(12, 1, 1_730_462_400_000L, ZoneOffset.UTC))
        assertEquals("Backfill: 1 ALPR camera pass since Nov 1, 2024", Sightings.summaryTitle(1, 0, 1_730_462_400_000L, ZoneOffset.UTC))
        assertEquals("beaconfix://sighting/pass%3Aosm%3Anode%2F1%3A29333333", Sightings.deepLink("pass:osm:node/1:29333333"))
    }
}
