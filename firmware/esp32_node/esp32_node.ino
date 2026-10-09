#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include "NodeConfig.h"
#include "LedPatterns.h"
#include "WifiMonitor.h"
#include "BleScanner.h"
#include "Telemetry.h"
#include "BatteryMonitor.h"
#include "OtaUpdater.h"
#include "TimeSync.h"
#include "MeshEngine.h"
#include "MeshOtaEngine.h"
#include "DisplayOled.h"
#include "GpsReceiver.h"

static unsigned long g_lastStatusTime = 0;
static String g_serialBuffer = "";

// ── BLE Server Callbacks ──────────────────────────────────────────────────────
void BleServerCallbacksImpl::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
    BleScanner::instance().clientConnected();
}

void BleServerCallbacksImpl::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
    BleScanner::instance().clientDisconnected();
}

void BleRxCallbacksImpl::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    std::string val = pCharacteristic->getValue();
    if (!val.empty()) {
        BleScanner::instance().processIncomingRx(val);
    }
}

// ── BLE Scanner Callbacks ────────────────────────────────────────────────────
void BleAdvertisedCallbacksImpl::onResult(const NimBLEAdvertisedDevice* dev) {
    const std::string mfg = dev->getManufacturerData();
    DetectedBleTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.rssi = dev->getRSSI();
    tag.timestampMs = millis();
    strncpy(tag.mac, dev->getAddress().toString().c_str(), sizeof(tag.mac) - 1);
    if (dev->haveName()) {
        strncpy(tag.name, dev->getName().c_str(), sizeof(tag.name) - 1);
    }

    bool isTracker = false;

    // Check Apple FindMy / AirTag (Company ID 0x004F, Type 0x12)
    if (mfg.length() >= 4 && (uint8_t)mfg[0] == 0x4C && (uint8_t)mfg[1] == 0x00) {
        uint8_t type = (uint8_t)mfg[2];
        if (type == 0x12) {
            strncpy(tag.kind, "airtag", sizeof(tag.kind) - 1);
            isTracker = true;
        } else if (type == 0x02 && mfg.length() >= 23) {
            strncpy(tag.kind, "ibeacon", sizeof(tag.kind) - 1);
        } else {
            strncpy(tag.kind, "apple", sizeof(tag.kind) - 1);
        }
    } else if (mfg.length() >= 2 && (uint8_t)mfg[0] == 0x75 && (uint8_t)mfg[1] == 0x00) {
        strncpy(tag.kind, "smarttag", sizeof(tag.kind) - 1);
        isTracker = true;
    } else if (dev->haveServiceUUID() && dev->getServiceUUID().equals(NimBLEUUID((uint16_t)0xFEEC))) {
        strncpy(tag.kind, "tile", sizeof(tag.kind) - 1);
        isTracker = true;
    }

    if (isTracker) {
        // Convert up to 16 bytes of payload to hex
        int len = mfg.length();
        if (len > 16) len = 16;
        for (int i = 0; i < len; i++) {
            snprintf(tag.payloadHex + (i * 2), 3, "%02x", (uint8_t)mfg[i]);
        }
        BleScanner::instance().enqueueTag(tag);
        LedPatterns::instance().triggerDetection(true, tag.kind);
    }
}

// ── Command Helper ────────────────────────────────────────────────────────────
static LedRole parseRole(const String& str) {
    if (str.equalsIgnoreCase("battery") || str.equalsIgnoreCase("batt")) return ROLE_BATTERY_ONLY;
    if (str.equalsIgnoreCase("sniffer") || str.equalsIgnoreCase("wifi")) return ROLE_SNIFFER_ONLY;
    if (str.equalsIgnoreCase("tracker") || str.equalsIgnoreCase("ble")) return ROLE_TRACKER_ONLY;
    if (str.equalsIgnoreCase("alert") || str.equalsIgnoreCase("deauth")) return ROLE_ALERT_ONLY;
    if (str.equalsIgnoreCase("off")) return ROLE_OFF;
    return ROLE_MIRROR;
}

static bool parseHexBytes(const char* hexStr, uint8_t* outBuf, size_t maxBytes, size_t* outLen = nullptr) {
    if (!hexStr) return false;
    size_t hexLen = strlen(hexStr);
    size_t byteCount = hexLen / 2;
    if (byteCount > maxBytes) byteCount = maxBytes;
    for (size_t i = 0; i < byteCount; i++) {
        char h = hexStr[i * 2];
        char l = hexStr[i * 2 + 1];
        uint8_t hb = (h >= '0' && h <= '9') ? (h - '0') : ((h >= 'a' && h <= 'f') ? (h - 'a' + 10) : ((h >= 'A' && h <= 'F') ? (h - 'A' + 10) : 0));
        uint8_t lb = (l >= '0' && l <= '9') ? (l - '0') : ((l >= 'a' && l <= 'f') ? (l - 'a' + 10) : ((l >= 'A' && l <= 'F') ? (l - 'A' + 10) : 0));
        outBuf[i] = (hb << 4) | lb;
    }
    if (outLen) *outLen = byteCount;
    return true;
}

static bool parseMacStr(const char* str, uint8_t* macOut) {
    if (!str || strcmp(str, "all") == 0 || strcmp(str, "*") == 0 || strcmp(str, "broadcast") == 0) {
        memset(macOut, 0xFF, 6);
        return true;
    }
    unsigned int m[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
        for (int i = 0; i < 6; i++) macOut[i] = (uint8_t)m[i];
        return true;
    }
    return false;
}

// ── Command Processing ───────────────────────────────────────────────────────

void dumpMeshQueueToBle() {
    MeshEngine::instance().dumpQueueToBle();
}
void sendBleTelemetry(const char* msg) {
    if (BleScanner::instance().isConnected()) BleScanner::instance().sendTelemetry(msg);
}
// Command replies go to USB and BLE. SPRINTF output may be a fragment of one line (mesh peers), so it is sent raw.
#define SPRINTF(...) do { char __b[512]; snprintf(__b, sizeof(__b), __VA_ARGS__); Serial.print(__b); if(BleScanner::instance().isConnected()) BleScanner::instance().sendRaw(__b, strlen(__b)); Telemetry::instance().udpReply(__b); } while(0)
#define SPRINTLN(msg) do { Serial.println(msg); String __s = String(msg) + "\n"; if(BleScanner::instance().isConnected()) BleScanner::instance().sendRaw(__s.c_str(), __s.length()); Telemetry::instance().udpReply(__s.c_str()); } while(0)

