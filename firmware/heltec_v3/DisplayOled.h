#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "HeltecV3Pins.h"
#include "NodeConfig.h"
#include "BatteryMonitor.h"
#include "WifiMonitor.h"
#include "TimeSync.h"
#include "MeshEngine.h"
#include "LoraRadio.h"

// ── Master Low-Detail BeaconFix Icons (Converted from data/icons/) ────────────
// 16x16 Official BeaconFix Icon (32 bytes)
static const uint8_t BEACONFIX_ICON_16[] PROGMEM = {
    0xe0, 0x07, 0x08, 0x10, 0x04, 0x20, 0x02, 0x40,
    0x00, 0x00, 0x01, 0x80, 0x81, 0x81, 0x81, 0x01,
    0x01, 0x00, 0x01, 0x80, 0x01, 0x80, 0x00, 0x00,
    0x02, 0x40, 0x04, 0x20, 0x08, 0x10, 0xe0, 0x07
};

// 24x24 Official BeaconFix Icon (72 bytes)
static const uint8_t BEACONFIX_ICON_24[] PROGMEM = {
    0x00, 0xff, 0x00, 0xc0, 0x01, 0x03, 0x20, 0x00, 0x04, 0x10, 0x00, 0x18,
    0x08, 0x00, 0x10, 0x04, 0x00, 0x20, 0x02, 0x00, 0x40, 0x02, 0x00, 0x40,
    0x03, 0x00, 0x80, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80,
    0x81, 0x24, 0x81, 0x11, 0x04, 0x88, 0x01, 0x42, 0x88, 0x01, 0x00, 0x88,
    0x12, 0x00, 0x48, 0x12, 0x00, 0x48, 0x84, 0x00, 0x29, 0x08, 0x00, 0x10,
    0x18, 0x00, 0x18, 0x20, 0x00, 0x04, 0xc0, 0x00, 0x03, 0x00, 0xff, 0x00
};

struct AlprAlert {
    char operatorName[24];
    char model[20];
    float distanceM;
    uint8_t confidence;
    int8_t facing; // 1 = faced you, -1 = faced away, 0 = unknown
    float speedKmh;
    double lat;
    double lon;
    unsigned long triggerMs;
    uint32_t durationMs;
    bool active;
};

class DisplayOled {
public:
    static constexpr uint8_t NUM_PAGES = 10;
    static constexpr uint8_t HIST_BINS = 60; // 60 seconds rolling history

    void triggerAlprAlert(const char* op, const char* model, float distM, uint8_t conf = 95,
                          int8_t facing = 1, float speedKmh = -1.0f, double lat = 0.0,
                          double lon = 0.0, uint32_t durationMs = 8000) {
        strncpy(m_alpr.operatorName, op ? op : "FLOCK SAFETY", sizeof(m_alpr.operatorName) - 1);
        m_alpr.operatorName[sizeof(m_alpr.operatorName) - 1] = '\0';
        for (int i = 0; m_alpr.operatorName[i]; i++) {
            if (m_alpr.operatorName[i] == '_') m_alpr.operatorName[i] = ' ';
        }
        strncpy(m_alpr.model, model ? model : "Falcon Flex", sizeof(m_alpr.model) - 1);
        m_alpr.model[sizeof(m_alpr.model) - 1] = '\0';
        for (int i = 0; m_alpr.model[i]; i++) {
            if (m_alpr.model[i] == '_') m_alpr.model[i] = ' ';
        }
        m_alpr.distanceM = distM;
        m_alpr.confidence = conf;
        m_alpr.facing = facing;
        m_alpr.speedKmh = (speedKmh >= 0.0f) ? speedKmh : NodeConfig::instance().getSpeedKmh();
        m_alpr.lat = (lat != 0.0) ? lat : NodeConfig::instance().getLat();
        m_alpr.lon = (lon != 0.0) ? lon : NodeConfig::instance().getLon();
        m_alpr.triggerMs = millis();
        m_alpr.durationMs = durationMs;
        m_alpr.active = true;
        m_alprPassCount++;
        noteActivity();
    }

    void dismissAlprAlert() {
        m_alpr.active = false;
        noteActivity();
    }

    bool isAlprAlertActive() const {
        return m_alpr.active;
    }

    uint32_t getAlprPassCount() const {
        return m_alprPassCount;
    }

    void showOtaProgress(uint8_t percent, uint16_t chunk, uint16_t totalChunks, const char* statusMsg, const char* version = "3.10.0") {
        m_otaActive = true;
        m_otaPercent = percent;
        m_otaChunk = chunk;
        m_otaTotalChunks = totalChunks;
        if (statusMsg) {
            strncpy(m_otaStatusMsg, statusMsg, sizeof(m_otaStatusMsg) - 1);
            m_otaStatusMsg[sizeof(m_otaStatusMsg) - 1] = '\0';
        }
        if (version) {
            strncpy(m_otaVersion, version, sizeof(m_otaVersion) - 1);
            m_otaVersion[sizeof(m_otaVersion) - 1] = '\0';
        }
        setScreenEnabled(true);
        noteActivity();
    }

    void dismissOtaProgress() {
        m_otaActive = false;
        noteActivity();
    }

    bool isOtaActive() const {
        return m_otaActive;
    }

    void showSeederProgress(uint8_t percent, uint16_t chunk, uint16_t totalChunks, const char* targetNode, uint32_t speedKbps) {
        m_seederActive = true;
        m_seederPercent = percent;
        m_seederChunk = chunk;
        m_seederTotalChunks = totalChunks;
        m_seederSpeedKbps = speedKbps;
        if (targetNode) {
            strncpy(m_seederTargetNode, targetNode, sizeof(m_seederTargetNode) - 1);
            m_seederTargetNode[sizeof(m_seederTargetNode) - 1] = '\0';
        }
        setScreenEnabled(true);
        noteActivity();
    }

    void dismissSeederProgress() {
        m_seederActive = false;
        noteActivity();
    }

    bool isSeederActive() const {
        return m_seederActive;
    }

    static uint8_t calculateLinkQuality(int8_t rssi, int8_t snr) {
        if (rssi <= -120) return 0;
        if (rssi >= -50) return 100;
        int q = ((int)rssi + 120) * 100 / 70;
        if (snr > 0) q += (snr * 2);
        else if (snr < -5) q -= (-snr * 3);
        if (q < 0) q = 0;
        if (q > 100) q = 100;
        return (uint8_t)q;
    }

    static uint8_t rssiToBars(int8_t rssi) {
        if (rssi >= -70) return 4;
        if (rssi >= -85) return 3;
        if (rssi >= -100) return 2;
        if (rssi >= -115) return 1;
        return 0;
    }

