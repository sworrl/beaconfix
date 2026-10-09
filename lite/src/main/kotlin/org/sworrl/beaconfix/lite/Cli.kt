package org.sworrl.beaconfix.lite

import java.io.File

/**
 * Command line, for trying the locator on a desktop or straight on a device (dexed, run with app_process):
 *
 *     adb shell cmd wifi list-scan-results | java -cp beaconfix-lite.jar org.sworrl.beaconfix.lite.Cli <cache-dir> [--offline] [--travelling a,b]
 *
 * Reads Android's `cmd wifi list-scan-results` table, or plain lines of "bssid rssi [freq] [ssid…]", from stdin.
 * Prints the fix as JSON (or {"fix":null}) and how long the locate took.
 */
object Cli {
    @JvmStatic
    fun main(args: Array<String>) {
        if (args.isEmpty()) { System.err.println("usage: Cli <cache-dir> [--offline] [--travelling ssid,ssid] [--repeat n] [--import file] < scan"); return }
        val dir = File(args[0])
        val offline = "--offline" in args
        val travelling = args.indexOf("--travelling").takeIf { it >= 0 && it + 1 < args.size }?.let { args[it + 1].split(',') } ?: emptyList()
        val repeat = args.indexOf("--repeat").takeIf { it >= 0 && it + 1 < args.size }?.let { args[it + 1].toInt() } ?: 1
        // SSIDs are raw bytes (emoji, Latin-1, garbage): never let one stop the read
        val dec = Charsets.UTF_8.newDecoder().onMalformedInput(java.nio.charset.CodingErrorAction.REPLACE).onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPLACE)
        val scan = parse(java.io.InputStreamReader(System.`in`, dec).readLines())
        val loc = LiteLocator(dir)
        // --import file: lines of "bssid lat lon acc" (a BeaconFix desktop's placements, say)
        args.indexOf("--import").takeIf { it >= 0 && it + 1 < args.size }?.let { i ->
            val m = HashMap<String, Triple<Double, Double, Double>>()
            for (l in File(args[i + 1]).readLines()) { val q = l.trim().split(Regex("\\s+")); if (q.size >= 4) m[q[0]] = Triple(q[1].toDouble(), q[2].toDouble(), q[3].toDouble()) }
            System.err.println("imported ${loc.import(m)} placements")
        }
        for (i in 0 until repeat) {
            val t0 = System.nanoTime()
            val fix = loc.locate(scan, travelling, allowNetwork = !offline)
            val ms = (System.nanoTime() - t0) / 1e6
            println("{\"run\":${i + 1},\"heard\":${scan.size},\"ms\":${"%.1f".format(java.util.Locale.US, ms)},\"cache\":${loc.cache.size},\"fix\":${fix?.toJson() ?: "null"}}")
        }
    }

    private val MAC = Regex("^([0-9A-Fa-f]{1,2}(?::[0-9A-Fa-f]{1,2}){5})$")

    /** Either table: `cmd wifi list-scan-results` (BSSID, Frequency, RSSI, Age, SSID, Flags) or "bssid rssi [freq] [ssid]". */
    fun parse(lines: List<String>): List<Heard> {
        val out = ArrayList<Heard>()
        for (raw in lines) {
            val p = raw.trim().split(Regex("\\s+"))
            if (p.size < 2 || !MAC.matches(p[0])) continue
            // RSSI may carry per-chain levels: "-84(0:-84)"
            val rssi2 = p.getOrNull(2)?.substringBefore('(')?.toIntOrNull()
            if (p.size >= 4 && p[1].toIntOrNull()?.let { it > 2000 } == true && rssi2 != null) {
                // cmd wifi: BSSID Frequency RSSI Age(sec) SSID… Flags; the SSID may have spaces, the flags start with '['
                val ssidEnd = p.indexOfFirst { it.startsWith("[") }.let { if (it < 0) p.size else it }
                val ssid = if (ssidEnd > 4) p.subList(4, ssidEnd).joinToString(" ") else ""
                out += Heard(p[0], rssi2, p[1].toInt(), ssid)
            } else {
                val rssi = p[1].toIntOrNull() ?: continue
                val freq = p.getOrNull(2)?.toIntOrNull() ?: 0
                out += Heard(p[0], rssi, freq, if (p.size > 3) p.subList(3, p.size).joinToString(" ") else "")
            }
        }
        return out
    }
}
