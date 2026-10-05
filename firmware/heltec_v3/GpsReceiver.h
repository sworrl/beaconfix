#pragma once
#include <Arduino.h>
#include <HardwareSerial.h>
#include "NodeConfig.h"
#include "TimeSync.h"

#if BEACONFIX_HW_TYPE == 1
  // Heltec WiFi LoRa 32 (V3) Hardware UART pins
  #define GPS_PIN_RX 47
  #define GPS_PIN_TX 48
#else
  // Generic ESP32 Hardware UART pins (UART2)
  #define GPS_PIN_RX 16
  #define GPS_PIN_TX 17
#endif

class GpsReceiver {
public:
    static GpsReceiver& instance() {
        static GpsReceiver inst;
        return inst;
    }

    void begin(uint32_t baudRate = 9600) {
        m_baudRate = baudRate;
        m_serial.begin(baudRate, SERIAL_8N1, GPS_PIN_RX, GPS_PIN_TX);
        m_lastCharTime = millis();
        m_hasHardware = false;
        m_bufPos = 0;
        m_satellites = 0;
        m_hdop = 99.9f;
        m_altitudeM = 0.0f;
    }

    void update() {
        while (m_serial.available() > 0) {
            char c = (char)m_serial.read();
            m_lastCharTime = millis();
            m_hasHardware = true;

            if (c == '$') {
                m_bufPos = 0;
                m_lineBuf[m_bufPos++] = c;
            } else if (c == '\r' || c == '\n') {
                if (m_bufPos > 10) {
                    m_lineBuf[m_bufPos] = '\0';
                    parseNmeaSentence(m_lineBuf);
                }
                m_bufPos = 0;
            } else if (m_bufPos < sizeof(m_lineBuf) - 1) {
                m_lineBuf[m_bufPos++] = c;
            }
        }

        // If no hardware GPS activity seen for 10s, flag as absent
        if (millis() - m_lastCharTime > 10000) {
            m_hasHardware = false;
        }
    }

    bool hasHardwareDetected() const { return m_hasHardware; }
    uint8_t getSatellites() const { return m_satellites; }
    float getHdop() const { return m_hdop; }

private:
    GpsReceiver() : m_serial(1), m_bufPos(0), m_baudRate(9600),
                    m_lastCharTime(0), m_hasHardware(false),
                    m_satellites(0), m_hdop(99.9f), m_altitudeM(0.0f) {}

    static double parseCoord(const char* str, char dir) {
        if (!str || str[0] == '\0') return 0.0;
        double raw = atof(str);
        int deg = (int)(raw / 100.0);
        double min = raw - (deg * 100.0);
        double dec = deg + (min / 60.0);
        if (dir == 'S' || dir == 's' || dir == 'W' || dir == 'w') {
            dec = -dec;
        }
        return dec;
    }

    static int getField(const char* sentence, int fieldNum, char* out, size_t maxLen) {
        int curField = 0;
        size_t outPos = 0;
        for (size_t i = 0; sentence[i] != '\0' && sentence[i] != '*'; i++) {
            if (sentence[i] == ',') {
                curField++;
                if (curField == fieldNum) {
                    outPos = 0;
                } else if (curField > fieldNum) {
                    break;
                }
            } else if (curField == fieldNum) {
                if (outPos < maxLen - 1) {
                    out[outPos++] = sentence[i];
                }
            }
        }
        out[outPos] = '\0';
        return outPos;
    }

    void parseNmeaSentence(const char* s) {
        // Fast NMEA sentence prefix check
        if (strncmp(s, "$GNRMC", 6) == 0 || strncmp(s, "$GPRMC", 6) == 0) {
            char status[4], latStr[16], latDir[4], lonStr[16], lonDir[4], spdStr[12], hdgStr[12], timeStr[16], dateStr[16];
            getField(s, 2, status, sizeof(status));

            if (status[0] == 'A') { // 'A' = Valid Fix, 'V' = Warning / No Fix
                getField(s, 1, timeStr, sizeof(timeStr));
                getField(s, 3, latStr, sizeof(latStr));
                getField(s, 4, latDir, sizeof(latDir));
                getField(s, 5, lonStr, sizeof(lonStr));
                getField(s, 6, lonDir, sizeof(lonDir));
                getField(s, 7, spdStr, sizeof(spdStr));
                getField(s, 8, hdgStr, sizeof(hdgStr));
                getField(s, 9, dateStr, sizeof(dateStr));

                double lat = parseCoord(latStr, latDir[0]);
                double lon = parseCoord(lonStr, lonDir[0]);
                float speedMps = atof(spdStr) * 0.514444f; // Knots to m/s
                float headingDeg = (hdgStr[0] != '\0') ? atof(hdgStr) : -1.0f;
                float accM = (m_hdop > 0.0f && m_hdop < 20.0f) ? (m_hdop * 3.5f) : 6.0f;

                NodeConfig::instance().setGpsFix(lat, lon, accM, speedMps, headingDeg, m_altitudeM, m_satellites);

                // Compute UTC epoch from timeStr (hhmmss.ss) and dateStr (ddmmyy)
                if (strlen(timeStr) >= 6 && strlen(dateStr) == 6) {
                    struct tm t;
                    memset(&t, 0, sizeof(t));
                    char dd[3] = {dateStr[0], dateStr[1], '\0'};
                    char mm[3] = {dateStr[2], dateStr[3], '\0'};
                    char yy[3] = {dateStr[4], dateStr[5], '\0'};
                    char hh[3] = {timeStr[0], timeStr[1], '\0'};
                    char mi[3] = {timeStr[2], timeStr[3], '\0'};
                    char ss[3] = {timeStr[4], timeStr[5], '\0'};

                    t.tm_mday = atoi(dd);
                    t.tm_mon  = atoi(mm) - 1;
                    t.tm_year = 2000 + atoi(yy) - 1900;
                    t.tm_hour = atoi(hh);
                    t.tm_min  = atoi(mi);
                    t.tm_sec  = atoi(ss);

                    time_t epochSec = mktime(&t);
                    if (epochSec > 1700000000) {
                        uint64_t epochUs = (uint64_t)epochSec * 1000000ULL;
                        // GPS time lock promotes node to Stratum 1 Master
                        TimeSync::instance().setMasterTimeUs(epochUs);
                    }
                }
            }
        } else if (strncmp(s, "$GNGGA", 6) == 0 || strncmp(s, "$GPGGA", 6) == 0) {
            char fixQual[4], satsStr[6], hdopStr[8], altStr[12];
            getField(s, 6, fixQual, sizeof(fixQual));
            getField(s, 7, satsStr, sizeof(satsStr));
            getField(s, 8, hdopStr, sizeof(hdopStr));
            getField(s, 9, altStr, sizeof(altStr));

            if (fixQual[0] != '0') {
                m_satellites = (uint8_t)atoi(satsStr);
                if (hdopStr[0] != '\0') m_hdop = atof(hdopStr);
                if (altStr[0] != '\0') m_altitudeM = atof(altStr);
            }
        }
    }

    HardwareSerial m_serial;
    char m_lineBuf[128];
    uint8_t m_bufPos;
    uint32_t m_baudRate;
    unsigned long m_lastCharTime;
    bool m_hasHardware;
    uint8_t m_satellites;
    float m_hdop;
    float m_altitudeM;
};
