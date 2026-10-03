package org.sworrl.beaconfix.estimate

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.double
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import kotlin.math.abs
import kotlin.math.max

/**
 * The Kotlin estimator against the desktop's golden vectors (tests/fixtures/estimator_golden.json, written by
 * tests/estimator_golden.cpp --write): the same checks as `estimator_golden --check` — every case, the update streams
 * (the second with a device offset, "offsetDb"),
 * the self-location cases and the helpers. lat/lon/suggestLat/suggestLon within 1e-9°, other numbers within
 * 1e-6·max(1, |want|), strings (grades!) and booleans exactly.
 */
class EstimatorGoldenTest {
    private val root: JsonObject by lazy {
        // unit tests run with the module (android/app) as the working directory
        val f = listOf(File("../../tests/fixtures/estimator_golden.json"), File("../tests/fixtures/estimator_golden.json"), File("tests/fixtures/estimator_golden.json"))
            .firstOrNull { it.isFile } ?: error("tests/fixtures/estimator_golden.json not found from ${File(".").absolutePath}")
        Json.parseToJsonElement(f.readText()).jsonObject
    }

    // ── JSON → estimator inputs (tests/estimator_golden.cpp obsFrom/optFrom/ctxFrom/fitFrom/knownFrom) ──
    private fun JsonObject.d(k: String, def: Double = 0.0): Double = (this[k] as? JsonPrimitive)?.doubleOrNull ?: def
    private fun JsonObject.i(k: String, def: Int = 0): Int = (this[k] as? JsonPrimitive)?.doubleOrNull?.toInt() ?: def
    private fun JsonObject.b(k: String): Boolean = (this[k] as? JsonPrimitive)?.booleanOrNull ?: false
    private fun JsonObject.s(k: String): String = (this[k] as? JsonPrimitive)?.takeIf { it.isString }?.content ?: ""

    private fun obsFrom(j: JsonObject) = Obs(lat = j.d("lat"), lon = j.d("lon"), acc = j.d("acc"), dbm = j.i("dbm"), t = j.d("t").toLong(),
        weight = j.d("weight", 1.0), device = j.s("device"), rangeM = j.d("rangeM", -1.0), rangeSd = j.d("rangeSd", 0.0))

    private fun optFrom(j: JsonObject): Options {
        val o = Options()
        o.defaultN = j.d("defaultN", o.defaultN); o.nSd = j.d("nSd", o.nSd); o.p0Mean = j.d("p0Mean", o.p0Mean)
        o.p0Sd = j.d("p0Sd", o.p0Sd); o.kappa = j.d("kappa", o.kappa); o.bootstrap = j.i("bootstrap", o.bootstrap)
        o.devOffsetSd = j.d("devOffsetSd", o.devOffsetSd)
        return o
    }

    private fun fitFrom(j: JsonObject) = Fit(
        valid = j.b("valid"), kind = j.s("kind"), grade = j.s("grade"), pendingGrade = j.s("pendingGrade"), quality = j.s("quality"), score = j.d("score"),
        lat = j.d("lat"), lon = j.d("lon"), acc = j.d("acc"), r95 = j.d("r95"), cep50 = j.d("cep50"), pWithin25 = j.d("pWithin25"),
        cxx = j.d("cxx"), cxy = j.d("cxy"), cyy = j.d("cyy"), semiMajor = j.d("semiMajor"), semiMinor = j.d("semiMinor"), orientDeg = j.d("orient"),
        p0 = j.d("p0"), pathloss = j.d("pathloss"), n = j.i("n"), vantage = j.i("vantage"), rejected = j.i("rejected"), rms = j.d("rms"),
        rssDop = j.d("rssDop"), crlbR95 = j.d("crlbR95"), rbar = j.d("rbar"), maxGapDeg = j.d("maxGapDeg"), inHull = j.b("inHull"), linRatio = j.d("linRatio"),
        chi2nu = j.d("chi2nu"), sigmaDb = j.d("sigmaDb"), outlierFrac = j.d("outlierFrac"), ess = j.d("ess"), sessions = j.i("sessions"), devices = j.i("devices"),
        spearman = j.d("spearman"), p0RangeCorr = j.d("p0RangeCorr"), dminRatio = j.d("dminRatio"), ambiguous = j.b("ambiguous"), modes = j.i("modes"),
        driftD2 = j.d("driftD2"), extD2 = j.d("extD2"), jackMax = j.d("jackMax"), nisEwma = j.d("nisEwma"), fadingDb = j.d("fadingDb"), moved = j.b("moved"),
        newest = j.d("newest").toLong(), suggestLat = j.d("suggestLat"), suggestLon = j.d("suggestLon"), suggestGain = j.d("suggestGain"),
        cP = j.d("cP"), cG = j.d("cG"), cE = j.d("cE"), cF = j.d("cF"), cFfit = j.d("cFfit"), cS = j.d("cS"), cT = j.d("cT"), cX = j.d("cX"), updated = j.d("updated").toLong(),
    )

