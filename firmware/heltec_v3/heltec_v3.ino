#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include "HeltecV3Pins.h"
#include "NodeConfig.h"
#include "LedPatterns.h"
#include "BatteryMonitor.h"
#include "DisplayOled.h"
#include "LoraRadio.h"
#include "WifiMonitor.h"
#include "BleScanner.h"
#include "Telemetry.h"
#include "OtaUpdater.h"
#include "TimeSync.h"
#include "MeshEngine.h"
#include "MeshOtaEngine.h"
#include "GpsReceiver.h"

static unsigned long g_lastStatusTime = 0;
static String g_serialBuffer = "";
static WiFiUDP g_cmdUdp;

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

static bool isBeaconFixNodeName(const char* name) {
    if (!name || name[0] == '\0') return false;
    if (strcmp(name, NodeConfig::instance().getName()) == 0) return true;
    if (strstr(name, "BeaconFix") || strstr(name, "beaconfix") || strstr(name, "Node") || strstr(name, "ESP32")) return true;
    int len = strlen(name);
    if (len >= 12 && len <= 32) {
        if (isdigit(name[len - 1]) && isdigit(name[len - 2]) && isdigit(name[len - 3]) && isdigit(name[len - 4])) {
            int caps = 0;
            for (int i = 0; i < len - 4; i++) {
                if (isupper(name[i])) caps++;
            }
            if (caps >= 2) return true;
        }
    }
    return false;
}

