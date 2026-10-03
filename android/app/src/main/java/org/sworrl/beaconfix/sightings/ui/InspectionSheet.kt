// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.SheetState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.sworrl.beaconfix.sightings.InspectionPlan
import org.sworrl.beaconfix.sightings.VantagePoint
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.compass
import org.sworrl.beaconfix.ui.theme.Gold
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Red
import org.sworrl.beaconfix.ui.theme.Slate

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun InspectionSheet(
    plan: InspectionPlan?,
    loading: Boolean,
    onDismiss: () -> Unit,
    onStartPrivateInspection: (InspectionPlan) -> Unit,
    onOpenGpx: (InspectionPlan) -> Unit,
    onProfileChange: (String) -> Unit,
    sheetState: SheetState,
) {
    val ctx = LocalContext.current
    var selectedProfile by remember { mutableStateOf("foot") }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
    ) {
        Column(
            Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 8.dp)
        ) {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("Inspect Camera Unseen", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                TextButton(onClick = onDismiss) { Text("Close") }
            }

            if (loading) {
                Box(Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator(color = Gold)
                }
                return@ModalBottomSheet
            }

            if (plan == null) {
                Text("No inspection plan available.", color = Slate, modifier = Modifier.padding(16.dp))
                return@ModalBottomSheet
            }

            // 1. Camera Facts
            Card(
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
                modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp)
            ) {
                Column(Modifier.padding(12.dp)) {
                    Text(
                        "${plan.model.ifEmpty { "ALPR Camera" }} · ${plan.operatorName.ifEmpty { "Unknown Operator" }}",
                        fontWeight = FontWeight.Bold,
                        style = MaterialTheme.typography.titleMedium
                    )
                    Text(
                        "Camera ID: ${plan.cameraId} · Position: %.6f, %.6f".format(plan.cameraLat, plan.cameraLon),
                        color = Slate,
                        style = MaterialTheme.typography.bodySmall
                    )
                    Text(
                        "Facing: ${plan.direction.ifEmpty { "Omni / Unspecified (sees all round)" }}",
                        color = MaterialTheme.colorScheme.primary,
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
            }

            // 2. Honest Limits Notice
            Surface(
                color = Color(0xFF2B2614),
                shape = RoundedCornerShape(8.dp),
                modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp)
            ) {
                Text(
                    plan.limits.ifEmpty { "Limits: Only mapped cameras with their mapped directions are avoided. Unmapped cameras, PTZ / 360° domes, private CCTV, police-car ALPRs and cell tracking are not." },
                    color = Gold,
                    fontSize = 12.sp,
                    lineHeight = 16.sp,
                    modifier = Modifier.padding(10.dp)
                )
            }

            // 3. Profile Toggle
            Row(
                Modifier.fillMaxWidth().padding(vertical = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("Profile:", style = MaterialTheme.typography.bodyMedium)
                FilterChip(
                    selected = selectedProfile == "foot",
                    onClick = { selectedProfile = "foot"; onProfileChange("foot") },
                    label = { Text("Foot (walk)") }
                )
                FilterChip(
                    selected = selectedProfile == "car",
                    onClick = { selectedProfile = "car"; onProfileChange("car") },
                    label = { Text("Car (drive)") }
                )
            }

            // 4. Status Banner
            val isSafe = plan.safe && plan.exposures.isEmpty()
            val bannerBg = if (isSafe) Color(0xFF144620) else Color(0xFF5A1414)
            val bannerTextCol = if (isSafe) Color(0xFFB3FFCC) else Color(0xFFFFCCCC)
            val statusText = if (plan.exposures.isNotEmpty()) {
                "EXPOSURE DETECTED: Route enters field of view of ${plan.exposures.size} camera(s)!"
            } else if (isSafe) {
                "CLEAN / SAFE: Vantage is outside camera view, and route avoids all known ALPRs."
            } else {
                plan.note.ifEmpty { "No safe path could be verified." }
            }

            Surface(
                color = bannerBg,
                shape = RoundedCornerShape(8.dp),
                modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp)
            ) {
                Column(Modifier.padding(10.dp)) {
                    Text(statusText, color = bannerTextCol, fontWeight = FontWeight.Bold, fontSize = 13.sp)
                    if (plan.note.isNotBlank() && isSafe) {
                        Text(plan.note, color = bannerTextCol.copy(alpha = 0.85f), fontSize = 11.sp)
                    }
                }
            }

            // 5. Safe Vantage Points
            Text("Safe Vantage Points", fontWeight = FontWeight.Bold, style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(top = 8.dp))
            if (plan.vantages.isEmpty()) {
                Text("No vantage points found in the specified distance band outside avoid regions.", color = Slate, style = MaterialTheme.typography.bodySmall)
            } else {
                for ((idx, vp) in plan.vantages.withIndex()) {
                    Card(
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                        modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp)
                    ) {
                        Row(Modifier.padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
                            Surface(
                                color = if (idx == 0) Gold else Slate,
                                shape = RoundedCornerShape(12.dp),
                                modifier = Modifier.size(24.dp)
                            ) {
                                Box(contentAlignment = Alignment.Center) {
                                    Text("${idx + 1}", color = Color.Black, fontWeight = FontWeight.Bold, fontSize = 12.sp)
                                }
                            }
                            Spacer(Modifier.width(10.dp))
                            Column(Modifier.weight(1f)) {
                                Row(horizontalArrangement = Arrangement.SpaceBetween, modifier = Modifier.fillMaxWidth()) {
                                    Text(
                                        "${vp.side.replaceFirstChar { it.uppercase() }} · ${vp.distanceM.toInt()} m",
                                        fontWeight = FontWeight.Bold,
                                        style = MaterialTheme.typography.bodyMedium
                                    )
                                    Text(
                                        "Look: ${vp.bearingToCamera.toInt()}° ${compass(vp.bearingToCamera)}",
                                        color = Gold,
                                        fontWeight = FontWeight.Bold,
                                        style = MaterialTheme.typography.bodySmall
                                    )
                                }
                                Text(vp.reason, color = Slate, style = MaterialTheme.typography.bodySmall)
                            }
                        }
                    }
                }
            }

            // 6. Navigation Legs Summary
            if (plan.toVantageLeg.ok) {
                Spacer(Modifier.height(8.dp))
                Text(
                    "Approach: ${plan.toVantageLeg.distanceM.toInt()} m (~${(plan.toVantageLeg.durationS / 60).toInt()} min) · Departure: ${plan.awayLeg.distanceM.toInt()} m · Provider: ${plan.providerName}",
                    color = Slate,
                    style = MaterialTheme.typography.bodySmall
                )
            }

            Spacer(Modifier.height(16.dp))

            // 7. Action Buttons
            Button(
                onClick = { onStartPrivateInspection(plan); onDismiss() },
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF009E73)),
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Start Private Inspection (Silent Mode & Live Guard)", color = Color.White, fontWeight = FontWeight.Bold)
            }

            Spacer(Modifier.height(6.dp))

            OutlinedButton(
                onClick = { onOpenGpx(plan) },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Open in OsmAnd / Organic Maps (GPX)")
            }

            Spacer(Modifier.height(24.dp))
        }
    }
}