    void drawSignalBars(int16_t x, int16_t y, uint8_t bars) {
        if (!m_u8g2) return;
        for (uint8_t b = 0; b < 4; b++) {
            uint8_t barH = 2 + (b * 2);
            int16_t barX = x + (b * 3);
            int16_t barY = (y + 8) - barH;
            if (b < bars) {
                m_u8g2->drawVLine(barX, barY, barH);
                m_u8g2->drawVLine(barX + 1, barY, barH);
            } else {
                m_u8g2->drawPixel(barX, y + 7);
            }
        }
    }

    static DisplayOled& instance() {
        static DisplayOled inst;
        return inst;
    }

    bool begin() {
        // 1. Ensure Vext power rail is active on Heltec V3
        HeltecV3::enableVext(true);

        // 2. Hardware reset sequence for OLED panel
        HeltecV3::resetOled();

        // 3. Initialize I2C on Heltec V3 OLED pins (SDA=17, SCL=18)
        Wire.begin(HeltecV3::PIN_OLED_SDA, HeltecV3::PIN_OLED_SCL);
        Wire.setClock(400000); // 400kHz Fast I2C

        // 4. Instantiate U8g2 driver with explicit reset, clock, and data pins
        m_u8g2 = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(
            U8G2_R0,
            HeltecV3::PIN_OLED_RST,
            HeltecV3::PIN_OLED_SCL,
            HeltecV3::PIN_OLED_SDA
        );

        if (!m_u8g2->begin()) {
            Serial.println("{\"type\":\"error\",\"component\":\"display\",\"msg\":\"ssd1306_init_failed\"}");
            m_ready = false;
            return false;
        }

        m_ready = true;
        m_screenEnabled = true;
        m_autoCycle = true;
        m_currentPage = 0;
        m_lastPageFlipMs = millis();
        m_lastUserActivityMs = millis();
        m_btnPressStartMs = 0;
        m_btnHandledHold = false;
        m_lastBtnState = HIGH;
        m_histHead = 0;
        m_peakPps = 10;
        memset(m_ppsHistory, 0, sizeof(m_ppsHistory));

        // Draw boot splash with official BeaconFix icon & station mode
        drawSplash();
        return true;
    }

    void drawSplash() {
        if (!m_ready || !m_u8g2) return;
        m_u8g2->clearBuffer();

        // Official BeaconFix 24x24 low res icon
        m_u8g2->drawXBMP(6, 18, 24, 24, BEACONFIX_ICON_24);

        // Branding & title
        m_u8g2->setFont(u8g2_font_ncenB10_tr);
        m_u8g2->drawStr(36, 26, "BEACONFIX");

        m_u8g2->setFont(u8g2_font_6x10_tr);
        if (NodeConfig::instance().isBaseStation()) {
            m_u8g2->drawStr(36, 39, "BASE STATION (GW)");
        } else {
            m_u8g2->drawStr(36, 39, "MOBILE FIELD NODE");
        }

        m_u8g2->setFont(u8g2_font_5x7_tr);
        m_u8g2->drawStr(36, 52, "v" BEACONFIX_FW_VERSION " LoRa+Wi-Fi Mesh");

        m_u8g2->sendBuffer();
        delay(1200);
    }

    void showModeToast(NodeOpMode mode) {
        if (!m_u8g2 || !m_ready) return;
        m_screenEnabled = true;
        m_u8g2->setPowerSave(0);
        m_u8g2->clearBuffer();
        m_u8g2->drawXBMP(6, 18, 24, 24, BEACONFIX_ICON_24);
        m_u8g2->setFont(u8g2_font_7x14B_tr);
        m_u8g2->drawStr(36, 26, "MODE CHANGED");
        m_u8g2->setFont(u8g2_font_6x10_tr);
        if (mode == OP_MODE_BASE_STATION) {
            m_u8g2->drawStr(36, 40, "BASE STATION");
            m_u8g2->setFont(u8g2_font_5x7_tr);
            m_u8g2->drawStr(36, 52, "Root GW | LoRa +22dBm");
        } else {
            m_u8g2->drawStr(36, 40, "MOBILE NODE");
            m_u8g2->setFont(u8g2_font_5x7_tr);
            m_u8g2->drawStr(36, 52, "Batt-Opt | 60s Sleep");
        }
        m_u8g2->sendBuffer();
        delay(1200);
    }

    void pushTrafficSample(uint16_t pps) {
        m_ppsHistory[m_histHead] = pps;
        m_histHead = (m_histHead + 1) % HIST_BINS;

        // Recalculate dynamic peak for histogram scaling
        uint16_t maxV = 10;
        for (uint8_t i = 0; i < HIST_BINS; i++) {
            if (m_ppsHistory[i] > maxV) maxV = m_ppsHistory[i];
        }
        m_peakPps = maxV;
    }

    void noteActivity() {
        m_lastUserActivityMs = millis();
        if (!m_screenEnabled) {
            setScreenEnabled(true);
        }
    }

    void setScreenEnabled(bool on) {
        if (m_screenEnabled == on) return;
        m_screenEnabled = on;
        if (m_u8g2 && m_ready) {
            m_u8g2->setPowerSave(on ? 0 : 1);
        }
        if (on) {
            m_lastUserActivityMs = millis();
        }
    }

    bool isScreenEnabled() const { return m_screenEnabled; }

    void toggleScreen() {
        setScreenEnabled(!m_screenEnabled);
    }

    void setContrast(uint8_t val) {
        if (m_u8g2 && m_ready) {
            m_u8g2->setContrast(val);
        }
    }

    void setPage(uint8_t page) {
        if (page < NUM_PAGES) {
            m_currentPage = page;
            m_lastPageFlipMs = millis();
            noteActivity();
        }
    }

    void nextPage() {
        m_currentPage = (m_currentPage + 1) % NUM_PAGES;
        m_lastPageFlipMs = millis();
        noteActivity();
    }

    void prevPage() {
        m_currentPage = (m_currentPage + NUM_PAGES - 1) % NUM_PAGES;
        m_lastPageFlipMs = millis();
        noteActivity();
    }

    void setAutoCycle(bool en) {
        m_autoCycle = en;
    }

    bool isAutoCycle() const { return m_autoCycle; }
    uint8_t getCurrentPage() const { return m_currentPage; }

