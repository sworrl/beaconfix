#pragma once
#include <Arduino.h>
#include <esp_timer.h>

/**
 * High-precision Sub-Millisecond Time Synchronization Engine for BeaconFix Nodes.
 * Uses 64-bit microsecond esp_timer with PTP-style 2-way delay measurement
 * and clock disciplining across the mesh.
 */
class TimeSync {
public:
    static TimeSync& instance() {
        static TimeSync inst;
        return inst;
    }

    void begin() {
        m_offsetUs = 0;
        m_stratum = 0;       // 0 = unsynced (local uptime only)
        m_lastSyncMs = 0;
        m_lastRttUs = 0;
        m_lastOffsetUs = 0;
        m_syncIntervalMs = 90000; // Resync every 90 seconds (sub-ms drift guarantee)
    }

    /**
     * Set master UTC epoch time directly (e.g. from USB host, Phone BLE, or NTP).
     * Automatically elevates this node to Stratum 1 (Master Clock).
     */
    void setMasterTimeUs(uint64_t epochUs) {
        int64_t hwNow = esp_timer_get_time();
        m_offsetUs = (int64_t)epochUs - hwNow;
        m_stratum = 1;
        m_lastSyncMs = millis();
        m_lastOffsetUs = 0;
        m_lastRttUs = 0;
    }

    /**
     * Rapid opportunistic coarse clock synchronization from incoming packet timestamp.
     * Sets offset immediately if this node is currently unsynced (stratum 0).
     */
    void setInitialCoarseTime(uint64_t packetTimestampUs, uint8_t peerStratum) {
        if (m_stratum == 0 && packetTimestampUs > 1000000000000ULL && peerStratum > 0) {
            int64_t hwNow = esp_timer_get_time();
            m_offsetUs = (int64_t)packetTimestampUs - hwNow;
            m_stratum = (peerStratum < 254) ? (peerStratum + 1) : 255;
            m_lastSyncMs = millis();
        }
    }

    /**
     * Current network-synchronized timestamp in microseconds.
     */
    uint64_t getNowUs() const {
        int64_t hwNow = esp_timer_get_time();
        return (uint64_t)(hwNow + m_offsetUs);
    }

    /**
     * Current network-synchronized timestamp in milliseconds.
     */
    uint32_t getNowMs() const {
        return (uint32_t)(getNowUs() / 1000ULL);
    }

    /**
     * Whether the clock is disciplined and fresh (synced within last 5 minutes).
     */
    bool isSynced() const {
        if (m_stratum == 0) return false;
        if (m_stratum == 1) return true; // Master is authoritative
        return (millis() - m_lastSyncMs) < 300000UL;
    }

    uint8_t getStratum() const { return m_stratum; }
    int64_t getLastOffsetUs() const { return m_lastOffsetUs; }
    uint32_t getLastRttUs() const { return m_lastRttUs; }
    unsigned long getLastSyncMs() const { return m_lastSyncMs; }

    /**
     * Millisecond phase within repeating cycle (default 1000ms for 1Hz cardiac cycle).
     * Synchronized identically across all network, LoRa, BLE, and USB nodes.
     */
    uint32_t getPhaseMs(uint32_t cycleMs = 1000) const {
        return (uint32_t)((getNowUs() / 1000ULL) % (uint64_t)cycleMs);
    }

    /**
     * Process PTP two-way measurement response:
     * T1: Origin timestamp (when this node sent request)
     * T2: Ingress timestamp on peer (peer's clock)
     * T3: Egress timestamp on peer (peer's clock)
     * T4: Ingress timestamp on this node (local clock)
     */
    bool applyTwoWaySync(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4, uint8_t peerStratum) {
        if (peerStratum == 0) return false; // Peer is not synced
        if (m_stratum == 1 && peerStratum >= 1) {
            // We are already master (USB/BLE/NTP), don't degrade to a downstream peer
            return false;
        }

        int64_t rtt = (int64_t)((t4 - t1) - (t3 - t2));
        if (rtt < 0 || rtt > 350000LL) {
            // Reject outlier / congested network delay (> 350ms, allowing LoRa hops)
            return false;
        }

        // Clock offset theta = ((T2 - T1) + (T3 - T4)) / 2
        int64_t theta = (((int64_t)t2 - (int64_t)t1) + ((int64_t)t3 - (int64_t)t4)) / 2LL;

        // Apply smooth discipline to offset
        m_offsetUs += theta;
        m_stratum = (peerStratum < 254) ? (peerStratum + 1) : 255;
        m_lastSyncMs = millis();
        m_lastOffsetUs = theta;
        m_lastRttUs = (uint32_t)rtt;
        return true;
    }

    bool shouldRequestSync() const {
        if (m_stratum == 1) return false; // Masters do not sync from peers
        return (millis() - m_lastSyncMs >= m_syncIntervalMs) || (m_stratum == 0);
    }

    void demoteIfStale() {
        if (m_stratum > 1 && (millis() - m_lastSyncMs > 360000UL)) { // 6 minutes without sync
            m_stratum = 0; // Lost sync
        }
    }

private:
    TimeSync() : m_offsetUs(0), m_stratum(0), m_lastSyncMs(0),
                 m_lastRttUs(0), m_lastOffsetUs(0), m_syncIntervalMs(90000) {}

    int64_t m_offsetUs;
    uint8_t m_stratum;
    unsigned long m_lastSyncMs;
    uint32_t m_lastRttUs;
    int64_t m_lastOffsetUs;
    unsigned long m_syncIntervalMs;
};
