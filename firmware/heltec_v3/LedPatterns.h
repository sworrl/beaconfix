#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "TimeSync.h"

// 86-step organic human cardiac profile (0ms..425ms at 5ms intervals, 1000ms cycle = 60 BPM).
// Systole 1 "Lub" (0-190ms, peak 255 at 70ms) -> pause (190-260ms) -> Systole 2 "Dub" (260-420ms, peak 175 at 325ms) -> Diastole rest (420-1000ms).
static const uint8_t CARDIAC_LUT[86] PROGMEM = {
      0,   9,  26,  48,  72,  98, 125, 151, 176, 198,
    218, 233, 245, 252, 255, 254, 251, 246, 239, 231,
    220, 207, 192, 176, 158, 139, 119,  99,  80,  61,
     44,  29,  17,   8,   3,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   2,   7,  16,  28,  44,  62,  82, 102,
    122, 140, 155, 166, 172, 175, 174, 171, 166, 158,
    148, 136, 122, 107,  91,  75,  60,  45,  32,  20,
     11,   5,   1,   0,   0,   0
};

static inline uint8_t getCardiacBrightness(uint32_t phaseMs) {
    if (phaseMs >= 425) return 0;
    uint32_t idx = phaseMs / 5;
    uint32_t rem = phaseMs % 5;
    uint8_t v1 = pgm_read_byte(&CARDIAC_LUT[idx]);
    if (rem == 0 || idx >= 85) return v1;
    uint8_t v2 = pgm_read_byte(&CARDIAC_LUT[idx + 1]);
    return (uint8_t)(v1 + (((int32_t)(v2 - v1) * (int32_t)rem) / 5));
}

/**
 * Advanced Multi-LED Status & Pattern Engine for ESP32 Nodes.
 * 
 * Supports:
 *  - Internal Board LED (Slot 0, default GPIO 2).
 *  - User-defined External Single-Color LEDs (any safe GPIO).
 *  - User-defined External 4-Pin RGB LEDs (Common Cathode or Common Anode).
 *  - Granular Per-LED Role Assignments:
 *      * ROLE_MIRROR: Augments/extends internal LED (e.g. for waterproof enclosure).
 *      * ROLE_BATTERY_ONLY: Dedicated full-time battery gauge without packet interruption.
 *      * ROLE_SNIFFER_ONLY: Dedicated Wi-Fi probe/beacon traffic monitor.
 *      * ROLE_TRACKER_ONLY: Dedicated BLE tracker (AirTag/SmartTag/Tile) alarm.
 *      * ROLE_ALERT_ONLY: Dedicated deauth / disassoc threat alert strobe.
 *      * ROLE_OFF: Disabled.
 *  - Non-blocking, PWM/Digital capable, NVS persistent configuration.
 */

enum LedPatternId {
    PATTERN_OFF = 0,
    PATTERN_HEARTBEAT,       // Normal operational idle: 1 brief pulse every 2s
    PATTERN_LINK_ACTIVE,     // Client linked (USB/BLE/WiFi): double pulse every 1.5s
    PATTERN_PROBE_FLASH,     // Frame/probe detected: 15ms sharp strobe
    PATTERN_TRACKER_FLASH,   // BLE Tracker / AirTag detected: 2 quick blips
    PATTERN_DEAUTH_ALERT,    // Deauth/threat alert: 5 frantic flashes in 250ms
    PATTERN_BROKEN_RADIO,    // Radio init or hardware failure: continuous 100ms fast blink
    PATTERN_BROKEN_HEAP,     // Out of memory warning: 3 long pulses
    PATTERN_BROKEN_SOS,      // System panic: Morse SOS (... --- ...)
    PATTERN_BATT_CHARGING,   // Charging: steady breathing pulse
    PATTERN_BATT_FULL,       // Battery Full (>=80%): 4 pulses
    PATTERN_BATT_HALFWAY,    // Battery Halfway (35-79%): 2 pulses
    PATTERN_BATT_DYING,      // Battery Dying (<20%): urgent 3-flicker warning
    PATTERN_BATT_NO_BATTERY, // USB-only / No battery connected: quiet double-tick every 3.5s
    PATTERN_OTA_PROGRESS,    // Active Mesh OTA receiving: energetic double-pulse cadence
    PATTERN_OTA_SUCCESS,     // Mesh OTA success: solid ON before reboot
    PATTERN_OTA_FAILED       // Mesh OTA failed: frantic error strobe
};

