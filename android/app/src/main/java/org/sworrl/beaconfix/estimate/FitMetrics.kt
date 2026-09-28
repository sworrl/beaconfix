package org.sworrl.beaconfix.estimate

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

/**
 * The grading metrics of a fit that have no column of their own (ApEntity.fitMetrics, JSON). Everything is optional:
 * rows graded by the desktop carry only what its API sends. Letters for the score components as in docs/GRADING.md:
 * P precision, G geometry, E evidence, F fit, S stability, T freshness, X external agreement (−1 = none).
 */
@Serializable
data class FitMetrics(
    val source: String? = null,          // phone | desktop
    val cP: Double? = null, val cG: Double? = null, val cE: Double? = null, val cF: Double? = null, val cFfit: Double? = null,
    val cS: Double? = null, val cT: Double? = null, val cX: Double? = null,
    val pendingGrade: String? = null,
    val inHull: Boolean? = null, val ambiguous: Boolean? = null, val moved: Boolean? = null,
    val jackMax: Double? = null, val driftD2: Double? = null, val nisEwma: Double? = null, val extD2: Double? = null,
    val modes: Int? = null, val sessions: Int? = null, val n: Int? = null, val rejected: Int? = null,
    val rms: Double? = null, val rssDop: Double? = null, val crlbR95: Double? = null, val rbar: Double? = null, val maxGapDeg: Double? = null,
    val linRatio: Double? = null, val chi2nu: Double? = null, val sigmaDb: Double? = null, val outlierFrac: Double? = null, val ess: Double? = null,
    val spearman: Double? = null, val p0RangeCorr: Double? = null, val dminRatio: Double? = null, val fadingDb: Double? = null,
    val altLat: Double? = null, val altLon: Double? = null, val newest: Long? = null, val updated: Long? = null,
    val suggestLat: Double? = null, val suggestLon: Double? = null, val suggestGain: Double? = null,
) {
    /** "fragile": dropping one place moves the fix by more than twice its 1-σ semi-major axis. */
    fun fragile(semiMajor: Double?): Boolean = jackMax != null && semiMajor != null && semiMajor > 0 && jackMax / semiMajor > 2

    fun encode(): String = json.encodeToString(serializer(), this)

    companion object {
        private val json = Json { ignoreUnknownKeys = true; explicitNulls = false; encodeDefaults = false; allowSpecialFloatingPointValues = true }

        fun parse(text: String?): FitMetrics? = text?.takeIf { it.isNotBlank() }?.let { runCatching { json.decodeFromString(serializer(), it) }.getOrNull() }

        /** Everything the AP row does not already hold as a column. */
        fun of(f: Fit): FitMetrics = FitMetrics(
            source = "phone", cP = f.cP, cG = f.cG, cE = f.cE, cF = f.cF, cFfit = f.cFfit, cS = f.cS, cT = f.cT, cX = f.cX,
            pendingGrade = f.pendingGrade, inHull = f.inHull, ambiguous = f.ambiguous, moved = f.moved,
            jackMax = f.jackMax, driftD2 = f.driftD2, nisEwma = f.nisEwma, extD2 = f.extD2,
            modes = f.modes, sessions = f.sessions, n = f.n, rejected = f.rejected,
            rms = f.rms, rssDop = f.rssDop.takeIf { it.isFinite() }, crlbR95 = f.crlbR95, rbar = f.rbar, maxGapDeg = f.maxGapDeg,
            linRatio = f.linRatio, chi2nu = f.chi2nu, sigmaDb = f.sigmaDb, outlierFrac = f.outlierFrac, ess = f.ess,
            spearman = f.spearman, p0RangeCorr = f.p0RangeCorr, dminRatio = f.dminRatio, fadingDb = f.fadingDb,
            altLat = f.altLat, altLon = f.altLon, newest = f.newest, updated = f.updated,
            suggestLat = f.suggestLat, suggestLon = f.suggestLon, suggestGain = f.suggestGain,
        )
    }
}
