#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "TimeSync.h"
#include "NodeConfig.h"
#include "BleScanner.h"
#include "BatteryMonitor.h"

#define MESH_MAGIC_0 0xBF
#define MESH_MAGIC_1 0x4D
#define MESH_MAX_PAYLOAD 200
#define MESH_DEDUP_SIZE 128
#define MESH_MAX_NEIGHBORS 16
#define MESH_MAX_QUEUE 120

enum MeshMsgType : uint8_t {
    MESH_MSG_TELEMETRY       = 0x01,
    MESH_MSG_TIME_REQ        = 0x02,
    MESH_MSG_TIME_RESP       = 0x03,
    MESH_MSG_ANNOUNCE        = 0x04,
    MESH_MSG_ACK             = 0x05,
    MESH_MSG_ALPR_ALERT      = 0x06,
    MESH_MSG_OTA_START       = 0x10,
    MESH_MSG_OTA_CHUNK       = 0x11,
    MESH_MSG_OTA_STATUS_REQ  = 0x12,
    MESH_MSG_OTA_STATUS_RESP = 0x13,
    MESH_MSG_OTA_ABORT       = 0x14
};

#define OTA_CHUNK_PAYLOAD_MAX 192
#define OTA_WINDOW_SIZE 4

#pragma pack(push, 1)
struct MeshOtaStartPayload {
    uint32_t totalBytes;       // Total size of binary
    uint16_t chunkSize;        // e.g. 192 bytes
    uint16_t totalChunks;      // Total chunks
    uint8_t  sha256[32];       // SHA-256 of complete binary
    char     version[16];      // Firmware version (e.g. "3.10.0")
    uint8_t  hardwareType;     // 1 = Heltec V3 ESP32-S3, 2 = Generic ESP32
    uint8_t  channel;          // Wi-Fi channel (1-14)
    uint8_t  sigLen;           // Length of ECDSA signature
    uint8_t  sig[72];          // ECDSA P-256 DER signature
};

struct MeshOtaChunkPayload {
    uint16_t chunkIndex;       // 0 to totalChunks - 1
    uint16_t dataLen;          // Payload length (<= 192)
    uint8_t  data[OTA_CHUNK_PAYLOAD_MAX]; // Raw chunk binary
};

struct MeshOtaStatusReqPayload {
    uint8_t targetMac[6];
};

struct MeshOtaStatusRespPayload {
    uint8_t  state;            // 0=IDLE, 1=RECEIVING, 2=VERIFYING, 3=SUCCESS, 4=FAILED
    uint16_t nextExpectedChunk;// Next chunk index needed
    uint16_t totalChunks;      // Total chunks expected
    uint8_t  percent;          // 0 - 100%
    uint8_t  errorCode;        // 0=NONE, 1=FLASH_BEGIN, 2=FLASH_WRITE, 3=SIG_FAIL, 4=TIMEOUT
};

struct MeshOtaAbortPayload {
    char reason[32];
};
#pragma pack(pop)


#pragma pack(push, 1)
struct MeshPacketHdr {
    uint8_t magic[2];      // 0xBF, 0x4D
    uint8_t version;       // 1
    uint8_t msgType;       // MeshMsgType
    uint32_t msgId;        // Unique packet ID
    uint8_t srcMac[6];     // Originating node MAC
    uint8_t dstMac[6];     // Final destination (FF:FF:FF:FF:FF:FF = any gateway)
    uint8_t prevHopMac[6]; // Last hop transmitter MAC
    uint8_t hopCount;      // Hop count (starts 0, increments per forward)
    uint8_t ttl;           // Time to live (starts 8, decrements per forward)
    uint64_t timestampUs;  // Synchronized UTC/network epoch in microseconds
    uint16_t payloadLen;   // Payload length
};

struct MeshTimeReqPayload {
    uint64_t t1;           // Origin timestamp (us)
};

struct MeshTimeRespPayload {
    uint64_t t1;           // Request origin timestamp
    uint64_t t2;           // Responder ingress timestamp (us)
    uint64_t t3;           // Responder egress timestamp (us)
    uint8_t stratum;       // Responder stratum level
};

struct MeshAnnouncePayload {
    char nodeName[24];     // Node friendly name
    uint8_t hopsToGateway; // 0 = this node IS a gateway
    uint8_t isGateway;     // 1 if USB host, BLE phone, or Wi-Fi AP active
    uint8_t stratum;       // Clock stratum
    uint8_t battPct;       // Battery percentage
    uint32_t uptimeS;      // Node uptime
    uint8_t hardwareType;  // 1 = Heltec V3 ESP32-S3, 2 = Generic ESP32
    char fwVersion[12];    // Firmware version (e.g. "3.10.1")
};
#pragma pack(pop)

struct MeshNeighbor {
    uint8_t mac[6];
    char name[24];
    int8_t rssi;
    uint8_t channel;
    uint8_t hopsToGateway;
    uint8_t isGateway;
    uint8_t stratum;
    uint8_t hardwareType;
    char fwVersion[12];
    unsigned long lastSeenMs;
};

