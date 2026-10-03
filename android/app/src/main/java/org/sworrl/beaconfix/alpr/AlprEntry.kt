package org.sworrl.beaconfix.alpr

import android.app.Activity
import android.content.Context
import android.os.Bundle
import android.widget.Toast
import dagger.hilt.EntryPoint
import dagger.hilt.InstallIn
import dagger.hilt.android.EntryPointAccessors
import dagger.hilt.components.SingletonComponent
import org.sworrl.beaconfix.MainActivity

@EntryPoint
@InstallIn(SingletonComponent::class)
interface AlprEntryPoint {
    fun alprSettings(): AlprSettings
    fun alprStatus(): AlprStatus
    fun falconLink(): FalconLink
    fun hotlistStore(): HotlistStore
    fun uplink(): AlprUplink
}

fun Context.alprEntry(): AlprEntryPoint = EntryPointAccessors.fromApplication(applicationContext, AlprEntryPoint::class.java)

/**
 * A no-UI trampoline that starts the dash cam from a foreground activity (the Quick Settings tile uses it: Android 14+
 * lets a camera service start only while the app is in the foreground). Without camera permission it opens the ALPR
 * screen instead, which asks.
 */
@android.annotation.SuppressLint("CustomSplashScreen")   // not a splash: a no-UI trampoline
class AlprLaunchActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val granted = checkSelfPermission(android.Manifest.permission.CAMERA) == android.content.pm.PackageManager.PERMISSION_GRANTED
        if (granted) {
            alprEntry().alprSettings().update { it.copy(enabled = true) }
            runCatching { DashCamService.start(this) }.onFailure { Toast.makeText(this, "Couldn't start the dash cam: ${it.message}", Toast.LENGTH_LONG).show() }
        } else {
            startActivity(android.content.Intent(this, MainActivity::class.java).putExtra("action", "alpr").addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
        }
        finish()
    }
}
