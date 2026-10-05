#pragma once
#include <Arduino.h>
#include <RadioLib.h>
#include "HeltecV3Pins.h"
#include "MeshEngine.h"

// Forward declaration of ISR
static volatile bool g_loraPacketReceived = false;
#if defined(ESP32) || defined(ESP8266)
static IRAM_ATTR void onLoraDio1Action(void) {
    g_loraPacketReceived = true;
}
#else
static void onLoraDio1Action(void) {
    g_loraPacketReceived = true;
}
#endif

class LoraRadio {
public:
    static LoraRadio& instance() {
        static LoraRadio inst;
        return inst;
    }

    bool begin(float freq = 915.0f, float bw = 250.0f, uint8_t sf = 7, uint8_t cr = 5) {
        m_freq = freq;
        m_bw = bw;
        m_sf = sf;
        m_cr = cr;

        // Ensure Vext power rail is active on Heltec V3 (do not reset OLED)
        HeltecV3::enableVext(true);

        // Initialize RadioLib Module with Heltec V3 pin mapping:
        // NSS=8, DIO1=14, NRST=12, BUSY=13
        m_mod = new Module(
            HeltecV3::PIN_LORA_NSS,
            HeltecV3::PIN_LORA_DIO1,
            HeltecV3::PIN_LORA_RST,
            HeltecV3::PIN_LORA_BUSY
        );

        m_radio = new SX1262(m_mod);

        // Heltec V3 SX1262 initialization:
        // TCXO reference on DIO3 is powered at 1.8V
        // begin(freq, bw, sf, cr, syncWord, power, preambleLength, tcxoVoltage, useRegulatorLDO)
        int state = m_radio->begin(m_freq, m_bw, m_sf, m_cr, 0x12, 22, 8, 1.8f, false);
        if (state != RADIOLIB_ERR_NONE) {
            Serial.printf("{\"type\":\"error\",\"component\":\"lora\",\"code\":%d,\"msg\":\"sx1262_init_failed\"}\n", state);
            m_ready = false;
            return false;
        }

        // Set up asynchronous packet reception on DIO1
        m_radio->setDio1Action(onLoraDio1Action);
        state = m_radio->startReceive();
        if (state != RADIOLIB_ERR_NONE) {
            Serial.printf("{\"type\":\"error\",\"component\":\"lora\",\"code\":%d,\"msg\":\"start_receive_failed\"}\n", state);
            m_ready = false;
            return false;
        }

        m_ready = true;

        // Load persistent antenna configuration if saved, else auto-detect
        Preferences p;
        p.begin("bcf_ant", false);
        if (p.isKey("has_ant")) {
            m_antennaDetected = p.getBool("has_ant", true);
            m_txInhibited = !m_antennaDetected;
            m_manualOverride = true;
        } else {
            m_manualOverride = false;
            checkAntennaPresence();
        }
        p.end();

        // Apply TX power level based on Base Station vs Mobile mode
        setBaseStationMode(NodeConfig::instance().isBaseStation());

        // Register LoRa transmission hook with MeshEngine so all mesh frames bridge over LoRa
        MeshEngine::instance().registerRadioTxHook([](const uint8_t* d, size_t l) {
            LoraRadio::instance().transmitPacket(d, l);
        });

        Serial.printf("{\"type\":\"init\",\"component\":\"lora\",\"chip\":\"SX1262\",\"freq\":%.2f,\"bw\":%.1f,\"sf\":%u,\"power\":%d,\"antenna\":%s,\"tx_inhibited\":%s,\"ambient_rssi\":%d,\"status\":\"listening\"}\n",
                      m_freq, m_bw, m_sf, m_powerDbm, m_antennaDetected ? "connected" : "DISCONNECTED_WARNING",
                      m_txInhibited ? "true" : "false", m_ambientRssi);
        return true;
    }

    bool isReady() const { return m_ready; }
    uint32_t getTxCount() const { return m_txCount; }
    uint32_t getRxCount() const { return m_rxCount; }
    int8_t getLastRssi() const { return m_lastRssi; }
    int8_t getLastSnr() const { return m_lastSnr; }
    float getFreq() const { return m_freq; }
    int8_t getPowerDbm() const { return m_powerDbm; }

    bool checkAntennaPresence() {
        if (!m_ready || !m_radio) return false;

        if (m_manualOverride) {
            return m_antennaDetected;
        }

        // Measure INSTANTANEOUS ambient RF floor on 915 MHz band (pass false for RADIOLIB_SX126X_CMD_GET_RSSI_INST)
        // With no antenna on the IPEX/SMA connector, the isolated front-end reads <= -120 dBm with near-zero noise.
        // With an antenna attached, ambient thermal noise and 902-928 MHz ISM band energy measures between -95 and -115 dBm.
        float sumRssi = 0;
        for (int i = 0; i < 4; i++) {
            sumRssi += m_radio->getRSSI(false);
            delayMicroseconds(200);
        }
        float rssi = sumRssi / 4.0f;
        m_ambientRssi = (int8_t)rssi;

        if (rssi <= -120.0f) {
            m_antennaDetected = false;
            m_txInhibited = true; // Protect PA from transmitting into an open load!
        } else {
            m_antennaDetected = true;
            m_txInhibited = false;
        }
        return m_antennaDetected;
    }