struct StoredMessage {
    uint32_t msgId;
    uint8_t srcMac[6];
    uint8_t dstMac[6];
    uint64_t timestampUs;
    uint16_t payloadLen;
    uint8_t payload[MESH_MAX_PAYLOAD];
    uint8_t retries;
    unsigned long storedTimeMs;
};

class MeshEngine {
public:
    static MeshEngine& instance() {
        static MeshEngine inst;
        return inst;
    }

    bool begin() {
        m_myMac[0] = 0;
        esp_wifi_get_mac(WIFI_IF_STA, m_myMac);
        if (m_myMac[0] == 0) {
            esp_wifi_get_mac(WIFI_IF_AP, m_myMac);
        }

        if (esp_now_init() != ESP_OK) {
            Serial.println("{\"type\":\"error\",\"component\":\"mesh\",\"message\":\"esp_now_init_failed\"}");
            return false;
        }

        // Register broadcast peer
        memset(&m_broadcastPeer, 0, sizeof(m_broadcastPeer));
        memset(m_broadcastPeer.peer_addr, 0xFF, 6);
        m_broadcastPeer.channel = 0; // Current Wi-Fi channel
        m_broadcastPeer.encrypt = false;
        esp_now_add_peer(&m_broadcastPeer);

        // Register receive callback
        esp_now_register_recv_cb(&MeshEngine::espNowRecvCb);

        m_lastAnnounceMs = 0;
        m_lastTimeReqMs = 0;
        m_lastQueueDrainMs = 0;
        m_hopsToGateway = 255;
        m_isGateway = false;
        m_usbActive = true; // Initially assume USB connected on boot
        m_lastUsbActivityMs = millis();
        m_seqCounter = 1;
        m_aimdPacingMs = 3;
        resetTrickle();

        Serial.println("{\"type\":\"init\",\"status\":\"mesh_active\",\"protocol\":\"esp_now_store_forward\",\"trickle\":\"rfc6206\"}");
        return true;
    }

    void resetTrickle() {
        m_trickleI = TRICKLE_I_MIN;
        m_trickleStartMs = millis();
        m_trickleT = (m_trickleI > 2) ? random(m_trickleI / 2, m_trickleI) : 500;
        m_trickleC = 0;
        m_trickleFired = false;
    }

    void noteUsbActivity() {
        m_lastUsbActivityMs = millis();
        m_usbActive = true;
    }

    void updateNeighborVersion(const uint8_t* mac, const char* ver) {
        if (!mac || !ver) return;
        for (int i = 0; i < m_neighborCount; i++) {
            if (memcmp(m_neighbors[i].mac, mac, 6) == 0) {
                strncpy(m_neighbors[i].fwVersion, ver, sizeof(m_neighbors[i].fwVersion) - 1);
                m_neighbors[i].fwVersion[sizeof(m_neighbors[i].fwVersion) - 1] = '\0';
                break;
            }
        }
    }

    void update() {
        unsigned long now = millis();

        // 1. Evaluate Gateway status based on NodeOpMode (Base Station vs Mobile)
        bool wasGateway = m_isGateway;
        bool usbLive = (now - m_lastUsbActivityMs < 20000);
        bool bleLive = BleScanner::instance().isConnected();
        bool staLive = (WiFi.status() == WL_CONNECTED);

        if (NodeConfig::instance().isBaseStation()) {
            // BASE STATION MODE:
            // Intended for permanent connection to server / desktop.
            // Always serves as Root Gateway (hops = 0) and Stratum 1 Master clock.
            m_isGateway = true;
            m_hopsToGateway = 0;
            if (TimeSync::instance().getStratum() == 0 || TimeSync::instance().getStratum() > 1) {
                if (usbLive || staLive) {
                    TimeSync::instance().setMasterTimeUs(1791138000000000ULL + (uint64_t)esp_timer_get_time());
                }
            }
        } else {
            // MOBILE MODE:
            // Intended for portable / battery-powered field nodes. Meshes back to the Base Station.
            // (Only acts as temporary emergency gateway if a direct USB or BLE client is actively attached)
            m_isGateway = (usbLive || bleLive);
            if (m_isGateway) {
                m_hopsToGateway = 0;
            } else {
                // Find shortest route back to Base Station from active neighbors
                uint8_t bestHops = 254;
                int bestIdx = -1;
                for (int i = 0; i < m_neighborCount; i++) {
                    if (now - m_neighbors[i].lastSeenMs < 45000) {
                        if (m_neighbors[i].hopsToGateway < bestHops) {
                            bestHops = m_neighbors[i].hopsToGateway;
                            bestIdx = i;
                        }
                    }
                }
                if (bestIdx >= 0 && bestHops < 254) {
                    m_hopsToGateway = bestHops + 1;
                    memcpy(m_nextHopMac, m_neighbors[bestIdx].mac, 6);
                } else {
                    m_hopsToGateway = 255; // Out of range of Base Station -> queue in store-and-forward
                }
            }
        }

        // 2. RFC 6206 Trickle Algorithm Announcement Dispatch
        unsigned long trickleElapsed = now - m_trickleStartMs;
        if (!m_trickleFired && trickleElapsed >= m_trickleT) {
            m_trickleFired = true;
            if (m_trickleC < TRICKLE_K) {
                sendAnnounce();
            }
        }
        if (trickleElapsed >= m_trickleI) {
            m_trickleI = (m_trickleI * 2 > TRICKLE_I_MAX) ? TRICKLE_I_MAX : (m_trickleI * 2);
            m_trickleStartMs = now;
            m_trickleT = (m_trickleI > 2) ? random(m_trickleI / 2, m_trickleI) : 500;
            m_trickleC = 0;
            m_trickleFired = false;
        }

        // 3. Periodic Sub-ms Time Sync Request (every 60-90s if not master)
        if (TimeSync::instance().shouldRequestSync() && (now - m_lastTimeReqMs >= 5000)) {
            m_lastTimeReqMs = now;
            // Send time request to neighbor with lowest stratum
            sendTimeRequest();
        }

        // 4. Drain Store-and-Forward Queue (burst out if path to gateway exists)
        if (hasRouteToGateway() && m_queueCount > 0 && (now - m_lastQueueDrainMs >= 200)) {
            m_lastQueueDrainMs = now;
            drainQueueBatch(3);
        }

        // 5. Clean stale neighbors
        cleanNeighbors(now);
    }

