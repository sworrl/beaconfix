package org.sworrl.beaconfix.data.db

import androidx.room.ColumnInfo
import androidx.room.Entity
import androidx.room.Index
import androidx.room.PrimaryKey

/** An access point we (or a paired desktop) have heard. Mirrors the desktop `aps` table. */
@Entity(tableName = "aps")
data class ApEntity(
    @PrimaryKey val bssid: String,
    val ssid: String = "",
    val freq: Int = 0,
    val band: String = "",
    val ch: Int = 0,
    val firstSeen: Long = 0,
    val lastSeen: Long = 0,
    val timesSeen: Int = 0,
    /** best known position, if any */
    val lat: Double? = null,
    val lon: Double? = null,
    val acc: Double? = null,
    /** observed (our fit) | desktop (from the desktop's map) | placed (Apple/WiGLE via desktop) */
    val posSource: String = "",
    /** fitted reference power at 1 m and path-loss exponent (our own model) */
    val refDbm: Double? = null,
    val pathExp: Double? = null,
    val residual: Double? = null,
    val home: Boolean = false,
    val travelling: Boolean = false,
    val ignored: Boolean = false,
    val security: String = "",
    val rsnFlags: Int = 0,
    val wpaFlags: Int = 0,
    // ── v5 (1.5): the graded estimate (docs/GRADING.md); all null until the estimator (ours or the desktop's) graded it ──
    /** fix | region | mobile | none */
    val fitKind: String? = null,
    /** A–F · R (region only) · M (travels with you) */
    val grade: String? = null,
    /** 0–100 */
    val score: Double? = null,
    /** radius holding 95 % / 50 % of the position probability (m) */
    val r95: Double? = null,
    val cep50: Double? = null,
    /** P(error < 25 m) */
    val pWithin25: Double? = null,
    /** position covariance (m², x = east, y = north) */
    val cxx: Double? = null,
    val cxy: Double? = null,
    val cyy: Double? = null,
    /** 1-σ error ellipse (m) and its orientation as the estimator reports it (degrees) */
    val semiMajor: Double? = null,
    val semiMinor: Double? = null,
    val orient: Double? = null,
    /** distinct places the AP was heard from, and distinct devices that heard it */
    val vantage: Int? = null,
    val devices: Int? = null,
    /** the remaining metrics as JSON (estimate.FitMetrics) */
    val fitMetrics: String? = null,
    /** when the grade was computed (ms) */
    val gradedAt: Long? = null,
)

/** The last estimates of one AP (schema v5): at most 20 rows per BSSID (EstimateHistoryDao.append trims). */
@Entity(tableName = "estimate_history", indices = [Index("bssid")])
data class EstimateHistoryEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val bssid: String,
    /** ms */
    val time: Long,
    val lat: Double? = null,
    val lon: Double? = null,
    val cxx: Double? = null,
    val cxy: Double? = null,
    val cyy: Double? = null,
    val score: Double? = null,
    val grade: String? = null,
)

/** One RSSI sample of a BSSID taken at a known position. Mirrors the desktop `observations` table. */
@Entity(tableName = "observations", indices = [Index("bssid"), Index("synced"), Index("time")])
data class ObservationEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val bssid: String,
    val time: Long,
    val lat: Double,
    val lon: Double,
    val acc: Double,
    val dbm: Int,
    val freq: Int = 0,
    /** gps | fused | wifi | desktop | remote */
    val source: String,
    /** false until pushed to at least one desktop */
    val synced: Boolean = false,
    /** true for rows that came from a desktop (never pushed back) */
    val remote: Boolean = false,
    // ── v6 (1.6): Wi-Fi RTT (802.11mc / az) range to this AP at this time and place, null when it did not answer ──
    /** metres, offset-corrected (ranging.ApRttMath.combine) */
    val rangeM: Double? = null,
    /** 1-σ of [rangeM], metres */
    val rangeSd: Double? = null,
)

/** The newest RTT range of one AP (ObservationDao.latestRanges). */
data class ApRangeRow(val bssid: String, val rangeM: Double, val rangeSd: Double?, val time: Long)

