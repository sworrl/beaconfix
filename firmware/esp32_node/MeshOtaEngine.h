#pragma once
#include <Arduino.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>
#include "MeshEngine.h"
#include "OtaUpdater.h"
#include "WifiMonitor.h"
#include "DisplayOled.h"
#include "LedPatterns.h"

enum MeshOtaEngineState : uint8_t {
    OTA_STATE_IDLE = 0,
    OTA_STATE_RECEIVING = 1,
    OTA_STATE_VERIFYING = 2,
    OTA_STATE_SUCCESS = 3,
    OTA_STATE_FAILED = 4
};

enum MeshOtaErrorCode : uint8_t {
    OTA_ERR_NONE = 0,
    OTA_ERR_BEGIN = 1,
    OTA_ERR_WRITE = 2,
    OTA_ERR_SIG = 3,
    OTA_ERR_TIMEOUT = 4,
    OTA_ERR_ABORTED = 5
};

class MeshOtaEngine {
public:
    static MeshOtaEngine& instance() {
        static MeshOtaEngine inst;
        return inst;
    }

    void begin() {
        MeshEngine::instance().registerOtaCallback(&MeshOtaEngine::onMeshOtaPacket);
        m_state = OTA_STATE_IDLE;
        m_errorCode = OTA_ERR_NONE;
        m_nextExpectedChunk = 0;
        m_totalChunks = 0;
        m_totalBytes = 0;
        m_bytesWritten = 0;
        m_rebootPending = false;
        m_rebootScheduledMs = 0;
        m_lastChunkMs = 0;
        m_lastNackMs = 0;
        m_startTimeMs = 0;
        m_origWifiChannel = 1;
        m_origAutoHop = true;
        m_wifiLocked = false;

        m_isSeeding = false;
        m_seedChunkIdx = 0;
        m_seedWindowStart = 0;
        m_seedTotalChunks = 0;
        m_seedWaitingAck = false;
        m_seedQuerySent = false;
        m_seedHoldoffUntilMs = 0;
        m_seedWindowSentMs = 0;
        m_seedWindowRetries = 0;
        m_lastSeedChunkMs = 0;
        m_lastSeedCheckMs = millis();
        m_seedStartTimeMs = 0;
        m_seedPacingMs = 5;
        memset(m_seedTargetMac, 0xFF, 6);

        loadManifestFromNvs();
    }

    bool isSeeding() const { return m_isSeeding; }
    bool hasManifest() const { return m_hasManifest; }
    const char* getManifestVersion() const { return m_manifestVersion; }
    uint32_t getManifestBytes() const { return m_manifestBytes; }
    bool triggerSeeding(const uint8_t* targetMac = nullptr) { return startAutonomousSeeding(targetMac); }

    void saveManifest(uint32_t totalBytes, uint16_t chunkSize, const uint8_t* sha256,
                      const uint8_t* sig, uint8_t sigLen, const char* version, uint8_t hwType) {
        Preferences prefs;
        prefs.begin("bcf_ota", false);
        prefs.putUInt("bytes", totalBytes);
        prefs.putUShort("chunk_sz", chunkSize);
        prefs.putUChar("hw", hwType);
        prefs.putUChar("sig_len", sigLen);
        prefs.putString("ver", version ? version : BEACONFIX_FW_VERSION);
        prefs.putBytes("sha256", sha256, 32);
        prefs.putBytes("sig", sig, sigLen);
        prefs.end();

        m_manifestBytes = totalBytes;
        m_manifestChunkSize = chunkSize > 0 ? chunkSize : OTA_CHUNK_PAYLOAD_MAX;
        m_manifestTotalChunks = (totalBytes + m_manifestChunkSize - 1) / m_manifestChunkSize;
        memcpy(m_manifestSha256, sha256, 32);
        m_manifestSigLen = sigLen;
        memcpy(m_manifestSig, sig, sigLen);
        strncpy(m_manifestVersion, version ? version : BEACONFIX_FW_VERSION, sizeof(m_manifestVersion) - 1);
        m_manifestVersion[sizeof(m_manifestVersion) - 1] = '\0';
        m_manifestHwType = hwType;
        m_hasManifest = true;

        Serial.printf("{\"type\":\"mesh_ota\",\"manifest_saved\":true,\"version\":\"%s\",\"bytes\":%u,\"chunks\":%u,\"hw\":%u}\n",
                      m_manifestVersion, m_manifestBytes, m_manifestTotalChunks, m_manifestHwType);
    }

    void loadManifestFromNvs() {
        Preferences prefs;
        prefs.begin("bcf_ota", true);
        m_manifestBytes = prefs.getUInt("bytes", 0);
        m_manifestChunkSize = prefs.getUShort("chunk_sz", OTA_CHUNK_PAYLOAD_MAX);
        m_manifestHwType = prefs.getUChar("hw", BEACONFIX_HW_TYPE);
        m_manifestSigLen = prefs.getUChar("sig_len", 0);
        String ver = prefs.getString("ver", "");
        if (ver.length() > 0 && ver.length() < sizeof(m_manifestVersion)) {
            strncpy(m_manifestVersion, ver.c_str(), sizeof(m_manifestVersion) - 1);
            m_manifestVersion[sizeof(m_manifestVersion) - 1] = '\0';
        } else {
            m_manifestVersion[0] = '\0';
        }
        size_t hLen = prefs.getBytes("sha256", m_manifestSha256, 32);
        size_t sLen = prefs.getBytes("sig", m_manifestSig, sizeof(m_manifestSig));
        prefs.end();

        m_hasManifest = (m_manifestBytes > 100000 && hLen == 32 && sLen > 0 && m_manifestSigLen > 0 && m_manifestVersion[0] != '\0');
        if (m_hasManifest) {
            m_manifestTotalChunks = (m_manifestBytes + m_manifestChunkSize - 1) / m_manifestChunkSize;
            Serial.printf("{\"type\":\"mesh_ota\",\"manifest_loaded\":true,\"version\":\"%s\",\"bytes\":%u,\"chunks\":%u,\"hw\":%u}\n",
                          m_manifestVersion, m_manifestBytes, m_manifestTotalChunks, m_manifestHwType);
        }
    }

