#pragma once
#include <Arduino.h>
#include "HeltecV3Pins.h"
#include "LedPatterns.h"
#include "NodeConfig.h"

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

    void begin(int pin = HeltecV3::PIN_BATT_ADC, float dividerRatio = HeltecV3::BATT_DIVIDER_RATIO) {
        m_pin = pin;
        m_dividerRatio = dividerRatio;
        pinMode(HeltecV3::PIN_ADC_CTRL, OUTPUT);
        digitalWrite(HeltecV3::PIN_ADC_CTRL, HIGH); // Default isolated
        pinMode(m_pin, INPUT);

        m_lastReadTime = 0;
        m_sampleCount = 0;
        m_sampleIndex = 0;
        m_readingsTotal = 0;
        m_batteryMv = 0;
        m_percentage = 0;
        m_rawMv = 0;
        m_state = BATT_NO_BATTERY;
        readSensor();
    }

    void setPin(int pin) {
        m_pin = pin;
        pinMode(m_pin, INPUT);
    }

    void setDivider(float ratio) {
        if (ratio > 0.1f && ratio < 20.0f) {
            m_dividerRatio = ratio;
        }
    }

    int getPin() const { return m_pin; }
    float getDivider() const { return m_dividerRatio; }
    uint32_t getMilliVolts() const { return m_batteryMv; }
    uint32_t getRawMilliVolts() const { return m_rawMv; }
    uint8_t getPercentage() const { return m_percentage; }
    BatteryState getState() const { return m_state; }
    bool isCharging() const { return m_state == BATT_CHARGING; }
    bool isDying() const { return m_state == BATT_DYING; }
    bool hasBattery() const { return NodeConfig::instance().hasBatteryEquipped() && (m_state != BATT_NO_BATTERY); }

    const char* getDiagnosticVerdict() const {
        if (m_rawMv < 50) return "DISCONNECTED_OR_PULLED_LOW";
        if (m_batteryMv < 1200) return "USB_POWERED_NO_BATTERY";
        if (m_batteryMv < 3300) return "DEEPLY_DISCHARGED_CELL";
        if (m_batteryMv > 4350) return "OVER_VOLTAGE_CHECK_DIVIDER";
        return "BATTERY_HEALTHY";
    }

    const char* getDiagnosticAdvice() const {
        if (m_rawMv < 50) return "No voltage on ADC pin. Check battery JST connector or wiring to BAT (+).";
        if (m_batteryMv < 1200) return "Node is running on USB 5V without a battery attached (normal).";
        if (m_batteryMv < 3300) return "Battery is depleted (<3.3V). Charge via Heltec USB-C port.";
        if (m_batteryMv > 4350) return "Voltage exceeds 4.35V! Verify calibration ratio.";
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
        if (now - m_lastReadTime >= 1000) {
            m_lastReadTime = now;
            readSensor();
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
    BatteryMonitor() : m_pin(HeltecV3::PIN_BATT_ADC), m_dividerRatio(HeltecV3::BATT_DIVIDER_RATIO),
                       m_lastReadTime(0), m_batteryMv(0), m_rawMv(0), m_percentage(0),
                       m_state(BATT_NO_BATTERY), m_sampleCount(0), m_sampleIndex(0), m_readingsTotal(0) {}

    void readSensor() {
        if (!NodeConfig::instance().hasBatteryEquipped()) {
            m_rawMv = 0;
            m_batteryMv = 0;
            m_percentage = 0;
            m_state = BATT_NO_BATTERY;
            m_sampleCount = 0;
            m_sampleIndex = 0;
            m_readingsTotal = 0;
            return;
        }

        // 1. Enable Heltec V3 divider circuit by pulling ADC_CTRL (GPIO 37) LOW
        digitalWrite(HeltecV3::PIN_ADC_CTRL, LOW);
        delayMicroseconds(120);

        // 2. Read calibrated millivolts from ADC1 CH0 (GPIO 1)
        uint32_t rawMv = analogReadMilliVolts(m_pin);
        m_rawMv = rawMv;

        // 3. Disable divider circuit to prevent battery drain
        digitalWrite(HeltecV3::PIN_ADC_CTRL, HIGH);

        uint32_t calculatedMv = (uint32_t)(rawMv * m_dividerRatio);

        // Moving average over 8 samples
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

        // Determine battery state & percentage for 3.7V vape Li-ion cell
        if (m_batteryMv < 1200) {
            m_percentage = 0;
            m_state = BATT_NO_BATTERY;
        } else {
            if (m_batteryMv >= 4180) {
                m_percentage = 100;
            } else if (m_batteryMv <= 3350) {
                m_percentage = 0;
            } else if (m_batteryMv >= 4050) {
                m_percentage = 80 + (uint8_t)(((m_batteryMv - 4050) * 20) / 130);
            } else if (m_batteryMv >= 3700) {
                m_percentage = 35 + (uint8_t)(((m_batteryMv - 3700) * 45) / 350);
            } else {
                m_percentage = (uint8_t)(((m_batteryMv - 3350) * 35) / 350);
            }

            if (m_batteryMv >= 4220) {
                m_state = BATT_CHARGING;
            } else if (m_batteryMv >= 4050) {
                m_state = BATT_FULL;
            } else if (m_batteryMv >= 3650) {
                m_state = BATT_HALFWAY;
            } else {
                m_state = BATT_DYING;
            }
        }

        if (m_state != oldState) {
            applyLedPattern();
            LedPatterns::instance().notifyBatteryState(m_state, m_percentage);
        }
    }

    int m_pin;
    float m_dividerRatio;
    unsigned long m_lastReadTime;
    uint32_t m_batteryMv;
    uint32_t m_rawMv;
    uint8_t m_percentage;
    BatteryState m_state;

    uint32_t m_samples[8];
    uint8_t m_sampleCount;
    uint8_t m_sampleIndex;
    uint32_t m_readingsTotal;
};
