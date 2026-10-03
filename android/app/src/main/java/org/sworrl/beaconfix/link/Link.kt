package org.sworrl.beaconfix.link

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.longOrNull
import org.bouncycastle.crypto.digests.SHA256Digest
import org.bouncycastle.crypto.generators.HKDFBytesGenerator
import org.bouncycastle.crypto.macs.HMac
import org.bouncycastle.crypto.params.HKDFParameters
import org.bouncycastle.crypto.params.KeyParameter
import org.sworrl.beaconfix.net.Bfs3
import org.sworrl.beaconfix.net.Bfs3Exception

/** A `bflink:` QR (docs/LINKING.md "The QR"): one link session the PC shows on its screen. */
data class LinkQr(
    val sid: String, val name: String, val hosts: List<String>, val port: Int, val pcPub: ByteArray, val k: ByteArray,
    val expires: Long, val hub: String?,
) {
    fun expired(nowS: Long, graceS: Long = 60) = expires > 0 && expires + graceS < nowS
    override fun equals(other: Any?) = other is LinkQr && other.sid == sid && other.pcPub.contentEquals(pcPub) && other.k.contentEquals(k)
    override fun hashCode() = sid.hashCode()
}

/** The PC as named in the sealed payload. */
data class LinkPc(val name: String, val id: String, val hosts: List<String>, val port: Int)

/** What an approved link delivers (sealed to the session key): the LAN token and, when the PC has a hub, an invite. */
data class LinkPayload(val token: String, val scopes: List<String>, val pc: LinkPc, val hub: String?)

/** A scanned QR, classified. */
sealed class Scanned {
    data class Link(val qr: LinkQr) : Scanned()
    data class HubInvite(val invite: Bfs3.Invite, val text: String) : Scanned()
    data class Bad(val message: String) : Scanned()
}

/**
 * Linking v3 (docs/LINKING.md — normative; reference `tools/link_ref.py`, vectors `tests/fixtures/link_vectors.json`):
 * X25519 (shared with BFS3), HKDF-**SHA256** (BFS3 uses SHA-512 — not here), ChaCha20-Poly1305 IETF, HMAC-SHA256.
 * Pure Kotlin/JVM, so the unit tests reproduce every vector on the host.
 */
object Link {
    const val PREFIX = "bflink:"
    const val KIND = "android"
    const val CODE_INFO = "beaconfix-link-code-v1"
    const val PAYLOAD_INFO = "beaconfix-link-payload-v1"
    /** The phone polls every 2 s for at most 2 min (docs/LINKING.md step 3). */
    const val POLL_MS = 2000L
    const val POLL_LIMIT_MS = 120_000L

    private val SID = Regex("[0-9a-f]{16}")
    private val lenient = Json { ignoreUnknownKeys = true; isLenient = true }
    private fun utf8(s: String) = s.toByteArray(Charsets.UTF_8)

    // ── primitives ──────────────────────────────────────────────────────────
    /** HKDF-SHA256 (RFC 5869). */
    fun hkdf256(ikm: ByteArray, salt: ByteArray, info: ByteArray, n: Int): ByteArray {
        val g = HKDFBytesGenerator(SHA256Digest())
        g.init(HKDFParameters(ikm, salt.takeIf { it.isNotEmpty() }, info))
        return ByteArray(n).also { g.generateBytes(it, 0, n) }
    }
    fun hmacSha256(key: ByteArray, msg: ByteArray): ByteArray {
        val m = HMac(SHA256Digest()); m.init(KeyParameter(key)); m.update(msg, 0, msg.size)
        return ByteArray(m.macSize).also { m.doFinal(it, 0) }
    }

    // ── the exchange ────────────────────────────────────────────────────────
    /** QR path: hex(HMAC-SHA256(k, "bflink\n" + sid + "\n" + name + "\n" + kind + "\n" + pub)), pub as sent (b64u). */
    fun mac(k: ByteArray, sid: String, name: String, kind: String, pubB64u: String): String =
        Bfs3.hex(hmacSha256(k, utf8("bflink\n$sid\n$name\n$kind\n$pubB64u")))

