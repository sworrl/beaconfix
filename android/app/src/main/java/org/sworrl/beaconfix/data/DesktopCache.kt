package org.sworrl.beaconfix.data

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.serialization.serializer
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.api.PoiDto
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.PoiDao
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.data.db.SnapshotDao
import org.sworrl.beaconfix.data.db.SnapshotEntity
import java.util.Locale
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The offline mirror: every desktop answer and every place this phone knows, kept in Room (tables `pois` and
 * `snapshots`) so Help, Places, the map and the trip journal work without a desktop in reach.
 *
 * Rules: a failed or empty fetch never deletes anything (the save calls are no-ops for empty input); readers get places
 * deduped by [PoiEntity.key] (newest [PoiEntity.fetchedAt] wins, a desktop row beats a phone row on a tie).
 */
@Singleton
class DesktopCache(private val poiDao: PoiDao, private val snapshotDao: SnapshotDao) {
    @Inject constructor(db: AppDatabase) : this(db.pois(), db.snapshots())

    /** Every cached place from every desktop and this phone, one row per key. */
    fun pois(): Flow<List<PoiEntity>> = poiDao.all().map { dedupe(it) }
    suspend fun poisNow(): List<PoiEntity> = dedupe(poiDao.allNow())

    /**
     * Store one desktop's `/pois` answer: per scope, that desktop's rows are replaced (other desktops, the phone and
     * the scopes not in [rows] keep theirs). [originLat]/[originLon] = where the desktop searched from (its fix).
     */
    suspend fun saveDesktopPois(desktopId: String, rows: List<PoiDto>, originLat: Double, originLon: Double, now: Long = System.currentTimeMillis()) {
        if (rows.isEmpty()) return
        val source = desktopSource(desktopId)
        // far first, so a place in both lists ends up as "near" (the desktop's own rule: near wins)
        rows.groupBy { scopeOf(it.scope) }.entries.sortedBy { if (it.key == NEAR) 1 else 0 }.forEach { (scope, part) ->
            poiDao.replace(source, scope, part.map { toEntity(source, it, originLat, originLon, now) }.distinctBy { it.key })
        }
    }

    /** Store what this phone found itself (source [PHONE]); rows of the other scope and every desktop's rows stay. */
    suspend fun savePhonePois(scope: String, rows: List<PoiEntity>) {
        if (rows.isEmpty()) return
        poiDao.replace(PHONE, scopeOf(scope), rows.distinctBy { it.key })
    }

    /** The newest snapshot of [kind] from any source. */
    fun snapshot(kind: String): Flow<SnapshotEntity?> = snapshotDao.newest(kind)
    suspend fun snapshotNow(kind: String): SnapshotEntity? = snapshotDao.newestNow(kind)
    /** The snapshot of [kind] from one [source] (`desktop:<id>` or [PHONE]). */
    suspend fun snapshotFrom(source: String, kind: String): SnapshotEntity? = snapshotDao.get(source, kind)

    suspend fun putSnapshot(source: String, kind: String, json: String, lat: Double = 0.0, lon: Double = 0.0, now: Long = System.currentTimeMillis()) =
        snapshotDao.put(SnapshotEntity(source = source, kind = kind, json = json, lat = lat, lon = lon, fetchedAt = now))

    /** A snapshot's body as [T] (the API's lenient JSON settings); null when absent or unreadable. */
    inline fun <reified T> decode(s: SnapshotEntity?): T? =
        s?.let { runCatching { ApiFactory.json.decodeFromString(serializer<T>(), it.json) }.getOrNull() }

    /** [value] as JSON for [putSnapshot]. */
    inline fun <reified T> encode(value: T): String = ApiFactory.json.encodeToString(serializer<T>(), value)

    companion object {
        const val PHONE = "phone"
        const val NEAR = "near"
        const val FAR = "far"
        /** Snapshot kinds. */
        val KINDS = listOf("emergency", "track", "trip", "devices", "location", "hello", "home", "address", "phoneplaces")

        fun desktopSource(desktopId: String) = "desktop:$desktopId"
        fun isDesktop(row: PoiEntity) = row.source.startsWith("desktop:")
        fun scopeOf(s: String) = if (s == FAR) FAR else NEAR

        private val OSM_LINK = Regex("""(?:openstreetmap\.org|osm\.org)/(node|way|relation)/(\d+)""")

        /** `https://www.openstreetmap.org/way/329264979` → `way/329264979`; null for anything else. */
        fun osmKey(link: String?): String? {
            val m = OSM_LINK.find(link ?: return null) ?: return null
            val id = m.groupValues[2].toLongOrNull()?.takeIf { it > 0 } ?: return null
            return "${m.groupValues[1]}/$id"
        }

        private fun osmTypeName(t: String): String? = when (t.lowercase(Locale.ROOT)) {
            "node", "n" -> "node"; "way", "w" -> "way"; "relation", "r" -> "relation"; else -> null
        }

        /** The fallback key for a place without an OSM object. */
        fun llKey(lat: Double, lon: Double, cat: String) = String.format(Locale.US, "ll:%.5f,%.5f:%s", lat, lon, cat)

        /** OSM type/id when sent (3.8), else parsed from the `osm` link, else position + category. */
        fun keyFor(dto: PoiDto): String {
            val t = osmTypeName(dto.osmType)
            if (t != null && dto.osmId > 0) return "$t/${dto.osmId}"
            return osmKey(dto.osm) ?: llKey(dto.lat, dto.lon, dto.cat)
        }

        fun toEntity(source: String, dto: PoiDto, oLat: Double, oLon: Double, now: Long): PoiEntity = PoiEntity(
            source = source, key = keyFor(dto), scope = scopeOf(dto.scope),
            cat = dto.cat, label = dto.label, grp = dto.group, icon = dto.icon, color = dto.color,
            name = dto.name, detail = dto.detail, address = dto.address, lat = dto.lat, lon = dto.lon,
            phone = dto.phone, hours = dto.hours, website = dto.website, wheelchair = dto.wheelchair,
            emergency = dto.emergency, wifi = dto.wifi, peds = dto.peds, er = dto.er, campus = dto.campusEr,
            driveS = dto.driveS, driveM = dto.driveM, driveEst = dto.driveEst,
            fetchedAt = now, originLat = oLat, originLon = oLon,
        )

        /** One row per key: the newest wins; on a tie a desktop row beats a phone row. Keeps first-seen key order. */
        fun dedupe(rows: List<PoiEntity>): List<PoiEntity> {
            val best = LinkedHashMap<String, PoiEntity>()
            for (r in rows) {
                val cur = best[r.key]
                if (cur == null || r.fetchedAt > cur.fetchedAt || (r.fetchedAt == cur.fetchedAt && isDesktop(r) && !isDesktop(cur))) best[r.key] = r
            }
            return best.values.toList()
        }
    }
}