enum LedType {
    LED_TYPE_NONE = 0,
    LED_TYPE_SINGLE,
    LED_TYPE_RGB
};

enum LedRole {
    ROLE_MIRROR = 0,       // Mirror internal LED (all diagnostic gestures)
    ROLE_BATTERY_ONLY,     // Dedicated full-time battery meter (no packet timesharing)
    ROLE_SNIFFER_ONLY,     // Dedicated probe/beacon traffic strobe
    ROLE_TRACKER_ONLY,     // Dedicated BLE tracker alert
    ROLE_ALERT_ONLY,       // Dedicated deauth attack strobe
    ROLE_OFF               // Off
};

struct RgbColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;

    static RgbColor Off()       { return {0, 0, 0}; }
    static RgbColor Red()       { return {255, 0, 0}; }
    static RgbColor Green()     { return {0, 255, 0}; }
    static RgbColor Blue()      { return {0, 0, 255}; }
    static RgbColor Yellow()    { return {255, 180, 0}; }
    static RgbColor Cyan()      { return {0, 220, 255}; }
    static RgbColor Magenta()   { return {255, 0, 220}; }
    static RgbColor White()     { return {255, 255, 255}; }
    static RgbColor Amber()     { return {255, 120, 0}; }
    static RgbColor Purple()    { return {180, 0, 255}; }
};

struct LedSlot {
    bool enabled;
    LedType type;
    LedRole role;
    uint8_t pinR;         // Also used as single pin for LED_TYPE_SINGLE
    uint8_t pinG;
    uint8_t pinB;
    bool inverted;        // Active-low for single, or Common-Anode for RGB
    char label[16];

    // Runtime state
    uint16_t stepDurations[20];
    bool stepStates[20];
    uint8_t stepCount;
    uint8_t stepIndex;
    unsigned long lastStepTime;
    unsigned long temporaryUntil;
    LedPatternId currentPattern;
    LedPatternId basePattern;
    RgbColor currentColor;
    RgbColor targetColor;
};

#define MAX_LED_SLOTS 4

class LedPatterns {
public:
    static LedPatterns& instance() {
        static LedPatterns inst;
        return inst;
    }

    void begin(int onboardPin = 2, bool activeLow = false) {
        m_prefs.begin("bcf_leds", false);

        // Slot 0 is always onboard LED
        m_slots[0].enabled = true;
        m_slots[0].type = LED_TYPE_SINGLE;
        m_slots[0].role = ROLE_MIRROR;
        m_slots[0].pinR = onboardPin;
        m_slots[0].inverted = activeLow;
        m_slots[0].basePattern = PATTERN_HEARTBEAT;
        strncpy(m_slots[0].label, "Onboard", sizeof(m_slots[0].label));
        setupHardware(0);
        playSlot(0, PATTERN_HEARTBEAT);

        // Load configured external LED slots from NVS
        loadFromPrefs();
    }

    void setPin(int pin, bool activeLow = false) {
        m_slots[0].pinR = pin;
        m_slots[0].inverted = activeLow;
        setupHardware(0);
        playSlot(0, m_slots[0].basePattern);
    }

    int getPin() const { return m_slots[0].pinR; }

    bool configureSingle(uint8_t slot, uint8_t pin, LedRole role, bool activeLow = false, const char* label = "External") {
        if (slot == 0 || slot >= MAX_LED_SLOTS) return false;
        if (!isSafeGpio(pin)) return false;

        m_slots[slot].enabled = true;
        m_slots[slot].type = LED_TYPE_SINGLE;
        m_slots[slot].pinR = pin;
        m_slots[slot].pinG = 0;
        m_slots[slot].pinB = 0;
        m_slots[slot].role = role;
        m_slots[slot].inverted = activeLow;
        strncpy(m_slots[slot].label, label, sizeof(m_slots[slot].label) - 1);

        setupHardware(slot);
        initSlotRolePattern(slot);
        saveSlotToPrefs(slot);
        return true;
    }

