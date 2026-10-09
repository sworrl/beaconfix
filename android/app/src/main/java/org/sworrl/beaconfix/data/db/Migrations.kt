package org.sworrl.beaconfix.data.db

import androidx.room.migration.Migration
import androidx.sqlite.db.SupportSQLiteDatabase

/**
 * Every schema step since 1.0. There is deliberately no destructive fallback: a phone carries the only copy of its
 * identity, anchors and unsynced observations, so an update must never wipe the database.
 *
 * The CREATE statements are copied verbatim from the exported schemas (android/app/schemas/…/N.json, `${TABLE_NAME}`
 * filled in); MigrationSqlTest checks that they still match, and the instrumented MigrationTest runs 1→8 … 7→8.
 *
 * Tables the migrations must never alter: fixes (schema), desktops, identity, pending_links, anchors.
 * `aps` (v5) and `observations` (v6) only ever gain nullable columns, so every existing row and position survives.
 */
object MigrationSql {
    // v2 (1.2): cross-app identity
    const val CREATE_IDENTITY = "CREATE TABLE IF NOT EXISTS `identity` (`id` TEXT NOT NULL, `name` TEXT NOT NULL, `created` TEXT NOT NULL, `pub` TEXT NOT NULL, `recordJson` TEXT NOT NULL, PRIMARY KEY(`id`))"
    const val CREATE_PENDING_LINKS = "CREATE TABLE IF NOT EXISTS `pending_links` (`id` TEXT NOT NULL, `pub` TEXT NOT NULL, `name` TEXT NOT NULL, `ts` TEXT NOT NULL, `statementJson` TEXT NOT NULL, PRIMARY KEY(`id`))"

    // v3 (1.3): anchors (docs/RANGING.md §4)
    const val CREATE_ANCHORS = "CREATE TABLE IF NOT EXISTS `anchors` (`id` TEXT NOT NULL, `json` TEXT NOT NULL, `name` TEXT NOT NULL, `kind` TEXT NOT NULL, `lat` REAL NOT NULL, `lon` REAL NOT NULL, `rv` INTEGER NOT NULL, `ref` INTEGER NOT NULL, `deleted` INTEGER NOT NULL, `placedAt` TEXT NOT NULL, `seq` INTEGER NOT NULL, `dirty` INTEGER NOT NULL, PRIMARY KEY(`id`))"

    // v4 (1.4): the places cache is rebuilt per source + the snapshots table
    const val DROP_POIS = "DROP TABLE IF EXISTS `pois`"
    const val CREATE_POIS = "CREATE TABLE IF NOT EXISTS `pois` (`source` TEXT NOT NULL, `key` TEXT NOT NULL, `scope` TEXT NOT NULL, `cat` TEXT NOT NULL, `label` TEXT NOT NULL, `grp` TEXT NOT NULL, `icon` TEXT NOT NULL, `color` TEXT NOT NULL, `name` TEXT NOT NULL, `detail` TEXT NOT NULL, `address` TEXT NOT NULL, `lat` REAL NOT NULL, `lon` REAL NOT NULL, `phone` TEXT NOT NULL, `hours` TEXT NOT NULL, `website` TEXT NOT NULL, `wheelchair` TEXT NOT NULL, `emergency` INTEGER NOT NULL, `wifi` INTEGER NOT NULL, `peds` INTEGER NOT NULL, `er` TEXT NOT NULL, `campus` TEXT NOT NULL, `driveS` INTEGER NOT NULL, `driveM` INTEGER NOT NULL, `driveEst` INTEGER NOT NULL, `fetchedAt` INTEGER NOT NULL, `originLat` REAL NOT NULL, `originLon` REAL NOT NULL, PRIMARY KEY(`source`, `key`))"
    const val CREATE_POIS_CAT = "CREATE INDEX IF NOT EXISTS `index_pois_cat` ON `pois` (`cat`)"
    const val CREATE_POIS_SOURCE_SCOPE = "CREATE INDEX IF NOT EXISTS `index_pois_source_scope` ON `pois` (`source`, `scope`)"
    const val CREATE_SNAPSHOTS = "CREATE TABLE IF NOT EXISTS `snapshots` (`source` TEXT NOT NULL, `kind` TEXT NOT NULL, `json` TEXT NOT NULL, `lat` REAL NOT NULL, `lon` REAL NOT NULL, `fetchedAt` INTEGER NOT NULL, PRIMARY KEY(`source`, `kind`))"

