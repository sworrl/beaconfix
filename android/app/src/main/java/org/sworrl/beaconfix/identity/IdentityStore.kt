package org.sworrl.beaconfix.identity

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.serialization.encodeToString
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.data.db.IdentityEntity
import org.sworrl.beaconfix.data.db.PendingLinkEntity
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Our identity: the record in Room, the Ed25519 seed in Keystore-wrapped EncryptedSharedPreferences.
 * Everything cryptographic lives in [IdentityOps] so it is testable without Android.
 */
@Singleton
class IdentityStore @Inject constructor(@ApplicationContext private val ctx: Context, private val db: AppDatabase) {
    private val prefs: SharedPreferences by lazy {
        val key = MasterKey.Builder(ctx).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build()
        EncryptedSharedPreferences.create(ctx, "beaconfix_identity", key,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV, EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM)
    }
    val deviceName: String get() = (Build.MANUFACTURER + " " + Build.MODEL).trim().ifEmpty { "Android phone" }

    val current: Flow<IdentityRecord?> = db.identity().current().map { it?.let { e -> identityJson.decodeFromString(IdentityRecord.serializer(), e.recordJson) } }
    val pending: Flow<List<PendingLinkEntity>> = db.identity().pending()
    suspend fun currentNow(): IdentityRecord? = db.identity().currentNow()?.let { identityJson.decodeFromString(IdentityRecord.serializer(), it.recordJson) }
    fun seed(): ByteArray? = prefs.getString("seed", null)?.let { Crypto.unb64(it) }
    suspend fun exists() = currentNow() != null && seed() != null

    private suspend fun save(rec: IdentityRecord) = db.identity().upsert(IdentityEntity(rec.id, rec.name, rec.created, rec.pub, identityJson.encodeToString(rec)))

    /** Mint a brand-new identity on this phone and register this device on it. */
    suspend fun create(name: String): IdentityRecord {
        val seed = Crypto.random(32)
        val rec = IdentityOps.newRecord(seed, name.trim().take(64).ifEmpty { "BeaconFix" }, DeviceEntry(deviceName, "android", isoNow()))
        prefs.edit().putString("seed", Crypto.b64(seed)).apply()
        db.identity().clear(); save(rec)
        return rec
    }

    /** Import a bundle ("BFID1:…" text or the JSON) with its passphrase; this device is added to the record. */
    suspend fun import(text: String, passphrase: String): IdentityRecord {
        val plain = IdentityOps.open(text, passphrase)
        val seed = Crypto.unb64(plain.seed)
        val rec0 = plain.record
        require(Crypto.idOf(Crypto.publicKey(seed)) == rec0.id) { "bundle is inconsistent: id does not match the key" }
        val rec = if (rec0.devices.any { it.name == deviceName && it.kind == "android" }) rec0 else rec0.copy(devices = rec0.devices + DeviceEntry(deviceName, "android", isoNow()))
        prefs.edit().putString("seed", Crypto.b64(seed)).apply()
        db.identity().clear(); save(rec)
        return rec
    }

    /** Export as the "BFID1:" text form under [passphrase] (a chosen one or a 6-word code). */
    suspend fun export(passphrase: String): String {
        val rec = currentNow() ?: error("no identity"); val seed = seed() ?: error("no identity")
        return IdentityOps.seal(rec, seed, passphrase)
    }

    suspend fun rename(name: String) { currentNow()?.let { save(it.copy(name = name.trim().take(64))) } }
    suspend fun forgetDevice(name: String) { currentNow()?.let { r -> save(r.copy(devices = r.devices.filter { it.name != name })) } }
    suspend fun forgetAll() { db.identity().clear(); db.identity().clearPending(); prefs.edit().remove("seed").apply() }

    /** Signature for the challenge-auth login (spec: "beaconfix-auth|v1|host|nonce|id|device"). */
    suspend fun authSignature(host: String, nonce: String): String? {
        val rec = currentNow() ?: return null; val seed = seed() ?: return null
        return Crypto.b64(Crypto.sign(seed, IdentityOps.authCanon(host, nonce, rec.id, deviceName).toByteArray()))
    }