void handleCommand(const String& cmdLine) {
    String cmd = cmdLine;
    cmd.trim();
    if (cmd.isEmpty()) return;

    MeshEngine::instance().noteUsbActivity();

    if (cmd.startsWith("{")) {
        // Parse incoming JSON time sync commands (e.g. from BLE, Serial, or UDP)
        int timeIdx = cmd.indexOf("\"epoch_us\":");
        if (timeIdx < 0) timeIdx = cmd.indexOf("\"time\":");
        if (timeIdx >= 0) {
            uint64_t epochUs = strtoull(cmd.substring(cmd.indexOf(':', timeIdx) + 1).c_str(), NULL, 10);
            if (epochUs > 1000000000000ULL) {
                TimeSync::instance().setMasterTimeUs(epochUs);
                SPRINTF("{\"type\":\"ack\",\"time_synced\":true,\"stratum\":1,\"epoch_us\":%llu}\n", (unsigned long long)epochUs);
                return;
            }
        }
    }

    if (cmd.equalsIgnoreCase("help")) {
        SPRINTLN("{\"type\":\"help\",\"commands\":["
                       "\"status\",\"name [new_name|reset]\",\"mode <base|mobile|toggle|status>\","
                       "\"channel <0-14>\",\"hop <ms>\",\"time <epoch_us>\","
                       "\"mesh status\",\"mesh peers\",\"mesh send <text>\","
                       "\"led list\",\"led config <slot> single <pin> <role> [inv]\","
                       "\"led config <slot> rgb <r> <g> <b> <role> [inv]\",\"led clear <slot>\","
                       "\"led <pattern>\",\"batt [stats|scan|reset|pin <n>|pin auto|divider <r>]\",\"sf [status|dump|clear|ack <seq>]\",\"ota\",\"reboot\"]}");
    } else if (cmd.equalsIgnoreCase("mode") || cmd.equalsIgnoreCase("mode status")) {
        SPRINTF("{\"type\":\"op_mode\",\"mode\":\"%s\",\"is_base_station\":%s,\"hops\":%u}\n",
            NodeConfig::instance().getOpModeStr(),
            NodeConfig::instance().isBaseStation() ? "true" : "false",
            MeshEngine::instance().getHopsToGateway());
    } else if (cmd.equalsIgnoreCase("mode base") || cmd.equalsIgnoreCase("mode base_station") || cmd.equalsIgnoreCase("mode station")) {
        NodeConfig::instance().setOpMode(OP_MODE_BASE_STATION);
        SPRINTLN("{\"type\":\"ack\",\"action\":\"mode_changed\",\"mode\":\"base_station\",\"is_base_station\":true}");
    } else if (cmd.equalsIgnoreCase("mode mobile")) {
        NodeConfig::instance().setOpMode(OP_MODE_MOBILE);
        SPRINTLN("{\"type\":\"ack\",\"action\":\"mode_changed\",\"mode\":\"mobile\",\"is_base_station\":false}");
    } else if (cmd.equalsIgnoreCase("mode toggle")) {
        NodeOpMode newMode = NodeConfig::instance().isBaseStation() ? OP_MODE_MOBILE : OP_MODE_BASE_STATION;
        NodeConfig::instance().setOpMode(newMode);
        SPRINTF("{\"type\":\"ack\",\"action\":\"mode_changed\",\"mode\":\"%s\",\"is_base_station\":%s}\n",
            NodeConfig::instance().getOpModeStr(),
            NodeConfig::instance().isBaseStation() ? "true" : "false");
    } else if (cmd.startsWith("time ")) {
        uint64_t epochUs = strtoull(cmd.substring(5).c_str(), NULL, 10);
        if (epochUs > 1000000000000ULL) {
            TimeSync::instance().setMasterTimeUs(epochUs);
            SPRINTF("{\"type\":\"ack\",\"time_synced\":true,\"stratum\":1,\"epoch_us\":%llu}\n", (unsigned long long)epochUs);
        } else {
            SPRINTLN("{\"type\":\"error\",\"message\":\"invalid_timestamp\"}");
        }
    } else if (cmd.equalsIgnoreCase("mesh status") || cmd.equalsIgnoreCase("mesh")) {
        SPRINTF("{\"type\":\"mesh_status\",\"is_gateway\":%s,\"hops\":%u,\"neighbors\":%u,\"queue\":%u,\"stratum\":%u,\"now_us\":%llu}\n",
            MeshEngine::instance().isGateway() ? "true" : "false",
            MeshEngine::instance().getHopsToGateway(),
            MeshEngine::instance().getNeighborCount(),
            MeshEngine::instance().getQueueCount(),
            TimeSync::instance().getStratum(),
            (unsigned long long)TimeSync::instance().getNowUs());
    } else if (cmd.equalsIgnoreCase("mesh peers")) {
        const MeshNeighbor* n = MeshEngine::instance().getNeighbors();
        uint8_t count = MeshEngine::instance().getNeighborCount();
        SPRINTF("{\"type\":\"mesh_peers\",\"count\":%u,\"peers\":[", count);
        for (int i = 0; i < count; i++) {
            char macStr[18];
            WifiMonitor::formatMac(n[i].mac, macStr);
            SPRINTF("{\"name\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,\"hops\":%u,\"gateway\":%s,\"stratum\":%u}%s",
                n[i].name, macStr, n[i].rssi, n[i].hopsToGateway,
                n[i].isGateway ? "true" : "false", n[i].stratum,
                (i < count - 1) ? "," : "");
        }
        SPRINTLN("]}");
    } else if (cmd.startsWith("mesh send ")) {
        String msg = cmd.substring(10);
        MeshEngine::instance().sendTelemetry(msg.c_str());
        SPRINTF("{\"type\":\"ack\",\"mesh_sent\":true,\"text\":\"%s\"}\n", msg.c_str());
    } else if (cmd.equalsIgnoreCase("name")) {
        SPRINTF("{\"type\":\"node_name\",\"name\":\"%s\"}\n", NodeConfig::instance().getName());
    } else if (cmd.equalsIgnoreCase("name reset")) {
        NodeConfig::instance().resetToDefaultName();
        SPRINTF("{\"type\":\"ack\",\"name\":\"%s\",\"action\":\"name_reset\"}\n", NodeConfig::instance().getName());
    } else if (cmd.startsWith("name ")) {
        String newName = cmd.substring(5);
        newName.trim();
        if (NodeConfig::instance().setName(newName.c_str())) {
            SPRINTF("{\"type\":\"ack\",\"name\":\"%s\",\"action\":\"name_updated\"}\n", NodeConfig::instance().getName());
        } else {
            SPRINTLN("{\"type\":\"error\",\"message\":\"invalid_name\"}");
        }
    } else if (cmd.equalsIgnoreCase("status")) {
        Telemetry::instance().emitStatus(
            NodeConfig::instance().getName(),
            millis() / 1000,
            ESP.getFreeHeap(),
            WifiMonitor::instance().getChannel(),
            WifiMonitor::instance().isAutoHop(),
            WifiMonitor::instance().getPps(),
            WifiMonitor::instance().getTotalFrames(),
            WifiMonitor::instance().getProbeCount(),
            WifiMonitor::instance().getBeaconCount(),
            WifiMonitor::instance().getDeauthCount(),
            BleScanner::instance().isConnected() ? 1 : 0,
            BatteryMonitor::instance().getMilliVolts(),
            BatteryMonitor::instance().getPercentage(),
            BatteryMonitor::instance().getStateStr(),
            BatteryMonitor::instance().isCharging(),
            BatteryMonitor::instance().isTrained(),
            BatteryMonitor::instance().getTrainingPct(),
            BatteryMonitor::instance().getBatteryCycles()
        );
    } else if (cmd.equalsIgnoreCase("batt reset") || cmd.equalsIgnoreCase("battery reset") || cmd.equalsIgnoreCase("batt stats reset") || cmd.equalsIgnoreCase("batt train reset")) {
        BatteryMonitor::instance().resetBatteryStats();
        const auto& s = BatteryMonitor::instance().getTrainingStats();
        SPRINTF("{\"type\":\"ack\",\"action\":\"battery_stats_reset\",\"trained\":false,\"v_min\":%u,\"v_max\":%u,\"cycles\":%.1f,\"runtime_sec\":0}\n",
                      s.vMin, s.vMax, s.cycles);
    } else if (cmd.equalsIgnoreCase("batt diag") || cmd.equalsIgnoreCase("battery diag") || cmd.equalsIgnoreCase("batt stats") || cmd.equalsIgnoreCase("battery stats") || cmd.equalsIgnoreCase("batt info")) {
        const auto& s = BatteryMonitor::instance().getTrainingStats();
        SPRINTF("{\"type\":\"battery_stats\",\"pin\":%d,\"raw_mv\":%lu,\"calc_mv\":%lu,\"pct\":%u,\"has_battery\":%s,\"state\":\"%s\",\"charging\":%s,\"trained\":%s,\"train_pct\":%u,\"v_min\":%u,\"v_max\":%u,\"v_nom\":%u,\"cycles\":%.2f,\"runtime_sec\":%lu,\"samples\":%lu,\"verdict\":\"%s\",\"advice\":\"%s\"}\n",
                      BatteryMonitor::instance().getPin(),
                      (unsigned long)BatteryMonitor::instance().getRawMilliVolts(),
                      (unsigned long)BatteryMonitor::instance().getMilliVolts(),
                      BatteryMonitor::instance().getPercentage(),
                      BatteryMonitor::instance().hasBattery() ? "true" : "false",
                      BatteryMonitor::instance().getStateStr(),
                      BatteryMonitor::instance().isCharging() ? "true" : "false",
                      s.isTrained ? "true" : "false",
                      s.trainPct,
                      s.vMin,
                      s.vMax,
                      s.vNom,
                      s.cycles,
                      (unsigned long)s.runtimeSec,
                      (unsigned long)s.sampleCount,
                      BatteryMonitor::instance().getDiagnosticVerdict(),
                      BatteryMonitor::instance().getDiagnosticAdvice());
    } else if (cmd.equalsIgnoreCase("batt") || cmd.equalsIgnoreCase("battery")) {
        const auto& s = BatteryMonitor::instance().getTrainingStats();
        Telemetry::instance().emitBattery(
            BatteryMonitor::instance().getMilliVolts(),
            BatteryMonitor::instance().getPercentage(),
            BatteryMonitor::instance().getStateStr(),
            BatteryMonitor::instance().isCharging()
        );
        SPRINTF("{\"type\":\"battery_status\",\"pin\":%d,\"calc_mv\":%lu,\"pct\":%u,\"has_battery\":%s,\"state\":\"%s\",\"charging\":%s,\"trained\":%s,\"train_pct\":%u,\"cycles\":%.1f}\n",
                      BatteryMonitor::instance().getPin(),
                      (unsigned long)BatteryMonitor::instance().getMilliVolts(),
                      BatteryMonitor::instance().getPercentage(),
                      BatteryMonitor::instance().hasBattery() ? "true" : "false",
                      BatteryMonitor::instance().getStateStr(),
                      BatteryMonitor::instance().isCharging() ? "true" : "false",
                      s.isTrained ? "true" : "false",
                      s.trainPct,
                      s.cycles);
    } else if (cmd.equalsIgnoreCase("batt scan")) {
        // What each input-only ADC pin reads, through the current divider ratio
        const float div = BatteryMonitor::instance().getDivider();
        String json = "{\"type\":\"battery_scan\",\"divider\":" + String(div, 2) + ",\"current_pin\":" +
                      String(BatteryMonitor::instance().getPin()) + ",\"fixed\":" +
                      (BatteryMonitor::instance().isPinFixed() ? "true" : "false") + ",\"pins\":{";
        bool first = true;
        for (int pin : BatteryMonitor::kScanPins) {
            json += String(first ? "" : ",") + "\"" + String(pin) + "\":" + String((uint32_t)(BatteryMonitor::instance().readPinMv(pin) * div));
            first = false;
        }
        pinMode(BatteryMonitor::instance().getPin(), INPUT);
        json += "}}";
        SPRINTLN(json);
    } else if (cmd.equalsIgnoreCase("batt pin auto")) {
        BatteryMonitor::instance().setPinAuto();
        SPRINTF("{\"type\":\"ack\",\"battPin\":%d,\"auto\":true}\n", BatteryMonitor::instance().getPin());
    } else if (cmd.startsWith("batt pin ")) {
        int p = cmd.substring(9).toInt();
        BatteryMonitor::instance().setPin(p);
        SPRINTF("{\"type\":\"ack\",\"battPin\":%d}\n", p);
    } else if (cmd.startsWith("batt divider ")) {
        float r = cmd.substring(13).toFloat();
        BatteryMonitor::instance().setDivider(r);
        SPRINTF("{\"type\":\"ack\",\"battDivider\":%.2f}\n", r);
    } else if (cmd.equalsIgnoreCase("gps") || cmd.equalsIgnoreCase("gps status")) {
        SPRINTF("{\"type\":\"gps_status\",\"hardware_detected\":%s,\"has_fix\":%s,\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.1f,\"speed_kmh\":%.1f,\"alt\":%.1f,\"sats\":%u,\"hdop\":%.1f}\n",
                      GpsReceiver::instance().hasHardwareDetected() ? "true" : "false",
                      NodeConfig::instance().hasGpsFix() ? "true" : "false",
                      NodeConfig::instance().getLat(), NodeConfig::instance().getLon(),
                      NodeConfig::instance().getAccM(), NodeConfig::instance().getSpeedKmh(),
                      NodeConfig::instance().getAltM(), GpsReceiver::instance().getSatellites(),
                      GpsReceiver::instance().getHdop());
    } else if (cmd.startsWith("attach ") || cmd.startsWith("follow ")) {
        String dev = cmd.substring(cmd.indexOf(' ') + 1);
        dev.trim();
        NodeConfig::instance().setAttachedDevice(dev.c_str());
        SPRINTF("{\"type\":\"ack\",\"action\":\"attached\",\"device\":\"%s\",\"following\":true}\n", dev.c_str());
    } else if (cmd.equalsIgnoreCase("detach") || cmd.equalsIgnoreCase("unfollow")) {
        NodeConfig::instance().clearAttachedDevice();
        NodeConfig::instance().clearGpsFix();
        SPRINTLN("{\"type\":\"ack\",\"action\":\"detached\",\"following\":false}");
    } else if (cmd.startsWith("gps ")) {
        String args = cmd.substring(4);
        args.trim();
        double lat = 0.0, lon = 0.0;
        float acc = 5.0f, speed = -1.0f, heading = -1.0f, alt = 0.0f;
        uint8_t sats = 0;
        float tripKm = -1.0f;

        int idx = 0;
        int field = 0;
        while (idx < args.length()) {
            int nextComma = args.indexOf(',', idx);
            String val = (nextComma >= 0) ? args.substring(idx, nextComma) : args.substring(idx);
            val.trim();
            if (field == 0) lat = val.toDouble();
            else if (field == 1) lon = val.toDouble();
            else if (field == 2) acc = val.toFloat();
            else if (field == 3) speed = val.toFloat();
            else if (field == 4) heading = val.toFloat();
            else if (field == 5) alt = val.toFloat();
            else if (field == 6) sats = val.toInt();
            else if (field == 7) tripKm = val.toFloat();

            field++;
            if (nextComma < 0) break;
            idx = nextComma + 1;
        }

        if (lat != 0.0 && lon != 0.0) {
            NodeConfig::instance().setGpsFix(lat, lon, acc, speed, heading, alt, sats, tripKm);
            SPRINTF("{\"type\":\"ack\",\"action\":\"gps_updated\",\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.1f,\"speed_mps\":%.2f,\"trip_km\":%.2f,\"traveling\":%s}\n",
                          lat, lon, acc, NodeConfig::instance().getSpeedMps(),
                          NodeConfig::instance().getTripDistKm(),
                          NodeConfig::instance().isTraveling() ? "true" : "false");
        }
    } else if (cmd.startsWith("travel ") || cmd.equalsIgnoreCase("travel")) {
        String args = cmd.length() > 6 ? cmd.substring(cmd.indexOf(' ') + 1) : "";
        args.trim();
        if (args.isEmpty() || args.equalsIgnoreCase("status")) {
            SPRINTF("{\"type\":\"travel_status\",\"traveling\":%s,\"state\":\"%s\",\"speed_kmh\":%.1f,\"speed_mph\":%.1f,\"heading\":%.1f,\"card\":\"%s\",\"alt_m\":%.1f,\"trip_km\":%.2f}\n",
                          NodeConfig::instance().isTraveling() ? "true" : "false",
                          NodeConfig::instance().getTravelStateStr(),
                          NodeConfig::instance().getSpeedKmh(),
                          NodeConfig::instance().getSpeedMph(),
                          NodeConfig::instance().getHeadingDeg(),
                          NodeConfig::instance().getHeadingCard(),
                          NodeConfig::instance().getAltM(),
                          NodeConfig::instance().getTripDistKm());
        } else {
            bool moving = args.startsWith("mov") || args.startsWith("1") || args.startsWith("true");
            float spdKmh = -1.0f;
            float hdg = -1.0f;
            float alt = 0.0f;
            float tripKm = -1.0f;
            int s1 = args.indexOf(' ');
            if (s1 > 0) {
                spdKmh = args.substring(s1 + 1).toFloat();
                int s2 = args.indexOf(' ', s1 + 1);
                if (s2 > 0) {
                    hdg = args.substring(s2 + 1).toFloat();
                    int s3 = args.indexOf(' ', s2 + 1);
                    if (s3 > 0) {
                        alt = args.substring(s3 + 1).toFloat();
                        int s4 = args.indexOf(' ', s3 + 1);
                        if (s4 > 0) {
                            tripKm = args.substring(s4 + 1).toFloat();
                        }
                    }
                }
            }
            NodeConfig::instance().setTravelState(moving, spdKmh, hdg, alt, tripKm);
            SPRINTF("{\"type\":\"ack\",\"action\":\"travel_updated\",\"traveling\":%s,\"speed_kmh\":%.1f,\"trip_km\":%.2f}\n",
                          moving ? "true" : "false", NodeConfig::instance().getSpeedKmh(),
                          NodeConfig::instance().getTripDistKm());
        }
    } else if (cmd.startsWith("trip ") || cmd.equalsIgnoreCase("trip")) {
        String args = cmd.length() > 4 ? cmd.substring(cmd.indexOf(' ') + 1) : "";
        args.trim();
        if (args.equalsIgnoreCase("reset")) {
            NodeConfig::instance().resetTrip();
            SPRINTLN("{\"type\":\"ack\",\"action\":\"trip_reset\"}");
        } else if (!args.isEmpty() && !args.equalsIgnoreCase("status")) {
            float distKm = args.toFloat();
            NodeConfig::instance().setTripDistKm(distKm);
            int s1 = args.indexOf(' ');
            if (s1 > 0) {
                float maxSpdKmh = args.substring(s1 + 1).toFloat();
                if (maxSpdKmh > 0.0f) {
                    NodeConfig::instance().setMaxSpeedKmh(maxSpdKmh);
                }
            }
            SPRINTF("{\"type\":\"ack\",\"action\":\"trip_updated\",\"trip_km\":%.2f,\"max_speed_kmh\":%.1f}\n",
                          NodeConfig::instance().getTripDistKm(), NodeConfig::instance().getMaxSpeedKmh());
        } else {
            SPRINTF("{\"type\":\"trip_status\",\"trip_km\":%.2f,\"max_speed_kmh\":%.1f}\n",
                          NodeConfig::instance().getTripDistKm(),
                          NodeConfig::instance().getMaxSpeedKmh());
        }
    } else if (cmd.startsWith("channel ") || cmd.startsWith("ch ")) {
        int ch = cmd.substring(cmd.indexOf(' ') + 1).toInt();
        WifiMonitor::instance().setChannel(ch);
        SPRINTF("{\"type\":\"ack\",\"channel\":%u,\"hop\":%s}\n",
                      WifiMonitor::instance().getChannel(),
                      WifiMonitor::instance().isAutoHop() ? "true" : "false");
    } else if (cmd.startsWith("hop ")) {
        int ms = cmd.substring(4).toInt();
        if (ms >= 20 && ms <= 5000) {
            WifiMonitor::instance().setHopInterval(ms);
            SPRINTF("{\"type\":\"ack\",\"hopInterval\":%u}\n", ms);
        }
    } else if (cmd.equalsIgnoreCase("led list")) {
        String json = "{\"type\":\"led_list\",\"slots\":[";
        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            const LedSlot& s = LedPatterns::instance().getSlot(i);
            if (i > 0) json += ",";
            json += "{\"slot\":" + String(i) +
                    ",\"enabled\":" + (s.enabled ? "true" : "false") +
                    ",\"type\":\"" + (s.type == LED_TYPE_SINGLE ? "single" : (s.type == LED_TYPE_RGB ? "rgb" : "none")) + "\"" +
                    ",\"role\":" + String((int)s.role) +
                    ",\"pinR\":" + String(s.pinR) +
                    ",\"pinG\":" + String(s.pinG) +
                    ",\"pinB\":" + String(s.pinB) +
                    ",\"inv\":" + (s.inverted ? "true" : "false") +
                    ",\"label\":\"" + String(s.label) + "\"}";
        }
        json += "],\"safePins\":[2,4,16,17,18,19,21,22,23,25,26,27,32,33]}";
        SPRINTLN(json);
    } else if (cmd.startsWith("led clear ")) {
        int slot = cmd.substring(10).toInt();
        LedPatterns::instance().clearSlot(slot);
        SPRINTF("{\"type\":\"ack\",\"action\":\"led_cleared\",\"slot\":%d}\n", slot);
    } else if (cmd.startsWith("led config ")) {
        // Syntax:
        // led config <slot> single <pin> <role> [inverted]
        // led config <slot> rgb <pinR> <pinG> <pinB> <role> [common_anode]
        char buf[128];
        strncpy(buf, cmd.c_str() + 11, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char* p = strtok(buf, " ");
        if (!p) { SPRINTLN("{\"type\":\"error\",\"message\":\"syntax_error\"}"); return; }
        int slot = atoi(p);

        p = strtok(NULL, " ");
        if (!p) { SPRINTLN("{\"type\":\"error\",\"message\":\"missing_type\"}"); return; }
        String typeStr = p;

        if (typeStr.equalsIgnoreCase("single")) {
            char* pinTok = strtok(NULL, " ");
            char* roleTok = strtok(NULL, " ");
            char* invTok = strtok(NULL, " ");
            if (!pinTok) { SPRINTLN("{\"type\":\"error\",\"message\":\"missing_pin\"}"); return; }
            int pin = atoi(pinTok);
            LedRole role = roleTok ? parseRole(roleTok) : ROLE_MIRROR;
            bool inv = invTok ? (atoi(invTok) != 0 || String(invTok).equalsIgnoreCase("true")) : false;

            if (LedPatterns::instance().configureSingle(slot, pin, role, inv)) {
                SPRINTF("{\"type\":\"ack\",\"action\":\"led_configured\",\"slot\":%d,\"type\":\"single\",\"pin\":%d,\"role\":%d}\n", slot, pin, (int)role);
            } else {
                SPRINTF("{\"type\":\"error\",\"message\":\"invalid_pin_or_slot\",\"pin\":%d}\n", pin);
            }
        } else if (typeStr.equalsIgnoreCase("rgb")) {
            char* rTok = strtok(NULL, " ");
            char* gTok = strtok(NULL, " ");
            char* bTok = strtok(NULL, " ");
            char* roleTok = strtok(NULL, " ");
            char* invTok = strtok(NULL, " ");
            if (!rTok || !gTok || !bTok) { SPRINTLN("{\"type\":\"error\",\"message\":\"missing_rgb_pins\"}"); return; }
            int pinR = atoi(rTok);
            int pinG = atoi(gTok);
            int pinB = atoi(bTok);
            LedRole role = roleTok ? parseRole(roleTok) : ROLE_MIRROR;
            bool ca = invTok ? (atoi(invTok) != 0 || String(invTok).equalsIgnoreCase("true") || String(invTok).equalsIgnoreCase("anode")) : false;

            if (LedPatterns::instance().configureRgb(slot, pinR, pinG, pinB, role, ca)) {
                SPRINTF("{\"type\":\"ack\",\"action\":\"led_configured\",\"slot\":%d,\"type\":\"rgb\",\"pins\":[%d,%d,%d],\"role\":%d}\n", slot, pinR, pinG, pinB, (int)role);
            } else {
                SPRINTLN("{\"type\":\"error\",\"message\":\"invalid_pins_or_slot\"}");
            }
        }
    } else if (cmd.startsWith("led ")) {
        String arg = cmd.substring(4);
        arg.trim();
        if (arg.equalsIgnoreCase("heartbeat")) {
            LedPatterns::instance().clearBroken();
            LedPatterns::instance().setBasePattern(PATTERN_HEARTBEAT);
        } else if (arg.equalsIgnoreCase("probe")) {
            LedPatterns::instance().triggerDetection(false);
        } else if (arg.equalsIgnoreCase("tracker")) {
            LedPatterns::instance().triggerDetection(true);
        } else if (arg.equalsIgnoreCase("alert")) {
            LedPatterns::instance().triggerDeauthAlert();
        } else if (arg.equalsIgnoreCase("radio_err")) {
            LedPatterns::instance().reportBroken(PATTERN_BROKEN_RADIO);
        } else if (arg.equalsIgnoreCase("heap_err")) {
            LedPatterns::instance().reportBroken(PATTERN_BROKEN_HEAP);
        } else if (arg.equalsIgnoreCase("sos")) {
            LedPatterns::instance().reportBroken(PATTERN_BROKEN_SOS);
        } else if (arg.equalsIgnoreCase("no_battery") || arg.equalsIgnoreCase("usb")) {
            LedPatterns::instance().setBasePattern(PATTERN_BATT_NO_BATTERY);
        } else if (arg.startsWith("pin ")) {
            int p = arg.substring(4).toInt();
            LedPatterns::instance().setPin(p);
            SPRINTF("{\"type\":\"ack\",\"ledPin\":%d}\n", p);
        }
        SPRINTLN("{\"type\":\"ack\",\"led\":\"pattern_applied\"}");
    } else if (cmd.equalsIgnoreCase("ota")) {
        WifiMonitor::instance().setChannel(1);
        SPRINTF("{\"type\":\"ack\",\"mode\":\"ota_ready\",\"url\":\"http://%s/\",\"ssid\":\"%s\"}\n",
                      WiFi.softAPIP().toString().c_str(), NodeConfig::instance().getName());
    } else if (cmd.startsWith("sf ack ")) {
        StoreForward::instance().ack((uint32_t)strtoul(cmd.c_str() + 7, nullptr, 10));
    } else if (cmd.equalsIgnoreCase("sf") || cmd.equalsIgnoreCase("sf status")) {
        SPRINTF("{\"type\":\"sf_status\",\"ok\":%s,\"pending\":%lu,\"used\":%u,\"total\":%u,\"storing\":%s,\"following\":\"%s\"}\n",
                StoreForward::instance().ok() ? "true" : "false", (unsigned long)StoreForward::instance().pending(),
                (unsigned)StoreForward::instance().usedBytes(), (unsigned)StoreForward::instance().totalBytes(),
                Telemetry::instance().storingForPhone() ? "true" : "false", NodeConfig::instance().getAttachedDevice());
    } else if (cmd.equalsIgnoreCase("sf clear")) {
        StoreForward::instance().clear();
        SPRINTLN("{\"type\":\"ack\",\"cmd\":\"sf clear\"}");
    } else if (cmd.equalsIgnoreCase("sf dump")) {
        StoreForward::instance().startDump();
    } else if (cmd.equalsIgnoreCase("wifi status") || cmd.equalsIgnoreCase("wifi")) {
        bool connected = (WiFi.status() == WL_CONNECTED);
        SPRINTF("{\"type\":\"wifi_status\",\"configured\":%s,\"ssid\":\"%s\",\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d}\n",
            NodeConfig::instance().hasWifiCreds() ? "true" : "false",
            NodeConfig::instance().getWifiSsid().c_str(),
            connected ? "true" : "false",
            connected ? WiFi.localIP().toString().c_str() : "",
            connected ? WiFi.RSSI() : 0);
    } else if (cmd.startsWith("wifi connect ")) {
        String args = cmd.substring(13);
        args.trim();
        String ssid, pass;
        int q = args.startsWith("\"") ? args.indexOf('"', 1) : -1;
        if (q > 0) {
            // wifi connect "SSID with spaces" pass
            ssid = args.substring(1, q);
            pass = args.substring(q + 1);
        } else {
            int spaceIdx = args.indexOf(' ');
            ssid = (spaceIdx > 0) ? args.substring(0, spaceIdx) : args;
            pass = (spaceIdx > 0) ? args.substring(spaceIdx + 1) : "";
        }
        pass.trim();
        NodeConfig::instance().setWifiCreds(ssid.c_str(), pass.c_str());
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        SPRINTF("{\"type\":\"ack\",\"action\":\"wifi_connecting\",\"ssid\":\"%s\"}\n", ssid.c_str());
    } else if (cmd.equalsIgnoreCase("wifi clear")) {
        NodeConfig::instance().clearWifiCreds();
        WiFi.disconnect(true);
    } else if (cmd.startsWith("baud ")) {
        long newBaud = cmd.substring(5).toInt();
        if (newBaud >= 9600 && newBaud <= 2000000) {
            SPRINTF("{\"type\":\"ack\",\"action\":\"baud_change\",\"baud\":%ld}\n", newBaud);
            Serial.flush();
            delay(50);
            Serial.begin(newBaud);
        }
    } else if (cmd.startsWith("mesh ota start ")) {
        // Syntax: mesh ota start <target_mac|all> <total_bytes> <chunk_size> <sha256_hex> <sig_hex> [version] [channel] [hw_type]
        String args = cmd.substring(15);
        args.trim();
        int s1 = args.indexOf(' ');
        int s2 = (s1 > 0) ? args.indexOf(' ', s1 + 1) : -1;
        int s3 = (s2 > 0) ? args.indexOf(' ', s2 + 1) : -1;
        int s4 = (s3 > 0) ? args.indexOf(' ', s3 + 1) : -1;
        int s5 = (s4 > 0) ? args.indexOf(' ', s4 + 1) : -1;

        if (s1 > 0 && s2 > 0 && s3 > 0 && s4 > 0) {
            String targetStr = args.substring(0, s1);
            uint32_t totalBytes = (uint32_t)args.substring(s1 + 1, s2).toInt();
            uint16_t chunkSize = (uint16_t)args.substring(s2 + 1, s3).toInt();
            String shaHex = args.substring(s3 + 1, s4);
            String sigHex = (s5 > 0) ? args.substring(s4 + 1, s5) : args.substring(s4 + 1);
            String verStr = "3.10.0";
            uint8_t channel = 1;
            uint8_t hwType = BEACONFIX_HW_TYPE;

            if (s5 > 0) {
                int s6 = args.indexOf(' ', s5 + 1);
                if (s6 > 0) {
                    verStr = args.substring(s5 + 1, s6);
                    int s7 = args.indexOf(' ', s6 + 1);
                    if (s7 > 0) {
                        channel = (uint8_t)args.substring(s6 + 1, s7).toInt();
                        hwType = (uint8_t)args.substring(s7 + 1).toInt();
                    } else {
                        channel = (uint8_t)args.substring(s6 + 1).toInt();
                    }
                } else {
                    verStr = args.substring(s5 + 1);
                }
            }

            uint8_t targetMac[6];
            parseMacStr(targetStr.c_str(), targetMac);

            uint8_t sha256[32];
            parseHexBytes(shaHex.c_str(), sha256, 32);

            uint8_t sig[72];
            size_t sigLen = 0;
            parseHexBytes(sigHex.c_str(), sig, sizeof(sig), &sigLen);

            bool ok = MeshOtaEngine::instance().transmitOtaStart(
                targetMac, totalBytes, chunkSize, sha256, sig, (uint8_t)sigLen, verStr.c_str(), channel, hwType
            );

            SPRINTF("{\"type\":\"ack\",\"action\":\"mesh_ota_start_sent\",\"ok\":%s,\"total\":%u,\"chunks\":%u,\"ch\":%u,\"hw\":%u}\n",
                          ok ? "true" : "false", totalBytes, (totalBytes + chunkSize - 1) / chunkSize, channel, hwType);
        } else {
            SPRINTLN("{\"type\":\"error\",\"action\":\"invalid_mesh_ota_start_args\"}");
        }
    } else if (cmd.startsWith("mesh ota chunk ")) {
        // Syntax: mesh ota chunk <target_mac|all> <chunk_idx> <hex_data>
        String args = cmd.substring(15);
        args.trim();
        int s1 = args.indexOf(' ');
        int s2 = (s1 > 0) ? args.indexOf(' ', s1 + 1) : -1;

        if (s1 > 0 && s2 > 0) {
            String targetStr = args.substring(0, s1);
            uint16_t chunkIdx = (uint16_t)args.substring(s1 + 1, s2).toInt();
            String hexData = args.substring(s2 + 1);

            uint8_t targetMac[6];
            parseMacStr(targetStr.c_str(), targetMac);

            uint8_t rawChunk[192];
            size_t chunkLen = 0;
            parseHexBytes(hexData.c_str(), rawChunk, sizeof(rawChunk), &chunkLen);

            bool ok = MeshOtaEngine::instance().transmitOtaChunk(targetMac, chunkIdx, rawChunk, (uint16_t)chunkLen);
            SPRINTF("{\"type\":\"ack\",\"action\":\"mesh_ota_chunk_sent\",\"chunk\":%u,\"len\":%u,\"ok\":%s}\n",
                          chunkIdx, (unsigned int)chunkLen, ok ? "true" : "false");
        } else {
            SPRINTLN("{\"type\":\"error\",\"action\":\"invalid_mesh_ota_chunk_args\"}");
        }
    } else if (cmd.startsWith("mesh ota query ")) {
        String targetStr = cmd.substring(15);
        targetStr.trim();
        uint8_t targetMac[6];
        parseMacStr(targetStr.c_str(), targetMac);
        bool ok = MeshOtaEngine::instance().transmitOtaQuery(targetMac);
        SPRINTF("{\"type\":\"ack\",\"action\":\"mesh_ota_query_sent\",\"ok\":%s}\n", ok ? "true" : "false");
    } else if (cmd.startsWith("mesh ota abort")) {
        String targetStr = cmd.length() > 15 ? cmd.substring(15) : "all";
        targetStr.trim();
        uint8_t targetMac[6];
        parseMacStr(targetStr.c_str(), targetMac);
        bool ok = MeshOtaEngine::instance().transmitOtaAbort(targetMac, "Host command aborted");
        SPRINTF("{\"type\":\"ack\",\"action\":\"mesh_ota_abort_sent\",\"ok\":%s}\n", ok ? "true" : "false");
    } else if (cmd.startsWith("mesh ota manifest ")) {
        // Syntax: mesh ota manifest <bytes> <chunk_sz> <sha256_hex> <sig_hex> [version] [hw_type]
        String args = cmd.substring(18);
        args.trim();
        int s1 = args.indexOf(' ');
        int s2 = (s1 > 0) ? args.indexOf(' ', s1 + 1) : -1;
        int s3 = (s2 > 0) ? args.indexOf(' ', s2 + 1) : -1;

        if (s1 > 0 && s2 > 0 && s3 > 0) {
            uint32_t totalBytes = (uint32_t)args.substring(0, s1).toInt();
            uint16_t chunkSz = (uint16_t)args.substring(s1 + 1, s2).toInt();
            String shaHex = args.substring(s2 + 1, s3);
            String rest = args.substring(s3 + 1);

            int s4 = rest.indexOf(' ');
            String sigHex = (s4 > 0) ? rest.substring(0, s4) : rest;
            String verStr = BEACONFIX_FW_VERSION;
            uint8_t hwType = BEACONFIX_HW_TYPE;

            if (s4 > 0) {
                String afterSig = rest.substring(s4 + 1);
                int s5 = afterSig.indexOf(' ');
                if (s5 > 0) {
                    verStr = afterSig.substring(0, s5);
                    hwType = (uint8_t)afterSig.substring(s5 + 1).toInt();
                } else {
                    verStr = afterSig;
                }
            }

            uint8_t sha256[32];
            parseHexBytes(shaHex.c_str(), sha256, 32);

            uint8_t sig[72];
            size_t sigLen = 0;
            parseHexBytes(sigHex.c_str(), sig, sizeof(sig), &sigLen);

            MeshOtaEngine::instance().saveManifest(totalBytes, chunkSz, sha256, sig, (uint8_t)sigLen, verStr.c_str(), hwType);
            SPRINTF("{\"type\":\"ack\",\"action\":\"manifest_set\",\"bytes\":%u,\"ver\":\"%s\",\"hw\":%u}\n",
                          totalBytes, verStr.c_str(), hwType);
        } else {
            SPRINTLN("{\"type\":\"error\",\"action\":\"invalid_manifest_args\"}");
        }
    } else if (cmd.startsWith("mesh ota seed") || cmd.startsWith("mesh seed")) {
        String targetStr = "";
        int s = cmd.indexOf(' ');
        if (s > 0) {
            int s2 = cmd.indexOf(' ', s + 1);
            if (s2 > 0) targetStr = cmd.substring(s2 + 1);
        }
        targetStr.trim();
        uint8_t targetMac[6];
        bool hasTarget = false;
        if (targetStr.length() >= 11 && !targetStr.equalsIgnoreCase("all")) {
            parseMacStr(targetStr.c_str(), targetMac);
            hasTarget = true;
        }
        bool ok = MeshOtaEngine::instance().triggerSeeding(hasTarget ? targetMac : nullptr);
        SPRINTF("{\"type\":\"ack\",\"action\":\"seeder_triggered\",\"ok\":%s,\"has_manifest\":%s,\"ver\":\"%s\"}\n",
                      ok ? "true" : "false",
                      MeshOtaEngine::instance().hasManifest() ? "true" : "false",
                      MeshOtaEngine::instance().getManifestVersion());
    } else if (cmd.equalsIgnoreCase("mesh ota status")) {
        SPRINTF("{\"type\":\"mesh_ota_local_status\",\"state\":%u,\"chunk\":%u,\"total\":%u,\"bytes\":%u,\"err\":%u,\"version\":\"%s\",\"has_manifest\":%s,\"is_seeding\":%s,\"manifest_ver\":\"%s\"}\n",
                      MeshOtaEngine::instance().getState(),
                      MeshOtaEngine::instance().getNextExpectedChunk(),
                      MeshOtaEngine::instance().getTotalChunks(),
                      MeshOtaEngine::instance().getBytesWritten(),
                      MeshOtaEngine::instance().getErrorCode(),
                      MeshOtaEngine::instance().getVersion(),
                      MeshOtaEngine::instance().hasManifest() ? "true" : "false",
                      MeshOtaEngine::instance().isSeeding() ? "true" : "false",
                      MeshOtaEngine::instance().getManifestVersion());
    } else if (cmd.equalsIgnoreCase("reboot")) {
        SPRINTLN("{\"type\":\"ack\",\"action\":\"rebooting\"}");
        delay(100);
        ESP.restart();
    } else {
        SPRINTF("{\"type\":\"error\",\"message\":\"unknown command '%s'\"}\n", cmd.c_str());
    }
}

