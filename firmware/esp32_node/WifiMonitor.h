#pragma once
#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <WiFi.h>
#include "LedPatterns.h"
#include "MeshEngine.h"

// 802.11 Management frame header
typedef struct {
    uint16_t fctl;
    uint16_t duration;
    uint8_t da[6];
    uint8_t sa[6];
    uint8_t bssid[6];
    uint16_t seqctl;
    uint8_t payload[0];
} __attribute__((packed)) WifiMgmtHdr;

enum FrameCategory {
    CAT_PROBE_REQ = 1,
    CAT_BEACON = 2,
    CAT_DEAUTH = 3,
    CAT_DISASSOC = 4,
    CAT_OTHER_MGMT = 5,
    CAT_DATA = 6
};

struct DetectedFrame {
    uint8_t category;
    int8_t rssi;
    uint8_t channel;
    uint16_t seq;
    uint8_t mac[6];
    uint8_t targetMac[6];
    char ssid[34];
    uint16_t reasonCode;
    uint32_t timestampMs;
};

class WifiMonitor {
public:
    static WifiMonitor& instance() {
        static WifiMonitor inst;
        return inst;
    }

    bool begin() {
        m_frameQueue = xQueueCreate(64, sizeof(DetectedFrame));
        if (!m_frameQueue) {
            LedPatterns::instance().reportBroken(PATTERN_BROKEN_HEAP);
            return false;
        }

        wifi_promiscuous_filter_t filt = {
            .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA
        };
        esp_wifi_set_promiscuous_filter(&filt);
        esp_wifi_set_promiscuous_rx_cb(&WifiMonitor::promiscuousRxCallback);
        
        esp_err_t err = esp_wifi_set_promiscuous(true);
        if (err != ESP_OK) {
            LedPatterns::instance().reportBroken(PATTERN_BROKEN_RADIO);
            return false;
        }

        setChannel(m_currentChannel);
        m_running = true;
        return true;
    }

    void update() {
        unsigned long now = millis();

        // Channel hopping logic with anti-phase jitter
        if (m_autoHop && (now - m_lastHopTime >= m_hopIntervalMs)) {
            m_lastHopTime = now;
            m_hopIntervalMs = 120 + (esp_random() % 60); // 120ms - 180ms pseudo-random jitter
            m_currentChannel++;
            if (m_currentChannel > 13) m_currentChannel = 1;
            esp_wifi_set_channel(m_currentChannel, WIFI_SECOND_CHAN_NONE);
        }

        // PPS calculation
        if (now - m_lastPpsTime >= 1000) {
            m_pps = m_packetsThisSec;
            m_packetsThisSec = 0;
            m_lastPpsTime = now;
        }
    }

    bool getNextFrame(DetectedFrame* outFrame) {
        if (!m_frameQueue) return false;
        return xQueueReceive(m_frameQueue, outFrame, 0) == pdTRUE;
    }

    void setChannel(uint8_t ch) {
        if (ch == 0) {
            m_autoHop = true;
        } else {
            m_autoHop = false;
            m_currentChannel = (ch > 14) ? 14 : ch;
            esp_wifi_set_channel(m_currentChannel, WIFI_SECOND_CHAN_NONE);
        }
    }

    uint8_t getChannel() const { return m_currentChannel; }
    bool isAutoHop() const { return m_autoHop; }
    void setHopInterval(uint16_t intervalMs) { m_hopIntervalMs = intervalMs; }
    uint16_t getHopInterval() const { return m_hopIntervalMs; }

    uint32_t getTotalFrames() const { return m_totalFrames; }
    uint32_t getProbeCount() const { return m_probeCount; }
    uint32_t getBeaconCount() const { return m_beaconCount; }
    uint32_t getDeauthCount() const { return m_deauthCount; }
    uint32_t getPps() const { return m_pps; }