    bool configureRgb(uint8_t slot, uint8_t pinR, uint8_t pinG, uint8_t pinB,
                      LedRole role, bool commonAnode = false, const char* label = "ExternalRGB") {
        if (slot == 0 || slot >= MAX_LED_SLOTS) return false;
        if (!isSafeGpio(pinR) || !isSafeGpio(pinG) || !isSafeGpio(pinB)) return false;

        m_slots[slot].enabled = true;
        m_slots[slot].type = LED_TYPE_RGB;
        m_slots[slot].pinR = pinR;
        m_slots[slot].pinG = pinG;
        m_slots[slot].pinB = pinB;
        m_slots[slot].role = role;
        m_slots[slot].inverted = commonAnode;
        strncpy(m_slots[slot].label, label, sizeof(m_slots[slot].label) - 1);

        setupHardware(slot);
        initSlotRolePattern(slot);
        saveSlotToPrefs(slot);
        return true;
    }

    void clearSlot(uint8_t slot) {
        if (slot == 0 || slot >= MAX_LED_SLOTS) return;
        writeSlotPhysical(slot, false, RgbColor::Off());
        m_slots[slot].enabled = false;
        m_slots[slot].type = LED_TYPE_NONE;
        m_slots[slot].role = ROLE_OFF;

        char key[16];
        snprintf(key, sizeof(key), "slot_%u_en", slot);
        m_prefs.remove(key);
    }

    const LedSlot& getSlot(uint8_t slot) const {
        return m_slots[slot < MAX_LED_SLOTS ? slot : 0];
    }

