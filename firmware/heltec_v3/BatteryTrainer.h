#pragma once
#include <Arduino.h>
#include <Preferences.h>

/**
 * BatteryTrainer - Adaptive State of Charge (SoC) Learning Engine for Li-ion / LiPo cells.
 *
 * Microcontroller battery voltage monitoring is inherently prone to error due to:
 * 1. Cell-to-cell variance in cutoff floor (3.1V - 3.5V) and full-charge ceiling (4.12V - 4.25V).
 * 2. Instantaneous voltage sag under radio transmission load (LoRa TX, Wi-Fi monitor hopping).
 * 3. Non-linear Li-ion discharge curve with upper/lower shoulders and a flat nominal plateau.
 *
 * This trainer continuously refines the battery percentage:
 * - Load-sag compensation using Exponential Moving Average (EMA) resting voltage tracking.
 * - Dynamic range calibration: learns true operational V_max (charge saturation) and V_min (discharge floor).
 * - Multi-point adaptive OCV curve adapting knee points to observed cell chemistry.
 * - Cumulative depth-of-discharge (DoD) tracking and cycle counting.
 * - Non-volatile persistence in ESP32 NVS (Preferences) with flash-wear rate-limiting.
 * - Instant "reset battery stats" to wipe calibration and retrain from baseline.
 */

struct BatteryTrainingStats {
    uint16_t vMin = 3350;        // Learned cutoff floor (mV)
    uint16_t vMax = 4180;        // Learned full charge saturation ceiling (mV)
    uint16_t vNom = 3720;        // Learned nominal plateau midpoint (mV)
    float cycles = 0.0f;         // Cumulative discharge cycles (1.0 = 100% total discharge)
    uint32_t runtimeSec = 0;     // Total seconds operated on battery
    uint32_t sampleCount = 0;    // Total valid battery samples evaluated
    bool isTrained = false;      // True if confidence >= 50%
    uint8_t trainPct = 0;        // 0-100% confidence/training progress
    bool learnedMin = false;     // True if real discharge cutoff observed
    bool learnedMax = false;     // True if real charge saturation observed
};

class BatteryTrainer {
public:
    BatteryTrainer() : m_filteredMv(0), m_prevPct(101), m_dischargeAccPct(0.0f),
                       m_lastSaveTime(0), m_lastSavedVmin(3350), m_lastSavedVmax(4180),
                       m_stableHighCount(0), m_stableLowCount(0) {}

    void begin() {
        load();
    }

    void load() {
        m_prefs.begin("bcf_btrain", false);
        m_stats.vMin = m_prefs.getUShort("v_min", 3350);
        m_stats.vMax = m_prefs.getUShort("v_max", 4180);
        m_stats.vNom = m_prefs.getUShort("v_nom", 3720);
        m_stats.cycles = m_prefs.getFloat("cycles", 0.0f);
        m_stats.runtimeSec = m_prefs.getUInt("runtime", 0);
        m_stats.sampleCount = m_prefs.getUInt("samples", 0);
        m_stats.isTrained = m_prefs.getBool("trained", false);
        m_stats.trainPct = m_prefs.getUChar("pct", 0);
        m_stats.learnedMin = m_prefs.getBool("lrn_min", false);
        m_stats.learnedMax = m_prefs.getBool("lrn_max", false);

        // Sanity clamp on loaded values to avoid corrupted flash
        if (m_stats.vMin < 3100 || m_stats.vMin > 3600) m_stats.vMin = 3350;
        if (m_stats.vMax < 4050 || m_stats.vMax > 4300) m_stats.vMax = 4180;
        if (m_stats.vNom < 3600 || m_stats.vNom > 3850) m_stats.vNom = 3720;
        if (m_stats.vMin >= m_stats.vNom) m_stats.vMin = 3350;
        if (m_stats.vMax <= m_stats.vNom) m_stats.vMax = 4180;

        m_lastSavedVmin = m_stats.vMin;
        m_lastSavedVmax = m_stats.vMax;
        m_lastSaveTime = millis();
    }

    void save(bool force = false) {
        unsigned long now = millis();
        // Rate-limit flash wear: write at most every 10 min unless forced or >= 15mV shift
        bool bigShift = (abs((int)m_stats.vMin - (int)m_lastSavedVmin) >= 15) ||
                        (abs((int)m_stats.vMax - (int)m_lastSavedVmax) >= 15);
        if (!force && !bigShift && (now - m_lastSaveTime < 600000UL)) {
            return;
        }

        m_lastSaveTime = now;
        m_lastSavedVmin = m_stats.vMin;
        m_lastSavedVmax = m_stats.vMax;

        m_prefs.putUShort("v_min", m_stats.vMin);
        m_prefs.putUShort("v_max", m_stats.vMax);
        m_prefs.putUShort("v_nom", m_stats.vNom);
        m_prefs.putFloat("cycles", m_stats.cycles);
        m_prefs.putUInt("runtime", m_stats.runtimeSec);
        m_prefs.putUInt("samples", m_stats.sampleCount);
        m_prefs.putBool("trained", m_stats.isTrained);
        m_prefs.putUChar("pct", m_stats.trainPct);
        m_prefs.putBool("lrn_min", m_stats.learnedMin);
        m_prefs.putBool("lrn_max", m_stats.learnedMax);
    }

    void reset() {
        m_prefs.clear();
        m_stats = BatteryTrainingStats();
        m_filteredMv = 0;
        m_prevPct = 101;
        m_dischargeAccPct = 0.0f;
        m_stableHighCount = 0;
        m_stableLowCount = 0;
        m_lastSavedVmin = m_stats.vMin;
        m_lastSavedVmax = m_stats.vMax;
        m_lastSaveTime = millis();
    }