    void update(uint32_t loraTx, uint32_t loraRx, int8_t lastRssi, int8_t lastSnr, float loraFreq) {
        if (!m_ready || !m_u8g2) return;

        unsigned long now = millis();

        // Check if ALPR alert timed out
        if (m_alpr.active && (now - m_alpr.triggerMs >= m_alpr.durationMs)) {
            m_alpr.active = false;
        }

        // 1. Two-Way Screen Navigation using PRG Button (GPIO 0)
        // Click (< 350ms): Next Page (Forward)
        // Double Click (< 350ms): Previous Page (Backward)
        // Hold (> 400ms): Previous Page (Backward)
        // Click during ALPR Alert: Dismiss Splash immediately
        // Manual button activity pauses auto-cycle for 60 seconds
        int btn = digitalRead(HeltecV3::PIN_USER_BUTTON);
        if (btn == LOW && m_lastBtnState == HIGH) {
            // Button pressed down
            m_btnPressStartMs = now;
            m_btnHandledHold = false;
            m_lastUserActivityMs = now;

            if (m_alpr.active) {
                m_alpr.active = false;
                m_btnHandledHold = true; // Don't flip page on this press
            }
        } else if (btn == LOW && !m_btnHandledHold && (now - m_btnPressStartMs >= 400)) {
            // Button held for > 400ms -> Navigate BACKWARD
            m_btnHandledHold = true;
            m_singleClickPending = false; // Cancel any forward click
            prevPage();
            m_lastUserActivityMs = now;
        } else if (btn == HIGH && m_lastBtnState == LOW) {
            // Button released
            if (!m_btnHandledHold) {
                m_lastUserActivityMs = now;
                if (!m_screenEnabled) {
                    setScreenEnabled(true);
                } else {
                    if (now - m_lastReleaseMs <= 350) {
                        // Double-click -> Navigate BACKWARD
                        m_singleClickPending = false;
                        prevPage();
                    } else {
                        // First click -> queue forward page navigation
                        m_singleClickPending = true;
                        m_singleClickTimerMs = now;
                    }
                }
                m_lastReleaseMs = now;
            }
        }
        m_lastBtnState = btn;

        // Resolve pending single click after 300 ms -> FORWARD
        if (m_singleClickPending && (now - m_singleClickTimerMs > 300)) {
            m_singleClickPending = false;
            nextPage();
        }

        // 2. Mobile Mode Auto-Blank Power Saving (blank after 60s idle on battery)
        if (NodeConfig::instance().isMobile() && m_screenEnabled &&
            !BatteryMonitor::instance().isCharging() && BatteryMonitor::instance().hasBattery() && !m_alpr.active) {
            if (now - m_lastUserActivityMs >= 60000) {
                setScreenEnabled(false);
            }
        }

        // 3. If screen is blanked, skip frame rendering
        if (!m_screenEnabled) return;

        // 4. Auto-cycle pages if enabled (every 6 seconds, paused for 60s upon user button interaction or ALPR alert)
        bool userRecentlyActive = (now - m_lastUserActivityMs < 60000);
        if (m_autoCycle && !userRecentlyActive && !m_alpr.active) {
            unsigned long cycleInterval = NodeConfig::instance().isTraveling() ? 5000 : 6000;
            if (now - m_lastPageFlipMs >= cycleInterval) {
                if (NodeConfig::instance().isTraveling()) {
                    // While actively driving/moving, keep focus alternating between Main Dashboard & Travel Metrics
                    m_currentPage = (m_currentPage == 0) ? 1 : 0;
                } else {
                    m_currentPage = (m_currentPage + 1) % NUM_PAGES;
                }
                m_lastPageFlipMs = now;
            }
        }

        // 5. Render at 10 Hz (every 100ms for smooth organic cardiac heartbeat)
        if (now - m_lastRenderMs < 100) return;
        m_lastRenderMs = now;

        m_u8g2->clearBuffer();

        if (m_otaActive) {
            // Decoupled 1 Hz rendering during OTA: prevents I2C bus starvation while receiver writes flash
            static unsigned long lastOtaRenderMs = 0;
            if (now - lastOtaRenderMs >= 1000 || m_otaPercent == 100 || m_otaChunk == 0) {
                lastOtaRenderMs = now;
                renderOtaSplash();
                m_u8g2->sendBuffer();
            }
            return;
        }

        if (m_seederActive && !m_otaActive) {
            static unsigned long lastSeederRenderMs = 0;
            if (now - lastSeederRenderMs >= 1000 || m_seederPercent == 100 || m_seederChunk == 0) {
                lastSeederRenderMs = now;
                renderSeederSplash();
                m_u8g2->sendBuffer();
            }
            return;
        }

        if (m_alpr.active) {
            renderAlprSplash();
            m_u8g2->sendBuffer();
            return;
        }

        switch (m_currentPage) {
            case 0:
                renderMainDashboard(loraTx, loraRx, lastRssi, lastSnr);
                break;
            case 1:
                renderTravelMetrics();
                break;
            case 2:
                renderAlprStatus();
                break;
            case 3:
                renderRfThreatLog();
                break;
            case 4:
                renderLoraMesh(loraTx, loraRx, lastRssi, lastSnr, loraFreq);
                break;
            case 5:
                renderRfSpectrum();
                break;
            case 6:
                renderConnectionAudit(loraTx, loraRx, lastRssi, lastSnr, loraFreq);
                break;
            case 7:
                renderBatteryDiagnostics();
                break;
            case 8:
                renderAntennaDiagnostics();
                break;
            case 9:
                renderSystemInfo();
                break;
            default:
                m_currentPage = 0;
                renderMainDashboard(loraTx, loraRx, lastRssi, lastSnr);
                break;
        }

        m_u8g2->sendBuffer();
    }

private:
    DisplayOled() : m_u8g2(nullptr), m_ready(false), m_screenEnabled(true),
                    m_autoCycle(true), m_currentPage(0), m_lastPageFlipMs(0),
                    m_lastUserActivityMs(0), m_btnPressStartMs(0), m_btnHandledHold(false),
                    m_lastRenderMs(0), m_lastBtnState(HIGH), m_histHead(0),
                    m_peakPps(10), m_alprPassCount(0), m_lastReleaseMs(0),
                    m_singleClickPending(false), m_singleClickTimerMs(0),
                    m_otaActive(false), m_otaPercent(0), m_otaChunk(0), m_otaTotalChunks(0),
                    m_seederActive(false), m_seederPercent(0), m_seederChunk(0), m_seederTotalChunks(0),
                    m_seederSpeedKbps(0) {
        memset(m_ppsHistory, 0, sizeof(m_ppsHistory));
        memset(&m_alpr, 0, sizeof(m_alpr));
        memset(m_otaStatusMsg, 0, sizeof(m_otaStatusMsg));
        memset(m_otaVersion, 0, sizeof(m_otaVersion));
        memset(m_seederTargetNode, 0, sizeof(m_seederTargetNode));
    }