    private fun ctxFrom(j: JsonObject): Context {
        val c = Context()
        c.deviceOffset = (j["deviceOffset"] as? JsonObject)?.mapValues { it.value.jsonPrimitive.double } ?: emptyMap()
        c.misses = j["misses"]?.jsonArray?.map { v -> val m = v.jsonObject; Miss(m.d("lat"), m.d("lon"), m.i("count", 1)) } ?: emptyList()
        (j["external"] as? JsonObject)?.let { e -> c.external = External(has = true, lat = e.d("lat"), lon = e.d("lon"), acc = e.d("acc")) }
        c.mobile = j.b("mobile")
        (j["prev"] as? JsonObject)?.let { p -> c.hasPrev = true; c.prev = fitFrom(p) }
        return c
    }

    private fun knownFrom(j: JsonObject) = Known(lat = j.d("lat"), lon = j.d("lon"), acc = j.d("acc"), dbm = j.i("dbm"), p0 = j.d("p0"), pathloss = j.d("pathloss"),
        haveModel = j.b("haveModel"), cxx = j.d("cxx"), cxy = j.d("cxy"), cyy = j.d("cyy"), weight = j.d("weight", 1.0))

    // ── estimator outputs → the same keys fitJson/selfJson write ──
    private fun fitJson(f: Fit): Map<String, Any> = linkedMapOf(
        "valid" to f.valid, "kind" to f.kind, "grade" to f.grade, "pendingGrade" to f.pendingGrade, "quality" to f.quality, "score" to f.score,
        "lat" to f.lat, "lon" to f.lon, "acc" to f.acc, "r95" to f.r95, "cep50" to f.cep50, "pWithin25" to f.pWithin25,
        "cxx" to f.cxx, "cxy" to f.cxy, "cyy" to f.cyy, "semiMajor" to f.semiMajor, "semiMinor" to f.semiMinor, "orient" to f.orientDeg,
        "p0" to f.p0, "pathloss" to f.pathloss, "n" to f.n, "vantage" to f.vantage, "rejected" to f.rejected, "rms" to f.rms,
        "rssDop" to f.rssDop, "crlbR95" to f.crlbR95, "rbar" to f.rbar, "maxGapDeg" to f.maxGapDeg, "inHull" to f.inHull, "linRatio" to f.linRatio,
        "chi2nu" to f.chi2nu, "sigmaDb" to f.sigmaDb, "outlierFrac" to f.outlierFrac, "ess" to f.ess, "sessions" to f.sessions, "devices" to f.devices,
        "spearman" to f.spearman, "p0RangeCorr" to f.p0RangeCorr, "dminRatio" to f.dminRatio, "ambiguous" to f.ambiguous, "modes" to f.modes,
        "driftD2" to f.driftD2, "extD2" to f.extD2, "jackMax" to f.jackMax, "nisEwma" to f.nisEwma, "fadingDb" to f.fadingDb, "moved" to f.moved,
        "newest" to f.newest.toDouble(), "suggestLat" to f.suggestLat, "suggestLon" to f.suggestLon, "suggestGain" to f.suggestGain,
        "cP" to f.cP, "cG" to f.cG, "cE" to f.cE, "cF" to f.cF, "cFfit" to f.cFfit, "cS" to f.cS, "cT" to f.cT, "cX" to f.cX, "updated" to f.updated.toDouble(),
    )
    private fun selfJson(s: SelfFix): Map<String, Any> = linkedMapOf(
        "valid" to s.valid, "lat" to s.lat, "lon" to s.lon, "acc" to s.acc, "rms" to s.rms, "used" to s.used, "rejected" to s.rejected,
        "excluded" to s.excluded, "integrity" to s.integrity, "r95" to s.r95,
    )

