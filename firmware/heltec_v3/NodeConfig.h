#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <esp_mac.h>

/**
 * Reddit-style Node Auto-Naming & NVS Configuration Store.
 * 
 * Generates names in the format: WordWordWordNUMBER (e.g. SwiftFalconRanger4821).
 * Seeded deterministically from the ESP32 MAC address so every flashed board
 * gets a consistent, memorable name out-of-the-box.
 * Users can rename the node at will via Serial, BLE, or Web UI; the custom
 * name is saved in NVS (Preferences) and persists across reboots.
 */

enum NodeOpMode : uint8_t {
    OP_MODE_MOBILE = 0,       // Portable / battery-optimized node (client mesh, adaptive TX, sleep timeout)
    OP_MODE_BASE_STATION = 1  // Permanent host / desktop station (root gateway, Stratum 1 master, continuous full-power)
};

#define BEACONFIX_HW_TYPE 1 // Heltec V3 ESP32-S3
#define BEACONFIX_FW_VERSION "3.10.5"
#define BEACONFIX_BUILD_DATE __DATE__

class NodeConfig {
public:
    static NodeConfig& instance() {
        static NodeConfig inst;
        return inst;
    }

    const char* getVersion() const { return BEACONFIX_FW_VERSION; }
    const char* getBuildDate() const { return BEACONFIX_BUILD_DATE; }

    void begin() {
        m_prefs.begin("bcf_node", false);

        // Load operational mode (default: Base Station if Heltec V3 on USB, or Mobile)
        m_opMode = (NodeOpMode)m_prefs.getUChar("op_mode", OP_MODE_BASE_STATION);

        // Load attached device if assigned
        if (m_prefs.isKey("attached_dev")) {
            String dev = m_prefs.getString("attached_dev", "");
            strncpy(m_attachedDevice, dev.c_str(), sizeof(m_attachedDevice) - 1);
            m_attachedDevice[sizeof(m_attachedDevice) - 1] = '\0';
        } else {
            m_attachedDevice[0] = '\0';
        }

        if (m_prefs.isKey("name")) {
            String savedName = m_prefs.getString("name", "");
            if (savedName.length() >= 3 && savedName.length() < sizeof(m_nodeName)) {
                strncpy(m_nodeName, savedName.c_str(), sizeof(m_nodeName) - 1);
                m_nodeName[sizeof(m_nodeName) - 1] = '\0';
                return;
            }
        }

        // Generate deterministic Reddit-style name from chip MAC
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        generateRedditName(mac, m_nodeName, sizeof(m_nodeName));
        m_prefs.putString("name", m_nodeName);
    }

    NodeOpMode getOpMode() const { return m_opMode; }
    bool isBaseStation() const { return m_opMode == OP_MODE_BASE_STATION; }
    bool isMobile() const { return m_opMode == OP_MODE_MOBILE; }
    const char* getOpModeStr() const {
        return (m_opMode == OP_MODE_BASE_STATION) ? "base_station" : "mobile";
    }

    bool setOpMode(NodeOpMode mode) {
        m_opMode = mode;
        m_prefs.putUChar("op_mode", (uint8_t)mode);
        return true;
    }

    const char* getName() const {
        return m_nodeName;
    }

    bool setName(const char* newName) {
        if (!newName || strlen(newName) < 3 || strlen(newName) >= sizeof(m_nodeName)) {
            return false;
        }

        // Clean name (alphanumeric, dashes, underscores)
        char cleaned[36];
        int j = 0;
        for (int i = 0; newName[i] != '\0' && j < 32; i++) {
            char c = newName[i];
            if (isalnum(c) || c == '-' || c == '_') {
                cleaned[j++] = c;
            }
        }
        cleaned[j] = '\0';

        if (j < 3) return false;

        strncpy(m_nodeName, cleaned, sizeof(m_nodeName) - 1);
        m_nodeName[sizeof(m_nodeName) - 1] = '\0';
        m_prefs.putString("name", m_nodeName);
        return true;
    }

    void resetToDefaultName() {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        generateRedditName(mac, m_nodeName, sizeof(m_nodeName));
        m_prefs.putString("name", m_nodeName);
    }

