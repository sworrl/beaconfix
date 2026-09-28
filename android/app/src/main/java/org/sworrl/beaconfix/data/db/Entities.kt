package org.sworrl.beaconfix.data.db

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
)

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