/** One of this phone's own observation rows, as a scan (ObservationDao.ownScanRows → estimate.ScanCells). */
data class ScanRow(val id: Long, val time: Long, val lat: Double, val lon: Double, val acc: Double)

@Entity(tableName = "fixes", indices = [Index("time")])
data class FixEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val time: Long,
    val lat: Double,
    val lon: Double,
    val acc: Double,
    /** phone-gps | phone-wifi | desktop */
    val source: String,
    val provider: String = "",
    val place: String = "",
)

/**
 * A cached place (schema v4). One row per ([source], [key]): every paired desktop keeps its own copy
 * (`desktop:<DesktopEntity.id>`) next to what this phone found itself (`phone`), so places survive being offline.
 * [key] is the OSM object (`way/329264979`) or, without one, `ll:<lat5>,<lon5>:<cat>`. [scope] is `near` (the
 * everyday search) or `far` (the wide children's ER search). Read it through `data.DesktopCache`, which dedupes by key.
 */
@Entity(tableName = "pois", primaryKeys = ["source", "key"], indices = [Index("cat"), Index(value = ["source", "scope"])])
data class PoiEntity(
    val source: String,
    val key: String,
    val scope: String = "near",
    val cat: String,
    val label: String = "",
    val grp: String = "",
    val icon: String = "",
    val color: String = "",
    val name: String = "",
    val detail: String = "",
    val address: String = "",
    val lat: Double,
    val lon: Double,
    val phone: String = "",
    val hours: String = "",
    val website: String = "",
    val wheelchair: String = "",
    val emergency: Boolean = false,
    val wifi: Boolean = false,
    /** pediatric tier: 0 none, 1 pediatric ER, 2 children's hospital (ER not confirmed), 3 ER with pediatrics, 4 pediatric urgent care */
    val peds: Int = 0,
    /** "yes" | "no" | "" (unknown) */
    val er: String = "",
    /** the ER on the same campus, for a tier-2 children's hospital */
    val campus: String = "",
    val driveS: Int = 0,
    val driveM: Int = 0,
    val driveEst: Boolean = true,
    val fetchedAt: Long,
    /** where the search that found this row was made from */
    val originLat: Double = 0.0,
    val originLon: Double = 0.0,
)

/**
 * The last answer of one kind from one source, kept so every screen works offline (schema v4). [kind] is one of
 * emergency | track | trip | devices | location | hello | home | address | phoneplaces; [json] is the body as received
 * (or as the phone built it); [lat]/[lon] is where it applies, when that matters.
 */
@Entity(tableName = "snapshots", primaryKeys = ["source", "kind"])
data class SnapshotEntity(
    val source: String,
    val kind: String,
    val json: String,
    val lat: Double = 0.0,
    val lon: Double = 0.0,
    val fetchedAt: Long,
)

/** A paired desktop. The token itself lives in EncryptedSharedPreferences keyed by [id]. */
@Entity(tableName = "desktops")
data class DesktopEntity(
    @PrimaryKey val id: String,          // host:port
    val host: String,
    val port: Int,
    val name: String,
    val hostname: String = "",
    val version: String = "",
    val tls: Boolean = false,
    val scopes: String = "",
    val paired: Boolean = false,
    val lastSeen: Long = 0,
    val lastSync: Long = 0,
    val lastError: String = "",
    val pushedObs: Long = 0,
    val pulledAps: Long = 0,
    val cursor: String = "",             // incremental-sync cursor when the desktop supports it
)

/** Our identity record (JSON as the spec defines it); the Ed25519 seed lives in EncryptedSharedPreferences, never here. */
@Entity(tableName = "identity")
data class IdentityEntity(@PrimaryKey val id: String, val name: String, val created: String, val pub: String, val recordJson: String)

/**
 * A surveyed anchor (docs/RANGING.md §4): an antenna or place whose position is ground truth. [json] holds the whole
 * contract object (forward compatible); the columns are for queries. [dirty] = changed here, not yet pushed to a desktop.
 */