    /**
     * Originates a telemetry packet from this node.
     * If a route to gateway is immediately available, transmits it over mesh.
     * Otherwise, stores it in the Store-and-Forward queue with sub-ms timestamp.
     */
    void sendTelemetry(const char* jsonStr) {
        size_t len = strlen(jsonStr);
        if (len == 0) return;

        // If this node is ALREADY the gateway, local telemetry has already been output
        // locally to Serial/BLE/UDP by Telemetry::broadcastJson(). Do NOT loop back to ourselves.
        if (m_isGateway) {
            return;
        }

        // If message is too long for a single frame, truncate payload safely
        if (len > MESH_MAX_PAYLOAD - 1) len = MESH_MAX_PAYLOAD - 1;

        uint32_t msgId = generateMsgId();
        uint64_t tsUs = TimeSync::instance().getNowUs();

        if (hasRouteToGateway()) {
            bool sent = transmitPacket(MESH_MSG_TELEMETRY, msgId, m_myMac, BROADCAST_MAC, 0, 8, tsUs, (const uint8_t*)jsonStr, len);
            if (!sent) {
                enqueueMessage(msgId, m_myMac, BROADCAST_MAC, tsUs, (const uint8_t*)jsonStr, len);
            }
        } else {
            // No route to gateway right now -> Store & Forward
            enqueueMessage(msgId, m_myMac, BROADCAST_MAC, tsUs, (const uint8_t*)jsonStr, len);
        }
    }

    void broadcastAlprAlert(const char* jsonStr) {
        if (!jsonStr) return;
        size_t len = strlen(jsonStr);
        if (len > MESH_MAX_PAYLOAD - 1) len = MESH_MAX_PAYLOAD - 1;

        uint32_t msgId = generateMsgId();
        uint64_t tsUs = TimeSync::instance().getNowUs();

        transmitPacket(MESH_MSG_ALPR_ALERT, msgId, m_myMac, BROADCAST_MAC, 0, 8, tsUs, (const uint8_t*)jsonStr, len);
    }

    bool hasRouteToGateway() const {
        return (m_isGateway || m_hopsToGateway < 254);
    }

    typedef void (*MeshRadioTxHook)(const uint8_t* data, size_t len);

    void registerRadioTxHook(MeshRadioTxHook hook) {
        m_radioTxHook = hook;
    }

    typedef void (*MeshHostDeliveryHook)(const char* json);

    void registerHostDeliveryHook(MeshHostDeliveryHook hook) {
        m_hostDeliveryHook = hook;
    }

    uint8_t getHopsToGateway() const { return m_hopsToGateway; }
    bool isGateway() const { return m_isGateway; }
    uint8_t getQueueCount() const { return m_queueCount; }
    uint8_t getNeighborCount() const { return m_neighborCount; }
    bool isUsbActive() const { return m_usbActive; }

    uint8_t getActiveMobileCount() const {
        unsigned long now = millis();
        uint8_t count = 0;
        for (int i = 0; i < m_neighborCount; i++) {
            if ((now - m_neighbors[i].lastSeenMs < 60000) && !m_neighbors[i].isGateway) {
                count++;
            }
        }
        return count;
    }

    int8_t getBaseStationRssi() const {
        unsigned long now = millis();
        int8_t bestRssi = -127;
        for (int i = 0; i < m_neighborCount; i++) {
            if ((now - m_neighbors[i].lastSeenMs < 60000) && m_neighbors[i].isGateway) {
                if (m_neighbors[i].rssi > bestRssi) {
                    bestRssi = m_neighbors[i].rssi;
                }
            }
        }
        return bestRssi;
    }

