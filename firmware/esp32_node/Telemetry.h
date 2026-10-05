#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>
#include "WifiMonitor.h"
#include "BleScanner.h"
#include "TimeSync.h"
#include "MeshEngine.h"
#include "NodeConfig.h"


class Telemetry {
public:
    static Telemetry& instance() {
        static Telemetry inst;
        return inst;
    }

    void begin(uint16_t udpPort = 47824) {
        m_udpPort = udpPort;
        m_udpEnabled = false;
    }

    void enableUdp(bool enable) {
        m_udpEnabled = enable;
        if (m_udpEnabled) {
            m_udp.begin(m_udpPort);
        } else {
            m_udp.stop();
        }
    }

    void broadcastUdp(const char* jsonStr) {
        if (m_udpEnabled) {
            m_udp.beginPacket(IPAddress(255, 255, 255, 255), m_udpPort);
            m_udp.write((const uint8_t*)jsonStr, strlen(jsonStr));
            m_udp.write('\n');
            m_udp.endPacket();
        }
    }

    void broadcastJson(const char* jsonStr, bool toMesh = true) {
        // 1. USB Serial
        Serial.println(jsonStr);

        // 2. BLE GATT notification
        BleScanner::instance().sendTelemetry(jsonStr);

        // 3. UDP Broadcast (if network is active)
        broadcastUdp(jsonStr);

        // 4. Mesh Engine multi-hop forwarding & store-and-forward queue
        if (toMesh) {
            MeshEngine::instance().sendTelemetry(jsonStr);
        }
    }

