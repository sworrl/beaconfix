#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include "LedPatterns.h"

#define SERVICE_UUID_NUS        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

struct DetectedBleTag {
    char kind[16];
    char mac[18];
    char name[32];
    int8_t rssi;
    uint32_t timestampMs;
    char payloadHex[64];
};

class BleScanner;

class BleServerCallbacksImpl : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override;
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override;
};

class BleRxCallbacksImpl : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override;
};

// TX subscribe = the client is ready for notifications (dump the store-and-forward queue then, not on connect)
class BleTxCallbacksImpl : public NimBLECharacteristicCallbacks {
public:
    void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, uint16_t subValue) override;
};

class BleAdvertisedCallbacksImpl : public NimBLEScanCallbacks {
public:
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
};

class BleScanner {
public:
    static BleScanner& instance() {
        static BleScanner inst;
        return inst;
    }

    bool begin(const char* nodeName) {
        m_tagQueue = xQueueCreate(32, sizeof(DetectedBleTag));
        m_txMutex = xSemaphoreCreateMutex();
        if (!m_tagQueue || !m_txMutex) {
            return false;
        }

        NimBLEDevice::init(nodeName);
        NimBLEDevice::setMTU(256);

        // GATT Server
        m_pServer = NimBLEDevice::createServer();
        m_pServer->setCallbacks(new BleServerCallbacksImpl());

        NimBLEService* pService = m_pServer->createService(SERVICE_UUID_NUS);
        m_pTxChar = pService->createCharacteristic(
            CHARACTERISTIC_UUID_TX,
            NIMBLE_PROPERTY::NOTIFY
        );
        m_pTxChar->setCallbacks(new BleTxCallbacksImpl());

        m_pRxChar = pService->createCharacteristic(
            CHARACTERISTIC_UUID_RX,
            NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
        );
        m_pRxChar->setCallbacks(new BleRxCallbacksImpl());

        pService->start();

        // Advertising
        // The service UUID in the advertisement and the name in the scan response: a 20-character name and a 128-bit
        // UUID don't both fit in 31 bytes, and the UUID got dropped. Phones with the screen off only see nodes through
        // a scan filter, and the filter matches on the UUID.
        NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
        NimBLEAdvertisementData advData;
        advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
        advData.setCompleteServices(NimBLEUUID(SERVICE_UUID_NUS));
        pAdv->setAdvertisementData(advData);
        NimBLEAdvertisementData scanData;
        scanData.setName(nodeName);
        pAdv->setScanResponseData(scanData);
        pAdv->start();

        // Scanner setup
        m_pScan = NimBLEDevice::getScan();
        m_pScan->setScanCallbacks(new BleAdvertisedCallbacksImpl());
        m_pScan->setActiveScan(false);
        m_pScan->setInterval(200);
        m_pScan->setWindow(100);
        m_pScan->start(0, false, false); // Continuous background scan
        m_initialized = true;
        return true;
    }

    bool isConnected() const { return m_connectedCount > 0; }
    void clientConnected() {
        m_connectedCount++;
        LedPatterns::instance().setBasePattern(PATTERN_LINK_ACTIVE);
        // NimBLE stops advertising on connect; keep advertising so a phone and a desktop can both link
        if (m_connectedCount < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) NimBLEDevice::startAdvertising();
    }
    void clientDisconnected() {
        if (m_connectedCount > 0) m_connectedCount--;
        if (m_connectedCount == 0) {
            LedPatterns::instance().setBasePattern(PATTERN_HEARTBEAT);
        }
    }