void BleScanner::processIncomingRx(const std::string& rxStr) {
    queueRx(rxStr);
}

// ── Setup & Loop ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);

    // 1. Initialize NVS Configuration & Reddit-Style Node Name
    NodeConfig::instance().begin();
    const char* nodeName = NodeConfig::instance().getName();

    // 2. Initialize Multi-LED status engine (Internal LED on default GPIO 2)
    LedPatterns::instance().begin(2, false);

    Serial.printf("\n--- Node [%s] Initializing ---\n", nodeName);

    // 3. Initialize Battery Monitor for 3.7V vape Li-ion cell (default GPIO 35, 2.0 divider)
    BatteryMonitor::instance().begin(35, 2.0f);

    // 4. Start Wi-Fi in AP or AP+STA mode
    if (NodeConfig::instance().hasWifiCreds()) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(nodeName);
        WiFi.begin(NodeConfig::instance().getWifiSsid().c_str(), NodeConfig::instance().getWifiPass().c_str());
        Serial.printf("{\"type\":\"init\",\"status\":\"wifi_ap_sta_ready\",\"ssid\":\"%s\",\"home_wifi\":\"%s\"}\n",
                      nodeName, NodeConfig::instance().getWifiSsid().c_str());
    } else {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(nodeName);
        Serial.printf("{\"type\":\"init\",\"status\":\"wifi_ap_ready\",\"ssid\":\"%s\",\"ip\":\"%s\"}\n",
                      nodeName, WiFi.softAPIP().toString().c_str());
    }
    esp_wifi_set_ps(WIFI_PS_NONE); // Disable Wi-Fi power saving so ESP-NOW frames are not delayed or dropped!

    // 4.5. Initialize OLED display if attached (SCL=GPIO 22, SDA=GPIO 21)
    DisplayOled::instance().begin(22, 21);

    // 5. Initialize Sub-ms Time Synchronization
    TimeSync::instance().begin();

    // 6. Initialize Multi-Hop Store & Forward Mesh Engine
    MeshEngine::instance().begin();
    MeshOtaEngine::instance().begin();

    // 7. Initialize Telemetry & UDP Broadcast
    Telemetry::instance().begin(47824);
    Telemetry::instance().enableUdp(true);
    StoreForward::instance().begin();
    MeshEngine::instance().registerHostDeliveryHook([](const char* json) {
        Telemetry::instance().broadcastUdp(json);
    });

    // 8. Initialize Web Server (Port 80)
    OtaUpdater::instance().begin(80);

    // 9. Initialize BLE Server and Scanner
    if (!BleScanner::instance().begin(nodeName)) {
        Serial.println("{\"type\":\"error\",\"component\":\"ble\",\"message\":\"init_failed\"}");
    }

    // 10. Initialize Wi-Fi Promiscuous Monitor
    if (!WifiMonitor::instance().begin()) {
        Serial.println("{\"type\":\"error\",\"component\":\"wifi\",\"message\":\"monitor_init_failed\"}");
        LedPatterns::instance().reportBroken(PATTERN_BROKEN_RADIO);
    } else {
        Serial.println("{\"type\":\"init\",\"status\":\"monitor_active\",\"mode\":\"promiscuous\"}");
    }

    // 11. Initialize Hardware GPS Receiver (UART2 9600 baud on GPIO 16/17)
    GpsReceiver::instance().begin(9600);
}

