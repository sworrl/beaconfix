// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.collector

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

/**
 * The surveillance-hardware rule set, data/signatures/surveillance.json (docs/DETECTION.md), shipped as the asset
 * `signatures/surveillance.json` copied from that one file at build time. Pure (no Android types) and evaluated exactly
 * like the desktop's src/flockdetector.cpp: tests/fixtures/surveillance_cases.json holds the cases both must agree on.
 */
class SurveillanceSignatures private constructor(
    val version: Int,
    private val detectTier: Int,
    private val ssidWithFamilyMacTier: Int,
    private val confidence: List<Int>,
    private val classes: Map<String, ClassInfo>,
    private val mac: Array<Map<String, MacRule>>,          // by prefix length in hex digits: 6, 7, 9, 12
    private val ssid: List<RxRule>,
    private val probe: List<RxRule>,
    private val bleName: List<RxRule>,
    private val company: List<CompanyRule>,
    private val service: Map<String, ServiceRule>,
    private val serviceRange: List<RangeRule>,
    private val tnSerial: Regex?,
) {
    data class Detection(
        /** Flock hardware (class flock / raven) at tier >= detectTier: reported as a sighting */
        val isFlock: Boolean = false,
        /** other surveillance / police gear at tier >= detectTier: shown, never reported as a Flock camera */
        val informational: Boolean = false,
        /** -1 no match, 0 weak (corroborates only) … 4 conclusive */
        val tier: Int = -1,
        val cls: String = "",
        val label: String = "",
        val cameraType: String = "",
        val confidence: Int = 0,
        val model: String = "",
        val method: String = "",
        val details: String = "",
        val rules: List<String> = emptyList(),
    )

    /** One manufacturer-data record of a BLE advert */
    data class BleCompany(val id: Int, val data: ByteArray = ByteArray(0))

    private data class ClassInfo(val label: String, val cameraType: String, val flock: Boolean)
    private data class MacRule(val prefix: String, val cls: String, val model: String, val vendor: String, val tier: Int, val family: Boolean)
    private data class RxRule(val id: String, val rx: Regex, val cls: String, val model: String, val tier: Int, val bssidSuffixTier: Int)
    private data class CompanyRule(val id: Int, val hex: String, val cls: String, val model: String, val vendor: String, val tier: Int, val corroboratedTier: Int)
    private data class ServiceRule(val uuid: String, val cls: String, val model: String, val tier: Int)
    private data class RangeRule(val from: Int, val to: Int, val cls: String, val model: String, val tier: Int)
    private data class Hit(val kind: String, val cls: String, val model: String, val rule: String, val text: String, val tier: Int)

    fun evaluateWifi(bssid: String, ssid: String, probedSsids: List<String> = emptyList()): Detection {
        val hits = ArrayList<Hit>()
        val hex = normalizeMac(bssid)
        val m = macRule(hex)
        m?.let { hits += macHit(it) }
        for (r in this.ssid) {
            val mm = r.rx.find(ssid) ?: continue
            var tier = r.tier
            var rule = "ssid:${r.id}"
            var text = "SSID \"$ssid\""
            if (r.bssidSuffixTier >= 0 && hex.length == 12 && mm.groupValues.getOrNull(1)?.equals(hex.takeLast(6), ignoreCase = true) == true) {
                tier = maxOf(tier, r.bssidSuffixTier); rule += "=bssid"; text += " ends in the BSSID"
            }
            hits += Hit("ssid", r.cls, r.model, rule, text, tier)
            if (m != null && m.family && m.cls == r.cls) hits += Hit("ssid+mac", r.cls, r.model, "ssid+mac", "", maxOf(tier, ssidWithFamilyMacTier))
        }
        for (p in probedSsids) for (r in probe) if (r.rx.containsMatchIn(p)) hits += Hit("probe", r.cls, r.model, "probe:${r.id}", "probes for \"$p\"", r.tier)
        return decide(hits, "wifi")
    }

    fun evaluateBle(mac: String, name: String, serviceUuids: List<String> = emptyList(), companies: List<BleCompany> = emptyList()): Detection {
        val hits = ArrayList<Hit>()
        macRule(normalizeMac(mac))?.let { hits += macHit(it) }
        var flockName = false
        for (r in bleName) {
            if (!r.rx.containsMatchIn(name)) continue
            hits += Hit("name", r.cls, r.model, "ble_name:${r.id}", "name \"$name\"", r.tier)
            if (classes[r.cls]?.flock == true) flockName = true
        }
        for (raw in serviceUuids) {
            val u = normalizeUuid(raw)
            if (u.isEmpty()) continue
            val s = service[u]
            if (s != null) { hits += Hit("uuid", s.cls, s.model, "ble_uuid:$u", "service $u", s.tier); continue }
            if (u.length != 4) continue
            val v = u.toInt(16)
            serviceRange.firstOrNull { v in it.from..it.to }?.let { hits += Hit("uuid", it.cls, it.model, "ble_uuid:$u", "service $u", it.tier) }
        }
        val tn = tnSerial != null && (tnSerial.containsMatchIn(name) || companies.any { tnSerial.containsMatchIn(String(it.data, Charsets.ISO_8859_1)) })
        for (c in companies) for (r in company) {
            if (r.id != c.id) continue
            val corroborated = r.corroboratedTier >= 0 && (flockName || tn)
            val how = if (!corroborated) "" else if (tn) "+tn" else "+name"
            hits += Hit("company", r.cls, r.model, "ble_company:${r.hex}$how",
                "company ${r.hex} ${r.vendor}" + (if (!corroborated) "" else if (tn) " with a TN serial" else " with a Flock name"),
                if (corroborated) maxOf(r.tier, r.corroboratedTier) else r.tier)
        }
        return decide(hits, "ble")
    }

    private fun macRule(hex: String): MacRule? {
        if (hex.length != 12) return null
        for (n in intArrayOf(12, 9, 7, 6)) mac[n][hex.substring(0, n)]?.let { return it }   // the longest assignment wins
        return null
    }

    private fun macHit(m: MacRule) = Hit("mac", m.cls, m.model, "mac:${m.prefix}", "MAC ${macText(m.prefix)} ${m.vendor}", m.tier)

    private fun decide(hits: List<Hit>, medium: String): Detection {
        var best: Hit? = null
        for (h in hits) {
            val b = best
            if (b == null || h.tier > b.tier) best = h
            else if (h.tier == b.tier && classes[h.cls]?.flock == true && classes[b.cls]?.flock != true) best = h   // a tie: Flock first
        }
        val win = best ?: return Detection()
        val ci = classes[win.cls]
        val mine = hits.filter { it.cls == win.cls }
        val kinds = listOf("mac", "ssid", "probe", "name", "uuid", "company").filter { k -> mine.any { k in it.kind.split('+') } }
        val model = mine.firstOrNull { it.model.isNotEmpty() && it.tier == win.tier }?.model ?: mine.firstOrNull { it.model.isNotEmpty() }?.model ?: ""
        val texts = mine.map { it.text }.filter { it.isNotEmpty() }.distinct()
        val label = ci?.label ?: win.cls
        return Detection(
            isFlock = ci?.flock == true && win.tier >= detectTier,
            informational = ci?.flock == false && win.tier >= detectTier,
            tier = win.tier, cls = win.cls, label = label, cameraType = ci?.cameraType ?: "",
            confidence = confidence.getOrElse(win.tier.coerceIn(0, 4)) { 0 },
            model = model, method = medium + "_" + kinds.joinToString("+"),
            details = "$label (tier ${win.tier}): " + texts.joinToString(" · "),
            rules = mine.map { it.rule },
        )
    }

    companion object {
        /** A MAC normalised to 12 upper-case hex digits ("" when it is not one); separators : - . or none */
        fun normalizeMac(mac: String): String {
            val sb = StringBuilder(12)
            for (c in mac) {
                if (c == ':' || c == '-' || c == '.' || c.isWhitespace()) continue
                val u = c.uppercaseChar()
                if (u !in '0'..'9' && u !in 'A'..'F') return ""
                sb.append(u)
            }
            return if (sb.length == 12) sb.toString() else ""
        }

        /** 16-bit SIG-base UUIDs → "XXXX", other 128-bit → lower case, "" not a UUID */
        fun normalizeUuid(u: String): String {
            val t = u.trim()
            if (t.length == 4) return t.uppercase()
            if (t.length == 8 && t.startsWith("0000")) return t.substring(4).uppercase()
            if (t.length != 36) return ""
            val l = t.lowercase()
            if (l.startsWith("0000") && l.endsWith("-0000-1000-8000-00805f9b34fb")) return l.substring(4, 8).uppercase()
            return l
        }

        private fun macText(hex: String) = hex.chunked(2).joinToString(":")

        /** Parse the rule set; throws IllegalArgumentException with the reason when it is not valid */
        fun parse(json: String): SurveillanceSignatures {
            val root = Json.parseToJsonElement(json).jsonObject
            fun JsonObject.s(k: String) = (this[k] as? JsonPrimitive)?.takeIf { it.isString }?.content ?: ""
            fun JsonObject.i(k: String, def: Int) = (this[k] as? JsonPrimitive)?.intOrNull ?: def
            fun JsonObject.arr(k: String) = (this[k] as? JsonArray) ?: JsonArray(emptyList())
            require(root.i("format", 0) == 1) { "unknown format ${root.i("format", 0)}" }
            val conf = root.arr("confidence").map { it.jsonPrimitive.intOrNull ?: 0 }
            require(conf.size == 5) { "confidence needs 5 values (tiers 0-4)" }
            val classes = (root["classes"] as? JsonObject ?: JsonObject(emptyMap())).mapValues { (k, v) ->
                val o = v.jsonObject
                ClassInfo(o.s("label").ifEmpty { k }, o.s("cameraType"), (o["flock"] as? JsonPrimitive)?.booleanOrNull == true)
            }
            fun checkClass(cls: String, what: String) = require(cls in classes) { "$what: unknown class \"$cls\"" }
            fun checkTier(t: Int, what: String) = require(t in 0..4) { "$what: bad tier" }
            val mac = Array<MutableMap<String, MacRule>>(13) { HashMap() }
            for (v in root.arr("mac")) {
                val o = v.jsonObject
                val raw = o.s("prefix")
                val hex = raw.uppercase().filter { it in '0'..'9' || it in 'A'..'F' }
                // 24 / 28 / 36 / 48 bits. 70:B3:D5 is the IEEE MA-S block: only its 36-bit assignments identify anyone
                require(hex.length in setOf(6, 7, 9, 12)) { "mac $raw: a prefix is 24, 28, 36 or 48 bits" }
                require(!(hex.length == 6 && hex == "70B3D5")) { "mac $raw: the bare MA-S block identifies no vendor" }
                val r = MacRule(hex, o.s("class"), o.s("model"), o.s("vendor"), o.i("tier", -1), (o["family"] as? JsonPrimitive)?.booleanOrNull == true)
                checkTier(r.tier, "mac $raw"); checkClass(r.cls, raw)
                mac[hex.length][hex] = r
            }
            fun rxList(a: JsonArray, what: String) = a.map { v ->
                val o = v.jsonObject
                val pattern = o.s("regex")
                require(pattern.isNotEmpty()) { "$what ${o.s("id")}: bad regex" }
                val r = RxRule(o.s("id"), Regex(pattern), o.s("class"), o.s("model"), o.i("tier", -1), o.i("bssidSuffixTier", -1))
                checkTier(r.tier, "$what ${r.id}"); checkClass(r.cls, "$what ${r.id}")
                r
            }
            val ble = root["ble"] as? JsonObject ?: JsonObject(emptyMap())
            val company = ble.arr("company").map { v ->
                val o = v.jsonObject
                val r = CompanyRule(o.i("id", -1), o.s("hex"), o.s("class"), o.s("model"), o.s("vendor"), o.i("tier", -1), o.i("corroboratedTier", -1))
                require(r.id in 0..0xFFFF) { "ble company ${r.hex}: bad id" }
                checkTier(r.tier, "ble company ${r.hex}"); checkClass(r.cls, r.hex)
                r
            }
            val service = HashMap<String, ServiceRule>()
            for (v in ble.arr("service")) {
                val o = v.jsonObject
                val u = o.s("uuid")
                require(u.length == 4 || u.length == 36) { "ble service $u: bad uuid" }
                val r = ServiceRule(if (u.length == 4) u.uppercase() else u.lowercase(), o.s("class"), o.s("model"), o.i("tier", -1))
                checkTier(r.tier, "ble service $u"); checkClass(r.cls, u)
                service[r.uuid] = r
            }
            val ranges = ble.arr("serviceRange").map { v ->
                val o = v.jsonObject
                val from = o.s("from").toIntOrNull(16); val to = o.s("to").toIntOrNull(16)
                require(from != null && to != null && from <= to) { "ble serviceRange: bad bounds" }
                val r = RangeRule(from, to, o.s("class"), o.s("model"), o.i("tier", -1))
                checkTier(r.tier, "ble serviceRange"); checkClass(r.cls, "serviceRange")
                r
            }
            return SurveillanceSignatures(
                version = root.i("version", 0), detectTier = root.i("detectTier", 2), ssidWithFamilyMacTier = root.i("ssidWithFamilyMacTier", 3),
                confidence = conf, classes = classes, mac = Array(13) { mac[it].toMap() },
                ssid = rxList(root.arr("ssid"), "ssid"), probe = rxList(root.arr("probe"), "probe"), bleName = rxList(ble.arr("name"), "ble name"),
                company = company, service = service, serviceRange = ranges,
                tnSerial = ble.s("tnSerialRegex").takeIf { it.isNotEmpty() }?.let { Regex(it) },
            )
        }
    }
}