    void renderHeader() {
        const char* name = NodeConfig::instance().getName();
        uint8_t battPct = BatteryMonitor::instance().getPercentage();
        bool hasBatt = BatteryMonitor::instance().hasBattery();
        bool charging = BatteryMonitor::instance().isCharging();

        // 1. Official BeaconFix Icon (16x16) at top-left
        m_u8g2->drawXBMP(0, 0, 16, 16, BEACONFIX_ICON_16);

        // 2. Reddit-Style Node Name
        m_u8g2->setFont(u8g2_font_6x10_tr);
        m_u8g2->setClipWindow(18, 0, 63, 14);
        m_u8g2->drawStr(19, 11, name);
        m_u8g2->setMaxClipWindow();

        // 3. Antenna warning OR LoRa link status
        int8_t bRssi = MeshEngine::instance().getBaseStationRssi();
        if (!LoraRadio::instance().hasAntenna()) {
            m_u8g2->setFont(u8g2_font_4x6_tr);
            m_u8g2->drawFrame(64, 2, 14, 9);
            m_u8g2->drawStr(65, 9, "!A");
        } else if (NodeConfig::instance().isBaseStation()) {
            m_u8g2->setFont(u8g2_font_4x6_tr);
            m_u8g2->drawStr(66, 9, "GW");
        } else if (bRssi > -125) {
            drawSignalBars(66, 3, rssiToBars(bRssi));
        } else {
            drawSignalBars(66, 3, 0);
        }

        // 4. Synchronized Cardiac Heartbeat Indicator (Epoch Locked)
        uint32_t phaseMs = TimeSync::instance().getPhaseMs(1000);
        bool systole = (phaseMs < 190) || (phaseMs >= 260 && phaseMs < 420);
        static const uint8_t HEART_FILLED[7] PROGMEM = { 0x36, 0x7F, 0x7F, 0x7F, 0x3E, 0x1C, 0x08 };
        static const uint8_t HEART_OUTLINE[7] PROGMEM = { 0x36, 0x49, 0x41, 0x41, 0x22, 0x14, 0x08 };
        m_u8g2->drawBitmap(80, 3, 1, 7, systole ? HEART_FILLED : HEART_OUTLINE);

        // 5. Battery status at top-right
        m_u8g2->setFont(u8g2_font_5x7_tr);
        if (charging) {
            m_u8g2->drawFrame(91, 2, 24, 9);
            m_u8g2->drawBox(115, 5, 2, 3);
            m_u8g2->drawStr(93, 9, "CHG");
        } else if (hasBatt) {
            char pStr[8];
            snprintf(pStr, sizeof(pStr), "%u%%%s", battPct, BatteryMonitor::instance().isTrained() ? "*" : "");
            m_u8g2->drawStr(BatteryMonitor::instance().isTrained() ? 85 : 89, 9, pStr);

            m_u8g2->drawFrame(110, 2, 14, 9);
            m_u8g2->drawBox(124, 5, 2, 3);
            int barW = (battPct * 10) / 100;
            if (barW > 0) m_u8g2->drawBox(112, 4, barW, 5);
        } else {
            m_u8g2->drawStr(91, 9, "[USB]");
        }

        // Horizontal separator line
        m_u8g2->drawHLine(0, 15, 128);
    }

    void renderMainDashboard(uint32_t loraTx, uint32_t loraRx, int8_t lastRssi, int8_t lastSnr) {
        renderHeader();

        m_u8g2->setFont(u8g2_font_5x7_tr);
        char lineBuf[36];

        // Row 1: Mode & Gateway Link Status with Firmware Version
        if (NodeConfig::instance().isBaseStation()) {
            snprintf(lineBuf, sizeof(lineBuf), "BASE (GW) | v%s", BEACONFIX_FW_VERSION);
        } else if (NodeConfig::instance().isAttached()) {
            snprintf(lineBuf, sizeof(lineBuf), "ATT:%.8s | v%s",
                     NodeConfig::instance().getAttachedDevice(), BEACONFIX_FW_VERSION);
        } else {
            uint8_t hops = MeshEngine::instance().getHopsToGateway();
            if (hops < 254) {
                snprintf(lineBuf, sizeof(lineBuf), "MOB (H:%u Q:%u%%) | v%s",
                         hops, calculateLinkQuality(MeshEngine::instance().getBaseStationRssi(), lastSnr),
                         BEACONFIX_FW_VERSION);
            } else {
                snprintf(lineBuf, sizeof(lineBuf), "MOB (Search) | v%s", BEACONFIX_FW_VERSION);
            }
        }
        m_u8g2->drawStr(0, 24, lineBuf);

        // Row 2: LoRa SX1262 Connection & Link Quality
        if (!LoraRadio::instance().hasAntenna()) {
            snprintf(lineBuf, sizeof(lineBuf), "LORA: !NO ANT! TX INHIBITED");
        } else if (NodeConfig::instance().isBaseStation()) {
            snprintf(lineBuf, sizeof(lineBuf), "LORA: +22dBm (100%%) Mob:%u",
                     MeshEngine::instance().getActiveMobileCount());
        } else {
            int8_t bRssi = MeshEngine::instance().getBaseStationRssi();
            int8_t actRssi = (bRssi > -125) ? bRssi : lastRssi;
            uint8_t lq = calculateLinkQuality(actRssi, lastSnr);
            snprintf(lineBuf, sizeof(lineBuf), "LORA: %ddBm S:%+ddB Q:%u%%",
                     actRssi, lastSnr, lq);
        }
        m_u8g2->drawStr(0, 33, lineBuf);

        // Row 3: Wi-Fi Sniffer Status & Channel
        snprintf(lineBuf, sizeof(lineBuf), "WIFI: Ch %u (%u pps) Tot:%lu",
                 WifiMonitor::instance().getChannel(),
                 WifiMonitor::instance().getPps(),
                 WifiMonitor::instance().getTotalFrames());
        m_u8g2->drawStr(0, 42, lineBuf);

        // Row 4: TimeSync & BLE Status OR Live Trip Odometer
        float tripKm = NodeConfig::instance().getTripDistKm();
        bool hasTrip = (tripKm > 0.005f) || NodeConfig::instance().isTraveling();
        uint32_t tSec = millis() / 2500;
        if (hasTrip && (NodeConfig::instance().isTraveling() || (tSec % 2 == 0))) {
            float spd = NodeConfig::instance().getSpeedKmh();
            const char* card = NodeConfig::instance().getHeadingCard();
            snprintf(lineBuf, sizeof(lineBuf), "TRIP: %.2fkm | %.0fkm/h %s",
                     tripKm, spd > 0.5f ? spd : 0.0f, card);
        } else {
            bool synced = TimeSync::instance().isSynced();
            uint8_t strat = TimeSync::instance().getStratum();
            int64_t offUs = TimeSync::instance().getLastOffsetUs();
            bool bleConn = BleScanner::instance().isConnected();
            snprintf(lineBuf, sizeof(lineBuf), "SYNC: S%u %ldus | BLE:%s",
                     strat, (long)abs(offUs), bleConn ? "LINK" : "ADV");
        }
        m_u8g2->drawStr(0, 51, lineBuf);

        // Mini 60s Traffic Histogram (y=53..63, 10px high)
        renderMiniHistogram(2, 53, 94, 10);

        // Page dots
        drawPageDots(0);
    }

