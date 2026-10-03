package org.sworrl.beaconfix.sightings

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.ZoneOffset

/** docs/SIGHTINGS.md §4 with a fake transport: no test here touches the network. */
class HibfTest {
    @Test fun hashPrefixOfTheKnownPlate() {
        assertEquals("9f89b1d8", Hibf.prefix("XYZ2345"))
        assertEquals("9f89b1d8", Hibf.prefix("  xyz2345 "))
        assertEquals(64, Hibf.fullHash("XYZ2345").length)
    }

    @Test fun variantsLikeTheSite() {
        val vars = Hibf.variants(listOf("XYZ-2345"))
        assertEquals(listOf("XYZ-2345", "XYZ2345"), vars.map { it.text })
        assertEquals(listOf("37b3a9d1", "9f89b1d8"), Hibf.prefixes(vars))
        assertTrue(vars.all { it.plate == "XYZ-2345" })
        // the site's a1(): typed form first, then the others in its depth-first order
        assertEquals(listOf("O1", "OI", "0I", "01"), Hibf.expand("o1"))
        assertEquals(10, Hibf.expand("OOOO").size)
        assertEquals("OOOO", Hibf.expand("OOOO").first())
        assertEquals(listOf("ABC1"), Hibf.forms("ABC 1"))
        assertEquals(listOf("AB-C1", "ABC1"), Hibf.forms("AB-C1"))
        assertEquals(listOf("abc1"), Hibf.forms("abc1"))
        // O/0 and I/1 forms, deduplicated by full hash across plates
        assertEquals(4, Hibf.variants(listOf("O1", "o1")).size)
    }

    private fun row(hash: String = Hibf.fullHash("xyz2345"), extra: String = ""): JsonObject = Json.parseToJsonElement(
        """{"org_id":17,"org_name":"Chehalis WA PD","org_state":"WA","org_locality":"Chehalis","search_time_utc":"2024-11-01T12:00:00Z",
           "license_plate_hash":"$hash","case_number":"24-1234","reason":"stolen vehicle","upload_id":"u9","source_url":"https://www.muckrock.com/foi/x/",
           "source_org_name":"Chehalis Police Department","total_networks_searched":4$extra}""").jsonObject

    @Test fun pagingFollowsTheCursor() {
        val bodies = ArrayList<String>()
        val pages = ArrayDeque(listOf(
            Hibf.HttpResult(200, """{"results":[{"a":1},{"a":2}],"nextCursor":"c1","hasMore":true,"count":2,"total":3,"limit":2}"""),
            Hibf.HttpResult(200, """{"results":[{"a":3}],"nextCursor":null,"hasMore":false,"count":1,"total":3,"limit":2}"""),
        ))
        var paced = 0
        val out = Hibf.search(listOf("9f89b1d8"), { url, body -> assertEquals(Hibf.SEARCH_URL, url); bodies += body; pages.removeFirst() }, { true }, { paced++ })
        assertTrue(out is Hibf.SearchOutcome.Ok)
        assertEquals(3, (out as Hibf.SearchOutcome.Ok).rows.size)
        assertEquals(2, out.requests); assertEquals(2, paced)
        assertEquals("""{"plates":["9f89b1d8"],"cursor":null}""", bodies[0])
        assertEquals("""{"plates":["9f89b1d8"],"cursor":"c1"}""", bodies[1])
    }

    @Test fun notFoundIsNoResultsAndErrorsAreReported() {
        assertEquals(Hibf.SearchOutcome.Ok(emptyList(), 1), Hibf.search(listOf("x"), { _, _ -> Hibf.HttpResult(404, """{"error":"none"}""") }, { true }, {}))
        val rl = Hibf.search(listOf("x"), { _, _ -> Hibf.HttpResult(429, "slow down", 120) }, { true }, {})
        assertEquals(120L, (rl as Hibf.SearchOutcome.RateLimited).retryAfterS)
        assertEquals(503, (Hibf.search(listOf("x"), { _, _ -> Hibf.HttpResult(503, "") }, { true }, {}) as Hibf.SearchOutcome.Failed).code)
        assertTrue(Hibf.search(listOf("x"), { _, _ -> throw java.io.IOException("no route") }, { true }, {}) is Hibf.SearchOutcome.Failed)
        assertTrue(Hibf.search(listOf("x"), { _, _ -> error("must not be called") }, { false }, {}) is Hibf.SearchOutcome.Capped)
    }

