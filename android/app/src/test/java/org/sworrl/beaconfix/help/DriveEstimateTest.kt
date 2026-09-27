package org.sworrl.beaconfix.help

import org.junit.Assert.assertEquals
import org.junit.Test

class DriveEstimateTest {
    @Test fun planExamples() {
        assertEquals("~1 h 50 min (est.)", DriveEstimate.text(DriveEstimate.seconds(90_000.0)))
        assertEquals("~2 h (est.)", DriveEstimate.text(DriveEstimate.seconds(100_000.0)))
        assertEquals("~10 min (est.)", DriveEstimate.text(DriveEstimate.seconds(10_000.0)))
    }

    @Test fun roundedToFiveMinutesWithAFiveMinuteMinimum() {
        assertEquals(300, DriveEstimate.seconds(0.0))
        assertEquals(300, DriveEstimate.seconds(100.0))
        assertEquals(0, DriveEstimate.seconds(12_345.0) % 300)
        assertEquals("~5 min (est.)", DriveEstimate.text(1))
        assertEquals("~5 min (est.)", DriveEstimate.text(DriveEstimate.seconds(-5.0)))
    }

    @Test fun routedTimesDropTheEstimateMark() {
        assertEquals("~1 h 5 min", DriveEstimate.text(3890, est = false))
        assertEquals("", DriveEstimate.text(0))
        assertEquals("", DriveEstimate.text(-1))
    }

    @Test fun roadMetres() {
        assertEquals(14_000, DriveEstimate.roadM(10_000.0))
    }
}
