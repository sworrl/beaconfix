// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.collector

import android.content.Context
import android.util.Log

/**
 * Passive detection of Flock Safety hardware and other surveillance gear in Wi-Fi scans (docs/DETECTION.md). The rules
 * are the shared data/signatures/surveillance.json, packaged as the asset `signatures/surveillance.json` at build time
 * (app/build.gradle.kts), and evaluated by [SurveillanceSignatures] exactly as the desktop does. The phone has no BLE
 * scanner of its own; [evaluateBle] is there so the shared cases cover both platforms' rules.
 */
object FlockDetectorKotlin {
    const val ASSET = "signatures/surveillance.json"

    @Volatile private var sigs: SurveillanceSignatures? = null

    /** Load the packaged rule set once (cheap after the first call). */
    fun ensureLoaded(context: Context) {
        if (sigs != null) return
        synchronized(this) {
            if (sigs != null) return
            sigs = try {
                context.assets.open(ASSET).use { SurveillanceSignatures.parse(it.readBytes().decodeToString()) }
            } catch (e: Exception) {
                Log.w("BeaconFix", "surveillance signatures unusable: ${e.message}")
                null
            }
        }
    }

    /** Use [s] (tests, or a newer set). */
    fun use(s: SurveillanceSignatures) { sigs = s }

    val version: Int get() = sigs?.version ?: 0

    fun evaluateWifi(bssid: String, ssid: String, probedSsids: List<String> = emptyList()): SurveillanceSignatures.Detection =
        sigs?.evaluateWifi(bssid, ssid, probedSsids) ?: SurveillanceSignatures.Detection()

    fun evaluateBle(mac: String, name: String, serviceUuids: List<String> = emptyList(), companies: List<SurveillanceSignatures.BleCompany> = emptyList()): SurveillanceSignatures.Detection =
        sigs?.evaluateBle(mac, name, serviceUuids, companies) ?: SurveillanceSignatures.Detection()
}
