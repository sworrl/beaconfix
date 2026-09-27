package org.sworrl.beaconfix.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import org.sworrl.beaconfix.ui.theme.Cyan
import org.sworrl.beaconfix.ui.theme.Gold
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Magenta
import org.sworrl.beaconfix.ui.theme.Orange
import org.sworrl.beaconfix.ui.theme.Red
import org.sworrl.beaconfix.ui.theme.Slate
import java.text.DateFormat
import java.util.Date

@Composable
fun InfoCard(title: String, modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Card(modifier = modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp)) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            content()
        }
    }
}

@Composable
fun KeyValue(k: String, v: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(k, color = Slate, style = MaterialTheme.typography.bodyMedium)
        Text(v, style = MaterialTheme.typography.bodyMedium)
    }
}

fun ago(ms: Long): String {
    if (ms <= 0) return "never"
    val s = (System.currentTimeMillis() - ms) / 1000
    return when { s < 5 -> "now"; s < 60 -> "$s s ago"; s < 3600 -> "${s / 60} min ago"; s < 86400 -> "${s / 3600} h ago"; else -> DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(Date(ms)) }
}
fun metres(m: Double) = Units.dist(m)

/** Same grading as the desktop widget's security.js, condensed. */
data class Grade(val label: String, val color: Color, val why: String)
fun grade(security: String): Grade = when (security) {
    "open" -> Grade("OPEN", Red, "No encryption at all: every frame is plaintext to anyone in range; trivial evil twin.")
    "wep" -> Grade("WEP", Red, "RC4 with 24-bit IVs: the key falls to PTW in minutes; CRC-32 lets frames be forged.")
    "wpa1" -> Grade("WPA1", Red, "TKIP only: Beck–Tews MIC recovery, deprecated since 802.11-2012.")
    "wpa2-tkip" -> Grade("WPA2+TKIP", Red, "TKIP negotiated for group traffic: RC4 under a shared key again.")
    "wpa2" -> Grade("WPA2-PSK", Orange, "CCMP is sound, but one captured handshake (or a PMKID) allows an offline dictionary attack on the passphrase; no forward secrecy; PMF usually off.")
    "wpa2-eap" -> Grade("WPA2-Ent", Gold, "Per-user keys via 802.1X; only as safe as the client's certificate validation.")
    "wpa2/3" -> Grade("WPA2/3", Gold, "Transition mode: clients can be downgraded to WPA2-PSK (Dragonblood).")
    "wpa3" -> Grade("WPA3", Green, "SAE: no offline dictionary attack, forward secrecy, PMF mandatory.")
    "wpa3-eap192" -> Grade("WPA3-192", Green, "Suite-B 192-bit enterprise.")
    "owe" -> Grade("OWE", Gold, "Encrypted but unauthenticated: a sniffer is blind, an active attacker is not.")
    else -> Grade("?", Slate, "Security not reported.")
}
fun statusColor(home: Boolean, travelling: Boolean, positioned: Boolean): Color = when { home -> Magenta; travelling -> Magenta; positioned -> Gold; else -> Cyan }

// ── shared building blocks for the richer screens ────────────────────────────
@Composable
fun EmptyState(icon: String, title: String, body: String, action: String? = null, onAction: (() -> Unit)? = null) {
    Column(Modifier.fillMaxWidth().padding(32.dp), horizontalAlignment = androidx.compose.ui.Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(icon, style = MaterialTheme.typography.displayMedium)
        Text(title, style = MaterialTheme.typography.titleMedium, textAlign = androidx.compose.ui.text.style.TextAlign.Center)
        Text(body, color = Slate, style = MaterialTheme.typography.bodyMedium, textAlign = androidx.compose.ui.text.style.TextAlign.Center)
        if (action != null && onAction != null) androidx.compose.material3.Button(onClick = onAction) { Text(action) }
    }
}

@Composable
fun Chip(text: String, color: Color, modifier: Modifier = Modifier) {
    androidx.compose.material3.Surface(modifier, shape = MaterialTheme.shapes.small, color = color.copy(alpha = 0.18f)) {
        Text(text, Modifier.padding(horizontal = 8.dp, vertical = 2.dp), color = color, style = MaterialTheme.typography.labelSmall, fontWeight = androidx.compose.ui.text.font.FontWeight.Bold)
    }
}