    bool startAutonomousSeeding(const uint8_t* targetMac = nullptr) {
        if (!m_hasManifest || m_state == OTA_STATE_RECEIVING || m_isSeeding) return false;

        if (targetMac) {
            memcpy(m_seedTargetMac, targetMac, 6);
        } else {
            memset(m_seedTargetMac, 0xFF, 6); // Broadcast to all matching outdated nodes
        }

        m_isSeeding = true;
        m_seedChunkIdx = 0;
        m_seedWindowStart = 0;
        m_seedTotalChunks = m_manifestTotalChunks;
        m_seedWaitingAck = false;
        m_seedQuerySent = false;
        m_seedHoldoffUntilMs = 0;
        m_seedLastRewindMs = 0;
        m_seedWindowSentMs = 0;
        m_seedWindowRetries = 0;
        m_seedWindowConsecutiveDrops = 0;
        m_seedPacingMs = 12; // 12ms adaptive pacing (~80 chunks/s ping-pong burst, finishes 7k chunks in ~85s)
        m_lastSeedChunkMs = millis();
        m_seedStartTimeMs = millis();

        // Silence promiscuous sniffing during OTA transfer for dedicated throughput
        esp_wifi_set_promiscuous(false);

        // Lock Wi-Fi to channel 1 for consistent mesh flash delivery
        if (!m_wifiLocked) {
            m_origWifiChannel = WifiMonitor::instance().getChannel();
            m_origAutoHop = WifiMonitor::instance().isAutoHop();
            m_wifiLocked = true;
        }
        WifiMonitor::instance().setChannel(1);

        bool ok = false;
        for (int i = 0; i < 3; i++) {
            ok |= transmitOtaStart(
                m_seedTargetMac,
                m_manifestBytes,
                m_manifestChunkSize,
                m_manifestSha256,
                m_manifestSig,
                m_manifestSigLen,
                m_manifestVersion,
                1,
                m_manifestHwType
            );
            if (i < 2) delay(20);
        }

        Serial.printf("{\"type\":\"mesh_ota_seeder\",\"status\":\"started\",\"chunks\":%u,\"target\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ver\":\"%s\"}\n",
                      m_seedTotalChunks, m_seedTargetMac[0], m_seedTargetMac[1], m_seedTargetMac[2],
                      m_seedTargetMac[3], m_seedTargetMac[4], m_seedTargetMac[5], m_manifestVersion);

        return ok;
    }

