package org.sworrl.beaconfix.node

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.Context
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.util.UUID
import org.sworrl.beaconfix.collector.LocationSource
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.api.TripDto
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.trip.Stops
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class EspNodeManager @Inject constructor(
    @ApplicationContext private val context: Context,
    private val locationSource: LocationSource,
    private val db: AppDatabase,
    private val cache: DesktopCache
) {
    companion object {
        val SERVICE_UUID_NUS: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
        val CHAR_UUID_RX: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
        val CHAR_UUID_TX: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
        val CLIENT_CONFIG_DESCRIPTOR: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val UDP_PORT = 47824
    }

    private val scope = CoroutineScope(Dispatchers.IO + Job())
    private val btManager by lazy { context.getSystemService(BluetoothManager::class.java) }
    private val btAdapter: BluetoothAdapter? get() = btManager?.adapter

    private val _connectionState = MutableStateFlow(EspConnectionState.DISCONNECTED)
    val connectionState: StateFlow<EspConnectionState> = _connectionState.asStateFlow()

    private val _nodeStatus = MutableStateFlow<EspNodeStatus?>(null)
    val nodeStatus: StateFlow<EspNodeStatus?> = _nodeStatus.asStateFlow()

    private val _batteryTraining = MutableStateFlow<EspBatteryTraining?>(null)
    val batteryTraining: StateFlow<EspBatteryTraining?> = _batteryTraining.asStateFlow()

    private val _isFollowingPhone = MutableStateFlow(false)
    val isFollowingPhone: StateFlow<Boolean> = _isFollowingPhone.asStateFlow()

    private var gpsStreamJob: Job? = null
    private var timeSyncJob: Job? = null

    private val _recentProbes = MutableStateFlow<List<EspProbeEvent>>(emptyList())
    val recentProbes: StateFlow<List<EspProbeEvent>> = _recentProbes.asStateFlow()

    private val _recentAlerts = MutableStateFlow<List<EspAlertEvent>>(emptyList())
    val recentAlerts: StateFlow<List<EspAlertEvent>> = _recentAlerts.asStateFlow()

    private val _recentTrackers = MutableStateFlow<List<EspBleTrackerEvent>>(emptyList())
    val recentTrackers: StateFlow<List<EspBleTrackerEvent>> = _recentTrackers.asStateFlow()

    private val _meshPeers = MutableStateFlow<Map<String, MeshPeerNode>>(emptyMap())
    val meshPeers: StateFlow<List<MeshPeerNode>> = MutableStateFlow<List<MeshPeerNode>>(emptyList()).also { out ->
        scope.launch {
            _meshPeers.collect { map ->
                out.value = map.values.sortedWith(compareBy({ it.hops }, { it.name }))
            }
        }
    }.asStateFlow()

    private var activeGatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var udpSocket: DatagramSocket? = null
    private var udpJob: Job? = null

    // BLE NUS framing: current firmware newline-terminates lines and splits them at the MTU; older firmware sends one
    // notify per line, cut at MTU-3 (which truncated every status heartbeat)
    private val bleRxBuf = StringBuilder()
    private var bleFramed = false
    private var bleLinkNode = ""

    private var autoConnectEnabled = true
    private var autoScanJob: Job? = null

    init {
        startUdpListener()
        startAutoConnect()
    }

    // ── UDP Telemetry Listener ───────────────────────────────────────────────
    fun startUdpListener() {
        if (udpJob?.isActive == true) return
        udpJob = scope.launch {
            try {
                val socket = DatagramSocket(UDP_PORT)
                udpSocket = socket
                socket.broadcast = true   // relayBleLine sends from this socket
                val buf = ByteArray(1024)
                while (true) {
                    val packet = DatagramPacket(buf, buf.size)
                    socket.receive(packet)
                    val str = String(packet.data, 0, packet.length, Charsets.UTF_8).trim()
                    // Our own relay of a BLE line (relayBleLine) comes back to this socket; it was parsed already
                    if (str.startsWith("{") && !str.contains("\"relay\":\"phone\"")) {
                        if (_connectionState.value == EspConnectionState.DISCONNECTED) {
                            _connectionState.value = EspConnectionState.CONNECTED_UDP
                        }
                        parseTelemetryLine(str)
                    }
                }
            } catch (e: Exception) {
                // Socket closed or error
            }
        }
    }

    fun isBeaconNodeName(name: String?): Boolean {
        if (name.isNullOrBlank()) return false
        val n = name.lowercase()
        if (n.contains("beaconfix") || n.contains("node") || n.contains("esp32") ||
            n.contains("cosmic") || n.contains("falcon") || n.contains("obsidian") || n.contains("cheetah") ||
            n.contains("heltec")) {
            return true
        }
        // Match Reddit 3-part names with 4 numbers at the end (e.g. AmberOtterPilot4821)
        return name.matches(Regex("^[A-Z][a-z]+[A-Z][a-z]+[A-Z][a-z]+\\d{4}$"))
    }

    fun isAutoConnectEnabled(): Boolean = autoConnectEnabled

    fun setAutoConnectEnabled(enabled: Boolean) {
        autoConnectEnabled = enabled
        if (enabled) {
            startAutoConnect()
        } else {
            autoScanJob?.cancel()
            autoScanJob = null
        }
    }

    fun startAutoConnect() {
        if (autoScanJob?.isActive == true) return
        autoScanJob = scope.launch {
            while (autoConnectEnabled) {
                try {
                    if (_connectionState.value == EspConnectionState.DISCONNECTED && hasBlePermissions()) {
                        val adapter = btAdapter
                        if (adapter != null && adapter.isEnabled) {
                            val scanner = adapter.bluetoothLeScanner
                            if (scanner != null) {
                                runAutoScanCycle(scanner)
                            }
                        }
                    }
                } catch (_: Exception) {}
                kotlinx.coroutines.delay(5000L)
            }
        }
    }

    fun stopAutoConnect() {
        autoScanJob?.cancel()
        autoScanJob = null
    }

    @SuppressLint("MissingPermission")
    private suspend fun runAutoScanCycle(scanner: android.bluetooth.le.BluetoothLeScanner) {
        val foundDevice = kotlinx.coroutines.CompletableDeferred<BluetoothDevice?>()
        val callback = object : android.bluetooth.le.ScanCallback() {
            override fun onScanResult(callbackType: Int, result: android.bluetooth.le.ScanResult?) {
                val dev = result?.device ?: return
                val name = dev.name ?: ""
                val record = result.scanRecord
                val hasNusUuid = record?.serviceUuids?.any { it.uuid == SERVICE_UUID_NUS } == true
                if (hasNusUuid || isBeaconNodeName(name)) {
                    if (!foundDevice.isCompleted) {
                        foundDevice.complete(dev)
                    }
                }
            }

            override fun onScanFailed(errorCode: Int) {
                if (!foundDevice.isCompleted) {
                    foundDevice.complete(null)
                }
            }
        }

        try {
            val settings = android.bluetooth.le.ScanSettings.Builder()
                .setScanMode(android.bluetooth.le.ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build()
            scanner.startScan(null, settings, callback)
            val dev = kotlinx.coroutines.withTimeoutOrNull(4000L) {
                foundDevice.await()
            }
            runCatching { scanner.stopScan(callback) }

            if (dev != null && _connectionState.value == EspConnectionState.DISCONNECTED && autoConnectEnabled) {
                connectBle(dev, autoAttach = true)
            }
        } catch (_: Exception) {
            runCatching { scanner.stopScan(callback) }
        }
    }

    private fun hasBlePermissions(): Boolean {
        return if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S) {
            androidx.core.content.ContextCompat.checkSelfPermission(
                context, android.Manifest.permission.BLUETOOTH_SCAN
            ) == android.content.pm.PackageManager.PERMISSION_GRANTED &&
            androidx.core.content.ContextCompat.checkSelfPermission(
                context, android.Manifest.permission.BLUETOOTH_CONNECT
            ) == android.content.pm.PackageManager.PERMISSION_GRANTED
        } else {
            androidx.core.content.ContextCompat.checkSelfPermission(
                context, android.Manifest.permission.BLUETOOTH
            ) == android.content.pm.PackageManager.PERMISSION_GRANTED
        }
    }

    // ── BLE Connection ───────────────────────────────────────────────────────
    @SuppressLint("MissingPermission")
    fun connectBle(device: BluetoothDevice, autoAttach: Boolean = true) {
        disconnect()
        _connectionState.value = EspConnectionState.SCANNING

        activeGatt = device.connectGatt(context, false, object : BluetoothGattCallback() {
            override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
                if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                    _connectionState.value = EspConnectionState.CONNECTED_BLE
                    synchronized(bleRxBuf) { bleRxBuf.setLength(0); bleFramed = false }
                    bleLinkNode = device.name ?: ""
                    gatt.discoverServices()
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    _connectionState.value = EspConnectionState.DISCONNECTED
                    _isFollowingPhone.value = false
                    gpsStreamJob?.cancel()
                    timeSyncJob?.cancel()
                    rxChar = null
                    activeGatt?.close()
                    activeGatt = null
                }
            }

            override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
                if (status != BluetoothGatt.GATT_SUCCESS) return
                val service = gatt.getService(SERVICE_UUID_NUS) ?: return
                rxChar = service.getCharacteristic(CHAR_UUID_RX)
                val tx = service.getCharacteristic(CHAR_UUID_TX)
                if (tx != null) {
                    gatt.setCharacteristicNotification(tx, true)
                    val desc = tx.getDescriptor(CLIENT_CONFIG_DESCRIPTOR)
                    if (desc != null) {
                        desc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        gatt.writeDescriptor(desc)
                    }
                }
                // Request initial status
                sendCommand("status")
                if (autoAttach) {
                    // Auto-attach this phone to the node: streams phone GPS fixes & microsecond time sync
                    attachToThisPhone(true)
                }
            }

            @Deprecated("Deprecated in Java")
            override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
                val bytes = characteristic.value ?: return
                feedBle(String(bytes, Charsets.UTF_8))
            }
        })
    }

    private fun feedBle(chunk: String) {
        val lines = mutableListOf<String>()
        synchronized(bleRxBuf) {
            if (chunk.endsWith("\n")) bleFramed = true
            if (!bleFramed && chunk.startsWith("{")) bleRxBuf.setLength(0)
            bleRxBuf.append(chunk)
            var nl = bleRxBuf.indexOf("\n")
            while (nl >= 0) {
                lines += bleRxBuf.substring(0, nl)
                bleRxBuf.delete(0, nl + 1)
                nl = bleRxBuf.indexOf("\n")
            }
            if (!bleFramed && bleRxBuf.isNotEmpty()) {
                val rest = bleRxBuf.toString()
                if (runCatching { JSONObject(rest) }.isSuccess) { lines += rest; bleRxBuf.setLength(0) }
            }
            if (bleRxBuf.length > 8192) bleRxBuf.setLength(0)
        }
        for (raw in lines) {
            val line = raw.trim()
            if (!line.startsWith("{")) continue
            parseTelemetryLine(line)
            relayBleLine(line)
        }
    }

    // Put what the BLE-linked node says on the LAN (UDP 47824), the way the desktop's USB/BLE bridge does, so the
    // desktop sees this node — and every node it gateways for over the mesh — while the phone holds the link
    private fun relayBleLine(line: String) {
        val sock = udpSocket ?: return
        scope.launch {
            try {
                val o = JSONObject(line)
                when (o.optString("type")) {
                    "status" -> {
                        bleLinkNode = o.optString("node", bleLinkNode)
                        o.put("is_usb", false)
                        o.put("transport", "ble")
                    }
                    "mesh_telemetry" -> {
                        o.put("via_link", "ble")
                        o.put("gateway", bleLinkNode)
                    }
                }
                o.put("relay", "phone")
                val data = (o.toString() + "\n").toByteArray(Charsets.UTF_8)
                sock.send(DatagramPacket(data, data.size, java.net.InetAddress.getByName("255.255.255.255"), UDP_PORT))
            } catch (_: Exception) {}
        }
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        activeGatt?.disconnect()
        activeGatt?.close()
        activeGatt = null
        rxChar = null
        if (_connectionState.value == EspConnectionState.CONNECTED_BLE) {
            _connectionState.value = EspConnectionState.DISCONNECTED
        }
    }

    @SuppressLint("MissingPermission")
    fun sendCommand(cmd: String) {
        val bytes = (cmd.trim() + "\n").toByteArray(Charsets.UTF_8)
        val char = rxChar
        val gatt = activeGatt
        if (char != null && gatt != null) {
            char.value = bytes
            gatt.writeCharacteristic(char)
        } else {
            scope.launch {
                try {
                    val bcast = java.net.InetAddress.getByName("255.255.255.255")
                    val p1 = DatagramPacket(bytes, bytes.size, bcast, UDP_PORT)
                    udpSocket?.send(p1)
                    val p2 = DatagramPacket(bytes, bytes.size, bcast, 47825)
                    udpSocket?.send(p2)
                } catch (_: Exception) {}
            }
        }
    }

    private suspend fun getAccumulatedTripKm(): Float {
        return try {
            val tripSnap = cache.decode<TripDto>(cache.snapshotNow("trip"))?.trip
            val snapKm = tripSnap?.distanceTripKm ?: tripSnap?.distanceTodayKm ?: 0.0
            if (snapKm > 0.01) {
                return snapKm.toFloat()
            }
            val now = System.currentTimeMillis()
            val zone = java.time.ZoneId.systemDefault()
            val phoneFixes = db.fixes().phoneSince(now - 24 * 3600_000L)
            val day = Stops.phoneDay(phoneFixes, now, zone)
            (day.distanceM / 1000.0).toFloat()
        } catch (_: Exception) {
            0.0f
        }
    }

    fun attachToThisPhone(enable: Boolean) {
        _isFollowingPhone.value = enable
        gpsStreamJob?.cancel()
        timeSyncJob?.cancel()

        if (!enable) {
            sendCommand("detach")
            return
        }

        val phoneName = try {
            btAdapter?.name?.takeIf { it.isNotBlank() } ?: android.os.Build.MODEL
        } catch (_: Exception) {
            android.os.Build.MODEL
        }

        sendCommand("attach $phoneName")

        // 1. Live GPS Stream (push GPS fixes every 2.5s / 1.5m movement)
        gpsStreamJob = scope.launch {
            try {
                // Send immediate current/last location fix upon attach!
                val initialLoc = locationSource.current(2000L)
                val initialTripKm = getAccumulatedTripKm()
                if (initialLoc != null) {
                    val lat = initialLoc.latitude
                    val lon = initialLoc.longitude
                    val acc = if (initialLoc.hasAccuracy()) initialLoc.accuracy else 5.0f
                    val spd = if (initialLoc.hasSpeed()) initialLoc.speed else -1.0f
                    val hdg = if (initialLoc.hasBearing()) initialLoc.bearing else -1.0f
                    val alt = if (initialLoc.hasAltitude()) initialLoc.altitude.toFloat() else 0.0f
                    val cmd = String.format(java.util.Locale.US, "gps %.6f,%.6f,%.1f,%.2f,%.1f,%.1f,0,%.2f", lat, lon, acc, spd, hdg, alt, initialTripKm)
                    sendCommand(cmd)
                    if (initialTripKm > 0.005f) {
                        sendCommand(String.format(java.util.Locale.US, "trip %.2f", initialTripKm))
                    }
                }

                locationSource.updates(2500L, 1.5f).collect { loc ->
                    val lat = loc.latitude
                    val lon = loc.longitude
                    val acc = if (loc.hasAccuracy()) loc.accuracy else 5.0f
                    val spd = if (loc.hasSpeed()) loc.speed else -1.0f
                    val hdg = if (loc.hasBearing()) loc.bearing else -1.0f
                    val alt = if (loc.hasAltitude()) loc.altitude.toFloat() else 0.0f
                    val tripKm = getAccumulatedTripKm()
                    val cmd = String.format(java.util.Locale.US, "gps %.6f,%.6f,%.1f,%.2f,%.1f,%.1f,0,%.2f", lat, lon, acc, spd, hdg, alt, tripKm)
                    sendCommand(cmd)
                    if (tripKm > 0.005f) {
                        sendCommand(String.format(java.util.Locale.US, "trip %.2f", tripKm))
                    }
                }
            } catch (_: Exception) {}
        }

        // 2. Microsecond Stratum 1 Time Sync & Periodic GPS/Trip Keepalive (every 15s)
        timeSyncJob = scope.launch {
            try {
                while (_isFollowingPhone.value) {
                    val epochUs = System.currentTimeMillis() * 1000L
                    sendCommand("time $epochUs")
                    val loc = locationSource.current(1000L)
                    val tripKm = getAccumulatedTripKm()
                    if (loc != null) {
                        val lat = loc.latitude
                        val lon = loc.longitude
                        val acc = if (loc.hasAccuracy()) loc.accuracy else 5.0f
                        val spd = if (loc.hasSpeed()) loc.speed else 0.0f
                        val hdg = if (loc.hasBearing()) loc.bearing else -1.0f
                        val alt = if (loc.hasAltitude()) loc.altitude.toFloat() else 0.0f
                        val cmd = String.format(java.util.Locale.US, "gps %.6f,%.6f,%.1f,%.2f,%.1f,%.1f,0,%.2f", lat, lon, acc, spd, hdg, alt, tripKm)
                        sendCommand(cmd)
                    }
                    if (tripKm > 0.005f) {
                        sendCommand(String.format(java.util.Locale.US, "trip %.2f", tripKm))
                    }
                    kotlinx.coroutines.delay(15000L)
                }
            } catch (_: Exception) {}
        }
    }

    fun sendGpsFix(lat: Double, lon: Double, accM: Float = 5.0f, speedMps: Float = -1.0f, headingDeg: Float = -1.0f, altM: Float = 0.0f) {
        val cmd = String.format(java.util.Locale.US, "gps %.6f,%.6f,%.1f,%.2f,%.1f,%.1f", lat, lon, accM, speedMps, headingDeg, altM)
        sendCommand(cmd)
    }

    /**
     * Send ALPR alert to connected ESP32 / Heltec node to display full OLED splash screen & alarm LED.
     */
    fun sendAlprAlert(
        operator: String,
        model: String,
        distanceM: Float,
        confidence: Int = 95,
        facing: Int = 1,
        speedKmh: Float = -1.0f,
        lat: Double = 0.0,
        lon: Double = 0.0,
        durationMs: Long = 9000L
    ) {
        val opClean = operator.ifBlank { "Flock Safety" }.replace(" ", "_").take(23)
        val mdlClean = model.ifBlank { "Falcon" }.replace(" ", "_").take(19)
        val cmd = String.format(
            java.util.Locale.US,
            "alpr %s %s %.1f %d %d %.1f %.6f %.6f %d",
            opClean, mdlClean, distanceM, confidence, facing, speedKmh, lat, lon, durationMs
        )
        sendCommand(cmd)

        val ev = EspAlertEvent(
            event = "alpr_pass",
            sa = opClean,
            da = mdlClean,
            reason = confidence,
            rssi = -distanceM.toInt(),
            ch = facing,
            ts = System.currentTimeMillis()
        )
        _recentAlerts.value = (_recentAlerts.value + ev).takeLast(50)
    }

    /**
     * Send ALPR camera proximity notification when approaching an active camera zone.
     */
    fun sendAlprProximity(
        operator: String,
        model: String,
        distanceM: Float,
        facing: Int = 0
    ) {
        val opClean = operator.ifBlank { "Flock Safety" }.replace(" ", "_").take(23)
        val mdlClean = model.ifBlank { "Falcon" }.replace(" ", "_").take(19)
        val cmd = String.format(
            java.util.Locale.US,
            "alpr %s %s %.1f 90 %d",
            opClean, mdlClean, distanceM, facing
        )
        sendCommand(cmd)
    }

    fun sendTimeSync() {
        val epochUs = System.currentTimeMillis() * 1000L
        sendCommand("time $epochUs")
    }

    fun setChannel(channel: Int) {
        sendCommand("channel $channel")
    }

    fun setHopInterval(ms: Int) {
        sendCommand("hop $ms")
    }

    fun setLedPattern(pattern: String) {
        sendCommand("led $pattern")
    }

    fun setNodeName(name: String) {
        sendCommand("name $name")
    }

    fun resetNodeName() {
        sendCommand("name reset")
    }

    fun configureLedSingle(slot: Int, pin: Int, role: String, inverted: Boolean = false) {
        sendCommand("led config $slot single $pin $role ${if (inverted) "1" else "0"}")
    }

    fun configureLedRgb(slot: Int, pinR: Int, pinG: Int, pinB: Int, role: String, commonAnode: Boolean = false) {
        sendCommand("led config $slot rgb $pinR $pinG $pinB $role ${if (commonAnode) "1" else "0"}")
    }

    fun clearLedSlot(slot: Int) {
        sendCommand("led clear $slot")
    }

    fun testAlert() {
        sendCommand("led alert")
    }

    // ── Telemetry Parser ─────────────────────────────────────────────────────
    fun parseTelemetryLine(jsonStr: String) {
        try {
            val o = JSONObject(jsonStr)
            when (o.optString("type")) {
                "status" -> {
                    val nName = o.optString("node")
                    val st = EspNodeStatus(
                        version = o.optString("version", ""),
                        node = nName,
                        uptime = o.optLong("uptime"),
                        heap = o.optLong("heap"),
                        ch = o.optInt("ch", 1),
                        hop = o.optBoolean("hop", true),
                        pps = o.optLong("pps"),
                        total = o.optLong("total"),
                        probes = o.optLong("probes"),
                        beacons = o.optLong("beacons"),
                        deauths = o.optLong("deauths"),
                        ble = o.optInt("ble", 0),
                        batt_mv = o.optInt("batt_mv", 0),
                        batt_pct = o.optInt("batt_pct", -1),
                        batt_state = o.optString("batt_state", "unknown"),
                        charging = o.optBoolean("charging", false),
                        batt_trained = o.optBoolean("batt_trained", false),
                        batt_train_pct = o.optInt("batt_train_pct", 0),
                        batt_cycles = o.optDouble("batt_cycles", 0.0).toFloat(),
                        attached_dev = o.optString("attached_dev", ""),
                        following = o.optBoolean("following", false),
                        has_gps = o.optBoolean("has_gps", false),
                        lat = o.optDouble("lat", 0.0),
                        lon = o.optDouble("lon", 0.0)
                    )
                    _nodeStatus.value = st
                    if (nName.isNotBlank()) {
                        val existing = _meshPeers.value[nName]
                        val peer = (existing ?: MeshPeerNode(name = nName)).copy(
                            name = nName,
                            role = o.optString("mode", existing?.role ?: "mobile"),
                            version = o.optString("version", existing?.version ?: ""),
                            battMv = o.optInt("batt_mv", existing?.battMv ?: 0),
                            battPct = o.optInt("batt_pct", existing?.battPct ?: -1),
                            battState = o.optString("batt_state", existing?.battState ?: "unknown"),
                            charging = o.optBoolean("charging", existing?.charging ?: false),
                            battTrained = o.optBoolean("batt_trained", existing?.battTrained ?: false),
                            battTrainPct = o.optInt("batt_train_pct", existing?.battTrainPct ?: 0),
                            battCycles = o.optDouble("batt_cycles", (existing?.battCycles ?: 0.0f).toDouble()).toFloat(),
                            hops = 0,
                            pps = o.optLong("pps", existing?.pps ?: 0),
                            lastSeen = System.currentTimeMillis(),
                            following = o.optBoolean("following", existing?.following ?: false),
                            attachedDevice = o.optString("attached_dev", existing?.attachedDevice ?: ""),
                            hasLocation = o.optBoolean("has_gps", existing?.hasLocation ?: false),
                            lat = o.optDouble("lat", existing?.lat ?: 0.0),
                            lon = o.optDouble("lon", existing?.lon ?: 0.0),
                            mac = o.optString("mac", existing?.mac ?: ""),
                            isUsb = o.optBoolean("is_usb", existing?.isUsb ?: false),
                            usbPort = o.optString("usb_port", existing?.usbPort ?: ""),
                            transport = o.optString("transport", if (o.optBoolean("is_usb", false)) "usb_mesh_dual" else (existing?.transport ?: "mesh"))
                        )
                        _meshPeers.value = _meshPeers.value + (nName to peer)
                    }
                }
                "mesh_telemetry" -> {
                    val origin = o.optString("origin")
                    if (origin.isNotBlank() && origin != "Unknown") {
                        val existing = _meshPeers.value[origin]
                        val mac = o.optString("mac", existing?.mac ?: "")
                        val hops = o.optInt("hops", 1)
                        val via = o.optString("via", existing?.via ?: "")
                        val prevMac = o.optString("prev_mac", existing?.prevMac ?: "")
                        val route = o.optString("route", existing?.route ?: "")
                        val inner = o.optJSONObject("data") ?: JSONObject()
                        val peer = (existing ?: MeshPeerNode(name = origin, mac = mac)).copy(
                            name = origin,
                            mac = mac,
                            role = inner.optString("mode", existing?.role ?: "mobile"),
                            version = inner.optString("version", existing?.version ?: ""),
                            battMv = inner.optInt("batt_mv", existing?.battMv ?: 0),
                            battPct = inner.optInt("batt_pct", existing?.battPct ?: -1),
                            battState = inner.optString("batt_state", existing?.battState ?: ""),
                            battTrained = inner.optBoolean("batt_trained", existing?.battTrained ?: false),
                            battTrainPct = inner.optInt("batt_train_pct", existing?.battTrainPct ?: 0),
                            battCycles = inner.optDouble("batt_cycles", (existing?.battCycles ?: 0.0f).toDouble()).toFloat(),
                            hops = hops,
                            via = via,
                            prevMac = prevMac,
                            route = route,
                            lastSeen = System.currentTimeMillis()
                        )
                        _meshPeers.value = _meshPeers.value + (origin to peer)
                    }
                }
                "mesh_ota_remote_status" -> {
                    val node = o.optString("node")
                    val pct = o.optInt("pct", 0)
                    val stateStr = o.optString("state", "")
                    if (node.isNotBlank() && _meshPeers.value.containsKey(node)) {
                        val cur = _meshPeers.value[node]!!
                        _meshPeers.value = _meshPeers.value + (node to cur.copy(otaPct = pct, otaState = stateStr))
                    }
                }
                "battery" -> {
                    val cur = _nodeStatus.value ?: EspNodeStatus()
                    val trained = o.optBoolean("batt_trained", cur.batt_trained)
                    val trainPct = o.optInt("batt_train_pct", cur.batt_train_pct)
                    val cycles = o.optDouble("batt_cycles", cur.batt_cycles.toDouble()).toFloat()
                    _nodeStatus.value = cur.copy(
                        batt_mv = o.optInt("mv", 0),
                        batt_pct = o.optInt("pct", -1),
                        batt_state = o.optString("state", "unknown"),
                        charging = o.optBoolean("charging", false),
                        batt_trained = trained,
                        batt_train_pct = trainPct,
                        batt_cycles = cycles
                    )
                }
                "battery_stats" -> {
                    val trn = EspBatteryTraining(
                        isTrained = o.optBoolean("trained", false),
                        trainPct = o.optInt("train_pct", 0),
                        vMin = o.optInt("v_min", 3250),
                        vMax = o.optInt("v_max", 4200),
                        vNom = o.optInt("v_nom", 3700),
                        cycles = o.optDouble("cycles", 0.0).toFloat(),
                        runtimeSec = o.optLong("runtime_sec", 0L),
                        samples = o.optLong("samples", 0L),
                        hasBattery = o.optBoolean("has_battery", true),
                        mah = o.optInt("mah", 240)
                    )
                    _batteryTraining.value = trn
                    val cur = _nodeStatus.value ?: EspNodeStatus()
                    _nodeStatus.value = cur.copy(
                        batt_mv = o.optInt("mv", o.optInt("calc_mv", cur.batt_mv)),
                        batt_pct = o.optInt("pct", cur.batt_pct),
                        batt_state = o.optString("state", cur.batt_state),
                        charging = o.optBoolean("charging", cur.charging),
                        batt_trained = trn.isTrained,
                        batt_train_pct = trn.trainPct,
                        batt_cycles = trn.cycles
                    )
                }
                "battery_status" -> {
                    val cur = _nodeStatus.value ?: EspNodeStatus()
                    val isTrained = o.optBoolean("trained", cur.batt_trained)
                    val trainPct = o.optInt("train_pct", cur.batt_train_pct)
                    val cycles = o.optDouble("cycles", cur.batt_cycles.toDouble()).toFloat()
                    _nodeStatus.value = cur.copy(
                        batt_mv = o.optInt("mv", o.optInt("calc_mv", cur.batt_mv)),
                        batt_pct = o.optInt("pct", cur.batt_pct),
                        batt_state = o.optString("state", cur.batt_state),
                        charging = o.optBoolean("charging", cur.charging),
                        batt_trained = isTrained,
                        batt_train_pct = trainPct,
                        batt_cycles = cycles
                    )
                }
                "ack" -> {
                    if (o.optString("action") == "battery_stats_reset") {
                        _batteryTraining.value = EspBatteryTraining(
                            isTrained = false,
                            trainPct = 0,
                            cycles = o.optDouble("cycles", 0.0).toFloat(),
                            vMin = o.optInt("v_min", 3250),
                            vMax = o.optInt("v_max", 4200)
                        )
                        val cur = _nodeStatus.value ?: EspNodeStatus()
                        _nodeStatus.value = cur.copy(
                            batt_trained = false,
                            batt_train_pct = 0,
                            batt_cycles = 0.0f
                        )
                    }
                }
                "probe" -> {
                    val ev = EspProbeEvent(
                        mac = o.optString("mac"),
                        ssid = o.optString("ssid"),
                        rssi = o.optInt("rssi"),
                        ch = o.optInt("ch"),
                        seq = o.optInt("seq"),
                        ts = o.optLong("ts")
                    )
                    val list = (_recentProbes.value + ev).takeLast(150)
                    _recentProbes.value = list
                }
                "alert" -> {
                    val ev = EspAlertEvent(
                        event = o.optString("event"),
                        sa = o.optString("sa"),
                        da = o.optString("da"),
                        reason = o.optInt("reason"),
                        rssi = o.optInt("rssi"),
                        ch = o.optInt("ch"),
                        ts = o.optLong("ts")
                    )
                    val list = (_recentAlerts.value + ev).takeLast(50)
                    _recentAlerts.value = list
                }
                "ble_tracker" -> {
                    val ev = EspBleTrackerEvent(
                        kind = o.optString("kind"),
                        mac = o.optString("mac"),
                        name = o.optString("name"),
                        rssi = o.optInt("rssi"),
                        payload = o.optString("payload"),
                        ts = o.optLong("ts")
                    )
                    val list = (_recentTrackers.value + ev).takeLast(50)
                    _recentTrackers.value = list
                }
            }
        } catch (_: Exception) {}
    }

    fun broadcastTimeSync() {
        val epochUs = System.currentTimeMillis() * 1000L
        sendCommand("time $epochUs")
        scope.launch {
            try {
                val sock = DatagramSocket()
                sock.broadcast = true
                val data = "time $epochUs\n".toByteArray(Charsets.UTF_8)
                sock.send(DatagramPacket(data, data.size, java.net.InetAddress.getByName("255.255.255.255"), UDP_PORT))
                sock.close()
            } catch (_: Exception) {}
        }
    }

    fun sendMeshOtaAbort() {
        sendCommand("mesh ota abort")
        scope.launch {
            try {
                val sock = DatagramSocket()
                sock.broadcast = true
                val data = "mesh ota abort\n".toByteArray(Charsets.UTF_8)
                sock.send(DatagramPacket(data, data.size, java.net.InetAddress.getByName("255.255.255.255"), UDP_PORT))
                sock.close()
            } catch (_: Exception) {}
        }
    }

    fun triggerNodePing(nodeName: String = "") {
        sendCommand("led heartbeat")
        scope.launch {
            try {
                val sock = DatagramSocket()
                sock.broadcast = true
                val data = "led heartbeat\n".toByteArray(Charsets.UTF_8)
                sock.send(DatagramPacket(data, data.size, java.net.InetAddress.getByName("255.255.255.255"), UDP_PORT))
                sock.close()
            } catch (_: Exception) {}
        }
    }

    fun setNodeBatteryCapacity(nodeName: String, mah: Int) {
        val existing = _meshPeers.value[nodeName]
        if (existing != null) {
            _meshPeers.value = _meshPeers.value + (nodeName to existing.copy(battMah = mah))
        }
        sendCommand("batt $mah")
    }

    fun resetBatteryStats() {
        sendCommand("batt reset")
        scope.launch {
            try {
                val sock = DatagramSocket()
                sock.broadcast = true
                val data = "batt reset\n".toByteArray(Charsets.UTF_8)
                sock.send(DatagramPacket(data, data.size, java.net.InetAddress.getByName("255.255.255.255"), UDP_PORT))
                sock.close()
            } catch (_: Exception) {}
        }
    }

    fun requestBatteryStats() {
        sendCommand("batt stats")
        scope.launch {
            try {
                val sock = DatagramSocket()
                sock.broadcast = true
                val data = "batt stats\n".toByteArray(Charsets.UTF_8)
                sock.send(DatagramPacket(data, data.size, java.net.InetAddress.getByName("255.255.255.255"), UDP_PORT))
                sock.close()
            } catch (_: Exception) {}
        }
    }

    /**
     * Trigger desktop API to flash connected ESP32 over local network.
     */
    suspend fun triggerHostFlash(hostUrl: String = "http://10.0.2.2:8080"): Boolean = kotlinx.coroutines.withContext(Dispatchers.IO) {
        val targets = listOf("$hostUrl/api/v1/esp/flash")
        for (u in targets) {
            try {
                val conn = java.net.URL(u).openConnection() as java.net.HttpURLConnection
                conn.requestMethod = "POST"
                conn.connectTimeout = 3000
                conn.readTimeout = 65000
                conn.doOutput = true
                conn.setRequestProperty("Content-Type", "application/json")
                conn.outputStream.write("{}".toByteArray())
                if (conn.responseCode in 200..299) {
                    return@withContext true
                }
            } catch (_: Exception) {}
        }
        false
    }

    /**
     * Correlate with ALPR sightings: retrieves probe requests spotted in the given time window
     * (e.g. within 15 seconds of a plate read) while mobile.
     */
    fun getProbesNearTime(timestampMs: Long, windowMs: Long = 15000): List<EspProbeEvent> {
        return _recentProbes.value.filter { Math.abs(it.timeReceivedMs - timestampMs) <= windowMs }
    }
}
