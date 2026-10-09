package org.sworrl.beaconfix.lite

/**
 * Which access points may vote on where we are, and may be sent to a lookup service.
 *
 * Never: the opt-out convention (`_nomap` at the end of the SSID, `_optout` anywhere), anything named like it rides
 * with someone (phone hotspots, car and train Wi-Fi, an RV's Starlink, dashcams), and the networks the caller says
 * travel with the device (its own router, the network it's joined to). A beacon that moves with you tells you only
 * where it was mapped, which is where you WERE; and asking about it poisons the services for everyone else. The
 * name list is a first line only: [LiteLocator] also drops APs whose mapped position contradicts the others.
 */
object ApFilter {
    private val MOBILE = Regex(
        "(?i)(iphone|ipad|android.?ap|\\bandroid\\b|galaxy|pixel|oneplus|redmi|xiaomi|huawei|motorola|moto ?[egz]|coolpad|" +
            "hotspot|mifi|jetpack|franklin|inseego|netgear ?nighthawk ?m|verizon_mifi|mobile.?wifi|" +
            "tesla|uconnect|onstar|fordpass|sync ?\\d|\\bmy ?car\\b|car ?wifi|\\bford\\b|\\bchevy\\b|toyota|" +
            "gopro|dashcam|viofo|nextbase|" +
            "amtrak|united_?wifi|\\bdelta\\b|gogo|\\bsbb\\b|db ?ice|bus ?wifi|" +
            "starlink|dishy|rv ?link|winnebago|\\bvan\\b|camper|rv ?park ?guest|" +
            "direct-|chromecast|roku|firetv|nintendo)"
    )

    fun optedOut(ssid: String): Boolean = ssid.endsWith("_nomap", ignoreCase = true) || ssid.contains("_optout", ignoreCase = true)

    fun looksMobile(ssid: String): Boolean = ssid.isNotBlank() && MOBILE.containsMatchIn(ssid)

    /**
     * [travelling]: SSIDs (case-insensitive substrings) that move with this device. A blank SSID is fine: hidden
     * networks are ordinary fixed APs as often as not.
     *
     * No "locally administered MAC" filter: on real networks most mesh nodes, Starlink and ISP gateways advertise a
     * locally administered BSSID and are perfectly well mapped (the bit means "not the vendor's", not "random").
     */
    fun usable(h: Heard, travelling: Collection<String> = emptyList()): Boolean {
        if (h.key < 0 || h.rssi >= 0 || h.rssi < -100) return false
        val ssid = h.ssid
        if (optedOut(ssid) || looksMobile(ssid)) return false
        for (t in travelling) if (t.isNotBlank() && ssid.contains(t, ignoreCase = true)) return false
        return true
    }
}