    // ── High-Level Trigger APIs ───────────────────────────────────────────────
    void triggerDetection(bool isTracker = false, const char* kind = nullptr) {
        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            if (!m_slots[i].enabled) continue;

            if (m_slots[i].role == ROLE_MIRROR) {
                if (isTracker) {
                    triggerTemporarySlot(i, PATTERN_TRACKER_FLASH, 250, RgbColor::Yellow());
                } else {
                    triggerTemporarySlot(i, PATTERN_PROBE_FLASH, 30, RgbColor::Magenta());
                }
            } else if (m_slots[i].role == ROLE_SNIFFER_ONLY && !isTracker) {
                triggerTemporarySlot(i, PATTERN_PROBE_FLASH, 35, RgbColor::Magenta());
            } else if (m_slots[i].role == ROLE_TRACKER_ONLY && isTracker) {
                RgbColor col = RgbColor::Yellow();
                if (kind && strcmp(kind, "tile") == 0) col = RgbColor::Blue();
                else if (kind && strcmp(kind, "smarttag") == 0) col = RgbColor::Purple();
                triggerTemporarySlot(i, PATTERN_TRACKER_FLASH, 300, col);
            }
        }
    }

    void triggerDeauthAlert() {
        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            if (!m_slots[i].enabled) continue;
            if (m_slots[i].role == ROLE_MIRROR || m_slots[i].role == ROLE_ALERT_ONLY) {
                triggerTemporarySlot(i, PATTERN_DEAUTH_ALERT, 600, RgbColor::Red());
            }
        }
    }

    void setBasePattern(LedPatternId pattern) {
        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            if (!m_slots[i].enabled) continue;
            if (m_slots[i].role == ROLE_MIRROR) {
                m_slots[i].basePattern = pattern;
                if (m_slots[i].temporaryUntil == 0) {
                    playSlot(i, pattern);
                }
            }
        }
    }

    void triggerTemporarySlot(uint8_t slot, LedPatternId pattern, unsigned long durationMs, RgbColor color) {
        if (m_broken && m_slots[slot].role == ROLE_MIRROR) return;
        m_slots[slot].temporaryUntil = millis() + durationMs;
        playSlot(slot, pattern, color);
    }

    void playSlot(uint8_t slot, LedPatternId pattern, RgbColor color = RgbColor::Cyan()) {
        m_slots[slot].currentPattern = pattern;
        if (!m_broken && m_slots[slot].temporaryUntil == 0) {
            m_slots[slot].basePattern = pattern;
        }
        m_slots[slot].stepIndex = 0;
        m_slots[slot].lastStepTime = millis();
        m_slots[slot].currentColor = color;
        m_slots[slot].targetColor = color;
        loadPatternTimings(slot, pattern);
        applySlotStepState(slot);
    }

    void triggerOtaProgress() {
        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            if (!m_slots[i].enabled) continue;
            if (m_slots[i].role == ROLE_MIRROR) {
                m_slots[i].temporaryUntil = 0;
                playSlot(i, PATTERN_OTA_PROGRESS, RgbColor::Cyan());
            }
        }
    }

    void triggerOtaSuccess() {
        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            if (!m_slots[i].enabled) continue;
            if (m_slots[i].role == ROLE_MIRROR) {
                m_slots[i].temporaryUntil = 0;
                playSlot(i, PATTERN_OTA_SUCCESS, RgbColor::Green());
            }
        }
    }

    void triggerOtaFailed() {
        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            if (!m_slots[i].enabled) continue;
            if (m_slots[i].role == ROLE_MIRROR) {
                m_slots[i].temporaryUntil = 0;
                playSlot(i, PATTERN_OTA_FAILED, RgbColor::Red());
            }
        }
    }

    void restoreBasePattern() {
        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            if (!m_slots[i].enabled) continue;
            if (m_slots[i].role == ROLE_MIRROR) {
                m_slots[i].temporaryUntil = 0;
                playSlot(i, m_slots[i].basePattern);
            }
        }
    }

    void notifyBatteryState(int state, uint8_t pct) {
        m_battState = state;
        m_battPct = pct;

        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            if (!m_slots[i].enabled) continue;
            if (m_slots[i].role == ROLE_BATTERY_ONLY) {
                applyBatteryRolePattern(i);
            }
        }
    }

    void reportBroken(LedPatternId brokenPattern) {
        m_broken = true;
        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            if (!m_slots[i].enabled) continue;
            if (m_slots[i].role == ROLE_MIRROR) {
                m_slots[i].temporaryUntil = 0;
                playSlot(i, brokenPattern, RgbColor::Red());
            }
        }
    }

    void clearBroken() {
        m_broken = false;
        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            if (!m_slots[i].enabled) continue;
            if (m_slots[i].role == ROLE_MIRROR) {
                playSlot(i, m_slots[i].basePattern);
            }
        }
    }

    bool isBroken() const { return m_broken; }

    void update() {
        unsigned long now = millis();

        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            if (!m_slots[i].enabled) continue;

            // Revert expired temporary pattern
            if (!m_broken && m_slots[i].temporaryUntil > 0 && now >= m_slots[i].temporaryUntil) {
                m_slots[i].temporaryUntil = 0;
                playSlot(i, m_slots[i].basePattern, m_slots[i].targetColor);
            }

            renderSlot(i, now);
        }
    }

    // Safety check helper
    static bool isSafeGpio(uint8_t pin) {
        // Safe general-purpose output GPIOs on ESP32:
        // 2, 4, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33
        switch (pin) {
            case 2: case 4: case 16: case 17: case 18: case 19:
            case 21: case 22: case 23: case 25: case 26: case 27:
            case 32: case 33:
                return true;
            default:
                return false;
        }
    }