    void emitProbe(const DetectedFrame& f) {
        char macStr[18];
        WifiMonitor::formatMac(f.mac, macStr);

        char buf[320];
        if (NodeConfig::instance().hasGpsFix()) {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"probe\",\"mac\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%u,\"seq\":%u,\"ts\":%lu,\"ts_us\":%llu,\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.1f}",
                macStr, f.ssid, f.rssi, f.channel, f.seq, f.timestampMs, (unsigned long long)TimeSync::instance().getNowUs(),
                NodeConfig::instance().getLat(), NodeConfig::instance().getLon(), NodeConfig::instance().getAccM());
        } else {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"probe\",\"mac\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%u,\"seq\":%u,\"ts\":%lu,\"ts_us\":%llu}",
                macStr, f.ssid, f.rssi, f.channel, f.seq, f.timestampMs, (unsigned long long)TimeSync::instance().getNowUs());
        }
        broadcastJson(buf);
    }

    void emitBeacon(const DetectedFrame& f) {
        char macStr[18];
        WifiMonitor::formatMac(f.mac, macStr);

        char buf[320];
        if (NodeConfig::instance().hasGpsFix()) {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"beacon\",\"bssid\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%u,\"ts\":%lu,\"ts_us\":%llu,\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.1f}",
                macStr, f.ssid, f.rssi, f.channel, f.timestampMs, (unsigned long long)TimeSync::instance().getNowUs(),
                NodeConfig::instance().getLat(), NodeConfig::instance().getLon(), NodeConfig::instance().getAccM());
        } else {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"beacon\",\"bssid\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%u,\"ts\":%lu,\"ts_us\":%llu}",
                macStr, f.ssid, f.rssi, f.channel, f.timestampMs, (unsigned long long)TimeSync::instance().getNowUs());
        }
        broadcastJson(buf, false);
    }

    void emitDeauth(const DetectedFrame& f) {
        char srcMac[18], dstMac[18];
        WifiMonitor::formatMac(f.mac, srcMac);
        WifiMonitor::formatMac(f.targetMac, dstMac);

        char buf[320];
        if (NodeConfig::instance().hasGpsFix()) {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"alert\",\"event\":\"%s\",\"sa\":\"%s\",\"da\":\"%s\",\"reason\":%u,\"rssi\":%d,\"ch\":%u,\"ts\":%lu,\"ts_us\":%llu,\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.1f}",
                (f.category == CAT_DEAUTH) ? "deauth" : "disassoc",
                srcMac, dstMac, f.reasonCode, f.rssi, f.channel, f.timestampMs, (unsigned long long)TimeSync::instance().getNowUs(),
                NodeConfig::instance().getLat(), NodeConfig::instance().getLon(), NodeConfig::instance().getAccM());
        } else {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"alert\",\"event\":\"%s\",\"sa\":\"%s\",\"da\":\"%s\",\"reason\":%u,\"rssi\":%d,\"ch\":%u,\"ts\":%lu,\"ts_us\":%llu}",
                (f.category == CAT_DEAUTH) ? "deauth" : "disassoc",
                srcMac, dstMac, f.reasonCode, f.rssi, f.channel, f.timestampMs, (unsigned long long)TimeSync::instance().getNowUs());
        }
        broadcastJson(buf);
    }

    void emitBleTag(const DetectedBleTag& t) {
        char buf[320];
        if (NodeConfig::instance().hasGpsFix()) {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"ble_tracker\",\"kind\":\"%s\",\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"payload\":\"%s\",\"ts\":%lu,\"ts_us\":%llu,\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.1f}",
                t.kind, t.mac, t.name, t.rssi, t.payloadHex, t.timestampMs, (unsigned long long)TimeSync::instance().getNowUs(),
                NodeConfig::instance().getLat(), NodeConfig::instance().getLon(), NodeConfig::instance().getAccM());
        } else {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"ble_tracker\",\"kind\":\"%s\",\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"payload\":\"%s\",\"ts\":%lu,\"ts_us\":%llu}",
                t.kind, t.mac, t.name, t.rssi, t.payloadHex, t.timestampMs, (unsigned long long)TimeSync::instance().getNowUs());
        }
        broadcastJson(buf);
    }

    void emitBattery(uint32_t mv, uint8_t pct, const char* state, bool charging) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"battery\",\"mv\":%lu,\"pct\":%u,\"state\":\"%s\",\"charging\":%s}",
            mv, pct, state, charging ? "true" : "false");
        broadcastJson(buf);
    }

    void emitStatus(const char* nodeName, uint32_t uptimeS, uint32_t freeHeap,
                    uint8_t ch, bool hopping, uint32_t pps, uint32_t total,
                    uint32_t probes, uint32_t beacons, uint32_t deauths, int bleClients,
                    uint32_t battMv = 3800, uint8_t battPct = 50, const char* battState = "halfway", bool charging = false) {
        char buf[512];
        bool hasAnt = false;
        bool txInhib = true;
        int ambRssi = 0;
        bool traveling = NodeConfig::instance().isTraveling();
        float speedKmh = NodeConfig::instance().getSpeedKmh();

        if (NodeConfig::instance().hasGpsFix()) {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"status\",\"version\":\"%s\",\"node\":\"%s\",\"mac\":\"%s\",\"uptime\":%lu,\"heap\":%lu,\"ch\":%u,\"hop\":%s,\"pps\":%lu,\"total\":%lu,\"probes\":%lu,\"beacons\":%lu,\"deauths\":%lu,\"ble\":%d,\"batt_mv\":%lu,\"batt_pct\":%u,\"batt_state\":\"%s\",\"charging\":%s,\"antenna_detected\":%s,\"tx_inhibited\":%s,\"ambient_rssi\":%d,\"traveling\":%s,\"speed_kmh\":%.1f,\"attached_dev\":\"%s\",\"following\":%s,\"has_gps\":true,\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.1f}",
                BEACONFIX_FW_VERSION, nodeName, WiFi.macAddress().c_str(), uptimeS, freeHeap, ch, hopping ? "true" : "false", pps, total, probes, beacons, deauths, bleClients,
                battMv, battPct, battState, charging ? "true" : "false",
                hasAnt ? "true" : "false", txInhib ? "true" : "false", ambRssi,
                traveling ? "true" : "false", speedKmh,
                NodeConfig::instance().getAttachedDevice(),
                NodeConfig::instance().isAttached() ? "true" : "false",
                NodeConfig::instance().getLat(), NodeConfig::instance().getLon(), NodeConfig::instance().getAccM());
        } else {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"status\",\"version\":\"%s\",\"node\":\"%s\",\"mac\":\"%s\",\"uptime\":%lu,\"heap\":%lu,\"ch\":%u,\"hop\":%s,\"pps\":%lu,\"total\":%lu,\"probes\":%lu,\"beacons\":%lu,\"deauths\":%lu,\"ble\":%d,\"batt_mv\":%lu,\"batt_pct\":%u,\"batt_state\":\"%s\",\"charging\":%s,\"antenna_detected\":%s,\"tx_inhibited\":%s,\"ambient_rssi\":%d,\"traveling\":%s,\"speed_kmh\":%.1f,\"attached_dev\":\"%s\",\"following\":%s,\"has_gps\":false}",
                BEACONFIX_FW_VERSION, nodeName, WiFi.macAddress().c_str(), uptimeS, freeHeap, ch, hopping ? "true" : "false", pps, total, probes, beacons, deauths, bleClients,
                battMv, battPct, battState, charging ? "true" : "false",
                hasAnt ? "true" : "false", txInhib ? "true" : "false", ambRssi,
                traveling ? "true" : "false", speedKmh,
                NodeConfig::instance().getAttachedDevice(),
                NodeConfig::instance().isAttached() ? "true" : "false");
        }
        broadcastJson(buf);
    }

private:
    Telemetry() : m_udpPort(47824), m_udpEnabled(false) {}
    uint16_t m_udpPort;
    bool m_udpEnabled;
    WiFiUDP m_udp;
};
