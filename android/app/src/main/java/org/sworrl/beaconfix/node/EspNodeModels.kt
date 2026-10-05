package org.sworrl.beaconfix.node

import kotlinx.serialization.Serializable

enum class EspConnectionState {
    DISCONNECTED,
    SCANNING,
    CONNECTED_BLE,
    CONNECTED_UDP,
    CONNECTED_USB
}

@Serializable
data class EspNodeStatus(
    val version: String = "3.10.0",
    val node: String = "",
    val uptime: Long = 0,
    val heap: Long = 0,
    val ch: Int = 1,
    val hop: Boolean = true,
    val pps: Long = 0,
    val total: Long = 0,
    val probes: Long = 0,
    val beacons: Long = 0,
    val deauths: Long = 0,
    val ble: Int = 0,
    val batt_mv: Int = 0,
    val batt_pct: Int = 0,
    val batt_state: String = "unknown",
    val charging: Boolean = false,
    val attached_dev: String = "",
    val following: Boolean = false,
    val has_gps: Boolean = false,
    val lat: Double = 0.0,
    val lon: Double = 0.0
)

data class EspProbeEvent(
    val mac: String,
    val ssid: String,
    val rssi: Int,
    val ch: Int,
    val seq: Int,
    val ts: Long,
    val timeReceivedMs: Long = System.currentTimeMillis(),
    val lat: Double? = null,
    val lon: Double? = null
)

data class EspBeaconEvent(
    val bssid: String,
    val ssid: String,
    val rssi: Int,
    val ch: Int,
    val ts: Long,
    val timeReceivedMs: Long = System.currentTimeMillis()
)

data class EspAlertEvent(
    val event: String,
    val sa: String,
    val da: String,
    val reason: Int,
    val rssi: Int,
    val ch: Int,
    val ts: Long,
    val timeReceivedMs: Long = System.currentTimeMillis()
)

data class EspBleTrackerEvent(
    val kind: String,
    val mac: String,
    val name: String,
    val rssi: Int,
    val payload: String,
    val ts: Long,
    val timeReceivedMs: Long = System.currentTimeMillis()
)

@Serializable
data class MeshPeerNode(
    val name: String,
    val mac: String = "",
    val role: String = "mobile",
    val version: String = "3.10.2",
    val battMv: Int = 0,
    val battPct: Int = -1,
    val battState: String = "unknown",
    val charging: Boolean = false,
    val battMah: Int = 240,
    val hops: Int = 0,
    val rssi: Int = 0,
    val pps: Long = 0,
    val lastSeen: Long = System.currentTimeMillis(),
    val isOnline: Boolean = true,
    val following: Boolean = false,
    val attachedDevice: String = "",
    val hasLocation: Boolean = false,
    val lat: Double = 0.0,
    val lon: Double = 0.0,
    val accM: Float = 5.0f,
    val via: String = "",
    val prevMac: String = "",
    val route: String = "",
    val otaPct: Int = -1,
    val otaState: String = "",
    val isUsb: Boolean = false,
    val usbPort: String = "",
    val transport: String = "mesh"
)

