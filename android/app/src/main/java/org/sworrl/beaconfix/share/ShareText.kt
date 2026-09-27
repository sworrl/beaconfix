package org.sworrl.beaconfix.share

import java.net.URLEncoder
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.math.roundToInt

/**
 * Plain-text messages for the share sheet (pure; no Android types). Every message carries coordinates plus links
 * that open in any map app: `geo:`, OpenStreetMap, Google Maps and Apple Maps.
 */
object ShareText {
    const val MY_LOCATION = "My location"
    /** What the text says when the position is the RV's, not the phone's (Help's origin fell back to the desktop). */
    const val RV_POSITION = "RV position (phone has no recent fix)"
    /** The phone's own fix, but not a fresh one (the share had to fall back to the last fix it took). */
    const val LAST_KNOWN = "My last known location"
    /** A fix older than this gets a "Fix taken HH:MM (N min ago)" line: a dispatcher must know how old it is. */
    const val FRESH_FIX_MS = 2 * 60_000L

    /**
     * "My location" (or [label]) with ±accuracy, the address when known, when the fix was taken if that is more than
     * [FRESH_FIX_MS] before [nowMs] ([fixAtMs] epoch ms; 0 = unknown / fresh), and the map links.
     */
    fun location(lat: Double, lon: Double, accM: Double?, address: String?, label: String? = null,
                 fixAtMs: Long = 0L, nowMs: Long = 0L, zone: ZoneId = ZoneId.systemDefault()): String = buildString {
        append(label?.takeIf { it.isNotBlank() } ?: MY_LOCATION)
        append(": ").append(coords(lat, lon))
        if (accM != null && accM > 0) append(" (±").append(accM.roundToInt()).append(" m)")
        append('\n')
        fixAge(fixAtMs, nowMs, zone)?.let { append(it).append('\n') }
        address?.takeIf { it.isNotBlank() }?.let { append(it.trim()).append('\n') }
        links(lat, lon, label?.takeIf { it.isNotBlank() } ?: MY_LOCATION).forEach { append(it).append('\n') }
    }.trimEnd()

    /** "Fix taken 14:05 (25 min ago)" / "Fix taken 2026-09-26 14:05 (1 d ago)"; null when fresh or unknown. */
    fun fixAge(fixAtMs: Long, nowMs: Long, zone: ZoneId = ZoneId.systemDefault()): String? {
        if (fixAtMs <= 0 || nowMs <= 0) return null
        val age = nowMs - fixAtMs
        if (age < FRESH_FIX_MS) return null
        val at = Instant.ofEpochMilli(fixAtMs).atZone(zone)
        val sameDay = at.toLocalDate() == Instant.ofEpochMilli(nowMs).atZone(zone).toLocalDate()
        val clock = at.format(DateTimeFormatter.ofPattern(if (sameDay) "HH:mm" else "yyyy-MM-dd HH:mm", Locale.US))
        val mins = age / 60_000
        val ago = when { mins < 60 -> "$mins min"; mins < 48 * 60 -> "${mins / 60} h"; else -> "${mins / (24 * 60)} d" }
        return "Fix taken $clock ($ago ago)"
    }

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
