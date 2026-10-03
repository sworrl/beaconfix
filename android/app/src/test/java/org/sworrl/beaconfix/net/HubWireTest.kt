package org.sworrl.beaconfix.net

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.Interceptor
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import okhttp3.HttpUrl.Companion.toHttpUrl
import okio.Buffer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.BeaconFixApi
import org.sworrl.beaconfix.data.api.DevicePositionBody
import org.sworrl.beaconfix.data.api.LinkedDevice
import org.sworrl.beaconfix.sync.HubSync
import retrofit2.Retrofit
import retrofit2.converter.kotlinx.serialization.asConverterFactory
import java.io.IOException
import java.util.Date

/**
 * The client half of BFS3 end to end, against an in-process hub (an interceptor that never touches the network): it
 * opens what the phone sealed exactly as the spec says, keeps a replay set, and seals its answers.
 */
class HubWireTest {
    private class MemStore(var disk: Long = 0) : CounterAllocator.Store {
        override fun reserved() = disk
        override fun reserve(upTo: Long) { disk = upTo }
    }

    private val ssk = Bfs3.newPrivateKey(); private val spk = Bfs3.publicKey(ssk)
    private val dsk = Bfs3.newPrivateKey(); private val dpk = Bfs3.publicKey(dsk)
    private val did = Bfs3.deviceId(dpk)
    private val store = MemStore()
    private fun session(st: CounterAllocator.Store = store) = Bfs3Session(HubConfig("https://hub.test/", "Pixel", did, spk, Bfs3.fingerprint(spk), 0), Bfs3.rootKeyOf(dsk, spk), CounterAllocator(st, 4))

    /** The hub, as docs/SECURE-API.md describes it. */
    private inner class FakeHub : Interceptor {
        val rk = Bfs3.rootKey(Bfs3.x25519(ssk, dpk), spk, dpk)
        val seen = HashSet<Long>()
        var lastPlain = ""; var lastTarget = ""; var lastMethod = ""; var lastAuth: String? = null; var counters = ArrayList<Long>()
        var answer: (String, String) -> Pair<Int, String> = { _, _ -> 200 to "{\"ok\":true}" }
        var tamper = false; var counterOffset = 0L; var unsealed: Int? = null; var date: Long? = null; var serverTime: Long? = null
        override fun intercept(chain: Interceptor.Chain): Response {
            val req = chain.request()
            fun reply(code: Int, body: ByteArray, h: Map<String, String> = emptyMap()) = Response.Builder().request(req).protocol(Protocol.HTTP_1_1).code(code).message("x")
                .apply { h.forEach { (k, v) -> header(k, v) }; date?.let { header("Date", httpDate(it)) }; serverTime?.let { header(Bfs3.H_SERVER_TIME, (it / 1000).toString()) } }
                .body(body.toResponseBody(Bfs3.SEALED_TYPE.toMediaType())).build()
            unsealed?.let { return reply(it, "{\"error\":\"unauthorized\"}".toByteArray()) }
            assertTrue(req.url.encodedPath.startsWith("/api/v3/"))
            lastAuth = req.header("Authorization")
            val c = req.header(Bfs3.H_COUNTER)!!.toLong(); val ts = req.header(Bfs3.H_TIME)!!.toLong()
            val nonce = Bfs3.unb64u(req.header(Bfs3.H_NONCE)!!)
            // the hub reads the body when there is one, else X-BF-Seal (spec rev. 2)
            val bodyBytes = req.body?.let { b -> Buffer().also { b.writeTo(it) }.readByteArray() } ?: ByteArray(0)
            val sealed = if (bodyBytes.isEmpty()) Bfs3.unb64u(req.header(Bfs3.H_SEALED)!!) else bodyBytes
            if (bodyBytes.isNotEmpty()) { assertEquals(Bfs3.SEALED_TYPE, req.body!!.contentType().toString()); assertNull(req.header(Bfs3.H_SEALED)) }
            if (!seen.add(c)) return reply(401, "{\"error\":\"unauthorized\"}".toByteArray())
            val target = HubUrls.target(req.url)
            val plain = try { Bfs3.openRequest(rk, req.header(Bfs3.H_DEVICE)!!, req.method, target, c, ts, nonce, sealed) } catch (e: Bfs3Exception) { return reply(401, "{}".toByteArray()) }
            lastPlain = String(plain); lastTarget = target; lastMethod = req.method; counters += c
            val (code, body) = answer(req.method, target)
            val n = Bfs3.random(12)
            var out = Bfs3.sealResponse(rk, did, code, c, n, body.toByteArray())
            if (tamper) out = out.copyOf().also { it[0] = (it[0].toInt() xor 1).toByte() }
            return reply(code, out, mapOf(Bfs3.H_COUNTER to (c + counterOffset).toString(), Bfs3.H_NONCE to Bfs3.b64u(n)))
        }
    }