    // v5 (1.5): the graded estimate on each AP (nullable columns, additive) + the estimate history
    val APS_V5_COLUMNS: List<Pair<String, String>> = listOf(
        "fitKind" to "TEXT", "grade" to "TEXT", "score" to "REAL", "r95" to "REAL", "cep50" to "REAL", "pWithin25" to "REAL",
        "cxx" to "REAL", "cxy" to "REAL", "cyy" to "REAL", "semiMajor" to "REAL", "semiMinor" to "REAL", "orient" to "REAL",
        "vantage" to "INTEGER", "devices" to "INTEGER", "fitMetrics" to "TEXT", "gradedAt" to "INTEGER",
    )
    val ALTER_APS_V5: List<String> = APS_V5_COLUMNS.map { (name, type) -> "ALTER TABLE `aps` ADD COLUMN `$name` $type" }
    const val CREATE_ESTIMATE_HISTORY = "CREATE TABLE IF NOT EXISTS `estimate_history` (`id` INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, `bssid` TEXT NOT NULL, `time` INTEGER NOT NULL, `lat` REAL, `lon` REAL, `cxx` REAL, `cxy` REAL, `cyy` REAL, `score` REAL, `grade` TEXT)"
    const val CREATE_ESTIMATE_HISTORY_BSSID = "CREATE INDEX IF NOT EXISTS `index_estimate_history_bssid` ON `estimate_history` (`bssid`)"

    // v6 (1.6): the Wi-Fi RTT range on each observation (nullable columns, additive)
    val OBSERVATIONS_V6_COLUMNS: List<Pair<String, String>> = listOf("rangeM" to "REAL", "rangeSd" to "REAL")
    val ALTER_OBSERVATIONS_V6: List<String> = OBSERVATIONS_V6_COLUMNS.map { (name, type) -> "ALTER TABLE `observations` ADD COLUMN `$name` $type" }

