package org.sworrl.beaconfix.help

import kotlinx.serialization.Serializable

/** The kinds a [HelpPlace] can be, in the order the Help screen shows them. */
object HelpKind {
    const val PEDS_ER = "peds_er"
    const val PEDS_CLOSER = "peds_closer"
    const val PEDS_URGENT = "peds_urgent"
    const val ER = "er"
    const val URGENT = "urgent"
    const val POLICE = "police"
    const val FIRE = "fire"
    const val PHARMACY = "pharmacy"
    const val VET = "vet"
    val ALL = listOf(PEDS_ER, PEDS_CLOSER, PEDS_URGENT, ER, URGENT, POLICE, FIRE, PHARMACY, VET)
}

/**
 * One place on the Help screen / widget / tile.
 * [tier]: 1 pediatric ER, 2 children's hospital (ER not confirmed), 3 ER with pediatrics, 4 pediatric urgent care.
 * [er]: "yes" | "no" | "". [campus]: the ER on the same campus (tier 2). [notEr]: urgent care, never an ER.
 * [distM]/[brgDeg]/[driveS] are measured from the snapshot's origin; -1 = unknown. [openNow]: null = unknown.
 */
@Serializable
data class HelpPlace(
    val kind: String,
    val name: String,
    val lat: Double,
    val lon: Double,
    val phone: String = "",
    val address: String = "",
    val hours: String = "",
    val website: String = "",
    val osmKey: String = "",
    val tier: Int = 0,
    val er: String = "",
    val campus: String = "",
    val notEr: Boolean = false,
    val distM: Double = -1.0,
    val brgDeg: Double = 0.0,
    val driveS: Int = -1,
    val driveEst: Boolean = true,
    val openNow: Boolean? = null,
)

/** A street address for the dispatcher; [fromCache] = resolved earlier, not for this exact spot. */
@Serializable
data class AddressLine(
    val line: String = "",
    val locality: String = "",
    val county: String = "",
    val state: String = "",
    val fromCache: Boolean = false,
)

/**
 * Everything the Help surfaces show, computed by HelpRepository.
 * [origin]: "phone" | "rv" | "none" — where distances are measured from ([originLat]/[originLon], [originAgeMs] old).
 * [source]: who found the places ("desktop:<id>" | "phone" | ""). [pediatricSupported]: the desktop has the 3.8
 * "pediatric" feature (otherwise the tiers come from the phone-side fallback). [stale]: old or far from the fetch origin.
 */
@Serializable
data class HelpSnapshot(
    val number: String = "",
    val countryCode: String = "",
    val places: List<HelpPlace> = emptyList(),
    val pedsNote: String = "",
    val pedsSearchKm: Int = 0,
    val pediatricSupported: Boolean = false,
    val origin: String = "none",
    val originLat: Double = 0.0,
    val originLon: Double = 0.0,
    val originAgeMs: Long = 0,
    val source: String = "",
    val fetchedAt: Long = 0,
    val stale: Boolean = false,
    val lastError: String = "",
    val poisonControl: String? = null,
    val address: AddressLine? = null,
) {
    /** The first place of [kind] (see [HelpKind]), or null. */
    fun first(kind: String): HelpPlace? = places.firstOrNull { it.kind == kind }
}