    /** mDNS path: the phone's commitment to its key, sent before it sees the PC's: hex(SHA-256("bflink-commit\n" + name + "\n" + kind + "\n" + pub)). */
    fun commit(name: String, kind: String, pubB64u: String): String = Bfs3.hex(Bfs3.sha256(utf8("bflink-commit\n$name\n$kind\n$pubB64u")))

    /** The six digits both screens show: u32be(HKDF-SHA256(shared, sid, "beaconfix-link-code-v1", 4)) mod 1 000 000. */
    fun code(shared: ByteArray, sid: String): String {
        val b = hkdf256(shared, utf8(sid), utf8(CODE_INFO), 4)
        val v = ((b[0].toLong() and 0xff) shl 24) or ((b[1].toLong() and 0xff) shl 16) or ((b[2].toLong() and 0xff) shl 8) or (b[3].toLong() and 0xff)
        return (v % 1_000_000L).toString().padStart(6, '0')
    }
    /** "740948" → "740 948", as on the PC's screen. */
    fun grouped(code: String): String = if (code.length == 6) code.substring(0, 3) + " " + code.substring(3) else code

    fun payloadKey(shared: ByteArray, sid: String): ByteArray = hkdf256(shared, utf8(sid), utf8(PAYLOAD_INFO), 32)
    fun payloadAad(sid: String): ByteArray = utf8("bflink-payload\n$sid")

    /** b64u(nonce(12) ‖ ChaCha20-Poly1305(k_p, nonce, payload, aad)) → payload; throws [Bfs3Exception] when it does not authenticate. */
    fun openPayload(shared: ByteArray, sid: String, sealedB64u: String): ByteArray {
        val raw = Bfs3.unb64u(sealedB64u)
        if (raw.size < 12 + 16) throw Bfs3Exception("sealed payload too short")
        return Bfs3.aead(false, payloadKey(shared, sid), raw.copyOfRange(0, 12), payloadAad(sid), raw.copyOfRange(12, raw.size))
    }
    /** The PC's side (and the tests'). */
    fun sealPayload(shared: ByteArray, sid: String, nonce: ByteArray, payload: ByteArray): String =
        Bfs3.b64u(nonce + Bfs3.aead(true, payloadKey(shared, sid), nonce, payloadAad(sid), payload))

    // ── parsing ─────────────────────────────────────────────────────────────
    private fun JsonObject.str(k: String): String? = (this[k] as? JsonPrimitive)?.takeIf { it !is JsonNull && it.isString }?.contentOrNull
    private fun JsonObject.strList(k: String): List<String> = (this[k] as? JsonArray)?.mapNotNull { (it as? JsonPrimitive)?.takeIf { p -> p.isString }?.content?.trim()?.takeIf(String::isNotEmpty) } ?: emptyList()

    /** `bflink:` + b64u(JSON {...}). Throws [Bfs3Exception] with a user-readable message. */
    fun parseQr(text: String): LinkQr {
        val t = text.trim().replace(Regex("\\s+"), "")
        if (!t.startsWith(PREFIX, ignoreCase = true)) throw Bfs3Exception("not a BeaconFix link QR")
        val o = try { lenient.parseToJsonElement(String(Bfs3.unb64u(t.substring(PREFIX.length)), Charsets.UTF_8)).jsonObject }
                catch (e: Bfs3Exception) { throw Bfs3Exception("the link QR is damaged") } catch (e: Exception) { throw Bfs3Exception("the link QR is damaged") }
        val v = (o["v"] as? JsonPrimitive)?.intOrNull ?: 1
        if (v != 1) throw Bfs3Exception("this link QR is from a newer BeaconFix (v$v) — update the app")
        val sid = o.str("sid")?.lowercase() ?: throw Bfs3Exception("the link QR has no session")
        if (!SID.matches(sid)) throw Bfs3Exception("the link QR's session id is not 16 hex characters")
        val pub = Bfs3.unb64u(o.str("pub") ?: throw Bfs3Exception("the link QR has no key"))
        if (pub.size != 32) throw Bfs3Exception("the link QR's key is not 32 bytes")
        val k = Bfs3.unb64u(o.str("k") ?: throw Bfs3Exception("the link QR has no secret"))
        if (k.size != 32) throw Bfs3Exception("the link QR's secret is not 32 bytes")
        val port = (o["port"] as? JsonPrimitive)?.intOrNull ?: throw Bfs3Exception("the link QR has no port")
        if (port !in 1..65535) throw Bfs3Exception("the link QR's port is invalid")
        val hosts = o.strList("hosts").filter { validHost(it) }.distinct()
        if (hosts.isEmpty()) throw Bfs3Exception("the link QR names no address for the PC")
        val hub = o.str("hub")?.trim()?.takeIf { it.startsWith(Bfs3.PREFIX, ignoreCase = true) }
        return LinkQr(sid, o.str("name")?.trim()?.take(80).orEmpty(), orderHosts(hosts), port, pub, k, (o["e"] as? JsonPrimitive)?.longOrNull ?: 0L, hub)
    }

