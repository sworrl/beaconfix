package org.sworrl.beaconfix.share

import java.net.URLEncoder
import java.util.Locale
import kotlin.math.roundToInt

/**
 * Plain-text messages for the share sheet (pure; no Android types). Every message carries coordinates plus links
 * that open in any map app: `geo:`, OpenStreetMap, Google Maps and Apple Maps.
 */
object ShareText {
    /** "My location" (or [label]) with ±accuracy, the address when known, and the map links. */
    fun location(lat: Double, lon: Double, accM: Double?, address: String?, label: String? = null): String = buildString {
        append(label?.takeIf { it.isNotBlank() } ?: "My location")
        append(": ").append(coords(lat, lon))
        if (accM != null && accM > 0) append(" (±").append(accM.roundToInt()).append(" m)")
        append('\n')
        address?.takeIf { it.isNotBlank() }?.let { append(it.trim()).append('\n') }
        links(lat, lon, label?.takeIf { it.isNotBlank() } ?: "My location").forEach { append(it).append('\n') }
    }.trimEnd()

    /** A place: name, address, phone, coordinates and the map links. */
    fun place(name: String, lat: Double, lon: Double, address: String, phone: String): String = buildString {
        append(name.ifBlank { "Place" }).append('\n')
        if (address.isNotBlank()) append(address.trim()).append('\n')
        if (phone.isNotBlank()) append("Phone: ").append(phone.trim()).append('\n')
        append(coords(lat, lon)).append('\n')
        links(lat, lon, name).forEach { append(it).append('\n') }
    }.trimEnd()

    /** `geo:`, OpenStreetMap (`mlat`/`mlon`), Google (`query=`) and Apple (`ll=`) links, 6 decimals. */
    fun links(lat: Double, lon: Double, label: String): List<String> {
        val la = f6(lat); val lo = f6(lon)
        val q = enc(label.replace('(', ' ').replace(')', ' ').trim())
        return listOf(
            if (q.isEmpty()) "geo:$la,$lo?q=$la,$lo" else "geo:$la,$lo?q=$la,$lo($q)",
            "https://www.openstreetmap.org/?mlat=$la&mlon=$lo#map=17/$la/$lo",
            "https://www.google.com/maps/search/?api=1&query=$la,$lo",
            if (q.isEmpty()) "https://maps.apple.com/?ll=$la,$lo" else "https://maps.apple.com/?ll=$la,$lo&q=$q",
        )
    }

    /** "39.63520, -79.95590" — 5 decimals (about a metre), what a dispatcher reads out. */
    fun coords(lat: Double, lon: Double): String = String.format(Locale.US, "%.5f, %.5f", lat, lon)

    private fun f6(v: Double) = String.format(Locale.US, "%.6f", v)
    private fun enc(s: String) = URLEncoder.encode(s, "UTF-8").replace("+", "%20")
}
