package org.sworrl.beaconfix.identity

import org.bouncycastle.crypto.generators.SCrypt
import org.bouncycastle.crypto.params.Ed25519PrivateKeyParameters
import org.bouncycastle.crypto.params.Ed25519PublicKeyParameters
import org.bouncycastle.crypto.signers.Ed25519Signer
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * The primitives behind BeaconFix identities, exactly as the shared spec (v1) defines them:
 * Ed25519 (RFC 8032) keys and signatures, SHA-256, Crockford base32 ids, scrypt-derived AES-256-GCM
 * export bundles. Pure Kotlin/JVM so the unit tests run on the host.
 */
object Crypto {
    private val rng = SecureRandom()
    fun random(n: Int): ByteArray = ByteArray(n).also { rng.nextBytes(it) }

    // ── Ed25519 ──────────────────────────────────────────────────────────────
    fun publicKey(seed: ByteArray): ByteArray = Ed25519PrivateKeyParameters(seed, 0).generatePublicKey().encoded
    fun sign(seed: ByteArray, message: ByteArray): ByteArray {
        val s = Ed25519Signer(); s.init(true, Ed25519PrivateKeyParameters(seed, 0)); s.update(message, 0, message.size); return s.generateSignature()
    }
    fun verify(pub: ByteArray, message: ByteArray, sig: ByteArray): Boolean = try {
        val v = Ed25519Signer(); v.init(false, Ed25519PublicKeyParameters(pub, 0)); v.update(message, 0, message.size); v.verifySignature(sig)
    } catch (e: Exception) { false }

    // ── hashing / ids ────────────────────────────────────────────────────────
    fun sha256(b: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(b)
    /** id = Crockford base32 (lower-case, no padding) of the first 16 bytes of SHA-256(pub): 26 characters. */
    fun idOf(pub: ByteArray): String = base32Crockford(sha256(pub).copyOf(16))
    fun grouped(id: String): String = id.chunked(4).joinToString(" ")

    private const val B32 = "0123456789abcdefghjkmnpqrstvwxyz"
    fun base32Crockford(data: ByteArray): String {
        val sb = StringBuilder(); var buf = 0; var bits = 0
        for (b in data) { buf = (buf shl 8) or (b.toInt() and 0xff); bits += 8; while (bits >= 5) { bits -= 5; sb.append(B32[(buf shr bits) and 31]) } }
        if (bits > 0) sb.append(B32[(buf shl (5 - bits)) and 31])
        return sb.toString()
    }

    // ── base64 ───────────────────────────────────────────────────────────────
    fun b64(b: ByteArray): String = Base64.getEncoder().encodeToString(b)
    fun unb64(s: String): ByteArray = Base64.getDecoder().decode(s.trim())
    fun b64url(b: ByteArray): String = Base64.getUrlEncoder().withoutPadding().encodeToString(b)
    fun unb64url(s: String): ByteArray = Base64.getUrlDecoder().decode(s.trim().trimEnd('='))

    // ── scrypt + AES-256-GCM (export bundles) ────────────────────────────────
    const val SCRYPT_N = 32768; const val SCRYPT_R = 8; const val SCRYPT_P = 1
    const val AAD = "beaconfix-identity-v1"
    fun scrypt(passphrase: String, salt: ByteArray, n: Int = SCRYPT_N, r: Int = SCRYPT_R, p: Int = SCRYPT_P): ByteArray =
        SCrypt.generate(passphrase.toByteArray(Charsets.UTF_8), salt, n, r, p, 32)
    fun aesGcmEncrypt(key: ByteArray, nonce: ByteArray, plaintext: ByteArray, aad: ByteArray): ByteArray {
        val c = Cipher.getInstance("AES/GCM/NoPadding"); c.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, nonce)); c.updateAAD(aad); return c.doFinal(plaintext)
    }
    fun aesGcmDecrypt(key: ByteArray, nonce: ByteArray, ct: ByteArray, aad: ByteArray): ByteArray {
        val c = Cipher.getInstance("AES/GCM/NoPadding"); c.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, nonce)); c.updateAAD(aad); return c.doFinal(ct)
    }

    /** Passphrases typed by people: trim, lower-case, hyphens and runs of whitespace → one space (so a 6-word code survives any separator). */
    fun normalisePassphrase(p: String): String = p.trim().lowercase().replace(Regex("[\\s\\-_]+"), " ")
}