    bool setWifiCreds(const char* ssid, const char* pass) {
        if (!ssid || strlen(ssid) == 0) {
            m_prefs.remove("wifi_ssid");
            m_prefs.remove("wifi_pass");
            return true;
        }
        m_prefs.putString("wifi_ssid", ssid);
        m_prefs.putString("wifi_pass", pass ? pass : "");
        return true;
    }

    String getWifiSsid() {
        return m_prefs.getString("wifi_ssid", "");
    }

    String getWifiPass() {
        return m_prefs.getString("wifi_pass", "");
    }

    bool hasWifiCreds() {
        return m_prefs.isKey("wifi_ssid") && m_prefs.getString("wifi_ssid", "").length() > 0;
    }

    void clearWifiCreds() {
        m_prefs.remove("wifi_ssid");
        m_prefs.remove("wifi_pass");
    }

    bool hasBatteryEquipped() {
        if (m_prefs.isKey("has_batt")) {
            return m_prefs.getBool("has_batt", false);
        }
        // Base stations running on host USB default to NO battery
        // Mobile nodes default to having a battery
        return isMobile();
    }

    void setBatteryEquipped(bool equipped) {
        m_prefs.putBool("has_batt", equipped);
        if (!equipped) {
            m_prefs.putUInt("batt_mah", 0);
        } else if (m_prefs.getUInt("batt_mah", 0) == 0) {
            m_prefs.putUInt("batt_mah", 240);
        }
    }

    uint32_t getBattMah() {
        if (!hasBatteryEquipped()) return 0;
        return m_prefs.getUInt("batt_mah", 240); // 240 mAh default for Heltec V3
    }

    bool setBattMah(uint32_t mah) {
        if (mah == 0) {
            setBatteryEquipped(false);
            return true;
        }
        if (mah < 50 || mah > 50000) return false;
        m_prefs.putUInt("batt_mah", mah);
        m_prefs.putBool("has_batt", true);
        return true;
    }

    // Follow / Attached Device State
    bool isAttached() const { return m_attachedDevice[0] != '\0'; }
    const char* getAttachedDevice() const { return m_attachedDevice; }
    bool setAttachedDevice(const char* devName) {
        if (!devName || strlen(devName) == 0) {
            m_attachedDevice[0] = '\0';
            m_prefs.remove("attached_dev");
            return true;
        }
        strncpy(m_attachedDevice, devName, sizeof(m_attachedDevice) - 1);
        m_attachedDevice[sizeof(m_attachedDevice) - 1] = '\0';
        m_prefs.putString("attached_dev", m_attachedDevice);
        return true;
    }
    void clearAttachedDevice() {
        m_attachedDevice[0] = '\0';
        m_prefs.remove("attached_dev");
    }

    // Dynamic Follow / GPS & Travel/Motion state injected from phone, host, or GNSS
    bool hasGpsFix() const {
        return m_gpsValid && (millis() - m_lastGpsUpdateMs < 45000);
    }
    bool hasLocation() const {
        return (m_lat != 0.0 || m_lon != 0.0);
    }
    void setTripDistKm(float distKm) {
        if (distKm >= 0.0f) {
            m_tripDistM = distKm * 1000.0f;
        }
    }
    void setMaxSpeedKmh(float spdKmh) {
        if (spdKmh >= 0.0f) {
            m_maxSpeedMps = spdKmh / 3.6f;
        }
    }
    void resetTrip() {
        m_tripDistM = 0.0f;
        m_maxSpeedMps = 0.0f;
    }
    void setGpsFix(double lat, double lon, float accM = 5.0f, float speedMps = -1.0f, float headingDeg = -1.0f, float altM = 0.0f, uint8_t sats = 0, float tripKm = -1.0f) {
        unsigned long now = millis();
        if (tripKm >= 0.0f) {
            m_tripDistM = tripKm * 1000.0f;
        } else if (m_gpsValid && now > m_lastGpsUpdateMs && lat != 0.0 && lon != 0.0 && m_lat != 0.0 && m_lon != 0.0) {
            double dLat = (lat - m_lat) * 111320.0;
            double dLon = (lon - m_lon) * 111320.0 * cos(lat * 0.0174532925);
            double distM = sqrt(dLat * dLat + dLon * dLon);
            if (distM < 5000.0) {
                m_tripDistM += (float)distM;
                if (speedMps < 0.0f) {
                    float dtS = (now - m_lastGpsUpdateMs) / 1000.0f;
                    if (dtS > 0.5f) speedMps = (float)(distM / dtS);
                }
            }
        }
        m_lat = lat;
        m_lon = lon;
        m_accM = accM;
        if (speedMps >= 0.0f) m_speedMps = speedMps;
        if (headingDeg >= 0.0f) m_headingDeg = headingDeg;
        if (altM != 0.0f) m_altM = altM;
        if (sats > 0) m_sats = sats;
        m_gpsValid = true;
        m_lastGpsUpdateMs = now;

        if (m_speedMps > 1.2f) {
            m_isMoving = true;
            m_lastMoveMs = now;
            if (m_speedMps > m_maxSpeedMps) m_maxSpeedMps = m_speedMps;
        } else if (now - m_lastMoveMs > 15000) {
            m_isMoving = false;
        }
    }

