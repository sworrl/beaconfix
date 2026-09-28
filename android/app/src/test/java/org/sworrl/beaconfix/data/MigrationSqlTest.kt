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

    @Test fun migrationsChainToTheNewestSchema() {
        val versions = dir.listFiles()!!.mapNotNull { it.name.removeSuffix(".json").toIntOrNull() }.sorted()
        assertTrue(versions.toString(), versions.containsAll(listOf(3, 4, 5)))
        assertEquals((1 until versions.last()).toList(), ALL_MIGRATIONS.map { it.startVersion })
        ALL_MIGRATIONS.forEach { assertEquals(it.startVersion + 1, it.endVersion) }
    }
}
