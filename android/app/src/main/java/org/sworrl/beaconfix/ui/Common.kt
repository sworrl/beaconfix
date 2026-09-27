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
fun metres(m: Double) = if (m >= 1000) String.format(java.util.Locale.US, "%.1f km", m / 1000) else "${m.toInt()} m"

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
