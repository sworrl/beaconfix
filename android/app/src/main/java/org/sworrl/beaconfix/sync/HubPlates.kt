// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sync

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.doubleOrNull
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.PlateEventDto
import org.sworrl.beaconfix.data.api.PlateEventsPage
import org.sworrl.beaconfix.net.HubClient
import org.sworrl.beaconfix.net.HubSyncBody

/**
 * Plate events over the hub (docs/SIGHTINGS.md §5): the pure rules [HubSync] follows, unit-tested in HubPlatesTest.
 *
 * Push: records only, in their own `db/sync` body (`{"device","kind","identity","observations":[],"fixes":[],
 * "plateEvents":[…]}`); the hub merges them per §1.1 with `PlateWatch::ingest` and answers
 * `{"accepted":{"plateEvents":n,…},"cursor",…}`. A hub that answers `plateEventUids` (where each event landed, in the
 * order sent, null = refused — as `POST /plate-events` answers `uids`) is followed exactly; otherwise the count decides.
 * Images never go to the hub: they wait for a LAN desktop.
 *
 * Pull: `db/changes` carries `plateEvents` (every column, `raw` included). Their own cursor ([HubStore.plateCursor])
 * follows the feed's while it is not behind it; a phone that pulled the feed before it carried plate events catches up
 * once through the plate-only feed (`GET plate-events?since=`, the same sequence), then follows again.
 */
object HubPlates {
    /** One push body: well under the hub's request limit; halved on `413`. */
    const val MAX_BYTES = 256 * 1024
    /** The catch-up feed: events per page, pages per run. */
    const val PAGE = 500
    const val MAX_CATCH_UP_PAGES = 20

    fun body(device: String, identity: String?, events: List<PlateEventDto>): HubSyncBody =
        HubSyncBody(device, HubClient.KIND, identity, emptyList(), emptyList(), null, events)

    /** `accepted.plateEvents` of a `db/sync` answer; null when the hub did not report it (a hub that does not take them). */
    fun accepted(answer: JsonObject?): Int? {
        val p = (answer?.get("accepted") as? JsonObject)?.get("plateEvents") as? JsonPrimitive ?: return null
        if (p is JsonNull) return null
        return p.contentOrNull?.toDoubleOrNull()?.toInt()
    }

    /** `plateEventUids` of a `db/sync` answer (where each event landed, in the order sent; null = refused), when the hub sends it. */
    fun uids(answer: JsonObject?): List<String?>? {
        val a = answer?.get("plateEventUids") as? JsonArray ?: return null
        return a.map { (it as? JsonPrimitive)?.takeIf { p -> p !is JsonNull && p.isString }?.content?.ifEmpty { null } }
    }

    /**
     * What a `db/sync` answer means for the events sent (uids in order): [pushed] (their hub flag clears; [renames]
     * first, sent uid → the hub's), [refused] (kept, retried next run), [dropped] (refused by a hub that does store
     * plate events: the hub flag clears, the desktop still gets them), [split] (the count is short: send them one by
     * one to find out which), [unsupported] (no count at all: the hub does not take plate events — all stay queued).
     */
    data class Outcome(val pushed: List<String> = emptyList(), val renames: List<Pair<String, String>> = emptyList(), val refused: List<String> = emptyList(),
                       val dropped: List<String> = emptyList(), val split: Boolean = false, val unsupported: Boolean = false)

    fun outcome(sent: List<String>, answer: JsonObject?): Outcome {
        val u = uids(answer)
        if (u != null && u.size == sent.size) {
            val pushed = ArrayList<String>(); val renames = ArrayList<Pair<String, String>>(); val refused = ArrayList<String>()
            sent.forEachIndexed { i, s -> val landed = u[i]; if (landed == null) refused += s else { pushed += s; if (landed != s) renames += s to landed } }
            return Outcome(pushed, renames, refused)
        }
        val n = accepted(answer) ?: return Outcome(unsupported = true)
        return when {
            n >= sent.size -> Outcome(pushed = sent)
            sent.size > 1 -> Outcome(split = true)
            else -> Outcome(refused = sent)
        }
    }

    /**
     * After sending a short batch one by one: when the hub took some of them it stores plate events, so the ones it
     * refused are refused for what they are — their hub flag clears too (they still go to the desktop). When it took
     * none, nothing is decided: all stay queued for the next run.
     */
    fun afterSplit(singles: List<Pair<String, Outcome>>): Outcome {
        val (took, not) = singles.partition { it.second.pushed.isNotEmpty() }
        if (took.isEmpty()) return Outcome(refused = singles.map { it.first }, unsupported = singles.any { it.second.unsupported })
        return Outcome(pushed = took.map { it.first }, renames = took.flatMap { it.second.renames }, dropped = not.map { it.first })
    }

    /** The plate events of a `db/changes` page; a row that does not decode is skipped, never the page. */
    fun decode(rows: List<JsonObject>): List<PlateEventDto> =
        rows.mapNotNull { r -> runCatching { ApiFactory.json.decodeFromJsonElement(PlateEventDto.serializer(), r) }.getOrNull()?.takeIf { it.uid.isNotEmpty() } }

    /** A feed cursor ("12345", "12345.0") as a number; null when it is not one. */
    fun cursorLong(text: String): Long? = HubSync.cursorText(text).toLongOrNull()?.takeIf { it >= 0 }

    /** Whether a `db/changes` page from [since] is plate events' business too: the plate cursor is not behind it. */
    fun inStep(plateCursor: Long?, since: Long?): Boolean = since != null && (plateCursor ?: 0L) >= since

    /** The plate cursor after an in-step page that moved the feed to [next]. */
    fun follow(plateCursor: Long?, next: Long?): Long? = if (next == null) plateCursor else maxOf(plateCursor ?: 0L, next)

    /** Whether the plate cursor lags the feed's [main] cursor (a catch-up is due). */
    fun behind(plateCursor: Long?, main: Long?): Boolean = main != null && (plateCursor ?: 0L) < main

    /** The cursor of a plate-feed page ([PlateEventsPage.cursor] a number or a numeric string), else its newest seq, else [since]. */
    fun pageCursor(page: PlateEventsPage, since: Long): Long =
        (page.cursor as? JsonPrimitive)?.let { p -> p.doubleOrNull ?: p.contentOrNull?.toDoubleOrNull() }?.toLong()
            ?: page.events.maxOfOrNull { it.seq ?: 0L } ?: since

    /**
     * One catch-up page from [since] that reached [next] (`more` = the feed has more): the new plate cursor and whether
     * to ask for another page. Once the plate feed ran dry, every plate event up to the feed's [main] cursor is in, so
     * the cursor joins it. A page that does not move stops the run.
     */
    fun catchUp(since: Long, next: Long, more: Boolean, main: Long): Pair<Long, Boolean> = when {
        !more -> maxOf(since, next, main) to false
        next > since -> next to true
        else -> since to false
    }
}