    // One JSON line, newline-terminated so the client can reassemble it
    // False when it didn't all go out (no link, or the notify buffers stayed full)
    bool sendTelemetry(const char* line) {
        if (!line || !m_pTxChar || m_connectedCount == 0) return false;
        size_t len = strlen(line);
        if (len == 0) return false;
        if (line[len - 1] == '\n') { sendRaw(line, len); return true; }
        if (!m_txMutex || xSemaphoreTake(m_txMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
        const bool ok = sendChunksLocked((const uint8_t*)line, len) && sendChunksLocked((const uint8_t*)"\n", 1);
        xSemaphoreGive(m_txMutex);
        return ok;
    }

    // Bytes as-is (caller frames them). Split to the smallest peer ATT MTU: a notify longer than
    // MTU-3 is silently truncated, which cut every status heartbeat off at 253 bytes.
    void sendRaw(const char* data, size_t len) {
        if (!data || len == 0 || !m_pTxChar || m_connectedCount == 0) return;
        if (!m_txMutex || xSemaphoreTake(m_txMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
        sendChunksLocked((const uint8_t*)data, len);
        xSemaphoreGive(m_txMutex);
    }

    // BLE RX arrives on the NimBLE host task; commands run on loop() instead (popRxLine)
    void queueRx(const std::string& data) {
        portENTER_CRITICAL(&m_rxMux);
        if (m_rxBuf.size() + data.size() <= 1024) m_rxBuf += data;
        m_rxLastMs = millis();
        portEXIT_CRITICAL(&m_rxMux);
    }

    // Next newline-terminated command; an unterminated write counts as a command once 300 ms idle
    bool popRxLine(String& out) {
        bool got = false;
        portENTER_CRITICAL(&m_rxMux);
        size_t nl = m_rxBuf.find_first_of("\r\n");
        if (nl != std::string::npos) {
            out = String(m_rxBuf.substr(0, nl).c_str());
            m_rxBuf.erase(0, nl + 1);
            got = true;
        } else if (!m_rxBuf.empty() && millis() - m_rxLastMs > 300) {
            out = String(m_rxBuf.c_str());
            m_rxBuf.clear();
            got = true;
        }
        portEXIT_CRITICAL(&m_rxMux);
        if (got) out.trim();
        return got;
    }

    void requestQueueDump() { m_queueDumpPending = true; }
    bool takeQueueDumpRequest() {
        if (!m_queueDumpPending) return false;
        m_queueDumpPending = false;
        return true;
    }

    bool getNextTag(DetectedBleTag* outTag) {
        if (!m_tagQueue) return false;
        return xQueueReceive(m_tagQueue, outTag, 0) == pdTRUE;
    }

    void enqueueTag(const DetectedBleTag& tag) {
        if (m_tagQueue) {
            xQueueSend(m_tagQueue, &tag, 0);
        }
    }

    void processIncomingRx(const std::string& rxStr);

private:
    BleScanner() : m_pServer(nullptr), m_pTxChar(nullptr), m_pRxChar(nullptr),
                   m_pScan(nullptr), m_tagQueue(nullptr), m_connectedCount(0),
                   m_initialized(false), m_txMutex(nullptr), m_rxLastMs(0), m_queueDumpPending(false) {}

    bool sendChunksLocked(const uint8_t* data, size_t len) {
        uint16_t mtu = 0;
        for (uint16_t h : m_pServer->getPeerDevices()) {
            uint16_t m = m_pServer->getPeerMTU(h);
            if (m && (mtu == 0 || m < mtu)) mtu = m;
        }
        size_t chunk = (mtu > 23) ? (size_t)(mtu - 3) : 20;
        for (size_t off = 0; off < len; off += chunk) {
            size_t n = (len - off < chunk) ? (len - off) : chunk;
            // notify fails when the controller's buffers are full; give it a moment rather than drop mid-line
            bool ok = false;
            for (int tries = 0; tries < 10 && !(ok = m_pTxChar->notify(data + off, n)); tries++) delay(4);
            if (!ok) return false;
        }
        return true;
    }

    NimBLEServer* m_pServer;
    NimBLECharacteristic* m_pTxChar;
    NimBLECharacteristic* m_pRxChar;
    NimBLEScan* m_pScan;
    QueueHandle_t m_tagQueue;
    volatile int m_connectedCount;
    bool m_initialized;
    SemaphoreHandle_t m_txMutex;
    portMUX_TYPE m_rxMux = portMUX_INITIALIZER_UNLOCKED;
    std::string m_rxBuf;
    volatile unsigned long m_rxLastMs;
    volatile bool m_queueDumpPending;
};

inline void BleTxCallbacksImpl::onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, uint16_t subValue) {
    if (subValue) BleScanner::instance().requestQueueDump();
}
