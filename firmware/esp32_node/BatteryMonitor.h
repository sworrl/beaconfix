#pragma once
#include <Arduino.h>
#include "LedPatterns.h"
#include "BatteryTrainer.h"
#include <Preferences.h>

enum BatteryState {
    BATT_UNKNOWN = 0,
    BATT_NO_BATTERY,  // Powered via USB / external 5V; no 3.7V cell detected
    BATT_CHARGING,    // Battery connected & charging (>= 4.22V)
    BATT_FULL,        // Battery full (>= 4.05V / ~80-100%)
    BATT_HALFWAY,     // Battery normal / halfway (3.65V - 4.05V / ~35-79%)
    BATT_DYING        // Battery low / dying (< 3.65V / < 20%)
};

class BatteryMonitor {
public:
    static BatteryMonitor& instance() {
        static BatteryMonitor inst;
        return inst;
    }

    void begin(int pin = 35, float dividerRatio = 2.0f) {
        // A pin/divider set with `batt pin` / `batt divider` or found by the scan survives reboots
        Preferences prefs;
        prefs.begin("battmon", true);
        m_pin = prefs.getInt("pin", pin);
        m_dividerRatio = prefs.getFloat("divider", dividerRatio);
        m_pinFixed = prefs.getBool("fixed", false);
        prefs.end();
        pinMode(m_pin, INPUT);
        m_lastReadTime = 0;
        m_sampleCount = 0;
        m_readingsTotal = 0;
        m_batteryMv = 0;
        m_percentage = 0;
        m_state = BATT_NO_BATTERY;
        m_trainer.begin();
        if (!m_pinFixed && readPinMv(m_pin) * m_dividerRatio < 1200) autoDetectPin();
        readSensor();
    }

    // Set by the user: kept across reboots and never overridden by the scan
    void setPin(int pin) {
        m_pin = pin;
        pinMode(m_pin, INPUT);
        resetSamples();
        m_pinFixed = true;
        save();
    }

    void setDivider(float ratio) {
        if (ratio > 0.1f && ratio < 20.0f) {
            m_dividerRatio = ratio;
            resetSamples();
            save();
        }
    }

    // Back to automatic: forget a user-set pin and scan again
    void setPinAuto() {
        m_pinFixed = false;
        save();
        autoDetectPin();
    }
    bool isPinFixed() const { return m_pinFixed; }

    // Input-only ADC1 pins: safe to read with Wi-Fi up, and never an LED output (32/33 can be)
    static constexpr int kScanPins[4] = {35, 34, 36, 39};

    uint32_t readPinMv(int pin) {
        pinMode(pin, INPUT);
        uint32_t sum = 0;
        for (int i = 0; i < 4; i++) sum += analogReadMilliVolts(pin);
        return sum / 4;
    }

    // A divided Li-ion cell (2.8-4.4 V through the divider) on one of the scan pins; a floating pin reads under ~0.6 V
    bool autoDetectPin() {
        for (int pin : kScanPins) {
            uint32_t mv = (uint32_t)(readPinMv(pin) * m_dividerRatio);
            if (mv >= 2800 && mv <= 4400) {
                if (pin != m_pin) {
                    m_pin = pin;
                    resetSamples();
                    save();
                    Serial.printf("{\"type\":\"battery_pin_detected\",\"pin\":%d,\"mv\":%lu}\n", pin, (unsigned long)mv);
                }
                pinMode(m_pin, INPUT);
                return true;
            }
        }
        pinMode(m_pin, INPUT);
        return false;
    }

    int getPin() const { return m_pin; }
    float getDivider() const { return m_dividerRatio; }
    uint32_t getMilliVolts() const { return m_batteryMv; }
    uint8_t getPercentage() const { return m_percentage; }
    BatteryState getState() const { return m_state; }
    bool isCharging() const { return m_state == BATT_CHARGING; }
    bool isDying() const { return m_state == BATT_DYING; }
    bool hasBattery() const { return m_state != BATT_NO_BATTERY; }

    void resetBatteryStats() {
        m_trainer.reset();
        readSensor();
    }

    const BatteryTrainingStats& getTrainingStats() const { return m_trainer.getStats(); }
    bool isTrained() const { return m_trainer.getStats().isTrained; }
    uint8_t getTrainingPct() const { return m_trainer.getStats().trainPct; }
    float getBatteryCycles() const { return m_trainer.getStats().cycles; }
    uint32_t getBatteryRuntimeSec() const { return m_trainer.getStats().runtimeSec; }

    uint32_t getRawMilliVolts() const { return m_rawMv; }

    const char* getDiagnosticVerdict() const {
        if (m_rawMv < 50) return "DISCONNECTED_OR_PULLED_LOW";
        if (m_batteryMv < 1200) return "USB_POWERED_NO_BATTERY";
        if (m_batteryMv < 3300) return "DEEPLY_DISCHARGED_CELL";
        if (m_batteryMv > 4350) return "OVER_VOLTAGE_CHECK_DIVIDER";
        return "BATTERY_HEALTHY";
    }