    void setTravelState(bool moving, float speedKmh = -1.0f, float headingDeg = -1.0f, float altM = 0.0f, float tripKm = -1.0f) {
        m_isMoving = moving;
        m_lastGpsUpdateMs = millis();
        if (moving) m_lastMoveMs = millis();
        if (speedKmh >= 0.0f) {
            m_speedMps = speedKmh / 3.6f;
            if (m_speedMps > m_maxSpeedMps) m_maxSpeedMps = m_speedMps;
        }
        if (headingDeg >= 0.0f) m_headingDeg = headingDeg;
        if (altM != 0.0f) m_altM = altM;
        if (tripKm >= 0.0f) setTripDistKm(tripKm);
    }

    void clearGpsFix() {
        m_gpsValid = false;
        m_isMoving = false;
    }
    double getLat() const { return m_lat; }
    double getLon() const { return m_lon; }
    float getAccM() const { return m_accM; }
    unsigned long getLastGpsUpdateMs() const { return m_lastGpsUpdateMs; }

    bool isTraveling() const { return m_isMoving; }
    const char* getTravelStateStr() const {
        if (m_isMoving) return "MOVING";
        if (hasGpsFix()) return "STATIONARY";
        if (m_tripDistM > 5.0f || (m_lat != 0.0 && m_lon != 0.0)) return "SYNCED";
        return "ACQUIRING";
    }
    float getSpeedMps() const { return m_speedMps; }
    float getSpeedKmh() const { return m_speedMps * 3.6f; }
    float getSpeedMph() const { return m_speedMps * 2.23694f; }
    float getMaxSpeedKmh() const { return m_maxSpeedMps * 3.6f; }
    float getHeadingDeg() const { return m_headingDeg; }
    float getAltM() const { return m_altM; }
    uint8_t getSats() const { return m_sats; }
    float getTripDistKm() const { return m_tripDistM / 1000.0f; }
    float getTripDistMiles() const { return m_tripDistM * 0.000621371f; }
    unsigned long getLastMoveMs() const { return m_lastMoveMs; }

    const char* getHeadingCard() const {
        if (m_headingDeg < 0.0f) return "--";
        float h = fmod(m_headingDeg, 360.0f);
        if (h < 0.0f) h += 360.0f;
        if (h < 22.5f || h >= 337.5f) return "N";
        if (h < 67.5f) return "NE";
        if (h < 112.5f) return "E";
        if (h < 157.5f) return "SE";
        if (h < 202.5f) return "S";
        if (h < 247.5f) return "SW";
        if (h < 292.5f) return "W";
        return "NW";
    }