    void renderAlprSplash() {
        m_u8g2->clearBuffer();

        // 1. Inverted High-Priority Alert Banner
        m_u8g2->drawBox(0, 0, 128, 12);
        m_u8g2->setDrawColor(0); // White text on dark
        m_u8g2->setFont(u8g2_font_7x14B_tr);
        m_u8g2->drawStr(4, 11, "[!] ALPR DETECTED [!]");
        m_u8g2->setDrawColor(1); // Restore standard foreground

        // 2. Camera Operator & Model
        m_u8g2->setFont(u8g2_font_6x10_tr);
        char buf[36];
        snprintf(buf, sizeof(buf), "%.13s %.7s", m_alpr.operatorName, m_alpr.model);
        m_u8g2->drawStr(0, 23, buf);

        // 3. Distance & Confidence
        m_u8g2->setFont(u8g2_font_5x7_tr);
        snprintf(buf, sizeof(buf), "Dist: %.0fm | Conf: %u%%", m_alpr.distanceM, m_alpr.confidence);
        m_u8g2->drawStr(0, 32, buf);

        // 4. Camera Facing / Orientation
        const char* fStr = (m_alpr.facing == 1) ? "FACED YOU (Front Read)" :
                           (m_alpr.facing == -1) ? "FACED AWAY (Rear Read)" : "Facing: Unknown";
        snprintf(buf, sizeof(buf), "%s", fStr);
        m_u8g2->drawStr(0, 41, buf);

        // 5. Travel & Speed at Detection
        snprintf(buf, sizeof(buf), "Speed: %.0fkm/h (%s)",
                 m_alpr.speedKmh, NodeConfig::instance().getTravelStateStr());
        m_u8g2->drawStr(0, 50, buf);

        // 6. Coordinates & Dismiss Hint
        if (m_alpr.lat != 0.0 && m_alpr.lon != 0.0) {
            snprintf(buf, sizeof(buf), "%.4f, %.4f", m_alpr.lat, m_alpr.lon);
            m_u8g2->drawStr(0, 59, buf);
        } else {
            m_u8g2->drawStr(0, 59, "Loc: Attached Device");
        }

        // Timeout progress bar at bottom (y=62..63)
        uint32_t elapsed = millis() - m_alpr.triggerMs;
        if (elapsed < m_alpr.durationMs) {
            uint8_t barW = 128 - ((elapsed * 128) / m_alpr.durationMs);
            m_u8g2->drawHLine(0, 62, barW);
            m_u8g2->drawHLine(0, 63, barW);
        }
    }

    void renderOtaSplash() {
        m_u8g2->clearBuffer();

        // 1. Inverted High-Priority Title Banner
        m_u8g2->drawBox(0, 0, 128, 12);
        m_u8g2->setDrawColor(0); // White text on dark
        m_u8g2->setFont(u8g2_font_7x14B_tr);
        m_u8g2->drawStr(6, 11, "MESH OTA UPDATE");
        m_u8g2->setDrawColor(1);

        // 2. Version & Mode
        m_u8g2->setFont(u8g2_font_6x10_tr);
        char buf[36];
        snprintf(buf, sizeof(buf), "Target: v%s (Signed)", m_otaVersion);
        m_u8g2->drawStr(0, 23, buf);

        // 3. Progress Bar (108 px wide, centered at x=10, y=26..35)
        m_u8g2->drawFrame(10, 26, 108, 10);
        uint8_t fillW = (m_otaPercent * 104) / 100;
        if (fillW > 0) {
            m_u8g2->drawBox(12, 28, fillW, 6);
        }

        // 4. Progress percentage & Chunk count
        m_u8g2->setFont(u8g2_font_5x7_tr);
        snprintf(buf, sizeof(buf), "%u%% | Chunk %u/%u", m_otaPercent, m_otaChunk, m_otaTotalChunks);
        m_u8g2->drawStr(10, 46, buf);

        // 5. Status / Speed Line
        snprintf(buf, sizeof(buf), "%s", m_otaStatusMsg);
        m_u8g2->drawStr(0, 55, buf);

        // 6. Source & Transport
        snprintf(buf, sizeof(buf), "Mesh ESP-NOW | Ch %u", WifiMonitor::instance().getChannel());
        m_u8g2->drawStr(0, 63, buf);
    }

    void renderSeederSplash() {
        m_u8g2->clearBuffer();

        // 1. Inverted High-Priority Title Banner
        m_u8g2->drawBox(0, 0, 128, 12);
        m_u8g2->setDrawColor(0); // White text on dark
        m_u8g2->setFont(u8g2_font_7x14B_tr);
        m_u8g2->drawStr(6, 11, "MESH OTA SEEDER");
        m_u8g2->setDrawColor(1);

        // 2. Target Node Name
        m_u8g2->setFont(u8g2_font_6x10_tr);
        char buf[36];
        snprintf(buf, sizeof(buf), "To: %s", m_seederTargetNode);
        m_u8g2->drawStr(0, 23, buf);

        // 3. Progress Bar (108 px wide, centered at x=10, y=26..35)
        m_u8g2->drawFrame(10, 26, 108, 10);
        uint8_t fillW = (m_seederPercent * 104) / 100;
        if (fillW > 0) {
            m_u8g2->drawBox(12, 28, fillW, 6);
        }

        // 4. Progress percentage & Chunk count
        m_u8g2->setFont(u8g2_font_5x7_tr);
        snprintf(buf, sizeof(buf), "%u%% | Chunk %u/%u", m_seederPercent, m_seederChunk, m_seederTotalChunks);
        m_u8g2->drawStr(10, 46, buf);

        // 5. Speed Line
        snprintf(buf, sizeof(buf), "SPEED: %lu KB/s", (unsigned long)m_seederSpeedKbps);
        m_u8g2->drawStr(0, 55, buf);

        // 6. Source & Transport
        snprintf(buf, sizeof(buf), "Mesh ESP-NOW | Ch %u", WifiMonitor::instance().getChannel());
        m_u8g2->drawStr(0, 63, buf);
    }