/**
 * Floating HUD banner on map while Private Inspection Mode is active.
 */
@Composable
fun InspectionActiveHud(
    plan: InspectionPlan,
    guardAlert: String?,
    onDone: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        color = if (guardAlert != null) Color(0xEE8B0000) else Color(0xEE144620),
        shape = RoundedCornerShape(12.dp),
        shadowElevation = 6.dp,
        modifier = modifier.fillMaxWidth().padding(horizontal = 12.dp)
    ) {
        Row(
            Modifier.padding(10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    if (guardAlert != null) "⚠️ $guardAlert" else "INSPECT UNSEEN · PRIVATE MODE ACTIVE",
                    color = if (guardAlert != null) Color.Yellow else Color(0xFFB3FFCC),
                    fontWeight = FontWeight.Bold,
                    fontSize = 12.sp
                )
                Text(
                    "Target: ${plan.model.ifEmpty { plan.cameraId }} · Fixes/Passes Suppressed · Guard: ON",
                    color = Color.White.copy(alpha = 0.9f),
                    fontSize = 11.sp
                )
            }
            Spacer(Modifier.width(8.dp))
            Button(
                onClick = onDone,
                colors = ButtonDefaults.buttonColors(containerColor = Color.White),
                contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 12.dp, vertical = 4.dp)
            ) {
                Text("Done", color = Color.Black, fontWeight = FontWeight.Bold, fontSize = 12.sp)
            }
        }
    }
}