@Entity(tableName = "anchors")
data class AnchorEntity(
    @PrimaryKey val id: String,
    val json: String,
    val name: String,
    val kind: String,
    val lat: Double,
    val lon: Double,
    val rv: Boolean,
    val ref: Boolean,
    val deleted: Boolean,
    val placedAt: String,
    val seq: Long,
    val dirty: Boolean,
)

/** A link that is not complete yet: the other identity's offer (id, pub, name, ts) and our half-signed statement, if any. */
@Entity(tableName = "pending_links")
data class PendingLinkEntity(@PrimaryKey val id: String, val pub: String, val name: String, val ts: String, val statementJson: String = "")

/**
 * A plate event (schema v7, docs/SIGHTINGS.md §1): a `camera_pass` (our route passed close to a known camera) or a
 * `plate_search` (an agency searched our plate in Flock, from a released audit log). [uid] is the merge key between the
 * desktop, this phone and the hub (§1.1). Columns mirror the desktop's `plate_events`; [timeMs], [dirty] and
 * [notified] are the phone's own bookkeeping (when, whether it still has to be pushed, whether it was alerted).
 */
@Entity(tableName = "plate_events", indices = [Index(value = ["uid"], unique = true), Index("time_ms"), Index(value = ["camera_id", "time_ms"])])
data class PlateEventEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val uid: String,
    /** camera_pass | plate_search */
    val kind: String,
    /** display plate ("ABC-1234"); "" when unknown */
    val plate: String? = null,
    /** local ISO (the desktop's MapDb::localIso convention): the closest approach / the search time */
    val time: String,
    @ColumnInfo(name = "time_ms") val timeMs: Long,
    val lat: Double? = null,
    val lon: Double? = null,
    val acc: Double? = null,
    @ColumnInfo(name = "camera_id") val cameraId: String? = null,
    @ColumnInfo(name = "camera_lat") val cameraLat: Double? = null,
    @ColumnInfo(name = "camera_lon") val cameraLon: Double? = null,
    @ColumnInfo(name = "distance_m") val distanceM: Double? = null,
    @ColumnInfo(name = "speed_kmh") val speedKmh: Double? = null,
    @ColumnInfo(name = "heading_deg") val headingDeg: Double? = null,
    /** bearing camera → us at the closest approach */
    @ColumnInfo(name = "approach_bearing_deg") val approachBearingDeg: Double? = null,
    /** the camera's facing (OSM direction / camera:direction), null unknown */
    @ColumnInfo(name = "camera_dir_deg") val cameraDirDeg: Double? = null,
    /** 1 the camera saw our front or rear plate, 0 it did not, null unknown */
    val facing: Int? = null,
    val operator: String? = null,
    val agency: String? = null,
    val model: String? = null,
    /** alpr | camera: only an ALPR reads plates */
    @ColumnInfo(name = "camera_type") val cameraType: String? = null,
    /** live_route | route_backfill | phone_live | dashcam | haveibeenflocked */
    val source: String,
    @ColumnInfo(name = "source_url") val sourceUrl: String? = null,
    @ColumnInfo(name = "source_name") val sourceName: String? = null,
    /** 0–100 */
    val confidence: Int? = null,
    @ColumnInfo(defaultValue = "0") val leaky: Int = 0,
    /** one honest sentence */
    val details: String? = null,
    /** JSON object: every other number */
    val metrics: String? = null,
    /** JSON: the source record as received */
    val raw: String? = null,
    /** who recorded it: "" the desktop itself, else the device name */
    val device: String? = null,
    @ColumnInfo(name = "created_at") val createdAt: String? = null,
    @ColumnInfo(name = "updated_at") val updatedAt: String? = null,
    /** the desktop's sync-feed sequence (0 = never seen on a desktop) */
    @ColumnInfo(defaultValue = "0") val seq: Long = 0,
    /** recorded or changed here and not yet accepted by a LAN desktop (the only place its images can go) */
    @ColumnInfo(defaultValue = "0") val dirty: Boolean = false,
    /** an alert (or the backfill summary) already covered it */
    @ColumnInfo(defaultValue = "0") val notified: Boolean = false,
    /** recorded or changed here and not yet accepted by the hub (`/api/v3` `db/sync`, records only; schema v8) */
    @ColumnInfo(name = "hub_dirty", defaultValue = "0") val hubDirty: Boolean = false,
)

