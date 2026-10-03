package org.sworrl.beaconfix.widget

import android.content.Context
import android.content.Intent
import androidx.compose.runtime.Composable
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.glance.GlanceId
import androidx.glance.GlanceModifier
import androidx.glance.GlanceTheme
import androidx.glance.action.clickable
import androidx.glance.appwidget.GlanceAppWidget
import androidx.glance.appwidget.GlanceAppWidgetReceiver
import androidx.glance.appwidget.action.actionStartActivity
import androidx.glance.appwidget.cornerRadius
import androidx.glance.appwidget.provideContent
import androidx.glance.background
import androidx.glance.layout.Alignment
import androidx.glance.layout.Box
import androidx.glance.layout.Column
import androidx.glance.layout.Row
import androidx.glance.layout.Spacer
import androidx.glance.layout.fillMaxSize
import androidx.glance.layout.fillMaxWidth
import androidx.glance.layout.height
import androidx.glance.layout.padding
import androidx.glance.text.FontWeight
import androidx.glance.text.Text
import androidx.glance.text.TextStyle
import androidx.glance.unit.ColorProvider
import org.sworrl.beaconfix.MainActivity
import androidx.compose.ui.graphics.Color

class DetectorWidget : GlanceAppWidget() {
    override suspend fun provideGlance(context: Context, id: GlanceId) {
        val prefs = context.getSharedPreferences("beaconfix_detector_prefs", Context.MODE_PRIVATE)
        val lastCamModel = prefs.getString("last_cam_model", "Flock Falcon") ?: "Flock Falcon"
        val lastCamDistFt = prefs.getInt("last_cam_dist_ft", 0)
        val lastCamDir = prefs.getString("last_cam_dir", "Omni") ?: "Omni"
        val isArmed = prefs.getBoolean("master_sound", true)

        provideContent {
            GlanceTheme {
                DetectorWidgetContent(
                    camModel = lastCamModel,
                    distFt = lastCamDistFt,
                    direction = lastCamDir,
                    armed = isArmed
                )
            }
        }
    }

    @Composable
    private fun DetectorWidgetContent(
        camModel: String,
        distFt: Int,
        direction: String,
        armed: Boolean
    ) {
        val launchIntent = Intent(Intent.ACTION_VIEW).apply {
            setClassName("org.sworrl.beaconfix", "org.sworrl.beaconfix.MainActivity")
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
            putExtra("open_tab", "detector")
        }

        Box(
            modifier = GlanceModifier
                .fillMaxSize()
                .background(ColorProvider(Color(0xFF0C1017)))
                .cornerRadius(16.dp)
                .padding(12.dp)
                .clickable(actionStartActivity(launchIntent)),
            contentAlignment = Alignment.CenterStart
        ) {
            Column(modifier = GlanceModifier.fillMaxWidth()) {
                Row(
                    modifier = GlanceModifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = if (distFt in 1..2500) "⚠️ ALPR DETECTED" else "🛡️ RADAR ARMED",
                        style = TextStyle(
                            color = ColorProvider(if (distFt in 1..2500) Color(0xFFFF9100) else Color(0xFF00E5FF)),
                            fontSize = 13.sp,
                            fontWeight = FontWeight.Bold
                        )
                    )
                    Spacer(modifier = GlanceModifier.defaultWeight())
                    Text(
                        text = if (armed) "🔊 ON" else "🔇 MUTE",
                        style = TextStyle(
                            color = ColorProvider(Color(0xFF9FB0C8)),
                            fontSize = 11.sp
                        )
                    )
                }

                Spacer(modifier = GlanceModifier.height(4.dp))

                Text(
                    text = if (distFt in 1..2500) "$camModel · $distFt ft ($direction)" else "No ALPRs in range · scanning",
                    style = TextStyle(
                        color = ColorProvider(Color(0xFFE6EDF7)),
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Medium
                    ),
                    maxLines = 1
                )

                Spacer(modifier = GlanceModifier.height(2.dp))

                Text(
                    text = "Tap to open Detector Radar Mode",
                    style = TextStyle(
                        color = ColorProvider(Color(0xFF6CFF8A)),
                        fontSize = 10.sp
                    )
                )
            }
        }
    }
}

class DetectorWidgetReceiver : GlanceAppWidgetReceiver() {
    override val glanceAppWidget: GlanceAppWidget = DetectorWidget()
}
