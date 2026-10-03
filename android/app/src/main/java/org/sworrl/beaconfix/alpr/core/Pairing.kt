package org.sworrl.beaconfix.alpr.core

import java.net.URI
import java.net.URLDecoder

/** What a FalconEyez pairing QR carries: `falconeyez://pair?u=<url1>,<url2>&t=<token>&n=<server name>`. */
data class PairingPayload(val urls: List<String>, val token: String, val name: String)

object Pairing {
    const val SCHEME = "falconeyez"

    /** Parses the QR text; null when it isn't a FalconEyez pairing payload. URLs are normalised (no trailing slash). */
    fun parse(text: String): PairingPayload? {
        val t = text.trim()
        if (!t.startsWith("$SCHEME://pair", ignoreCase = true)) return null
        val q = t.substringAfter('?', "")
        if (q.isEmpty()) return null
        val params = q.split('&').mapNotNull { kv ->
            val k = kv.substringBefore('='); val v = kv.substringAfter('=', "")
            if (k.isEmpty()) null else k to decode(v)
        }.toMap()
        val urls = (params["u"] ?: "").split(',').map { decode(it).trim() }.mapNotNull { normalizeUrl(it) }.distinct()
        val token = params["t"]?.trim().orEmpty()
        if (urls.isEmpty() || token.isEmpty()) return null
        return PairingPayload(urls, token, params["n"]?.trim().orEmpty().ifEmpty { "FalconEyez" })
    }

    /** "192.0.2.10:8900" → "http://192.0.2.10:8900"; a bare IPv6 literal gets brackets. Null when unusable. */
    fun normalizeUrl(raw: String): String? {
        var s = raw.trim().trimEnd('/')
        if (s.isEmpty()) return null
        if (!s.contains("://")) {
            // bare IPv6 with port is ambiguous; an unbracketed IPv6 literal is taken without a port
            s = if (s.count { it == ':' } >= 2 && !s.startsWith("[")) "http://[$s]" else "http://$s"
        }
        val u = runCatching { URI(s) }.getOrNull() ?: return null
        if (u.scheme?.lowercase() !in setOf("http", "https") || u.host.isNullOrBlank()) return null
        return s
    }

    /** Percent-decoding only: a '+' stays a '+' (tokens may be base64). */
    private fun decode(s: String): String = runCatching { URLDecoder.decode(s.replace("+", "%2B"), "UTF-8") }.getOrDefault(s)
}