    private fun httpDate(ms: Long) = java.text.SimpleDateFormat("EEE, dd MMM yyyy HH:mm:ss 'GMT'", java.util.Locale.US).apply { timeZone = java.util.TimeZone.getTimeZone("GMT") }.format(Date(ms))
    private val hub = FakeHub()
    private val wire = Bfs3Wire()
    private fun client(s: Bfs3Session = session()) = OkHttpClient.Builder().addInterceptor(Bfs3Interceptor({ s }, wire)).addInterceptor(hub).build()
    private val json = "application/json".toMediaType()

    @Test fun postIsSealedToV3AndTheAnswerOpened() {
        val c = client()
        val body = "{\"lat\":40.0031,\"lon\":-75.0686,\"acc\":4.0,\"time\":\"2026-10-01T19:40:00\"}"
        val r = c.newCall(Request.Builder().url("https://hub.test/api/v1/devices/position").header("Authorization", "Bearer x").post(body.toRequestBody(json)).build()).execute()
        assertEquals(200, r.code); assertEquals("{\"ok\":true}", r.body!!.string())
        assertEquals(body, hub.lastPlain); assertEquals("/api/v3/devices/position", hub.lastTarget); assertNull(hub.lastAuth)
    }

    @Test fun getCarriesTheTagAndTheQueryIsPartOfTheTarget() {
        val r = client().newCall(Request.Builder().url("https://hub.test/api/v1/db/changes?since=12&limit=500").build()).execute()
        assertEquals(200, r.code)
        assertEquals("/api/v3/db/changes?since=12&limit=500", hub.lastTarget); assertEquals("", hub.lastPlain)
    }

    @Test fun retrofitRoutesAndDtosAreReusedUnchanged() = runBlocking {
        hub.answer = { _, t -> 200 to if (t.endsWith("/devices/positions")) """{"devices":[{"device":"steamdeck","kind":"laptop","lat":37.77,"lon":-122.42,"acc":12.0,"ageS":40}],"count":1}""" else "{\"ok\":true}" }
        val api = Retrofit.Builder().baseUrl("https://hub.test/").client(client()).addConverterFactory(ApiFactory.json.asConverterFactory(json)).build().create(BeaconFixApi::class.java)
        val pos = api.devicesPositions(HubClient.AUTH)
        assertEquals("steamdeck", pos.body()!!.devices.single().device)
        assertTrue(api.devicePosition(HubClient.AUTH, DevicePositionBody(37.77, -122.42, 5.0, "2026-10-01T19:40:00", 3, "wifi")).isSuccessful)
        val sent = Json.parseToJsonElement(hub.lastPlain).jsonObject
        assertEquals("wifi", sent["source"]!!.jsonPrimitive.content); assertEquals("POST", hub.lastMethod)
        assertEquals(listOf(1L, 2L), hub.counters)
    }

    /** Plate events through the hub: the record-only db/sync body sealed to /api/v3/db/sync, and the db/changes rows opened. */
    @Test fun plateEventsGoSealedThroughDbSyncAndComeBackInDbChanges() = runBlocking {
        hub.answer = { _, t -> 200 to when {
            t.startsWith("/api/v3/db/sync") -> """{"accepted":{"observations":0,"aps":0,"fixes":0,"anchors":0,"plateEvents":1},"cursor":812,"refitQueued":false,"device":"Test phone"}"""
            t.startsWith("/api/v3/db/changes") -> """{"since":800,"aps":[],"observations":[],"fixes":[],"anchors":[],"count":1,"cursor":812,"more":false,
                "plateEvents":[{"uid":"hibf:0123456789abcdef01234567","kind":"plate_search","plate":"ABC-1234","time":"2025-10-09T08:00:00","agency":"Example County SO",
                "source":"haveibeenflocked","confidence":100,"leaky":1,"metrics":{"reason":"test"},"raw":{"org_name":"Example County SO"},"seq":811}]}"""
            else -> "{}"
        } }
        val api = Retrofit.Builder().baseUrl("https://hub.test/").client(client()).addConverterFactory(ApiFactory.json.asConverterFactory(json)).build().create(HubApi::class.java)
        val ev = org.sworrl.beaconfix.data.api.PlateEventDto(uid = "pass:osm:node/42:29333332", kind = "camera_pass", time = "2025-10-09T08:53:20", cameraId = "osm:node/42",
            source = "phone_live", confidence = 90.0, device = "Test phone")
        val r = api.sync(org.sworrl.beaconfix.sync.HubPlates.body("Test phone", null, listOf(ev)))
        assertEquals("POST", hub.lastMethod); assertEquals("/api/v3/db/sync", hub.lastTarget)
        val sent = Json.parseToJsonElement(hub.lastPlain).jsonObject
        assertEquals("osm:node/42", sent["plateEvents"]!!.jsonArray.single().jsonObject["camera_id"]!!.jsonPrimitive.content)
        assertEquals(org.sworrl.beaconfix.sync.HubPlates.Outcome(pushed = listOf(ev.uid)), org.sworrl.beaconfix.sync.HubPlates.outcome(listOf(ev.uid), r.body()))
        val c = api.changes("800", 1000).body()!!
        assertEquals("GET", hub.lastMethod); assertEquals("/api/v3/db/changes?since=800&limit=1000", hub.lastTarget)
        assertEquals("812", HubSync.cursorText(c.cursor))
        assertEquals(listOf("hibf:0123456789abcdef01234567"), org.sworrl.beaconfix.sync.HubPlates.decode(c.plateEvents).map { it.uid })
    }