    void update() {
        unsigned long now = millis();

        // 1. Process scheduled reboot after successful OTA
        if (m_rebootPending && now >= m_rebootScheduledMs) {
            Serial.println("{\"type\":\"mesh_ota\",\"action\":\"restarting_into_new_firmware\"}");
            delay(100);
            ESP.restart();
        }

        // 2. Timeout detection during receiving
        if (m_state == OTA_STATE_RECEIVING) {
            if (now - m_lastChunkMs > 20000) { // 20s without a chunk
                Serial.printf("{\"type\":\"error\",\"component\":\"mesh_ota\",\"msg\":\"session_timeout\",\"chunk\":%u,\"total\":%u}\n",
                              m_nextExpectedChunk, m_totalChunks);
                Update.abort();
                m_state = OTA_STATE_FAILED;
                m_errorCode = OTA_ERR_TIMEOUT;
                LedPatterns::instance().playSlot(0, PATTERN_OTA_FAILED, RgbColor::Red());
                DisplayOled::instance().showOtaProgress(0, m_nextExpectedChunk, m_totalChunks, "TIMEOUT ABORTED!", m_version);
                restoreWifi();
                delay(1500);
                DisplayOled::instance().dismissOtaProgress();
                LedPatterns::instance().playSlot(0, PATTERN_HEARTBEAT, RgbColor::Cyan());
            }
        }

        // 3. Autonomous Seeder: Check if any neighbor is outdated and needs update
        if (m_hasManifest && !m_isSeeding && m_state == OTA_STATE_IDLE) {
            if (now - m_lastSeedCheckMs >= 8000) {
                m_lastSeedCheckMs = now;
                const MeshNeighbor* n = MeshEngine::instance().getNeighbors();
                uint8_t count = MeshEngine::instance().getNeighborCount();
                for (uint8_t i = 0; i < count; i++) {
                    if (now - n[i].lastSeenMs < 45000) {
                        // Check if hardware matches our manifest
                        if (n[i].hardwareType == m_manifestHwType || n[i].hardwareType == 0) {
                            // Check if neighbor firmware version is known and strictly older
                            if (n[i].fwVersion[0] != '\0' && strcmp(n[i].fwVersion, m_manifestVersion) < 0) {
                                Serial.printf("{\"type\":\"mesh_ota_seeder\",\"event\":\"outdated_neighbor_detected\",\"neighbor\":\"%s\",\"ver\":\"%s\",\"our_ver\":\"%s\"}\n",
                                              n[i].name, n[i].fwVersion, m_manifestVersion);
                                startAutonomousSeeding(n[i].mac);
                                break;
                            }
                        }
                    }
                }
            }
        }

        // 4. Autonomous Seeder: Stream chunks with Sliding Window Flow Control
        if (m_isSeeding) {
            if (now < m_seedHoldoffUntilMs) return;
            if (!m_seedWaitingAck) {
                if ((m_seedChunkIdx - m_seedWindowStart < OTA_WINDOW_SIZE) && (m_seedChunkIdx < m_seedTotalChunks)) {
                    if (now - m_lastSeedChunkMs >= m_seedPacingMs) {
                        m_lastSeedChunkMs = now;
                        const esp_partition_t* running = esp_ota_get_running_partition();
                        if (running) {
                            uint32_t offset = (uint32_t)m_seedChunkIdx * m_manifestChunkSize;
                            uint16_t len = m_manifestChunkSize;
                            if (offset + len > m_manifestBytes) {
                                len = m_manifestBytes - offset;
                            }

                            uint8_t chunkBuf[OTA_CHUNK_PAYLOAD_MAX];
                            esp_err_t r = esp_partition_read(running, offset, chunkBuf, len);
                            if (r == ESP_OK) {
                                transmitOtaChunk(m_seedTargetMac, m_seedChunkIdx, chunkBuf, len);
                                m_seedChunkIdx++;

                                // End of window reached or final chunk sent: pause and prepare to solicit receiver ACK
                                if ((m_seedChunkIdx - m_seedWindowStart >= OTA_WINDOW_SIZE) || (m_seedChunkIdx >= m_seedTotalChunks)) {
                                    m_seedWaitingAck = true;
                                    m_seedWindowSentMs = now;
                                    m_seedWindowRetries = 0;
                                    m_seedQuerySent = false;
                                }
                            } else {
                                Serial.printf("{\"type\":\"error\",\"component\":\"mesh_ota_seeder\",\"msg\":\"partition_read_err\",\"err\":%d}\n", r);
                                m_isSeeding = false;
                                DisplayOled::instance().dismissSeederProgress();
                                restoreWifi();
                            }
                        } else {
                            m_isSeeding = false;
                            DisplayOled::instance().dismissSeederProgress();
                            restoreWifi();
                        }
                    }
                } else {
                    // Window boundary reached or final chunk sent: pause
                    m_seedWaitingAck = true;
                    m_seedWindowSentMs = now;
                    m_seedWindowRetries = 0;
                    m_seedQuerySent = false;
                }
            } else {
                // Waiting for receiver status ACK for current window
                if (!m_seedQuerySent) {
                    // Give receiver 30ms to finish processing and emit its autonomous ACK before querying
                    if (now - m_seedWindowSentMs >= 30) {
                        m_seedQuerySent = true;
                        m_seedWindowSentMs = now;
                        transmitOtaQuery(m_seedTargetMac);
                    }
                } else if (now - m_seedWindowSentMs >= 45) { // 45ms ACK query timeout
                    m_seedWindowSentMs = now;
                    m_seedWindowRetries++;
                    if (m_seedWindowRetries <= 2) {
                        transmitOtaQuery(m_seedTargetMac);
                    } else {
                        // Query retry exceeded: rewind to window start
                        m_seedWindowConsecutiveDrops++;
                        if (m_seedWindowConsecutiveDrops >= 5) {
                            Serial.printf("{\"type\":\"mesh_ota_seeder\",\"status\":\"aborted_no_progress\",\"target\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}\n",
                                          m_seedTargetMac[0], m_seedTargetMac[1], m_seedTargetMac[2],
                                          m_seedTargetMac[3], m_seedTargetMac[4], m_seedTargetMac[5]);
                            m_isSeeding = false;
                            DisplayOled::instance().dismissSeederProgress();
                            restoreWifi();
                        } else {
                            if (m_seedPacingMs < 30) m_seedPacingMs += 2;
                            m_seedChunkIdx = m_seedWindowStart;
                            m_seedWaitingAck = false;
                            m_seedWindowRetries = 0;
                            m_seedQuerySent = false;
                        }
                    }
                }
            }
        }
    }

    // ── Coordinator / Transmitter Methods ────────────────────────────────────

    bool transmitOtaStart(const uint8_t* dstMac, uint32_t totalBytes, uint16_t chunkSize,
                          const uint8_t* sha256, const uint8_t* sig, uint8_t sigLen,
                          const char* version, uint8_t channel = 1, uint8_t hardwareType = BEACONFIX_HW_TYPE) {
        MeshOtaStartPayload p;
        memset(&p, 0, sizeof(p));
        p.totalBytes = totalBytes;
        p.chunkSize = chunkSize > 0 ? chunkSize : OTA_CHUNK_PAYLOAD_MAX;
        p.totalChunks = (totalBytes + p.chunkSize - 1) / p.chunkSize;
        memcpy(p.sha256, sha256, 32);
        strncpy(p.version, version ? version : "3.10.0", sizeof(p.version) - 1);
        p.hardwareType = hardwareType;
        p.channel = channel > 0 ? channel : 1;
        p.sigLen = sigLen > sizeof(p.sig) ? sizeof(p.sig) : sigLen;
        memcpy(p.sig, sig, p.sigLen);

        // Lock local Wi-Fi channel so sender and receivers align
        WifiMonitor::instance().setChannel(p.channel);

        uint32_t id = MeshEngine::instance().generateMsgId();
        return MeshEngine::instance().transmitPacket(
            MESH_MSG_OTA_START,
            id,
            MeshEngine::instance().getMyMac(),
            dstMac,
            0,
            8,
            TimeSync::instance().getNowUs(),
            (const uint8_t*)&p,
            sizeof(p)
        );
    }

