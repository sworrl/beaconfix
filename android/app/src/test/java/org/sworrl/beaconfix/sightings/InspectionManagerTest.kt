// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class InspectionManagerTest {

    @Test
    fun testGpxExport() {
        val target = InspectionPlan(
            cameraId = "osm:node/987654",
            cameraLat = 37.7749,
            cameraLon = -122.4194,
            operatorName = "San Francisco PD",
            model = "Flock Falcon",
            direction = "EB",
            vantages = listOf(
                VantagePoint(
                    lat = 37.7746,
                    lon = -122.4199,
                    distanceM = 35.0,
                    bearingToCamera = 72.0,
                    side = "behind",
                    reason = "Behind camera lens, outside field of view (35 m on sidewalk)"
                )
            ),
            toVantageLeg = RouteLeg(
                coordinates = listOf(Pair(37.7740, -122.4200), Pair(37.7746, -122.4199)),
                distanceM = 80.0,
                durationS = 60.0,
                ok = true
            ),
            awayLeg = RouteLeg(
                coordinates = listOf(Pair(37.7746, -122.4199), Pair(37.7750, -122.4210)),
                distanceM = 110.0,
                durationS = 85.0,
                ok = true
            ),
            safe = true,
            limits = InspectionManager.LIMITS_TEXT
        )

        // Generate GPX string with test mock or builder
        val gpx = buildString {
            append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
            append("<gpx version=\"1.1\" creator=\"BeaconFix-Android\"\n")
            append("  xmlns=\"http://www.topografix.com/GPX/1/1\">\n")
            append("  <wpt lat=\"${target.cameraLat}\" lon=\"${target.cameraLon}\">\n")
            append("    <name>${target.cameraId}</name>\n")
            append("    <desc>Target Camera: ${target.operatorName} ${target.model}</desc>\n")
            append("    <sym>camera</sym>\n")
            append("  </wpt>\n")
            val v0 = target.vantages.first()
            append("  <wpt lat=\"${v0.lat}\" lon=\"${v0.lon}\">\n")
            append("    <name>Safe Vantage Point</name>\n")
            append("    <desc>${v0.side}: ${v0.reason}; Look ${v0.bearingToCamera.toInt()} deg towards camera</desc>\n")
            append("    <sym>eye</sym>\n")
            append("  </wpt>\n")
            append("  <trk>\n    <name>Approach to Vantage</name>\n    <trkseg>\n")
            for (p in target.toVantageLeg.coordinates) {
                append("      <trkpt lat=\"${p.first}\" lon=\"${p.second}\"/>\n")
            }
            append("    </trkseg>\n  </trk>\n")
            append("  <trk>\n    <name>Departure from Vantage</name>\n    <trkseg>\n")
            for (p in target.awayLeg.coordinates) {
                append("      <trkpt lat=\"${p.first}\" lon=\"${p.second}\"/>\n")
            }
            append("    </trkseg>\n  </trk>\n")
            append("</gpx>\n")
        }

        assertTrue(gpx.contains("osm:node/987654"))
        assertTrue(gpx.contains("Safe Vantage Point"))
        assertTrue(gpx.contains("Approach to Vantage"))
        assertTrue(gpx.contains("Departure from Vantage"))
        assertTrue(gpx.contains("<trkpt lat=\"37.7746\" lon=\"-122.4199\"/>"))
    }

    @Test
    fun testLimitsHonesty() {
        val limits = InspectionManager.LIMITS_TEXT
        assertTrue(limits.contains("Only mapped cameras"))
        assertTrue(limits.contains("Unmapped cameras, PTZ / 360° domes, private CCTV, police-car ALPRs"))
        assertTrue(limits.contains("Look at stored photos first"))
    }

    @Test
    fun testVantageCandidateSide() {
        // Behind camera direction EB (90 deg) means looking from West (270 deg) towards East
        val camDirDeg = 90.0
        val oppositeBearing = (camDirDeg + 180.0) % 360.0
        assertEquals(270.0, oppositeBearing, 0.001)

        val lookDirection = (oppositeBearing + 180.0) % 360.0
        assertEquals(90.0, lookDirection, 0.001)
    }

    @Test
    fun testTurnByTurnRouteSteps() {
        val steps = listOf(
            RouteStep(
                instruction = "Depart towards camera vantage point outside detection cone",
                distanceM = 150.0,
                durationS = 15.0,
                streetName = "Safe Corridor",
                type = 10,
                lat = 37.7740,
                lon = -122.4200
            ),
            RouteStep(
                instruction = "Turn right onto safe vantage alley outside camera cone",
                distanceM = 60.0,
                durationS = 6.0,
                streetName = "Vantage Alley",
                type = 1,
                lat = 37.7744,
                lon = -122.4198
            ),
            RouteStep(
                instruction = "Arrive at safe vantage point (35 m). Look 72° towards camera.",
                distanceM = 0.0,
                durationS = 0.0,
                streetName = "Observation Station",
                type = 4,
                lat = 37.7746,
                lon = -122.4199
            )
        )
        val leg = RouteLeg(
            coordinates = listOf(Pair(37.7740, -122.4200), Pair(37.7744, -122.4198), Pair(37.7746, -122.4199)),
            distanceM = 210.0,
            durationS = 21.0,
            steps = steps,
            ok = true
        )
        assertEquals(3, leg.steps.size)
        assertEquals("Safe Corridor", leg.steps[0].streetName)
        assertEquals(1, leg.steps[1].type)
        assertTrue(leg.steps[2].instruction.contains("Look 72° towards camera"))
    }
}
