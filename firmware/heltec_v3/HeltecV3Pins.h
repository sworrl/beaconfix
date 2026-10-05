#pragma once
#include <Arduino.h>

/**
 * Heltec WiFi LoRa 32 (V3) Hardware Pin Definitions
 * MCU: ESP32-S3FN8 (Dual-core LX7 @ 240MHz)
 * Radio: Semtech SX1262 LoRa
 * Display: 0.96" SSD1306 128x64 OLED (I2C)
 */

namespace HeltecV3 {

    // Power Control Pin
    // Pull LOW to enable Vext (powers OLED display, LoRa power rail, and external sensors)
    constexpr uint8_t PIN_VEXT = 36;

    // Onboard User LED
    constexpr uint8_t PIN_LED = 35;

    // PRG / User Button (active LOW, pulled high)
    constexpr uint8_t PIN_USER_BUTTON = 0;

    // 0.96" SSD1306 OLED (I2C)
    constexpr uint8_t PIN_OLED_SDA = 17;
    constexpr uint8_t PIN_OLED_SCL = 18;
    constexpr uint8_t PIN_OLED_RST = 21;
    constexpr uint8_t OLED_I2C_ADDR = 0x3C;

    // Semtech SX1262 LoRa Transceiver (SPI)
    constexpr uint8_t PIN_LORA_NSS  = 8;   // Chip Select / SS
    constexpr uint8_t PIN_LORA_SCK  = 9;   // SPI Clock
    constexpr uint8_t PIN_LORA_MOSI = 10;  // SPI MOSI
    constexpr uint8_t PIN_LORA_MISO = 11;  // SPI MISO
    constexpr uint8_t PIN_LORA_RST  = 12;  // Reset
    constexpr uint8_t PIN_LORA_BUSY = 13;  // Busy indicator
    constexpr uint8_t PIN_LORA_DIO1 = 14;  // Interrupt / Packet Ready

    // Battery ADC Voltage Divider Circuit
    // Pin 37 controls the P-FET / divider enable (pull LOW to measure, HIGH to isolate)
    constexpr uint8_t PIN_ADC_CTRL = 37;
    constexpr uint8_t PIN_BATT_ADC = 1;    // ADC1 Channel 0 (GPIO 1)
    
    // Heltec V3 hardware divider: (390k + 100k) / 100k = 4.9x
    constexpr float BATT_DIVIDER_RATIO = 4.90f;

    inline void enableVext(bool enable = true) {
        // Pull LOW to enable Vext (powers OLED display, LoRa power rail, and external sensors)
        pinMode(PIN_VEXT, OUTPUT);
        digitalWrite(PIN_VEXT, enable ? LOW : HIGH);
    }

    inline void resetOled() {
        // Hardware reset sequence for SSD1306 OLED
        pinMode(PIN_OLED_RST, OUTPUT);
        digitalWrite(PIN_OLED_RST, LOW);
        delay(25);
        digitalWrite(PIN_OLED_RST, HIGH);
        delay(25);
    }

    inline void initPower() {
        // Power on Vext rail (OLED + LoRa)
        enableVext(true);

        // Reset OLED hardware (only during initial board boot)
        resetOled();

        // Setup user button
        pinMode(PIN_USER_BUTTON, INPUT_PULLUP);

        // Setup battery divider control pin
        pinMode(PIN_ADC_CTRL, OUTPUT);
        digitalWrite(PIN_ADC_CTRL, HIGH); // Default HIGH (isolated to prevent battery drain)
    }

} // namespace HeltecV3
