package org.sworrl.beaconfix.nav

/**
 * A request to pair from outside the app (`beaconfix://pair?host=&port=`, or the `pair_host` / `pair_port` extras any
 * app can send), made safe to route (pure, unit-tested). The host must look like a host name or an IP address (IPv6 in
 * brackets or bare) and is percent-encoded into the route, so a crafted value can neither add route parameters (an
 * `&auto=true` of its own) nor put a sentence on the confirmation card. [auto] is decided by the caller: only adb
 * automation pairs without asking.
 */
object PairRequest {
    private val HOST = Regex("""[A-Za-z0-9\[][A-Za-z0-9.\-:%\[\]]{0,252}""")

    /** The trimmed host, or null when it does not look like one. */
    fun host(raw: String?): String? = raw?.trim()?.takeIf { HOST.matches(it) }

    /** The Pair screen route for [rawHost]:[port], or null for an unusable request. */
    fun route(rawHost: String?, port: Int, auto: Boolean, encode: (String) -> String): String? {
        val h = host(rawHost) ?: return null
        if (port !in 1..65535) return null
        return "pair?host=${encode(h)}&port=$port&auto=$auto"
    }
}
