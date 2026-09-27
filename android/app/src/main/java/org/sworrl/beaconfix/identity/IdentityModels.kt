package org.sworrl.beaconfix.identity

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

/** JSON exactly as the shared spec: unknown keys are ignored, absent optionals are omitted. */
val identityJson = Json { ignoreUnknownKeys = true; explicitNulls = false; encodeDefaults = true }

@Serializable data class DeviceEntry(val name: String, val kind: String, val added: String, val pub: String? = null)

/** {"v":1,"a":idA,"b":idB,"ts":ISO,"sigA":b64,"sigB":b64} — valid only with both signatures over [LinkStatement.canon]. */
@Serializable data class LinkStatement(val v: Int = 1, val a: String, val b: String, val ts: String, val sigA: String = "", val sigB: String = "", val pubA: String? = null, val pubB: String? = null) {
    val canon: String get() = canonOf(a, b, ts)
    val complete: Boolean get() = sigA.isNotEmpty() && sigB.isNotEmpty()
    fun other(id: String) = if (a == id) b else a
    companion object { fun canonOf(idA: String, idB: String, ts: String) = "beaconfix-link|v1|" + minOf(idA, idB) + "|" + maxOf(idA, idB) + "|" + ts }
}

@Serializable data class IdentityRecord(
    val v: Int = 1, val id: String, val name: String, val created: String, val pub: String,
    val devices: List<DeviceEntry> = emptyList(), val links: List<LinkStatement> = emptyList(),
)

@Serializable data class Kdf(val name: String = "scrypt", val n: Int = Crypto.SCRYPT_N, val r: Int = Crypto.SCRYPT_R, val p: Int = Crypto.SCRYPT_P, val salt: String)
@Serializable data class Aead(val name: String = "aes-256-gcm", val nonce: String)
@Serializable data class ExportBundle(val v: Int = 1, val t: String = "beaconfix-identity", val kdf: Kdf, val aead: Aead, val ct: String)
@Serializable data class ExportPlain(val record: IdentityRecord, val seed: String)

/** The public half of an identity as another node publishes it (GET /api/v1/identity). */
@Serializable data class IdentityPublic(val id: String = "", val name: String = "", val pub: String = "", val devices: List<DeviceEntry> = emptyList(), val links: List<LinkStatement> = emptyList(), val linkedIds: List<String> = emptyList())

/** A "link payload" (QR / text `BFLNK1:`): {v, t, id, name, pub, ts} — no signature, because the canonical string needs both ids. */
@Serializable data class LinkOffer(val v: Int = 1, val t: String = "beaconfix-link", val id: String, val name: String = "", val pub: String, val ts: String)
/** `POST /api/v1/identity/link` answers with the completed statement (pubs included) and the resulting linked set. */
@Serializable data class LinkResponse(val statement: LinkStatement? = null, val linkedIds: List<String> = emptyList(), val error: String = "")

@Serializable data class Challenge(val nonce: String = "", val host: String = "", val expires: String = "")
@Serializable data class AuthDevice(val name: String, val kind: String = "android")
@Serializable data class AuthBody(val id: String, val pub: String, val device: AuthDevice, val nonce: String, val sig: String)
@Serializable data class AuthResult(val token: String = "", val scopes: List<String> = emptyList(), val identity: JsonObject? = null, val error: String = "")
@Serializable data class ExportCode(val code: String = "", val expires: String = "")

fun isoNow(): String = DateTimeFormatter.ISO_INSTANT.format(Instant.now().atOffset(ZoneOffset.UTC).withNano(0))