static bool isFlockRfDevice(const char* name, const uint8_t* mac = nullptr) {
    if (isBeaconFixNodeName(name)) return false;

    if (name && name[0] != '\0') {
        char s[36];
        int i = 0;
        while (name[i] && i < 35) {
            s[i] = tolower((unsigned char)name[i]);
            i++;
        }
        s[i] = '\0';
        if (strncmp(s, "flock", 5) == 0 ||
            strncmp(s, "fs_", 3) == 0 ||
            strncmp(s, "fs-", 3) == 0 ||
            strncmp(s, "falcon-flex", 11) == 0 ||
            strncmp(s, "falcon_flex", 11) == 0 ||
            strncmp(s, "sparrow-", 8) == 0 ||
            strstr(s, "cradlepoint") != NULL) {
            return true;
        }
    }
    if (mac) {
        // InHand Networks OUI: 00:18:0A
        if (mac[0] == 0x00 && mac[1] == 0x18 && mac[2] == 0x0A) return true;
        // Cradlepoint OUI: 00:30:44
        if (mac[0] == 0x00 && mac[1] == 0x30 && mac[2] == 0x44) return true;
    }
    return false;
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

    // Check Apple FindMy / AirTag (Company ID 0x004C, Type 0x12)
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
    } else if (dev->haveName()) {
        if (isFlockRfDevice(tag.name)) {
            strncpy(tag.kind, "flock_ble", sizeof(tag.kind) - 1);
            isTracker = true;
        }
    }

    if (isTracker) {
        int len = mfg.length();
        if (len > 16) len = 16;
        for (int i = 0; i < len; i++) {
            snprintf(tag.payloadHex + (i * 2), 3, "%02x", (uint8_t)mfg[i]);
        }
        BleScanner::instance().enqueueTag(tag);
        LedPatterns::instance().triggerDetection(true, tag.kind);
    }
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
                Serial.printf("{\"type\":\"ack\",\"time_synced\":true,\"stratum\":1,\"epoch_us\":%llu}\n", (unsigned long long)epochUs);
                return;
            }
        }

        // Parse incoming ALPR JSON commands (e.g. from BLE, Serial, or UDP)
        if (cmd.indexOf("\"alpr\"") >= 0 || cmd.indexOf("\"camera_pass\"") >= 0 ||
            cmd.indexOf("\"flock_pass\"") >= 0 || cmd.indexOf("\"alpr_alert\"") >= 0) {
            char op[32] = "Flock Safety";
            char mdl[24] = "Falcon";
            float dist = 42.0f;
            uint8_t conf = 95;
            int facing = 1;
            float spd = -1.0f;
            double lat = 0.0, lon = 0.0;

            int opIdx = cmd.indexOf("\"operator\":");
            if (opIdx >= 0) {
                int q1 = cmd.indexOf('\"', opIdx + 11);
                int q2 = (q1 >= 0) ? cmd.indexOf('\"', q1 + 1) : -1;
                if (q1 >= 0 && q2 > q1) {
                    String opStr = cmd.substring(q1 + 1, q2);
                    strncpy(op, opStr.c_str(), sizeof(op) - 1);
                    op[sizeof(op) - 1] = '\0';
                }
            }
            int distIdx = cmd.indexOf("\"dist\":");
            if (distIdx < 0) distIdx = cmd.indexOf("\"distance_m\":");
            if (distIdx >= 0) {
                dist = cmd.substring(cmd.indexOf(':', distIdx) + 1).toFloat();
            }
            int confIdx = cmd.indexOf("\"conf\":");
            if (confIdx >= 0) {
                conf = (uint8_t)cmd.substring(confIdx + 7).toInt();
            }
            int faceIdx = cmd.indexOf("\"facing\":");
            if (faceIdx >= 0) {
                facing = cmd.substring(faceIdx + 9).toInt();
            }
            int spdIdx = cmd.indexOf("\"speed\":");
            if (spdIdx >= 0) {
                spd = cmd.substring(spdIdx + 8).toFloat();
            }
            int latIdx = cmd.indexOf("\"lat\":");
            if (latIdx >= 0) {
                lat = cmd.substring(latIdx + 6).toDouble();
            }
            int lonIdx = cmd.indexOf("\"lon\":");
            if (lonIdx >= 0) {
                lon = cmd.substring(lonIdx + 6).toDouble();
            }

            DisplayOled::instance().triggerAlprAlert(op, mdl, dist, conf, (int8_t)facing, spd, lat, lon);
            LedPatterns::instance().triggerDeauthAlert();
            Serial.printf("{\"type\":\"ack\",\"action\":\"alpr_json_triggered\",\"operator\":\"%s\",\"distance\":%.1f}\n", op, dist);
            return;
        }
    }

    if (cmd.equalsIgnoreCase("help")) {
        Serial.println("{\"type\":\"help\",\"commands\":["
                       "\"status\",\"name [new_name|reset]\",\"mode <base|mobile|toggle|status>\","
                       "\"channel <0-14>\",\"hop <ms>\",\"time <epoch_us>\","
                       "\"mesh status\",\"mesh peers\",\"mesh send <text>\","
                       "\"lora freq <mhz>\",\"lora send <text>\","
                       "\"screen <on|off|toggle|status>\",\"page <0-7|next|prev|cycle on|cycle off>\",\"contrast <0-255>\","
                       "\"travel <status|moving|stationary> [kmh] [hdg] [alt] [trip_km]\",\"trip <km|reset|status>\",\"alpr <test|dismiss|alert ...>\","
                       "\"wifi status\",\"wifi connect <ssid> <pass>\",\"wifi clear\","
                       "\"gps [lat,lon,acc,...]\",\"led <pattern>\",\"batt [status|stats|reset|on|off|<mah>]\",\"ota\",\"reboot\"]}");
    } else if (cmd.equalsIgnoreCase("gps") || cmd.equalsIgnoreCase("gps status")) {
        Serial.printf("{\"type\":\"gps_status\",\"hardware_detected\":%s,\"has_fix\":%s,\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.1f,\"speed_kmh\":%.1f,\"alt\":%.1f,\"sats\":%u,\"hdop\":%.1f}\n",
                      GpsReceiver::instance().hasHardwareDetected() ? "true" : "false",
                      NodeConfig::instance().hasGpsFix() ? "true" : "false",
                      NodeConfig::instance().getLat(), NodeConfig::instance().getLon(),
                      NodeConfig::instance().getAccM(), NodeConfig::instance().getSpeedKmh(),
                      NodeConfig::instance().getAltM(), GpsReceiver::instance().getSatellites(),
                      GpsReceiver::instance().getHdop());
    } else if (cmd.equalsIgnoreCase("mode") || cmd.equalsIgnoreCase("mode status")) {
        Serial.printf("{\"type\":\"op_mode\",\"mode\":\"%s\",\"is_base_station\":%s,\"hops\":%u,\"power_dbm\":%d}\n",
            NodeConfig::instance().getOpModeStr(),
            NodeConfig::instance().isBaseStation() ? "true" : "false",
            MeshEngine::instance().getHopsToGateway(),
            LoraRadio::instance().getPowerDbm());
    } else if (cmd.equalsIgnoreCase("mode base") || cmd.equalsIgnoreCase("mode base_station") || cmd.equalsIgnoreCase("mode station")) {
        NodeConfig::instance().setOpMode(OP_MODE_BASE_STATION);
        LoraRadio::instance().setBaseStationMode(true);
        DisplayOled::instance().showModeToast(OP_MODE_BASE_STATION);
        Serial.println("{\"type\":\"ack\",\"action\":\"mode_changed\",\"mode\":\"base_station\",\"is_base_station\":true,\"power_dbm\":22}");
    } else if (cmd.equalsIgnoreCase("mode mobile")) {
        NodeConfig::instance().setOpMode(OP_MODE_MOBILE);
        LoraRadio::instance().setBaseStationMode(false);
        DisplayOled::instance().showModeToast(OP_MODE_MOBILE);
        Serial.println("{\"type\":\"ack\",\"action\":\"mode_changed\",\"mode\":\"mobile\",\"is_base_station\":false,\"power_dbm\":14}");
    } else if (cmd.equalsIgnoreCase("mode toggle")) {
        NodeOpMode newMode = NodeConfig::instance().isBaseStation() ? OP_MODE_MOBILE : OP_MODE_BASE_STATION;
        NodeConfig::instance().setOpMode(newMode);
        LoraRadio::instance().setBaseStationMode(newMode == OP_MODE_BASE_STATION);
        DisplayOled::instance().showModeToast(newMode);
        Serial.printf("{\"type\":\"ack\",\"action\":\"mode_changed\",\"mode\":\"%s\",\"is_base_station\":%s,\"power_dbm\":%d}\n",
            NodeConfig::instance().getOpModeStr(),
            NodeConfig::instance().isBaseStation() ? "true" : "false",
            LoraRadio::instance().getPowerDbm());
    } else if (cmd.startsWith("time ")) {
        uint64_t epochUs = strtoull(cmd.substring(5).c_str(), NULL, 10);
        if (epochUs > 1000000000000ULL) {
            TimeSync::instance().setMasterTimeUs(epochUs);
            Serial.printf("{\"type\":\"ack\",\"time_synced\":true,\"stratum\":1,\"epoch_us\":%llu}\n", (unsigned long long)epochUs);
        } else {
            Serial.println("{\"type\":\"error\",\"message\":\"invalid_timestamp\"}");
        }
    } else if (cmd.startsWith("channel ") || cmd.startsWith("ch ")) {
        uint8_t ch = cmd.substring(cmd.indexOf(' ') + 1).toInt();
        WifiMonitor::instance().setChannel(ch);
        Serial.printf("{\"type\":\"ack\",\"channel\":%u,\"hop\":%s}\n",
                      WifiMonitor::instance().getChannel(),
                      WifiMonitor::instance().isAutoHop() ? "true" : "false");
    } else if (cmd.startsWith("attach ") || cmd.startsWith("follow ")) {
        String dev = cmd.substring(cmd.indexOf(' ') + 1);
        dev.trim();
        NodeConfig::instance().setAttachedDevice(dev.c_str());
        Serial.printf("{\"type\":\"ack\",\"action\":\"attached\",\"device\":\"%s\",\"following\":true}\n", dev.c_str());
    } else if (cmd.equalsIgnoreCase("detach") || cmd.equalsIgnoreCase("unfollow")) {
        NodeConfig::instance().clearAttachedDevice();
        NodeConfig::instance().clearGpsFix();
        Serial.println("{\"type\":\"ack\",\"action\":\"detached\",\"following\":false}");
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
            Serial.printf("{\"type\":\"ack\",\"action\":\"gps_updated\",\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.1f,\"speed_mps\":%.2f,\"trip_km\":%.2f,\"traveling\":%s}\n",
                          lat, lon, acc, NodeConfig::instance().getSpeedMps(),
                          NodeConfig::instance().getTripDistKm(),
                          NodeConfig::instance().isTraveling() ? "true" : "false");
        }
    } else if (cmd.startsWith("travel ") || cmd.equalsIgnoreCase("travel")) {
        String args = cmd.length() > 6 ? cmd.substring(cmd.indexOf(' ') + 1) : "";
        args.trim();
        if (args.isEmpty() || args.equalsIgnoreCase("status")) {
            Serial.printf("{\"type\":\"travel_status\",\"traveling\":%s,\"state\":\"%s\",\"speed_kmh\":%.1f,\"speed_mph\":%.1f,\"heading\":%.1f,\"card\":\"%s\",\"alt_m\":%.1f,\"trip_km\":%.2f}\n",
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
            Serial.printf("{\"type\":\"ack\",\"action\":\"travel_updated\",\"traveling\":%s,\"speed_kmh\":%.1f,\"trip_km\":%.2f}\n",
                          moving ? "true" : "false", NodeConfig::instance().getSpeedKmh(),
                          NodeConfig::instance().getTripDistKm());
        }
    } else if (cmd.startsWith("trip ") || cmd.equalsIgnoreCase("trip")) {
        String args = cmd.length() > 4 ? cmd.substring(cmd.indexOf(' ') + 1) : "";
        args.trim();
        if (args.equalsIgnoreCase("reset")) {
            NodeConfig::instance().resetTrip();
            Serial.println("{\"type\":\"ack\",\"action\":\"trip_reset\"}");
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
            Serial.printf("{\"type\":\"ack\",\"action\":\"trip_updated\",\"trip_km\":%.2f,\"max_speed_kmh\":%.1f}\n",
                          NodeConfig::instance().getTripDistKm(), NodeConfig::instance().getMaxSpeedKmh());
        } else {
            Serial.printf("{\"type\":\"trip_status\",\"trip_km\":%.2f,\"max_speed_kmh\":%.1f}\n",
                          NodeConfig::instance().getTripDistKm(),
                          NodeConfig::instance().getMaxSpeedKmh());
        }
    } else if (cmd.startsWith("alpr ") || cmd.equalsIgnoreCase("alpr")) {
        String args = cmd.length() > 4 ? cmd.substring(5) : "";
        args.trim();
        if (args.equalsIgnoreCase("test") || args.equalsIgnoreCase("demo")) {
            // Trigger test ALPR splash with realistic Flock Falcon metrics
            DisplayOled::instance().triggerAlprAlert("Flock Safety", "Falcon Flex", 38.0f, 96, 1, 58.0f,
                                                   NodeConfig::instance().getLat(), NodeConfig::instance().getLon(), 9000);
            LedPatterns::instance().triggerDeauthAlert();
            Serial.println("{\"type\":\"ack\",\"action\":\"alpr_splash_triggered\",\"operator\":\"Flock Safety\",\"model\":\"Falcon Flex\",\"distance_m\":38,\"conf\":96}");
        } else if (args.equalsIgnoreCase("dismiss")) {
            DisplayOled::instance().dismissAlprAlert();
            Serial.println("{\"type\":\"ack\",\"action\":\"alpr_dismissed\"}");
        } else if (args.equalsIgnoreCase("status") || args.isEmpty()) {
            Serial.printf("{\"type\":\"alpr_status\",\"passes\":%u,\"active\":%s}\n",
                          DisplayOled::instance().getAlprPassCount(),
                          DisplayOled::instance().isAlprAlertActive() ? "true" : "false");
        } else {
            char op[32] = "Flock Safety";
            char mdl[24] = "Falcon";
            float dist = 45.0f;
            uint8_t conf = 95;
            int facing = 1;
            float speed = -1.0f;
            double lat = 0.0;
            double lon = 0.0;
            uint32_t dur = 9000;
            sscanf(args.c_str(), "%31s %23s %f %hhu %d %f %lf %lf %u",
                   op, mdl, &dist, &conf, &facing, &speed, &lat, &lon, &dur);
            DisplayOled::instance().triggerAlprAlert(op, mdl, dist, conf, (int8_t)facing, speed, lat, lon, dur);
            LedPatterns::instance().triggerDeauthAlert();
            Serial.printf("{\"type\":\"ack\",\"action\":\"alpr_alert_set\",\"operator\":\"%s\",\"distance\":%.1f}\n", op, dist);
        }
    } else if (cmd.startsWith("antenna") || cmd.equalsIgnoreCase("ant")) {
        String args = cmd.length() > 7 ? cmd.substring(8) : "";
        args.trim();
        if (args.equalsIgnoreCase("on") || args.equalsIgnoreCase("enable") || args.equalsIgnoreCase("1")) {
            LoraRadio::instance().setAntennaDetected(true);
            LoraRadio::instance().setTxInhibited(false);
            Serial.println("{\"type\":\"ack\",\"action\":\"antenna_forced\",\"detected\":true,\"tx_inhibited\":false}");
        } else if (args.equalsIgnoreCase("off") || args.equalsIgnoreCase("disable") || args.equalsIgnoreCase("0")) {
            LoraRadio::instance().setAntennaDetected(false);
            LoraRadio::instance().setTxInhibited(true);
            Serial.println("{\"type\":\"ack\",\"action\":\"antenna_forced\",\"detected\":false,\"tx_inhibited\":true}");
        } else if (args.equalsIgnoreCase("auto") || args.equalsIgnoreCase("detect")) {
            LoraRadio::instance().clearManualOverride();
            bool det = LoraRadio::instance().checkAntennaPresence();
            Serial.printf("{\"type\":\"ack\",\"action\":\"antenna_auto\",\"detected\":%s,\"tx_inhibited\":%s,\"ambient_rssi\":%d}\n",
                          det ? "true" : "false",
                          LoraRadio::instance().isTxInhibited() ? "true" : "false",
                          LoraRadio::instance().getAmbientRssi());
        } else {
            bool det = LoraRadio::instance().hasAntenna();
            Serial.printf("{\"type\":\"antenna_status\",\"detected\":%s,\"tx_inhibited\":%s,\"ambient_rssi\":%d}\n",
                          det ? "true" : "false",
                          LoraRadio::instance().isTxInhibited() ? "true" : "false",
                          LoraRadio::instance().getAmbientRssi());
        }
    } else if (cmd.startsWith("batt") || cmd.startsWith("battery")) {
        String args = cmd.length() > 4 ? cmd.substring(cmd.indexOf(' ') + 1) : "";
        args.trim();
        if (args.equalsIgnoreCase("reset") || args.equalsIgnoreCase("stats reset") || args.equalsIgnoreCase("reset stats") || args.equalsIgnoreCase("train reset")) {
            BatteryMonitor::instance().resetBatteryStats();
            const auto& s = BatteryMonitor::instance().getTrainingStats();
            Serial.printf("{\"type\":\"ack\",\"action\":\"battery_stats_reset\",\"trained\":false,\"v_min\":%u,\"v_max\":%u,\"cycles\":%.1f,\"runtime_sec\":0}\n",
                          s.vMin, s.vMax, s.cycles);
        } else if (args.equalsIgnoreCase("stats") || args.equalsIgnoreCase("diag") || args.equalsIgnoreCase("info")) {
            const auto& s = BatteryMonitor::instance().getTrainingStats();
            Serial.printf("{\"type\":\"battery_stats\",\"has_battery\":%s,\"mah\":%u,\"mv\":%lu,\"pct\":%u,\"state\":\"%s\",\"charging\":%s,\"trained\":%s,\"train_pct\":%u,\"v_min\":%u,\"v_max\":%u,\"v_nom\":%u,\"cycles\":%.2f,\"runtime_sec\":%lu,\"samples\":%lu}\n",
                          BatteryMonitor::instance().hasBattery() ? "true" : "false",
                          NodeConfig::instance().getBattMah(),
                          (unsigned long)BatteryMonitor::instance().getMilliVolts(),
                          BatteryMonitor::instance().getPercentage(),
                          BatteryMonitor::instance().getStateStr(),
                          BatteryMonitor::instance().isCharging() ? "true" : "false",
                          s.isTrained ? "true" : "false",
                          s.trainPct,
                          s.vMin,
                          s.vMax,
                          s.vNom,
                          s.cycles,
                          (unsigned long)s.runtimeSec,
                          (unsigned long)s.sampleCount);
        } else if (args.equalsIgnoreCase("none") || args.equalsIgnoreCase("off") || args.equalsIgnoreCase("0") || args.equalsIgnoreCase("usb")) {
            NodeConfig::instance().setBatteryEquipped(false);
            BatteryMonitor::instance().update();
            Serial.println("{\"type\":\"ack\",\"action\":\"battery_disabled\",\"has_battery\":false,\"power\":\"usb_5v\"}");
        } else if (args.equalsIgnoreCase("on") || args.equalsIgnoreCase("enable")) {
            NodeConfig::instance().setBatteryEquipped(true);
            BatteryMonitor::instance().update();
            Serial.printf("{\"type\":\"ack\",\"action\":\"battery_enabled\",\"has_battery\":true,\"mah\":%u}\n", NodeConfig::instance().getBattMah());
        } else if (args.toInt() > 0) {
            uint32_t mah = args.toInt();
            NodeConfig::instance().setBattMah(mah);
            BatteryMonitor::instance().update();
            Serial.printf("{\"type\":\"ack\",\"action\":\"battery_capacity_set\",\"mah\":%u,\"has_battery\":true}\n", mah);
        } else {
            const auto& s = BatteryMonitor::instance().getTrainingStats();
            Serial.printf("{\"type\":\"battery_status\",\"has_battery\":%s,\"mah\":%u,\"mv\":%lu,\"pct\":%u,\"state\":\"%s\",\"charging\":%s,\"trained\":%s,\"train_pct\":%u,\"cycles\":%.1f}\n",
                          BatteryMonitor::instance().hasBattery() ? "true" : "false",
                          NodeConfig::instance().getBattMah(),
                          (unsigned long)BatteryMonitor::instance().getMilliVolts(),
                          BatteryMonitor::instance().getPercentage(),
                          BatteryMonitor::instance().getStateStr(),
                          BatteryMonitor::instance().isCharging() ? "true" : "false",
                          s.isTrained ? "true" : "false",
                          s.trainPct,
                          s.cycles);
        }
    } else if (cmd.equalsIgnoreCase("base") || cmd.equalsIgnoreCase("mode base")) {
        NodeConfig::instance().setOpMode(OP_MODE_BASE_STATION);
        LoraRadio::instance().setBaseStationMode(true);
        Serial.println("{\"type\":\"ack\",\"action\":\"mode_set\",\"mode\":\"base_station\"}");
    } else if (cmd.equalsIgnoreCase("mobile") || cmd.equalsIgnoreCase("mode mobile")) {
        NodeConfig::instance().setOpMode(OP_MODE_MOBILE);
        LoraRadio::instance().setBaseStationMode(false);
        Serial.println("{\"type\":\"ack\",\"action\":\"mode_set\",\"mode\":\"mobile\"}");
    } else if (cmd.equalsIgnoreCase("mesh status") || cmd.equalsIgnoreCase("mesh")) {
        Serial.printf("{\"type\":\"mesh_status\",\"is_gateway\":%s,\"mode\":\"%s\",\"hops\":%u,\"neighbors\":%u,\"queue\":%u,\"stratum\":%u,\"now_us\":%llu,\"lora\":%s,\"lora_tx\":%u,\"lora_rx\":%u,\"power_dbm\":%d}\n",
            MeshEngine::instance().isGateway() ? "true" : "false",
            NodeConfig::instance().getOpModeStr(),
            MeshEngine::instance().getHopsToGateway(),
            MeshEngine::instance().getNeighborCount(),
            MeshEngine::instance().getQueueCount(),
            TimeSync::instance().getStratum(),
            (unsigned long long)TimeSync::instance().getNowUs(),
            LoraRadio::instance().isReady() ? "true" : "false",
            LoraRadio::instance().getTxCount(),
            LoraRadio::instance().getRxCount(),
            LoraRadio::instance().getPowerDbm());
    } else if (cmd.equalsIgnoreCase("mesh peers")) {
        const MeshNeighbor* n = MeshEngine::instance().getNeighbors();
        uint8_t count = MeshEngine::instance().getNeighborCount();
        Serial.printf("{\"type\":\"mesh_peers\",\"count\":%u,\"peers\":[", count);
        for (int i = 0; i < count; i++) {
            char macStr[18];
            WifiMonitor::formatMac(n[i].mac, macStr);
            Serial.printf("{\"name\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,\"ch\":%u,\"hops\":%u,\"gateway\":%s,\"stratum\":%u}%s",
                n[i].name, macStr, n[i].rssi, n[i].channel, n[i].hopsToGateway,
                n[i].isGateway ? "true" : "false", n[i].stratum,
                (i < count - 1) ? "," : "");
        }
        Serial.println("]}");
    } else if (cmd.startsWith("mesh send ")) {
        String msg = cmd.substring(10);
        MeshEngine::instance().sendTelemetry(msg.c_str());
        Serial.printf("{\"type\":\"ack\",\"mesh_sent\":true,\"text\":\"%s\"}\n", msg.c_str());
    } else if (cmd.startsWith("baud ")) {
        long newBaud = cmd.substring(5).toInt();
        if (newBaud >= 9600 && newBaud <= 2000000) {
            Serial.printf("{\"type\":\"ack\",\"action\":\"baud_change\",\"baud\":%ld}\n", newBaud);
            Serial.flush();
            delay(50);
            Serial.begin(newBaud);
        }
    } else if (cmd.startsWith("mesh ota start ")) {
        // Syntax: mesh ota start <target_mac|all> <total_bytes> <chunk_size> <sha256_hex> <sig_hex> [version] [channel]
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

            Serial.printf("{\"type\":\"ack\",\"action\":\"mesh_ota_start_sent\",\"ok\":%s,\"total\":%u,\"chunks\":%u,\"ch\":%u,\"hw\":%u}\n",
                          ok ? "true" : "false", totalBytes, (totalBytes + chunkSize - 1) / chunkSize, channel, hwType);
        } else {
            Serial.println("{\"type\":\"error\",\"action\":\"invalid_mesh_ota_start_args\"}");
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
            Serial.printf("{\"type\":\"ack\",\"action\":\"mesh_ota_chunk_sent\",\"chunk\":%u,\"len\":%u,\"ok\":%s}\n",
                          chunkIdx, (unsigned int)chunkLen, ok ? "true" : "false");
        } else {
            Serial.println("{\"type\":\"error\",\"action\":\"invalid_mesh_ota_chunk_args\"}");
        }
    } else if (cmd.startsWith("mesh ota query ")) {
        String targetStr = cmd.substring(15);
        targetStr.trim();
        uint8_t targetMac[6];
        parseMacStr(targetStr.c_str(), targetMac);
        bool ok = MeshOtaEngine::instance().transmitOtaQuery(targetMac);
        Serial.printf("{\"type\":\"ack\",\"action\":\"mesh_ota_query_sent\",\"ok\":%s}\n", ok ? "true" : "false");
    } else if (cmd.startsWith("mesh ota abort")) {
        String targetStr = cmd.length() > 15 ? cmd.substring(15) : "all";
        targetStr.trim();
        uint8_t targetMac[6];
        parseMacStr(targetStr.c_str(), targetMac);
        bool ok = MeshOtaEngine::instance().transmitOtaAbort(targetMac, "Host command aborted");
        Serial.printf("{\"type\":\"ack\",\"action\":\"mesh_ota_abort_sent\",\"ok\":%s}\n", ok ? "true" : "false");
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
            Serial.printf("{\"type\":\"ack\",\"action\":\"manifest_set\",\"bytes\":%u,\"ver\":\"%s\",\"hw\":%u}\n",
                          totalBytes, verStr.c_str(), hwType);
        } else {
            Serial.println("{\"type\":\"error\",\"action\":\"invalid_manifest_args\"}");
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
        Serial.printf("{\"type\":\"ack\",\"action\":\"seeder_triggered\",\"ok\":%s,\"has_manifest\":%s,\"ver\":\"%s\"}\n",
                      ok ? "true" : "false",
                      MeshOtaEngine::instance().hasManifest() ? "true" : "false",
                      MeshOtaEngine::instance().getManifestVersion());
    } else if (cmd.equalsIgnoreCase("mesh ota status")) {
        Serial.printf("{\"type\":\"mesh_ota_local_status\",\"state\":%u,\"chunk\":%u,\"total\":%u,\"bytes\":%u,\"err\":%u,\"version\":\"%s\",\"has_manifest\":%s,\"is_seeding\":%s,\"manifest_ver\":\"%s\"}\n",
                      MeshOtaEngine::instance().getState(),
                      MeshOtaEngine::instance().getNextExpectedChunk(),
                      MeshOtaEngine::instance().getTotalChunks(),
                      MeshOtaEngine::instance().getBytesWritten(),
                      MeshOtaEngine::instance().getErrorCode(),
                      MeshOtaEngine::instance().getVersion(),
                      MeshOtaEngine::instance().hasManifest() ? "true" : "false",
                      MeshOtaEngine::instance().isSeeding() ? "true" : "false",
                      MeshOtaEngine::instance().getManifestVersion());
    } else if (cmd.startsWith("lora send ")) {
        String msg = cmd.substring(10);
        bool ok = LoraRadio::instance().transmitPacket((const uint8_t*)msg.c_str(), msg.length());
    } else if (cmd.equalsIgnoreCase("version") || cmd.equalsIgnoreCase("ver")) {
        Serial.printf("{\"type\":\"version\",\"version\":\"%s\",\"build\":\"%s\",\"hardware\":\"Heltec_LoRa_V3_ESP32S3\",\"chip\":\"ESP32-S3\"}\n",
                      BEACONFIX_FW_VERSION, BEACONFIX_BUILD_DATE);
    } else if (cmd.equalsIgnoreCase("status")) {
        Serial.printf("{\"type\":\"status\",\"version\":\"%s\",\"node\":\"%s\",\"mac\":\"%s\",\"mode\":\"%s\",\"is_base_station\":%s,\"hardware\":\"Heltec_LoRa_V3_ESP32S3\","
                      "\"batt_mv\":%lu,\"batt_pct\":%u,\"batt_state\":\"%s\",\"batt_trained\":%s,\"batt_train_pct\":%u,\"batt_cycles\":%.2f,"
                      "\"wifi_ch\":%u,\"wifi_pps\":%u,\"total_frames\":%lu,"
                      "\"lora_freq\":%.2f,\"lora_tx\":%u,\"lora_rx\":%u,\"lora_rssi\":%d,\"power_dbm\":%d,"
                      "\"antenna_detected\":%s,\"tx_inhibited\":%s,\"ambient_rssi\":%d,"
                      "\"mesh_gw\":%s,\"mesh_hops\":%u,\"stratum\":%u,"
                      "\"traveling\":%s,\"speed_kmh\":%.1f,\"heading\":%.1f,\"alt\":%.1f,\"trip_km\":%.2f,"
                      "\"attached_dev\":\"%s\",\"following\":%s}\n",
                      BEACONFIX_FW_VERSION,
                      NodeConfig::instance().getName(),
                      WiFi.macAddress().c_str(),
                      NodeConfig::instance().getOpModeStr(),
                      NodeConfig::instance().isBaseStation() ? "true" : "false",
                      (unsigned long)BatteryMonitor::instance().getMilliVolts(),
                      BatteryMonitor::instance().getPercentage(),
                      BatteryMonitor::instance().getStateStr(),
                      BatteryMonitor::instance().isTrained() ? "true" : "false",
                      BatteryMonitor::instance().getTrainingPct(),
                      BatteryMonitor::instance().getBatteryCycles(),
                      WifiMonitor::instance().getChannel(),
                      WifiMonitor::instance().getPps(),
                      (unsigned long)WifiMonitor::instance().getTotalFrames(),
                      LoraRadio::instance().getFreq(),
                      LoraRadio::instance().getTxCount(),
                      LoraRadio::instance().getRxCount(),
                      LoraRadio::instance().getLastRssi(),
                      LoraRadio::instance().getPowerDbm(),
                      LoraRadio::instance().hasAntenna() ? "true" : "false",
                      LoraRadio::instance().isTxInhibited() ? "true" : "false",
                      LoraRadio::instance().getAmbientRssi(),
                      MeshEngine::instance().isGateway() ? "true" : "false",
                      MeshEngine::instance().getHopsToGateway(),
                      TimeSync::instance().getStratum(),
                      NodeConfig::instance().isTraveling() ? "true" : "false",
                      NodeConfig::instance().getSpeedKmh(),
                      NodeConfig::instance().getHeadingDeg(),
                      NodeConfig::instance().getAltM(),
                      NodeConfig::instance().getTripDistKm(),
                      NodeConfig::instance().getAttachedDevice(),
                      NodeConfig::instance().isAttached() ? "true" : "false");
    } else if (cmd.equalsIgnoreCase("wifi status") || cmd.equalsIgnoreCase("wifi")) {
        bool connected = (WiFi.status() == WL_CONNECTED);
        Serial.printf("{\"type\":\"wifi_status\",\"configured\":%s,\"ssid\":\"%s\",\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d}\n",
            NodeConfig::instance().hasWifiCreds() ? "true" : "false",
            NodeConfig::instance().getWifiSsid().c_str(),
            connected ? "true" : "false",
            connected ? WiFi.localIP().toString().c_str() : "",
            connected ? WiFi.RSSI() : 0);
    } else if (cmd.startsWith("wifi connect ")) {
        String args = cmd.substring(13);
        int spaceIdx = args.indexOf(' ');
        String ssid = (spaceIdx > 0) ? args.substring(0, spaceIdx) : args;
        String pass = (spaceIdx > 0) ? args.substring(spaceIdx + 1) : "";
        ssid.trim();
        pass.trim();
        NodeConfig::instance().setWifiCreds(ssid.c_str(), pass.c_str());
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        Serial.printf("{\"type\":\"ack\",\"action\":\"wifi_connecting\",\"ssid\":\"%s\"}\n", ssid.c_str());
    } else if (cmd.equalsIgnoreCase("wifi clear")) {
        NodeConfig::instance().clearWifiCreds();
        WiFi.disconnect(true);
        Serial.println("{\"type\":\"ack\",\"action\":\"wifi_cleared\"}");
    } else if (cmd.equalsIgnoreCase("screen off") || cmd.equalsIgnoreCase("display off") || cmd.equalsIgnoreCase("oled off")) {
        DisplayOled::instance().setScreenEnabled(false);
        Serial.println("{\"type\":\"ack\",\"screen\":\"off\",\"power\":false}");
    } else if (cmd.equalsIgnoreCase("screen on") || cmd.equalsIgnoreCase("display on") || cmd.equalsIgnoreCase("oled on")) {
        DisplayOled::instance().setScreenEnabled(true);
        Serial.println("{\"type\":\"ack\",\"screen\":\"on\",\"power\":true}");
    } else if (cmd.equalsIgnoreCase("screen toggle") || cmd.equalsIgnoreCase("display toggle") || cmd.equalsIgnoreCase("oled toggle")) {
        DisplayOled::instance().toggleScreen();
        bool en = DisplayOled::instance().isScreenEnabled();
        Serial.printf("{\"type\":\"ack\",\"screen\":\"%s\",\"power\":%s}\n", en ? "on" : "off", en ? "true" : "false");
    } else if (cmd.equalsIgnoreCase("screen") || cmd.equalsIgnoreCase("screen status") || cmd.equalsIgnoreCase("display status") || cmd.equalsIgnoreCase("oled status")) {
        Serial.printf("{\"type\":\"screen_status\",\"enabled\":%s,\"page\":%u,\"auto_cycle\":%s}\n",
            DisplayOled::instance().isScreenEnabled() ? "true" : "false",
            DisplayOled::instance().getCurrentPage(),
            DisplayOled::instance().isAutoCycle() ? "true" : "false");
    } else if (cmd.startsWith("screen dim ") || cmd.startsWith("contrast ")) {
        int val = cmd.substring(cmd.indexOf(' ') + 1).toInt();
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        DisplayOled::instance().setContrast((uint8_t)val);
        Serial.printf("{\"type\":\"ack\",\"action\":\"contrast_set\",\"value\":%d}\n", val);
    } else if (cmd.startsWith("page ")) {
        String pStr = cmd.substring(5);
        pStr.trim();
        if (pStr.equalsIgnoreCase("next")) {
            DisplayOled::instance().nextPage();
        } else if (pStr.equalsIgnoreCase("prev")) {
            DisplayOled::instance().prevPage();
        } else if (pStr.equalsIgnoreCase("cycle on")) {
            DisplayOled::instance().setAutoCycle(true);
        } else if (pStr.equalsIgnoreCase("cycle off")) {
            DisplayOled::instance().setAutoCycle(false);
        } else {
            uint8_t p = pStr.toInt();
            DisplayOled::instance().setPage(p);
            DisplayOled::instance().setAutoCycle(false); // stay on user-selected page
        }
        Serial.printf("{\"type\":\"ack\",\"page\":%u,\"auto_cycle\":%s}\n",
            DisplayOled::instance().getCurrentPage(),
            DisplayOled::instance().isAutoCycle() ? "true" : "false");
    } else if (cmd.equalsIgnoreCase("page") || cmd.equalsIgnoreCase("page next")) {
        DisplayOled::instance().nextPage();
        Serial.printf("{\"type\":\"ack\",\"page\":%u}\n", DisplayOled::instance().getCurrentPage());
    } else if (cmd.equalsIgnoreCase("reboot")) {
        Serial.println("{\"type\":\"ack\",\"action\":\"rebooting\"}");
        delay(100);
        ESP.restart();
    } else {
        Serial.printf("{\"type\":\"error\",\"message\":\"unknown command '%s'\"}\n", cmd.c_str());
    }
}

