package org.sworrl.beaconfix.net

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import org.bouncycastle.crypto.agreement.X25519Agreement
import org.bouncycastle.crypto.digests.SHA512Digest
import org.bouncycastle.crypto.generators.HKDFBytesGenerator
import org.bouncycastle.crypto.macs.HMac
import org.bouncycastle.crypto.modes.ChaCha20Poly1305
import org.bouncycastle.crypto.params.AEADParameters
import org.bouncycastle.crypto.params.HKDFParameters
import org.bouncycastle.crypto.params.KeyParameter
import org.bouncycastle.crypto.params.X25519PrivateKeyParameters
import org.bouncycastle.crypto.params.X25519PublicKeyParameters
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64

/** A BFS3 message that does not authenticate (bad tag, wrong counter, malformed) or an invite / answer we cannot accept. */
class Bfs3Exception(message: String) : Exception(message)

/**
 * The BFS3 secure channel (docs/SECURE-API.md — normative; reference `tools/bfs3_ref.py`, vectors
 * `tests/fixtures/bfs3_vectors.json`): X25519 device keys, HKDF-SHA512, ChaCha20-Poly1305 (RFC 8439) per message
 * with a key derived from the message counter, HMAC-SHA512 for enrolment. Pure Kotlin/JVM (BouncyCastle), so the
 * unit tests reproduce every vector on the host. Strings are UTF-8; `b64u` is base64url without padding.
 */
object Bfs3 {
    const val PREFIX = "bfs3:"
    const val SEALED_TYPE = "application/vnd.beaconfix.sealed"
    const val TIME_WINDOW_S = 300L
    const val H_DEVICE = "X-BF-Device"
    const val H_COUNTER = "X-BF-Counter"
    const val H_TIME = "X-BF-Time"
    const val H_NONCE = "X-BF-Nonce"
    /**
     * The sealed bytes of a bodiless request (GET/HEAD: OkHttp, like most HTTP stacks, refuses a GET body), as the hub
     * reads them: b64u(ciphertext ‖ tag) — just the tag for an empty plaintext.
     */
    const val H_SEALED = "X-BF-Seal"
    /** The hub's clock (unix seconds) on every answer, sealed or not: informative, lets the client correct its skew. */
    const val H_SERVER_TIME = "X-BF-Time"

    private val rng = SecureRandom()
    fun random(n: Int): ByteArray = ByteArray(n).also { rng.nextBytes(it) }

    // ── encoding ────────────────────────────────────────────────────────────
    fun b64u(b: ByteArray): String = Base64.getUrlEncoder().withoutPadding().encodeToString(b)
    fun unb64u(s: String): ByteArray = try { Base64.getUrlDecoder().decode(s.trim().trimEnd('=')) } catch (e: IllegalArgumentException) { throw Bfs3Exception("not base64url") }
    fun hex(b: ByteArray): String = buildString(b.size * 2) { for (x in b) { val v = x.toInt() and 0xff; append(HEX[v shr 4]); append(HEX[v and 15]) } }
    private const val HEX = "0123456789abcdef"
    fun u64be(x: Long): ByteArray = ByteArray(8) { i -> (x ushr (56 - 8 * i)).toByte() }
    private fun utf8(s: String) = s.toByteArray(Charsets.UTF_8)
    private fun cat(vararg parts: ByteArray): ByteArray { val out = ByteArray(parts.sumOf { it.size }); var o = 0; for (p in parts) { p.copyInto(out, o); o += p.size }; return out }
    /** Counters are u64 on the wire and printed in decimal; Kotlin's Long holds every counter a device will ever reach. */
    private fun dec(c: Long) = java.lang.Long.toUnsignedString(c)

