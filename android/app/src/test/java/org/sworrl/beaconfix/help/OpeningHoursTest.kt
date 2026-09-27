package org.sworrl.beaconfix.help

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.time.LocalDateTime

class OpeningHoursTest {
    // 2026-09-21 is a Monday
    private fun mon(h: Int, m: Int = 0) = LocalDateTime.of(2026, 9, 21, h, m)
    private fun tue(h: Int, m: Int = 0) = LocalDateTime.of(2026, 9, 22, h, m)
    private fun wed(h: Int, m: Int = 0) = LocalDateTime.of(2026, 9, 23, h, m)
    private fun sat(h: Int, m: Int = 0) = LocalDateTime.of(2026, 9, 26, h, m)
    private fun sun(h: Int, m: Int = 0) = LocalDateTime.of(2026, 9, 27, h, m)

    @Test fun planExamples() {
        assertEquals(true, OpeningHours.isOpen("Mo-Su 18:00-02:00", tue(1)))
        assertEquals(false, OpeningHours.isOpen("Mo-Fr 08:00-17:00; Sa 09:00-12:00", sun(10)))
        assertNull(OpeningHours.isOpen("sunrise-sunset", sun(10)))
    }

    @Test fun alwaysOpen() {
        assertEquals(true, OpeningHours.isOpen("24/7", sun(3)))
        assertEquals(true, OpeningHours.isOpen("Mo-Su 00:00-24:00", wed(23, 59)))
        assertNull(OpeningHours.closesAt("24/7", sun(3)))
    }

    @Test fun splitDaysAndBreaks() {
        val spec = "Mo-Fr 08:00-12:00,13:00-17:00"
        assertEquals(true, OpeningHours.isOpen(spec, wed(9)))
        assertEquals(false, OpeningHours.isOpen(spec, wed(12, 30)))
        assertEquals(true, OpeningHours.isOpen(spec, wed(13, 30)))
        assertEquals(false, OpeningHours.isOpen(spec, sat(10)))
        assertEquals(wed(12), OpeningHours.closesAt(spec, wed(9)))
        assertEquals(wed(17), OpeningHours.closesAt(spec, wed(16)))
        assertNull(OpeningHours.closesAt(spec, wed(12, 30)))
    }

    @Test fun laterRulesReplaceEarlierOnesAndCommasAdd() {
        assertEquals(false, OpeningHours.isOpen("Mo-Su 08:00-20:00; Su off", sun(10)))
        assertEquals(true, OpeningHours.isOpen("Mo-Su 08:00-20:00; Su off", sat(10)))
        assertEquals(true, OpeningHours.isOpen("Mo-Fr 08:00-17:00, Sa 09:00-12:00", sat(10)))
        assertEquals(false, OpeningHours.isOpen("Mo-Fr 08:00-17:00, Sa 09:00-12:00", sun(10)))
        assertEquals(true, OpeningHours.isOpen("Mo,We,Fr 09:00-13:00", wed(10)))
        assertEquals(false, OpeningHours.isOpen("Mo,We,Fr 09:00-13:00", tue(10)))
    }

    @Test fun wrappingDayRangesAndOvernight() {
        assertEquals(true, OpeningHours.isOpen("Fr-Mo 10:00-14:00", sun(11)))
        assertEquals(false, OpeningHours.isOpen("Fr-Mo 10:00-14:00", wed(11)))
        assertEquals(true, OpeningHours.isOpen("Mo 18:00-02:00", tue(1, 30)))
        assertEquals(false, OpeningHours.isOpen("Mo 18:00-02:00", wed(1, 30)))
        assertEquals(tue(2), OpeningHours.closesAt("Mo 18:00-02:00", tue(1, 30)))
        assertEquals(tue(2), OpeningHours.closesAt("Mo-Su 18:00-02:00", mon(19)))
        assertEquals(true, OpeningHours.isOpen("Sa 22:00-26:00", sun(1)))
    }

    @Test fun holidaysAreSkippedNotFatal() {
        assertEquals(true, OpeningHours.isOpen("Mo-Fr 09:00-17:00; PH off", mon(10)))
        assertEquals(false, OpeningHours.isOpen("Mo-Fr 09:00-17:00; PH off", sat(10)))
        assertNull(OpeningHours.isOpen("PH off", mon(10)))
    }

    @Test fun midnightContinuations() {
        assertEquals(tue(2), OpeningHours.closesAt("Mo 20:00-24:00; Tu 00:00-02:00", mon(21)))
    }

    @Test fun unknownStaysUnknown() {
        for (s in listOf(null, "", "  ", "\"by appointment\"", "Jan-Mar Mo-Fr 08:00-12:00", "Mo-Fr 08:00-dusk", "week 1-20 Mo 10:00-12:00", "Mo[1] 09:00-12:00", "unknown"))
            assertNull(s, OpeningHours.isOpen(s, mon(10)))
    }

    @Test fun shorthandForms() {
        assertEquals(true, OpeningHours.isOpen("Mo-Fr: 8:00 - 17:00", mon(8, 30)))
        assertEquals(true, OpeningHours.isOpen("08:00-20:00", sun(12)))
        assertEquals(false, OpeningHours.isOpen("08:00-20:00", sun(21)))
        assertEquals(true, OpeningHours.isOpen("Mo-Fr 08:00-17:00 \"call first\"", mon(9)))
        assertEquals(true, OpeningHours.isOpen("Mo-Sa 07:00-21:00; Su 08:00-20:00", sun(8)))
        assertEquals(true, OpeningHours.isOpen("Mo-Fr 09:00+", mon(23)))
    }
}
