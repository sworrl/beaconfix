package org.sworrl.beaconfix.data

import android.database.sqlite.SQLiteDatabase
import androidx.room.Room
import androidx.room.testing.MigrationTestHelper
import androidx.sqlite.db.SupportSQLiteDatabase
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.sworrl.beaconfix.data.db.ALL_MIGRATIONS
import org.sworrl.beaconfix.data.db.AppDatabase

/**
 * Every database a released build can have (v1: 1.0/1.1, v2: 1.2, v3: 1.3.x, v4: 1.4.x) must reach v5 with its data intact.
 * Run on a device against the debug package (the release install is untouched):
 *   ./gradlew :app:connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=org.sworrl.beaconfix.data.MigrationTest
 */
@RunWith(AndroidJUnit4::class)
class MigrationTest {
    private val name = "migration-test.db"
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val ctx get() = instrumentation.targetContext

    @get:Rule
    val helper = MigrationTestHelper(instrumentation, AppDatabase::class.java)

    @Before fun clean() { ctx.deleteDatabase(name) }
    @After fun cleanUp() { ctx.deleteDatabase(name) }

    @Test fun fourToFive() {
        helper.createDatabase(name, 4).use { seed(it, 4) }
        helper.runMigrationsAndValidate(name, 5, true, *ALL_MIGRATIONS).use { verify(it, 4) }
    }

    @Test fun threeToFive() {
        helper.createDatabase(name, 3).use { seed(it, 3) }
        helper.runMigrationsAndValidate(name, 5, true, *ALL_MIGRATIONS).use { verify(it, 3) }
    }

    @Test fun twoToFive() {
        createRaw(2)
        helper.runMigrationsAndValidate(name, 5, true, *ALL_MIGRATIONS).use { verify(it, 2) }
    }

    @Test fun oneToFive() {
        createRaw(1)
        helper.runMigrationsAndValidate(name, 5, true, *ALL_MIGRATIONS).use { verify(it, 1) }
    }

    /** The app's own builder (AppModule: migrations, no destructive fallback) opens a migrated v3 file and its DAOs work. */
    @Test fun roomOpensAMigratedDatabase(): Unit = runBlocking {
        helper.createDatabase(name, 3).use { seed(it, 3) }
        val db = Room.databaseBuilder(ctx, AppDatabase::class.java, name).addMigrations(*ALL_MIGRATIONS).build()
        try {
            assertEquals("Test Name", db.identity().currentNow()?.name)
            assertEquals(1, db.anchors().allNow().size)
            assertEquals(0, db.pois().allNow().size)
            assertNotNull(db.fixes().lastPhone())
            assertEquals(3000L, db.fixes().lastDesktop()?.time)
            assertEquals(0, db.fixes().dedupeDesktop())
            db.snapshots().put(org.sworrl.beaconfix.data.db.SnapshotEntity("phone", "address", "{}", fetchedAt = 1))
            assertEquals("phone", db.snapshots().newestNow("address")?.source)
            // v5: the old position survives ungraded; the history keeps the last 20 per AP
            val ap = db.aps().get("02:00:00:00:00:01")
            assertEquals(40.0, ap?.lat ?: 0.0, 0.0); assertEquals(null, ap?.grade)
            repeat(25) { db.estimateHistory().append(org.sworrl.beaconfix.data.db.EstimateHistoryEntity(bssid = "02:00:00:00:00:01", time = it.toLong(), lat = 40.0, lon = -75.0, grade = "B")) }
            assertEquals(20, db.estimateHistory().forAp("02:00:00:00:00:01").size)
            assertEquals(24L, db.estimateHistory().forAp("02:00:00:00:00:01").first().time)
        } finally { db.close() }
    }

    // ── helpers ───────────────────────────────────────────────────────────────
    /** CREATE statements per table from an exported schema in the test assets (`${TABLE_NAME}` filled in). */
    private fun exported(v: Int): Map<String, List<String>> {
        val text = instrumentation.context.assets.open("org.sworrl.beaconfix.data.db.AppDatabase/$v.json").bufferedReader().use { it.readText() }
        val db = Json.parseToJsonElement(text).jsonObject["database"]!!.jsonObject
        return db["entities"]!!.jsonArray.associate { e ->
            val o = e.jsonObject; val t = o["tableName"]!!.jsonPrimitive.content
            val sql = listOf(o["createSql"]!!.jsonPrimitive.content) + (o["indices"]?.jsonArray?.map { it.jsonObject["createSql"]!!.jsonPrimitive.content } ?: emptyList())
            t to sql.map { it.replace("\${TABLE_NAME}", t) }
        }
    }

    /**
     * A v1 or v2 file the way Room wrote it. v1 = aps, observations, fixes, pois, desktops (all unchanged in 3.json;
     * v1's pois is the same (osmType, osmId) table, see `git show c79e01d:…/Entities.kt`); v2 adds identity + pending_links.
     */
    private fun createRaw(v: Int) {
        val s = exported(3)
        val tables = listOf("aps", "observations", "fixes", "pois", "desktops") + (if (v >= 2) listOf("identity", "pending_links") else emptyList())
        val f = ctx.getDatabasePath(name); f.parentFile?.mkdirs()
        val db = SQLiteDatabase.openOrCreateDatabase(f, null)
        try {
            tables.forEach { t -> s[t]!!.forEach { db.execSQL(it) } }
            db.execSQL("CREATE TABLE IF NOT EXISTS room_master_table (id INTEGER PRIMARY KEY,identity_hash TEXT)")
            db.execSQL("INSERT OR REPLACE INTO room_master_table (id,identity_hash) VALUES(42, 'v$v')")
            seedRaw({ db.execSQL(it) }, v)
            db.version = v
        } finally { db.close() }
    }

