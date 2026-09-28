package org.sworrl.beaconfix.sync

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.ApsDto
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.estimate.FitMetrics

/** The desktop's `fit` object on `/api/v1/aps` rows: old and graded shapes, and what lands on the AP row. Synthetic data. */
class DesktopFitTest {
    private val graded = """{"aps":[{"bssid":"02:00:00:00:00:01","ssid":"Test","kind":"observed","lat":40.0,"lon":-75.0,"r":19.0,
        "fit":{"n":24,"vantage":24,"rms":2.4,"acc":19.0,"p0":-42.7,"pathloss":2.35,"quality":"good","rejected":0,"semiMajor":19.0,"semiMinor":18.6,"orient":83.9,
               "updated":"2026-09-28T12:00:00","kind":"fix","grade":"B","score":82.1,"r95":46.1,"cep50":22.2,"pWithin25":0.586,"cxx":348.0,"cxy":1.44,"cyy":361.5,
               "lat":40.0,"lon":-75.0,"devices":1,"sessions":1,"inHull":true,"ambiguous":false,"modes":1,"moved":false}},
      {"bssid":"02:00:00:00:00:02","kind":"wigle","lat":40.001,"lon":-75.0,"r":30.0,"fit":{"n":5,"vantage":3,"acc":60.0,"lat":40.004,"lon":-75.0,"grade":"D","r95":150.0}},
      {"bssid":"02:00:00:00:00:03","kind":"observed","lat":40.002,"lon":-75.0,"r":40.0,"fit":{"n":5,"vantage":3,"rms":4.0,"acc":40.0,"p0":-40.0,"pathloss":2.4,"quality":"fair","updated":null}}]}"""

    @Test fun parsesGradedAndOlderFits() {
        val aps = ApiFactory.json.decodeFromString(ApsDto.serializer(), graded).aps
        val a = aps[0].fit!!
        assertEquals("B", a.grade); assertEquals(46.1, a.r95!!, 1e-9); assertEquals(true, a.inHull); assertEquals(1, a.devices); assertTrue(a.graded)
        val old = aps[2].fit!!
        assertNull(old.grade); assertNull(old.r95); assertEquals(3, old.vantage); assertTrue(!old.graded)
    }

    @Test fun onlyTheFitThatPlacedThePositionIsKept() {
        val aps = ApiFactory.json.decodeFromString(ApsDto.serializer(), graded).aps
        assertNotNull(Mirror.fitOfPosition(aps[0]))
        assertNull("a WiGLE position with the desktop's fit 330 m away", Mirror.fitOfPosition(aps[1]))
        assertNotNull("older desktop: kind observed", Mirror.fitOfPosition(aps[2]))
    }

    @Test fun theDesktopFitBecomesTheRowsGrade() {
        val aps = ApiFactory.json.decodeFromString(ApsDto.serializer(), graded).aps
        val row = ApEntity(bssid = "02:00:00:00:00:01", lat = 40.0, lon = -75.0, acc = 19.0, posSource = "placed", grade = "F", fitKind = "fix")
        val r = Mirror.withDesktopFit(row, Mirror.fitOfPosition(aps[0]), 1000L)
        assertEquals("B", r.grade); assertEquals("fix", r.fitKind); assertEquals(82.1, r.score!!, 1e-9); assertEquals(348.0, r.cxx!!, 1e-9); assertEquals(24, r.vantage)
        assertEquals(1000L, r.gradedAt)
        val m = FitMetrics.parse(r.fitMetrics)!!
        assertEquals("desktop", m.source); assertEquals(true, m.inHull); assertEquals(false, m.ambiguous)
        // no fit: the stale grade is cleared with the position change
        val cleared = Mirror.withDesktopFit(row, null, 1000L)
        assertNull(cleared.grade); assertNull(cleared.fitKind); assertNull(cleared.fitMetrics)
        assertEquals(40.0, cleared.lat!!, 0.0)
    }
}
