package org.sworrl.beaconfix.estimate

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import org.sworrl.beaconfix.data.api.EstimatorDto
import kotlin.math.max
import kotlin.math.sqrt

/**
 * What the desktop feeds its estimator beyond the samples (src/locator.cpp Locator::estimatorOptions / contextFor),
 * fetched from the paired desktop's `GET /api/v1/estimator` at sync and kept in Prefs. Re-referenced to this phone:
 * the desktop's offsets are "dB louder than the desktop", the phone's estimator wants "dB louder than this phone".
 */
@Serializable
data class EstimatorCalibration(
    /** the anchors' calibration κ (scales every covariance); 1 = none */
    val kappa: Double = 1.0,
    /** the desktop's per-band environment fit ("2.4" / "5" / "6") */
    val environment: Map<String, Band> = emptyMap(),
    /** the desktop's device offsets as it sent them: dB each device hears louder than the desktop */
    val desktopOffsets: Map<String, Double> = emptyMap(),
    /** the name the desktop knows this phone by ("" when it has no offset for it) */
    val phoneName: String = "",
    /** dB this phone hears louder than the desktop (0 when the desktop has not calibrated it) */
    val phoneOffsetDb: Double = 0.0,
    val fetchedAt: Long = 0,
) {
    /** One band: path-loss exponent n with its covariance S = [S00, S01, S11] (P0, n), from [samples] anchor samples. */
    @Serializable data class Band(val p0: Double = 0.0, val n: Double = 0.0, val s: List<Double> = emptyList(), val samples: Int = 0)

    /**
     * Context.deviceOffset in this phone's frame, keyed as EstimateRepository.obsOf tags the rows: the phone's own
     * samples ("") are the reference, the desktop's rows ("desktop") hear −offset(this phone) louder, any other device X
     * offset(X) − offset(this phone).
     */
    fun deviceOffsets(): Map<String, Double> {
        val out = HashMap<String, Double>()
        for ((name, off) in desktopOffsets) {
            if (name.isEmpty() || name == DESKTOP || name == phoneName || !off.isFinite()) continue
            out[name] = off - phoneOffsetDb
        }
        if (phoneOffsetDb != 0.0) out[DESKTOP] = -phoneOffsetDb
        return out
    }

    fun encode(): String = JSON.encodeToString(serializer(), this)

    companion object {
        /** How EstimateRepository.obsOf tags rows pulled from a desktop. */
        const val DESKTOP = "desktop"
        private val JSON = Json { ignoreUnknownKeys = true; explicitNulls = false }

        fun decode(json: String): EstimatorCalibration? = if (json.isBlank()) null else runCatching { JSON.decodeFromString(serializer(), json) }.getOrNull()

        /**
         * From the desktop's answer. [names]: what the desktop may call this phone, most likely first (the device name it
         * pushes observations under, then its token's name); the first one with an offset is this phone, none → 0.
         */
        fun fromDesktop(dto: EstimatorDto, names: List<String>, now: Long): EstimatorCalibration {
            val offs = dto.deviceOffsets.filterValues { it.isFinite() }
            val name = names.firstOrNull { it.isNotEmpty() && offs.containsKey(it) } ?: ""
            return EstimatorCalibration(
                kappa = dto.kappa.takeIf { it.isFinite() && it > 0 } ?: 1.0,
                environment = dto.environment.mapValues { (_, e) -> Band(e.p0, e.n, e.s, e.samples) },
                desktopOffsets = offs,
                phoneName = name,
                phoneOffsetDb = if (name.isEmpty()) 0.0 else offs.getValue(name),
                fetchedAt = now,
            )
        }

        /** std::clamp */
        private fun clamp(v: Double, lo: Double, hi: Double): Double = if (v < lo) lo else if (hi < v) hi else v

        /**
         * The desktop's Locator::estimatorOptions: band priors of P0 and n by frequency (unknown band: wide), n and its σ
         * from the environment fit when that band has ≥ 3 samples, and κ.
         */
        fun optionsFor(freqMHz: Int, cal: EstimatorCalibration?): Options {
            val o = Options()
            val band = if (freqMHz >= 5925) "6" else if (freqMHz >= 4900) "5" else "2.4"
            when {
                freqMHz <= 0 -> { o.p0Mean = -42.0; o.p0Sd = 10.0; o.defaultN = 2.5 }      // unknown band: wide enough for 2.4 and 5 GHz
                band == "2.4" -> { o.p0Mean = -40.0; o.defaultN = 2.4 }
                band == "5" -> { o.p0Mean = -47.0; o.defaultN = 2.7 }
                else -> { o.p0Mean = -48.0; o.defaultN = 2.7 }
            }
            val e = cal?.environment?.get(band)
            if (freqMHz > 0 && e != null && e.samples >= 3 && e.n.isFinite()) {
                o.defaultN = clamp(e.n, 1.6, 4.5)
                val s11 = e.s.getOrNull(2)
                if (s11 != null && s11.isFinite()) o.nSd = clamp(sqrt(max(0.0, s11)) + 0.3, 0.3, 0.6)
            }
            if (cal != null) o.kappa = cal.kappa
            return o
        }
    }
}