    // v7 (1.6): plate events and their images (docs/SIGHTINGS.md §1); new tables only
    const val CREATE_PLATE_EVENTS = "CREATE TABLE IF NOT EXISTS `plate_events` (`id` INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, `uid` TEXT NOT NULL, `kind` TEXT NOT NULL, `plate` TEXT, `time` TEXT NOT NULL, `time_ms` INTEGER NOT NULL, `lat` REAL, `lon` REAL, `acc` REAL, `camera_id` TEXT, `camera_lat` REAL, `camera_lon` REAL, `distance_m` REAL, `speed_kmh` REAL, `heading_deg` REAL, `approach_bearing_deg` REAL, `camera_dir_deg` REAL, `facing` INTEGER, `operator` TEXT, `agency` TEXT, `model` TEXT, `camera_type` TEXT, `source` TEXT NOT NULL, `source_url` TEXT, `source_name` TEXT, `confidence` INTEGER, `leaky` INTEGER NOT NULL DEFAULT 0, `details` TEXT, `metrics` TEXT, `raw` TEXT, `device` TEXT, `created_at` TEXT, `updated_at` TEXT, `seq` INTEGER NOT NULL DEFAULT 0, `dirty` INTEGER NOT NULL DEFAULT 0, `notified` INTEGER NOT NULL DEFAULT 0)"
    const val CREATE_PLATE_EVENTS_UID = "CREATE UNIQUE INDEX IF NOT EXISTS `index_plate_events_uid` ON `plate_events` (`uid`)"
    const val CREATE_PLATE_EVENTS_TIME = "CREATE INDEX IF NOT EXISTS `index_plate_events_time_ms` ON `plate_events` (`time_ms`)"
    const val CREATE_PLATE_EVENTS_CAMERA = "CREATE INDEX IF NOT EXISTS `index_plate_events_camera_id_time_ms` ON `plate_events` (`camera_id`, `time_ms`)"
    const val CREATE_PLATE_EVENT_MEDIA = "CREATE TABLE IF NOT EXISTS `plate_event_media` (`id` INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, `uid` TEXT NOT NULL, `event_uid` TEXT, `camera_id` TEXT, `kind` TEXT NOT NULL, `mime` TEXT NOT NULL, `path` TEXT, `width` INTEGER, `height` INTEGER, `bytes` INTEGER, `original_url` TEXT, `original_mime` TEXT, `original_bytes` INTEGER, `original_sha256` TEXT, `jpeg_reconstructible` INTEGER NOT NULL DEFAULT 0, `attribution` TEXT, `license` TEXT, `captured_at` TEXT, `created_at` TEXT, `remote` INTEGER NOT NULL DEFAULT 0)"
    const val CREATE_PLATE_EVENT_MEDIA_UID = "CREATE UNIQUE INDEX IF NOT EXISTS `index_plate_event_media_uid` ON `plate_event_media` (`uid`)"
    const val CREATE_PLATE_EVENT_MEDIA_EVENT = "CREATE INDEX IF NOT EXISTS `index_plate_event_media_event_uid` ON `plate_event_media` (`event_uid`)"
    const val CREATE_PLATE_EVENT_MEDIA_CAMERA = "CREATE INDEX IF NOT EXISTS `index_plate_event_media_camera_id` ON `plate_event_media` (`camera_id`)"
    // v9: what ESP32 nodes hand the phone over BLE
    val V9: List<String> = listOf(
        "CREATE TABLE IF NOT EXISTS `node_detections` (`id` INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, `uid` TEXT NOT NULL, `node` TEXT NOT NULL, `kind` TEXT NOT NULL, `mac` TEXT NOT NULL, `ssid` TEXT NOT NULL, `rssi` INTEGER NOT NULL, `ch` INTEGER NOT NULL, `detail` TEXT NOT NULL, `time_ms` INTEGER NOT NULL, `lat` REAL, `lon` REAL, `acc` REAL, `loc_source` TEXT NOT NULL, `stored` INTEGER NOT NULL, `hub_dirty` INTEGER NOT NULL)",
        "CREATE UNIQUE INDEX IF NOT EXISTS `index_node_detections_uid` ON `node_detections` (`uid`)",
        "CREATE INDEX IF NOT EXISTS `index_node_detections_time_ms` ON `node_detections` (`time_ms`)",
        "CREATE INDEX IF NOT EXISTS `index_node_detections_hub_dirty` ON `node_detections` (`hub_dirty`)",
    )

    val V7: List<String> = listOf(CREATE_PLATE_EVENTS, CREATE_PLATE_EVENTS_UID, CREATE_PLATE_EVENTS_TIME, CREATE_PLATE_EVENTS_CAMERA,
        CREATE_PLATE_EVENT_MEDIA, CREATE_PLATE_EVENT_MEDIA_UID, CREATE_PLATE_EVENT_MEDIA_EVENT, CREATE_PLATE_EVENT_MEDIA_CAMERA)

    // v8 (1.6): plate events are pushed per destination — `dirty` (a LAN desktop: records + images) and `hub_dirty`
    // (the hub: records only). What was waiting for a desktop is waiting for the hub too.
    const val ALTER_PLATE_EVENTS_V8 = "ALTER TABLE `plate_events` ADD COLUMN `hub_dirty` INTEGER NOT NULL DEFAULT 0"
    const val FILL_PLATE_EVENTS_V8 = "UPDATE `plate_events` SET `hub_dirty` = `dirty`"
}

