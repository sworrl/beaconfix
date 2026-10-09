#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "WifiMonitor.h"
#include "BleScanner.h"
#include "TimeSync.h"
#include "MeshEngine.h"
#include "NodeConfig.h"
#include "StoreForward.h"

#include "LoraRadio.h"

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

    // A node following a phone that's out of reach (no BLE link, no WiFi) keeps its detections on flash until the
    // phone is back, instead of sending them nowhere
    bool storingForPhone() const {
        return StoreForward::instance().ok() && NodeConfig::instance().isAttached() &&
               !BleScanner::instance().isConnected() && WiFi.status() != WL_CONNECTED;
    }

    void broadcastJson(const char* jsonStr, bool toMesh = true, bool detection = false) {
        if (detection && storingForPhone()) {
            StoreForward::instance().store(jsonStr);
            toMesh = false;   // the mesh queue would hand the same line to the phone a second time, cut short
        }
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
        broadcastJson(buf, true, true);
    }

    void emitBeacon(const DetectedFrame& f) {
        // An AP beacons ~10x a second; one report per AP every few seconds is
        // plenty, and over WiFi every frame flooded the network the node is on.
        if (!beaconDue(f)) return;
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
        broadcastJson(buf, false, true);
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
        broadcastJson(buf, true, true);
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
        broadcastJson(buf, true, true);
    }

    void emitBattery(uint32_t mv, uint8_t pct, const char* state, bool charging,
                     bool trained = false, uint8_t trainPct = 0, float cycles = 0.0f) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"battery\",\"mv\":%lu,\"pct\":%u,\"state\":\"%s\",\"charging\":%s,\"batt_trained\":%s,\"batt_train_pct\":%u,\"batt_cycles\":%.2f}",
            mv, pct, state, charging ? "true" : "false",
            trained ? "true" : "false", trainPct, cycles);
        broadcastJson(buf);
    }

    void emitStatus(const char* nodeName, uint32_t uptimeS, uint32_t freeHeap,
                    uint8_t ch, bool hopping, uint32_t pps, uint32_t total,
                    uint32_t probes, uint32_t beacons, uint32_t deauths, int bleClients,
                    uint32_t battMv = 3800, uint8_t battPct = 50, const char* battState = "halfway", bool charging = false,
                    bool battTrained = false, uint8_t battTrainPct = 0, float battCycles = 0.0f) {
        char buf[768];   // GPS fix + a long attached-device name runs past 600
        bool hasAnt = LoraRadio::instance().hasAntenna();
        bool txInhib = LoraRadio::instance().isTxInhibited();
        int ambRssi = LoraRadio::instance().getAmbientRssi();
        bool traveling = NodeConfig::instance().isTraveling();
        float speedKmh = NodeConfig::instance().getSpeedKmh();

        if (NodeConfig::instance().hasGpsFix()) {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"status\",\"version\":\"%s\",\"hardware\":\"Heltec V3\",\"node\":\"%s\",\"uptime\":%lu,\"heap\":%lu,\"ch\":%u,\"hop\":%s,\"pps\":%lu,\"total\":%lu,\"probes\":%lu,\"beacons\":%lu,\"deauths\":%lu,\"ble\":%d,\"batt_mv\":%lu,\"batt_pct\":%u,\"batt_state\":\"%s\",\"charging\":%s,\"batt_trained\":%s,\"batt_train_pct\":%u,\"batt_cycles\":%.2f,\"antenna_detected\":%s,\"tx_inhibited\":%s,\"ambient_rssi\":%d,\"traveling\":%s,\"speed_kmh\":%.1f,\"attached_dev\":\"%s\",\"following\":%s,\"has_gps\":true,\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.1f}",
                BEACONFIX_FW_VERSION, nodeName, uptimeS, freeHeap, ch, hopping ? "true" : "false", pps, total, probes, beacons, deauths, bleClients,
                battMv, battPct, battState, charging ? "true" : "false",
                battTrained ? "true" : "false", battTrainPct, battCycles,
                hasAnt ? "true" : "false", txInhib ? "true" : "false", ambRssi,
                traveling ? "true" : "false", speedKmh,
                NodeConfig::instance().getAttachedDevice(),
                NodeConfig::instance().isAttached() ? "true" : "false",
                NodeConfig::instance().getLat(), NodeConfig::instance().getLon(), NodeConfig::instance().getAccM());
        } else {
            snprintf(buf, sizeof(buf),
                "{\"type\":\"status\",\"version\":\"%s\",\"hardware\":\"Heltec V3\",\"node\":\"%s\",\"uptime\":%lu,\"heap\":%lu,\"ch\":%u,\"hop\":%s,\"pps\":%lu,\"total\":%lu,\"probes\":%lu,\"beacons\":%lu,\"deauths\":%lu,\"ble\":%d,\"batt_mv\":%lu,\"batt_pct\":%u,\"batt_state\":\"%s\",\"charging\":%s,\"batt_trained\":%s,\"batt_train_pct\":%u,\"batt_cycles\":%.2f,\"antenna_detected\":%s,\"tx_inhibited\":%s,\"ambient_rssi\":%d,\"traveling\":%s,\"speed_kmh\":%.1f,\"attached_dev\":\"%s\",\"following\":%s,\"has_gps\":false}",
                BEACONFIX_FW_VERSION, nodeName, uptimeS, freeHeap, ch, hopping ? "true" : "false", pps, total, probes, beacons, deauths, bleClients,
                battMv, battPct, battState, charging ? "true" : "false",
                battTrained ? "true" : "false", battTrainPct, battCycles,
                hasAnt ? "true" : "false", txInhib ? "true" : "false", ambRssi,
                traveling ? "true" : "false", speedKmh,
                NodeConfig::instance().getAttachedDevice(),
                NodeConfig::instance().isAttached() ? "true" : "false");
        }
        broadcastJson(buf);
    }

    // Command replies (SPRINTF) arrive in fragments; put each finished line on the LAN as one datagram
    void udpReply(const char* s) {
        if (!m_udpEnabled || WiFi.status() != WL_CONNECTED) return;
        for (; *s; ++s) {
            if (*s == '\n' || m_replyLen >= sizeof(m_replyBuf) - 1) {
                m_replyBuf[m_replyLen] = 0;
                if (m_replyLen) broadcastUdp(m_replyBuf);
                m_replyLen = 0;
                if (*s == '\n') continue;
            }
            m_replyBuf[m_replyLen++] = *s;
        }
    }

    // Commands over WiFi on UDP 47825, as the desktop bridge sends them ("@NodeName cmd" or plain "cmd" for every
    // node). Only from the network the node joined as a station: the soft-AP is open, so anyone in range could
    // join it and reconfigure the node. Returns false when nothing is waiting; cmd is empty when a packet was dropped.
    bool popUdpCommand(String& cmd) {
        cmd = "";
        if (!m_udpEnabled || WiFi.status() != WL_CONNECTED) return false;
        if (!m_cmdOpen) m_cmdOpen = m_cmdUdp.begin(47825);
        if (!m_cmdOpen || m_cmdUdp.parsePacket() <= 0) return false;
        char buf[256];
        int n = m_cmdUdp.read((uint8_t*)buf, sizeof(buf) - 1);
        uint32_t from = m_cmdUdp.remoteIP(), ip = WiFi.localIP(), mask = WiFi.subnetMask();
        if (n <= 0 || (from & mask) != (ip & mask)) return true;
        buf[n] = 0;
        String line(buf);
        int nl = line.indexOf('\n');
        if (nl >= 0) line = line.substring(0, nl);
        line.trim();
        if (line.startsWith("@")) {
            int sp = line.indexOf(' ');
            if (sp < 0 || !line.substring(1, sp).equalsIgnoreCase(NodeConfig::instance().getName())) return true;
            line = line.substring(sp + 1);
            line.trim();
        }
        cmd = line;
        return true;
    }

