package org.sworrl.beaconfix.identity

import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.api.ApiFactory
import org.sworrl.beaconfix.data.db.DesktopEntity
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import javax.inject.Inject
import javax.inject.Singleton

sealed class LoginResult {
    data class Ok(val desktop: DesktopEntity, val identityName: String) : LoginResult()
    data class UnknownIdentity(val message: String) : LoginResult()   // 403: the desktop lists us as pending; link from its UI (or ours)
    data class Failed(val message: String) : LoginResult()
}

/** The network side of identities: challenge-auth login, LAN import by code, sending link statements. */
@Singleton
class IdentityRepository @Inject constructor(private val store: IdentityStore, private val desktops: DesktopStore) {

    /** Challenge/auth against a desktop that advertises "identity": no pairing prompt when we are its identity or linked to it. */
    suspend fun login(d: DesktopEntity): LoginResult {
        val rec = store.currentNow() ?: return LoginResult.Failed("no identity on this phone")
        val api = desktops.api(d)
        return try {
            val ch = api.identityChallenge().body() ?: return LoginResult.Failed("desktop gave no challenge")
            val host = ch.host.ifEmpty { d.hostname.ifEmpty { d.host } }
            val sig = store.authSignature(host, ch.nonce) ?: return LoginResult.Failed("no identity key")
            val r = api.identityAuth(AuthBody(rec.id, rec.pub, AuthDevice(store.deviceName, "android"), ch.nonce, sig))
            when {
                r.isSuccessful && !r.body()?.token.isNullOrEmpty() -> {
                    val body = r.body()!!
                    desktops.saveToken(d.id, body.token)
                    val paired = d.copy(paired = true, scopes = body.scopes.joinToString(",").ifEmpty { "read,control" }, lastError = "", lastSeen = System.currentTimeMillis())
                    desktops.upsert(paired)
                    LoginResult.Ok(paired, body.identity?.get("name")?.toString()?.trim('"') ?: "")
                }
                r.code() == 403 -> LoginResult.UnknownIdentity("that desktop does not know this identity yet — link the two (Settings → Identity → Link) or pair with a code instead")
                else -> LoginResult.Failed("login failed (HTTP ${r.code()})")
            }
        } catch (e: Exception) { LoginResult.Failed(e.message ?: "login failed") }
    }

    /** Fetch an export bundle another BeaconFix on this network is holding under a one-time 6-digit code. */
    suspend fun fetchBundle(host: String, port: Int, tls: Boolean, code: String): String {
        val r = ApiFactory.create(host, port, tls).identityExport(code.trim())
        if (r.code() == 404) error("no such code (expired, used, or mistyped)")
        if (!r.isSuccessful) error("the other app answered HTTP ${r.code()}")
        val raw = r.body()?.string()?.trim() ?: error("empty reply")
        // the desktop answers {"bundle":"BFID1:…"}; a bare BFID1 text or the bundle JSON itself are accepted too
        val text = if (raw.startsWith("{") && raw.contains("\"bundle\"")) (kotlinx.serialization.json.Json.parseToJsonElement(raw).jsonObject["bundle"]?.jsonPrimitive?.content ?: error("no bundle in reply")) else raw
        IdentityOps.parseBundle(text); return text
    }

    /** Send our half of a link to a desktop; it verifies, co-signs, and answers with the completed statement, which we store. */
    suspend fun sendLink(d: DesktopEntity, half: LinkStatement, theirPub: ByteArray): Result<LinkStatement> = runCatching {
        val r = desktops.api(d).identityLink(half)
        if (!r.isSuccessful) error("the desktop refused the link (HTTP ${r.code()}): ${r.errorBody()?.string()?.take(120)}")
        val full = r.body()?.statement ?: error("no statement returned")
        val rec = store.currentNow() ?: error("no identity")
        val ours = Crypto.unb64(rec.pub)
        if (!store.addLink(full, if (full.a == rec.id) ours else theirPub, if (full.a == rec.id) theirPub else ours)) error("the returned statement did not verify")
        full
    }

    /** Public identity of a desktop (no auth). */
    suspend fun remoteIdentity(d: DesktopEntity): IdentityPublic? = runCatching { desktops.api(d).identity().body() }.getOrNull()
}