    bool hasAntenna() const { return m_antennaDetected; }
    bool isTxInhibited() const { return m_txInhibited; }
    void setTxInhibited(bool en) { m_txInhibited = en; }

    void clearManualOverride() {
        m_manualOverride = false;
        Preferences p;
        p.begin("bcf_ant", false);
        p.remove("has_ant");
        p.end();
    }

    void setAntennaDetected(bool detected, bool manual = true) {
        m_antennaDetected = detected;
        m_txInhibited = !detected;
        m_manualOverride = manual;
        if (manual) {
            Preferences p;
            p.begin("bcf_ant", false);
            p.putBool("has_ant", detected);
            p.end();
        }
        setBaseStationMode(NodeConfig::instance().isBaseStation());
    }
    int8_t getAmbientRssi() const { return m_ambientRssi; }

    void setBaseStationMode(bool isBaseStation) {
        if (!m_ready || !m_radio) return;
        // Base Station: +22 dBm full output on continuous host power (if antenna safe)
        // Mobile Mode: +14 dBm balanced output saving battery current on vape cells
        int8_t pwr = isBaseStation ? 22 : 14;
        if (m_txInhibited || !m_antennaDetected) pwr = 2; // Throttle down to minimum if no antenna detected
        m_radio->setOutputPower(pwr);
        m_powerDbm = pwr;
    }

    bool transmitPacket(const uint8_t* data, size_t len) {
        if (!m_ready || !m_radio || len == 0) return false;

        // Hardware protection: Never transmit at +22dBm into an open SMA connector!
        if (m_txInhibited || !m_antennaDetected) {
            Serial.println("{\"type\":\"warn\",\"component\":\"lora\",\"action\":\"tx_inhibited\",\"reason\":\"no_antenna_protection\"}");
            return false;
        }

        // Transmit blocking (fast at SF7, ~20ms airtime)
        int state = m_radio->transmit(data, len);

        // Re-arm receiver
        m_radio->startReceive();

        if (state == RADIOLIB_ERR_NONE) {
            m_txCount++;
            return true;
        } else {
            Serial.printf("{\"type\":\"warn\",\"component\":\"lora\",\"action\":\"tx_failed\",\"code\":%d}\n", state);
            return false;
        }
    }

    void update() {
        if (!m_ready || !m_radio) return;

        unsigned long now = millis();
        if (now - m_lastAntennaCheckMs >= 4000) {
            m_lastAntennaCheckMs = now;
            checkAntennaPresence();
        }

        if (g_loraPacketReceived) {
            g_loraPacketReceived = false;

            uint8_t buffer[256];
            size_t len = m_radio->getPacketLength();

            if (len > 0 && len <= sizeof(buffer)) {
                int state = m_radio->readData(buffer, len);
                if (state == RADIOLIB_ERR_NONE) {
                    m_rxCount++;
                    m_lastRssi = (int8_t)m_radio->getRSSI();
                    m_lastSnr = (int8_t)m_radio->getSNR();

                    // Check for BeaconFix Mesh packet magic (0xBF, 0x4D)
                    if (len >= sizeof(MeshPacketHdr) && buffer[0] == MESH_MAGIC_0 && buffer[1] == MESH_MAGIC_1) {
                        const MeshPacketHdr* hdr = (const MeshPacketHdr*)buffer;
                        // Forward into MeshEngine: bridges LoRa packet straight into mesh!
                        MeshEngine::instance().handleRawFrame(hdr->prevHopMac, buffer, len, m_lastRssi, 0);
                    }
                }
            }

            // Re-arm receiver
            m_radio->startReceive();
        }
    }

private:
    LoraRadio() : m_mod(nullptr), m_radio(nullptr), m_ready(false),
                  m_txCount(0), m_rxCount(0), m_lastRssi(0), m_lastSnr(0),
                  m_freq(915.0f), m_bw(250.0f), m_sf(7), m_cr(5), m_powerDbm(22),
                  m_antennaDetected(true), m_txInhibited(false), m_manualOverride(false),
                  m_ambientRssi(-110), m_lastAntennaCheckMs(0) {}

    Module* m_mod;
    SX1262* m_radio;
    bool m_ready;
    uint32_t m_txCount;
    uint32_t m_rxCount;
    int8_t m_lastRssi;
    int8_t m_lastSnr;
    float m_freq;
    float m_bw;
    uint8_t m_sf;
    uint8_t m_cr;
    int8_t m_powerDbm;
    bool m_antennaDetected;
    bool m_txInhibited;
    bool m_manualOverride;
    int8_t m_ambientRssi;
    unsigned long m_lastAntennaCheckMs;
};
