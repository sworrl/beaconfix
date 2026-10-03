package org.sworrl.beaconfix.alpr

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.alpr.core.AlprJson
import org.sworrl.beaconfix.alpr.core.Hotlist
import org.sworrl.beaconfix.alpr.core.HotlistEntry
import org.sworrl.beaconfix.alpr.core.HotlistMatcher
import org.sworrl.beaconfix.alpr.core.MatchKind
import org.sworrl.beaconfix.alpr.core.PlateLattice
import org.sworrl.beaconfix.alpr.core.SlotChar

class HotlistMatcherTest {
    private val now = java.time.Instant.parse("2026-10-02T12:00:00Z").toEpochMilli()
    private val list = Hotlist(
        entries = listOf(
            HotlistEntry(id = "a1", plate = "ABC-1234", plateState = "TX", alertType = "AMBER", title = "Child abduction"),
            HotlistEntry(id = "s1", plate = "SLV500", alertType = "Silver"),
            HotlistEntry(id = "old", plate = "OLD123", expiresAt = "2026-10-01T00:00:00Z"),
            HotlistEntry(id = "live", plate = "NEW123", expiresAt = "2026-10-03T00:00:00-05:00"),
        ),
        ownPlates = listOf("ZZB5678"),
    )
    private val m = HotlistMatcher(list) { now }

    @Test fun exactMatchAfterNormalising() {
        val r = m.match("abc 1234")
        assertEquals(1, r.size); assertEquals(MatchKind.EXACT, r[0].kind); assertEquals("a1", r[0].entry.id)
    }

    /** A lattice for [text] at [p] per slot, with [alts] = slot → (character, probability) runner-ups. */
    private fun lat(text: String, p: Float = 0.9f, alts: Map<Int, Pair<Char, Float>> = emptyMap()) =
        PlateLattice(text.mapIndexed { i, c -> listOfNotNull(SlotChar(c, alts[i]?.let { 1f - it.second - 0.01f } ?: p), alts[i]?.let { SlotChar(it.first, it.second) }) })

    @Test fun hotlistPlateAmongTheLatticeAlternativesIsAPossibleMatch() {
        // the recognizer said 8 (0.6) or B (0.35) for the second slot: ABC1234 is a near miss
        val r = m.match("A8C1234", lat("A8C1234", alts = mapOf(1 to ('B' to 0.35f))))
        assertEquals(1, r.size); assertEquals(MatchKind.POSSIBLE, r[0].kind); assertEquals("a1", r[0].entry.id)
        assertEquals("ABC1234", r[0].candidate); assertEquals(0.35 / 0.64, r[0].ratio, 0.01)
        // any character pair, not only a fixed table: X vs C, if that is what the slot offered
        assertEquals(MatchKind.POSSIBLE, m.match("ABX1234", lat("ABX1234", alts = mapOf(2 to ('C' to 0.2f))))[0].kind)
    }

    @Test fun noAlternativeNoMatch() {
        assertTrue(m.match("A8C1234").isEmpty())                                        // text only: no lattice
        assertTrue(m.match("A8C1234", lat("A8C1234", 0.95f)).isEmpty())                 // B never offered
        assertTrue(m.match("A8C1234", lat("A8C1234", alts = mapOf(1 to ('D' to 0.3f)))).isEmpty())
        // two slots each offering the listed character only at 1 %: 1e-4 as likely, under MIN_RATIO
        assertTrue(m.match("A8C1Z34", lat("A8C1Z34", 0.98f, mapOf(1 to ('B' to 0.01f), 4 to ('2' to 0.01f)))).isEmpty())
        assertTrue(m.match("ABC12345", lat("ABC12345")).isEmpty())                      // different length
        assertTrue(m.match("A").isEmpty())
    }

    @Test fun twoUncertainSlotsStillMatch() {
        val r = m.match("A8C1Z34", lat("A8C1Z34", alts = mapOf(1 to ('B' to 0.4f), 4 to ('2' to 0.4f))))
        assertEquals(listOf("ABC1234"), r.map { it.candidate })
    }