    // ── links ────────────────────────────────────────────────────────────────
    suspend fun linkedIds(): Set<String> = currentNow()?.let { IdentityOps.linkedSet(it) } ?: emptySet()
    /** Our link payload (QR / `BFLNK1:` text): id, name, pub, ts — nothing to sign yet, the canon needs both ids. */
    suspend fun offer(): LinkOffer? {
        val rec = currentNow() ?: return null
        return LinkOffer(id = rec.id, name = rec.name, pub = rec.pub, ts = isoNow())
    }
    /** Take another identity's payload (scanned/pasted): build the statement, sign our side, carry both pubs; the other side co-signs. */
    suspend fun complete(offer: LinkOffer): LinkStatement {
        val rec = currentNow() ?: error("no identity"); val seed = seed() ?: error("no identity")
        require(IdentityOps.verifyOffer(offer)) { "that link payload is inconsistent (id does not match the key)" }
        require(offer.id != rec.id) { "that is this identity" }
        val st = IdentityOps.completeLink(rec.id, seed, offer.id, offer.ts, rec.pub, offer.pub)
        addPending(offer, st)
        return st
    }
    /** Record a link statement (from the LAN API, a QR, or sync). Public keys come from the statement's pubA/pubB, the
     *  arguments, or a pending entry; the link is stored only when BOTH signatures verify against keys whose ids match. */
    suspend fun addLink(st: LinkStatement, pubA: ByteArray? = null, pubB: ByteArray? = null): Boolean {
        val rec = currentNow() ?: return false
        if (!st.complete || (st.a != rec.id && st.b != rec.id)) return false
        val other = st.other(rec.id)
        val pending = db.identity().pendingNow().firstOrNull { it.id == other }?.pub?.let { runCatching { Crypto.unb64(it) }.getOrNull() }
        val ours = Crypto.unb64(rec.pub)
        val pa = if (st.a == rec.id) ours else (st.pubA?.let { Crypto.unb64(it) } ?: pubA ?: pending) ?: return false
        val pb = if (st.b == rec.id) ours else (st.pubB?.let { Crypto.unb64(it) } ?: pubB ?: pending) ?: return false
        if (!IdentityOps.verifyLink(st, pa, pb)) return false
        val stored = st.copy(pubA = Crypto.b64(pa), pubB = Crypto.b64(pb))
        if (rec.links.none { it.a == st.a && it.b == st.b && it.ts == st.ts }) save(rec.copy(links = rec.links + stored))
        db.identity().removePending(other)
        return true
    }
    /** Sign our side of a statement another node started; our pub goes into the statement so the other side can verify. */
    suspend fun countersign(st: LinkStatement): LinkStatement {
        val rec = currentNow() ?: error("no identity"); val seed = seed() ?: error("no identity")
        val sig = Crypto.b64(Crypto.sign(seed, st.canon.toByteArray()))
        return if (st.a == rec.id) st.copy(sigA = sig, pubA = rec.pub) else st.copy(sigB = sig, pubB = rec.pub)
    }
    suspend fun addPending(offer: LinkOffer, st: LinkStatement?) = db.identity().upsertPending(PendingLinkEntity(offer.id, offer.pub, offer.name, offer.ts, st?.let { identityJson.encodeToString(it) } ?: ""))
    suspend fun removePending(id: String) = db.identity().removePending(id)
}

/** Pure functions of the spec; shared by the store and the unit tests. */
object IdentityOps {
    fun newRecord(seed: ByteArray, name: String, device: DeviceEntry): IdentityRecord {
        val pub = Crypto.publicKey(seed)
        return IdentityRecord(id = Crypto.idOf(pub), name = name, created = isoNow(), pub = Crypto.b64(pub), devices = listOf(device))
    }
    fun authCanon(host: String, nonce: String, id: String, device: String) = "beaconfix-auth|v1|$host|$nonce|$id|$device"
    /** A link payload is not signed; it is consistent when the id is the hash of the key it carries. */
    fun verifyOffer(o: LinkOffer): Boolean {
        val pub = runCatching { Crypto.unb64(o.pub) }.getOrNull() ?: return false
        return o.t == "beaconfix-link" && pub.size == 32 && Crypto.idOf(pub) == o.id
    }
    /** Our side of the statement for (us, other) at [ts], with both public keys attached for the receiver to verify. */
    fun completeLink(ourId: String, ourSeed: ByteArray, otherId: String, ts: String, ourPub: String, otherPub: String): LinkStatement {
        val a = minOf(ourId, otherId); val b = maxOf(ourId, otherId)
        val sig = Crypto.b64(Crypto.sign(ourSeed, LinkStatement.canonOf(a, b, ts).toByteArray()))
        val pa = if (ourId == a) ourPub else otherPub; val pb = if (ourId == a) otherPub else ourPub
        return if (ourId == a) LinkStatement(a = a, b = b, ts = ts, sigA = sig, pubA = pa, pubB = pb) else LinkStatement(a = a, b = b, ts = ts, sigB = sig, pubA = pa, pubB = pb)
    }
    fun verifyLink(st: LinkStatement, pubA: ByteArray, pubB: ByteArray): Boolean {
        if (!st.complete || Crypto.idOf(pubA) != st.a || Crypto.idOf(pubB) != st.b) return false
        val sa = runCatching { Crypto.unb64(st.sigA) }.getOrNull() ?: return false
        val sb = runCatching { Crypto.unb64(st.sigB) }.getOrNull() ?: return false
        return Crypto.verify(pubA, st.canon.toByteArray(), sa) && Crypto.verify(pubB, st.canon.toByteArray(), sb)
    }
    /** Transitive closure over the record's links (an identity linked to a linked identity is ours too). */
    fun linkedSet(rec: IdentityRecord): Set<String> {
        val out = mutableSetOf(rec.id); var grew = true
        while (grew) { grew = false; for (l in rec.links) if (l.complete && (l.a in out || l.b in out)) { if (out.add(l.a)) grew = true; if (out.add(l.b)) grew = true } }
        return out
    }

