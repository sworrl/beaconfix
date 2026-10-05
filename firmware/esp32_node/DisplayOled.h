#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "NodeConfig.h"
#include "BatteryMonitor.h"
#include "WifiMonitor.h"
#include "TimeSync.h"
#include "MeshEngine.h"
#include "BleScanner.h"

// ── Official BeaconFix 16x16 and 24x24 Icons ──────────────────────────────────
static const uint8_t BEACONFIX_ICON_16[] PROGMEM = {
    0xe0, 0x07, 0x08, 0x10, 0x04, 0x20, 0x02, 0x40,
    0x00, 0x00, 0x01, 0x80, 0x81, 0x81, 0x81, 0x01,
    0x01, 0x00, 0x01, 0x80, 0x01, 0x80, 0x00, 0x00,
    0x02, 0x40, 0x04, 0x20, 0x08, 0x10, 0xe0, 0x07
};

static const uint8_t BEACONFIX_ICON_24[] PROGMEM = {
    0x00, 0xff, 0x00, 0xc0, 0x01, 0x03, 0x20, 0x00, 0x04, 0x10, 0x00, 0x18,
    0x08, 0x00, 0x10, 0x04, 0x00, 0x20, 0x02, 0x00, 0x40, 0x02, 0x00, 0x40,
    0x03, 0x00, 0x80, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80,
    0x81, 0x24, 0x81, 0x11, 0x04, 0x88, 0x01, 0x42, 0x88, 0x01, 0x00, 0x88,
    0x12, 0x00, 0x48, 0x12, 0x00, 0x48, 0x84, 0x00, 0x29, 0x08, 0x00, 0x10,
    0x18, 0x00, 0x18, 0x20, 0x00, 0x04, 0xc0, 0x00, 0x03, 0x00, 0xff, 0x00
};

class DisplayOled {
public:
    static constexpr uint8_t NUM_PAGES = 5;
    static constexpr uint8_t HIST_BINS = 60;

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

    bool begin(int sda = 21, int scl = 22) {
        // Probe I2C on default ESP32 pins (SDA=21, SCL=22)
        Wire.begin(sda, scl);
        Wire.setClock(400000);

        // Check if an SSD1306 OLED responds at address 0x3C
        Wire.beginTransmission(0x3C);
        if (Wire.endTransmission() != 0) {
            // No OLED attached to this ESP32 board
            m_ready = false;
            return false;
        }

        m_u8g2 = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE, scl, sda);
        if (!m_u8g2->begin()) {
            m_ready = false;
            return false;
        }

        m_ready = true;
        m_screenEnabled = true;
        m_autoCycle = true;
        m_currentPage = 0;
        m_lastPageFlipMs = millis();
        m_lastUserActivityMs = millis();
        m_lastRenderMs = 0;
        m_histHead = 0;
        m_peakPps = 10;
        memset(m_ppsHistory, 0, sizeof(m_ppsHistory));

        drawSplash();
        return true;
    }

    bool isReady() const { return m_ready; }

    void drawSplash() {
        if (!m_ready || !m_u8g2) return;
        m_u8g2->clearBuffer();
        m_u8g2->drawXBMP(6, 18, 24, 24, BEACONFIX_ICON_24);
        m_u8g2->setFont(u8g2_font_ncenB10_tr);
        m_u8g2->drawStr(36, 26, "BEACONFIX");
        m_u8g2->setFont(u8g2_font_6x10_tr);
        if (NodeConfig::instance().isBaseStation()) {
            m_u8g2->drawStr(36, 39, "BASE STATION (GW)");
        } else {
            m_u8g2->drawStr(36, 39, "MOBILE FIELD NODE");
        }
        m_u8g2->setFont(u8g2_font_5x7_tr);
        m_u8g2->drawStr(36, 52, "v3.8.0 ESP-NOW Mesh");
        m_u8g2->sendBuffer();
        delay(1200);
    }

    void pushTrafficSample(uint16_t pps) {
        m_ppsHistory[m_histHead] = pps;
        m_histHead = (m_histHead + 1) % HIST_BINS;
        uint16_t maxV = 10;
        for (uint8_t i = 0; i < HIST_BINS; i++) {
            if (m_ppsHistory[i] > maxV) maxV = m_ppsHistory[i];
        }
        m_peakPps = maxV;
    }

    void showOtaProgress(uint8_t pct, uint16_t currentChunk, uint16_t totalChunks, const char* stateStr, const char* version) {
        m_otaActive = true;
        m_otaPercent = pct;
        m_otaChunk = currentChunk;
        m_otaTotalChunks = totalChunks;
        if (stateStr) {
            strncpy(m_otaStatusMsg, stateStr, sizeof(m_otaStatusMsg) - 1);
            m_otaStatusMsg[sizeof(m_otaStatusMsg) - 1] = '\0';
        }
        if (version) {
            strncpy(m_otaVersion, version, sizeof(m_otaVersion) - 1);
            m_otaVersion[sizeof(m_otaVersion) - 1] = '\0';
        }
    }

    void dismissOtaProgress() {
        m_otaActive = false;
    }

    void showSeederProgress(uint8_t percent, uint16_t chunk, uint16_t totalChunks, const char* targetNode, uint32_t speedKbps) {
        // Stubs for headless or external display
    }

    void dismissSeederProgress() {
        // Stubs for headless or external display
    }

    void update() {
        if (!m_ready || !m_u8g2) return;
        unsigned long now = millis();

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

        if (m_autoCycle && (now - m_lastPageFlipMs >= 6000)) {
            m_currentPage = (m_currentPage + 1) % NUM_PAGES;
            m_lastPageFlipMs = now;
        }

        if (now - m_lastRenderMs < 100) return;
        m_lastRenderMs = now;

        m_u8g2->clearBuffer();
        switch (m_currentPage) {
            case 0: renderMainDashboard(); break;
            case 1: renderConnectionAudit(); break;
            case 2: renderRfSpectrum(); break;
            case 3: renderMeshPeers(); break;
            case 4: renderBatteryDiagnostics(); break;
            default:
                m_currentPage = 0;
                renderMainDashboard();
                break;
        }
        m_u8g2->sendBuffer();
    }