    bool transmitOtaChunk(const uint8_t* dstMac, uint16_t chunkIndex, const uint8_t* data, uint16_t dataLen) {
        MeshOtaChunkPayload p;
        p.chunkIndex = chunkIndex;
        p.dataLen = dataLen > OTA_CHUNK_PAYLOAD_MAX ? OTA_CHUNK_PAYLOAD_MAX : dataLen;
        memcpy(p.data, data, p.dataLen);

        uint32_t id = MeshEngine::instance().generateMsgId();
        return MeshEngine::instance().transmitPacket(
            MESH_MSG_OTA_CHUNK,
            id,
            MeshEngine::instance().getMyMac(),
            dstMac,
            0,
            8,
            TimeSync::instance().getNowUs(),
            (const uint8_t*)&p,
            sizeof(MeshOtaChunkPayload) - (OTA_CHUNK_PAYLOAD_MAX - p.dataLen)
        );
    }

    bool transmitOtaQuery(const uint8_t* dstMac) {
        MeshOtaStatusReqPayload p;
        memcpy(p.targetMac, dstMac, 6);

        uint32_t id = MeshEngine::instance().generateMsgId();
        return MeshEngine::instance().transmitPacket(
            MESH_MSG_OTA_STATUS_REQ,
            id,
            MeshEngine::instance().getMyMac(),
            dstMac,
            0,
            8,
            TimeSync::instance().getNowUs(),
            (const uint8_t*)&p,
            sizeof(p)
        );
    }

    bool transmitOtaAbort(const uint8_t* dstMac, const char* reason = "User aborted") {
        MeshOtaAbortPayload p;
        memset(&p, 0, sizeof(p));
        strncpy(p.reason, reason ? reason : "User aborted", sizeof(p.reason) - 1);

        uint32_t id = MeshEngine::instance().generateMsgId();
        return MeshEngine::instance().transmitPacket(
            MESH_MSG_OTA_ABORT,
            id,
            MeshEngine::instance().getMyMac(),
            dstMac,
            0,
            8,
            TimeSync::instance().getNowUs(),
            (const uint8_t*)&p,
            sizeof(p)
        );
    }

    // ── Receiver Status Accessors ────────────────────────────────────────────

    uint8_t getState() const { return m_state; }
    uint16_t getNextExpectedChunk() const { return m_nextExpectedChunk; }
    uint16_t getTotalChunks() const { return m_totalChunks; }
    uint32_t getTotalBytes() const { return m_totalBytes; }
    uint32_t getBytesWritten() const { return m_bytesWritten; }
    uint8_t getErrorCode() const { return m_errorCode; }
    const char* getVersion() const { return m_version; }

    static void onMeshOtaPacket(uint8_t type, const MeshPacketHdr* hdr, const uint8_t* payload, uint16_t len) {
        MeshOtaEngine::instance().processOtaPacket(type, hdr, payload, len);
    }

private:
    MeshOtaEngine() : m_state(OTA_STATE_IDLE), m_errorCode(OTA_ERR_NONE),
                      m_totalBytes(0), m_chunkSize(0), m_totalChunks(0),
                      m_nextExpectedChunk(0), m_bytesWritten(0), m_sigLen(0),
                      m_startTimeMs(0), m_lastChunkMs(0), m_lastNackMs(0),
                      m_rebootScheduledMs(0), m_rebootPending(false),
                      m_origWifiChannel(1), m_origAutoHop(true), m_wifiLocked(false),
                      m_hasManifest(false), m_manifestBytes(0),
                      m_manifestChunkSize(OTA_CHUNK_PAYLOAD_MAX), m_manifestTotalChunks(0),
                      m_manifestSigLen(0), m_manifestHwType(BEACONFIX_HW_TYPE),
                      m_isSeeding(false), m_seedChunkIdx(0), m_seedWindowStart(0),
                      m_seedTotalChunks(0), m_seedWaitingAck(false), m_seedQuerySent(false),
                      m_seedHoldoffUntilMs(0), m_seedLastRewindMs(0), m_seedWindowSentMs(0),
                      m_seedWindowRetries(0), m_lastSeedChunkMs(0), m_lastSeedCheckMs(0),
                      m_seedStartTimeMs(0), m_seedPacingMs(20) {
        memset(m_sha256Expected, 0, sizeof(m_sha256Expected));
        memset(m_sig, 0, sizeof(m_sig));
        memset(m_version, 0, sizeof(m_version));
        memset(m_senderMac, 0, sizeof(m_senderMac));
        memset(m_manifestSha256, 0, sizeof(m_manifestSha256));
        memset(m_manifestSig, 0, sizeof(m_manifestSig));
        memset(m_manifestVersion, 0, sizeof(m_manifestVersion));
        memset(m_seedTargetMac, 0xFF, 6);
    }

    void processOtaPacket(uint8_t type, const MeshPacketHdr* hdr, const uint8_t* payload, uint16_t len) {
        switch (type) {
            case MESH_MSG_OTA_START:
                if (len >= sizeof(MeshOtaStartPayload)) {
                    handleOtaStart(hdr, (const MeshOtaStartPayload*)payload);
                }
                break;

            case MESH_MSG_OTA_CHUNK:
                if (len >= sizeof(uint16_t) * 2) {
                    handleOtaChunk(hdr, (const MeshOtaChunkPayload*)payload);
                }
                break;

            case MESH_MSG_OTA_STATUS_REQ:
                if (len >= sizeof(MeshOtaStatusReqPayload)) {
                    handleOtaStatusReq(hdr, (const MeshOtaStatusReqPayload*)payload);
                }
                break;

            case MESH_MSG_OTA_STATUS_RESP:
                if (len >= sizeof(MeshOtaStatusRespPayload)) {
                    handleOtaStatusResp(hdr, (const MeshOtaStatusRespPayload*)payload);
                }
                break;

            case MESH_MSG_OTA_ABORT:
                if (len >= sizeof(MeshOtaAbortPayload)) {
                    handleOtaAbort(hdr, (const MeshOtaAbortPayload*)payload);
                }
                break;

            default:
                break;
        }
    }