    // ── primitives ──────────────────────────────────────────────────────────
    fun sha256(b: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(b)
    fun newPrivateKey(): ByteArray = X25519PrivateKeyParameters(rng).encoded
    fun publicKey(sk: ByteArray): ByteArray { require(sk.size == 32) { "X25519 private key must be 32 bytes" }; return X25519PrivateKeyParameters(sk, 0).generatePublicKey().encoded }
    /** X25519(sk, pk); an all-zero result (a low-order public key) is refused. */
    fun x25519(sk: ByteArray, pk: ByteArray): ByteArray {
        if (sk.size != 32 || pk.size != 32) throw Bfs3Exception("X25519 keys are 32 bytes")
        val a = X25519Agreement(); a.init(X25519PrivateKeyParameters(sk, 0))
        val out = ByteArray(a.agreementSize)
        try { a.calculateAgreement(X25519PublicKeyParameters(pk, 0), out, 0) } catch (e: IllegalStateException) { throw Bfs3Exception("X25519: invalid public key") }
        return out
    }
    /** HKDF-SHA512 (RFC 5869). An empty salt is the RFC's default (HashLen zero bytes), as in the reference. */
    fun hkdf(ikm: ByteArray, salt: ByteArray, info: ByteArray, n: Int = 32): ByteArray {
        val g = HKDFBytesGenerator(SHA512Digest())
        g.init(HKDFParameters(ikm, salt.takeIf { it.isNotEmpty() }, info))
        return ByteArray(n).also { g.generateBytes(it, 0, n) }
    }
    fun hmacSha512(key: ByteArray, msg: ByteArray): ByteArray {
        val m = HMac(SHA512Digest()); m.init(KeyParameter(key)); m.update(msg, 0, msg.size)
        return ByteArray(m.macSize).also { m.doFinal(it, 0) }
    }
    /** ChaCha20-Poly1305 IETF (RFC 8439): 32-byte key, 12-byte nonce, the 16-byte tag appended. Open throws on a bad tag. */
    fun aead(encrypt: Boolean, key: ByteArray, nonce: ByteArray, aad: ByteArray, input: ByteArray): ByteArray {
        if (key.size != 32) throw Bfs3Exception("key must be 32 bytes")
        if (nonce.size != 12) throw Bfs3Exception("nonce must be 12 bytes")
        if (!encrypt && input.size < 16) throw Bfs3Exception("sealed message too short")
        val c = ChaCha20Poly1305()
        c.init(encrypt, AEADParameters(KeyParameter(key), 128, nonce, aad))
        val out = ByteArray(c.getOutputSize(input.size))
        return try {
            var n = c.processBytes(input, 0, input.size, out, 0)
            n += c.doFinal(out, n)
            if (n == out.size) out else out.copyOf(n)
        } catch (e: org.bouncycastle.crypto.InvalidCipherTextException) { throw Bfs3Exception("authentication failed") }
    }
    fun constantTimeEquals(a: String, b: String): Boolean = MessageDigest.isEqual(utf8(a), utf8(b))

    // ── identities ──────────────────────────────────────────────────────────
    /** hex(SHA-256(S_pk))[0:32] — shown at the hub, compared by the user, pinned at enrolment. */
    fun fingerprint(serverPub: ByteArray): String = hex(sha256(serverPub)).substring(0, 32)
    /** The fingerprint in groups of four, as the user compares it with the hub's screen. */
    fun groupedFingerprint(fp: String): String = fp.chunked(4).joinToString(" ")
    fun deviceId(devicePub: ByteArray): String = "d" + hex(sha256(devicePub)).substring(0, 24)

    // ── root key ────────────────────────────────────────────────────────────
    fun rootKey(shared: ByteArray, serverPub: ByteArray, devicePub: ByteArray): ByteArray =
        hkdf(shared, sha256(cat(utf8("bfs3"), serverPub, devicePub)), utf8("beaconfix.bfs3.root"))
    /** The device side: rk from our private key and the hub's public key. */
    fun rootKeyOf(deviceSk: ByteArray, serverPub: ByteArray): ByteArray = rootKey(x25519(deviceSk, serverPub), serverPub, publicKey(deviceSk))

    // ── enrolment ───────────────────────────────────────────────────────────
    fun enrollMac(inviteSecret: ByteArray, inviteId: String, name: String, kind: String, pub: String, ts: Long): String =
        hex(hmacSha512(inviteSecret, utf8("bfs3-enroll\n$inviteId\n$name\n$kind\n$pub\n$ts")))
    fun proof(rk: ByteArray, deviceId: String): String = hex(hmacSha512(rk, utf8("bfs3-enrolled\n$deviceId")))
    fun checkProof(rk: ByteArray, deviceId: String, proof: String): Boolean = constantTimeEquals(proof(rk, deviceId), proof.trim().lowercase())

    /** `bfs3:` + b64u(JSON {"u": url, "s": b64u(S_pk), "i": inviteId, "k": b64u(I), "e": expiryUnix}). */
    class Invite(val url: String, val serverPub: ByteArray, val inviteId: String, val secret: ByteArray, val expires: Long) {
        val fingerprint: String get() = fingerprint(serverPub)
        fun expired(nowS: Long) = expires in 1 until nowS
    }
    private val lenient = Json { ignoreUnknownKeys = true; isLenient = true }
    fun parseInvite(text: String): Invite {
        val t = text.trim().replace(Regex("\\s+"), "")
        if (!t.startsWith(PREFIX, ignoreCase = true)) throw Bfs3Exception("not a BFS3 invite (it starts with bfs3:)")
        val o = try { lenient.parseToJsonElement(String(unb64u(t.substring(PREFIX.length)), Charsets.UTF_8)).jsonObject } catch (e: Bfs3Exception) { throw e } catch (e: Exception) { throw Bfs3Exception("the invite is damaged") }
        fun s(k: String) = o[k]?.jsonPrimitive?.contentOrNull ?: throw Bfs3Exception("the invite has no \"$k\"")
        val spk = unb64u(s("s")); val secret = unb64u(s("k")); val id = s("i")
        if (spk.size != 32) throw Bfs3Exception("the invite's server key is not 32 bytes")
        if (secret.size != 32) throw Bfs3Exception("the invite's secret is not 32 bytes")
        if (!id.matches(Regex("[0-9a-f]{16}"))) throw Bfs3Exception("the invite id is not 16 hex characters")
        return Invite(s("u"), spk, id, secret, o["e"]?.jsonPrimitive?.longOrNull ?: 0L)
    }

    // ── requests ────────────────────────────────────────────────────────────
    fun requestKey(rk: ByteArray, c: Long): ByteArray = hkdf(rk, ByteArray(0), cat(utf8("bfs3 req"), u64be(c)))
    fun requestAad(method: String, target: String, deviceId: String, c: Long, ts: Long): String = "bfs3\n$method\n$target\n$deviceId\n${dec(c)}\n$ts"
    fun sealRequest(rk: ByteArray, deviceId: String, method: String, target: String, c: Long, ts: Long, nonce: ByteArray, plaintext: ByteArray): ByteArray =
        aead(true, requestKey(rk, c), nonce, utf8(requestAad(method, target, deviceId, c, ts)), plaintext)
    /** The hub's side (and the tests'): throws [Bfs3Exception] on a bad tag. */
    fun openRequest(rk: ByteArray, deviceId: String, method: String, target: String, c: Long, ts: Long, nonce: ByteArray, sealed: ByteArray): ByteArray =
        aead(false, requestKey(rk, c), nonce, utf8(requestAad(method, target, deviceId, c, ts)), sealed)

    // ── responses ───────────────────────────────────────────────────────────
    fun responseKey(rk: ByteArray, c: Long): ByteArray = hkdf(rk, ByteArray(0), cat(utf8("bfs3 resp"), u64be(c)))
    fun responseAad(status: Int, deviceId: String, c: Long): String = "bfs3-resp\n$status\n$deviceId\n${dec(c)}"
    fun sealResponse(rk: ByteArray, deviceId: String, status: Int, c: Long, nonce: ByteArray, plaintext: ByteArray): ByteArray =
        aead(true, responseKey(rk, c), nonce, utf8(responseAad(status, deviceId, c)), plaintext)
    fun openResponse(rk: ByteArray, deviceId: String, status: Int, c: Long, nonce: ByteArray, sealed: ByteArray): ByteArray =
        aead(false, responseKey(rk, c), nonce, utf8(responseAad(status, deviceId, c)), sealed)
    /**
     * A response as the client receives it: the counter header must be the request's counter (else it answers another
     * request), the nonce header must be there, and the tag must verify under the status it came with.
     */
    fun openResponseFor(rk: ByteArray, deviceId: String, requestCounter: Long, status: Int, counterHeader: String?, nonceHeader: String?, sealed: ByteArray): ByteArray {
        val c = counterHeader?.trim()?.let { runCatching { java.lang.Long.parseUnsignedLong(it) }.getOrNull() } ?: throw Bfs3Exception("response without a counter")
        if (c != requestCounter) throw Bfs3Exception("response counter $c is not the request's ${dec(requestCounter)}")
        val nonce = nonceHeader?.let { unb64u(it) } ?: throw Bfs3Exception("response without a nonce")
        return openResponse(rk, deviceId, status, c, nonce, sealed)
    }

    // ── events (GET /api/v3/stream) ─────────────────────────────────────────
    fun eventKey(rk: ByteArray, c: Long, seq: Long): ByteArray = hkdf(rk, ByteArray(0), cat(utf8("bfs3 evt"), u64be(c), u64be(seq)))
    fun eventAad(deviceId: String, c: Long, seq: Long): String = "bfs3-evt\n$deviceId\n${dec(c)}\n${dec(seq)}"
    fun sealEvent(rk: ByteArray, deviceId: String, c: Long, seq: Long, nonce: ByteArray, plaintext: ByteArray): String =
        b64u(cat(nonce, aead(true, eventKey(rk, c, seq), nonce, utf8(eventAad(deviceId, c, seq)), plaintext)))
    /** One `data:` line of the stream opened by request counter [c]: b64u(nonce ‖ ciphertext ‖ tag) → the event JSON. */
    fun openEvent(rk: ByteArray, deviceId: String, c: Long, seq: Long, line: String): ByteArray {
        val raw = unb64u(line.trim().removePrefix("data:").trim())
        if (raw.size < 12 + 16) throw Bfs3Exception("event too short")
        return aead(false, eventKey(rk, c, seq), raw.copyOfRange(0, 12), utf8(eventAad(deviceId, c, seq)), raw.copyOfRange(12, raw.size))
    }
}

/**
 * Hands out request counters that are never reused, even across crashes: a block is reserved durably
 * ([Store.reserve] returns only once the new upper bound is on disk) before any counter in it is used, and a restart
 * continues after the last reserved block (the unused rest of it is skipped — the hub only needs counters to grow).
 */
class CounterAllocator(private val store: Store, private val block: Long = 32) {
    interface Store {
        /** The highest counter reserved so far (0 = none; counters start at 1). */
        fun reserved(): Long
        /** Durably record [upTo] as reserved; throw when that fails. */
        fun reserve(upTo: Long)
    }
    private var next = store.reserved() + 1
    private var limit = store.reserved()
    @Synchronized fun next(): Long {
        if (next > limit) { val up = next + block - 1; store.reserve(up); limit = up }
        return next++
    }
    /**
     * Jump [n] counters ahead (durably): after an unauthenticated 401 the hub may have seen counters this phone lost
     * track of (a restored or rolled-back store) — the spec's recovery is to move past them.
     */
    @Synchronized fun skip(n: Long) {
        next += n
        if (next > limit) { val up = next + block - 1; store.reserve(up); limit = up }
    }
}
