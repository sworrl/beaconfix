package org.sworrl.beaconfix.estimate

import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.FixEntity
import javax.inject.Inject
import javax.inject.Singleton

/** Keeps every AP's position current as observations accumulate, and locates the phone from known APs. */
@Singleton
class EstimateRepository @Inject constructor(private val db: AppDatabase) {
    /** Re-fit the given BSSIDs from all their observations (ours and the desktop's). */
    suspend fun refit(bssids: Collection<String>): Int {
        var updated = 0
        for (b in bssids.toSet()) {
            val ap = db.aps().get(b) ?: continue
            if (ap.home || ap.travelling || ap.ignored) continue
            val obs = db.observations().forAp(b)
            val fit = Estimator.fitAp(obs.map { Sample(it.lat, it.lon, it.acc, it.dbm) }) ?: continue
            // keep a better externally placed position (desktop/Apple/WiGLE) unless our fit is clearly tighter
            if (ap.posSource == "placed" && ap.acc != null && ap.acc <= fit.acc) continue
            db.aps().setPosition(b, fit.lat, fit.lon, fit.acc, "observed", fit.refDbm, fit.pathExp, fit.residual)
            updated++
        }
        return updated
    }
    suspend fun refitAll(): Int = refit(db.aps().allBssids())

    /** Locate the phone from a scan, using APs with known positions; records a fix on success. */
    suspend fun locateFromScan(scan: List<ScanSample>): PhoneFix? {
        val known = ArrayList<KnownAp>()
        for (s in scan) {
            val ap = db.aps().get(s.bssid) ?: continue
            if (ap.home || ap.travelling || ap.ignored || ap.lat == null || ap.lon == null) continue
            known.add(KnownAp(ap.lat, ap.lon, ap.acc ?: 100.0, s.dbm, ap.refDbm, ap.pathExp))
        }
        val fix = Estimator.locatePhone(known) ?: return null
        db.fixes().insert(FixEntity(time = System.currentTimeMillis(), lat = fix.lat, lon = fix.lon, acc = fix.acc, source = "phone-wifi", provider = fix.method))
        return fix
    }
}

data class ScanSample(val bssid: String, val ssid: String, val dbm: Int, val freq: Int, val capabilities: String = "")