    const MeshNeighbor* getNeighbors() const { return m_neighbors; }

    static constexpr uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    const uint8_t* getMyMac() const { return m_myMac; }

    typedef void (*MeshOtaCallback)(uint8_t type, const MeshPacketHdr* hdr, const uint8_t* payload, uint16_t len);
    void registerOtaCallback(MeshOtaCallback cb) { m_otaCallback = cb; }

    uint32_t generateMsgId() {
        uint32_t id = ((uint32_t)m_myMac[4] << 24) | ((uint32_t)m_myMac[5] << 16) | (m_seqCounter++ & 0xFFFF);
        if (id == 0) id = 1;
        return id;
    }

    bool transmitPacket(uint8_t type, uint32_t msgId, const uint8_t* srcMac, const uint8_t* dstMac,
                        uint8_t hops, uint8_t ttl, uint64_t tsUs, const uint8_t* payload, uint16_t payloadLen) {
        uint8_t buf[250];
        if (sizeof(MeshPacketHdr) + payloadLen > sizeof(buf)) return false;

        MeshPacketHdr* hdr = (MeshPacketHdr*)buf;
        hdr->magic[0] = MESH_MAGIC_0;
        hdr->magic[1] = MESH_MAGIC_1;
        hdr->version = 1;
        hdr->msgType = type;
        hdr->msgId = msgId;
        memcpy(hdr->srcMac, srcMac, 6);
        memcpy(hdr->dstMac, dstMac, 6);
        memcpy(hdr->prevHopMac, m_myMac, 6);
        hdr->hopCount = hops;
        hdr->ttl = ttl;
        hdr->timestampUs = tsUs;
        hdr->payloadLen = payloadLen;

        if (payload && payloadLen > 0) {
            memcpy(buf + sizeof(MeshPacketHdr), payload, payloadLen);
        }

        size_t totalLen = sizeof(MeshPacketHdr) + payloadLen;

        // If unicast to a known neighbor, temporarily tune to peer channel (skip for OTA packets)
        uint8_t origCh = 0;
        wifi_second_chan_t origSec;
        bool changedCh = false;
        bool isOta = (type >= MESH_MSG_OTA_START && type <= MESH_MSG_OTA_ABORT);
        if (!isOta && memcmp(dstMac, BROADCAST_MAC, 6) != 0) {
            for (int i = 0; i < m_neighborCount; i++) {
                if (memcmp(m_neighbors[i].mac, dstMac, 6) == 0 && m_neighbors[i].channel > 0) {
                    esp_wifi_get_channel(&origCh, &origSec);
                    if (origCh != m_neighbors[i].channel) {
                        esp_wifi_set_channel(m_neighbors[i].channel, WIFI_SECOND_CHAN_NONE);
                        changedCh = true;
                    }
                    break;
                }
            }
        }

        // CSMA/CA Slotted Random Jitter (industry standard 0.4ms - 2.5ms slotted backoff)
        // Skip jitter during high-speed OTA streaming to maintain optimal pipeline throughput
        if (!isOta) {
            delayMicroseconds(random(400, 2500));
        }

        const uint8_t* targetPeer = m_broadcastPeer.peer_addr;
        if (memcmp(dstMac, BROADCAST_MAC, 6) != 0) {
            if (!esp_now_is_peer_exist(dstMac)) {
                esp_now_peer_info_t peer;
                memset(&peer, 0, sizeof(peer));
                memcpy(peer.peer_addr, dstMac, 6);
                peer.channel = 0; // Current Wi-Fi channel
                peer.encrypt = false;
                esp_now_add_peer(&peer);
            }
            targetPeer = dstMac;
        }

        esp_err_t err = esp_now_send(targetPeer, buf, totalLen);

        if (changedCh) {
            esp_wifi_set_channel(origCh, origSec);
        }

        // AIMD dynamic pacing update
        if (err == ESP_OK) {
            if (m_aimdPacingMs > 2) m_aimdPacingMs--;
        } else {
            m_aimdPacingMs = (m_aimdPacingMs * 2 > 40) ? 40 : (m_aimdPacingMs * 2);
        }

        // Bridge over external radio (e.g. SX1262 LoRa) if registered, but do NOT clog LoRa with OTA packets
        if (m_radioTxHook && !isOta) {
            m_radioTxHook(buf, totalLen);
        }

        return (err == ESP_OK);
    }