private:
    DisplayOled() : m_u8g2(nullptr), m_ready(false), m_screenEnabled(true),
                    m_autoCycle(true), m_currentPage(0), m_lastPageFlipMs(0),
                    m_lastUserActivityMs(0), m_lastRenderMs(0), m_histHead(0), m_peakPps(10),
                    m_otaActive(false), m_otaPercent(0), m_otaChunk(0), m_otaTotalChunks(0) {
        memset(m_ppsHistory, 0, sizeof(m_ppsHistory));
        memset(m_otaStatusMsg, 0, sizeof(m_otaStatusMsg));
        memset(m_otaVersion, 0, sizeof(m_otaVersion));
    }

    void renderHeader() {
        const char* name = NodeConfig::instance().getName();
        uint8_t battPct = BatteryMonitor::instance().getPercentage();
        bool hasBatt = BatteryMonitor::instance().hasBattery();
        bool charging = BatteryMonitor::instance().isCharging();

        m_u8g2->drawXBMP(0, 0, 16, 16, BEACONFIX_ICON_16);

        m_u8g2->setFont(u8g2_font_6x10_tr);
        m_u8g2->setClipWindow(18, 0, 63, 14);
        m_u8g2->drawStr(19, 11, name);
        m_u8g2->setMaxClipWindow();

        int8_t bRssi = MeshEngine::instance().getBaseStationRssi();
        if (NodeConfig::instance().isBaseStation()) {
            m_u8g2->setFont(u8g2_font_4x6_tr);
            m_u8g2->drawStr(66, 9, "GW");
        } else if (bRssi > -125) {
            drawSignalBars(66, 3, rssiToBars(bRssi));
        } else {
            drawSignalBars(66, 3, 0);
        }

        // Synchronized Cardiac Heartbeat Indicator (Epoch Locked)
        uint32_t phaseMs = TimeSync::instance().getPhaseMs(1000);
        bool systole = (phaseMs < 190) || (phaseMs >= 260 && phaseMs < 420);
        static const uint8_t HEART_FILLED[7] PROGMEM = { 0x36, 0x7F, 0x7F, 0x7F, 0x3E, 0x1C, 0x08 };
        static const uint8_t HEART_OUTLINE[7] PROGMEM = { 0x36, 0x49, 0x41, 0x41, 0x22, 0x14, 0x08 };
        m_u8g2->drawBitmap(77, 3, 1, 7, systole ? HEART_FILLED : HEART_OUTLINE);

        m_u8g2->setFont(u8g2_font_5x7_tr);
        if (charging) {
            m_u8g2->drawFrame(88, 2, 26, 9);
            m_u8g2->drawBox(114, 5, 2, 3);
            m_u8g2->drawStr(90, 9, "CHG");
        } else if (hasBatt) {
            char pStr[8];
            snprintf(pStr, sizeof(pStr), "%u%%", battPct);
            m_u8g2->drawStr(86, 9, pStr);

            m_u8g2->drawFrame(108, 2, 16, 9);
            m_u8g2->drawBox(124, 5, 2, 3);
            int barW = (battPct * 12) / 100;
            if (barW > 0) m_u8g2->drawBox(110, 4, barW, 5);
        } else {
            m_u8g2->drawStr(90, 9, "[USB]");
        }

        m_u8g2->drawHLine(0, 15, 128);
    }

    void renderMainDashboard() {
        renderHeader();
        m_u8g2->setFont(u8g2_font_5x7_tr);
        char lineBuf[36];

        if (NodeConfig::instance().isBaseStation()) {
            snprintf(lineBuf, sizeof(lineBuf), "MODE: BASE (GW S1 Root)");
        } else {
            uint8_t hops = MeshEngine::instance().getHopsToGateway();
            if (hops < 254) {
                int8_t bRssi = MeshEngine::instance().getBaseStationRssi();
                snprintf(lineBuf, sizeof(lineBuf), "MODE: MOB (Hop:%u Q:%u%%)",
                         hops, calculateLinkQuality(bRssi, 6));
            } else {
                snprintf(lineBuf, sizeof(lineBuf), "MODE: MOB (Searching Base)");
            }
        }
        m_u8g2->drawStr(0, 24, lineBuf);

        // Row 2: Mesh link status
        if (NodeConfig::instance().isBaseStation()) {
            snprintf(lineBuf, sizeof(lineBuf), "MESH: ESP-NOW GW | Mob:%u",
                     MeshEngine::instance().getActiveMobileCount());
        } else {
            int8_t bRssi = MeshEngine::instance().getBaseStationRssi();
            uint8_t lq = calculateLinkQuality(bRssi, 6);
            snprintf(lineBuf, sizeof(lineBuf), "MESH: Base %ddBm Q:%u%%", bRssi, lq);
        }
        m_u8g2->drawStr(0, 33, lineBuf);

        // Row 3: Wi-Fi Sniffer
        snprintf(lineBuf, sizeof(lineBuf), "WIFI: Ch %u (%u pps) Tot:%lu",
                 WifiMonitor::instance().getChannel(),
                 WifiMonitor::instance().getPps(),
                 WifiMonitor::instance().getTotalFrames());
        m_u8g2->drawStr(0, 42, lineBuf);

        // Row 4: TimeSync & BLE
        bool synced = TimeSync::instance().isSynced();
        uint8_t strat = TimeSync::instance().getStratum();
        int64_t offUs = TimeSync::instance().getLastOffsetUs();
        bool bleConn = BleScanner::instance().isConnected();
        snprintf(lineBuf, sizeof(lineBuf), "SYNC: S%u %ldus | BLE:%s",
                 strat, (long)abs(offUs), bleConn ? "LINK" : "ADV");
        m_u8g2->drawStr(0, 51, lineBuf);

        renderMiniHistogram(2, 53, 94, 10);
        drawPageDots(0);
    }

    void renderConnectionAudit() {
        renderHeader();
        m_u8g2->setFont(u8g2_font_5x7_tr);
        char buf[36];

        // 1. ESP-NOW Mesh Link
        int8_t bRssi = MeshEngine::instance().getBaseStationRssi();
        uint8_t meshQ = NodeConfig::instance().isBaseStation() ? 100 : calculateLinkQuality(bRssi, 6);
        snprintf(buf, sizeof(buf), "Mesh : ESP-NOW %ddBm Q:%u%%", bRssi, meshQ);
        m_u8g2->drawStr(0, 24, buf);
        drawSignalBars(114, 17, rssiToBars(bRssi));

        // 2. Wi-Fi Sniffer
        uint16_t pps = WifiMonitor::instance().getPps();
        uint8_t wifiQ = (pps > 10) ? min((int)(pps * 100 / 120), 100) : 40;
        snprintf(buf, sizeof(buf), "Wi-Fi: Ch %u (%up/s) Q:%u%%",
                 WifiMonitor::instance().getChannel(), pps, wifiQ);
        m_u8g2->drawStr(0, 34, buf);

        // 3. BLE Interface
        bool bleConn = BleScanner::instance().isConnected();
        snprintf(buf, sizeof(buf), "BLE  : %s (%s)",
                 bleConn ? "PHONE LINKED" : "ADV NUS 6E40",
                 bleConn ? "Q:100%" : "READY");
        m_u8g2->drawStr(0, 44, buf);

        // 4. Time Sync
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

        // 5. Host Bridge
        bool usb = MeshEngine::instance().isUsbActive();
        snprintf(buf, sizeof(buf), "Host : %s", usb ? "USB Serial 115k (Live)" : "Battery Mesh Mode");
        m_u8g2->drawStr(0, 64, buf);

        drawPageDots(1);
    }

    void renderRfSpectrum() {
        renderHeader();
        m_u8g2->setFont(u8g2_font_5x7_tr);
        char buf[36];
        snprintf(buf, sizeof(buf), "HIST 60s: %u pps (Peak: %u)",
                 WifiMonitor::instance().getPps(), m_peakPps);
        m_u8g2->drawStr(0, 24, buf);

        uint8_t graphX = 4, graphY = 26, graphW = 120, graphH = 27;
        for (uint8_t x = graphX; x < graphX + graphW; x += 4) {
            m_u8g2->drawPixel(x, graphY + (graphH / 2));
        }

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
        m_u8g2->drawHLine(graphX, graphY + graphH, graphW);

        snprintf(buf, sizeof(buf), "Tot:%lu Bcn:%lu Prb:%lu",
                 WifiMonitor::instance().getTotalFrames(),
                 WifiMonitor::instance().getBeaconCount(),
                 WifiMonitor::instance().getProbeCount());
        m_u8g2->drawStr(0, 63, buf);
        drawPageDots(2);
    }

    void renderMeshPeers() {
        renderHeader();
        m_u8g2->setFont(u8g2_font_5x7_tr);
        char buf[36];

        snprintf(buf, sizeof(buf), "ESP-NOW MESH PEERS (%u)",
                 MeshEngine::instance().getNeighborCount());
        m_u8g2->drawStr(0, 24, buf);

        snprintf(buf, sizeof(buf), "Queue:%u/120 | Hop:%u",
                 MeshEngine::instance().getQueueCount(),
                 MeshEngine::instance().getHopsToGateway());
        m_u8g2->drawStr(0, 36, buf);

        snprintf(buf, sizeof(buf), "Mobiles:%u | Base RSSI:%d",
                 MeshEngine::instance().getActiveMobileCount(),
                 MeshEngine::instance().getBaseStationRssi());
        m_u8g2->drawStr(0, 48, buf);

        snprintf(buf, sizeof(buf), "Free Heap: %u KB", ESP.getFreeHeap() / 1024);
        m_u8g2->drawStr(0, 60, buf);
        drawPageDots(3);
    }

    void renderBatteryDiagnostics() {
        renderHeader();
        m_u8g2->setFont(u8g2_font_6x10_tr);
        char buf[36];

        uint32_t mv = BatteryMonitor::instance().getMilliVolts();
        uint8_t pct = BatteryMonitor::instance().getPercentage();
        bool hasBatt = BatteryMonitor::instance().hasBattery();

        if (hasBatt) {
            snprintf(buf, sizeof(buf), "Batt: %u mV (%u%%)", mv, pct);
        } else {
            snprintf(buf, sizeof(buf), "Power: 5V USB (No Batt)");
        }
        m_u8g2->drawStr(0, 26, buf);

        m_u8g2->setFont(u8g2_font_5x7_tr);
        snprintf(buf, sizeof(buf), "ADC: %u mV (2.0x divider)",
                 BatteryMonitor::instance().getRawMilliVolts());
        m_u8g2->drawStr(0, 36, buf);

        snprintf(buf, sizeof(buf), "State: %s",
                 BatteryMonitor::instance().getStateStr());
        m_u8g2->drawStr(0, 46, buf);

        snprintf(buf, sizeof(buf), "Verdict: %s",
                 BatteryMonitor::instance().getDiagnosticVerdict());
        m_u8g2->drawStr(0, 56, buf);

        snprintf(buf, sizeof(buf), "Free Heap: %u KB", ESP.getFreeHeap() / 1024);
        m_u8g2->drawStr(0, 64, buf);
        drawPageDots(4);
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
                m_u8g2->drawDisc(102 + (i * 5), 60, 2);
            } else {
                m_u8g2->drawCircle(102 + (i * 5), 60, 1);
            }
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

    U8G2_SSD1306_128X64_NONAME_F_HW_I2C* m_u8g2;
    bool m_ready;
    bool m_screenEnabled;
    bool m_autoCycle;
    uint8_t m_currentPage;
    unsigned long m_lastPageFlipMs;
    unsigned long m_lastUserActivityMs;
    unsigned long m_lastRenderMs;
    uint16_t m_ppsHistory[HIST_BINS];
    uint8_t m_histHead;
    uint16_t m_peakPps;

    bool m_otaActive;
    uint8_t m_otaPercent;
    uint16_t m_otaChunk;
    uint16_t m_otaTotalChunks;
    char m_otaStatusMsg[32];
    char m_otaVersion[16];
};
