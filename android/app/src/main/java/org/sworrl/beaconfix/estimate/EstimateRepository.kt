package org.sworrl.beaconfix.estimate

import org.sworrl.beaconfix.data.db.ApEntity
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.EstimateHistoryEntity
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.data.db.ObservationEntity
import javax.inject.Inject
import javax.inject.Singleton

/** The phone's own Wi-Fi position: what [EstimateRepository.locateFromScan] recorded. */
data class PhoneFix(val lat: Double, val lon: Double, val acc: Double, val used: Int, val method: String, val r95: Double = acc * 2.45, val integrity: String = "")

/** Keeps every AP's graded estimate current as observations accumulate, and locates the phone from known APs. */
@Singleton
class EstimateRepository @Inject constructor(private val db: AppDatabase, private val bus: RefitBus, private val anchors: org.sworrl.beaconfix.anchors.AnchorRepository) {
    /** When each BSSID was last refit (ms): the collector path refits an AP at most once per [MIN_REFIT_INTERVAL_MS]. */
    private val lastRefit = java.util.concurrent.ConcurrentHashMap<String, Long>()

    /**
     * Re-fit the given BSSIDs from all their observations (ours and the desktop's). [announce] = emit refit animations.
     * Without [force] (the collector, every scan) an AP refit less than [MIN_REFIT_INTERVAL_MS] ago is skipped; sync,
     * import and refitAll force. The fit itself is always the full one, so grades match the desktop for the same data.
     */
    suspend fun refit(bssids: Collection<String>, announce: Boolean = true, force: Boolean = false): Int {
        var updated = 0
        val pinned = anchors.pinned()
        val nowMs = System.currentTimeMillis()
        for (b in bssids.toSet()) {
            if (!force) { val last = lastRefit[b]; if (last != null && nowMs - last < MIN_REFIT_INTERVAL_MS) continue }
            lastRefit[b] = nowMs
            val ap = db.aps().get(b) ?: continue
            if (ap.home || ap.ignored || ap.posSource == "anchor" || b.uppercase() in pinned) continue
            val obs = db.observations().forAp(b)
            if (obs.isEmpty() && !ap.travelling) continue
            val fit = runCatching { Estimator.fitAp(obs.map { obsOf(it) }, nowMs / 1000, optionsFor(ap), contextFor(ap)) }.getOrNull() ?: continue
            if (store(ap, fit, nowMs, obs, announce)) updated++
        }
        return updated
    }
    suspend fun refitAll(announce: Boolean = true): Int = refit(db.aps().allBssids(), announce, force = true)