/** 1.0/1.1 → 1.2: the identity and pending-link tables. */
val MIGRATION_1_2 = object : Migration(1, 2) {
    override fun migrate(db: SupportSQLiteDatabase) {
        db.execSQL(MigrationSql.CREATE_IDENTITY)
        db.execSQL(MigrationSql.CREATE_PENDING_LINKS)
    }
}

/** 1.2 → 1.3: the anchors table (docs/RANGING.md §4). Everything else is untouched, so no data is lost on update. */
val MIGRATION_2_3 = object : Migration(2, 3) {
    override fun migrate(db: SupportSQLiteDatabase) {
        db.execSQL(MigrationSql.CREATE_ANCHORS)
    }
}

/**
 * 1.3 → 1.4: `pois` becomes a per-source cache (it only ever held re-downloadable desktop places, so it is dropped and
 * recreated), `snapshots` is new, and desktop fixes that earlier syncs inserted more than once are collapsed (data only).
 */
val MIGRATION_3_4 = object : Migration(3, 4) {
    override fun migrate(db: SupportSQLiteDatabase) {
        db.execSQL(MigrationSql.DROP_POIS)
        db.execSQL(MigrationSql.CREATE_POIS)
        db.execSQL(MigrationSql.CREATE_POIS_CAT)
        db.execSQL(MigrationSql.CREATE_POIS_SOURCE_SCOPE)
        db.execSQL(MigrationSql.CREATE_SNAPSHOTS)
        db.execSQL(DEDUPE_DESKTOP_FIXES)
    }
}

/**
 * 1.4 → 1.5: additive only. `aps` gains the graded-estimate columns (all nullable: existing positions stay as they are)
 * and `estimate_history` is new. The app recomputes every estimate once afterwards (Prefs.estimatorVersion).
 */
val MIGRATION_4_5 = object : Migration(4, 5) {
    override fun migrate(db: SupportSQLiteDatabase) {
        for (sql in MigrationSql.ALTER_APS_V5) db.execSQL(sql)
        db.execSQL(MigrationSql.CREATE_ESTIMATE_HISTORY)
        db.execSQL(MigrationSql.CREATE_ESTIMATE_HISTORY_BSSID)
    }
}

/** 1.5 → 1.6: additive only. `observations` gains the Wi-Fi RTT range and its 1-σ (null on every existing row). */
val MIGRATION_5_6 = object : Migration(5, 6) {
    override fun migrate(db: SupportSQLiteDatabase) {
        for (sql in MigrationSql.ALTER_OBSERVATIONS_V6) db.execSQL(sql)
    }
}

/** 1.6 → 1.6 + sightings: additive only. The plate-event tables (docs/SIGHTINGS.md §1) are new; nothing else changes. */
val MIGRATION_6_7 = object : Migration(6, 7) {
    override fun migrate(db: SupportSQLiteDatabase) {
        for (sql in MigrationSql.V7) db.execSQL(sql)
    }
}

/** Sightings over the hub: additive only. `plate_events` gains `hub_dirty` (the hub's "waiting to send"), filled from `dirty`. */
val MIGRATION_7_8 = object : Migration(7, 8) {
    override fun migrate(db: SupportSQLiteDatabase) {
        db.execSQL(MigrationSql.ALTER_PLATE_EVENTS_V8)
        db.execSQL(MigrationSql.FILL_PLATE_EVENTS_V8)
    }
}

/** ESP32 nodes: additive only. `node_detections` is new (what a node hands the phone over BLE, kept and sent to the hub). */
val MIGRATION_8_9 = object : Migration(8, 9) {
    override fun migrate(db: SupportSQLiteDatabase) {
        for (sql in MigrationSql.V9) db.execSQL(sql)
    }
}

/** In order; register all of them (AppModule, MigrationTest). */
val ALL_MIGRATIONS: Array<Migration> = arrayOf(MIGRATION_1_2, MIGRATION_2_3, MIGRATION_3_4, MIGRATION_4_5, MIGRATION_5_6, MIGRATION_6_7, MIGRATION_7_8, MIGRATION_8_9)