    void handleOtaStart(const MeshPacketHdr* hdr, const MeshOtaStartPayload* payload) {
        const uint8_t* myMac = MeshEngine::instance().getMyMac();
        bool isForMe = (memcmp(hdr->dstMac, myMac, 6) == 0) ||
                       (memcmp(hdr->dstMac, MeshEngine::BROADCAST_MAC, 6) == 0);

        if (!isForMe) return;
        
        // Hardware compatibility verification:
        // 0 = any, 1 = Heltec V3 ESP32-S3, 2 = Generic ESP32
        if (payload->hardwareType != 0 && payload->hardwareType != BEACONFIX_HW_TYPE) {
            Serial.printf("{\"type\":\"mesh_ota\",\"status\":\"ignored_hardware_mismatch\",\"expected\":%u,\"got\":%u}\n",
                          BEACONFIX_HW_TYPE, payload->hardwareType);
            return;
        }

        // Check if this node is ALREADY running this version or a newer version!
        if (strcmp(payload->version, NodeConfig::instance().getVersion()) <= 0) {
            Serial.printf("{\"type\":\"mesh_ota\",\"status\":\"ignored_already_up_to_date\",\"our_ver\":\"%s\",\"offered\":\"%s\"}\n",
                          NodeConfig::instance().getVersion(), payload->version);
            sendStatusResp(hdr->srcMac);
            return;
        }

        // If an update is already receiving from the same sender, abort and restart clean
        if (m_state == OTA_STATE_RECEIVING) {
            Update.abort();
            mbedtls_sha256_free(&m_shaCtx);
        }

        // Lock Wi-Fi to specified OTA channel (prevents channel-hopping packet drops)
        if (!m_wifiLocked) {
            m_origWifiChannel = WifiMonitor::instance().getChannel();
            m_origAutoHop = WifiMonitor::instance().isAutoHop();
            m_wifiLocked = true;
        }
        WifiMonitor::instance().setChannel(payload->channel > 0 ? payload->channel : 1);
        esp_wifi_set_promiscuous(false); // Dedicate 100% CPU and buffers to receiving firmware chunks

        // Check partition space and begin flash updater
        if (!Update.begin(payload->totalBytes, U_FLASH)) {
            Serial.printf("{\"type\":\"error\",\"component\":\"mesh_ota\",\"msg\":\"update_begin_failed\",\"err\":\"%s\"}\n",
                          Update.errorString());
            m_state = OTA_STATE_FAILED;
            m_errorCode = OTA_ERR_BEGIN;
            restoreWifi();
            sendStatusResp(hdr->srcMac);
            return;
        }

        // Initialize SHA-256 context
        mbedtls_sha256_init(&m_shaCtx);
        mbedtls_sha256_starts(&m_shaCtx, 0);

        m_totalBytes = payload->totalBytes;
        m_chunkSize = payload->chunkSize > 0 ? payload->chunkSize : OTA_CHUNK_PAYLOAD_MAX;
        m_totalChunks = payload->totalChunks;
        m_nextExpectedChunk = 0;
        m_bytesWritten = 0;
        memcpy(m_sha256Expected, payload->sha256, 32);
        m_sigLen = payload->sigLen > sizeof(m_sig) ? sizeof(m_sig) : payload->sigLen;
        memcpy(m_sig, payload->sig, m_sigLen);
        strncpy(m_version, payload->version, sizeof(m_version) - 1);
        m_version[sizeof(m_version) - 1] = '\0';
        memcpy(m_senderMac, hdr->srcMac, 6);

        m_state = OTA_STATE_RECEIVING;
        m_errorCode = OTA_ERR_NONE;
        m_startTimeMs = millis();
        m_lastChunkMs = millis();
        m_lastNackMs = 0;

        // Visual indicator on headless ESP32: energetic OTA progress pattern!
        LedPatterns::instance().playSlot(0, PATTERN_OTA_PROGRESS, RgbColor::Cyan());

        // Update OLED screen with live OTA splash (if screen exists)
        DisplayOled::instance().showOtaProgress(0, 0, m_totalChunks, "STARTING MESH FLASH...", m_version);

        // Serial log
        Serial.printf("{\"type\":\"mesh_ota\",\"status\":\"session_started\",\"total\":%u,\"chunks\":%u,\"version\":\"%s\"}\n",
                      m_totalBytes, m_totalChunks, m_version);

        // Send immediate acknowledgment back to sender
        sendStatusResp(hdr->srcMac);
    }