    static void formatMac(const uint8_t* mac, char* outStr) {
        snprintf(outStr, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

private:
    WifiMonitor() : m_running(false), m_autoHop(true), m_currentChannel(1),
                    m_hopIntervalMs(150), m_lastHopTime(0), m_frameQueue(nullptr),
                    m_totalFrames(0), m_probeCount(0), m_beaconCount(0),
                    m_deauthCount(0), m_packetsThisSec(0), m_pps(0), m_lastPpsTime(0) {}

    static void promiscuousRxCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
        WifiMonitor::instance().handlePacket((wifi_promiscuous_pkt_t*)buf, type);
    }

    void handlePacket(wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type) {
        m_totalFrames++;
        m_packetsThisSec++;

        const uint8_t* payload = pkt->payload;
        const int len = pkt->rx_ctrl.sig_len;
        if (len < 24) return;

        const WifiMgmtHdr* hdr = (const WifiMgmtHdr*)payload;

        // Fast-path: Check for BeaconFix Mesh Action frames / ESP-NOW packets across MGMT & DATA frames
        if (len >= 24 + 10) {
            const uint8_t* p = payload + 24;
            int maxSearch = (len - 24 - 10 < 32) ? (len - 24 - 10) : 32;
            for (int off = 0; off <= maxSearch; off++) {
                if (p[off] == MESH_MAGIC_0 && p[off + 1] == MESH_MAGIC_1) {
                    MeshEngine::instance().handleRawFrame(hdr->sa, p + off, len - 24 - off, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel);
                    return;
                }
            }
        }

        if (type != WIFI_PKT_MGMT) {
            return;
        }

        uint16_t fctl = hdr->fctl;
        uint8_t frameType = (fctl >> 2) & 0x03;
        uint8_t subType = (fctl >> 4) & 0x0F;

        if (frameType != 0) return; // Only management frames

        DetectedFrame frame;
        memset(&frame, 0, sizeof(frame));
        frame.rssi = pkt->rx_ctrl.rssi;
        frame.channel = pkt->rx_ctrl.channel;
        frame.seq = hdr->seqctl >> 4;
        frame.timestampMs = millis();

        bool notifyLed = false;
        bool isAlert = false;

        switch (subType) {
            case 0x04: // Probe Request
                frame.category = CAT_PROBE_REQ;
                memcpy(frame.mac, hdr->sa, 6);
                m_probeCount++;
                notifyLed = true;

                // Extract SSID from tagged elements starting at offset 24
                if (len > 26) {
                    const uint8_t* tags = payload + 24;
                    int tagIdx = 0;
                    int remaining = len - 24;
                    while (remaining >= 2) {
                        uint8_t tagNum = tags[tagIdx];
                        uint8_t tagLen = tags[tagIdx + 1];
                        if (remaining < 2 + tagLen) break;

                        if (tagNum == 0) { // SSID
                            int copyLen = (tagLen < 33) ? tagLen : 32;
                            memcpy(frame.ssid, tags + tagIdx + 2, copyLen);
                            frame.ssid[copyLen] = '\0';
                            break;
                        }
                        tagIdx += 2 + tagLen;
                        remaining -= (2 + tagLen);
                    }
                }
                break;

            case 0x08: // Beacon
                frame.category = CAT_BEACON;
                memcpy(frame.mac, hdr->bssid, 6);
                m_beaconCount++;

                // Beacons have 12 bytes fixed params after 24 byte header
                if (len > 38) {
                    const uint8_t* tags = payload + 36;
                    int tagIdx = 0;
                    int remaining = len - 36;
                    while (remaining >= 2) {
                        uint8_t tagNum = tags[tagIdx];
                        uint8_t tagLen = tags[tagIdx + 1];
                        if (remaining < 2 + tagLen) break;

                        if (tagNum == 0) { // SSID
                            int copyLen = (tagLen < 33) ? tagLen : 32;
                            memcpy(frame.ssid, tags + tagIdx + 2, copyLen);
                            frame.ssid[copyLen] = '\0';
                            break;
                        }
                        tagIdx += 2 + tagLen;
                        remaining -= (2 + tagLen);
                    }
                }
                break;

            case 0x0C: // Deauthentication
            case 0x0A: // Disassociation
                frame.category = (subType == 0x0C) ? CAT_DEAUTH : CAT_DISASSOC;
                memcpy(frame.mac, hdr->sa, 6);
                memcpy(frame.targetMac, hdr->da, 6);
                if (len >= 26) {
                    frame.reasonCode = payload[24] | (payload[25] << 8);
                }
                m_deauthCount++;
                isAlert = true;
                break;

            default:
                return;
        }

        if (m_frameQueue) {
            xQueueSendFromISR(m_frameQueue, &frame, NULL);
        }

        if (isAlert) {
            LedPatterns::instance().triggerDeauthAlert();
        } else if (notifyLed) {
            LedPatterns::instance().triggerDetection(false);
        }
    }

    bool m_running;
    bool m_autoHop;
    uint8_t m_currentChannel;
    uint16_t m_hopIntervalMs;
    unsigned long m_lastHopTime;
    QueueHandle_t m_frameQueue;

    volatile uint32_t m_totalFrames;
    volatile uint32_t m_probeCount;
    volatile uint32_t m_beaconCount;
    volatile uint32_t m_deauthCount;
    volatile uint32_t m_packetsThisSec;
    volatile uint32_t m_pps;
    unsigned long m_lastPpsTime;
};
