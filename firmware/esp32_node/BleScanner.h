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
        if (!m_tagQueue) {
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

        m_pRxChar = pService->createCharacteristic(
            CHARACTERISTIC_UUID_RX,
            NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
        );
        m_pRxChar->setCallbacks(new BleRxCallbacksImpl());

        pService->start();

        // Advertising
        NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
        pAdv->setName(nodeName);
        pAdv->addServiceUUID(SERVICE_UUID_NUS);
        pAdv->enableScanResponse(true);
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
    }
    void clientDisconnected() {
        if (m_connectedCount > 0) m_connectedCount--;
        if (m_connectedCount == 0) {
            LedPatterns::instance().setBasePattern(PATTERN_HEARTBEAT);
        }
    }

    void sendTelemetry(const char* line) {
        if (!m_pTxChar || m_connectedCount == 0) return;
        m_pTxChar->setValue((const uint8_t*)line, strlen(line));
        m_pTxChar->notify();
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
                   m_initialized(false) {}

    NimBLEServer* m_pServer;
    NimBLECharacteristic* m_pTxChar;
    NimBLECharacteristic* m_pRxChar;
    NimBLEScan* m_pScan;
    QueueHandle_t m_tagQueue;
    volatile int m_connectedCount;
    bool m_initialized;
};