    void handleOtaChunk(const MeshPacketHdr* hdr, const MeshOtaChunkPayload* payload) {
        if (m_state != OTA_STATE_RECEIVING) return;

        // If duplicate chunk already written, ignore
        if (payload->chunkIndex < m_nextExpectedChunk) {
            return;
        }

        // If packet dropped and chunk arrived out of order, rate-limited NACK
        if (payload->chunkIndex > m_nextExpectedChunk) {
            unsigned long now = millis();
            if (now - m_lastNackMs >= 60) {
                m_lastNackMs = now;
                sendStatusResp(hdr->srcMac);
            }
            return;
        }

        // Write sequential chunk to flash
        size_t written = Update.write(const_cast<uint8_t*>(payload->data), payload->dataLen);
        if (written != payload->dataLen) {
            Serial.printf("{\"type\":\"error\",\"component\":\"mesh_ota\",\"msg\":\"flash_write_error\",\"chunk\":%u,\"err\":\"%s\"}\n",
                          payload->chunkIndex, Update.errorString());
            Update.abort();
            mbedtls_sha256_free(&m_shaCtx);
            m_state = OTA_STATE_FAILED;
            m_errorCode = OTA_ERR_WRITE;
            LedPatterns::instance().playSlot(0, PATTERN_OTA_FAILED, RgbColor::Red());
            DisplayOled::instance().showOtaProgress(0, m_nextExpectedChunk, m_totalChunks, "WRITE ERROR!", m_version);
            restoreWifi();
            sendStatusResp(hdr->srcMac);
            return;
        }

        // Feed bytes into SHA-256 calculation
        mbedtls_sha256_update(&m_shaCtx, payload->data, payload->dataLen);
        m_bytesWritten += written;
        m_nextExpectedChunk++;
        m_lastChunkMs = millis();

        // Update OLED display metadata (non-blocking, decouple from I2C)
        uint8_t pct = (uint8_t)(((uint32_t)m_nextExpectedChunk * 100) / m_totalChunks);
        unsigned long elapsed = millis() - m_startTimeMs;
        uint32_t speedKbps = elapsed > 0 ? (m_bytesWritten / elapsed) : 0;
        char msg[32];
        snprintf(msg, sizeof(msg), "RECV %lu KB/s", (unsigned long)speedKbps);
        DisplayOled::instance().showOtaProgress(pct, m_nextExpectedChunk, m_totalChunks, msg, m_version);

        // Window boundary reached (every 16 chunks) or final chunk: Send ACK response immediately
        if (m_nextExpectedChunk % OTA_WINDOW_SIZE == 0 || m_nextExpectedChunk == m_totalChunks) {
            sendStatusResp(hdr->srcMac);
        }

        // Check if all chunks have arrived
        if (m_nextExpectedChunk == m_totalChunks) {
            finishAndVerify();
        }
    }

    void finishAndVerify() {
        m_state = OTA_STATE_VERIFYING;
        DisplayOled::instance().showOtaProgress(100, m_totalChunks, m_totalChunks, "VERIFYING CRYPTO...", m_version);

        uint8_t computedHash[32];
        mbedtls_sha256_finish(&m_shaCtx, computedHash);
        mbedtls_sha256_free(&m_shaCtx);

        // 1. Verify computed SHA-256 against expected hash
        if (memcmp(computedHash, m_sha256Expected, 32) != 0) {
            Serial.println("{\"type\":\"error\",\"component\":\"mesh_ota\",\"msg\":\"sha256_mismatch\"}");
            Update.abort();
            m_state = OTA_STATE_FAILED;
            m_errorCode = OTA_ERR_SIG;
            LedPatterns::instance().playSlot(0, PATTERN_OTA_FAILED, RgbColor::Red());
            DisplayOled::instance().showOtaProgress(0, m_totalChunks, m_totalChunks, "HASH MISMATCH!", m_version);
            restoreWifi();
            sendStatusResp(m_senderMac);
            return;
        }

        // 2. Cryptographic ECDSA secp256r1 signature verification
        bool sigValid = OtaUpdater::instance().verifySignature(computedHash, m_sig, m_sigLen);
        if (!sigValid) {
            Serial.println("{\"type\":\"error\",\"component\":\"mesh_ota\",\"msg\":\"signature_verification_rejected\"}");
            Update.abort();
            m_state = OTA_STATE_FAILED;
            m_errorCode = OTA_ERR_SIG;
            LedPatterns::instance().playSlot(0, PATTERN_OTA_FAILED, RgbColor::Red());
            DisplayOled::instance().showOtaProgress(0, m_totalChunks, m_totalChunks, "SIG INVALID!", m_version);
            restoreWifi();
            sendStatusResp(m_senderMac);
            return;
        }

        // 3. Mark update as valid and set boot partition
        if (Update.end(true)) {
            m_state = OTA_STATE_SUCCESS;
            m_errorCode = OTA_ERR_NONE;

            // Signal LED success
            LedPatterns::instance().playSlot(0, PATTERN_OTA_SUCCESS, RgbColor::Green());

            // Save verified manifest into NVS so this node can seed to other neighbors after reboot!
            saveManifest(m_totalBytes, m_chunkSize, computedHash, m_sig, m_sigLen, m_version, BEACONFIX_HW_TYPE);

            DisplayOled::instance().showOtaProgress(100, m_totalChunks, m_totalChunks, "SUCCESS! REBOOTING...", m_version);
            Serial.println("{\"type\":\"mesh_ota\",\"status\":\"verified_flashed\",\"action\":\"rebooting\"}");
            restoreWifi();
            sendStatusResp(m_senderMac);
            m_rebootPending = true;
            m_rebootScheduledMs = millis() + 1500;
        } else {
            Serial.printf("{\"type\":\"error\",\"component\":\"mesh_ota\",\"msg\":\"update_commit_failed\",\"err\":\"%s\"}\n",
                          Update.errorString());
            Update.abort();
            m_state = OTA_STATE_FAILED;
            m_errorCode = OTA_ERR_WRITE;
            LedPatterns::instance().playSlot(0, PATTERN_OTA_FAILED, RgbColor::Red());
            DisplayOled::instance().showOtaProgress(0, m_totalChunks, m_totalChunks, "COMMIT FAILED!", m_version);
            restoreWifi();
            sendStatusResp(m_senderMac);
        }
    }