    /**
     * Raw frame ingress handler (called by ESP-NOW or Promiscuous RX parser)
     */
    void handleRawFrame(const uint8_t* senderMac, const uint8_t* data, size_t len, int8_t rssi, uint8_t channel = 0) {
        if (len < sizeof(MeshPacketHdr)) return;

        const MeshPacketHdr* hdr = (const MeshPacketHdr*)data;
        if (hdr->magic[0] != MESH_MAGIC_0 || hdr->magic[1] != MESH_MAGIC_1) return;
        if (hdr->version != 1) return;

        // Ignore our own looped packets
        if (memcmp(hdr->srcMac, m_myMac, 6) == 0) return;

        // Deduplication check (bypass for OTA chunks)
        if (hdr->msgType != MESH_MSG_OTA_CHUNK) {
            if (isDuplicate(hdr->msgId)) return;
            markSeen(hdr->msgId);
        }

        const uint8_t* payload = data + sizeof(MeshPacketHdr);
        uint16_t payloadLen = hdr->payloadLen;
        if (payloadLen > len - sizeof(MeshPacketHdr)) payloadLen = len - sizeof(MeshPacketHdr);

        switch (hdr->msgType) {
            case MESH_MSG_ANNOUNCE:
                handleAnnounce(hdr, payload, payloadLen, rssi, channel);
                break;

            case MESH_MSG_TIME_REQ:
                handleTimeReq(hdr, payload, payloadLen);
                break;

            case MESH_MSG_TIME_RESP:
                handleTimeResp(hdr, payload, payloadLen);
                break;

            case MESH_MSG_TELEMETRY:
                handleTelemetry(hdr, payload, payloadLen, rssi);
                break;

            case MESH_MSG_ALPR_ALERT:
                handleAlprAlert(hdr, payload, payloadLen, rssi);
                break;

            case MESH_MSG_OTA_START:
            case MESH_MSG_OTA_CHUNK:
            case MESH_MSG_OTA_STATUS_REQ:
            case MESH_MSG_OTA_STATUS_RESP:
            case MESH_MSG_OTA_ABORT:
                if (m_otaCallback) {
                    m_otaCallback(hdr->msgType, hdr, payload, payloadLen);
                }
                break;

            default:
                break;
        }
    }

private:
    MeshEngine() : m_neighborCount(0), m_queueHead(0), m_queueTail(0),
                   m_queueCount(0), m_dedupIdx(0), m_hopsToGateway(255),
                   m_isGateway(false), m_usbActive(false), m_lastAnnounceMs(0),
                   m_lastTimeReqMs(0), m_lastQueueDrainMs(0), m_lastUsbActivityMs(0),
                   m_seqCounter(1), m_radioTxHook(nullptr), m_hostDeliveryHook(nullptr),
                   m_otaCallback(nullptr),
                   m_trickleI(1000), m_trickleT(500), m_trickleC(0),
                   m_trickleStartMs(0), m_trickleFired(false), m_aimdPacingMs(3) {
        memset(m_dedupRing, 0, sizeof(m_dedupRing));
        memset(m_neighbors, 0, sizeof(m_neighbors));
    }