    void renderTravelMetrics() {
        renderHeader();
        m_u8g2->setFont(u8g2_font_6x10_tr);
        char buf[36];

        // Line 1: Traveling state
        bool moving = NodeConfig::instance().isTraveling();
        bool hasFix = NodeConfig::instance().hasGpsFix();
        bool hasLoc = NodeConfig::instance().hasLocation();
        float tripDist = NodeConfig::instance().getTripDistKm();

        if (moving) {
            snprintf(buf, sizeof(buf), "TRAVEL: MOVING (Active)");
        } else if (hasFix) {
            snprintf(buf, sizeof(buf), "TRAVEL: STATIONARY");
        } else if (tripDist > 0.005f || hasLoc) {
            snprintf(buf, sizeof(buf), "TRAVEL: SYNCED (Hold)");
        } else {
            snprintf(buf, sizeof(buf), "TRAVEL: ACQUIRING FIX");
        }
        m_u8g2->drawStr(0, 26, buf);

        // Line 2: Speed
        m_u8g2->setFont(u8g2_font_5x7_tr);
        float kmh = NodeConfig::instance().getSpeedKmh();
        float mph = NodeConfig::instance().getSpeedMph();
        float maxKmh = NodeConfig::instance().getMaxSpeedKmh();
        snprintf(buf, sizeof(buf), "Speed : %.1f km/h (%.1f mph)", kmh, mph);
        m_u8g2->drawStr(0, 36, buf);

        // Line 3: Heading & Altitude
        float hdg = NodeConfig::instance().getHeadingDeg();
        const char* card = NodeConfig::instance().getHeadingCard();
        float alt = NodeConfig::instance().getAltM();
        if (hdg >= 0) {
            snprintf(buf, sizeof(buf), "Course: %.0f\xb0 %s | Alt: %.0fm", hdg, card, alt);
        } else {
            snprintf(buf, sizeof(buf), "Course: -- | Alt: %.0fm", alt);
        }
        m_u8g2->drawStr(0, 46, buf);

        // Line 4: Trip Odometer & Peak Speed
        snprintf(buf, sizeof(buf), "Trip  : %.2f km | Peak: %.0fkm/h",
                 NodeConfig::instance().getTripDistKm(), maxKmh);
        m_u8g2->drawStr(0, 55, buf);

        // Line 5: GPS Coordinates & Accuracy
        if (hasLoc) {
            if (hasFix) {
                snprintf(buf, sizeof(buf), "%.5f, %.5f (\xb1%.1fm)",
                         NodeConfig::instance().getLat(), NodeConfig::instance().getLon(),
                         NodeConfig::instance().getAccM());
            } else {
                snprintf(buf, sizeof(buf), "%.5f, %.5f [Held]",
                         NodeConfig::instance().getLat(), NodeConfig::instance().getLon());
            }
        } else if (NodeConfig::instance().isAttached()) {
            snprintf(buf, sizeof(buf), "Sync: %.14s", NodeConfig::instance().getAttachedDevice());
        } else {
            snprintf(buf, sizeof(buf), "Awaiting GNSS sync...");
        }
        m_u8g2->drawStr(0, 64, buf);

        drawPageDots(1);
    }

    void renderAlprStatus() {
        renderHeader();
        m_u8g2->setFont(u8g2_font_6x10_tr);
        char buf[36];

        snprintf(buf, sizeof(buf), "ALPR LOG: %u Detected", m_alprPassCount);
        m_u8g2->drawStr(0, 26, buf);

        m_u8g2->setFont(u8g2_font_5x7_tr);
        if (m_alprPassCount > 0) {
            snprintf(buf, sizeof(buf), "Last: %.14s %.6s", m_alpr.operatorName, m_alpr.model);
            m_u8g2->drawStr(0, 36, buf);

            const char* fStr = (m_alpr.facing == 1) ? "Front (Faced You)" :
                               (m_alpr.facing == -1) ? "Rear (Faced Away)" : "Unknown Facing";
            snprintf(buf, sizeof(buf), "Read: %s", fStr);
            m_u8g2->drawStr(0, 45, buf);

            snprintf(buf, sizeof(buf), "Dist: %.0fm | Conf: %u%% | %.0fkm/h",
                     m_alpr.distanceM, m_alpr.confidence, m_alpr.speedKmh);
            m_u8g2->drawStr(0, 54, buf);

            snprintf(buf, sizeof(buf), "Threat: HIGH (ALPR Capture)");
            m_u8g2->drawStr(0, 64, buf);
        } else {
            m_u8g2->drawStr(0, 38, "No ALPR passes recorded.");
            m_u8g2->drawStr(0, 48, "Listening on mesh & GNSS...");
            m_u8g2->drawStr(0, 58, "Flock / ALPR alert: Armed");
        }

        drawPageDots(2);
    }

    void renderRfThreatLog() {
        renderHeader();
        m_u8g2->setFont(u8g2_font_6x10_tr);
        char buf[36];

        snprintf(buf, sizeof(buf), "THREAT AUDIT: Monitor");
        m_u8g2->drawStr(0, 26, buf);

        m_u8g2->setFont(u8g2_font_5x7_tr);
        snprintf(buf, sizeof(buf), "BLE Trackers: %u detected", BleScanner::instance().getDetectedCount());
        m_u8g2->drawStr(0, 36, buf);

        snprintf(buf, sizeof(buf), "Deauth Alert: %lu frames", (unsigned long)WifiMonitor::instance().getDeauthCount());
        m_u8g2->drawStr(0, 46, buf);

        snprintf(buf, sizeof(buf), "Wi-Fi Probes: %lu | Bcn: %lu",
                 (unsigned long)WifiMonitor::instance().getProbeCount(),
                 (unsigned long)WifiMonitor::instance().getBeaconCount());
        m_u8g2->drawStr(0, 55, buf);

        snprintf(buf, sizeof(buf), "Ch %u Util  : %u pps (Peak %u)",
                 WifiMonitor::instance().getChannel(),
                 WifiMonitor::instance().getPps(), m_peakPps);
        m_u8g2->drawStr(0, 64, buf);

        drawPageDots(3);
    }