    void handleOtaStatusReq(const MeshPacketHdr* hdr, const MeshOtaStatusReqPayload* payload) {
        const uint8_t* myMac = MeshEngine::instance().getMyMac();
        if (memcmp(payload->targetMac, myMac, 6) == 0 ||
            memcmp(payload->targetMac, MeshEngine::BROADCAST_MAC, 6) == 0) {
            sendStatusResp(hdr->srcMac);
        }
    }

    void handleOtaStatusResp(const MeshPacketHdr* hdr, const MeshOtaStatusRespPayload* resp) {
        char macStr[18];
        WifiMonitor::formatMac(hdr->srcMac, macStr);
        const char* stateStr = "idle";
        switch (resp->state) {
            case OTA_STATE_IDLE: stateStr = "idle"; break;
            case OTA_STATE_RECEIVING: stateStr = "receiving"; break;
            case OTA_STATE_VERIFYING: stateStr = "verifying"; break;
            case OTA_STATE_SUCCESS: stateStr = "success"; break;
            case OTA_STATE_FAILED: stateStr = "failed"; break;
        }

        // Find Reddit name of remote node from neighbor table
        char nodeName[32] = "UnknownNode";
        const MeshNeighbor* n = MeshEngine::instance().getNeighbors();
        uint8_t count = MeshEngine::instance().getNeighborCount();
        for (uint8_t i = 0; i < count; i++) {
            if (memcmp(n[i].mac, hdr->srcMac, 6) == 0) {
                strncpy(nodeName, n[i].name, sizeof(nodeName) - 1);
                nodeName[sizeof(nodeName) - 1] = '\0';
                break;
            }
        }

        Serial.printf(
            "{\"type\":\"mesh_ota_remote_status\",\"mac\":\"%s\",\"node\":\"%s\",\"state\":\"%s\",\"next_chunk\":%u,\"total\":%u,\"pct\":%u,\"err\":%u}\n",
            macStr, nodeName, stateStr, resp->nextExpectedChunk, resp->totalChunks, resp->percent, resp->errorCode
        );

        if (m_isSeeding) {
            if (resp->errorCode != OTA_ERR_NONE) {
                // Remote reported an error
                m_isSeeding = false;
                DisplayOled::instance().dismissSeederProgress();
                restoreWifi();
                return;
            }

            if (resp->state == OTA_STATE_IDLE) {
                // Remote node is IDLE (not in receiving state or already up to date)
                Serial.printf("{\"type\":\"mesh_ota_seeder\",\"status\":\"remote_is_idle\",\"mac\":\"%s\",\"node\":\"%s\"}\n",
                              macStr, nodeName);
                MeshEngine::instance().updateNeighborVersion(hdr->srcMac, m_manifestVersion);
                m_isSeeding = false;
                DisplayOled::instance().dismissSeederProgress();
                restoreWifi();
                return;
            }

            // Calculate transfer speed
            unsigned long elapsed = millis() - m_seedStartTimeMs;
            uint32_t speedKbps = elapsed > 0 ? (((uint32_t)resp->nextExpectedChunk * m_manifestChunkSize) / elapsed) : 0;

            // Update Seeder OLED display (stub on headless)
            DisplayOled::instance().showSeederProgress(resp->percent, resp->nextExpectedChunk, resp->totalChunks, nodeName, speedKbps);

            // Remote finished successfully
            if (resp->state == OTA_STATE_SUCCESS || resp->percent == 100) {
                Serial.printf("{\"type\":\"mesh_ota_seeder\",\"status\":\"neighbor_updated_successfully\",\"mac\":\"%s\",\"node\":\"%s\"}\n",
                              macStr, nodeName);
                MeshEngine::instance().updateNeighborVersion(hdr->srcMac, m_manifestVersion);
                m_isSeeding = false;
                DisplayOled::instance().dismissSeederProgress();
                restoreWifi();
                return;
            }

            // Sliding Window Flow Control & Adaptive Pacing:
            if (resp->nextExpectedChunk >= m_seedTotalChunks) {
                Serial.printf("{\"type\":\"mesh_ota_seeder\",\"status\":\"all_chunks_delivered\",\"chunks\":%u}\n", m_seedTotalChunks);
                m_isSeeding = false;
                DisplayOled::instance().dismissSeederProgress();
                restoreWifi();
                return;
            }

            unsigned long nowMs = millis();

            if (resp->nextExpectedChunk > m_seedWindowStart) {
                // POSITIVE ACK: Receiver cleanly advanced past window start!
                m_seedWindowStart = resp->nextExpectedChunk;
                m_seedWindowConsecutiveDrops = 0;
                if (m_seedPacingMs > 15) m_seedPacingMs--;

                if (m_seedWaitingAck) {
                    m_seedWaitingAck = false;
                    m_seedWindowRetries = 0;
                    m_seedQuerySent = false;
                    m_seedChunkIdx = resp->nextExpectedChunk;
                }
                // If !m_seedWaitingAck: actively streaming; keep advancing the stream without interruption!
            } else if (resp->nextExpectedChunk == m_seedWindowStart && m_seedChunkIdx > m_seedWindowStart) {
                // NACK: Receiver did not advance and is missing chunk at m_seedWindowStart
                bool isTrailingNack = (!m_seedWaitingAck && (nowMs - m_seedLastRewindMs < 60));
                if (!isTrailingNack) {
                    m_seedChunkIdx = m_seedWindowStart;
                    m_seedWaitingAck = false;
                    m_seedWindowRetries = 0;
                    m_seedQuerySent = false;
                    m_seedLastRewindMs = nowMs;
                    if (m_seedPacingMs < 35) m_seedPacingMs += 2;
                    // Hold off retransmission by 35ms to let receiver's radio finish
                    // transmitting the NACK packet and return to RX listening state!
                    m_seedHoldoffUntilMs = nowMs + 35;
                    m_lastSeedChunkMs = nowMs;
                }
            } else if (m_seedWaitingAck) {
                // Receiver is behind what we expected at end of window: rewind to receiver position
                m_seedChunkIdx = resp->nextExpectedChunk;
                m_seedWindowStart = resp->nextExpectedChunk;
                m_seedWaitingAck = false;
                m_seedWindowRetries = 0;
                m_seedQuerySent = false;
                m_seedLastRewindMs = nowMs;
                if (m_seedPacingMs < 35) m_seedPacingMs += 2;
                m_seedHoldoffUntilMs = nowMs + 35;
                m_lastSeedChunkMs = nowMs;
            }
            // Stale ACKs with resp->nextExpectedChunk <= m_seedWindowStart while !m_seedWaitingAck are ignored!
        }
    }

