package org.sworrl.beaconfix.ui

import androidx.compose.ui.graphics.Color
import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.estimate.FitMetrics
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * The position grades (docs/GRADING.md) on screen: one palette shared with the desktop (Okabe–Ito, colour-blind safe)
 * and the 95 % ellipse geometry for the map.
 */
object FixGrade {
    /** 95 % of a 2-D normal lies within 2.4477 σ along each axis of its 1-σ ellipse (√χ²₂(0.95)). */
    const val K95 = 2.4477

    fun hex(grade: String?): String = when (grade) {
        "A" -> "#009E73"
        "B" -> "#56B4E9"
        "C" -> "#F0E442"
        "D" -> "#E69F00"
        "E" -> "#D55E00"
        "F" -> "#CC79A7"
        "R" -> "#8A93A6"
        "M" -> "#0072B2"
        else -> "#9FB0C8"
    }
    fun argb(grade: String?): Int = android.graphics.Color.parseColor(hex(grade))
    /** The same colour with alpha 0–255 (for fills). */
    fun argb(grade: String?, alpha: Int): Int = (argb(grade) and 0x00FFFFFF) or (alpha.coerceIn(0, 255) shl 24)
    fun color(grade: String?): Color = Color(argb(grade))

    /** A grade worth drawing: ours or the desktop's, never on an anchored row (its survey position replaced the fit). */
    fun graded(a: ApEntity): Boolean = !a.grade.isNullOrEmpty() && a.posSource != "anchor"
    fun isRegion(a: ApEntity): Boolean = a.fitKind == "region" || a.grade == "R"
    fun isMobile(a: ApEntity): Boolean = a.fitKind == "mobile" || a.grade == "M"

    /** Outline dashed: the fix lies outside the places it was heard from, or a mirror explains the data as well. */
    fun dashed(a: ApEntity, m: FitMetrics? = FitMetrics.parse(a.fitMetrics)): Boolean = m?.inHull == false || m?.ambiguous == true

    /**
     * Bearing of the major axis, degrees clockwise from north. Taken from the covariance itself when the row has one
     * (x = east, y = north), so it does not depend on how a particular engine reported `orient`; else [ApEntity.orient].
     */
    fun bearingDeg(a: ApEntity): Double {
        val cxx = a.cxx; val cxy = a.cxy; val cyy = a.cyy
        if (cxx != null && cxy != null && cyy != null && cxx > 0 && cyy > 0) {
            val tr = cxx + cyy; val det = cxx * cyy - cxy * cxy
            val l1 = tr / 2 + sqrt(max(0.0, tr * tr / 4 - det))
            val (east, north) = if (abs(cxy) > 1e-12) (l1 - cyy) to cxy else if (cxx >= cyy) 1.0 to 0.0 else 0.0 to 1.0
            return (atan2(east, north) * 180.0 / PI + 360.0) % 180.0
        }
        return a.orient ?: 0.0
    }

    /** The 95 % ellipse as a closed ring of (lat, lon), [n] points. */
    fun ellipse95(a: ApEntity, n: Int = 72): List<Pair<Double, Double>> {
        val lat0 = a.lat ?: return emptyList(); val lon0 = a.lon ?: return emptyList()
        val major = a.semiMajor ?: return emptyList()
        val sa = major * K95
        val sb = (a.semiMinor ?: major) * K95
        val th = bearingDeg(a) * PI / 180.0
        val ue = sin(th); val un = cos(th)          // major axis (east, north)
        val we = cos(th); val wn = -sin(th)         // minor axis
        val mLat = 111320.0; val mLon = 111320.0 * cos(lat0 * PI / 180.0)
        return (0..n).map { k ->
            val t = 2 * PI * k / n
            val along = sa * cos(t); val across = sb * sin(t)
            val east = along * ue + across * we; val north = along * un + across * wn
            (lat0 + north / mLat) to (lon0 + east / mLon)
        }
    }
}
