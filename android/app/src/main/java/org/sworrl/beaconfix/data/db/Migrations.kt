package org.sworrl.beaconfix.data.db

import androidx.room.migration.Migration
import androidx.sqlite.db.SupportSQLiteDatabase

/**
 * Every schema step since 1.0. There is deliberately no destructive fallback: a phone carries the only copy of its
 * identity, anchors and unsynced observations, so an update must never wipe the database.
 *
 * The CREATE statements are copied verbatim from the exported schemas (android/app/schemas/…/N.json, `${TABLE_NAME}`
 * filled in); MigrationSqlTest checks that they still match, and the instrumented MigrationTest runs 1→5, 2→5, 3→5 and 4→5.
 *
 * Tables the migrations must never alter: observations, fixes (schema), desktops, identity, pending_links, anchors.
 * `aps` only ever gains nullable columns (v5), so every existing row and position survives.
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

/** In order; register all of them (AppModule, MigrationTest). */
val ALL_MIGRATIONS: Array<Migration> = arrayOf(MIGRATION_1_2, MIGRATION_2_3, MIGRATION_3_4, MIGRATION_4_5)