    /** The decrypted payload; throws [Bfs3Exception] when it is not what the spec says. */
    fun parsePayload(bytes: ByteArray, fallbackPort: Int): LinkPayload {
        val o = try { lenient.parseToJsonElement(String(bytes, Charsets.UTF_8)).jsonObject } catch (e: Exception) { throw Bfs3Exception("the link payload is not JSON") }
        val token = o.str("token")?.takeIf { it.isNotBlank() } ?: throw Bfs3Exception("the link payload carries no token")
        val pc = o["pc"] as? JsonObject
        val lp = LinkPc(pc?.str("name").orEmpty(), pc?.str("id").orEmpty(), pc?.strList("hosts")?.filter { validHost(it) }.orEmpty(),
            (pc?.get("port") as? JsonPrimitive)?.intOrNull?.takeIf { it in 1..65535 } ?: fallbackPort)
        val hub = o.str("hub")?.trim()?.takeIf { it.startsWith(Bfs3.PREFIX, ignoreCase = true) }
        return LinkPayload(token, o.strList("scopes").ifEmpty { listOf("read", "control") }, lp, hub)
    }

    /** What the one scanner on the Link screen read: a link QR, a hub invite, or something else. */
    fun classify(text: String): Scanned {
        val t = text.trim()
        return try {
            when {
                t.startsWith(PREFIX, ignoreCase = true) -> Scanned.Link(parseQr(t))
                t.startsWith(Bfs3.PREFIX, ignoreCase = true) -> Scanned.HubInvite(Bfs3.parseInvite(t), t.replace(Regex("\\s+"), ""))
                else -> Scanned.Bad("That QR is not a BeaconFix link (it should start with bflink: or bfs3:)")
            }
        } catch (e: Bfs3Exception) { Scanned.Bad(e.message ?: "unreadable QR") }
    }

    // ── addresses ───────────────────────────────────────────────────────────
    private val HOST = Regex("""[A-Za-z0-9][A-Za-z0-9.\-]{0,252}|[0-9A-Fa-f:]{2,45}""")
    private val IPV4 = Regex("""\d{1,3}(\.\d{1,3}){3}""")
    fun isIpv4(h: String) = IPV4.matches(h) && h.split('.').all { (it.toIntOrNull() ?: 256) <= 255 }
    /** A host we can dial: an IPv4, a name, or a global IPv6 (link-local fe80:: needs a scope id URLs cannot carry reliably). */
    fun validHost(h: String): Boolean = HOST.matches(h) && !h.lowercase().startsWith("fe80:") && h != "0.0.0.0" && !h.startsWith("127.")
    /** IPv4 first (private ranges first), then names (.local), then IPv6. */
    fun orderHosts(hosts: Collection<String>): List<String> = hosts.distinct().sortedBy { h ->
        when {
            isIpv4(h) && (h.startsWith("192.168.") || h.startsWith("10.") || Regex("""172\.(1[6-9]|2\d|3[01])\..*""").matches(h)) -> 0
            isIpv4(h) -> if (h.startsWith("100.") || h.startsWith("169.254.")) 2 else 1
            !h.contains(':') -> 3
            else -> 4
        }
    }
}