void BleScanner::processIncomingRx(const std::string& rxStr) {
    String cmd(rxStr.c_str());
    handleCommand(cmd);
}

// ── Setup & Loop ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);

    // 1. Initialize Heltec V3 Power Rails (Vext, OLED Reset, Battery ADC Ctrl)
    HeltecV3::initPower();

    // 2. Initialize NVS Configuration & Deterministic Reddit-Style Node Name
    NodeConfig::instance().begin();
    const char* nodeName = NodeConfig::instance().getName();

    // 3. Initialize Multi-LED status engine (Heltec V3 Onboard LED on GPIO 35)
    LedPatterns::instance().begin(HeltecV3::PIN_LED, false);

    Serial.printf("\n=========================================\n");
    Serial.printf("  BEACONFIX HELTEC V3 LORA + WIFI NODE   \n");
    Serial.printf("  Node Name: %s\n", nodeName);
    Serial.printf("=========================================\n");

    // 4. Initialize OLED 128x64 Status Display (U8g2)
    if (DisplayOled::instance().begin()) {
        Serial.println("{\"type\":\"init\",\"component\":\"display\",\"status\":\"oled_128x64_active\"}");
    }

    // 5. Initialize Battery Monitor for 3.7V vape Li-ion cell (Heltec V3 divider circuit)
    BatteryMonitor::instance().begin(HeltecV3::PIN_BATT_ADC, HeltecV3::BATT_DIVIDER_RATIO);

    // 6. Start Wi-Fi in AP or AP+STA mode
    if (NodeConfig::instance().hasWifiCreds()) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(nodeName);
        WiFi.begin(NodeConfig::instance().getWifiSsid().c_str(), NodeConfig::instance().getWifiPass().c_str());
        Serial.printf("{\"type\":\"init\",\"status\":\"wifi_ap_sta_ready\",\"ssid\":\"%s\",\"home_wifi\":\"%s\"}\n",
                      nodeName, NodeConfig::instance().getWifiSsid().c_str());
    } else {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(nodeName);
        Serial.printf("{\"type\":\"init\",\"status\":\"wifi_ap_ready\",\"ssid\":\"%s\",\"ip\":\"%s\"}\n",
                      nodeName, WiFi.softAPIP().toString().c_str());
    }
    esp_wifi_set_ps(WIFI_PS_NONE); // Disable Wi-Fi power saving so ESP-NOW frames are not delayed or dropped!

    // 7. Initialize Sub-ms Time Synchronization
    TimeSync::instance().begin();

    // 8. Initialize Multi-Hop Store & Forward Mesh Engine
    MeshEngine::instance().begin();
    MeshOtaEngine::instance().begin();

    // 9. Initialize Semtech SX1262 LoRa Radio (US915 band: 915.0 MHz, 250 kHz, SF7, CR 4/5)
    if (!LoraRadio::instance().begin(915.0f, 250.0f, 7, 5)) {
        Serial.println("{\"type\":\"warn\",\"component\":\"lora\",\"message\":\"sx1262_offline\"}");
    }

    // 10. Initialize Telemetry & UDP Broadcast
    Telemetry::instance().begin(47824);
    Telemetry::instance().enableUdp(true);
    MeshEngine::instance().registerHostDeliveryHook([](const char* json) {
        Telemetry::instance().broadcastUdp(json);
    });
    g_cmdUdp.begin(47825);

    // 11. Initialize Web Server (Port 80)
    OtaUpdater::instance().begin(80);

    // 12. Initialize BLE Server and Scanner
    if (!BleScanner::instance().begin(nodeName)) {
        Serial.println("{\"type\":\"error\",\"component\":\"ble\",\"message\":\"init_failed\"}");
    }

    // 13. Initialize Wi-Fi Promiscuous Monitor Mode
    if (!WifiMonitor::instance().begin()) {
        Serial.println("{\"type\":\"error\",\"component\":\"wifi\",\"message\":\"monitor_init_failed\"}");
        LedPatterns::instance().reportBroken(PATTERN_BROKEN_RADIO);
    } else {
        Serial.println("{\"type\":\"init\",\"status\":\"monitor_active\",\"mode\":\"promiscuous\"}");
    }

    // 14. Initialize Hardware GPS Receiver (UART 9600 baud on GPIO 47/48)
    GpsReceiver::instance().begin(9600);
}