    const char* getDiagnosticAdvice() const {
        if (m_rawMv < 50) return "No voltage on ADC pin. Check 100k resistor to Battery (+), and check common GND.";
        if (m_batteryMv < 1200) return "No cell voltage seen. The ESP32 can only measure the cell through a divider: 100k from TP4056 B+ to GPIO 34/35/36/39 and 100k from that pin to GND (common GND). The node finds the pin by itself within a minute.";
        if (m_batteryMv < 3300) return "Battery is depleted (<3.3V). Charge via TP4056 USB-C port.";
        if (m_batteryMv > 4350) return "Voltage exceeds 4.35V! Verify 100k/100k resistor divider ratio.";
        return "3.7V Li-ion battery is connected and reading correctly.";
    }

    const char* getStateStr() const {
        switch (m_state) {
            case BATT_NO_BATTERY: return "no_battery";
            case BATT_CHARGING:   return "charging";
            case BATT_FULL:       return "full";
            case BATT_HALFWAY:    return "halfway";
            case BATT_DYING:      return "dying";
            default:              return "unknown";
        }
    }

    void update() {
        unsigned long now = millis();
        if (now - m_lastReadTime >= 1000) { // update once per second
            m_lastReadTime = now;
            readSensor();
        }
        // Nothing on the pin yet: a cell wired up later (or to another pin) gets found within a minute
        if (!m_pinFixed && m_state == BATT_NO_BATTERY && now - m_lastScanTime >= 60000) {
            m_lastScanTime = now;
            autoDetectPin();
        }
    }

    void applyLedPattern() {
        switch (m_state) {
            case BATT_NO_BATTERY:
                LedPatterns::instance().setBasePattern(PATTERN_BATT_NO_BATTERY);
                break;
            case BATT_CHARGING:
                LedPatterns::instance().setBasePattern(PATTERN_BATT_CHARGING);
                break;
            case BATT_FULL:
                LedPatterns::instance().setBasePattern(PATTERN_BATT_FULL);
                break;
            case BATT_HALFWAY:
                LedPatterns::instance().setBasePattern(PATTERN_BATT_HALFWAY);
                break;
            case BATT_DYING:
                LedPatterns::instance().setBasePattern(PATTERN_BATT_DYING);
                break;
            default:
                break;
        }
    }

private:
    BatteryMonitor() : m_pin(35), m_dividerRatio(2.0f), m_lastReadTime(0),
                       m_batteryMv(0), m_percentage(0), m_state(BATT_NO_BATTERY),
                       m_sampleCount(0), m_readingsTotal(0) {}

    void save() {
        Preferences prefs;
        prefs.begin("battmon", false);
        prefs.putInt("pin", m_pin);
        prefs.putFloat("divider", m_dividerRatio);
        prefs.putBool("fixed", m_pinFixed);
        prefs.end();
    }

    void resetSamples() {
        m_sampleCount = 0;
        m_sampleIndex = 0;
        m_readingsTotal = 0;
    }

    void readSensor() {
        // Read ADC with ESP32 calibration
        uint32_t rawMv = analogReadMilliVolts(m_pin);
        m_rawMv = rawMv;
        uint32_t calculatedMv = (uint32_t)(rawMv * m_dividerRatio);

        // Moving average filter over 8 samples
        if (m_sampleCount < 8) {
            m_samples[m_sampleCount++] = calculatedMv;
            m_readingsTotal += calculatedMv;
        } else {
            m_readingsTotal -= m_samples[m_sampleIndex];
            m_samples[m_sampleIndex] = calculatedMv;
            m_readingsTotal += calculatedMv;
            m_sampleIndex = (m_sampleIndex + 1) % 8;
        }

        m_batteryMv = m_readingsTotal / m_sampleCount;

        BatteryState oldState = m_state;

        // Determine battery state & percentage with training algorithm
        if (m_batteryMv < 1200) {
            m_percentage = 0;
            m_state = BATT_NO_BATTERY;
        } else {
            bool charging = (m_batteryMv >= 4220);
            bool onBattery = hasBattery() && !charging;
            m_percentage = m_trainer.updateAndCalculatePct(m_batteryMv, charging, onBattery);

            if (charging) {
                m_state = BATT_CHARGING;
            } else if (m_percentage >= 80) {
                m_state = BATT_FULL;
            } else if (m_percentage >= 20) {
                m_state = BATT_HALFWAY;
            } else {
                m_state = BATT_DYING;
            }
        }

        if (m_state != oldState) {
            // Apply corresponding base LED pattern for internal & mirror LEDs
            applyLedPattern();
            // Notify LedPatterns of battery state change for dedicated full-time battery LEDs
            LedPatterns::instance().notifyBatteryState(m_state, m_percentage);
        }
    }

    int m_pin;
    float m_dividerRatio;
    unsigned long m_lastReadTime;
    uint32_t m_batteryMv;
    uint32_t m_rawMv = 0;
    uint8_t m_percentage;
    BatteryState m_state;
    BatteryTrainer m_trainer;

    uint32_t m_samples[8];
    uint8_t m_sampleCount;
    uint8_t m_sampleIndex = 0;
    uint32_t m_readingsTotal;
    bool m_pinFixed = false;
    unsigned long m_lastScanTime = 0;
};
