// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import java.security.MessageDigest
import java.time.Instant
import java.time.LocalDateTime
import java.time.OffsetDateTime
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

/**
 * HaveIBeenFlocked plate searches (docs/SIGHTINGS.md §4), the pure part: hashing, variants, paging, result → event,
 * the phone's schedule and the request floors. The network is behind [Transport] so unit tests never touch it.
 */
object Hibf {
    const val SEARCH_URL = "https://haveibeenflocked.com/api/search/text"
    const val SITE = "https://haveibeenflocked.com/"
    const val MAX_VARIANTS_PER_FORM = 10
    const val MIN_GAP_MS = 10_000L
    const val MAX_PER_DAY = 30
    const val BACKOFF_MIN_MS = 3_600_000L
    const val BACKOFF_MAX_MS = 24 * 3_600_000L
    const val HOUR = 3_600_000L
    const val DAY = 24 * HOUR

    private val json = Json { ignoreUnknownKeys = true; isLenient = true }

    fun sha256Hex(s: String): String = sha256Hex(s.toByteArray(Charsets.UTF_8))
    fun sha256Hex(b: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it) }

    /** The site's k-anonymity prefix: the first 8 hex of SHA-256 of the variant lowercased and trimmed. */
    fun prefix(variant: String): String = fullHash(variant).take(8)
    fun fullHash(variant: String): String = sha256Hex(variant.lowercase().trim())

    /**
     * The site's own O↔0 / I↔1 expansion (its `a1(plate, 10)`): upper-case, every O/0 and I/1 position tried both ways
     * (the first such position varying slowest), the plate as typed first, at most [max].
     */
    fun expand(form: String, max: Int = MAX_VARIANTS_PER_FORM): List<String> {
        val o = form.uppercase()
        val slots = o.indices.mapNotNull { i -> when (o[i]) { 'O', '0' -> i to charArrayOf('O', '0'); 'I', '1' -> i to charArrayOf('I', '1'); else -> null } }
        if (slots.isEmpty()) return listOf(o)
        val seen = LinkedHashSet<String>(); seen += o
        val buf = o.toCharArray()
        fun rec(k: Int) {
            if (k == slots.size) { seen += String(buf); return }
            val (idx, chars) = slots[k]
            for (c in chars) { buf[idx] = c; rec(k + 1) }
        }
        rec(0)
        return (listOf(o) + seen.filter { it != o }).take(max)
    }

    /** The forms the site would search (its own rule): the display form with whitespace and '&' removed, and the letters-and-digits-only form. */
    fun forms(displayPlate: String): List<String> {
        val display = displayPlate.replace(Regex("""[\s&]"""), "")
        val alnum = display.filter { it.isLetterOrDigit() }
        val out = ArrayList<String>()
        if (display.isNotEmpty()) out += display
        if (alnum.isNotEmpty() && !alnum.equals(display, ignoreCase = true)) out += alnum
        return out
    }

    /** One hashed variant: [plate] is the display plate it came from, [text] the variant as expanded. */
    data class Variant(val plate: String, val text: String, val prefix: String, val full: String)

    /** Every variant of every plate (both forms, each expanded ≤ 10), unique by full hash, in order. */
    fun variants(displayPlates: List<String>): List<Variant> {
        val out = ArrayList<Variant>(); val have = HashSet<String>()
        for (p in displayPlates) for (form in forms(p)) for (v in expand(form, MAX_VARIANTS_PER_FORM)) {
            val full = fullHash(v)
            if (have.add(full)) out += Variant(p, v, full.take(8), full)
        }
        return out
    }

    fun prefixes(vars: List<Variant>): List<String> = vars.map { it.prefix }.distinct()

    fun requestBody(prefixes: List<String>, cursor: String?): String = buildJsonObject {
        putJsonArray("plates") { prefixes.forEach { add(JsonPrimitive(it)) } }
        put("cursor", if (cursor == null) JsonNull else JsonPrimitive(cursor))
    }.toString()

    // ── transport ──
    data class HttpResult(val code: Int, val body: String, val retryAfterS: Long? = null)
    fun interface Transport { fun post(url: String, body: String): HttpResult }

    data class Page(val results: List<JsonObject>, val nextCursor: String?, val hasMore: Boolean, val total: Int?)

    fun parsePage(body: String): Page {
        val o = json.parseToJsonElement(body).jsonObject
        val res = (o["results"] as? JsonArray)?.mapNotNull { it as? JsonObject } ?: emptyList()
        val cur = (o["nextCursor"] as? JsonPrimitive)?.takeIf { it !is JsonNull }?.contentOrNull
        return Page(res, cur, (o["hasMore"] as? JsonPrimitive)?.booleanOrNull ?: false, (o["total"] as? JsonPrimitive)?.contentOrNull?.toIntOrNull())
    }

    sealed class SearchOutcome {
        data class Ok(val rows: List<JsonObject>, val requests: Int) : SearchOutcome()
        /** 429: [retryAfterS] from the header when it had one. */
        data class RateLimited(val retryAfterS: Long?, val rows: List<JsonObject>, val requests: Int) : SearchOutcome()
        data class Failed(val code: Int, val message: String, val rows: List<JsonObject>, val requests: Int) : SearchOutcome()
        /** The daily cap ran out mid-way: what was fetched so far. */
        data class Capped(val rows: List<JsonObject>, val requests: Int) : SearchOutcome()
    }

    /**
     * One plate's search, every page (`hasMore` / `nextCursor`). [mayRequest] is asked before each request (the daily
     * cap) and [pace] waits out the ≥ 10 s floor; both come from [Limiter] in the worker and are fakes in the tests.
     * A 404 is "no results" (the site answers it that way).
     */
    fun search(prefixes: List<String>, transport: Transport, mayRequest: () -> Boolean, pace: () -> Unit, maxPages: Int = 26): SearchOutcome {
        val rows = ArrayList<JsonObject>()
        var cursor: String? = null
        var n = 0
        while (n < maxPages) {
            if (!mayRequest()) return SearchOutcome.Capped(rows, n)
            pace()
            val r = try { transport.post(SEARCH_URL, requestBody(prefixes, cursor)) } catch (e: Exception) { return SearchOutcome.Failed(-1, e.message ?: e.javaClass.simpleName, rows, n + 1) }
            n++
            when {
                r.code == 404 -> return SearchOutcome.Ok(rows, n)
                r.code == 429 -> return SearchOutcome.RateLimited(r.retryAfterS, rows, n)
                r.code !in 200..299 -> return SearchOutcome.Failed(r.code, r.body.take(200), rows, n)
            }
            val page = try { parsePage(r.body) } catch (e: Exception) { return SearchOutcome.Failed(r.code, "unreadable answer", rows, n) }
            rows += page.results
            if (!page.hasMore || page.nextCursor.isNullOrEmpty() || page.nextCursor == cursor) return SearchOutcome.Ok(rows, n)
            cursor = page.nextCursor
        }
        return SearchOutcome.Ok(rows, n)
    }

    // ── results → plate_search events (§4.2, §4.3, §1.1) ──
    fun JsonObject.s(k: String): String = (this[k] as? JsonPrimitive)?.takeIf { it !is JsonNull }?.contentOrNull.orEmpty()

    /** `hibf:` + first 24 hex of SHA-256 of `org_id|search_time_utc|license_plate_hash|case_number|reason|upload_id`. */
    fun searchUid(row: JsonObject): String =
        "hibf:" + sha256Hex(listOf("org_id", "search_time_utc", "license_plate_hash", "case_number", "reason", "upload_id").joinToString("|") { row.s(it) }).take(24)

    /** A result row that is (or may be) ours: [verified] = its full hash equals one of our variants'. */
    data class Match(val row: JsonObject, val plate: String, val verified: Boolean)

    private val FULL = Regex("^[0-9a-f]{64}$")

    /**
     * §4.2: a row is ours when its `license_plate_hash` (full SHA-256) equals one of our variants' full hashes (another
     * plate behind the same prefix is dropped); a row without a full hash is kept unverified, under the plate whose
     * prefix it matches (else the first plate).
     */
    fun filterResults(rows: List<JsonObject>, vars: List<Variant>): List<Match> {
        val out = ArrayList<Match>()
        for (row in rows) {
            val h = row.s("license_plate_hash").trim().lowercase()
            if (FULL.matches(h)) { vars.firstOrNull { it.full == h }?.let { out += Match(row, it.plate, true) }; continue }
            var plate = vars.firstOrNull()?.plate.orEmpty()
            if (h.isNotEmpty()) vars.firstOrNull { it.prefix.startsWith(h.take(8)) }?.let { plate = it.plate }
            out += Match(row, plate, false)
        }
        return out
    }

    data class SearchEvent(val uid: String, val plate: String, val timeMs: Long, val timeLocal: String, val agency: String, val details: String,
                           val sourceUrl: String, val sourceName: String, val confidence: Int, val metrics: JsonObject, val raw: JsonObject)

    private val localIso: DateTimeFormatter = DateTimeFormatter.ISO_LOCAL_DATE_TIME

    /** `search_time_utc` → epoch ms (ISO with or without zone — without one it is UTC by the field's name — or epoch s / ms); null when unreadable. */
    fun parseUtc(s: String): Long? {
        val t = s.trim(); if (t.isEmpty()) return null
        t.toLongOrNull()?.let { return if (it > 100_000_000_000L) it else it * 1000 }
        runCatching { return OffsetDateTime.parse(t.replace(' ', 'T')).toInstant().toEpochMilli() }
        runCatching { return Instant.parse(t.replace(' ', 'T')).toEpochMilli() }
        runCatching { return LocalDateTime.parse(t.replace(' ', 'T')).toInstant(ZoneOffset.UTC).toEpochMilli() }
        runCatching { return java.time.LocalDate.parse(t.take(10)).atStartOfDay().toInstant(ZoneOffset.UTC).toEpochMilli() }
        return null
    }

    fun toLocalIso(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String = LocalDateTime.ofInstant(Instant.ofEpochMilli(ms), zone).withNano(0).format(localIso)

    /** §4.2's plate_search row (the desktop's Hibf::searchEvent). */
    fun toEvent(m: Match, now: Long, zone: ZoneId = ZoneId.systemDefault()): SearchEvent {
        val r = m.row
        val agency = r.s("org_name").ifEmpty { r.s("agency_name") }
        val reason = r.s("reason")
        val url = r.s("source_url").trim().takeIf { it.startsWith("https://") || it.startsWith("http://") } ?: SITE
        val srcOrg = r.s("source_org_name").ifEmpty { agency }
        val caseNo = r.s("case_number")
        val parsed = parseUtc(r.s("search_time_utc"))
        val ms = parsed ?: now
        val metrics = JsonObject(r + ("hashVerified" to JsonPrimitive(m.verified)))
        return SearchEvent(searchUid(r), m.plate, ms, if (parsed != null) toLocalIso(ms, zone) else r.s("search_time_utc").ifEmpty { toLocalIso(ms, zone) },
            agency, "${agency.ifEmpty { "An agency" }} searched ${m.plate} — ${reason.ifEmpty { "no reason given" }}${if (caseNo.isEmpty()) "" else " (case $caseNo)"}",
            url, "HaveIBeenFlocked · ${srcOrg.ifEmpty { "released" }} audit log", if (m.verified) 100 else 50, metrics, r)
    }

    // ── schedule (§4.5, the phone's part) ──
    enum class Mode(val key: String, val intervalMs: Long) {
        LEAKY("leaky", 3 * HOUR), FLOCK("flock", 12 * HOUR), DRIVING("driving", DAY), IDLE("idle", 7 * DAY)
    }

    /**
     * Which interval applies now: within 72 h after a leaky-camera pass 3 h, after a Flock-network ALPR pass 12 h, while
     * driving (a vehicle / > 20 km/h in the last 30 min) 24 h, else 7 days.
     */
    fun mode(now: Long, lastDrivingMs: Long?, lastFlockPassMs: Long?, lastLeakyPassMs: Long?): Mode = when {
        lastLeakyPassMs != null && now - lastLeakyPassMs <= 72 * HOUR -> Mode.LEAKY
        lastFlockPassMs != null && now - lastFlockPassMs <= 72 * HOUR -> Mode.FLOCK
        lastDrivingMs != null && now - lastDrivingMs <= 30 * 60_000L -> Mode.DRIVING
        else -> Mode.IDLE
    }

    /** The watcher's persistent state (Prefs `hibf_watch`, the desktop's kv of the same name plus the floors). */
    data class WatchState(
        val lastCheck: Long = 0, val nextCheck: Long = 0, val mode: String = "", val lastStatus: String = "", val lastError: String = "", val hits: Int = 0,
        val backoffMs: Long = 0, val blockedUntil: Long = 0, val day: String = "", val requestsToday: Int = 0, val lastRequestAt: Long = 0,
    ) {
        fun encode(): String = buildJsonObject {
            put("lastCheck", lastCheck); put("nextCheck", nextCheck); put("mode", mode); put("lastStatus", lastStatus); put("lastError", lastError); put("hits", hits)
            put("backoffMs", backoffMs); put("blockedUntil", blockedUntil); put("day", day); put("requestsToday", requestsToday); put("lastRequestAt", lastRequestAt)
        }.toString()

        companion object {
            fun decode(s: String?): WatchState {
                if (s.isNullOrBlank()) return WatchState()
                return runCatching {
                    val o = json.parseToJsonElement(s).jsonObject
                    fun l(k: String) = o[k]?.jsonPrimitive?.contentOrNull?.toLongOrNull() ?: 0L
                    fun t(k: String) = o[k]?.jsonPrimitive?.contentOrNull.orEmpty()
                    WatchState(l("lastCheck"), l("nextCheck"), t("mode"), t("lastStatus"), t("lastError"), l("hits").toInt(), l("backoffMs"), l("blockedUntil"), t("day"), l("requestsToday").toInt(), l("lastRequestAt"))
                }.getOrDefault(WatchState())
            }
        }
    }

    /** Due now? The first run is immediate; a backoff / Retry-After holds everything until it ends. */
    fun due(s: WatchState, mode: Mode, now: Long): Boolean {
        if (now < s.blockedUntil) return false
        if (s.lastCheck == 0L) return true
        return now >= s.lastCheck + mode.intervalMs
    }

    fun nextCheck(s: WatchState, mode: Mode): Long = maxOf(s.blockedUntil, if (s.lastCheck == 0L) 0L else s.lastCheck + mode.intervalMs)

    /** 429 → honour Retry-After, else ×2 from 1 h up to 24 h; 5xx / network errors back off the same way. */
    fun backoff(s: WatchState, now: Long, retryAfterS: Long?): WatchState {
        val next = (if (s.backoffMs <= 0) BACKOFF_MIN_MS else s.backoffMs * 2).coerceIn(BACKOFF_MIN_MS, BACKOFF_MAX_MS)
        val until = if (retryAfterS != null && retryAfterS > 0) now + retryAfterS * 1000 else now + next
        return s.copy(backoffMs = if (retryAfterS != null && retryAfterS > 0) s.backoffMs else next, blockedUntil = until)
    }

    /** The request floors: ≥ 10 s apart, ≤ 30 a day per device (the day in the device's zone). */
    class Limiter(var state: WatchState, private val clock: () -> Long, private val sleep: (Long) -> Unit, private val zone: ZoneId = ZoneId.systemDefault()) {
        private fun today(): String = Instant.ofEpochMilli(clock()).atZone(zone).toLocalDate().toString()
        fun mayRequest(): Boolean {
            val d = today()
            if (state.day != d) state = state.copy(day = d, requestsToday = 0)
            return state.requestsToday < MAX_PER_DAY
        }
        fun pace() {
            val wait = state.lastRequestAt + MIN_GAP_MS - clock()
            if (wait > 0) sleep(wait)
            state = state.copy(lastRequestAt = clock(), requestsToday = state.requestsToday + 1)
        }
    }

    // ── §4.4 leaky agencies (the desktop's Hibf::nameTokens / leakyMatch / stateFromText) ──
    private val STOP: Set<String> = (("pd police department dept sheriff sheriffs office county city of the network audit audits organization org search searches csv " +
        "flock safety foi and for request requests records record public general order sharing data files file report reports clean redacted responsive alpr lpr " +
        "plate plates license camera cameras logs log transparency portal news township twp village town borough commission comm parish division agency law " +
        "enforcement services state patrol highway marshal marshals constable precinct inc llc through from copy final export exported full list lookup lookups " +
        "history with xlsx pdf docs document documents response results result part page sheet jurisdiction agencies department's pds dps sos fy ytd all usage " +
        "access information info attachment attachments released release updated update version solutions transportation dot district attorney attorneys " +
        "courts court municipal regional school schools isd campus cpra foia pra removed authority port airport transit fire rescue security private community " +
        "association hoa corp corporation company group partners properties property management store stores mall center retail per via fwd re email emails " +
        "folder zip detail details january february march april may june july august september october november december jan feb mar apr jun jul aug sep sept " +
        "oct nov dec").split(' ').filter { it.isNotEmpty() }).toSet()
    private val STATES = setOf("AL", "AK", "AZ", "AR", "CA", "CO", "CT", "DE", "FL", "GA", "HI", "ID", "IL", "IN", "IA", "KS", "KY", "LA", "ME", "MD", "MA", "MI", "MN", "MS",
        "MO", "MT", "NE", "NV", "NH", "NJ", "NM", "NY", "NC", "ND", "OH", "OK", "OR", "PA", "RI", "SC", "SD", "TN", "TX", "UT", "VT", "VA", "WA", "WV", "WI", "WY", "DC")

    /**
     * Words only (digits and punctuation separate them), stop words and months out, tokens of 3+ letters. A two-letter US
     * state code is the state: upper case only when [upperStatesOnly] (file names, operator names), else the last one.
     */
    fun nameTokens(text: String, upperStatesOnly: Boolean): Pair<List<String>, String> {
        val raw = ArrayList<String>(); val cur = StringBuilder()
        for (c in "$text ") { if (c.isLetter() || c == '\'') cur.append(c) else { if (cur.isNotEmpty()) raw += cur.toString(); cur.clear() } }
        val out = ArrayList<String>(); var state = ""
        for (t0 in raw) {
            val t = t0.replace("'", "")
            if (t.length == 2) {
                val up = t.uppercase()
                if (up in STATES && (!upperStatesOnly || t == up)) { if (!upperStatesOnly || state.isEmpty()) state = up }
                continue
            }
            val l = t.lowercase()
            if (l.length < 3 || l in STOP) continue
            if (l.endsWith("s") && l.dropLast(1) in STOP) continue
            if (l !in out) out += l
        }
        return out to state
    }

    /** ", TX" / " TX 75001" at the end of an address → "TX". */
    fun stateFromText(text: String): String? {
        val m = Regex("""(?:^|[,\s])([A-Z]{2})(?:\s+\d{5}(?:-\d{4})?)?\s*$""").find(text.trim()) ?: return null
        return m.groupValues[1].takeIf { it in STATES }
    }

    /**
     * The camera's agency publishes its audit logs (§4.4): every place token of its operator is among one file's tokens
     * and, when both have a state, the states agree. Returns what matched ("<file> · <url>"), null when not leaky.
     */
    fun leakyMatch(operator: String?, cameraState: String?, agencies: JsonArray): String? {
        if (operator.isNullOrBlank()) return null
        val (op, opState) = nameTokens(operator, true)
        if (op.isEmpty()) return null
        val state = cameraState?.takeIf { it.isNotEmpty() } ?: opState
        var best: String? = null; var bestScore = 0.0
        for (v in agencies) {
            val a = v as? JsonObject ?: continue
            val toks = (a["tokens"] as? JsonArray)?.mapNotNull { (it as? JsonPrimitive)?.contentOrNull }?.toSet() ?: continue
            if (!op.all { it in toks }) continue
            val st = a.s("state")
            if (state.isNotEmpty() && st.isNotEmpty() && state != st) continue
            val score = op.size.toDouble() / toks.size.toDouble() + if (state.isEmpty() || st.isEmpty()) 0.0 else 1.0
            if (score > bestScore) {
                bestScore = score
                best = a.s("file") + a.s("url").let { if (it.isEmpty()) "" else " · $it" }
            }
        }
        return best
    }

    /** Every registered plate the desktop serves (`GET /api/v1/plates`): display form, normalized — all of them, like the desktop's watch. */
    fun plateList(o: JsonObject?): List<Pair<String, String>> =
        (o?.get("plates") as? JsonArray)?.mapNotNull { e ->
            val p = e as? JsonObject ?: return@mapNotNull null
            (p.s("displayPlate").ifEmpty { p.s("plate") } to p.s("plate")).takeIf { it.first.isNotBlank() }
        }?.distinctBy { it.first } ?: emptyList()

    /** The active plate (the one a pass is recorded for), as displayed. */
    fun activePlate(o: JsonObject?): String? =
        (o?.get("plates") as? JsonArray)?.mapNotNull { it as? JsonObject }?.firstOrNull { ((it["active"] as? JsonPrimitive)?.booleanOrNull ?: true) }
            ?.let { it.s("displayPlate").ifEmpty { it.s("plate") } }?.takeIf { it.isNotBlank() }
}