private:
    Telemetry() : m_udpPort(47824), m_udpEnabled(false) {}
    uint16_t m_udpPort;
    bool m_udpEnabled;
    WiFiUDP m_udp;
    WiFiUDP m_cmdUdp;
    bool m_cmdOpen = false;

    struct BeaconSeen { uint8_t mac[6]; uint32_t ms; int8_t rssi; bool used; };
    BeaconSeen m_beaconSeen[48] = {};

    bool beaconDue(const DetectedFrame& f) {
        const uint32_t now = millis();
        BeaconSeen* slot = nullptr;
        for (auto& b : m_beaconSeen) {
            if (b.used && memcmp(b.mac, f.mac, 6) == 0) { slot = &b; break; }
        }
        if (slot) {
            const uint32_t every = storingForPhone() ? 60000 : 3000;   // flash is small; once a minute per AP
            if (now - slot->ms < every && abs(f.rssi - slot->rssi) < 8) return false;
        } else {
            slot = &m_beaconSeen[0];
            for (auto& b : m_beaconSeen) {
                if (!b.used) { slot = &b; break; }
                if (b.ms - slot->ms > 0x80000000u) slot = &b;   // oldest, wrap-safe
            }
            memcpy(slot->mac, f.mac, 6);
            slot->used = true;
        }
        slot->ms = now;
        slot->rssi = f.rssi;
        return true;
    }
    char m_replyBuf[768];
    size_t m_replyLen = 0;
};
