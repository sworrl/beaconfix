package org.sworrl.beaconfix.data

import androidx.room.migration.Migration
import androidx.sqlite.db.SupportSQLiteDatabase
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.data.db.ALL_MIGRATIONS
import org.sworrl.beaconfix.data.db.DEDUPE_DESKTOP_FIXES
import org.sworrl.beaconfix.data.db.MIGRATION_1_2
import org.sworrl.beaconfix.data.db.MIGRATION_2_3
import org.sworrl.beaconfix.data.db.MIGRATION_3_4
import org.sworrl.beaconfix.data.db.MIGRATION_4_5
import org.sworrl.beaconfix.data.db.MIGRATION_5_6
import org.sworrl.beaconfix.data.db.MIGRATION_6_7
import org.sworrl.beaconfix.data.db.MIGRATION_7_8
import org.sworrl.beaconfix.data.db.MigrationSql
import java.io.File
import java.lang.reflect.Proxy

/**
 * JVM guard for the Room migrations: the SQL they run must be exactly the exported schema's (android/app/schemas),
 * and the tables 1.4 promises not to touch must be identical in v3 and v4. The instrumented MigrationTest
 * (androidTest) runs the same migrations on a real SQLite.
 */
class MigrationSqlTest {
    private val dir = File("schemas/org.sworrl.beaconfix.data.db.AppDatabase")

    /** tableName → [createSql, index createSql…] with `${TABLE_NAME}` filled in. */
    private fun schema(v: Int): Map<String, List<String>> {
        val db = Json.parseToJsonElement(File(dir, "$v.json").readText()).jsonObject["database"]!!.jsonObject
        assertEquals(v, db["version"]!!.jsonPrimitive.content.toInt())
        return db["entities"]!!.jsonArray.associate { e ->
            val o = e.jsonObject; val t = o["tableName"]!!.jsonPrimitive.content
            val sql = listOf(o["createSql"]!!.jsonPrimitive.content) + (o["indices"]?.jsonArray?.map { it.jsonObject["createSql"]!!.jsonPrimitive.content } ?: emptyList())
            t to sql.map { it.replace("\${TABLE_NAME}", t) }
        }
    }

    /** The statements [m] executes, captured by a stand-in database. */
    private fun sqlOf(m: Migration): List<String> {
        val out = ArrayList<String>()
        val db = Proxy.newProxyInstance(javaClass.classLoader, arrayOf(SupportSQLiteDatabase::class.java)) { _, method, args ->
            require(method.name == "execSQL") { "unexpected ${method.name}" }; out += args!![0] as String; null
        } as SupportSQLiteDatabase
        m.migrate(db)
        return out
    }

    @Test fun oneToTwoCreatesIdentityAndPendingLinksAsExported() {
        val s = schema(3)
        assertEquals(s["identity"]!! + s["pending_links"]!!, sqlOf(MIGRATION_1_2))
    }

    @Test fun twoToThreeCreatesAnchorsAsExported() {
        assertEquals(schema(3)["anchors"]!!, sqlOf(MIGRATION_2_3))
    }

    @Test fun threeToFourRebuildsPoisAddsSnapshotsAndDedupesFixes() {
        val s = schema(4)
        assertEquals(listOf("DROP TABLE IF EXISTS `pois`") + s["pois"]!! + s["snapshots"]!! + DEDUPE_DESKTOP_FIXES, sqlOf(MIGRATION_3_4))
    }

    @Test fun protectedTablesAreUnchangedFromThreeToFour() {
        val a = schema(3); val b = schema(4)
        for (t in listOf("aps", "observations", "fixes", "desktops", "identity", "pending_links", "anchors")) assertEquals(t, a[t], b[t])
        assertEquals(a.keys - "pois" + "pois" + "snapshots", b.keys)
    }

    @Test fun fourToFiveOnlyAddsNullableApColumnsAndTheHistoryTable() {
        val s = schema(5)
        val sql = sqlOf(MIGRATION_4_5)
        assertEquals(MigrationSql.ALTER_APS_V5 + s["estimate_history"]!!, sql)
        assertTrue(sql.none { it.contains("DROP", ignoreCase = true) || it.contains("DELETE", ignoreCase = true) || it.contains("RENAME", ignoreCase = true) })
        // every added column is in the exported v5 `aps`, nullable, and v4's `aps` is v5's minus exactly those columns
        val apsV5 = s["aps"]!!.first()
        var apsV4FromV5 = apsV5
        for ((name, type) in MigrationSql.APS_V5_COLUMNS) {
            val col = ", `$name` $type"
            assertTrue("$name in the v5 aps table", apsV5.contains("$col,"))
            apsV4FromV5 = apsV4FromV5.replace(col, "")
        }
        assertEquals(schema(4)["aps"]!!.first(), apsV4FromV5)
    }