    static void espNowRecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
        if (info && data && len > 0) {
            uint8_t ch = info->rx_ctrl ? info->rx_ctrl->channel : 0;
            MeshEngine::instance().handleRawFrame(info->src_addr, data, len, info->rx_ctrl ? info->rx_ctrl->rssi : -60, ch);
        }
    }

    bool isDuplicate(uint32_t msgId) {
        for (int i = 0; i < MESH_DEDUP_SIZE; i++) {
            if (m_dedupRing[i] == msgId) return true;
        }
        return false;
    }

    void markSeen(uint32_t msgId) {
        m_dedupRing[m_dedupIdx] = msgId;
        m_dedupIdx = (m_dedupIdx + 1) % MESH_DEDUP_SIZE;
    }

    void handleAnnounce(const MeshPacketHdr* hdr, const uint8_t* payload, uint16_t len, int8_t rssi, uint8_t channel) {
        if (len < 32) return; // Minimum legacy announce payload size (v3.10.0)
        const MeshAnnouncePayload* ann = (const MeshAnnouncePayload*)payload;

        // Upsert into neighbor table
        int idx = -1;
        for (int i = 0; i < m_neighborCount; i++) {
            if (memcmp(m_neighbors[i].mac, hdr->srcMac, 6) == 0) {
                idx = i;
                break;
            }
        }
        if (idx == -1 && m_neighborCount < MESH_MAX_NEIGHBORS) {
            idx = m_neighborCount++;
        }
        if (idx >= 0) {
            memcpy(m_neighbors[idx].mac, hdr->srcMac, 6);
            strncpy(m_neighbors[idx].name, ann->nodeName, sizeof(m_neighbors[idx].name) - 1);
            m_neighbors[idx].rssi = rssi;
            if (channel > 0) {
                m_neighbors[idx].channel = channel;
            }
            m_neighbors[idx].hopsToGateway = ann->hopsToGateway;
            m_neighbors[idx].isGateway = ann->isGateway;
            m_neighbors[idx].stratum = ann->stratum;
            if (len >= sizeof(MeshAnnouncePayload)) {
                m_neighbors[idx].hardwareType = ann->hardwareType;
                strncpy(m_neighbors[idx].fwVersion, ann->fwVersion, sizeof(m_neighbors[idx].fwVersion) - 1);
                m_neighbors[idx].fwVersion[sizeof(m_neighbors[idx].fwVersion) - 1] = '\0';
            } else {
                m_neighbors[idx].hardwareType = BEACONFIX_HW_TYPE;
                strncpy(m_neighbors[idx].fwVersion, "3.10.0", sizeof(m_neighbors[idx].fwVersion) - 1);
                m_neighbors[idx].fwVersion[sizeof(m_neighbors[idx].fwVersion) - 1] = '\0';
            }
            m_neighbors[idx].lastSeenMs = millis();

            // RFC 6206: Consistency Check
            bool versionConsistent = (strncmp(m_neighbors[idx].fwVersion, NodeConfig::instance().getVersion(), sizeof(m_neighbors[idx].fwVersion)) == 0);
            bool stratumConsistent = (m_neighbors[idx].stratum == TimeSync::instance().getStratum());
            bool gatewayConsistent = (m_neighbors[idx].isGateway == (m_isGateway ? 1 : 0));

            if (versionConsistent && stratumConsistent && gatewayConsistent) {
                m_trickleC++;
            } else {
                resetTrickle();
            }

            // Instant coarse clock sync for unsynced nodes
            if (TimeSync::instance().getStratum() == 0 && ann->stratum > 0) {
                TimeSync::instance().setInitialCoarseTime(hdr->timestampUs, ann->stratum);
            }

            // If neighbor has higher clock precision (lower stratum) or we are unsynced, request sync immediately
            if (ann->stratum > 0 && (TimeSync::instance().getStratum() == 0 || ann->stratum < TimeSync::instance().getStratum())) {
                unsigned long now = millis();
                if (now - m_lastTimeReqMs >= 2000) {
                    m_lastTimeReqMs = now;
                    sendTimeRequest();
                }
            }
        }
    }

    void handleTimeReq(const MeshPacketHdr* hdr, const uint8_t* payload, uint16_t len) {
        if (len < sizeof(MeshTimeReqPayload)) return;
        const MeshTimeReqPayload* req = (const MeshTimeReqPayload*)payload;
        uint64_t t2 = TimeSync::instance().getNowUs();

        // Prepare response
        MeshTimeRespPayload resp;
        resp.t1 = req->t1;
        resp.t2 = t2;
        resp.t3 = TimeSync::instance().getNowUs();
        resp.stratum = TimeSync::instance().getStratum();

        uint32_t respId = generateMsgId();
        transmitPacket(MESH_MSG_TIME_RESP, respId, m_myMac, hdr->srcMac, 0, 1, resp.t3, (const uint8_t*)&resp, sizeof(resp));
    }

    void handleTimeResp(const MeshPacketHdr* hdr, const uint8_t* payload, uint16_t len) {
        if (len < sizeof(MeshTimeRespPayload)) return;
        const MeshTimeRespPayload* resp = (const MeshTimeRespPayload*)payload;
        uint64_t t4 = TimeSync::instance().getNowUs();

        TimeSync::instance().applyTwoWaySync(resp->t1, resp->t2, resp->t3, t4, resp->stratum);
    }

    void handleTelemetry(const MeshPacketHdr* hdr, const uint8_t* payload, uint16_t len, int8_t rssi) {
        char jsonBuf[MESH_MAX_PAYLOAD + 1];
        memcpy(jsonBuf, payload, len);
        jsonBuf[len] = '\0';

        // Upsert sender into neighbor table if not already present
        char senderName[24] = "Node";
        int sIdx = -1;
        for (int i = 0; i < m_neighborCount; i++) {
            if (memcmp(m_neighbors[i].mac, hdr->srcMac, 6) == 0) {
                sIdx = i;
                strncpy(senderName, m_neighbors[i].name, sizeof(senderName) - 1);
                m_neighbors[i].lastSeenMs = millis();
                break;
            }
        }
        if (sIdx == -1 && m_neighborCount < MESH_MAX_NEIGHBORS) {
            sIdx = m_neighborCount++;
            memcpy(m_neighbors[sIdx].mac, hdr->srcMac, 6);
            NodeConfig::generateRedditName(hdr->srcMac, m_neighbors[sIdx].name, sizeof(m_neighbors[sIdx].name));
            strncpy(senderName, m_neighbors[sIdx].name, sizeof(senderName) - 1);
            m_neighbors[sIdx].rssi = rssi;
            m_neighbors[sIdx].hopsToGateway = hdr->hopCount + 1;
            m_neighbors[sIdx].isGateway = 0;
            m_neighbors[sIdx].stratum = 0;
            m_neighbors[sIdx].hardwareType = BEACONFIX_HW_TYPE;
            m_neighbors[sIdx].fwVersion[0] = '\0';
            m_neighbors[sIdx].lastSeenMs = millis();
        }

        // Instant coarse clock sync for unsynced nodes
        if (TimeSync::instance().getStratum() == 0 && hdr->timestampUs > 1000000000000ULL) {
            TimeSync::instance().setInitialCoarseTime(hdr->timestampUs, 2);
        }

        // Case A: This node IS a gateway -> Destination reached! Deliver locally
        if (m_isGateway) {
            deliverToHost(senderName, hdr->srcMac, hdr->prevHopMac, hdr->hopCount + 1, hdr->timestampUs, jsonBuf);
            return;
        }

        // Case B: This node is an intermediate mesh forwarder (e.g. Device 2 between 3 and 1)
        if (hdr->ttl > 1 && m_hopsToGateway < 254) {
            // Forward packet upstream towards gateway
            transmitPacket(
                MESH_MSG_TELEMETRY,
                hdr->msgId,
                hdr->srcMac,
                hdr->dstMac,
                hdr->hopCount + 1,
                hdr->ttl - 1,
                hdr->timestampUs,
                payload,
                len
            );
        } else if (hdr->ttl <= 1) {
            // Drop packet (TTL expired)
        } else {
            // We have temporarily lost route to gateway -> Enqueue in our Store & Forward queue
            enqueueMessage(hdr->msgId, hdr->srcMac, hdr->dstMac, hdr->timestampUs, payload, len);
        }
    }

    void handleAlprAlert(const MeshPacketHdr* hdr, const uint8_t* payload, uint16_t len, int8_t rssi) {
        char jsonBuf[MESH_MAX_PAYLOAD + 1];
        memcpy(jsonBuf, payload, len);
        jsonBuf[len] = '\0';

        // Deliver locally: Print to Serial, stream over BLE NUS, alert LED
        Serial.printf("%s\n", jsonBuf);
        BleScanner::instance().sendTelemetry(jsonBuf);
        LedPatterns::instance().triggerDeauthAlert();

        // Forward to mesh neighbors if TTL > 1
        if (hdr->ttl > 1) {
            transmitPacket(
                MESH_MSG_ALPR_ALERT,
                hdr->msgId,
                hdr->srcMac,
                hdr->dstMac,
                hdr->hopCount + 1,
                hdr->ttl - 1,
                hdr->timestampUs,
                payload,
                len
            );
        }
    }

    void deliverToHost(const char* originName, const uint8_t* originMac, const uint8_t* prevHopMac, uint8_t hops, uint64_t tsUs, const char* jsonStr) {
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 originMac[0], originMac[1], originMac[2], originMac[3], originMac[4], originMac[5]);

        char prevMacStr[18];
        snprintf(prevMacStr, sizeof(prevMacStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 prevHopMac[0], prevHopMac[1], prevHopMac[2], prevHopMac[3], prevHopMac[4], prevHopMac[5]);

        char viaName[24] = "Direct";
        char routeStr[64] = "Direct";

        if (hops > 1 && memcmp(prevHopMac, originMac, 6) != 0) {
            bool foundRelay = false;
            for (int i = 0; i < m_neighborCount; i++) {
                if (memcmp(m_neighbors[i].mac, prevHopMac, 6) == 0) {
                    strncpy(viaName, m_neighbors[i].name, sizeof(viaName) - 1);
                    viaName[sizeof(viaName) - 1] = '\0';
                    foundRelay = true;
                    break;
                }
            }
            if (!foundRelay) {
                NodeConfig::generateRedditName(prevHopMac, viaName, sizeof(viaName));
            }
            snprintf(routeStr, sizeof(routeStr), "via %s", viaName);
        }

        char outBuf[384];
        snprintf(outBuf, sizeof(outBuf),
            "{\"type\":\"mesh_telemetry\",\"origin\":\"%s\",\"mac\":\"%s\",\"hops\":%u,\"via\":\"%s\",\"prev_mac\":\"%s\",\"route\":\"%s\",\"ts_us\":%llu,\"data\":%s}",
            originName, macStr, hops, viaName, prevMacStr, routeStr, (unsigned long long)tsUs, jsonStr);

        // 1. USB Serial output to host computer
        Serial.println(outBuf);

        // 2. BLE GATT notification to phone
        BleScanner::instance().sendTelemetry(outBuf);

        // 3. UDP Broadcast to local network clients (phone/desktop on Wi-Fi)
        if (m_hostDeliveryHook) {
            m_hostDeliveryHook(outBuf);
        }
    }

    void sendAnnounce() {
        MeshAnnouncePayload ann;
        memset(&ann, 0, sizeof(ann));
        strncpy(ann.nodeName, NodeConfig::instance().getName(), sizeof(ann.nodeName) - 1);
        ann.hopsToGateway = m_hopsToGateway;
        ann.isGateway = m_isGateway ? 1 : 0;
        ann.stratum = TimeSync::instance().getStratum();
        ann.battPct = BatteryMonitor::instance().getPercentage();
        ann.uptimeS = millis() / 1000UL;
        ann.hardwareType = BEACONFIX_HW_TYPE;
        strncpy(ann.fwVersion, NodeConfig::instance().getVersion(), sizeof(ann.fwVersion) - 1);

        uint32_t id = generateMsgId();

        // Broadcast across primary Wi-Fi channels (1, 6, 11) plus current channel
        uint8_t curCh = 1;
        wifi_second_chan_t curSec;
        esp_wifi_get_channel(&curCh, &curSec);

        const uint8_t announceChannels[] = {1, 6, 11};
        for (uint8_t ch : announceChannels) {
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            transmitPacket(MESH_MSG_ANNOUNCE, id, m_myMac, BROADCAST_MAC, 0, 1, TimeSync::instance().getNowUs(), (const uint8_t*)&ann, sizeof(ann));
        }
        esp_wifi_set_channel(curCh, curSec);
    }

    void sendTimeRequest() {
        // Find best neighbor to sync from (lowest stratum)
        int bestIdx = -1;
        uint8_t bestStratum = 255;
        for (int i = 0; i < m_neighborCount; i++) {
            if (m_neighbors[i].stratum > 0 && m_neighbors[i].stratum < bestStratum) {
                bestStratum = m_neighbors[i].stratum;
                bestIdx = i;
            }
        }
        if (bestIdx < 0) return;

        MeshTimeReqPayload req;
        req.t1 = TimeSync::instance().getNowUs();

        uint32_t id = generateMsgId();
        transmitPacket(MESH_MSG_TIME_REQ, id, m_myMac, m_neighbors[bestIdx].mac, 0, 1, req.t1, (const uint8_t*)&req, sizeof(req));
    }

    void enqueueMessage(uint32_t msgId, const uint8_t* srcMac, const uint8_t* dstMac, uint64_t tsUs, const uint8_t* payload, uint16_t len) {
        if (len > MESH_MAX_PAYLOAD) len = MESH_MAX_PAYLOAD;

        // If queue full, drop oldest item (FIFO overflow protection)
        if (m_queueCount >= MESH_MAX_QUEUE) {
            m_queueTail = (m_queueTail + 1) % MESH_MAX_QUEUE;
            m_queueCount--;
        }

        StoredMessage* item = &m_queue[m_queueHead];
        item->msgId = msgId;
        memcpy(item->srcMac, srcMac, 6);
        memcpy(item->dstMac, dstMac, 6);
        item->timestampUs = tsUs;
        item->payloadLen = len;
        memcpy(item->payload, payload, len);
        item->retries = 0;
        item->storedTimeMs = millis();

        m_queueHead = (m_queueHead + 1) % MESH_MAX_QUEUE;
        m_queueCount++;
    }

    void drainQueueBatch(uint8_t maxBatch) {
        uint8_t sent = 0;
        while (m_queueCount > 0 && sent < maxBatch) {
            StoredMessage* item = &m_queue[m_queueTail];
            bool ok = transmitPacket(
                MESH_MSG_TELEMETRY,
                item->msgId,
                item->srcMac,
                item->dstMac,
                0,
                8,
                item->timestampUs,
                item->payload,
                item->payloadLen
            );

            if (ok) {
                m_queueTail = (m_queueTail + 1) % MESH_MAX_QUEUE;
                m_queueCount--;
                sent++;
                delay(m_aimdPacingMs); // Dynamic AIMD pacing
            } else {
                item->retries++;
                if (item->retries >= 3) {
                    m_queueTail = (m_queueTail + 1) % MESH_MAX_QUEUE;
                    m_queueCount--;
                }
                break;
            }
        }
    }

    void cleanNeighbors(unsigned long now) {
        int i = 0;
        while (i < m_neighborCount) {
            if (now - m_neighbors[i].lastSeenMs > 60000) {
                if (esp_now_is_peer_exist(m_neighbors[i].mac)) {
                    esp_now_del_peer(m_neighbors[i].mac);
                }
                m_neighbors[i] = m_neighbors[m_neighborCount - 1];
                m_neighborCount--;
            } else {
                i++;
            }
        }
    }

    uint8_t m_myMac[6];
    uint8_t m_nextHopMac[6];
    esp_now_peer_info_t m_broadcastPeer;

    MeshNeighbor m_neighbors[MESH_MAX_NEIGHBORS];
    uint8_t m_neighborCount;

    StoredMessage m_queue[MESH_MAX_QUEUE];
    uint8_t m_queueHead;
    uint8_t m_queueTail;
    uint8_t m_queueCount;

    uint32_t m_dedupRing[MESH_DEDUP_SIZE];
    uint8_t m_dedupIdx;

    uint8_t m_hopsToGateway;
    bool m_isGateway;
    bool m_usbActive;
    unsigned long m_lastAnnounceMs;
    unsigned long m_lastTimeReqMs;
    unsigned long m_lastQueueDrainMs;
    unsigned long m_lastUsbActivityMs;
    uint16_t m_seqCounter;
    MeshRadioTxHook m_radioTxHook;
    MeshHostDeliveryHook m_hostDeliveryHook;
    MeshOtaCallback m_otaCallback;

    // RFC 6206 Trickle parameters
    static constexpr uint32_t TRICKLE_I_MIN = 1000;
    static constexpr uint32_t TRICKLE_I_MAX = 30000;
    static constexpr uint8_t  TRICKLE_K     = 2;

    uint32_t m_trickleI;
    uint32_t m_trickleT;
    uint8_t  m_trickleC;
    unsigned long m_trickleStartMs;
    bool     m_trickleFired;
    uint16_t m_aimdPacingMs;
};
