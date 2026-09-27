package org.sworrl.beaconfix.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

val Cyan = Color(0xFF35D6FF)
val Gold = Color(0xFFFFD166)
val Magenta = Color(0xFFFF4FD8)
val Green = Color(0xFF6CFF8A)
val Orange = Color(0xFFFF9F43)
val Red = Color(0xFFFF4D4D)
val Ink = Color(0xFF0B101A)
val Slate = Color(0xFF9FB0C8)

private val Dark = darkColorScheme(primary = Cyan, secondary = Gold, tertiary = Magenta, background = Ink, surface = Color(0xFF111827), error = Red)
private val Light = lightColorScheme(primary = Color(0xFF0E7C9B), secondary = Color(0xFF9A6A00), tertiary = Color(0xFFA0138E))

@Composable
fun BeaconFixTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val ctx = LocalContext.current
    val scheme = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> if (dark) dynamicDarkColorScheme(ctx) else dynamicLightColorScheme(ctx)
        dark -> Dark
        else -> Light
    }
    MaterialTheme(colorScheme = scheme, content = content)
}
