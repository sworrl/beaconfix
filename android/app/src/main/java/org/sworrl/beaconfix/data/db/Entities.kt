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

@Entity(tableName = "pois", primaryKeys = ["osmType", "osmId"])
data class PoiEntity(
    val osmType: String,
    val osmId: Long,
    val cat: String,
    val name: String,
    val detail: String = "",
    val lat: Double,
    val lon: Double,
    val phone: String = "",
    val hours: String = "",
    val website: String = "",
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