    void handleOtaAbort(const MeshPacketHdr* hdr, const MeshOtaAbortPayload* payload) {
        const uint8_t* myMac = MeshEngine::instance().getMyMac();
        if (memcmp(hdr->dstMac, myMac, 6) == 0 || memcmp(hdr->dstMac, MeshEngine::BROADCAST_MAC, 6) == 0) {
            if (m_state == OTA_STATE_RECEIVING) {
                Update.abort();
                mbedtls_sha256_free(&m_shaCtx);
                m_state = OTA_STATE_FAILED;
                m_errorCode = OTA_ERR_ABORTED;
                LedPatterns::instance().playSlot(0, PATTERN_OTA_FAILED, RgbColor::Red());
                DisplayOled::instance().showOtaProgress(0, m_nextExpectedChunk, m_totalChunks, "UPDATE ABORTED", m_version);
                restoreWifi();
                delay(1200);
                DisplayOled::instance().dismissOtaProgress();
                LedPatterns::instance().playSlot(0, PATTERN_HEARTBEAT, RgbColor::Cyan());
            }
            if (m_isSeeding) {
                m_isSeeding = false;
                DisplayOled::instance().dismissSeederProgress();
                restoreWifi();
                Serial.println("{\"type\":\"mesh_ota_seeder\",\"status\":\"aborted_by_remote\"}");
            }
        }
    }

    void sendStatusResp(const uint8_t* dstMac) {
        MeshOtaStatusRespPayload resp;
        resp.state = m_state;
        resp.nextExpectedChunk = m_nextExpectedChunk;
        resp.totalChunks = m_totalChunks;
        resp.percent = (m_totalChunks > 0) ? (uint8_t)(((uint32_t)m_nextExpectedChunk * 100) / m_totalChunks) : 0;
        resp.errorCode = m_errorCode;

        uint32_t id = MeshEngine::instance().generateMsgId();
        MeshEngine::instance().transmitPacket(
            MESH_MSG_OTA_STATUS_RESP,
            id,
            MeshEngine::instance().getMyMac(),
            dstMac,
            0,
            8,
            TimeSync::instance().getNowUs(),
            (const uint8_t*)&resp,
            sizeof(resp)
        );
    }

    void restoreWifi() {
        esp_wifi_set_promiscuous(true);
        if (m_wifiLocked) {
            if (m_origAutoHop) {
                WifiMonitor::instance().setChannel(0); // Restore auto-hop
            } else {
                WifiMonitor::instance().setChannel(m_origWifiChannel);
            }
            m_wifiLocked = false;
        }
    }

    MeshOtaEngineState m_state;
    MeshOtaErrorCode m_errorCode;
    uint32_t m_totalBytes;
    uint16_t m_chunkSize;
    uint16_t m_totalChunks;
    uint16_t m_nextExpectedChunk;
    uint32_t m_bytesWritten;
    uint8_t m_sha256Expected[32];
    uint8_t m_sig[72];
    uint8_t m_sigLen;
    char m_version[16];
    uint8_t m_senderMac[6];
    mbedtls_sha256_context m_shaCtx;

    unsigned long m_startTimeMs;
    unsigned long m_lastChunkMs;
    unsigned long m_lastNackMs;
    unsigned long m_rebootScheduledMs;
    bool m_rebootPending;

    uint8_t m_origWifiChannel;
    bool m_origAutoHop;
    bool m_wifiLocked;

    bool m_hasManifest;
    uint32_t m_manifestBytes;
    uint16_t m_manifestChunkSize;
    uint16_t m_manifestTotalChunks;
    uint8_t m_manifestSha256[32];
    uint8_t m_manifestSig[72];
    uint8_t m_manifestSigLen;
    char m_manifestVersion[16];
    uint8_t m_manifestHwType;

    bool m_isSeeding;
    uint8_t m_seedTargetMac[6];
    uint16_t m_seedChunkIdx;
    uint16_t m_seedWindowStart;
    uint16_t m_seedTotalChunks;
    bool m_seedWaitingAck;
    bool m_seedQuerySent;
    unsigned long m_seedHoldoffUntilMs;
    unsigned long m_seedLastRewindMs;
    unsigned long m_seedWindowSentMs;
    uint8_t m_seedWindowRetries;
    uint8_t m_seedWindowConsecutiveDrops;
    unsigned long m_lastSeedChunkMs;
    unsigned long m_lastSeedCheckMs;
    unsigned long m_seedStartTimeMs;
    uint16_t m_seedPacingMs;
};