    @Test fun countersGrowAndSurviveARestart() {
        repeat(3) { client().newCall(Request.Builder().url("https://hub.test/api/v1/state").build()).execute().close() }
        // a "restart": a new session over the same counter store — the hub accepts it (no counter reused)
        val r = client(session()).newCall(Request.Builder().url("https://hub.test/api/v1/state").build()).execute()
        assertEquals(200, r.code)
        assertEquals(hub.counters.size, hub.counters.toSet().size)
        assertTrue(hub.counters.zipWithNext().all { (a, b) -> b > a })
        // and a store that forgot its reservation would be refused as a replay — why the reservation is on disk first
        val lost = session(MemStore())
        val r2 = client(lost).newCall(Request.Builder().url("https://hub.test/api/v1/state").build()).execute()
        assertEquals(401, r2.code)
        // …from which the spec's recovery (jump 256 after an unauthenticated 401) gets it back
        assertEquals(200, client(lost).newCall(Request.Builder().url("https://hub.test/api/v1/state").build()).execute().code)
    }

    @Test fun emptyPostsAndDeletesCarryTheTagInTheHeader() {
        val c = client()
        assertEquals(200, c.newCall(Request.Builder().url("https://hub.test/api/v1/refresh").post(ByteArray(0).toRequestBody(null)).build()).execute().code)
        assertEquals("POST", hub.lastMethod); assertEquals("", hub.lastPlain)
        assertEquals(200, c.newCall(Request.Builder().url("https://hub.test/api/v1/anchors/a1").delete().build()).execute().code)
        assertEquals("DELETE", hub.lastMethod); assertEquals("/api/v3/anchors/a1", hub.lastTarget)
    }

    @Test fun capabilitiesSpeakTheHubsVocabulary() {
        val st = NodeState(8, rtt = true, rttAz = false, tensor = true, nnapi = false, batteryPct = 64, charging = true, version = "1.5.0", sdk = 35, model = "Google Pixel 10 Pro XL", estimator = 2)
        val j = NodeCapabilities.json(st)
        assertEquals("0", j["compute"]!!.jsonPrimitive.content)          // runs no hub jobs yet: the hub must not count on it
        assertEquals("8", j["cores"]!!.jsonPrimitive.content)
        assertEquals("true", j["mobile"]!!.jsonPrimitive.content); assertEquals("true", j["rtt"]!!.jsonPrimitive.content)
        assertEquals("1", j["wifiScan"]!!.jsonPrimitive.content); assertEquals("phone", j["role"]!!.jsonPrimitive.content)
        assertEquals("64", j["battery"]!!.jsonObject["pct"]!!.jsonPrimitive.content)
        assertTrue(j["jobs"].toString() == "[]")
        val runner = object : JobRunner { override val types = listOf("refit"); override fun eligible(state: NodeState) = state.charging; override suspend fun run(job: kotlinx.serialization.json.JsonObject) = job }
        assertEquals("8", NodeCapabilities.json(st, runner)["compute"]!!.jsonPrimitive.content)
    }

    private fun expectRejected(f: () -> Unit) { try { f(); fail("accepted") } catch (e: HubSecurityException) { /* expected */ } }

    @Test fun aTamperedAnswerIsRejected() {
        hub.tamper = true
        expectRejected { client().newCall(Request.Builder().url("https://hub.test/api/v1/state").build()).execute() }
    }

    @Test fun anAnswerForAnotherCounterIsRejected() {
        hub.counterOffset = 1
        expectRejected { client().newCall(Request.Builder().url("https://hub.test/api/v1/state").build()).execute() }
    }

    @Test fun unsealedErrorsPassUnsealedSuccessDoesNot() {
        hub.unsealed = 401
        val r = client().newCall(Request.Builder().url("https://hub.test/api/v1/state").build()).execute()
        assertEquals(401, r.code); assertEquals("1", r.header(HubClient.UNSEALED))
        hub.unsealed = 200
        expectRejected { client().newCall(Request.Builder().url("https://hub.test/api/v1/state").build()).execute() }
    }