void loop() {
    unsigned long now = millis();

    // 0. Process Hardware GPS Receiver UART stream
    GpsReceiver::instance().update();

    // Wi-Fi Station connection monitoring & NTP time sync
    static bool g_wasWifiConnected = false;
    static unsigned long g_lastNtpCheck = 0;
    bool isWifiConnected = (WiFi.status() == WL_CONNECTED);
    if (isWifiConnected && !g_wasWifiConnected) {
        g_wasWifiConnected = true;
        Serial.printf("{\"type\":\"wifi_status\",\"connected\":true,\"ip\":\"%s\",\"rssi\":%d}\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        configTime(0, 0, "pool.ntp.org", "time.google.com");
    } else if (!isWifiConnected && g_wasWifiConnected) {
        g_wasWifiConnected = false;
        Serial.println("{\"type\":\"wifi_status\",\"connected\":false}");
    }

    if (isWifiConnected && (now - g_lastNtpCheck >= 10000)) {
        g_lastNtpCheck = now;
        time_t ntpNow;
        time(&ntpNow);
        if (ntpNow > 1700000000) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            uint64_t epochUs = (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
            TimeSync::instance().setMasterTimeUs(epochUs);
        }
    }

    // 1. Update LED patterns (non-blocking)
    LedPatterns::instance().update();

    // 2. Update Battery Monitor
    BatteryMonitor::instance().update();

    // 3. Update Web Server
    OtaUpdater::instance().update();

    // 4. Update Multi-Hop Mesh & Store-and-Forward Queue
    MeshEngine::instance().update();
    MeshOtaEngine::instance().update();

    // 4.5. Update OLED Display
    DisplayOled::instance().update();

    // 5. Update Wi-Fi Monitor (channel hopping & PPS calculation)
    WifiMonitor::instance().update();

    // 5. Process captured Wi-Fi frames
    DetectedFrame frame;
    int framesProcessed = 0;
    while (WifiMonitor::instance().getNextFrame(&frame) && framesProcessed < 15) {
        framesProcessed++;
        switch (frame.category) {
            case CAT_PROBE_REQ:
                Telemetry::instance().emitProbe(frame);
                LedPatterns::instance().triggerDetection(false);
                break;
            case CAT_BEACON:
                Telemetry::instance().emitBeacon(frame);
                break;
            case CAT_DEAUTH:
            case CAT_DISASSOC:
                Telemetry::instance().emitDeauth(frame);
                LedPatterns::instance().triggerDeauthAlert();
                break;
            default:
                break;
        }
    }

    // 6. Process captured BLE tags
    DetectedBleTag tag;
    int tagsProcessed = 0;
    while (BleScanner::instance().getNextTag(&tag) && tagsProcessed < 5) {
        tagsProcessed++;
        Telemetry::instance().emitBleTag(tag);
    }

    // 7. Read Serial command input
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (g_serialBuffer.length() > 0) {
                handleCommand(g_serialBuffer);
                g_serialBuffer = "";
            }
        } else if (g_serialBuffer.length() < 128) {
            g_serialBuffer += c;
        }
    }

    // BLE NUS commands (queued by the NimBLE task) and the queue dump a new subscriber asked for
    String bleCmd;
    while (BleScanner::instance().popRxLine(bleCmd)) {
        if (bleCmd.length() > 0) handleCommand(bleCmd);
    }
    // WiFi commands (UDP 47825); a few per pass so a flood can't starve the loop
    String udpCmd;
    for (int i = 0; i < 4 && Telemetry::instance().popUdpCommand(udpCmd); ++i) {
        if (udpCmd.length() > 0) handleCommand(udpCmd);
    }
    if (BleScanner::instance().takeQueueDumpRequest()) {
        dumpMeshQueueToBle();
        StoreForward::instance().startDump();
    }
    StoreForward::instance().pump(BleScanner::instance().isConnected(), [](const char* line) {
        return BleScanner::instance().sendTelemetry(line);
    });

    // 8. Periodic Status Telemetry Heartbeat (every 5 seconds)
    if (now - g_lastStatusTime >= 5000) {
        g_lastStatusTime = now;
        uint32_t freeHeap = ESP.getFreeHeap();

        // Check heap health
        if (freeHeap < 20000 && !LedPatterns::instance().isBroken()) {
            LedPatterns::instance().reportBroken(PATTERN_BROKEN_HEAP);
        } else if (freeHeap >= 25000 && LedPatterns::instance().isBroken()) {
            LedPatterns::instance().clearBroken();
        }

        Telemetry::instance().emitStatus(
            NodeConfig::instance().getName(),
            now / 1000,
            freeHeap,
            WifiMonitor::instance().getChannel(),
            WifiMonitor::instance().isAutoHop(),
            WifiMonitor::instance().getPps(),
            WifiMonitor::instance().getTotalFrames(),
            WifiMonitor::instance().getProbeCount(),
            WifiMonitor::instance().getBeaconCount(),
            WifiMonitor::instance().getDeauthCount(),
            BleScanner::instance().isConnected() ? 1 : 0,
            BatteryMonitor::instance().getMilliVolts(),
            BatteryMonitor::instance().getPercentage(),
            BatteryMonitor::instance().getStateStr(),
            BatteryMonitor::instance().isCharging(),
            BatteryMonitor::instance().isTrained(),
            BatteryMonitor::instance().getTrainingPct(),
            BatteryMonitor::instance().getBatteryCycles()
        );
    }

    // Small yield for FreeRTOS background tasks
    delay(2);
}