/**
 * An image of a plate event (schema v7, docs/SIGHTINGS.md §1, §3). The desktop keeps the bytes in a BLOB; the phone keeps
 * its own dash-cam frames as files under noBackupFilesDir ([path]) until the desktop has them, and knows the desktop's
 * images by uid only ([path] null, [remote] true), fetching them for display on demand.
 */
@Entity(tableName = "plate_event_media", indices = [Index(value = ["uid"], unique = true), Index("event_uid"), Index("camera_id")])
data class PlateEventMediaEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    /** first 32 hex of SHA-256 of the stored bytes */
    val uid: String,
    @ColumnInfo(name = "event_uid") val eventUid: String? = null,
    @ColumnInfo(name = "camera_id") val cameraId: String? = null,
    /** dashcam | camera_photo | webcam */
    val kind: String,
    /** image/webp | image/png | image/jxl | image/jpeg */
    val mime: String,
    /** the local file (this phone's own frame), null when the image lives on the desktop only */
    val path: String? = null,
    val width: Int? = null,
    val height: Int? = null,
    val bytes: Long? = null,
    @ColumnInfo(name = "original_url") val originalUrl: String? = null,
    @ColumnInfo(name = "original_mime") val originalMime: String? = null,
    @ColumnInfo(name = "original_bytes") val originalBytes: Long? = null,
    @ColumnInfo(name = "original_sha256") val originalSha256: String? = null,
    @ColumnInfo(name = "jpeg_reconstructible", defaultValue = "0") val jpegReconstructible: Boolean = false,
    val attribution: String? = null,
    val license: String? = null,
    /** what [captured_at] says: when the frame was taken (local ISO) */
    @ColumnInfo(name = "captured_at") val capturedAt: String? = null,
    @ColumnInfo(name = "created_at") val createdAt: String? = null,
    /** the desktop holds it (uploaded from here, or listed in its feed) */
    @ColumnInfo(defaultValue = "0") val remote: Boolean = false,
)

/**
 * Something an ESP32 node picked up and handed to this phone over BLE (schema v9): a probe request, a beacon, a deauth
 * `alert` or a `ble_tracker`. Lines the node kept on flash while the phone was away arrive later with [stored] set;
 * [uid] (`node:sf:<seq>` for those, `node:kind:mac:time` for live ones) keeps a resend from doubling up. [lat]/[lon]
 * come from the node when it had a fix; otherwise a live line gets where this phone was at [timeMs], and a stored one
 * where the node last had a fix, since it was out of the phone's reach ([locSource] says which).
 */
@Entity(tableName = "node_detections", indices = [Index(value = ["uid"], unique = true), Index("time_ms"), Index("hub_dirty")])
data class NodeDetectionEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val uid: String,
    val node: String,
    /** probe | beacon | alert | ble_tracker */
    val kind: String,
    /** client MAC (probe), BSSID (beacon), attacker (alert), tag (ble_tracker) */
    val mac: String,
    val ssid: String = "",
    val rssi: Int = 0,
    val ch: Int = 0,
    /** the deauth target, or the tracker kind (airtag, smarttag, tile) */
    val detail: String = "",
    @ColumnInfo(name = "time_ms") val timeMs: Long,
    val lat: Double? = null,
    val lon: Double? = null,
    val acc: Double? = null,
    /** node | phone (live, so near the phone) | node_last (stored while apart: where the node last had a fix) | "" */
    @ColumnInfo(name = "loc_source") val locSource: String = "",
    val stored: Boolean = false,
    @ColumnInfo(name = "hub_dirty") val hubDirty: Boolean = true,
)

