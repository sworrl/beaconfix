package org.sworrl.beaconfix.ui.screens

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.BluetoothSearching
import androidx.compose.material.icons.filled.Lightbulb
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Sensors
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.RadioButtonDefaults
import org.sworrl.beaconfix.node.MeshPeerNode
import androidx.compose.material3.ScrollableTabRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.node.EspConnectionState
import org.sworrl.beaconfix.node.EspNodeManager
import org.sworrl.beaconfix.node.EspProbeEvent
import org.sworrl.beaconfix.ui.Chip
import org.sworrl.beaconfix.ui.EmptyState
import org.sworrl.beaconfix.ui.theme.Cyan
import org.sworrl.beaconfix.ui.theme.Gold
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Orange
import org.sworrl.beaconfix.ui.theme.Red
import org.sworrl.beaconfix.ui.theme.Slate

@SuppressLint("MissingPermission")
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun EspNodesScreen(
    onBack: () -> Unit,
    nodeManager: EspNodeManager = run {
        val ctx = LocalContext.current
        remember {
            dagger.hilt.android.EntryPointAccessors.fromApplication(
                ctx.applicationContext,
                org.sworrl.beaconfix.widget.WidgetEntryPoint::class.java
            ).espNodeManager()
        }
    }
) {
    val ctx = LocalContext.current
    val connectionState by nodeManager.connectionState.collectAsState()
    val status by nodeManager.nodeStatus.collectAsState()
    val probes by nodeManager.recentProbes.collectAsState()
    val alerts by nodeManager.recentAlerts.collectAsState()
    val trackers by nodeManager.recentTrackers.collectAsState()
    val isFollowing by nodeManager.isFollowingPhone.collectAsState()
    val meshPeers by nodeManager.meshPeers.collectAsState()
    var editingBatteryNode by remember { mutableStateOf<MeshPeerNode?>(null) }

    var selectedTab by remember { mutableIntStateOf(0) }
    var discoveredDevices by remember { mutableStateOf<List<BluetoothDevice>>(emptyList()) }
    var isScanningBt by remember { mutableStateOf(false) }
    val coroutineScope = androidx.compose.runtime.rememberCoroutineScope()
    var isFlashingHost by remember { mutableStateOf(false) }
    var hostFlashResult by remember { mutableStateOf("") }

    fun scanForBleNodes() {
        val btManager = ctx.getSystemService(BluetoothManager::class.java) ?: return
        val adapter = btManager.adapter ?: return
        if (!adapter.isEnabled) return
        isScanningBt = true
        discoveredDevices = emptyList()

        val scanner = adapter.bluetoothLeScanner ?: return
        val callback = object : android.bluetooth.le.ScanCallback() {
            override fun onScanResult(callbackType: Int, result: android.bluetooth.le.ScanResult?) {
                val dev = result?.device ?: return
                val name = dev.name ?: ""
                val record = result.scanRecord
                val hasNus = record?.serviceUuids?.any { it.uuid == EspNodeManager.SERVICE_UUID_NUS } == true
                if (hasNus || nodeManager.isBeaconNodeName(name)) {
                    if (discoveredDevices.none { it.address == dev.address }) {
                        discoveredDevices = discoveredDevices + dev
                    }
                }
            }
        }
        scanner.startScan(callback)
        android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
            runCatching { scanner.stopScan(callback) }
            isScanningBt = false
        }, 6000)
    }

    Column(Modifier.fillMaxSize().padding(vertical = 8.dp)) {
        // Header
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("‹ Back") }
            Text("Nodes & Mesh", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.weight(1f))
            if (connectionState != EspConnectionState.DISCONNECTED) {
                OutlinedButton(onClick = { nodeManager.disconnect() }) { Text("Disconnect") }
            }
        }

        // Mesh Network Status Card
        Card(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)
        ) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Box(Modifier.size(10.dp).clip(CircleShape).background(Green))
                        Column {
                            Text("LoRa & Wi-Fi Mesh Network", fontWeight = FontWeight.Bold, style = MaterialTheme.typography.titleMedium)
                            Text("Stratum 1 Master · Sub-ms clock sync active", color = Slate, style = MaterialTheme.typography.bodySmall)
                        }
                    }
                    Surface(color = Green.copy(alpha = 0.15f), shape = RoundedCornerShape(4.dp)) {
                        Text(
                            "${meshPeers.size} ${if (meshPeers.size == 1) "Node" else "Nodes"}",
                            color = Green,
                            fontWeight = FontWeight.Bold,
                            fontSize = 11.sp,
                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
                        )
                    }
                }

                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    OutlinedButton(
                        onClick = { nodeManager.broadcastTimeSync() },
                        modifier = Modifier.weight(1f)
                    ) {
                        Text("Sync Clocks", fontSize = 11.sp)
                    }
                    OutlinedButton(
                        onClick = { nodeManager.sendMeshOtaAbort() },
                        modifier = Modifier.weight(1f)
                    ) {
                        Text("Abort OTA", fontSize = 11.sp)
                    }
                    OutlinedButton(
                        onClick = { nodeManager.triggerNodePing("*") },
                        modifier = Modifier.weight(1f)
                    ) {
                        Text("Ping All", fontSize = 11.sp)
                    }
                }
            }
        }

        // Tabs
        ScrollableTabRow(selectedTabIndex = selectedTab, edgePadding = 16.dp) {
            Tab(selected = selectedTab == 0, onClick = { selectedTab = 0 }, text = { Text("📡 Nodes (${meshPeers.size})") })
            Tab(selected = selectedTab == 1, onClick = { selectedTab = 1 }, text = { Text("⚙️ Node Controls") })
            Tab(selected = selectedTab == 2, onClick = { selectedTab = 2 }, text = { Text("📶 Probes (${probes.size})") })
            Tab(selected = selectedTab == 3, onClick = { selectedTab = 3 }, text = { Text("🛡 Alerts (${alerts.size})") })
            Tab(selected = selectedTab == 4, onClick = { selectedTab = 4 }, text = { Text("🏷 Trackers (${trackers.size})") })
        }

        // Tab Content
        when (selectedTab) {
            0 -> {
                if (meshPeers.isEmpty()) {
                    EmptyState(
                        "📡",
                        "No mesh peers heard yet",
                        "Nodes broadcasting telemetry over UDP 47824 or LoRa mesh beacons will appear here automatically with real-time signal, battery, and role info."
                    )
                } else {
                    LazyColumn(
                        Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 4.dp),
                        verticalArrangement = Arrangement.spacedBy(6.dp)
                    ) {
                        items(meshPeers) { peer ->
                            val isOnline = (System.currentTimeMillis() - peer.lastSeen) < 15000L
                            Card(
                                Modifier.fillMaxWidth(),
                                colors = CardDefaults.cardColors(
                                    containerColor = if (isOnline) MaterialTheme.colorScheme.surfaceVariant else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.6f)
                                )
                            ) {
                                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                                    // Header
                                    Row(
                                        Modifier.fillMaxWidth(),
                                        horizontalArrangement = Arrangement.SpaceBetween,
                                        verticalAlignment = Alignment.CenterVertically
                                    ) {
                                        Row(
                                            verticalAlignment = Alignment.CenterVertically,
                                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                                            modifier = Modifier.weight(1f)
                                        ) {
                                            Box(
                                                Modifier.size(10.dp).clip(CircleShape).background(if (isOnline) Green else Slate)
                                            )
                                            Text(
                                                peer.name.ifEmpty { "Unnamed Node" },
                                                fontWeight = FontWeight.Bold,
                                                style = MaterialTheme.typography.titleSmall
                                            )
                                            Surface(
                                                color = if (peer.role == "master") Gold.copy(alpha = 0.2f) else Cyan.copy(alpha = 0.2f),
                                                shape = RoundedCornerShape(4.dp)
                                            ) {
                                                Text(
                                                    peer.role.uppercase(),
                                                    color = if (peer.role == "master") Gold else Cyan,
                                                    fontWeight = FontWeight.Bold,
                                                    fontSize = 10.sp,
                                                    modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp)
                                                )
                                            }
                                            Surface(
                                                color = MaterialTheme.colorScheme.outline.copy(alpha = 0.15f),
                                                shape = RoundedCornerShape(4.dp)
                                            ) {
                                                Text(
                                                    "v${peer.version}",
                                                    color = Slate,
                                                    fontSize = 10.sp,
                                                    modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp)
                                                )
                                            }
                                        }
                                        Chip(
                                            when {
                                                peer.isUsb -> "🔌 USB + 📡 Gateway"
                                                peer.hops <= 0 -> "Direct (Local)"
                                                peer.hops == 1 -> "1 Hop (Direct RF)"
                                                peer.via.isNotBlank() && peer.via != "Direct" -> "${peer.hops} Hops (via ${peer.via})"
                                                peer.prevMac.isNotBlank() -> "${peer.hops} Hops (via ${peer.prevMac.takeLast(8)})"
                                                else -> "${peer.hops} Hops"
                                            },
                                            if (peer.isUsb) Cyan else if (peer.hops <= 0) Green else if (peer.hops == 1) Cyan else Gold
                                        )
                                    }

                                    // Visual Mesh Route Path Banner
                                    Surface(
                                        color = MaterialTheme.colorScheme.surface.copy(alpha = 0.6f),
                                        shape = RoundedCornerShape(6.dp),
                                        modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)
                                    ) {
                                        Row(
                                            Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                                            verticalAlignment = Alignment.CenterVertically,
                                            horizontalArrangement = Arrangement.spacedBy(6.dp)
                                        ) {
                                            Text("🛤 Route:", color = Slate, fontSize = 11.sp, fontWeight = FontWeight.SemiBold)
                                            if (peer.isUsb) {
                                                Text("Local Host USB (${peer.usbPort.ifEmpty { "/dev/ttyUSB0" }}) ➔ Mesh Gateway", color = Cyan, fontSize = 11.sp, fontWeight = FontWeight.Medium)
                                            } else if (peer.hops <= 0) {
                                                Text("Local USB / BLE Connection ➔ This Device", color = Green, fontSize = 11.sp, fontWeight = FontWeight.Medium)
                                            } else if (peer.hops == 1) {
                                                Text("${peer.name} ──(Direct RF Link)──➔ This Device", color = Green, fontSize = 11.sp, fontWeight = FontWeight.Medium)
                                            } else {
                                                val relayNode = if (peer.via.isNotBlank() && peer.via != "Direct") peer.via else if (peer.prevMac.isNotBlank()) peer.prevMac else "Relay"
                                                Text("${peer.name} ➔ ", color = Slate, fontSize = 11.sp)
                                                Surface(
                                                    color = Cyan.copy(alpha = 0.2f),
                                                    shape = RoundedCornerShape(4.dp)
                                                ) {
                                                    Text(
                                                        "🔄 $relayNode",
                                                        color = Cyan,
                                                        fontSize = 11.sp,
                                                        fontWeight = FontWeight.Bold,
                                                        modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp)
                                                    )
                                                }
                                                Text(" ➔ Phone (${peer.hops} hops)", color = Gold, fontSize = 11.sp, fontWeight = FontWeight.SemiBold)
                                            }
                                        }
                                    }

                                    // Metrics Row
                                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                                        Column {
                                            Text("Power / Battery:", color = Slate, style = MaterialTheme.typography.labelSmall)
                                            if (peer.battState == "no_battery" || peer.battMv < 1200) {
                                                Text("USB 5V (No Batt)", color = Cyan, fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
                                            } else {
                                                Text(
                                                    "${peer.battPct}% (${peer.battMv}mV${if (peer.charging) ", ⚡" else ""})",
                                                    color = if (peer.battPct > 20) Green else Red,
                                                    fontWeight = FontWeight.Bold,
                                                    style = MaterialTheme.typography.bodySmall
                                                )
                                            }
                                            Text("${peer.battMah} mAh cell", color = Slate, style = MaterialTheme.typography.labelSmall)
                                        }
                                        Column {
                                            Text("RF & Mesh:", color = Slate, style = MaterialTheme.typography.labelSmall)
                                            Text("${peer.pps} pps", color = Cyan, fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
                                            if (peer.rssi != 0) {
                                                Text("${peer.rssi} dBm", color = if (peer.rssi > -65) Green else if (peer.rssi > -85) Gold else Red, style = MaterialTheme.typography.labelSmall)
                                            } else {
                                                Text("Host / Local", color = Slate, style = MaterialTheme.typography.labelSmall)
                                            }
                                            if (peer.hops > 1 && peer.route.isNotBlank()) {
                                                Text(peer.route, color = Slate, style = MaterialTheme.typography.labelSmall)
                                            }
                                        }
                                        Column {
                                            Text("Location / Host:", color = Slate, style = MaterialTheme.typography.labelSmall)
                                            if (peer.following) {
                                                Text("Following", color = Green, fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
                                                Text(peer.attachedDevice.ifEmpty { "Host" }.take(12), color = Slate, style = MaterialTheme.typography.labelSmall)
                                            } else if (peer.hasLocation && (peer.lat != 0.0 || peer.lon != 0.0)) {
                                                Text("GPS Fix", color = Cyan, fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
                                                Text(String.format(java.util.Locale.US, "%.4f, %.4f", peer.lat, peer.lon), color = Slate, style = MaterialTheme.typography.labelSmall)
                                            } else {
                                                Text("Fixed / Static", color = Slate, style = MaterialTheme.typography.labelSmall)
                                            }
                                        }
                                    }

                                    // OTA Progress banner if active
                                    if (peer.otaState.isNotBlank() && peer.otaState != "idle") {
                                        Column(Modifier.fillMaxWidth().padding(vertical = 2.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                                            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                                                Text("Mesh OTA: ${peer.otaState}", color = Gold, fontWeight = FontWeight.SemiBold, fontSize = 11.sp)
                                                Text("${if (peer.otaPct >= 0) peer.otaPct else 0}%", color = Gold, fontWeight = FontWeight.Bold, fontSize = 11.sp)
                                            }
                                            LinearProgressIndicator(
                                                progress = { if (peer.otaPct >= 0) peer.otaPct / 100f else 0f },
                                                modifier = Modifier.fillMaxWidth().height(4.dp),
                                                color = Gold
                                            )
                                        }
                                    }

                                    // Node Actions
                                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                        OutlinedButton(
                                            onClick = { nodeManager.triggerNodePing(peer.name) },
                                            modifier = Modifier.weight(1f)
                                        ) {
                                            Text("💓 Blink", fontSize = 11.sp)
                                        }
                                        OutlinedButton(
                                            onClick = { editingBatteryNode = peer },
                                            modifier = Modifier.weight(1f)
                                        ) {
                                            Text("🔋 Capacity", fontSize = 11.sp)
                                        }
                                        OutlinedButton(
                                            onClick = {
                                                coroutineScope.launch {
                                                    nodeManager.attachToThisPhone(!peer.following)
                                                }
                                            },
                                            modifier = Modifier.weight(1f)
                                        ) {
                                            Text(if (peer.following) "Unfollow" else "Follow", fontSize = 11.sp)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            1 -> {
                Column(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(bottom = 16.dp)
                ) {
                    // Connection Card
                    Card(
                        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp),
                        colors = CardDefaults.cardColors(
                            containerColor = when (connectionState) {
                                EspConnectionState.CONNECTED_BLE -> Green.copy(alpha = 0.12f)
                                EspConnectionState.CONNECTED_UDP -> Cyan.copy(alpha = 0.12f)
                                EspConnectionState.CONNECTED_USB -> Gold.copy(alpha = 0.12f)
                                else -> MaterialTheme.colorScheme.surfaceVariant
                            }
                        )
                    ) {
                        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Box(
                                    Modifier.size(12.dp).clip(CircleShape).background(
                                        when (connectionState) {
                                            EspConnectionState.CONNECTED_BLE, EspConnectionState.CONNECTED_UDP, EspConnectionState.CONNECTED_USB -> Green
                                            EspConnectionState.SCANNING -> Orange
                                            else -> Slate
                                        }
                                    )
                                )
                                Spacer(Modifier.size(8.dp))
                                Text(
                                    when (connectionState) {
                                        EspConnectionState.CONNECTED_BLE -> "Connected via Bluetooth LE"
                                        EspConnectionState.CONNECTED_UDP -> "Connected via Wi-Fi (UDP Telemetry)"
                                        EspConnectionState.CONNECTED_USB -> "Connected via USB Serial"
                                        EspConnectionState.SCANNING -> "Connecting to ESP32 node…"
                                        else -> "No ESP32 Node Connected Directly"
                                    },
                                    fontWeight = FontWeight.Bold,
                                    modifier = Modifier.weight(1f)
                                )
                                if (connectionState == EspConnectionState.DISCONNECTED) {
                                    Button(onClick = { scanForBleNodes() }, enabled = !isScanningBt) {
                                        if (isScanningBt) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                                        else Text("Scan BLE")
                                    }
                                }
                            }

                            status?.let { st ->
                                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                                    Column {
                                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                            Text("Node: ${st.node}", fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
                                            Surface(color = Cyan.copy(alpha = 0.2f), shape = RoundedCornerShape(4.dp)) {
                                                Text("v${st.version}", color = Cyan, style = MaterialTheme.typography.labelSmall, modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp))
                                            }
                                        }
                                        Text("Uptime: ${st.uptime}s", color = Slate, style = MaterialTheme.typography.bodySmall)
                                    }
                                    Column { Text("Rate: ${st.pps} pps", color = Cyan, style = MaterialTheme.typography.bodySmall); Text("Heap: ${st.heap / 1024} KB free", color = Slate, style = MaterialTheme.typography.bodySmall) }
                                    Column {
                                        if (st.batt_state == "no_battery" || st.batt_mv < 1200) {
                                            Text("Power: USB (No Batt)", color = Cyan, fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
                                            Text("State: External 5V", color = Slate, style = MaterialTheme.typography.bodySmall)
                                        } else {
                                            Text("Battery: ${st.batt_pct}% (${st.batt_mv}mV)", color = if (st.batt_pct > 20) Green else Red, fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
                                            Text("State: ${st.batt_state}${if (st.charging) " ⚡" else ""}", color = Slate, style = MaterialTheme.typography.bodySmall)
                                        }
                                    }
                                }
                            }

                            // Discovered Bluetooth Devices
                            if (discoveredDevices.isNotEmpty() && connectionState == EspConnectionState.DISCONNECTED) {
                                Text("Discovered Nearby Nodes:", style = MaterialTheme.typography.labelMedium)
                                for (dev in discoveredDevices) {
                                    Row(
                                        Modifier.fillMaxWidth().clickable { nodeManager.connectBle(dev) }.padding(vertical = 4.dp),
                                        verticalAlignment = Alignment.CenterVertically,
                                        horizontalArrangement = Arrangement.SpaceBetween
                                    ) {
                                        Column { Text(dev.name ?: "Unknown Node", fontWeight = FontWeight.SemiBold); Text(dev.address, color = Slate, style = MaterialTheme.typography.labelSmall) }
                                        Button(onClick = { nodeManager.connectBle(dev) }) { Text("Connect") }
                                    }
                                }
                            }
                        }
                    }

                    // Follow Host Device / Real-Time GPS Sharing Card
                    Card(
                        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
                        colors = CardDefaults.cardColors(
                            containerColor = if (isFollowing) Cyan.copy(alpha = 0.12f) else MaterialTheme.colorScheme.surfaceVariant
                        )
                    ) {
                        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Row(
                                Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Column(Modifier.weight(1f)) {
                                    Text("🔗 Follow / Real-Time GPS Sharing", fontWeight = FontWeight.Bold, style = MaterialTheme.typography.titleSmall)
                                    Text(
                                        if (isFollowing) "Streaming phone GPS fixes & microsecond time sync to node"
                                        else "Share live GPS coordinates and synchronize microsecond clocks",
                                        color = Slate,
                                        style = MaterialTheme.typography.bodySmall
                                    )
                                }
                                Switch(
                                    checked = isFollowing,
                                    onCheckedChange = { nodeManager.attachToThisPhone(it) },
                                    enabled = connectionState != EspConnectionState.DISCONNECTED
                                )
                            }

                            if (isFollowing || status?.following == true) {
                                Surface(
                                    color = Color(0xFF0D1B2A),
                                    shape = RoundedCornerShape(8.dp),
                                    modifier = Modifier.fillMaxWidth()
                                ) {
                                    Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                                            Text("Attached Host:", color = Slate, style = MaterialTheme.typography.bodySmall)
                                            Text(
                                                status?.attached_dev?.takeIf { it.isNotBlank() } ?: "This Phone",
                                                color = Cyan,
                                                fontWeight = FontWeight.Bold,
                                                style = MaterialTheme.typography.bodySmall
                                            )
                                        }
                                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                                            Text("GPS Geotagging:", color = Slate, style = MaterialTheme.typography.bodySmall)
                                            Text(
                                                if (status?.has_gps == true) "Active (Lat: ${String.format(java.util.Locale.US, "%.5f", status?.lat)}, Lon: ${String.format(java.util.Locale.US, "%.5f", status?.lon)})"
                                                else "Active (Streaming Fixes)",
                                                color = Green,
                                                fontWeight = FontWeight.SemiBold,
                                                style = MaterialTheme.typography.bodySmall
                                            )
                                        }
                                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                                            Text("Clock Synchronization:", color = Slate, style = MaterialTheme.typography.bodySmall)
                                            Text("Stratum 1 Master (Sub-ms)", color = Gold, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.bodySmall)
                                        }
                                    }
                                }

                                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                    OutlinedButton(
                                        onClick = { nodeManager.sendTimeSync() },
                                        modifier = Modifier.weight(1f)
                                    ) {
                                        Text("Sync Clock", fontSize = 12.sp)
                                    }
                                    OutlinedButton(
                                        onClick = {
                                            coroutineScope.launch {
                                                nodeManager.attachToThisPhone(true)
                                            }
                                        },
                                        modifier = Modifier.weight(1f)
                                    ) {
                                        Text("Push GPS Fix", fontSize = 12.sp)
                                    }
                                }
                            }
                        }
                    }

                    // Quick Controls & LED Verification
                    Card(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp)) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Text("Monitor Mode & LED Diagnostics", style = MaterialTheme.typography.labelLarge)
                            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                FilterChip(selected = status?.hop == true, onClick = { nodeManager.setChannel(0) }, label = { Text("Auto-Hop") })
                                FilterChip(selected = status?.hop == false && status?.ch == 1, onClick = { nodeManager.setChannel(1) }, label = { Text("Ch 1") })
                                FilterChip(selected = status?.hop == false && status?.ch == 6, onClick = { nodeManager.setChannel(6) }, label = { Text("Ch 6") })
                                FilterChip(selected = status?.hop == false && status?.ch == 11, onClick = { nodeManager.setChannel(11) }, label = { Text("Ch 11") })
                            }
                            Text("LED Status Pattern Test:", color = Slate, style = MaterialTheme.typography.bodySmall)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                OutlinedButton(onClick = { nodeManager.setLedPattern("heartbeat") }) { Text("Heartbeat") }
                                OutlinedButton(onClick = { nodeManager.setLedPattern("probe") }) { Text("Probe Strobe") }
                                OutlinedButton(onClick = { nodeManager.setLedPattern("tracker") }) { Text("AirTag Double") }
                                OutlinedButton(onClick = { nodeManager.testAlert() }) { Text("Deauth Alert") }
                                OutlinedButton(onClick = { nodeManager.setLedPattern("usb") }) { Text("USB Dual-Tick") }
                                OutlinedButton(onClick = { nodeManager.setLedPattern("batt_charge") }) { Text("Batt Pulse") }
                                OutlinedButton(onClick = { nodeManager.setLedPattern("batt_low") }) { Text("Batt Dying") }
                            }
                        }
                    }

                    // Hardware & Wiring Guide Expandable Card
                    var isWiringExpanded by remember { mutableStateOf(false) }
                    Card(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp)) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Row(
                                Modifier.fillMaxWidth().clickable { isWiringExpanded = !isWiringExpanded },
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Column(Modifier.weight(1f)) {
                                    Text("📐 Hardware Wiring & Pinout Safety", style = MaterialTheme.typography.labelLarge)
                                    Text("Where to wire and WHERE NOT to wire vape batteries & LEDs", color = Slate, style = MaterialTheme.typography.bodySmall)
                                }
                                Text(if (isWiringExpanded) "▲ Hide" else "▼ View", color = Cyan, fontWeight = FontWeight.Bold, fontSize = 12.sp)
                            }

                            if (isWiringExpanded) {
                                Spacer(Modifier.height(4.dp))
                                Text("Pinout Rules:", fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
                                Text("• Safe LED Outputs: GPIO 2, 4, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 (always use 220Ω-330Ω resistor!)", color = Green, style = MaterialTheme.typography.bodySmall)
                                Text("• Input-Only (ADC): GPIO 34, 35, 36, 39 (Battery ADC pin: GPIO 35. CANNOT drive LEDs!)", color = Gold, style = MaterialTheme.typography.bodySmall)
                                Text("• DANGER / Strapping Pins: GPIO 0, 1, 3, 6-11, 12, 15 (Never wire LEDs here! Pulling GPIO 0 low stops boot; GPIO 6-11 crashes flash memory!)", color = Red, style = MaterialTheme.typography.bodySmall)
                                Spacer(Modifier.height(6.dp))
                                Text("Battery Divider (3.7V Vape Cell):", fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
                                Surface(color = Color(0xFF090D16), shape = RoundedCornerShape(6.dp), modifier = Modifier.fillMaxWidth()) {
                                    Text(
                                        "Vape (+) ──> TP4056 B+  ──> OUT+ ──> ESP32 VIN (5V)\n" +
                                        "Vape (-) ──> TP4056 B-  ──> OUT- ──> ESP32 GND\n" +
                                        "Precision Divider: Vape(+) ─[100kΩ]─┬─> GPIO 35\n" +
                                        "                                    │\n" +
                                        "                                 [100kΩ]─> GND",
                                        fontFamily = FontFamily.Monospace,
                                        fontSize = 10.sp,
                                        color = Cyan,
                                        modifier = Modifier.padding(8.dp)
                                    )
                                }
                                Spacer(Modifier.height(6.dp))
                                Text("External LED Options:", fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
                                Text("• Satellite Single LED: GPIO 4 ─[330Ω]─> Anode (+), Cathode (-) ─> GND (augments internal LED for waterproof case)", color = Slate, style = MaterialTheme.typography.bodySmall)
                                Text("• 4-Pin RGB LED: R─[330Ω]─GPIO18, G─[220Ω]─GPIO19, B─[220Ω]─GPIO23, Common─>GND (supports dedicated full-time battery meter)", color = Slate, style = MaterialTheme.typography.bodySmall)
                            }
                        }
                    }

                    // Battery Diagnostics & Troubleshooter Card
                    var isBattDiagExpanded by remember { mutableStateOf(false) }
                    Card(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp)) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Row(
                                Modifier.fillMaxWidth().clickable { isBattDiagExpanded = !isBattDiagExpanded },
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Column(Modifier.weight(1f)) {
                                    Text("🔍 Battery Troubleshooter (\"Can't see battery?\")", style = MaterialTheme.typography.labelLarge)
                                    Text(
                                        if (status?.batt_state == "no_battery" || (status?.batt_mv ?: 0) < 1200) "Status: No battery detected (USB 5V)"
                                        else "Status: 3.7V Battery Active (${status?.batt_pct}%)",
                                        color = if (status?.batt_state == "no_battery" || (status?.batt_mv ?: 0) < 1200) Cyan else Green,
                                        style = MaterialTheme.typography.bodySmall
                                    )
                                }
                                Text(if (isBattDiagExpanded) "▲ Hide" else "▼ Troubleshoot", color = Cyan, fontWeight = FontWeight.Bold, fontSize = 12.sp)
                            }

                            if (isBattDiagExpanded) {
                                Spacer(Modifier.height(4.dp))
                                Text("Diagnosis Checklist:", fontWeight = FontWeight.Bold, style = MaterialTheme.typography.bodySmall)
                                Text("1. Common Ground (GND): Is the ESP32 GND connected to TP4056 GND? If grounds are isolated, the ADC cannot read voltage and floats near 0V.", color = Slate, style = MaterialTheme.typography.bodySmall)
                                Text("2. Divider Resistors: Verify 100kΩ between Battery (+) and GPIO 35, plus 100kΩ between GPIO 35 and GND.", color = Slate, style = MaterialTheme.typography.bodySmall)
                                Text("3. Recovered Vape Battery Protection: Disposable vape cells shut down when drained (<2.5V). Plug the TP4056 into USB-C for 15 minutes to awaken the cell.", color = Gold, style = MaterialTheme.typography.bodySmall)
                                Text("4. Running on USB Only: If you are intentionally powering the node via USB 5V with no battery attached, this is completely normal! The ESP enters USB mode and plays a calm dual-tick pattern.", color = Cyan, style = MaterialTheme.typography.bodySmall)
                            }
                        }
                    }

                    // Network Flashing & Web OTA Card
                    Card(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp)) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Row(
                                Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Column(Modifier.weight(1f)) {
                                    Text("Network Firmware Flasher", style = MaterialTheme.typography.labelLarge)
                                    Text("Flash connected ESP32 on host or via Web OTA", color = Slate, style = MaterialTheme.typography.bodySmall)
                                }
                                Button(
                                    onClick = {
                                        coroutineScope.launch {
                                            isFlashingHost = true
                                            hostFlashResult = "Triggering network flash on host..."
                                            val ok = nodeManager.triggerHostFlash()
                                            hostFlashResult = if (ok) "⚡ Flashed successfully over network!" else "❌ Host flash failed or unavailable."
                                            isFlashingHost = false
                                        }
                                    },
                                    enabled = !isFlashingHost
                                ) {
                                    if (isFlashingHost) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                                    else Text("Flash via Host")
                                }
                            }
                            if (hostFlashResult.isNotEmpty()) {
                                Text(hostFlashResult, color = Cyan, fontFamily = FontFamily.Monospace, fontSize = 11.sp)
                            }
                        }
                    }
                }
            }
            2 -> {
                if (probes.isEmpty()) EmptyState("📡", "No probes heard yet", "When devices or vehicles probe for Wi-Fi networks in promiscuous monitor mode, their MACs and probed SSIDs appear here.")
                else LazyColumn(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
                    items(probes.reversed()) { p ->
                        Card(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
                            Row(Modifier.padding(10.dp), verticalAlignment = Alignment.CenterVertically) {
                                Column(Modifier.weight(1f)) {
                                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                        Text(p.mac, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold, fontSize = 13.sp)
                                        Chip("Ch ${p.ch}", Slate)
                                        Chip("${p.rssi} dBm", if (p.rssi > -65) Green else if (p.rssi > -80) Gold else Slate)
                                    }
                                    if (p.ssid.isNotEmpty()) Text("Probing: \"${p.ssid}\"", color = Cyan, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.bodySmall)
                                    else Text("Wildcard probe", color = Slate, style = MaterialTheme.typography.bodySmall)
                                }
                            }
                        }
                    }
                }
            }
            3 -> {
                if (alerts.isEmpty()) EmptyState("🛡", "No security alerts", "Deauthentication frames and disassociation storms will be detected and alerted here.")
                else LazyColumn(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
                    items(alerts.reversed()) { a ->
                        Card(Modifier.fillMaxWidth().padding(vertical = 4.dp), colors = CardDefaults.cardColors(containerColor = Red.copy(alpha = 0.12f))) {
                            Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                    Text("⚠ ${a.event.uppercase()}", fontWeight = FontWeight.Bold, color = Red)
                                    Chip("Ch ${a.ch}", Slate)
                                    Chip("${a.rssi} dBm", Red)
                                }
                                Text("Source: ${a.sa} ➔ Target: ${a.da}", fontFamily = FontFamily.Monospace, fontSize = 12.sp)
                                Text("Reason code: ${a.reason}", color = Slate, style = MaterialTheme.typography.bodySmall)
                            }
                        }
                    }
                }
            }
            4 -> {
                if (trackers.isEmpty()) EmptyState("🏷", "No BLE trackers detected", "Apple AirTags, FindMy beacons, Tile, and SmartTags detected in the 2.4GHz spectrum appear here.")
                else LazyColumn(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
                    items(trackers.reversed()) { t ->
                        Card(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
                            Column(Modifier.padding(10.dp)) {
                                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                    Text("🏷 ${t.kind.uppercase()}", fontWeight = FontWeight.Bold, color = Gold)
                                    Chip("${t.rssi} dBm", Green)
                                }
                                Text("MAC: ${t.mac}", fontFamily = FontFamily.Monospace, fontSize = 12.sp)
                                if (t.payload.isNotEmpty()) Text("Payload: ${t.payload}", color = Slate, fontFamily = FontFamily.Monospace, fontSize = 11.sp)
                            }
                        }
                    }
                }
            }
        }
    }

    // Battery Capacity Selection Dialog
    editingBatteryNode?.let { node ->
        var selectedCap by remember { mutableIntStateOf(node.battMah) }
        val options = listOf(
            240 to "240 mAh (Tiny / Disposable Vape)",
            500 to "500 mAh (Standard Compact)",
            1000 to "1,000 mAh (18650 Small / Pouch)",
            3000 to "3,000 mAh (18650 High Capacity)",
            5000 to "5,000 mAh (21700 Cell)",
            0 to "USB 5V Only (No Battery)"
        )
        AlertDialog(
            onDismissRequest = { editingBatteryNode = null },
            title = { Text("Set Battery Capacity: ${node.name}") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("Select physical cell capacity for accurate battery percentage & runtime estimation:", style = MaterialTheme.typography.bodySmall, color = Slate)
                    Spacer(Modifier.height(4.dp))
                    for ((mah, label) in options) {
                        Row(
                            Modifier.fillMaxWidth().clickable { selectedCap = mah }.padding(vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            RadioButton(
                                selected = (selectedCap == mah),
                                onClick = { selectedCap = mah }
                            )
                            Spacer(Modifier.size(8.dp))
                            Text(label, style = MaterialTheme.typography.bodyMedium)
                        }
                    }
                }
            },
            confirmButton = {
                Button(onClick = {
                    nodeManager.setNodeBatteryCapacity(node.name, selectedCap)
                    editingBatteryNode = null
                }) {
                    Text("Save")
                }
            },
            dismissButton = {
                TextButton(onClick = { editingBatteryNode = null }) {
                    Text("Cancel")
                }
            }
        )
    }
}