    const BatteryTrainingStats& getStats() const { return m_stats; }
    uint32_t getFilteredMilliVolts() const { return m_filteredMv; }

    /**
     * Train on a newly measured millivolt reading.
     * Called once per second while a valid battery is connected.
     */
    uint8_t updateAndCalculatePct(uint32_t rawCalculatedMv, bool isCharging, bool onBattery) {
        m_stats.sampleCount++;

        // 1. Sag Compensation Filter: Exponential Moving Average
        if (m_filteredMv == 0) {
            m_filteredMv = rawCalculatedMv;
        } else {
            // Alpha = 0.06 (~16s time constant) to smooth out momentary radio burst sag
            m_filteredMv = (uint32_t)(0.06f * rawCalculatedMv + 0.94f * m_filteredMv);
        }

        uint32_t effectiveMv = m_filteredMv;

        // 2. Learn Full Charge Ceiling (V_max)
        if (isCharging || effectiveMv >= 4120) {
            m_stableLowCount = 0;
            if (effectiveMv >= 4140 && effectiveMv <= 4260) {
                m_stableHighCount++;
                if (m_stableHighCount >= 30) { // Stable for 30 seconds near full charge
                    if (effectiveMv > m_stats.vMax || !m_stats.learnedMax) {
                        uint16_t targetMax = (uint16_t)constrain(effectiveMv, 4120UL, 4260UL);
                        m_stats.vMax = (uint16_t)(0.2f * targetMax + 0.8f * m_stats.vMax);
                        m_stats.learnedMax = true;
                    }
                }
            } else {
                if (m_stableHighCount > 0) m_stableHighCount--;
            }
        } else {
            m_stableHighCount = 0;
        }

        // 3. Learn Low Cutoff Floor (V_min) & Discharge Runtime
        if (onBattery && !isCharging) {
            m_stats.runtimeSec++;

            if (effectiveMv >= 3150 && effectiveMv <= 3500) {
                m_stableLowCount++;
                if (m_stableLowCount >= 15) { // Sustained low voltage under operating load
                    if (effectiveMv < m_stats.vMin || !m_stats.learnedMin) {
                        uint16_t targetMin = (uint16_t)constrain(effectiveMv, 3150UL, 3500UL);
                        m_stats.vMin = (uint16_t)(0.2f * targetMin + 0.8f * m_stats.vMin);
                        m_stats.learnedMin = true;
                    }
                }
            } else {
                if (m_stableLowCount > 0) m_stableLowCount--;
            }
        }

        // 4. Calculate State of Charge % using adaptive 5-point curve
        uint8_t pct = calculatePctFromMv(effectiveMv);

        // 5. Track cumulative discharge depth and cycle count
        if (onBattery && !isCharging) {
            if (m_prevPct <= 100 && pct < m_prevPct) {
                m_dischargeAccPct += (m_prevPct - pct);
                if (m_dischargeAccPct >= 100.0f) {
                    m_stats.cycles += 1.0f;
                    m_dischargeAccPct -= 100.0f;
                }
            }
        }
        m_prevPct = pct;

        // 6. Update training confidence progress
        updateTrainingProgress();

        // 7. Check if save needed
        save();

        return pct;
    }

private:
    uint8_t calculatePctFromMv(uint32_t mv) const {
        if (mv <= m_stats.vMin) return 0;
        if (mv >= m_stats.vMax) return 100;

        // Knee points adapted to cell's learned dynamic range:
        // v0   = vMin (0%)
        // v20  = vMin + 22% of span to nominal (rapid drop-off knee)
        // v50  = vNom (50% nominal plateau)
        // v80  = vNom + 65% of span to max (upper plateau shoulder)
        // v100 = vMax (100% full saturation)
        uint16_t v20 = m_stats.vMin + (uint16_t)((m_stats.vNom - m_stats.vMin) * 0.22f);
        uint16_t v50 = m_stats.vNom;
        uint16_t v80 = m_stats.vNom + (uint16_t)((m_stats.vMax - m_stats.vNom) * 0.65f);

        if (mv < v20) {
            return (uint8_t)(((mv - m_stats.vMin) * 20UL) / (v20 - m_stats.vMin));
        } else if (mv < v50) {
            return 20 + (uint8_t)(((mv - v20) * 30UL) / (v50 - v20));
        } else if (mv < v80) {
            return 50 + (uint8_t)(((mv - v50) * 30UL) / (v80 - v50));
        } else {
            return 80 + (uint8_t)(((mv - v80) * 20UL) / (m_stats.vMax - v80));
        }
    }

    void updateTrainingProgress() {
        uint32_t score = 0;
        // Runtime contribution: up to 35 points (3.5 hours = ~12,600s)
        score += min(35UL, m_stats.runtimeSec / 360UL);

        // Cycle contribution: up to 35 points (1.75 cycles = 35 points)
        score += min(35UL, (unsigned long)(m_stats.cycles * 20.0f));

        // Observation of full saturation: 15 points
        if (m_stats.learnedMax) score += 15;

        // Observation of cutoff knee: 15 points
        if (m_stats.learnedMin) score += 15;

        m_stats.trainPct = (uint8_t)min(100UL, score);
        m_stats.isTrained = (m_stats.trainPct >= 50);
    }

    BatteryTrainingStats m_stats;
    uint32_t m_filteredMv;
    uint8_t m_prevPct;
    float m_dischargeAccPct;
    unsigned long m_lastSaveTime;
    uint16_t m_lastSavedVmin;
    uint16_t m_lastSavedVmax;
    uint16_t m_stableHighCount;
    uint16_t m_stableLowCount;
    Preferences m_prefs;
};
