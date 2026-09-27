package org.sworrl.beaconfix.ui

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat

object Permissions {
    fun foreground(): Array<String> = buildList {
        add(Manifest.permission.ACCESS_FINE_LOCATION); add(Manifest.permission.ACCESS_COARSE_LOCATION)
        if (Build.VERSION.SDK_INT >= 33) { add(Manifest.permission.NEARBY_WIFI_DEVICES); add(Manifest.permission.POST_NOTIFICATIONS) }
    }.toTypedArray()
    fun hasForeground(ctx: Context) = ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
    fun hasBackground(ctx: Context) = Build.VERSION.SDK_INT < 29 || ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_BACKGROUND_LOCATION) == PackageManager.PERMISSION_GRANTED
}

/** Device ranging needs the "Nearby devices" group on Android 12+ (BLE advertise/scan) and, on 13+, nearby Wi-Fi devices (RTT). */
fun Permissions.ranging(): Array<String> = buildList {
    if (android.os.Build.VERSION.SDK_INT >= 31) { add(android.Manifest.permission.BLUETOOTH_SCAN); add(android.Manifest.permission.BLUETOOTH_ADVERTISE); add(android.Manifest.permission.BLUETOOTH_CONNECT) }
    if (android.os.Build.VERSION.SDK_INT >= 33) add(android.Manifest.permission.NEARBY_WIFI_DEVICES)
    if (android.os.Build.VERSION.SDK_INT < 31) add(android.Manifest.permission.ACCESS_FINE_LOCATION)
}.toTypedArray()
fun Permissions.hasRanging(ctx: android.content.Context): Boolean = ranging().all { androidx.core.content.ContextCompat.checkSelfPermission(ctx, it) == android.content.pm.PackageManager.PERMISSION_GRANTED }