    void renderConnectionAudit(uint32_t loraTx, uint32_t loraRx, int8_t lastRssi, int8_t lastSnr, float loraFreq) {
        renderHeader();
        m_u8g2->setFont(u8g2_font_5x7_tr);
        char buf[36];

        // 1. LoRa SX1262 Link Status, Level & Quality
        if (!LoraRadio::instance().hasAntenna()) {
            snprintf(buf, sizeof(buf), "LoRa : NO ANTENNA (TX OFF)");
            m_u8g2->drawStr(0, 24, buf);
            drawSignalBars(114, 17, 0);
        } else {
            int8_t bRssi = MeshEngine::instance().getBaseStationRssi();
            int8_t actRssi = (bRssi > -125) ? bRssi : lastRssi;
            uint8_t loraQ = NodeConfig::instance().isBaseStation() ? 100 : calculateLinkQuality(actRssi, lastSnr);
            snprintf(buf, sizeof(buf), "LoRa : %ddBm S:%+d Q:%u%%", actRssi, lastSnr, loraQ);
            m_u8g2->drawStr(0, 24, buf);
            drawSignalBars(114, 17, rssiToBars(actRssi));
        }

        // 2. Wi-Fi Sniffer / Interface Quality
        uint16_t pps = WifiMonitor::instance().getPps();
        uint8_t wifiQ = (pps > 10) ? min((int)(pps * 100 / 120), 100) : 40;
        snprintf(buf, sizeof(buf), "Wi-Fi: Ch %u (%up/s) Q:%u%%",
                 WifiMonitor::instance().getChannel(), pps, wifiQ);
        m_u8g2->drawStr(0, 34, buf);

        // 3. BLE NUS Interface
        bool bleConn = BleScanner::instance().isConnected();
        snprintf(buf, sizeof(buf), "BLE  : %s (%s)",
                 bleConn ? "PHONE LINKED" : "ADV NUS 6E40",
                 bleConn ? "Q:100%" : "READY");
        m_u8g2->drawStr(0, 44, buf);

        // 4. Time Synchronization Quality (Sub-ms)
        bool synced = TimeSync::instance().isSynced();
        uint8_t strat = TimeSync::instance().getStratum();
        int64_t offUs = TimeSync::instance().getLastOffsetUs();
        if (strat == 1) {
            snprintf(buf, sizeof(buf), "Sync : S1 Master (±0us)");
        } else if (synced) {
            snprintf(buf, sizeof(buf), "Sync : S%u ±%ldus (Sub-ms)", strat, (long)abs(offUs));
        } else {
            snprintf(buf, sizeof(buf), "Sync : Unsynced (Local)");
        }
        m_u8g2->drawStr(0, 54, buf);

        // 5. Host Bridge / Follow Device
        bool usb = MeshEngine::instance().isUsbActive();
        if (NodeConfig::instance().isAttached()) {
            snprintf(buf, sizeof(buf), "Follow: %.12s %s",
                     NodeConfig::instance().getAttachedDevice(),
                     NodeConfig::instance().hasGpsFix() ? "(GPS)" : "(Sync)");
        } else {
            snprintf(buf, sizeof(buf), "Host : %s", usb ? "USB Serial 115k (Live)" : "Battery Mesh Mode");
        }
        m_u8g2->drawStr(0, 64, buf);

        drawPageDots(6);
    }

    void renderRfSpectrum() {
        renderHeader();

        // Subheader: Current PPS vs Peak PPS
        m_u8g2->setFont(u8g2_font_5x7_tr);
        char buf[36];
        snprintf(buf, sizeof(buf), "HIST 60s: %u pps (Peak: %u)",
                 WifiMonitor::instance().getPps(), m_peakPps);
        m_u8g2->drawStr(0, 24, buf);

        // Expanded 28px tall histogram (y=26..53)
        uint8_t graphX = 4;
        uint8_t graphY = 26;
        uint8_t graphW = 120;
        uint8_t graphH = 27;

        // Draw dotted 50% threshold guideline
        for (uint8_t x = graphX; x < graphX + graphW; x += 4) {
            m_u8g2->drawPixel(x, graphY + (graphH / 2));
        }

        // Draw 60 histogram bars
        for (uint8_t i = 0; i < HIST_BINS; i++) {
            uint8_t idx = (m_histHead + i) % HIST_BINS;
            uint16_t val = m_ppsHistory[idx];
            uint8_t barX = graphX + (i * 2);

            if (val > 0) {
                uint8_t barH = (val * graphH) / m_peakPps;
                if (barH == 0) barH = 1;
                if (barH > graphH) barH = graphH;
                m_u8g2->drawVLine(barX, (graphY + graphH) - barH, barH);
            } else {
                m_u8g2->drawPixel(barX, graphY + graphH);
            }
        }

        // Baseline line
        m_u8g2->drawHLine(graphX, graphY + graphH, graphW);

        // Footer: Total Frames captured
        m_u8g2->setFont(u8g2_font_5x7_tr);
        snprintf(buf, sizeof(buf), "Tot:%lu Bcn:%lu Prb:%lu",
                 WifiMonitor::instance().getTotalFrames(),
                 WifiMonitor::instance().getBeaconCount(),
                 WifiMonitor::instance().getProbeCount());
        m_u8g2->drawStr(0, 63, buf);

        drawPageDots(5);
    }

    void renderLoraMesh(uint32_t loraTx, uint32_t loraRx, int8_t lastRssi, int8_t lastSnr, float loraFreq) {
        renderHeader();

        m_u8g2->setFont(u8g2_font_6x10_tr);
        char buf[36];

        snprintf(buf, sizeof(buf), "LoRa: %.2f MHz SF7 (%+ddB)", loraFreq, LoraRadio::instance().getPowerDbm());
        m_u8g2->drawStr(0, 26, buf);

        if (NodeConfig::instance().isBaseStation()) {
            snprintf(buf, sizeof(buf), "Base GW: Active (S1 Master)");
            m_u8g2->drawStr(0, 36, buf);

            snprintf(buf, sizeof(buf), "TX: %u  RX: %u", loraTx, loraRx);
            m_u8g2->drawStr(0, 46, buf);

            snprintf(buf, sizeof(buf), "Mobiles: %u | Peers: %u",
                     MeshEngine::instance().getActiveMobileCount(),
                     MeshEngine::instance().getNeighborCount());
            m_u8g2->drawStr(0, 56, buf);
        } else {
            snprintf(buf, sizeof(buf), "TX: %u  RX: %u", loraTx, loraRx);
            m_u8g2->drawStr(0, 36, buf);

            int8_t bRssi = MeshEngine::instance().getBaseStationRssi();
            if (bRssi > -120) {
                snprintf(buf, sizeof(buf), "Base RSSI:%d dBm", bRssi);
            } else {
                snprintf(buf, sizeof(buf), "RSSI:%d SNR:%d dB", lastRssi, lastSnr);
            }
            m_u8g2->drawStr(0, 46, buf);

            snprintf(buf, sizeof(buf), "Queue:%u/120 | Hop:%u",
                     MeshEngine::instance().getQueueCount(),
                     MeshEngine::instance().getHopsToGateway());
            m_u8g2->drawStr(0, 56, buf);
        }

        drawPageDots(4);
    }