void loop() {
    unsigned long now = millis();

    // 0. Process Hardware GPS Receiver UART stream
    GpsReceiver::instance().update();

    // Push 1-second Wi-Fi traffic PPS sample into OLED rolling histogram
    static unsigned long g_lastHistPush = 0;
    if (now - g_lastHistPush >= 1000) {
        g_lastHistPush = now;
        DisplayOled::instance().pushTrafficSample(WifiMonitor::instance().getPps());
    }

    // Process incoming UDP command datagrams on port 47825
    int cmdPacketSize = g_cmdUdp.parsePacket();
    if (cmdPacketSize > 0) {
        char cmdBuf[256];
        int len = g_cmdUdp.read(cmdBuf, sizeof(cmdBuf) - 1);
        if (len > 0) {
            cmdBuf[len] = '\0';
            if (cmdBuf[0] == '{') {
                if (strstr(cmdBuf, "\"alpr\"") || strstr(cmdBuf, "\"camera_pass\"") ||
                    strstr(cmdBuf, "\"flock_pass\"") || strstr(cmdBuf, "\"alpr_alert\"")) {
                    DisplayOled::instance().triggerAlprAlert("Flock Safety", "Falcon Flex", 42.0f, 95, 1);
                    LedPatterns::instance().triggerDeauthAlert();
                }
            } else {
                handleCommand(String(cmdBuf));
            }
        }
    }

    // 1. Update OLED Display
    DisplayOled::instance().update(
        LoraRadio::instance().getTxCount(),
        LoraRadio::instance().getRxCount(),
        LoraRadio::instance().getLastRssi(),
        LoraRadio::instance().getLastSnr(),
        LoraRadio::instance().getFreq()
    );

    // 2. Update LoRa Radio (processes async RX packets and bridges into mesh)
    LoraRadio::instance().update();

    // 3. Wi-Fi Station connection monitoring & NTP time sync
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

    // 4. Update LED patterns (non-blocking)
    LedPatterns::instance().update();

    // 5. Update Battery Monitor
    BatteryMonitor::instance().update();

    // 6. Update Web Server
    OtaUpdater::instance().update();

    // 7. Update Multi-Hop Mesh & Store-and-Forward Queue
    MeshEngine::instance().update();
    MeshOtaEngine::instance().update();

    // 8. Update Wi-Fi Monitor (channel hopping & PPS calculation)
    WifiMonitor::instance().update();

    // 9. Process captured Wi-Fi frames
    DetectedFrame frame;
    int framesProcessed = 0;
    static unsigned long s_lastWifiAlprAlert = 0;
    while (WifiMonitor::instance().getNextFrame(&frame) && framesProcessed < 15) {
        framesProcessed++;

        // Autonomous RF ALPR Signature Detection
        if (frame.category == CAT_PROBE_REQ || frame.category == CAT_BEACON) {
            bool isAlprRf = isFlockRfDevice(frame.ssid, frame.mac);

            if (isAlprRf && (now - s_lastWifiAlprAlert > 15000)) {
                s_lastWifiAlprAlert = now;
                float estDist = pow(10.0f, (-45.0f - (float)frame.rssi) / (10.0f * 2.5f));
                if (estDist < 3.0f) estDist = 3.0f;
                if (estDist > 120.0f) estDist = 120.0f;

                DisplayOled::instance().triggerAlprAlert("Flock (RF)", frame.ssid[0] ? frame.ssid : "Falcon AP", estDist, 98, 0);
                LedPatterns::instance().triggerDeauthAlert();

                char macStr[18];
                WifiMonitor::formatMac(frame.mac, macStr);
                char alertJson[192];
                snprintf(alertJson, sizeof(alertJson),
                         "{\"type\":\"alpr_rf_detect\",\"source\":\"wifi\",\"mac\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"dist_m\":%.1f}",
                         macStr, frame.ssid, frame.rssi, estDist);
                Serial.println(alertJson);
                BleScanner::instance().sendTelemetry(alertJson);
                MeshEngine::instance().broadcastAlprAlert(alertJson);
            }
        }

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

    // 10. Process captured BLE tags
    DetectedBleTag tag;
    int tagsProcessed = 0;
    static unsigned long s_lastBleAlprAlert = 0;
    while (BleScanner::instance().getNextTag(&tag) && tagsProcessed < 5) {
        tagsProcessed++;
        Telemetry::instance().emitBleTag(tag);

        if (strcmp(tag.kind, "flock_ble") == 0 && (now - s_lastBleAlprAlert > 15000)) {
            s_lastBleAlprAlert = now;
            float estDist = pow(10.0f, (-45.0f - (float)tag.rssi) / (10.0f * 2.5f));
            if (estDist < 3.0f) estDist = 3.0f;
            if (estDist > 120.0f) estDist = 120.0f;

            DisplayOled::instance().triggerAlprAlert("Flock (BLE)", tag.name[0] ? tag.name : "Falcon Beacon", estDist, 98, 0);
            LedPatterns::instance().triggerDeauthAlert();

            char alertJson[192];
            snprintf(alertJson, sizeof(alertJson),
                     "{\"type\":\"alpr_rf_detect\",\"source\":\"ble\",\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"dist_m\":%.1f}",
                     tag.mac, tag.name, tag.rssi, estDist);
            Serial.println(alertJson);
            BleScanner::instance().sendTelemetry(alertJson);
            MeshEngine::instance().broadcastAlprAlert(alertJson);
        }
    }

    // 11. Read Serial command input
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

    // 12. Periodic Status Telemetry Heartbeat (every 5 seconds)
    if (now - g_lastStatusTime >= 5000) {
        g_lastStatusTime = now;
        uint32_t freeHeap = ESP.getFreeHeap();

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

    delay(2);
}
