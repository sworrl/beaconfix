package org.sworrl.beaconfix.alpr.core

/**
 * Legal geofence for private ALPR. Maine, New Hampshire and Arkansas ban it, so capture never runs there; the server's
 * `blocked_regions` list adds to (never removes from) these three.
 *
 * The state comes from the system geocoder when it has a fresh answer; otherwise a coarse outline of each banned state
 * that has one here (a few dozen points each, generous on the coast) decides. Near a border the outline can be off by
 * a few km; the geocoder answer, when present, wins.
 */
object Geofence {
    val ALWAYS_BLOCKED = setOf("ME", "NH", "AR")

    data class Verdict(val blocked: Boolean, val region: String?, val how: String)

    fun check(blocked: Set<String>, lat: Double, lon: Double, geocodedState: String?): Verdict {
        val all = blocked.map { it.uppercase() }.toSet() + ALWAYS_BLOCKED
        if (!geocodedState.isNullOrBlank()) {
            val s = geocodedState.uppercase()
            return Verdict(s in all, s, "geocoder")
        }
        for ((code, poly) in OUTLINES) if (code in all && inside(poly, lat, lon)) return Verdict(true, code, "outline")
        return Verdict(false, null, "outline")
    }

    /** Ray casting over (lat, lon) pairs. */
    fun inside(poly: List<Pair<Double, Double>>, lat: Double, lon: Double): Boolean {
        var c = false; var j = poly.size - 1
        for (i in poly.indices) {
            val (yi, xi) = poly[i]; val (yj, xj) = poly[j]
            if ((yi > lat) != (yj > lat) && lon < (xj - xi) * (lat - yi) / (yj - yi) + xi) c = !c
            j = i
        }
        return c
    }

    private val NH_ME_BORDER = listOf(43.07 to -70.70, 43.25 to -70.82, 43.50 to -70.96, 43.80 to -70.99, 44.30 to -71.02, 44.80 to -71.05, 45.305 to -71.084)

    val OUTLINES: Map<String, List<Pair<Double, Double>>> = mapOf(
        "AR" to listOf(
            36.4996 to -94.6179, 36.4996 to -90.1528, 36.0 to -90.37, 35.995 to -89.733, 35.6 to -89.95, 35.15 to -90.10,
            34.53 to -90.60, 34.0 to -91.02, 33.6 to -91.20, 33.0045 to -91.166, 33.0195 to -94.043, 33.55 to -94.043,
            33.637 to -94.485, 35.38 to -94.43,
        ),
        "NH" to NH_ME_BORDER + listOf(
            // Quebec, then down the Connecticut River (the Vermont line), the Massachusetts line, the coast
            45.01 to -71.50, 44.89 to -71.50, 44.75 to -71.63, 44.57 to -71.57, 44.49 to -71.58, 44.40 to -71.70,
            44.15 to -72.04, 43.90 to -72.18, 43.70 to -72.305, 43.48 to -72.39, 43.13 to -72.445, 42.86 to -72.55,
            42.727 to -72.458, 42.70 to -71.30, 42.81 to -71.06, 42.86 to -70.93, 42.87 to -70.70,
        ),
        // north-west corner, Quebec, New Brunswick, the coast (offshore, so the islands are in), then the NH line north
        "ME" to listOf(
            45.305 to -71.084, 45.60 to -70.60, 46.10 to -70.28, 46.70 to -70.00, 47.45 to -69.22, 47.36 to -68.33,
            47.07 to -67.79, 45.94 to -67.78, 45.60 to -67.43, 45.17 to -67.28, 44.81 to -66.95, 44.60 to -67.10,
            44.10 to -68.10, 43.70 to -69.10, 43.45 to -70.10, 43.00 to -70.55,
        ) + NH_ME_BORDER.dropLast(1),
    )

    /** Full US state / territory name (as Android's geocoder gives `adminArea`) → USPS code. */
    val STATE_CODES: Map<String, String> = mapOf(
        "Alabama" to "AL", "Alaska" to "AK", "Arizona" to "AZ", "Arkansas" to "AR", "California" to "CA", "Colorado" to "CO",
        "Connecticut" to "CT", "Delaware" to "DE", "District of Columbia" to "DC", "Florida" to "FL", "Georgia" to "GA",
        "Hawaii" to "HI", "Idaho" to "ID", "Illinois" to "IL", "Indiana" to "IN", "Iowa" to "IA", "Kansas" to "KS",
        "Kentucky" to "KY", "Louisiana" to "LA", "Maine" to "ME", "Maryland" to "MD", "Massachusetts" to "MA",
        "Michigan" to "MI", "Minnesota" to "MN", "Mississippi" to "MS", "Missouri" to "MO", "Montana" to "MT",
        "Nebraska" to "NE", "Nevada" to "NV", "New Hampshire" to "NH", "New Jersey" to "NJ", "New Mexico" to "NM",
        "New York" to "NY", "North Carolina" to "NC", "North Dakota" to "ND", "Ohio" to "OH", "Oklahoma" to "OK",
        "Oregon" to "OR", "Pennsylvania" to "PA", "Rhode Island" to "RI", "South Carolina" to "SC", "South Dakota" to "SD",
        "Tennessee" to "TN", "Texas" to "TX", "Utah" to "UT", "Vermont" to "VT", "Virginia" to "VA", "Washington" to "WA",
        "West Virginia" to "WV", "Wisconsin" to "WI", "Wyoming" to "WY", "Puerto Rico" to "PR",
    )

    /** A geocoder `adminArea` ("Texas", or already "TX") → "TX"; null when it isn't a US state. */
    fun stateCode(adminArea: String?, countryCode: String?): String? {
        if (adminArea.isNullOrBlank()) return null
        if (!countryCode.isNullOrBlank() && !countryCode.equals("US", true) && !countryCode.equals("PR", true)) return null
        val a = adminArea.trim()
        if (a.length == 2 && a.uppercase() in STATE_CODES.values) return a.uppercase()
        return STATE_CODES.entries.firstOrNull { it.key.equals(a, ignoreCase = true) }?.value
    }
}