    @Test fun limiterKeepsTenSecondsAndThirtyADay() {
        var now = 1_760_000_000_000L
        val slept = ArrayList<Long>()
        val lim = Hibf.Limiter(Hibf.WatchState(), { now }, { slept += it; now += it }, ZoneOffset.UTC)
        val res = Hibf.search(listOf("x"), { _, _ -> Hibf.HttpResult(200, """{"results":[{"a":1}],"nextCursor":"n${now}","hasMore":true}""") }, lim::mayRequest, lim::pace, maxPages = 100)
        assertTrue(res is Hibf.SearchOutcome.Capped)
        assertEquals(30, lim.state.requestsToday)
        assertEquals(29, slept.size)
        assertTrue(slept.all { it >= Hibf.MIN_GAP_MS })
        // the next day the count starts over
        now += Hibf.DAY
        assertTrue(lim.mayRequest()); assertEquals(0, lim.state.requestsToday)
    }

    @Test fun onlyOurFullHashCounts() {
        val vars = Hibf.variants(listOf("XYZ-2345", "ABC123"))
        val got = Hibf.filterResults(listOf(row(), row(hash = "9f89b1d8" + "0".repeat(56)), row(hash = ""), row(hash = Hibf.prefix("ABC123"))), vars)
        assertEquals(3, got.size)                                   // same prefix, someone else's plate: dropped
        assertEquals(Hibf.Match(row(), "XYZ-2345", true), got[0])
        assertEquals("no full hash: kept, unverified", false, got[1].verified)
        assertEquals("XYZ-2345", got[1].plate)
        assertEquals("a bare prefix picks its plate", "ABC123", got[2].plate)
    }

