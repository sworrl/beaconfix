package org.sworrl.beaconfix.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.RadioButtonDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.sworrl.beaconfix.data.DoomBatteryMode
import org.sworrl.beaconfix.ui.theme.Slate

/**
 * Upfront DOOM battery difficulty profile chip for MapScreen and navigation bars.
 */
@Composable
fun DoomBatteryChip(
    mode: DoomBatteryMode,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val tierColor = runCatching { Color(android.graphics.Color.parseColor(mode.colorHex)) }.getOrDefault(Color(0xFFFFD166))
    Surface(
        modifier = modifier
            .clip(RoundedCornerShape(8.dp))
            .clickable(onClick = onClick),
        shape = RoundedCornerShape(8.dp),
        color = tierColor.copy(alpha = 0.18f),
        border = BorderStroke(1.2.dp, tierColor.copy(alpha = 0.75f))
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 9.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(5.dp)
        ) {
            Text(mode.iconEmoji, fontSize = 13.sp)
            Text(
                text = mode.title,
                color = tierColor,
                fontWeight = FontWeight.Bold,
                fontSize = 12.sp
            )
            Text(
                text = "▼",
                color = tierColor.copy(alpha = 0.8f),
                fontSize = 9.sp,
                modifier = Modifier.padding(start = 2.dp)
            )
        }
    }
}

/**
 * Interactive DOOM-style difficulty modal selector.
 */
@Composable
fun DoomBatteryDialog(
    currentMode: DoomBatteryMode,
    onSelectMode: (DoomBatteryMode) -> Unit,
    onDismiss: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Column {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "DOOM BATTERY DIFFICULTY",
                        fontWeight = FontWeight.Black,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 16.sp,
                        color = Color(0xFFFF3333)
                    )
                }
                Text(
                    "Choose how aggressively BeaconFix scans sensors & consumes battery",
                    style = MaterialTheme.typography.bodySmall,
                    color = Slate,
                    modifier = Modifier.padding(top = 2.dp)
                )
            }
        },
        text = {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                for (mode in DoomBatteryMode.entries) {
                    val isSelected = (mode == currentMode)
                    val tierColor = runCatching { Color(android.graphics.Color.parseColor(mode.colorHex)) }.getOrDefault(Color.White)

                    Card(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(10.dp))
                            .clickable { onSelectMode(mode) },
                        shape = RoundedCornerShape(10.dp),
                        colors = CardDefaults.cardColors(
                            containerColor = if (isSelected) tierColor.copy(alpha = 0.16f) else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)
                        ),
                        border = if (isSelected) BorderStroke(2.dp, tierColor) else BorderStroke(0.8.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.3f))
                    ) {
                        Column(Modifier.padding(10.dp)) {
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Text(mode.iconEmoji, fontSize = 18.sp, modifier = Modifier.padding(end = 8.dp))
                                Column(modifier = Modifier.weight(1f)) {
                                    Row(verticalAlignment = Alignment.CenterVertically) {
                                        Text(
                                            text = mode.title,
                                            fontWeight = FontWeight.Bold,
                                            fontSize = 14.sp,
                                            color = if (isSelected) tierColor else MaterialTheme.colorScheme.onSurface
                                        )
                                        Spacer(Modifier.width(6.dp))
                                        Surface(
                                            shape = RoundedCornerShape(4.dp),
                                            color = tierColor.copy(alpha = 0.25f)
                                        ) {
                                            Text(
                                                text = mode.tag,
                                                fontSize = 10.sp,
                                                fontWeight = FontWeight.SemiBold,
                                                color = tierColor,
                                                modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp)
                                            )
                                        }
                                    }
                                    Text(
                                        text = mode.subtitle,
                                        style = MaterialTheme.typography.bodySmall,
                                        color = Slate,
                                        fontSize = 11.sp
                                    )
                                }
                                RadioButton(
                                    selected = isSelected,
                                    onClick = { onSelectMode(mode) },
                                    colors = RadioButtonDefaults.colors(
                                        selectedColor = tierColor,
                                        unselectedColor = Slate
                                    )
                                )
                            }

                            // Classic DOOM flavor text
                            Text(
                                text = "“${mode.flavorText}”",
                                style = MaterialTheme.typography.bodySmall,
                                fontStyle = androidx.compose.ui.text.font.FontStyle.Italic,
                                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.85f),
                                fontSize = 11.5.sp,
                                modifier = Modifier.padding(top = 4.dp, bottom = 6.dp)
                            )

                            // Technical telemetry metrics
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .background(Color.Black.copy(alpha = 0.2f), RoundedCornerShape(6.dp))
                                    .padding(horizontal = 8.dp, vertical = 4.dp),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                MetricSpec("GPS", if (mode.gpsIntervalMs >= 1000) "${mode.gpsIntervalMs / 1000}s" else "${mode.gpsIntervalMs}ms")
                                MetricSpec("Wi-Fi", "${mode.wifiMovingIntervalMs / 1000}s / ${mode.wifiStationaryIntervalMs / 1000}s")
                                MetricSpec("ALPR", "${mode.alprFps} fps")
                                MetricSpec(
                                    "Wakelock",
                                    if (mode.holdWakeLock) "ACTIVE" else "OFF",
                                    color = if (mode.holdWakeLock) Color(0xFFFF5252) else Color(0xFF69F0AE)
                                )
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text("Close", fontWeight = FontWeight.Bold)
            }
        }
    )
}