    /** Writes [fit] onto the AP row (and its history) under the keep-the-better-position rules; true when the row changed. */
    private suspend fun store(ap: ApEntity, fit: Fit, nowMs: Long, obs: List<ObservationEntity>, announce: Boolean): Boolean {
        val metrics = FitMetrics.of(fit).encode()
        if (fit.kind == "mobile") {
            // travels with us: graded M, the stored position (if any) is left alone
            db.aps().setGrade(ap.bssid, "mobile", "M", fit.score, fit.vantage, fit.devices, metrics, nowMs)
            return true
        }
        if (!fit.valid || (fit.kind != "fix" && fit.kind != "region")) return false
        // keep a tighter position somebody else placed (desktop / Apple / WiGLE), compared by R95
        if (ap.lat != null && ap.lon != null && ap.posSource != "observed" && ap.posSource.isNotEmpty()) {
            val theirR95 = ap.r95 ?: ap.acc?.let { it * 2.45 }
            if (theirR95 != null && theirR95 <= fit.r95) return false
        }
        val acc = if (fit.kind == "region") fit.r95 / 2.45 else fit.acc
        db.aps().setEstimate(ap.bssid, fit.lat, fit.lon, acc, "observed", fit.p0, fit.pathloss, fit.rms,
            fit.kind, fit.grade.ifEmpty { null }, fit.score, fit.r95, fit.cep50, fit.pWithin25, fit.cxx, fit.cxy, fit.cyy,
            fit.semiMajor, fit.semiMinor, fit.orientDeg, fit.vantage, fit.devices, metrics, nowMs)
        db.estimateHistory().append(EstimateHistoryEntity(bssid = ap.bssid, time = nowMs, lat = fit.lat, lon = fit.lon, cxx = fit.cxx, cxy = fit.cxy, cyy = fit.cyy,
            score = fit.score, grade = fit.grade.ifEmpty { null }))
        if (announce && bus.worth(ap.lat, ap.lon, ap.acc, fit.lat, fit.lon, acc)) {
            // up to six vantage points, spread out: the strongest sample from each ~25 m cell
            val cells = LinkedHashMap<String, ObservationEntity>()
            for (o in obs) { val k = "${(o.lat * 4000).toInt()}:${(o.lon * 4000).toInt()}"; val c = cells[k]; if (c == null || o.dbm > c.dbm) cells[k] = o }
            bus.emit(RefitEvent(ap.bssid, ap.ssid, fit.lat, fit.lon, ap.lat, ap.lon, acc, ap.acc, obs.size, fit.vantage, fit.rms,
                cells.values.sortedByDescending { it.dbm }.take(6).map { RefitEvent.Vantage(it.lat, it.lon, it.dbm) }))
        }
        return true
    }

    /** Locate the phone from a scan, using APs with known positions; records a fix on success. */
    suspend fun locateFromScan(scan: List<ScanSample>): PhoneFix? {
        val known = ArrayList<Known>()
        val pinned = anchors.pinned()          // anchored transmitters (the RV router, the desktop's antenna) enter with their survey accuracy
        for (s in scan) {
            if (s.dbm <= -95) continue
            val ap = db.aps().get(s.bssid)
            val anchor = pinned[s.bssid.uppercase()]
            if (anchor != null) {
                known.add(Known(lat = anchor.lat, lon = anchor.lon, acc = anchor.accM, dbm = s.dbm, p0 = ap?.refDbm ?: -40.0, pathloss = ap?.pathExp ?: 2.4,
                    haveModel = ap?.refDbm != null && ap.pathExp != null, bssid = s.bssid, weight = 1.0))
                continue
            }
            if (ap == null || ap.home || ap.travelling || ap.ignored || ap.lat == null || ap.lon == null || ap.grade == "M") continue
            val acc = ap.acc ?: 100.0
            if (acc < 0 || acc > 300) continue
            known.add(Known(lat = ap.lat, lon = ap.lon, acc = acc, dbm = s.dbm, p0 = ap.refDbm ?: -40.0, pathloss = ap.pathExp ?: 2.4,
                haveModel = ap.refDbm != null && ap.pathExp != null, bssid = s.bssid,
                cxx = ap.cxx ?: 0.0, cxy = ap.cxy ?: 0.0, cyy = ap.cyy ?: 0.0, weight = weightFor(ap.grade)))
        }
        if (known.isEmpty()) return null
        val fix = if (known.size == 1) {
            // one beacon: "near it", as far as its level says
            val k = known[0]
            val d = Estimator.modelDistance(if (k.haveModel) k.p0 else -40.0, if (k.haveModel) k.pathloss else 2.4, k.dbm).coerceIn(1.0, 2000.0)
            PhoneFix(k.lat, k.lon, maxOf(k.acc, d) + 20, 1, "single-ap")
        } else {
            val s = Estimator.selfLocate(known)
            if (!s.valid) return null
            PhoneFix(s.lat, s.lon, s.acc, s.used, if (known.size >= 3) "wls" else "centroid", s.r95, s.integrity)
        }
        db.fixes().insert(FixEntity(time = System.currentTimeMillis(), lat = fix.lat, lon = fix.lon, acc = fix.acc, source = "phone-wifi", provider = fix.method))
        return fix
    }

