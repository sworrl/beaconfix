package org.sworrl.beaconfix.ui.screens

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Notifications
import androidx.compose.material.icons.filled.NotificationsOff
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Vibration
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.data.api.FlockCameraDto
import org.sworrl.beaconfix.detector.DetectionType
import org.sworrl.beaconfix.detector.DetectorAlertManager
import org.sworrl.beaconfix.detector.HapticPattern
import org.sworrl.beaconfix.detector.SoundTone
import org.sworrl.beaconfix.estimate.Geo
import org.sworrl.beaconfix.ui.vm.LiveViewModel
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DetectorScreen(
    onAlpr: () -> Unit = {},
    live: LiveViewModel = hiltViewModel(),
    alertManager: DetectorAlertManager = run {
        val ctx = androidx.compose.ui.platform.LocalContext.current
        remember {
            dagger.hilt.android.EntryPointAccessors.fromApplication(
                ctx.applicationContext,
                org.sworrl.beaconfix.widget.WidgetEntryPoint::class.java
            ).alertManager()
        }
    }
) {
    val phoneFix by live.phone.collectAsState()
    val flockCameras by live.flockCameras.collectAsState()
    val desktopViews by live.views.collectAsState()
    val activePlate = desktopViews.firstNotNullOfOrNull { it.alprSummary?.activePlate }   // the desktop's registered plate, never hard-coded
    val settings by alertManager.settings.collectAsState()
    var showSettingsSheet by remember { mutableStateOf(false) }

    val myLoc = phoneFix.fix
    var closestCam by remember { mutableStateOf<Pair<FlockCameraDto, Double>?>(null) }

    // Proximity evaluation
    LaunchedEffect(myLoc, flockCameras) {
        val loc = myLoc
        if (loc != null && loc.lat != 0.0 && loc.lon != 0.0) {
            var minD = Double.MAX_VALUE
            var bestCam: FlockCameraDto? = null
            for (c in flockCameras) {
                if (c.lat != 0.0 && c.lon != 0.0) {
                    val d = Geo.distanceM(loc.lat, loc.lon, c.lat, c.lon)
                    if (d < minD) {
                        minD = d
                        bestCam = c
                    }
                }
            }
            if (bestCam != null && minD < settings.maxDetectionRangeM) {
                closestCam = bestCam to minD
                alertManager.updateProximityDistance(minD)
            } else {
                closestCam = null
                alertManager.updateProximityDistance(Double.MAX_VALUE)
            }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF090D14))
            .padding(16.dp)
    ) {
        // Header
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column {
                Text(
                    text = "ALPR RADAR DETECTOR",
                    fontSize = 18.sp,
                    fontWeight = FontWeight.Bold,
                    color = Color(0xFF00E5FF)
                )
                Text(
                    text = "Proximity alerts & surveillance camera locator",
                    fontSize = 11.sp,
                    color = Color(0xFF8A93A6)
                )
            }
            Row {
                IconButton(onClick = {
                    alertManager.updateSettings(settings.copy(masterSoundEnabled = !settings.masterSoundEnabled))
                }) {
                    Icon(
                        imageVector = if (settings.masterSoundEnabled) Icons.Default.Notifications else Icons.Default.NotificationsOff,
                        contentDescription = "Sound Alert",
                        tint = if (settings.masterSoundEnabled) Color(0xFF6CFF8A) else Color(0xFF8A93A6)
                    )
                }
                IconButton(onClick = {
                    alertManager.updateSettings(settings.copy(masterHapticsEnabled = !settings.masterHapticsEnabled))
                }) {
                    Icon(
                        imageVector = Icons.Default.Vibration,
                        contentDescription = "Haptics",
                        tint = if (settings.masterHapticsEnabled) Color(0xFF6CFF8A) else Color(0xFF8A93A6)
                    )
                }
                IconButton(onClick = { showSettingsSheet = true }) {
                    Icon(
                        imageVector = Icons.Default.Settings,
                        contentDescription = "Settings",
                        tint = Color(0xFFE6EDF7)
                    )
                }
            }
        }

        Spacer(modifier = Modifier.height(12.dp))

        // HUD Banner
        val alert = closestCam
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(
                containerColor = if (alert != null && alert.second < 200.0) Color(0x33FF4D4D)
                else if (alert != null) Color(0x28FF9100)
                else Color(0x1A00E5FF)
            ),
            shape = RoundedCornerShape(12.dp)
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(14.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = if (alert != null) "⚠️" else "🛡️",
                    fontSize = 28.sp
                )
                Spacer(modifier = Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    if (alert != null) {
                        val cam = alert.first
                        val distFt = (alert.second * 3.28084).roundToInt()
                        val passes = cam.passCount
                        Text(
                            text = if (passes > 0) "FLOCK ALPR: $distFt FT AHEAD · PASSED ${passes}x" else "FLOCK ALPR: $distFt FT AHEAD",
                            fontSize = 15.sp,
                            fontWeight = FontWeight.Bold,
                            color = if (passes > 0 || alert.second < 200.0) Color(0xFFFF4D4D) else Color(0xFFFF9100)
                        )
                        Text(
                            text = "${cam.model} (${cam.operatorName.ifEmpty { "Flock Safety" }}) · Dir: ${cam.direction.ifEmpty { "Omni" }}",
                            fontSize = 12.sp,
                            color = Color(0xFFE6EDF7)
                        )
                    } else {
                        Text(
                            text = "RADAR ARMED · NO ALPRS IN RANGE",
                            fontSize = 14.sp,
                            fontWeight = FontWeight.Bold,
                            color = Color(0xFF00E5FF)
                        )
                        Text(
                            text = "Monitoring Wi-Fi/BLE beacons, ${flockCameras.size} known cameras",
                            fontSize = 12.sp,
                            color = Color(0xFF8A93A6)
                        )
                    }
                }
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        // Use this phone's camera as an ALPR feeding FalconEyez (alpr/)
        org.sworrl.beaconfix.alpr.ui.AlprDetectorCard(onOpen = onAlpr)

        Spacer(modifier = Modifier.height(8.dp))

        // Vehicle & License Plate Card
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = Color(0xFF0F1A2C)),
            shape = RoundedCornerShape(8.dp)
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("🚙", fontSize = 16.sp)
                    Spacer(modifier = Modifier.width(8.dp))
                    Column {
                        Text(
                            text = activePlate?.let { p -> listOf(p.state.uppercase(), p.displayPlate.ifBlank { p.plate }).filter { it.isNotBlank() }.joinToString(" · ") } ?: "No plate set",
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            color = Color(0xFF00E5FF)
                        )
                        Text(
                            text = activePlate?.let { p -> listOf(p.vehicleDesc, "Active ALPR Audit").filter { it.isNotBlank() }.joinToString(" · ") } ?: "Register your plate on the desktop (ALPR → Plates)",
                            fontSize = 10.sp,
                            color = Color(0xFF8A93A6)
                        )
                    }
                }
                Surface(
                    color = Color(0x2200E5FF),
                    shape = RoundedCornerShape(4.dp)
                ) {
                    Text(
                        text = "PRIMARY",
                        color = Color(0xFF00E5FF),
                        fontSize = 9.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
                    )
                }
            }
        }

        Spacer(modifier = Modifier.height(10.dp))

        // Circular Radar Sweep Scope
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .aspectRatio(1.1f),
            contentAlignment = Alignment.Center
        ) {
            val transition = rememberInfiniteTransition(label = "RadarSweep")
            val angle by transition.animateFloat(
                initialValue = 0f,
                targetValue = 360f,
                animationSpec = infiniteRepeatable(
                    animation = tween(2800, easing = LinearEasing),
                    repeatMode = RepeatMode.Restart
                ),
                label = "RadarAngle"
            )

            Canvas(modifier = Modifier.fillMaxSize()) {
                val center = Offset(size.width / 2f, size.height / 2f)
                val maxRadius = (size.minDimension / 2f) * 0.90f

                // Concentric Range Rings
                drawCircle(
                    color = Color(0xFF0F1E33),
                    radius = maxRadius,
                    center = center
                )
                val rings = listOf(0.33f to "500 ft", 0.66f to "1000 ft", 1.0f to "2500 ft")
                for ((ratio, _) in rings) {
                    drawCircle(
                        color = Color(0xFF00E5FF).copy(alpha = 0.25f),
                        radius = maxRadius * ratio,
                        center = center,
                        style = Stroke(
                            width = 1.5f,
                            pathEffect = PathEffect.dashPathEffect(floatArrayOf(8f, 6f), 0f)
                        )
                    )
                }

                // Crosshairs
                drawLine(
                    color = Color(0xFF00E5FF).copy(alpha = 0.20f),
                    start = Offset(center.x, center.y - maxRadius),
                    end = Offset(center.x, center.y + maxRadius),
                    strokeWidth = 1.2f
                )
                drawLine(
                    color = Color(0xFF00E5FF).copy(alpha = 0.20f),
                    start = Offset(center.x - maxRadius, center.y),
                    end = Offset(center.x + maxRadius, center.y),
                    strokeWidth = 1.2f
                )

                // Rotating Radar Sweep Line
                val rad = Math.toRadians(angle.toDouble())
                val sweepEnd = Offset(
                    (center.x + maxRadius * cos(rad)).toFloat(),
                    (center.y + maxRadius * sin(rad)).toFloat()
                )
                drawLine(
                    brush = Brush.radialGradient(
                        colors = listOf(Color(0xFF00E5FF), Color(0x0000E5FF)),
                        center = center,
                        radius = maxRadius
                    ),
                    start = center,
                    end = sweepEnd,
                    strokeWidth = 3f
                )

                // Plot Camera Blips on Radar
                val loc = myLoc
                if (loc != null && loc.lat != 0.0) {
                    for (cam in flockCameras) {
                        if (cam.lat == 0.0 && cam.lon == 0.0) continue
                        val distM = Geo.distanceM(loc.lat, loc.lon, cam.lat, cam.lon)
                        if (distM < settings.maxDetectionRangeM) {
                            val rRatio = (distM / settings.maxDetectionRangeM).toFloat().coerceIn(0.05f, 0.98f)
                            val camBearing = Geo.bearingDeg(loc.lat, loc.lon, cam.lat, cam.lon)
                            val camRad = Math.toRadians(camBearing - 90.0)
                            val blipOffset = Offset(
                                (center.x + (maxRadius * rRatio) * cos(camRad)).toFloat(),
                                (center.y + (maxRadius * rRatio) * sin(camRad)).toFloat()
                            )
                            val blipColor = if (distM < 150.0) Color(0xFFFF4D4D)
                            else if (cam.vetted) Color(0xFF00E5FF)
                            else Color(0xFFFF9100)

                            // Blip halo & dot
                            drawCircle(color = blipColor.copy(alpha = 0.35f), radius = 10f, center = blipOffset)
                            drawCircle(color = blipColor, radius = 4.5f, center = blipOffset)
                        }
                    }
                }

                // Own vehicle position at center
                drawCircle(color = Color(0xFF35D6FF), radius = 6f, center = center)
                drawCircle(color = Color.White, radius = 2.5f, center = center)
            }
        }

        Spacer(modifier = Modifier.height(10.dp))

        // Nearby Camera List Header
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = "NEARBY SURVEILLANCE & FLOCK CAMERAS",
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
                color = Color(0xFF8A93A6)
            )
            Surface(
                onClick = { live.syncNationwideUs() },
                color = Color(0xFF1E293B),
                shape = RoundedCornerShape(6.dp),
                border = BorderStroke(1.dp, Color(0xFF00E5FF).copy(alpha = 0.5f))
            ) {
                Row(
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text("🇺🇸 Sync US", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = Color(0xFF00E5FF))
                }
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        // List of cameras
        val loc = myLoc
        val sortedCams = remember(loc, flockCameras) {
            if (loc != null && loc.lat != 0.0) {
                flockCameras.map { it to Geo.distanceM(loc.lat, loc.lon, it.lat, it.lon) }
                    .sortedBy { it.second }
            } else {
                flockCameras.map { it to 0.0 }
            }
        }

        LazyColumn(
            modifier = Modifier.fillMaxWidth().weight(1f),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            items(sortedCams.take(20)) { (cam, d) ->
                Card(
                    modifier = Modifier.fillMaxWidth(),
                    colors = CardDefaults.cardColors(containerColor = Color(0xFF131A26)),
                    shape = RoundedCornerShape(8.dp)
                ) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        Column {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Text(
                                    text = if (cam.passCount > 0) "🚨 " else if (cam.vetted) "🛡️ " else "📷 ",
                                    fontSize = 14.sp
                                )
                                Text(
                                    text = "${cam.model} (${cam.operatorName.ifEmpty { "Flock Safety" }})",
                                    fontWeight = FontWeight.SemiBold,
                                    fontSize = 13.sp,
                                    color = if (cam.passCount > 0) Color(0xFFFF4D4D) else Color(0xFFE6EDF7)
                                )
                                if (cam.passCount > 0) {
                                    Spacer(modifier = Modifier.width(6.dp))
                                    Surface(
                                        color = Color(0x33FF2A4B),
                                        shape = RoundedCornerShape(4.dp)
                                    ) {
                                        Text(
                                            text = "${cam.passCount}x",
                                            color = Color(0xFFFF2A4B),
                                            fontSize = 10.sp,
                                            fontWeight = FontWeight.Bold,
                                            modifier = Modifier.padding(horizontal = 4.dp, vertical = 2.dp)
                                        )
                                    }
                                }
                            }
                            Text(
                                text = "Dir: ${cam.direction.ifEmpty { "Omni" }} · Status: ${if (cam.vetted) "Field Vetted" else "Candidate"}",
                                fontSize = 11.sp,
                                color = Color(0xFF8A93A6)
                            )
                        }
                        if (d > 0.0) {
                            Text(
                                text = if (d < 1000) "${(d * 3.28084).roundToInt()} ft" else "${(d / 1609.34).format(1)} mi",
                                fontSize = 13.sp,
                                fontWeight = FontWeight.Bold,
                                color = if (d < 300) Color(0xFFFF9100) else Color(0xFF00E5FF)
                            )
                        }
                    }
                }
            }
        }
    }

    // Settings Bottom Sheet for Sound and Vibration Customization
    if (showSettingsSheet) {
        val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
        ModalBottomSheet(
            onDismissRequest = { showSettingsSheet = false },
            sheetState = sheetState,
            containerColor = Color(0xFF101622)
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(20.dp)
            ) {
                Text(
                    text = "Detector Sound & Vibration Settings",
                    fontSize = 18.sp,
                    fontWeight = FontWeight.Bold,
                    color = Color(0xFFE6EDF7)
                )
                Text(
                    text = "Customize alert tones and haptics per device detection type",
                    fontSize = 12.sp,
                    color = Color(0xFF8A93A6)
                )

                Spacer(modifier = Modifier.height(16.dp))

                // Geiger proximity mode toggle
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Column {
                        Text("Proximity Geiger Mode", color = Color(0xFFE6EDF7), fontWeight = FontWeight.Medium)
                        Text("Beep rate accelerates as you approach ALPRs", fontSize = 11.sp, color = Color(0xFF8A93A6))
                    }
                    Switch(
                        checked = settings.geigerModeEnabled,
                        onCheckedChange = {
                            alertManager.updateSettings(settings.copy(geigerModeEnabled = it))
                        }
                    )
                }

                HorizontalDivider(color = Color(0x338A93A6), modifier = Modifier.padding(vertical = 12.dp))

                // ALPR / Flock Sound & Vibe
                DeviceAlertConfigRow(
                    title = "Flock & ALPR Cameras",
                    sound = settings.soundAlpr,
                    haptic = settings.vibeAlpr,
                    onSoundChange = { alertManager.updateSettings(settings.copy(soundAlpr = it)); alertManager.triggerAlert(DetectionType.ALPR_FLOCK) },
                    onHapticChange = { alertManager.updateSettings(settings.copy(vibeAlpr = it)); alertManager.triggerAlert(DetectionType.ALPR_FLOCK) }
                )

                Spacer(modifier = Modifier.height(10.dp))

                // BLE Trackers Sound & Vibe
                DeviceAlertConfigRow(
                    title = "BLE Trackers & Beacons",
                    sound = settings.soundBle,
                    haptic = settings.vibeBle,
                    onSoundChange = { alertManager.updateSettings(settings.copy(soundBle = it)); alertManager.triggerAlert(DetectionType.BLE_TRACKER) },
                    onHapticChange = { alertManager.updateSettings(settings.copy(vibeBle = it)); alertManager.triggerAlert(DetectionType.BLE_TRACKER) }
                )

                Spacer(modifier = Modifier.height(10.dp))

                // Wi-Fi Beacons Sound & Vibe
                DeviceAlertConfigRow(
                    title = "Unmapped Wi-Fi Beacons",
                    sound = settings.soundWifi,
                    haptic = settings.vibeWifi,
                    onSoundChange = { alertManager.updateSettings(settings.copy(soundWifi = it)); alertManager.triggerAlert(DetectionType.WIFI_BEACON) },
                    onHapticChange = { alertManager.updateSettings(settings.copy(vibeWifi = it)); alertManager.triggerAlert(DetectionType.WIFI_BEACON) }
                )

                Spacer(modifier = Modifier.height(24.dp))
            }
        }
    }
}