    @Test fun latticeScoresAndBestFirstCandidates() {
        val l = lat("A8C", alts = mapOf(1 to ('B' to 0.3f), 2 to ('G' to 0.1f)))
        val k = l.kBest(10).map { it.first }
        assertEquals("A8C", k[0]); assertEquals("ABC", k[1]); assertEquals("A8G", k[2]); assertEquals("ABG", k[3])
        assertEquals(4, k.size)
        val scores = l.kBest(10).map { it.second }
        assertEquals(scores.sortedDescending(), scores)
        assertEquals(l.logp("ABC"), l.kBest(10)[1].second, 1e-6)
        assertTrue(l.logp("XYZ") < l.logp("ABG"))                                       // off-lattice characters get the floor
        assertEquals(Double.NEGATIVE_INFINITY, l.logp("ABCD"), 0.0)
    }

    @Test fun ownPlatesAreNeverMatched() {
        val withOwnOnList = HotlistMatcher(list.copy(entries = list.entries + HotlistEntry(id = "x", plate = "ZZB5678"))) { now }
        assertTrue(withOwnOnList.match("ZZB5678").isEmpty())
        assertTrue(withOwnOnList.isOwn("ZZB-5678"))
        assertTrue(withOwnOnList.isOwn("zzb 5678"))
        assertTrue(withOwnOnList.isOwn("ZZ85678"))        // misread B/8
        assertTrue(withOwnOnList.match("ZZ85678").isEmpty())
        assertFalse(withOwnOnList.isOwn("ZZB5679"))
        // our plate as a lattice alternative of the read: still ours
        assertTrue(withOwnOnList.isOwn("ZZB5679", lat("ZZB5679", alts = mapOf(6 to ('8' to 0.3f)))))
    }

    @Test fun expiredEntriesAreIgnored() {
        assertTrue(m.match("OLD123").isEmpty())
        assertEquals(1, m.match("NEW123").size)
    }

    @Test fun parsesTheServerHotlist() {
        val h = AlprJson.json.decodeFromString(Hotlist.serializer(), """
            {"updated_at":"2026-10-02T11:00:00Z","entries":[{"id":"e1","plate":"XYZ987","plate_state":"OK","alert_type":"AMBER",
             "title":"t","summary":"s","vehicle_desc":"red sedan","url":"https://x","expires_at":"2026-10-04T00:00:00Z","extra":1}],
             "own_plates":["ZZB5678"],"blocked_regions":["ME","NH","AR"]}""")
        assertEquals("red sedan", h.entries[0].vehicleDesc); assertEquals(listOf("ME", "NH", "AR"), h.blockedRegions)
        assertEquals(MatchKind.EXACT, HotlistMatcher(h) { now }.match("XYZ987")[0].kind)
    }
}

class HotlistPartialAndIdsTest {
    @Test fun partialPlatesAreOnlyPossibleMatches() {
        val h = Hotlist(entries = listOf(
            HotlistEntry(id = "7", plate = "AB*12*", partial = true, alertType = "AMBER"),
            HotlistEntry(id = "8", plate = "XK9", partial = true, alertType = "Blue"),
        ))
        val m = HotlistMatcher(h)
        assertEquals(listOf(MatchKind.POSSIBLE), m.match("ABC123").map { it.kind })
        assertTrue(m.match("ABC1234").isEmpty())                // anchored: length must agree
        assertTrue(m.match("AXC123").isEmpty())
        assertEquals("8", m.match("7XK9123").single().entry.id)   // unanchored run anywhere
        assertTrue(m.match("XK").isEmpty())
    }

    @Test fun numericIdsFromTheServer() {
        val h = AlprJson.json.decodeFromString(Hotlist.serializer(), """{"entries":[{"id":42,"plate":"ABC123","alert_type":"AMBER","title":"t","partial":false}],"own_plates":[],"blocked_regions":null}""")
        assertEquals("42", h.entries[0].id)
        val hit = AlprJson.json.encodeToString(org.sworrl.beaconfix.alpr.core.HitBody.serializer(), org.sworrl.beaconfix.alpr.core.HitBody("42", "ABC123", 0.9, "2026-10-02T01:00:00.000-04:00"))
        assertTrue(hit, hit.contains("\"entry_id\":42"))
        val hello = AlprJson.json.decodeFromString(org.sworrl.beaconfix.alpr.core.HelloReply.serializer(), """{"camera_id":"mobile-5","camera_name":"HawtDawg dash-cam","device_id":5,"server_name":"desk-pc"}""")
        assertEquals("5", hello.deviceId); assertEquals("mobile-5", hello.cameraId)
        val fr = AlprJson.json.decodeFromString(org.sworrl.beaconfix.alpr.core.FramesReply.serializer(), """{"accepted":false,"reason":"private ALPR is not allowed in NH"}""")
        assertFalse(fr.accepted)
    }
}
