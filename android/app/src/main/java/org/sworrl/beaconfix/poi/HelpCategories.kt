package org.sworrl.beaconfix.poi

/**
 * The help categories the phone searches itself, with the desktop's labels, icons and colours
 * (`Locator::poiCategories()` in `src/locator.cpp`, 3.8), so a place found by the phone looks the same as one sent by
 * the desktop. Only these categories are ever stored from the phone's own search.
 */
object HelpCategories {
    data class Cat(val key: String, val label: String, val icon: String, val color: String, val wide: Boolean, val reachKm: Int = 0, val group: String = GROUP)

    const val GROUP = "civic"
    const val GROUP_LABEL = "Emergency & civic"

    val ALL: List<Cat> = listOf(
        Cat("police", "Police", "🚔", "#4d8bff", wide = true),
        Cat("fire", "Fire station", "🚒", "#ff4d4d", wide = true),
        Cat("health", "Hospital / ER", "🏥", "#ff4f4f", wide = true),
        Cat("urgent", "Urgent care / clinic", "🩺", "#ff8a8a", wide = true),
        Cat("peds_er", "Pediatric ER", "🧸", "#ff5fa2", wide = true, reachKm = 150),
        Cat("peds_urgent", "Pediatric urgent care", "🩹", "#ffa3cf", wide = true, reachKm = 50),
        Cat("pharmacy", "Pharmacy", "💊", "#ff7aa8", wide = false),
        Cat("vet", "Veterinary", "🐾", "#d0a06a", wide = true),
    )
    private val byKey = ALL.associateBy { it.key }
    val KEYS: Set<String> = byKey.keys

    fun of(key: String): Cat? = byKey[key]

    /** The categories that get a drive-time estimate (same set as the desktop's). */
    fun driveCat(cat: String) = cat == "peds_er" || cat == "peds_urgent" || cat == "health" || cat == "urgent" || cat == "police" || cat == "fire"
}