@Composable
private fun MetricSpec(label: String, value: String, color: Color = MaterialTheme.colorScheme.onSurface) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(label, fontSize = 9.sp, color = Slate, fontWeight = FontWeight.Medium)
        Text(value, fontSize = 10.sp, fontWeight = FontWeight.Bold, color = color, fontFamily = FontFamily.Monospace)
    }
}

/**
 * A prominent DOOM Battery profile summary card for MoreScreen and SettingsScreen.
 */
@Composable
fun DoomBatteryCard(
    mode: DoomBatteryMode,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val tierColor = runCatching { Color(android.graphics.Color.parseColor(mode.colorHex)) }.getOrDefault(Color(0xFFFFD166))
    Card(
        modifier = modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .clickable(onClick = onClick),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = tierColor.copy(alpha = 0.12f)
        ),
        border = BorderStroke(1.5.dp, tierColor.copy(alpha = 0.7f))
    ) {
        Row(
            modifier = Modifier.padding(14.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .size(44.dp)
                    .clip(CircleShape)
                    .background(tierColor.copy(alpha = 0.2f))
                    .border(1.dp, tierColor, CircleShape),
                contentAlignment = Alignment.Center
            ) {
                Text(mode.iconEmoji, fontSize = 22.sp)
            }
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "DOOM POWER PROFILE",
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Black,
                        fontFamily = FontFamily.Monospace,
                        color = Color(0xFFFF4444)
                    )
                    Spacer(Modifier.width(6.dp))
                    Surface(
                        shape = RoundedCornerShape(4.dp),
                        color = tierColor.copy(alpha = 0.3f)
                    ) {
                        Text(
                            mode.tag,
                            fontSize = 9.5.sp,
                            fontWeight = FontWeight.Bold,
                            color = tierColor,
                            modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp)
                        )
                    }
                }
                Text(
                    text = mode.title,
                    fontWeight = FontWeight.Bold,
                    fontSize = 15.sp,
                    color = tierColor
                )
                Text(
                    text = mode.subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = Slate,
                    fontSize = 11.5.sp
                )
            }
            OutlinedButton(
                onClick = onClick,
                colors = ButtonDefaults.outlinedButtonColors(contentColor = tierColor),
                border = BorderStroke(1.dp, tierColor.copy(alpha = 0.6f)),
                shape = RoundedCornerShape(8.dp)
            ) {
                Text("Change", fontSize = 11.sp, fontWeight = FontWeight.Bold)
            }
        }
    }
}