    void renderBatteryDiagnostics() {
        renderHeader();

        m_u8g2->setFont(u8g2_font_6x10_tr);
        char buf[36];

        uint32_t mv = BatteryMonitor::instance().getMilliVolts();
        uint8_t pct = BatteryMonitor::instance().getPercentage();
        bool hasBatt = BatteryMonitor::instance().hasBattery();
        uint32_t mah = NodeConfig::instance().getBattMah();

        if (hasBatt) {
            const auto& s = BatteryMonitor::instance().getTrainingStats();
            snprintf(buf, sizeof(buf), "Batt: %u mV (%u%%)%s", mv, pct, s.isTrained ? " [Trained]" : "");
            m_u8g2->drawStr(0, 26, buf);

            m_u8g2->setFont(u8g2_font_5x7_tr);
            snprintf(buf, sizeof(buf), "Train: %u%% (%.1f cyc, %lum)", s.trainPct, s.cycles, (unsigned long)(s.runtimeSec / 60));
            m_u8g2->drawStr(0, 36, buf);

            snprintf(buf, sizeof(buf), "Range: %u-%umV (%u)", s.vMin, s.vMax, s.vNom);
            m_u8g2->drawStr(0, 46, buf);

            snprintf(buf, sizeof(buf), "ADC: %umV | %s",
                     BatteryMonitor::instance().getRawMilliVolts(),
                     BatteryMonitor::instance().getStateStr());
            m_u8g2->drawStr(0, 56, buf);
        } else {
            snprintf(buf, sizeof(buf), "Power: 5V USB (No Batt)");
            m_u8g2->drawStr(0, 26, buf);

            m_u8g2->setFont(u8g2_font_5x7_tr);
            snprintf(buf, sizeof(buf), "Vol: %u mAh (Configured)", mah);
            m_u8g2->drawStr(0, 36, buf);

            snprintf(buf, sizeof(buf), "ADC: %u mV | State: %s",
                     BatteryMonitor::instance().getRawMilliVolts(),
                     BatteryMonitor::instance().getStateStr());
            m_u8g2->drawStr(0, 46, buf);

            snprintf(buf, sizeof(buf), "Verdict: %s",
                     BatteryMonitor::instance().getDiagnosticVerdict());
            m_u8g2->drawStr(0, 56, buf);
        }

        snprintf(buf, sizeof(buf), "Free Heap: %u KB", ESP.getFreeHeap() / 1024);
        m_u8g2->drawStr(0, 64, buf);

        drawPageDots(7);
    }

    void renderAntennaDiagnostics() {
        renderHeader();
        m_u8g2->setFont(u8g2_font_6x10_tr);
        char buf[36];

        bool hasAnt = LoraRadio::instance().hasAntenna();
        bool txInhib = LoraRadio::instance().isTxInhibited();
        int8_t ambRssi = LoraRadio::instance().getAmbientRssi();

        if (hasAnt) {
            snprintf(buf, sizeof(buf), "ANTENNA: CONNECTED [OK]");
        } else {
            snprintf(buf, sizeof(buf), "ANTENNA: NOT DETECTED!");
        }
        m_u8g2->drawStr(0, 26, buf);

        m_u8g2->setFont(u8g2_font_5x7_tr);
        if (txInhib) {
            snprintf(buf, sizeof(buf), "PA State : TX INHIBITED (Safe)");
        } else {
            snprintf(buf, sizeof(buf), "PA State : READY (+%ddBm)", LoraRadio::instance().getPowerDbm());
        }
        m_u8g2->drawStr(0, 36, buf);

        snprintf(buf, sizeof(buf), "RF Floor : %d dBm (915MHz)", ambRssi);
        m_u8g2->drawStr(0, 46, buf);

        if (!hasAnt) {
            snprintf(buf, sizeof(buf), "Alert    : High VSWR Open Load");
            m_u8g2->drawStr(0, 55, buf);
            snprintf(buf, sizeof(buf), "Action   : Attach SMA Ant before TX");
            m_u8g2->drawStr(0, 64, buf);
        } else {
            snprintf(buf, sizeof(buf), "Load     : 50 Ohm Matched");
            m_u8g2->drawStr(0, 55, buf);
            snprintf(buf, sizeof(buf), "Mesh     : LoRa + Wi-Fi Active");
            m_u8g2->drawStr(0, 64, buf);
        }

        drawPageDots(8);
    }

    void renderSystemInfo() {
        renderHeader();
        m_u8g2->setFont(u8g2_font_5x7_tr);
        char buf[36];

        snprintf(buf, sizeof(buf), "FW Ver   : v%s (Signed)", BEACONFIX_FW_VERSION);
        m_u8g2->drawStr(0, 24, buf);

        snprintf(buf, sizeof(buf), "Build    : %s (Release)", BEACONFIX_BUILD_DATE);
        m_u8g2->drawStr(0, 32, buf);

        snprintf(buf, sizeof(buf), "Hardware : Heltec LoRa V3");
        m_u8g2->drawStr(0, 40, buf);

        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(buf, sizeof(buf), "MAC      : %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        m_u8g2->drawStr(0, 48, buf);

        snprintf(buf, sizeof(buf), "Radio/PA : SX1262 915M (+%ddBm)", LoraRadio::instance().getPowerDbm());
        m_u8g2->drawStr(0, 56, buf);

        drawPageDots(9);
    }

    void renderMiniHistogram(uint8_t x0, uint8_t y0, uint8_t w, uint8_t h) {
        uint8_t numBars = (w < HIST_BINS) ? (w / 2) : HIST_BINS;
        uint8_t startIdx = (HIST_BINS >= numBars) ? (HIST_BINS - numBars) : 0;

        for (uint8_t i = 0; i < numBars; i++) {
            uint8_t idx = (m_histHead + startIdx + i) % HIST_BINS;
            uint16_t val = m_ppsHistory[idx];
            uint8_t barX = x0 + (i * 2);

            if (val > 0) {
                uint8_t barH = (val * (h - 2)) / m_peakPps;
                if (barH == 0) barH = 1;
                if (barH > (h - 2)) barH = h - 2;
                m_u8g2->drawVLine(barX, (y0 + h - 1) - barH, barH);
            } else {
                m_u8g2->drawPixel(barX, y0 + h - 1);
            }
        }
    }

    void drawPageDots(uint8_t activeIdx) {
        for (uint8_t i = 0; i < NUM_PAGES; i++) {
            if (i == activeIdx) {
                m_u8g2->drawDisc(76 + (i * 5), 60, 2);
            } else {
                m_u8g2->drawCircle(76 + (i * 5), 60, 1);
            }
        }
    }

    U8G2_SSD1306_128X64_NONAME_F_HW_I2C* m_u8g2;
    bool m_ready;
    bool m_screenEnabled;
    bool m_autoCycle;
    uint8_t m_currentPage;
    unsigned long m_lastPageFlipMs;
    unsigned long m_lastUserActivityMs;
    unsigned long m_btnPressStartMs;
    bool m_btnHandledHold;
    unsigned long m_lastRenderMs;
    int m_lastBtnState;

    uint16_t m_ppsHistory[HIST_BINS];
    uint8_t m_histHead;
    uint16_t m_peakPps;

    AlprAlert m_alpr;
    uint32_t m_alprPassCount;
    unsigned long m_lastReleaseMs;
    bool m_singleClickPending;
    unsigned long m_singleClickTimerMs;

    bool m_otaActive;
    uint8_t m_otaPercent;
    uint16_t m_otaChunk;
    uint16_t m_otaTotalChunks;
    char m_otaStatusMsg[32];
    char m_otaVersion[16];

    bool m_seederActive;
    uint8_t m_seederPercent;
    uint16_t m_seederChunk;
    uint16_t m_seederTotalChunks;
    uint32_t m_seederSpeedKbps;
    char m_seederTargetNode[32];
};