    @Test fun cleartextIsRefused() {
        expectRejected { client().newCall(Request.Builder().url("http://hub.test/api/v1/state").build()).execute() }
    }

    @Test fun theHubClockIsLearnedAndUsed() {
        hub.date = System.currentTimeMillis() + 600_000
        client().newCall(Request.Builder().url("https://hub.test/api/v1/state").build()).execute().close()
        assertTrue(wire.clockSkewS in 590..610)
        assertTrue(kotlin.math.abs(wire.nowS() - (System.currentTimeMillis() / 1000 + 600)) <= 10)
        // the hub's own X-BF-Time wins over Date; a clock within 30 s is left alone
        hub.serverTime = System.currentTimeMillis() - 120_000
        client().newCall(Request.Builder().url("https://hub.test/api/v1/state").build()).execute().close()
        assertTrue(wire.clockSkewS in -130..-110)
        hub.date = null; hub.serverTime = System.currentTimeMillis() + 5_000
        client().newCall(Request.Builder().url("https://hub.test/api/v1/state").build()).execute().close()
        assertEquals(0L, wire.clockSkewS)
    }

    @Test fun hubUrls() {
        assertEquals("https://hub.example.com/", HubUrls.normalise("hub.example.com"))
        assertEquals("https://hub.example.com/", HubUrls.normalise(" https://hub.example.com "))
        assertEquals("https://10.0.0.1:8443/bf/", HubUrls.normalise("https://10.0.0.1:8443/bf"))
        for (bad in listOf("http://hub.example.com", "", "https://", "ftp://x", "https://h/?q=1")) {
            try { HubUrls.normalise(bad); fail("accepted $bad") } catch (e: IllegalArgumentException) { /* expected */ }
        }
        assertEquals("/bf/api/v3/db/changes?since=1", HubUrls.target(HubUrls.v3("https://h/bf/api/v1/db/changes?since=1".toHttpUrl())))
        assertEquals("/healthz", HubUrls.target(HubUrls.v3("https://h/healthz".toHttpUrl())))
    }

    @Test fun publishCadence() {
        val c = HubPresence.Cadence
        assertTrue(c.due(1000, 0, 0, false))                                  // first fix: at once
        assertTrue(!c.due(10_000, 6_000, 0, true)); assertTrue(c.due(11_000, 6_000, 0, true))     // moving: every 5 s
        assertTrue(!c.due(100_000, 6_000, 0, false)); assertTrue(c.due(186_000, 6_000, 0, false)) // stationary: every 3 min
        assertTrue(!c.due(200_000, 6_000, 150_000, true))                      // a minute's pause after a failure
        assertTrue(c.moving(3f, 0.0, 5.0)); assertTrue(c.moving(null, 40.0, 10.0))
        assertTrue(!c.moving(0.2f, 8.0, 5.0)); assertTrue(!c.moving(null, 30.0, 50.0))
    }

    @Test fun mapMergeKeepsTheFreshestPerDevice() {
        val lan = listOf(LinkedDevice("rv-desktop", "desktop", lat = 1.0, lon = 1.0, ageS = 300.0))
        val hubL = listOf(LinkedDevice("rv-desktop", "desktop", lat = 2.0, lon = 2.0, ageS = 20.0), LinkedDevice("steamdeck", "laptop", lat = 3.0, lon = 3.0, ageS = null))
        val m = HubLive.merge(lan, hubL).associateBy { it.device }
        assertEquals(2.0, m["rv-desktop"]!!.lat, 0.0); assertEquals(2, m.size)
        assertTrue(HubLive.wakes("device_online")); assertTrue(HubLive.wakes("fix")); assertTrue(!HubLive.wakes("ap_refit"))
    }

    @Test fun changeRowsBecomeApRows() {
        val row = Json.parseToJsonElement("""{"bssid":"aa:bb:cc:dd:ee:ff","ssid":"RV","freq":5180,"lat":37.77,"lon":-122.42,"acc":12.5,"source":"observed","home":true,"travelling":true,"security":"wpa2","seq":7,"fit":{"n":40,"grade":"B","r95":30.0,"lat":37.77,"lon":-122.42}}""").jsonObject
        val a = HubSync.apOfChange(row)!!
        assertEquals("AA:BB:CC:DD:EE:FF", a.bssid); assertEquals(12.5, a.r!!, 0.0); assertEquals("observed", a.kind); assertEquals("travelling", a.status)
        assertTrue(a.home); assertEquals("B", a.fit!!.grade)
        assertNull(HubSync.apOfChange(Json.parseToJsonElement("""{"bssid":"nope"}""").jsonObject))
        assertEquals("12345", HubSync.cursorText("12345.0")); assertEquals("12345", HubSync.cursorText("12345")); assertEquals("0", HubSync.cursorText("0"))
    }
}