    // ── export bundles ───────────────────────────────────────────────────────
    const val PREFIX = "BFID1:"
    fun seal(rec: IdentityRecord, seed: ByteArray, passphrase: String): String {
        val salt = Crypto.random(16); val nonce = Crypto.random(12)
        val key = Crypto.scrypt(Crypto.normalisePassphrase(passphrase), salt)
        val plain = identityJson.encodeToString(ExportPlain(rec, Crypto.b64(seed))).toByteArray()
        val ct = Crypto.aesGcmEncrypt(key, nonce, plain, Crypto.AAD.toByteArray())
        val bundle = ExportBundle(kdf = Kdf(salt = Crypto.b64(salt)), aead = Aead(nonce = Crypto.b64(nonce)), ct = Crypto.b64(ct))
        return PREFIX + Crypto.b64url(identityJson.encodeToString(bundle).toByteArray())
    }
    fun parseBundle(text: String): ExportBundle {
        val t = text.trim()
        val json = when {
            t.startsWith(PREFIX) -> String(Crypto.unb64url(t.removePrefix(PREFIX)))
            t.startsWith("{") -> t
            else -> error("not a BeaconFix identity (expected BFID1:… or the bundle JSON)")
        }
        val b = identityJson.decodeFromString(ExportBundle.serializer(), json)
        require(b.v == 1 && b.t == "beaconfix-identity") { "unsupported bundle" }
        require(b.kdf.name == "scrypt" && b.aead.name == "aes-256-gcm") { "unsupported cipher suite" }
        return b
    }
    fun open(text: String, passphrase: String): ExportPlain {
        val b = parseBundle(text)
        val key = Crypto.scrypt(Crypto.normalisePassphrase(passphrase), Crypto.unb64(b.kdf.salt), b.kdf.n, b.kdf.r, b.kdf.p)
        val plain = try { Crypto.aesGcmDecrypt(key, Crypto.unb64(b.aead.nonce), Crypto.unb64(b.ct), Crypto.AAD.toByteArray()) } catch (e: Exception) { error("wrong passphrase") }
        return identityJson.decodeFromString(ExportPlain.serializer(), String(plain))
    }

    // ── link QR text forms ───────────────────────────────────────────────────
    const val OFFER_PREFIX = "BFLNK1:"       // a link payload {v,t,id,name,pub,ts} (no signature)
    const val STMT_PREFIX = "BFLINK1:"       // a (half- or fully-) signed statement {v,a,b,ts,sigA,sigB,pubA?,pubB?}
    fun encodeOffer(o: LinkOffer) = OFFER_PREFIX + Crypto.b64url(identityJson.encodeToString(o).toByteArray())
    fun decodeOffer(t: String): LinkOffer? = runCatching { identityJson.decodeFromString(LinkOffer.serializer(), String(Crypto.unb64url(t.trim().removePrefix(OFFER_PREFIX)))) }.getOrNull()?.takeIf { t.trim().startsWith(OFFER_PREFIX) }
    fun encodeStatement(s: LinkStatement) = STMT_PREFIX + Crypto.b64url(identityJson.encodeToString(s).toByteArray())
    fun decodeStatement(t: String): LinkStatement? = runCatching { identityJson.decodeFromString(LinkStatement.serializer(), if (t.trim().startsWith("{")) t.trim() else String(Crypto.unb64url(t.trim().removePrefix(STMT_PREFIX)))) }.getOrNull()?.takeIf { t.trim().startsWith(STMT_PREFIX) || t.trim().startsWith("{") }

    /** Fixed-seed vectors (seed = 32×0x01) for cross-implementation checks; the desktop prints the same from `--identity-selftest`. */
    data class SelfTest(val id: String, val pubB64: String, val sigB64: String)
    fun selfTest(): SelfTest {
        val seed = ByteArray(32) { 1 }
        val pub = Crypto.publicKey(seed)
        return SelfTest(Crypto.idOf(pub), Crypto.b64(pub), Crypto.b64(Crypto.sign(seed, "beaconfix-selftest|v1".toByteArray())))
    }
}
