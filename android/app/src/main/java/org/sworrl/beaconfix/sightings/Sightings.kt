// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings

import org.sworrl.beaconfix.data.db.PlateEventEntity
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.math.roundToInt

/** The wording and the alert policy of plate events (docs/SIGHTINGS.md §6). Pure; unit-tested in SightingsTest. */
object Sightings {
    const val RECENT_MS = 24 * 3_600_000L
    /** At most this many single alerts per batch of new events; the rest go into the summary. */
    const val MAX_SINGLE_ALERTS = 5
    private val LIVE = setOf(PlateEvents.SRC_PHONE_LIVE, PlateEvents.SRC_DASHCAM, PlateEvents.SRC_LIVE)

    fun deepLink(uid: String): String = "beaconfix://sighting/" + java.net.URLEncoder.encode(uid, "UTF-8")

    /** ALPR passes and plate searches alert; no other camera type does (§2.0). */
    fun alertable(e: PlateEventEntity): Boolean = when (e.kind) {
        PlateEvents.CAMERA_PASS -> e.cameraType == "alpr"
        PlateEvents.PLATE_SEARCH -> true
        else -> false
    }

    fun facingText(facing: Int?): String = when (facing) { 1 -> "camera faced you"; 0 -> "camera faced away"; else -> "facing unknown" }

    private val DATE: DateTimeFormatter = DateTimeFormatter.ofPattern("MMM d, yyyy", Locale.US)
    fun date(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String = DATE.format(Instant.ofEpochMilli(ms).atZone(zone))

    /** Title and body of [e]'s alert, null when it does not alert. */
    fun alertText(e: PlateEventEntity, zone: ZoneId = ZoneId.systemDefault()): Pair<String, String>? {
        if (!alertable(e)) return null
        return when (e.kind) {
            PlateEvents.CAMERA_PASS -> {
                val who = listOfNotNull(e.operator?.takeIf { it.isNotBlank() }, e.model?.takeIf { it.isNotBlank() }).joinToString(" ").ifEmpty { "unknown operator" }
                val d = e.distanceM?.let { " · ${it.roundToInt()} m" }.orEmpty()
                "Passed an ALPR camera · $who$d" to "Your plate was likely read (${facingText(e.facing)}). Confidence ${e.confidence ?: 0} %."
            }
            else -> {
                val m = PlateEvents.metricsObject(e.metrics)
                fun f(k: String) = m?.get(k)?.let { PlateEvents.show(it) }?.takeIf { it.isNotBlank() && it != "—" }
                val reason = f("reason") ?: "no reason given"
                val case = f("case_number")?.let { " (case $it)" }.orEmpty()
                "Your plate was searched in Flock · ${e.agency ?: "an agency"}" to "${date(e.timeMs, zone)}: $reason$case. From a released Flock audit log."
            }
        }
    }

    fun summaryTitle(passes: Int, searches: Int, sinceMs: Long, zone: ZoneId = ZoneId.systemDefault()): String {
        val parts = listOfNotNull(
            if (passes > 0) "$passes ALPR camera pass${if (passes == 1) "" else "es"}" else null,
            if (searches > 0) "$searches plate search${if (searches == 1) "" else "es"}" else null,
        )
        return "Backfill: ${parts.joinToString(", ")} since ${date(sinceMs, zone)}"
    }

    data class Plan(val single: List<PlateEventEntity>, val summaryPasses: Int, val summarySearches: Int, val summarySince: Long)

    /**
     * What a batch of events new to this phone becomes (§6): an alert each for ALPR passes newer than 24 h seen live
     * and for new plate searches (at most [MAX_SINGLE_ALERTS]); every other alertable event — backfill passes, older
     * passes, the overflow — counts toward one summary. Non-ALPR cameras and already-notified rows are left out.
     */
    fun plan(fresh: List<PlateEventEntity>, now: Long): Plan {
        val todo = fresh.filter { !it.notified && alertable(it) }.sortedByDescending { it.timeMs }
        val single = ArrayList<PlateEventEntity>(); val rest = ArrayList<PlateEventEntity>()
        for (e in todo) {
            val wants = when (e.kind) {
                PlateEvents.PLATE_SEARCH -> true
                else -> now - e.timeMs <= RECENT_MS && e.source in LIVE
            }
            if (wants && single.size < MAX_SINGLE_ALERTS) single += e else rest += e
        }
        return Plan(single, rest.count { it.kind == PlateEvents.CAMERA_PASS }, rest.count { it.kind == PlateEvents.PLATE_SEARCH }, rest.minOfOrNull { it.timeMs } ?: 0L)
    }
}