fun gradeColor(grade: String): Color = when (grade) { "critical" -> Red; "weak" -> Orange; "strong" -> Green; "ok" -> Gold; else -> Slate }
fun gradeGlyph(grade: String): String = when (grade) { "critical" -> "☠"; "weak" -> "⚠"; "strong" -> "🛡"; "ok" -> "◐"; else -> "?" }
fun secName(sec: String) = mapOf("open" to "Open", "owe" to "OWE", "wep" to "WEP", "wpa1" to "WPA1/TKIP", "wpa2-tkip" to "WPA2+TKIP", "wpa2" to "WPA2-PSK", "wpa2-eap" to "WPA2-Ent", "wpa2/3" to "WPA2/3", "wpa3" to "WPA3-SAE", "wpa3-eap192" to "WPA3-192")[sec] ?: (sec.ifEmpty { "?" })

fun hhmm(iso: String): String = iso.takeIf { it.length >= 16 }?.let { it.substring(11, 16) } ?: iso
fun durText(secs: Long): String = when { secs < 60 -> "$secs s"; secs < 3600 -> "${secs / 60} min"; secs < 86400 -> "${secs / 3600} h ${secs % 3600 / 60} min"; else -> "${secs / 86400} d ${secs % 86400 / 3600} h" }
fun km(v: Double) = Units.km(v)
fun compass(deg: Double): String = listOf("N","NNE","NE","ENE","E","ESE","SE","SSE","S","SSW","SW","WSW","W","WNW","NW","NNW")[((((deg % 360) + 360) % 360) / 22.5).toInt() % 16]

/** Sunrise / sunset for a day at a place, NOAA's method; returns local HH:MM or null in polar cases. */
object SunCalc {
    data class Times(val sunrise: String, val sunset: String, val dayLengthSecs: Long)
    fun today(lat: Double, lon: Double): Times? {
        val zone = java.time.ZoneId.systemDefault()
        val date = java.time.LocalDate.now(zone)
        val n = date.dayOfYear.toDouble()
        fun event(rising: Boolean): Double? {
            val lngHour = lon / 15.0
            val t = n + ((if (rising) 6.0 else 18.0) - lngHour) / 24.0
            val m = 0.9856 * t - 3.289
            var l = m + 1.916 * Math.sin(Math.toRadians(m)) + 0.020 * Math.sin(Math.toRadians(2 * m)) + 282.634
            l = ((l % 360) + 360) % 360
            var ra = Math.toDegrees(Math.atan(0.91764 * Math.tan(Math.toRadians(l)))); ra = ((ra % 360) + 360) % 360
            ra += (Math.floor(l / 90) * 90 - Math.floor(ra / 90) * 90); ra /= 15
            val sinDec = 0.39782 * Math.sin(Math.toRadians(l)); val cosDec = Math.cos(Math.asin(sinDec))
            val cosH = (Math.cos(Math.toRadians(90.833)) - sinDec * Math.sin(Math.toRadians(lat))) / (cosDec * Math.cos(Math.toRadians(lat)))
            if (cosH > 1 || cosH < -1) return null
            var h = if (rising) 360 - Math.toDegrees(Math.acos(cosH)) else Math.toDegrees(Math.acos(cosH)); h /= 15
            val tt = h + ra - 0.06571 * t - 6.622
            return (((tt - lngHour) % 24) + 24) % 24
        }
        val r = event(true) ?: return null; val s = event(false) ?: return null
        fun local(utcHours: Double): String {
            val secs = (utcHours * 3600).toLong()
            val inst = date.atStartOfDay(java.time.ZoneOffset.UTC).plusSeconds(secs).toInstant()
            return java.time.format.DateTimeFormatter.ofPattern("HH:mm").format(inst.atZone(zone))
        }
        val len = (((s - r) % 24 + 24) % 24 * 3600).toLong()
        return Times(local(r), local(s), len)
    }
}

/** Emergency numbers by ISO country, for the fix card. */
object Emergency {
    private val table = mapOf("US" to "911", "CA" to "911", "MX" to "911", "GB" to "999", "IE" to "112 / 999", "AU" to "000", "NZ" to "111", "JP" to "110 / 119", "IN" to "112", "BR" to "190 / 192", "ZA" to "10111", "AR" to "911", "CL" to "131 / 133")
    fun number(countryCode: String?): String = countryCode?.uppercase()?.let { table[it] } ?: (if (countryCode.isNullOrEmpty()) "112 / 911" else "112")
}
