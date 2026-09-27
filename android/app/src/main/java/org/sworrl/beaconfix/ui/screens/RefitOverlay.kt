package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import kotlinx.coroutines.flow.SharedFlow
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.sworrl.beaconfix.estimate.Geo
import org.sworrl.beaconfix.estimate.RefitEvent

/**
 * The "triangulation update" animation, 3.2 s at the display's frame rate: vantage points pop in as small triangles, dashed
 * range rings expand from each to its distance to the new position (so they visibly intersect there), the marker glides
 * old → new along a dashed trail while the uncertainty circle shrinks, a crosshair locks with two pulses, a label rises,
 * a brief sparkle. Drawn on a Compose Canvas above the osmdroid view using its projection.
 */
@Composable
fun RefitOverlay(map: MapView?, events: SharedFlow<RefitEvent>, onTicker: (String) -> Unit = {}) {
    var active by remember { mutableStateOf<RefitEvent?>(null) }
    var t0 by remember { mutableStateOf(0L) }
    var now by remember { mutableStateOf(0L) }
    val haptic = LocalHapticFeedback.current
    LaunchedEffect(events) { events.collect { e -> active = e; t0 = System.nanoTime(); haptic.performHapticFeedback(HapticFeedbackType.LongPress); onTicker("🎯 ${e.ssid.ifEmpty { e.bssid }} refit: ±${e.acc.toInt()} m from ${e.n} samples, ${e.vantage} vantage points") } }
    LaunchedEffect(active) { while (active != null) { withFrameNanos { now = it }; if ((now - t0) / 1e9 > 3.2) active = null } }
    val e = active ?: return
    val m = map ?: return
    Canvas(Modifier.fillMaxSize()) {
        val t = ((now - t0) / 1e9).toFloat().coerceIn(0f, 3.2f)
        val proj = m.projection
        fun px(lat: Double, lon: Double): Offset { val p = proj.toPixels(GeoPoint(lat, lon), null); return Offset(p.x.toFloat(), p.y.toFloat()) }
        val mpp = 156543.03 * Math.cos(Math.toRadians(e.lat)) / Math.pow(2.0, m.zoomLevelDouble)
        val to = px(e.lat, e.lon)
        val from = if (e.fromLat != null && e.fromLon != null) px(e.fromLat, e.fromLon) else to
        val gold = Color(0xFFFFD166); val cyan = Color(0xFF35D6FF); val white = Color.White
        // 1. vantage points pop in (0–0.8 s), staggered
        val palette = listOf(cyan, Color(0xFFFF4FD8), Color(0xFF6CFF8A), Color(0xFFFF9F43), Color(0xFFA29BFE), Color(0xFFFFD166))
        val deviceNames = e.vantagePoints.map { it.device }.distinct()
        e.vantagePoints.forEachIndexed { i, v ->
            val s = ((t - i * 0.12f) / 0.35f).coerceIn(0f, 1f); if (s <= 0f) return@forEachIndexed
            val col = palette[deviceNames.indexOf(v.device).coerceAtLeast(0) % palette.size]     // one colour per observing device
            val p = px(v.lat, v.lon); val r = 7f * (1.2f - 0.2f * s) * s
            val tri = Path().apply { moveTo(p.x, p.y - r); lineTo(p.x + r, p.y + r); lineTo(p.x - r, p.y + r); close() }
            drawPath(tri, col.copy(alpha = 0.9f * s)); drawPath(tri, white.copy(alpha = 0.6f * s), style = Stroke(1.5f))
            if (v.device.isNotEmpty() && s > 0.5f) drawContext.canvas.nativeCanvas.drawText(if (v.device.contains("Pixel", true) || v.device.contains("phone", true) || v.device.contains("android", true)) "📱" else "💻", p.x + 9f, p.y - 6f, android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply { textSize = 22f; alpha = (255 * s).toInt() })
            // 2. range rings expand from each vantage point to its distance to the new position (0.6–2.0 s)
            val g = ((t - 0.6f - i * 0.08f) / 1.2f).coerceIn(0f, 1f)
            if (g > 0f) {
                val dist = Geo.distanceM(v.lat, v.lon, e.lat, e.lon) / mpp
                drawCircle(col.copy(alpha = 0.55f * (1f - 0.4f * g)), radius = (dist * g).toFloat().coerceAtLeast(1f), center = p, style = Stroke(1.5f, pathEffect = PathEffect.dashPathEffect(floatArrayOf(6f, 6f))))
            }
        }
        // 3. marker glides old → new along a dashed trail (0.9–2.2 s) while the uncertainty circle shrinks
        val gl = ((t - 0.9f) / 1.3f).coerceIn(0f, 1f); val ease = 1f - (1f - gl) * (1f - gl) * (1f - gl)
        val cur = Offset(from.x + (to.x - from.x) * ease, from.y + (to.y - from.y) * ease)
        if (from != to) drawLine(gold.copy(alpha = 0.6f), from, cur, strokeWidth = 2f, pathEffect = PathEffect.dashPathEffect(floatArrayOf(5f, 5f)))
        val prev = (e.prevAcc ?: (e.acc * 2)).coerceAtLeast(e.acc); val accNow = prev + (e.acc - prev) * ease
        drawCircle(gold.copy(alpha = 0.15f), radius = (accNow / mpp).toFloat().coerceIn(4f, 600f), center = cur)
        drawCircle(gold.copy(alpha = 0.8f), radius = (accNow / mpp).toFloat().coerceIn(4f, 600f), center = cur, style = Stroke(1.5f))
        drawCircle(gold, radius = 6f, center = cur); drawCircle(white, radius = 6f, center = cur, style = Stroke(1.5f))
        // 4. crosshair lock pulses twice (2.0–3.0 s), label rises, sparkle
        val lk = ((t - 2.0f) / 1.0f).coerceIn(0f, 1f)
        if (lk > 0f) {
            val pulse = (Math.sin(lk * Math.PI * 2 * 2).toFloat() * 0.5f + 0.5f)
            val r = 14f + 6f * pulse; val a = (1f - lk * 0.6f)
            for (d in listOf(Offset(-1f, 0f), Offset(1f, 0f), Offset(0f, -1f), Offset(0f, 1f))) drawLine(white.copy(alpha = a), Offset(to.x + d.x * (r + 4), to.y + d.y * (r + 4)), Offset(to.x + d.x * (r + 14), to.y + d.y * (r + 14)), strokeWidth = 2f)
            drawCircle(white.copy(alpha = a * 0.8f), radius = r, center = to, style = Stroke(1.5f))
            val label = "±${e.acc.toInt()} m · ${e.n} samples"
            drawContext.canvas.nativeCanvas.apply {
                val p = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply { textSize = 30f; color = android.graphics.Color.WHITE; alpha = (255 * a).toInt(); isFakeBoldText = true; textAlign = android.graphics.Paint.Align.CENTER }
                val bg = android.graphics.Paint().apply { color = android.graphics.Color.argb((200 * a).toInt(), 8, 13, 20) }
                val w = p.measureText(label); val y = to.y - 34f - 18f * lk
                drawRoundRect(to.x - w / 2 - 10, y - 26, to.x + w / 2 + 10, y + 10, 8f, 8f, bg); drawText(label, to.x, y, p)
            }
            if (lk in 0.15f..0.55f) { val sp = (lk - 0.15f) / 0.4f; for (k in 0 until 8) { val ang = k * Math.PI / 4; val d = 18f + 22f * sp; drawCircle(gold.copy(alpha = 1f - sp), radius = 2.5f * (1f - sp) + 0.5f, center = Offset(to.x + (Math.cos(ang) * d).toFloat(), to.y + (Math.sin(ang) * d).toFloat())) } }
        }
    }
}