    companion object {
        const val MIN_REFIT_INTERVAL_MS = 120_000L

        /** Seconds since the epoch; the desktop's rows are "desktop" so its sessions and devices count separately. */
        fun obsOf(o: ObservationEntity): Obs = Obs(lat = o.lat, lon = o.lon, acc = o.acc, dbm = o.dbm, t = if (o.time > 0) o.time / 1000 else 0, device = if (o.remote) "desktop" else "")

        /** Band priors of P0 (dBm at 1 m) and the path-loss exponent. */
        fun optionsFor(freq: Int, band: String): Options = when {
            freq >= 5925 || (freq <= 0 && band == "6") -> Options(p0Mean = -48.0, defaultN = 2.7)
            freq >= 4900 || (freq <= 0 && band == "5") -> Options(p0Mean = -47.0, defaultN = 2.7)
            else -> Options(p0Mean = -40.0, defaultN = 2.4)
        }
        fun optionsFor(ap: ApEntity): Options = optionsFor(ap.freq, ap.band)

        /** The previous fit (drift, hysteresis), the "travels with you" flag, and a position somebody else placed. */
        fun contextFor(ap: ApEntity): Context {
            val ctx = Context(mobile = ap.travelling)
            prevFit(ap)?.let { ctx.hasPrev = true; ctx.prev = it }
            if (ap.lat != null && ap.lon != null && ap.posSource in EXTERNAL_SOURCES)
                ctx.external = External(has = true, lat = ap.lat, lon = ap.lon, acc = ap.acc ?: 50.0, source = ap.posSource)
            return ctx
        }
        private val EXTERNAL_SOURCES = setOf("placed", "wigle", "apple", "beacondb")

        /** Our previous graded fit, rebuilt from the row (only rows we graded ourselves). */
        fun prevFit(ap: ApEntity): Fit? {
            if (ap.posSource != "observed" || ap.lat == null || ap.lon == null) return null
            val kind = ap.fitKind ?: return null
            if (kind != "fix" && kind != "region") return null
            val m = FitMetrics.parse(ap.fitMetrics)
            return Fit(valid = true, kind = kind, lat = ap.lat, lon = ap.lon, acc = ap.acc ?: 0.0,
                semiMajor = ap.semiMajor ?: 0.0, semiMinor = ap.semiMinor ?: 0.0, orientDeg = ap.orient ?: 0.0,
                cxx = ap.cxx ?: 0.0, cxy = ap.cxy ?: 0.0, cyy = ap.cyy ?: 0.0, r95 = ap.r95 ?: 0.0, cep50 = ap.cep50 ?: 0.0, pWithin25 = ap.pWithin25 ?: 0.0,
                p0 = ap.refDbm ?: -40.0, pathloss = ap.pathExp ?: 2.4, rms = ap.residual ?: 0.0, vantage = ap.vantage ?: 0, devices = ap.devices ?: 0,
                score = ap.score ?: 0.0, grade = ap.grade ?: "", pendingGrade = m?.pendingGrade ?: "",
                driftD2 = m?.driftD2 ?: 0.0, nisEwma = m?.nisEwma ?: 0.0, cFfit = m?.cFfit ?: 0.0, jackMax = m?.jackMax ?: 0.0,
                inHull = m?.inHull ?: false, ambiguous = m?.ambiguous ?: false, modes = m?.modes ?: 0,
                n = m?.n ?: 0, newest = m?.newest ?: 0L, updated = m?.updated ?: 0L)
        }

        /** Self-location weight by grade. */
        fun weightFor(grade: String?): Double = when (grade) {
            "A", "B" -> 1.0
            "C" -> 0.8
            "D" -> 0.6
            "E", "R" -> 0.4
            "F" -> 0.3
            else -> 1.0
        }
    }
}

data class ScanSample(val bssid: String, val ssid: String, val dbm: Int, val freq: Int, val capabilities: String = "")