    @Test fun protectedTablesAreUnchangedFromFourToFive() {
        val a = schema(4); val b = schema(5)
        for (t in listOf("observations", "fixes", "desktops", "identity", "pending_links", "anchors", "pois", "snapshots")) assertEquals(t, a[t], b[t])
        assertEquals(a.keys + "estimate_history", b.keys)
    }

    @Test fun fiveToSixOnlyAddsNullableObservationColumns() {
        val sql = sqlOf(MIGRATION_5_6)
        assertEquals(MigrationSql.ALTER_OBSERVATIONS_V6, sql)
        assertTrue(sql.none { it.contains("DROP", ignoreCase = true) || it.contains("DELETE", ignoreCase = true) || it.contains("RENAME", ignoreCase = true) })
        // v5's `observations` is v6's minus exactly those columns (appended last, nullable), indices unchanged
        val v6 = schema(6)["observations"]!!; val v5 = schema(5)["observations"]!!
        var fromV6 = v6.first()
        for ((name, type) in MigrationSql.OBSERVATIONS_V6_COLUMNS) {
            val col = ", `$name` $type"
            assertTrue("$name in the v6 observations table", fromV6.contains(col))
            fromV6 = fromV6.replace(col, "")
        }
        assertEquals(v5.first(), fromV6)
        assertEquals(v5.drop(1), v6.drop(1))
    }

    @Test fun protectedTablesAreUnchangedFromFiveToSix() {
        val a = schema(5); val b = schema(6)
        for (t in a.keys - "observations") assertEquals(t, a[t], b[t])
        assertEquals(a.keys, b.keys)
    }

    @Test fun sixToSevenCreatesThePlateEventTablesAsExported() {
        val s = schema(7)
        val sql = sqlOf(MIGRATION_6_7)
        assertEquals(s["plate_events"]!! + s["plate_event_media"]!!, sql)
        assertTrue(sql.none { it.contains("DROP", ignoreCase = true) || it.contains("DELETE", ignoreCase = true) || it.contains("ALTER", ignoreCase = true) })
    }

    @Test fun protectedTablesAreUnchangedFromSixToSeven() {
        val a = schema(6); val b = schema(7)
        for (t in a.keys) assertEquals(t, a[t], b[t])
        assertEquals(a.keys + "plate_events" + "plate_event_media", b.keys)
    }

    @Test fun sevenToEightOnlyAddsTheHubFlagAndFillsItFromDirty() {
        val sql = sqlOf(MIGRATION_7_8)
        assertEquals(listOf(MigrationSql.ALTER_PLATE_EVENTS_V8, MigrationSql.FILL_PLATE_EVENTS_V8), sql)
        assertTrue(sql.none { it.contains("DROP", ignoreCase = true) || it.contains("DELETE", ignoreCase = true) || it.contains("RENAME", ignoreCase = true) })
        // v7's `plate_events` is v8's minus exactly that column (appended last, NOT NULL DEFAULT 0), indices unchanged
        val v8 = schema(8)["plate_events"]!!; val v7 = schema(7)["plate_events"]!!
        val col = ", `hub_dirty` INTEGER NOT NULL DEFAULT 0"
        assertTrue(v8.first(), v8.first().endsWith("$col)"))
        assertTrue(MigrationSql.ALTER_PLATE_EVENTS_V8.endsWith(col.removePrefix(", ")))
        assertEquals(v7.first(), v8.first().replace(col, ""))
        assertEquals(v7.drop(1), v8.drop(1))
    }

    @Test fun protectedTablesAreUnchangedFromSevenToEight() {
        val a = schema(7); val b = schema(8)
        for (t in a.keys - "plate_events") assertEquals(t, a[t], b[t])
        assertEquals(a.keys, b.keys)
    }

    @Test fun migrationsChainToTheNewestSchema() {
        val versions = dir.listFiles()!!.mapNotNull { it.name.removeSuffix(".json").toIntOrNull() }.sorted()
        assertTrue(versions.toString(), versions.containsAll(listOf(3, 4, 5, 6, 7, 8)))
        assertEquals((1 until versions.last()).toList(), ALL_MIGRATIONS.map { it.startVersion })
        ALL_MIGRATIONS.forEach { assertEquals(it.startVersion + 1, it.endVersion) }
    }
}