    // ── compare (tests/estimator_golden.cpp cmp) ──
    private val bad = ArrayList<String>()
    private fun cmp(where: String, want: JsonObject, got: Map<String, Any>) {
        for ((key, w) in want) {
            val g = got[key]
            val p = w as? JsonPrimitive
            if (p == null) { bad += "$where.$key: want $w (not a primitive)"; continue }
            val wantNumber: Double? = if (p.isString || p.booleanOrNull != null) null else p.doubleOrNull
            when {
                wantNumber != null -> {
                    val b = (g as? Number)?.toDouble()
                    val tol = if (key == "lat" || key == "lon" || key.startsWith("suggestL")) 1e-9 else 1e-6 * max(1.0, abs(wantNumber))
                    if (b == null || !(abs(wantNumber - b) <= tol)) bad += "$where.$key: want %.12g got %s".format(wantNumber, b?.let { "%.12g".format(it) } ?: "none")
                }
                !p.isString && p.booleanOrNull != null -> if (g != p.boolean) bad += "$where.$key: want ${p.boolean} got $g"
                else -> if (g != p.content) bad += "$where.$key: want '${p.content}' got '$g'"
            }
        }
    }

    /** The fixture was written by the same estimator generation (on its own, so a stale stamp does not hide the numbers). */
    @Test fun fixtureIsThisGeneration() {
        assertEquals("fixture and engine generation", Estimator.VERSION, root["estimatorVersion"]!!.jsonPrimitive.int)
    }

    @Test fun matchesTheDesktopEngine() {
        val cases = root["cases"]!!.jsonArray
        assertTrue("fixture has cases", cases.isNotEmpty())
        val fits = ArrayList<Fit>()
        for (v in cases) {
            val c = v.jsonObject
            val obs = c["samples"]!!.jsonArray.map { obsFrom(it.jsonObject) }
            val f = Estimator.fitAp(obs, c.d("now").toLong(), optFrom(c["options"]!!.jsonObject), ctxFrom(c["context"]!!.jsonObject))
            fits += f
            cmp(c.s("name"), c["expect"]!!.jsonObject, fitJson(f))
        }
        for (v in root["updates"]!!.jsonArray) {
            val u = v.jsonObject
            var f = fits[u.i("fromCase")]
            val stream = u["stream"]!!.jsonArray; val steps = u["expect"]!!.jsonArray
            for (i in 0 until stream.size) {
                // the second stream is heard by a device louder than this one: its calibrated offset goes to update()
                f = Estimator.update(f, obsFrom(stream[i].jsonObject), Options(), stream[i].jsonObject.d("offsetDb", 0.0))
                cmp("update[$i]", steps[i].jsonObject, fitJson(f))
            }
        }
        root["self"]!!.jsonArray.forEachIndexed { si, v ->
            val known = v.jsonObject["known"]!!.jsonArray.map { knownFrom(it.jsonObject) }
            cmp("self[$si]", v.jsonObject["expect"]!!.jsonObject, selfJson(Estimator.selfLocate(known)))
        }
        for (v in root["helpers"]!!.jsonArray) {
            val h = v.jsonObject
            val want = JsonObject(mapOf<String, JsonElement>("r" to h["r"]!!, "pAt25" to h["pAt25"]!!))
            cmp("helper", want, mapOf("r" to Estimator.radiusFor(h.d("s1"), h.d("s2"), h.d("p")), "pAt25" to Estimator.probWithin(h.d("s1"), h.d("s2"), 25.0)))
        }
        assertTrue("${bad.size} mismatch(es) over ${cases.size} case(s):\n" + bad.take(80).joinToString("\n"), bad.isEmpty())
    }

    /** The grades on their own, so a failure names the case even when the numbers drift elsewhere. */
    @Test fun gradesMatchExactly() {
        for (v in root["cases"]!!.jsonArray) {
            val c = v.jsonObject
            val obs = c["samples"]!!.jsonArray.map { obsFrom(it.jsonObject) }
            val f = Estimator.fitAp(obs, c.d("now").toLong(), optFrom(c["options"]!!.jsonObject), ctxFrom(c["context"]!!.jsonObject))
            val e = c["expect"]!!.jsonObject
            assertEquals("${c.s("name")} kind", e.s("kind"), f.kind)
            assertEquals("${c.s("name")} grade", e.s("grade"), f.grade)
        }
    }
}