@Composable
fun DeviceAlertConfigRow(
    title: String,
    sound: SoundTone,
    haptic: HapticPattern,
    onSoundChange: (SoundTone) -> Unit,
    onHapticChange: (HapticPattern) -> Unit
) {
    var soundExpanded by remember { mutableStateOf(false) }
    var hapticExpanded by remember { mutableStateOf(false) }

    Column(modifier = Modifier.fillMaxWidth()) {
        Text(text = title, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, color = Color(0xFF00E5FF))
        Spacer(modifier = Modifier.height(4.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            // Sound dropdown
            Box(modifier = Modifier.weight(1f)) {
                Surface(
                    color = Color(0xFF192231),
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier.fillMaxWidth().clickable { soundExpanded = true }
                ) {
                    Row(
                        modifier = Modifier.padding(horizontal = 10.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = "🔊 ${sound.displayName.take(16)}…",
                            fontSize = 11.sp,
                            color = Color(0xFFE6EDF7),
                            maxLines = 1
                        )
                    }
                }
                androidx.compose.material3.DropdownMenu(
                    expanded = soundExpanded,
                    onDismissRequest = { soundExpanded = false }
                ) {
                    SoundTone.entries.forEach { tone ->
                        DropdownMenuItem(
                            text = { Text(tone.displayName) },
                            onClick = {
                                onSoundChange(tone)
                                soundExpanded = false
                            }
                        )
                    }
                }
            }

            // Haptic dropdown
            Box(modifier = Modifier.weight(1f)) {
                Surface(
                    color = Color(0xFF192231),
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier.fillMaxWidth().clickable { hapticExpanded = true }
                ) {
                    Row(
                        modifier = Modifier.padding(horizontal = 10.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = "📳 ${haptic.displayName.take(16)}…",
                            fontSize = 11.sp,
                            color = Color(0xFFE6EDF7),
                            maxLines = 1
                        )
                    }
                }
                androidx.compose.material3.DropdownMenu(
                    expanded = hapticExpanded,
                    onDismissRequest = { hapticExpanded = false }
                ) {
                    HapticPattern.entries.forEach { pattern ->
                        DropdownMenuItem(
                            text = { Text(pattern.displayName) },
                            onClick = {
                                onHapticChange(pattern)
                                hapticExpanded = false
                            }
                        )
                    }
                }
            }
        }
    }
}

private fun Double.format(digits: Int): String = String.format("%.${digits}f", this)