    private fun seed(db: SupportSQLiteDatabase, v: Int) = seedRaw({ db.execSQL(it) }, v)

    private fun seedRaw(exec: (String) -> Unit, v: Int) {
        exec("INSERT INTO aps (bssid, ssid, freq, band, ch, firstSeen, lastSeen, timesSeen, lat, lon, acc, posSource, home, travelling, ignored, security, rsnFlags, wpaFlags) " +
            "VALUES ('02:00:00:00:00:01', 'Test AP', 2412, '2.4', 1, 1000, 2000, 3, 40.0, -75.0, 25.0, 'observed', 1, 0, 0, 'wpa2', 0, 0)")
        exec("INSERT INTO observations (bssid, time, lat, lon, acc, dbm, freq, source, synced, remote) VALUES ('02:00:00:00:00:01', 1500, 40.0, -75.0, 8.0, -61, 2412, 'phone-gps', 0, 0)")
        exec("INSERT INTO fixes (time, lat, lon, acc, source, provider, place) VALUES (1000, 40.0, -75.0, 8.0, 'phone-gps', 'fused', '')")
        repeat(if (v >= 4) 1 else 3) { exec("INSERT INTO fixes (time, lat, lon, acc, source, provider, place) VALUES (3000, 40.001, -75.001, 20.0, 'desktop', 'wifi', 'Testville')") }
        if (v < 4) exec("INSERT INTO pois (osmType, osmId, cat, name, detail, lat, lon, phone, hours, website) VALUES ('way', 1, 'health', 'Test General', '', 40.0, -75.0, '', '', '')")
        exec("INSERT INTO desktops (id, host, port, name, hostname, version, tls, scopes, paired, lastSeen, lastSync, lastError, pushedObs, pulledAps, cursor) " +
            "VALUES ('192.0.2.1:47822', '192.0.2.1', 47822, 'desktop', 'desktop', '3.7.0', 0, 'read control', 1, 0, 0, '', 5, 6, '')")
        if (v >= 2) {
            exec("INSERT INTO identity (id, name, created, pub, recordJson) VALUES ('test-id', 'Test Name', '2026-01-01T00:00:00Z', 'test-pub', '{}')")
            exec("INSERT INTO pending_links (id, pub, name, ts, statementJson) VALUES ('other-id', 'other-pub', 'pi', '2026-01-01T00:00:00Z', '')")
        }
        if (v >= 4) exec("INSERT INTO snapshots (source, kind, json, lat, lon, fetchedAt) VALUES ('phone', 'address', '{}', 40.0, -75.0, 1)")
        if (v >= 3) exec("INSERT INTO anchors (id, json, name, kind, lat, lon, rv, ref, deleted, placedAt, seq, dirty) VALUES ('a1', '{}', 'Test anchor', 'custom', 40.0, -75.0, 1, 0, 0, '2026-01-01T00:00:00Z', 1, 0)")
    }

    private fun long(db: SupportSQLiteDatabase, sql: String): Long = db.query(sql).use { c -> c.moveToFirst(); c.getLong(0) }
    private fun text(db: SupportSQLiteDatabase, sql: String): String = db.query(sql).use { c -> c.moveToFirst(); c.getString(0) }

    private fun verify(db: SupportSQLiteDatabase, from: Int) {
        assertEquals(1L, long(db, "SELECT COUNT(*) FROM aps"))
        assertEquals("Test AP", text(db, "SELECT ssid FROM aps WHERE bssid = '02:00:00:00:00:01'"))
        assertEquals(1L, long(db, "SELECT home FROM aps"))
        assertEquals(1L, long(db, "SELECT COUNT(*) FROM observations"))
        assertEquals(-61L, long(db, "SELECT dbm FROM observations"))
        assertEquals(1L, long(db, "SELECT COUNT(*) FROM fixes WHERE source LIKE 'phone%'"))
        assertEquals("3 duplicate desktop fixes collapse to 1", 1L, long(db, "SELECT COUNT(*) FROM fixes WHERE source = 'desktop'"))
        assertEquals(1L, long(db, "SELECT COUNT(*) FROM desktops"))
        assertEquals("read control", text(db, "SELECT scopes FROM desktops"))
        assertEquals(if (from >= 2) 1L else 0L, long(db, "SELECT COUNT(*) FROM identity"))
        assertEquals(if (from >= 2) 1L else 0L, long(db, "SELECT COUNT(*) FROM pending_links"))
        assertEquals(if (from >= 3) 1L else 0L, long(db, "SELECT COUNT(*) FROM anchors"))
        if (from >= 2) assertEquals("Test Name", text(db, "SELECT name FROM identity"))
        assertEquals("pois is a fresh cache", 0L, long(db, "SELECT COUNT(*) FROM pois"))
        assertEquals(if (from >= 4) 1L else 0L, long(db, "SELECT COUNT(*) FROM snapshots"))
        // v5: positions survive, the new columns start empty, the history table exists
        assertEquals(40.0, double(db, "SELECT lat FROM aps WHERE bssid = '02:00:00:00:00:01'"), 0.0)
        assertEquals(25.0, double(db, "SELECT acc FROM aps WHERE bssid = '02:00:00:00:00:01'"), 0.0)
        assertEquals("observed", text(db, "SELECT posSource FROM aps"))
        assertEquals(1L, long(db, "SELECT COUNT(*) FROM aps WHERE grade IS NULL AND fitKind IS NULL AND r95 IS NULL AND fitMetrics IS NULL"))
        assertEquals(0L, long(db, "SELECT COUNT(*) FROM estimate_history"))
    }
    private fun double(db: SupportSQLiteDatabase, sql: String): Double = db.query(sql).use { c -> c.moveToFirst(); c.getDouble(0) }
}