    Preferences& getPrefs() {
        return m_prefs;
    }

private:
    NodeConfig() : m_opMode(OP_MODE_BASE_STATION), m_lat(0.0), m_lon(0.0), m_accM(0.0f),
                   m_speedMps(0.0f), m_headingDeg(-1.0f), m_altM(0.0f), m_maxSpeedMps(0.0f),
                   m_tripDistM(0.0f), m_sats(0), m_isMoving(false), m_lastMoveMs(0),
                   m_gpsValid(false), m_lastGpsUpdateMs(0) {
        m_nodeName[0] = '\0';
        m_attachedDevice[0] = '\0';
    }

public:
    static void generateRedditName(const uint8_t mac[6], char* out, size_t maxLen) {
        static const char* const ADJECTIVES[] = {
            "Swift", "Brave", "Quiet", "Curious", "Silent", "Silver", "Golden", "Iron",
            "Crimson", "Frosty", "Shadow", "Astral", "Cosmic", "Vibrant", "Fierce", "Steady",
            "Mighty", "Wild", "Noble", "Clever", "Electric", "Solar", "Lunar", "Thunder",
            "Echo", "Phantom", "Stormy", "Hidden", "Ancient", "Radiant", "Apex", "Turbo",
            "Rapid", "Velvet", "Keen", "Amber", "Crystal", "Obsidian", "Hyper", "Blaze",
            "Ember", "Ghost", "Cobalt", "Rustic", "Mystic", "Daring", "Sonic", "Prism",
            "Copper", "Vivid", "Titan", "Alpha", "Stellar", "Vector", "Flash", "Arctic",
            "Neon", "Granite", "Zephyr", "Onyx", "Rogue", "Spirited", "Vortex", "Horizon"
        };

        static const char* const NOUNS[] = {
            "Falcon", "Wolf", "Hawk", "Raven", "Lynx", "Tiger", "Viper", "Badger",
            "Otter", "Fox", "Bear", "Eagle", "Cobra", "Panther", "Stag", "Puma",
            "Dragon", "Jackal", "Bison", "Raptor", "Jaguar", "Kestrel", "Cheetah", "Osprey",
            "Mustang", "Condor", "Badger", "Mantis", "Orca", "Hornet", "Sparrow", "Bison",
            "Grizzly", "Badger", "Condor", "Stallion", "Vulture", "Leopard", "Bison", "Coyote",
            "Heron", "Chameleon", "Gazelle", "Buffalo", "Lynx", "Scorpion", "Pelican", "Wolverine",
            "Beaver", "Penguin", "Armadillo", "Moose", "Badger", "Sturgeon", "Barracuda", "Shark",
            "Falcon", "Cougar", "Mastiff", "Husky", "Foxhound", "Terrier", "Collie", "Retriever"
        };

        static const char* const ROLES[] = {
            "Ranger", "Hunter", "Wanderer", "Seeker", "Watcher", "Tracker", "Scout", "Runner",
            "Drifter", "Nomad", "Guardian", "Prowler", "Rover", "Pilot", "Sentry", "Stalker",
            "Breaker", "Voyager", "Observer", "Crafter", "Strider", "Explorer", "Surveyor", "Chaser",
            "Patrol", "Navi", "Finder", "Recon", "Monitor", "Anchor", "Beacon", "Pioneer",
            "Spotter", "Sniper", "Leader", "Caster", "Cipher", "Scribe", "Herald", "Keeper",
            "Protector", "Vanguard", "Sentinel", "Operative", "Specialist", "Detective", "Guide", "Marshal",
            "Warden", "Surveyor", "Harvester", "Pathfinder", "Outrider", "Captain", "Master", "Agent",
            "Scout", "Lookout", "Navigator", "Signaler", "Relay", "Listener", "Sniffer", "Tracer"
        };

        const size_t numAdj = sizeof(ADJECTIVES) / sizeof(ADJECTIVES[0]);
        const size_t numNoun = sizeof(NOUNS) / sizeof(NOUNS[0]);
        const size_t numRole = sizeof(ROLES) / sizeof(ROLES[0]);

        // Deterministic hash from 6 MAC bytes
        uint32_t h = 2166136261u;
        for (int i = 0; i < 6; i++) {
            h ^= mac[i];
            h *= 16777619u;
        }

        uint32_t idxAdj = (h >> 16) % numAdj;
        uint32_t idxNoun = (h >> 8) % numNoun;
        uint32_t idxRole = h % numRole;
        uint32_t num = 1000 + ((h >> 4) % 9000);

        snprintf(out, maxLen, "%s%s%s%u", ADJECTIVES[idxAdj], NOUNS[idxNoun], ROLES[idxRole], num);
    }

private:
    char m_nodeName[36];
    char m_attachedDevice[48];
    NodeOpMode m_opMode;
    double m_lat;
    double m_lon;
    float m_accM;
    float m_speedMps;
    float m_headingDeg;
    float m_altM;
    float m_maxSpeedMps;
    float m_tripDistM;
    uint8_t m_sats;
    bool m_isMoving;
    unsigned long m_lastMoveMs;
    bool m_gpsValid;
    unsigned long m_lastGpsUpdateMs;
    Preferences m_prefs;
};