    @Test fun resultBecomesAnHonestEvent() {
        val e = Hibf.toEvent(Hibf.Match(row(), "XYZ-2345", true), now = 0, zone = ZoneOffset.UTC)
        assertTrue(e.uid.startsWith("hibf:")); assertEquals(5 + 24, e.uid.length)
        assertEquals(e.uid, Hibf.searchUid(row()))
        assertEquals("hibf:" + Hibf.sha256Hex("17|2024-11-01T12:00:00Z|${Hibf.fullHash("xyz2345")}|24-1234|stolen vehicle|u9").take(24), e.uid)
        assertEquals("2024-11-01T12:00:00", e.timeLocal)
        assertEquals("Chehalis WA PD", e.agency)
        assertEquals("Chehalis WA PD searched XYZ-2345 — stolen vehicle (case 24-1234)", e.details)
        assertEquals("https://www.muckrock.com/foi/x/", e.sourceUrl)
        assertEquals("HaveIBeenFlocked · Chehalis Police Department audit log", e.sourceName)
        assertEquals(100, e.confidence)
        assertEquals("true", e.metrics["hashVerified"]!!.jsonPrimitive.content)
        assertEquals("4", e.metrics["total_networks_searched"]!!.jsonPrimitive.content)
        val unverified = Hibf.toEvent(Hibf.Match(row(hash = "", extra = ""","source_url":"s3://bucket/x.csv""""), "XYZ-2345", false), now = 0, zone = ZoneOffset.UTC)
        assertEquals(50, unverified.confidence)
        assertEquals(Hibf.SITE, unverified.sourceUrl)
    }

    @Test fun timesParse() {
        assertEquals(1_730_462_400_000L, Hibf.parseUtc("2024-11-01T12:00:00Z"))
        assertEquals(1_730_462_400_000L, Hibf.parseUtc("2024-11-01 12:00:00"))
        assertEquals(1_730_462_400_000L, Hibf.parseUtc("2024-11-01T12:00:00.000+00:00"))
        assertEquals(1_730_462_400_000L, Hibf.parseUtc("1730462400"))
        assertNull(Hibf.parseUtc("soon"))
    }

    @Test fun scheduleModes() {
        val now = 1_760_000_000_000L
        assertEquals(Hibf.Mode.IDLE, Hibf.mode(now, null, null, null))
        assertEquals(Hibf.Mode.DRIVING, Hibf.mode(now, now - 10 * 60_000, null, null))
        assertEquals(Hibf.Mode.IDLE, Hibf.mode(now, now - 31 * 60_000, null, null))
        assertEquals(Hibf.Mode.FLOCK, Hibf.mode(now, now, now - 71 * Hibf.HOUR, null))
        assertEquals(Hibf.Mode.LEAKY, Hibf.mode(now, now, now, now - 72 * Hibf.HOUR))
        assertEquals(Hibf.Mode.DRIVING, Hibf.mode(now, now, now - 73 * Hibf.HOUR, now - 73 * Hibf.HOUR))
        assertEquals(3 * Hibf.HOUR, Hibf.Mode.LEAKY.intervalMs); assertEquals(12 * Hibf.HOUR, Hibf.Mode.FLOCK.intervalMs)
        assertEquals(Hibf.DAY, Hibf.Mode.DRIVING.intervalMs); assertEquals(7 * Hibf.DAY, Hibf.Mode.IDLE.intervalMs)
    }

    @Test fun dueAndBackoff() {
        val now = 1_760_000_000_000L
        assertTrue("the first run checks at once", Hibf.due(Hibf.WatchState(), Hibf.Mode.IDLE, now))
        val checked = Hibf.WatchState(lastCheck = now - 2 * Hibf.HOUR)
        assertFalse(Hibf.due(checked, Hibf.Mode.LEAKY, now)); assertTrue(Hibf.due(checked, Hibf.Mode.LEAKY, now + Hibf.HOUR))
        val b1 = Hibf.backoff(checked, now, null)
        assertEquals(Hibf.HOUR, b1.backoffMs); assertEquals(now + Hibf.HOUR, b1.blockedUntil)
        assertFalse(Hibf.due(b1, Hibf.Mode.LEAKY, now + 30 * 60_000))
        val b2 = Hibf.backoff(b1, now, null); assertEquals(2 * Hibf.HOUR, b2.backoffMs)
        var b = b2; repeat(10) { b = Hibf.backoff(b, now, null) }; assertEquals(Hibf.DAY, b.backoffMs)
        val ra = Hibf.backoff(checked, now, 600); assertEquals(now + 600_000, ra.blockedUntil)
        assertEquals(now + Hibf.HOUR, Hibf.nextCheck(b1, Hibf.Mode.LEAKY))
        // the state survives Prefs
        assertEquals(b2, Hibf.WatchState.decode(b2.encode()))
        assertEquals(Hibf.WatchState(), Hibf.WatchState.decode("not json"))
    }

    @Test fun leakyAgencies() {
        val agencies = Json.parseToJsonElement("""[{"tokens":["chehalis"],"state":"WA","files":2,"records":900,"file":"Chehalis WA PD_Network_Audit.csv","url":"https://www.muckrock.com/x/"},
            {"tokens":["lewis"],"state":"WA","file":"Lewis County Sheriff.csv","url":""}]""").jsonArray
        assertEquals("Chehalis WA PD_Network_Audit.csv · https://www.muckrock.com/x/", Hibf.leakyMatch("Chehalis Police Department", null, agencies))
        assertEquals("Lewis County Sheriff.csv", Hibf.leakyMatch("Lewis County Sheriff's Office", "WA", agencies))
        assertNull("another state's Lewis County", Hibf.leakyMatch("Lewis County Sheriff", "WV", agencies))
        assertNull(Hibf.leakyMatch("Morgantown Police Department", null, agencies))
        assertNull(Hibf.leakyMatch("Police Department", null, agencies))
        assertEquals(listOf("lewis") to "WA", Hibf.nameTokens("Lewis County Sheriff's Office WA", true))
        assertEquals(listOf("chehalis"), Hibf.nameTokens("Chehalis WA PD_Network_Audit_12_1_2024", true).first)
        assertEquals("TX", Hibf.stateFromText("100 Main St, Austin, TX 78701"))
        assertNull(Hibf.stateFromText("Main St"))
    }

    @Test fun registeredPlatesFromTheDesktop() {
        val o = Json.parseToJsonElement("""{"plates":[{"plate":"XYZ2345","displayPlate":"XYZ-2345","active":true},{"plate":"OLD1","displayPlate":"OLD 1","active":false}],"count":2}""").jsonObject
        assertEquals(listOf("XYZ-2345" to "XYZ2345", "OLD 1" to "OLD1"), Hibf.plateList(o))
        assertEquals("XYZ-2345", Hibf.activePlate(o))
    }
}