private:
    LedPatterns() : m_broken(false), m_battState(1), m_battPct(0) {
        for (int i = 0; i < MAX_LED_SLOTS; i++) {
            m_slots[i].enabled = false;
            m_slots[i].type = LED_TYPE_NONE;
            m_slots[i].role = ROLE_OFF;
            m_slots[i].pinR = 0;
            m_slots[i].pinG = 0;
            m_slots[i].pinB = 0;
            m_slots[i].inverted = false;
            m_slots[i].label[0] = '\0';
            m_slots[i].stepCount = 0;
            m_slots[i].stepIndex = 0;
            m_slots[i].lastStepTime = 0;
            m_slots[i].temporaryUntil = 0;
            m_slots[i].currentPattern = PATTERN_OFF;
            m_slots[i].basePattern = PATTERN_OFF;
            m_slots[i].currentColor = RgbColor::Off();
            m_slots[i].targetColor = RgbColor::Cyan();
        }
    }

    void setupHardware(uint8_t slot) {
        if (m_slots[slot].type == LED_TYPE_SINGLE) {
            pinMode(m_slots[slot].pinR, OUTPUT);
            writeSlotPhysical(slot, false, RgbColor::Off());
        } else if (m_slots[slot].type == LED_TYPE_RGB) {
            pinMode(m_slots[slot].pinR, OUTPUT);
            pinMode(m_slots[slot].pinG, OUTPUT);
            pinMode(m_slots[slot].pinB, OUTPUT);
            writeSlotPhysical(slot, false, RgbColor::Off());
        }
    }

    void initSlotRolePattern(uint8_t slot) {
        switch (m_slots[slot].role) {
            case ROLE_MIRROR:
                m_slots[slot].basePattern = PATTERN_HEARTBEAT;
                playSlot(slot, PATTERN_HEARTBEAT, RgbColor::Cyan());
                break;
            case ROLE_BATTERY_ONLY:
                applyBatteryRolePattern(slot);
                break;
            case ROLE_SNIFFER_ONLY:
                m_slots[slot].basePattern = PATTERN_OFF;
                playSlot(slot, PATTERN_OFF, RgbColor::Magenta());
                break;
            case ROLE_TRACKER_ONLY:
                m_slots[slot].basePattern = PATTERN_OFF;
                playSlot(slot, PATTERN_OFF, RgbColor::Yellow());
                break;
            case ROLE_ALERT_ONLY:
                m_slots[slot].basePattern = PATTERN_OFF;
                playSlot(slot, PATTERN_OFF, RgbColor::Red());
                break;
            case ROLE_OFF:
            default:
                m_slots[slot].basePattern = PATTERN_OFF;
                playSlot(slot, PATTERN_OFF, RgbColor::Off());
                break;
        }
    }

    void applyBatteryRolePattern(uint8_t slot) {
        // Full-time battery indicator
        // battState: 1 = NO_BATTERY, 2 = CHARGING, 3 = FULL, 4 = HALFWAY, 5 = DYING
        switch (m_battState) {
            case 1: // NO_BATTERY (USB-only)
                playSlot(slot, PATTERN_BATT_NO_BATTERY, RgbColor::Cyan());
                break;
            case 2: // CHARGING
                playSlot(slot, PATTERN_BATT_CHARGING, RgbColor::Cyan());
                break;
            case 3: // FULL
                playSlot(slot, PATTERN_BATT_FULL, RgbColor::Green());
                break;
            case 4: // HALFWAY
                playSlot(slot, PATTERN_BATT_HALFWAY, RgbColor::Yellow());
                break;
            case 5: // DYING
                playSlot(slot, PATTERN_BATT_DYING, RgbColor::Red());
                break;
            default:
                playSlot(slot, PATTERN_BATT_NO_BATTERY, RgbColor::Cyan());
                break;
        }
    }

    void loadPatternTimings(uint8_t slot, LedPatternId pattern) {
        LedSlot& s = m_slots[slot];
        switch (pattern) {
            case PATTERN_HEARTBEAT:
                // 40ms ON, 1960ms OFF
                s.stepDurations[0] = 40;   s.stepStates[0] = true;
                s.stepDurations[1] = 1960; s.stepStates[1] = false;
                s.stepCount = 2;
                break;

            case PATTERN_LINK_ACTIVE:
                // Double heartbeat
                s.stepDurations[0] = 40;   s.stepStates[0] = true;
                s.stepDurations[1] = 80;   s.stepStates[1] = false;
                s.stepDurations[2] = 40;   s.stepStates[2] = true;
                s.stepDurations[3] = 1340; s.stepStates[3] = false;
                s.stepCount = 4;
                break;

            case PATTERN_PROBE_FLASH:
                s.stepDurations[0] = 15; s.stepStates[0] = true;
                s.stepDurations[1] = 40; s.stepStates[1] = false;
                s.stepCount = 2;
                break;

            case PATTERN_TRACKER_FLASH:
                s.stepDurations[0] = 30; s.stepStates[0] = true;
                s.stepDurations[1] = 40; s.stepStates[1] = false;
                s.stepDurations[2] = 30; s.stepStates[2] = true;
                s.stepDurations[3] = 60; s.stepStates[3] = false;
                s.stepCount = 4;
                break;

            case PATTERN_DEAUTH_ALERT:
                for (int i = 0; i < 5; i++) {
                    s.stepDurations[i * 2] = 30;     s.stepStates[i * 2] = true;
                    s.stepDurations[i * 2 + 1] = 30; s.stepStates[i * 2 + 1] = false;
                }
                s.stepCount = 10;
                break;

            case PATTERN_BROKEN_RADIO:
                s.stepDurations[0] = 100; s.stepStates[0] = true;
                s.stepDurations[1] = 100; s.stepStates[1] = false;
                s.stepCount = 2;
                break;

            case PATTERN_BROKEN_HEAP:
                s.stepDurations[0] = 400; s.stepStates[0] = true;
                s.stepDurations[1] = 150; s.stepStates[1] = false;
                s.stepDurations[2] = 400; s.stepStates[2] = true;
                s.stepDurations[3] = 150; s.stepStates[3] = false;
                s.stepDurations[4] = 400; s.stepStates[4] = true;
                s.stepDurations[5] = 800; s.stepStates[5] = false;
                s.stepCount = 6;
                break;

            case PATTERN_BROKEN_SOS:
                s.stepDurations[0] = 80;  s.stepStates[0] = true;  s.stepDurations[1] = 80;  s.stepStates[1] = false;
                s.stepDurations[2] = 80;  s.stepStates[2] = true;  s.stepDurations[3] = 80;  s.stepStates[3] = false;
                s.stepDurations[4] = 80;  s.stepStates[4] = true;  s.stepDurations[5] = 200; s.stepStates[5] = false;
                s.stepDurations[6] = 240; s.stepStates[6] = true;  s.stepDurations[7] = 80;  s.stepStates[7] = false;
                s.stepDurations[8] = 240; s.stepStates[8] = true;  s.stepDurations[9] = 80;  s.stepStates[9] = false;
                s.stepDurations[10] = 240;s.stepStates[10] = true; s.stepDurations[11] = 200;s.stepStates[11] = false;
                s.stepDurations[12] = 80; s.stepStates[12] = true; s.stepDurations[13] = 80; s.stepStates[13] = false;
                s.stepDurations[14] = 80; s.stepStates[14] = true; s.stepDurations[15] = 80; s.stepStates[15] = false;
                s.stepDurations[16] = 80; s.stepStates[16] = true; s.stepDurations[17] = 800;s.stepStates[17] = false;
                s.stepCount = 18;
                break;

            case PATTERN_BATT_CHARGING:
                // Gentle breathing pulse: 600ms ON, 600ms OFF
                s.stepDurations[0] = 600; s.stepStates[0] = true;
                s.stepDurations[1] = 600; s.stepStates[1] = false;
                s.stepCount = 2;
                break;

            case PATTERN_BATT_FULL:
                // 4 crisp pulses then long pause
                for (int i = 0; i < 4; i++) {
                    s.stepDurations[i * 2] = 80;      s.stepStates[i * 2] = true;
                    s.stepDurations[i * 2 + 1] = 120; s.stepStates[i * 2 + 1] = false;
                }
                s.stepDurations[7] = 2500;
                s.stepCount = 8;
                break;

            case PATTERN_BATT_HALFWAY:
                // 2 medium pulses
                s.stepDurations[0] = 120;  s.stepStates[0] = true;
                s.stepDurations[1] = 150;  s.stepStates[1] = false;
                s.stepDurations[2] = 120;  s.stepStates[2] = true;
                s.stepDurations[3] = 2500; s.stepStates[3] = false;
                s.stepCount = 4;
                break;

            case PATTERN_BATT_DYING:
                // Urgent 3 quick flashes
                for (int i = 0; i < 3; i++) {
                    s.stepDurations[i * 2] = 50;     s.stepStates[i * 2] = true;
                    s.stepDurations[i * 2 + 1] = 50; s.stepStates[i * 2 + 1] = false;
                }
                s.stepDurations[5] = 800;
                s.stepCount = 6;
                break;

            case PATTERN_BATT_NO_BATTERY:
                // Quiet, distinctive dual-tick for USB-powered nodes with no battery connected
                s.stepDurations[0] = 25;   s.stepStates[0] = true;
                s.stepDurations[1] = 120;  s.stepStates[1] = false;
                s.stepDurations[2] = 25;   s.stepStates[2] = true;
                s.stepDurations[3] = 3330; s.stepStates[3] = false;
                s.stepCount = 4;
                break;

            case PATTERN_OTA_PROGRESS:
                // Energetic 400ms double-pulse cadence indicating active firmware transfer
                s.stepDurations[0] = 50;  s.stepStates[0] = true;
                s.stepDurations[1] = 50;  s.stepStates[1] = false;
                s.stepDurations[2] = 50;  s.stepStates[2] = true;
                s.stepDurations[3] = 250; s.stepStates[3] = false;
                s.stepCount = 4;
                break;

            case PATTERN_OTA_SUCCESS:
                // Solid bright ON for 3000ms right before reboot
                s.stepDurations[0] = 3000; s.stepStates[0] = true;
                s.stepCount = 1;
                break;

            case PATTERN_OTA_FAILED:
                // Frantic 5-strobe warning
                for (int i = 0; i < 5; i++) {
                    s.stepDurations[i * 2] = 40;     s.stepStates[i * 2] = true;
                    s.stepDurations[i * 2 + 1] = 40; s.stepStates[i * 2 + 1] = false;
                }
                s.stepDurations[9] = 300;
                s.stepCount = 10;
                break;

            case PATTERN_OFF:
            default:
                s.stepDurations[0] = 1000; s.stepStates[0] = false;
                s.stepCount = 1;
                break;
        }
    }

    void applySlotStepState(uint8_t slot) {
        LedSlot& s = m_slots[slot];
        if (s.stepCount > 0 && s.stepIndex < s.stepCount) {
            writeSlotPhysical(slot, s.stepStates[s.stepIndex], s.currentColor);
        }
    }

    void renderSlot(uint8_t slot, unsigned long now) {
        LedSlot& s = m_slots[slot];

        // 1. Organic Synchronized Human Cardiac Heartbeat (Epoch Locked)
        // Automatically syncs to (epochUs / 1000) % 1000 so all nodes pulse in unison
        if (s.currentPattern == PATTERN_HEARTBEAT || s.currentPattern == PATTERN_LINK_ACTIVE) {
            uint32_t phaseMs = TimeSync::instance().getPhaseMs(1000);
            uint8_t brightness = getCardiacBrightness(phaseMs);
            RgbColor col = TimeSync::instance().isSynced() ? RgbColor::Red() : RgbColor::Amber();
            writeSlotAnalog(slot, brightness, col);
            return;
        }

        // 2. Discrete Step Patterns (Temporary alarms, threats, battery gauges)
        if (s.stepCount == 0) return;
        if (now - s.lastStepTime >= s.stepDurations[s.stepIndex]) {
            s.lastStepTime = now;
            s.stepIndex = (s.stepIndex + 1) % s.stepCount;
            applySlotStepState(slot);
        }
    }

    void writeSlotAnalog(uint8_t slot, uint8_t brightness, RgbColor color) {
        LedSlot& s = m_slots[slot];
        if (!s.enabled) return;

        if (s.type == LED_TYPE_SINGLE) {
            uint8_t duty = s.inverted ? (255 - brightness) : brightness;
            analogWrite(s.pinR, duty);
        } else if (s.type == LED_TYPE_RGB) {
            uint8_t r = (uint8_t)(((uint16_t)color.r * brightness) / 255);
            uint8_t g = (uint8_t)(((uint16_t)color.g * brightness) / 255);
            uint8_t b = (uint8_t)(((uint16_t)color.b * brightness) / 255);
            analogWrite(s.pinR, s.inverted ? (255 - r) : r);
            analogWrite(s.pinG, s.inverted ? (255 - g) : g);
            analogWrite(s.pinB, s.inverted ? (255 - b) : b);
        }
    }

    void writeSlotPhysical(uint8_t slot, bool on, RgbColor color) {
        writeSlotAnalog(slot, on ? 255 : 0, color);
    }

    void saveSlotToPrefs(uint8_t slot) {
        char keyPrefix[16];
        snprintf(keyPrefix, sizeof(keyPrefix), "slot_%u_", slot);

        String kEn = String(keyPrefix) + "en";
        String kType = String(keyPrefix) + "type";
        String kRole = String(keyPrefix) + "role";
        String kPinR = String(keyPrefix) + "pinR";
        String kPinG = String(keyPrefix) + "pinG";
        String kPinB = String(keyPrefix) + "pinB";
        String kInv = String(keyPrefix) + "inv";

        m_prefs.putBool(kEn.c_str(), m_slots[slot].enabled);
        m_prefs.putUChar(kType.c_str(), (uint8_t)m_slots[slot].type);
        m_prefs.putUChar(kRole.c_str(), (uint8_t)m_slots[slot].role);
        m_prefs.putUChar(kPinR.c_str(), m_slots[slot].pinR);
        m_prefs.putUChar(kPinG.c_str(), m_slots[slot].pinG);
        m_prefs.putUChar(kPinB.c_str(), m_slots[slot].pinB);
        m_prefs.putBool(kInv.c_str(), m_slots[slot].inverted);
    }

    void loadFromPrefs() {
        for (uint8_t slot = 1; slot < MAX_LED_SLOTS; slot++) {
            char keyPrefix[16];
            snprintf(keyPrefix, sizeof(keyPrefix), "slot_%u_", slot);
            String kEn = String(keyPrefix) + "en";

            if (m_prefs.getBool(kEn.c_str(), false)) {
                String kType = String(keyPrefix) + "type";
                String kRole = String(keyPrefix) + "role";
                String kPinR = String(keyPrefix) + "pinR";
                String kPinG = String(keyPrefix) + "pinG";
                String kPinB = String(keyPrefix) + "pinB";
                String kInv = String(keyPrefix) + "inv";

                m_slots[slot].enabled = true;
                m_slots[slot].type = (LedType)m_prefs.getUChar(kType.c_str(), LED_TYPE_SINGLE);
                m_slots[slot].role = (LedRole)m_prefs.getUChar(kRole.c_str(), ROLE_MIRROR);
                m_slots[slot].pinR = m_prefs.getUChar(kPinR.c_str(), 0);
                m_slots[slot].pinG = m_prefs.getUChar(kPinG.c_str(), 0);
                m_slots[slot].pinB = m_prefs.getUChar(kPinB.c_str(), 0);
                m_slots[slot].inverted = m_prefs.getBool(kInv.c_str(), false);

                if (isSafeGpio(m_slots[slot].pinR)) {
                    setupHardware(slot);
                    initSlotRolePattern(slot);
                }
            }
        }
    }

    LedSlot m_slots[MAX_LED_SLOTS];
    Preferences m_prefs;
    bool m_broken;
    int m_battState;
    uint8_t m_battPct;
};
