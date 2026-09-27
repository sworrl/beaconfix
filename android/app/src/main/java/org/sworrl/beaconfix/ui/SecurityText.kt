package org.sworrl.beaconfix.ui

/**
 * The security explanations, verbatim from the desktop widget's security.js, so the phone and the desktop say the same thing.
 * Keyed by the issue title.
 */
object SecurityText {
    data class Issue(val title: String, val nerd: String, val severity: String)
    val issues: List<Issue> = listOf(
        Issue("No RSN/WPA IE, privacy bit clear", "Every 802.11 data frame is plaintext at layer 2. Anyone in radio range can passively capture DHCP, DNS, ARP and unencrypted application traffic, inject frames into existing sessions, and stand up an evil twin with the same SSID. There is no PMK, so no PTK is ever derived; only TLS above it protects anything.", "critical"),
        Issue("OWE (Enhanced Open)", "Per-association ECDH gives confidentiality without authentication: a passive sniffer sees CCMP ciphertext, but nothing proves the AP is who it says, so an active attacker can still impersonate it. Better than open, weaker than a PSK you actually know.", "info"),
        Issue("WEP (RC4, 24-bit IV, CRC-32 ICV)", "Key recovery is statistical and fast: PTW needs roughly 40–85k unique IVs for a 104-bit key, minutes with aircrack-ng on a busy AP, and ARP replay manufactures IVs on a quiet one. The CRC-32 ICV is linear, so chop-chop and fragmentation attacks forge frames without the key at all. Deprecated by 802.11-2012.", "critical"),
        Issue("WPA1 / TKIP only", "TKIP is RC4 with per-packet key mixing and the 64-bit Michael MIC. Beck–Tews and Ohigashi–Morii recover the MIC key and inject short frames (ARP/DNS), and a captured 4-way handshake still allows an offline PSK attack. Removed from 802.11-2012; modern clients refuse to associate.", "critical"),
        Issue("WPA2 with a TKIP cipher", "The pairwise or group cipher is TKIP, so at least broadcast traffic is RC4 under a GTK every station shares, and TKIP's MIC-key recovery applies. This is a mixed-mode AP kept for a legacy client; CCMP-only is the fix.", "critical"),
        Issue("WPA2-PSK (CCMP), no SAE", "AES-CCMP itself is sound, but the PMK is PBKDF2-SHA1(passphrase, SSID, 4096 rounds): one captured 4-way handshake, or a PMKID from the first EAPOL frame, is enough for an offline dictionary or mask attack (hashcat -m 22000). No forward secrecy: a recovered PSK decrypts every past capture.", "weak"),
        Issue("WPA2-Enterprise (802.1X)", "Per-station PMKs from the RADIUS exchange, so no shared secret to crack. Strength hinges on the supplicant validating the server certificate: PEAP/MSCHAPv2 without validation hands the challenge/response to an evil-twin RADIUS (hostapd-wpe) for offline NetNTLM-style cracking.", "info"),
        Issue("WPA3 transition mode (SAE + PSK on one BSS)", "SAE and PSK share the BSS. A client that does not honour the Transition-Disable indication can be coaxed to WPA2-PSK, reopening the offline dictionary attack (the Dragonblood downgrade). Fine as a bridge, not an end state.", "weak"),
        Issue("WPA3-SAE (Dragonfly PAKE)", "Password-authenticated key exchange: an attacker gets one online guess per handshake, no offline dictionary attack, forward secrecy per association, and PMF is mandatory so deauth spoofing fails. Residual: Dragonblood timing and cache side channels on early implementations.", "info"),
        Issue("WPA3-Enterprise 192-bit", "Suite-B-192: GCMP-256, HMAC-SHA384, ECDH/ECDSA P-384. About as good as consumer Wi-Fi gets.", "info"),
        Issue("Security not reported", "The scanner has not sent the RSN/WPA flags for this BSS yet (needs BeaconFix 3.3+). Nothing can be said about it.", "info"),
        Issue("PMF required (802.11w)", "Management frames carry a MIC, so spoofed deauth/disassoc frames are dropped. That removes the cheap forced-reconnect trick used to harvest handshakes.", "info"),
        Issue("PMF optional", "Clients that do not negotiate 802.11w stay unprotected: a spoofed deauth kicks them off, the reconnect handshake is captured, or they are shepherded onto an evil twin. Set MFPR (required) once the legacy clients are gone.", "weak"),
        Issue("PMF off", "Deauthentication and disassociation frames are unauthenticated. Anyone can kick any client with a one-line aireplay-ng deauth, capture the reconnect 4-way handshake, or push the client to an evil twin. 802.11w (MFPR) fixes it.", "weak"),
        Issue("PMF status unknown", "NetworkManager does not expose the RSN capabilities MFPC/MFPR bits, so assume management frames are unauthenticated unless the AP is WPA3 (where PMF is mandatory).", "info"),
        Issue("Controller: WPA3 transition enabled", "The UniFi WLAN advertises SAE alongside PSK; see the downgrade note above.", "weak"),
        Issue("Hidden SSID", "Every client probes for the name wherever it goes, leaking the SSID (a trackable fingerprint) to any AP it passes; it stops nobody with a sniffer.", "info"),
        Issue("Guest network without L2 isolation", "Guests can talk to each other at layer 2 (ARP spoofing, mDNS discovery, lateral movement).", "weak"),
        Issue("IBSS / ad-hoc", "No AP; peers share one GTK and there is no centralised key management. Rare on purpose.", "weak"),
        Issue("802.11b rates", "1–11 Mbit/s DSSS: a legacy radio that drags the whole channel's airtime down; usually old IoT with old firmware.", "info"),
    )
    fun byTitle(title: String) = issues.firstOrNull { it.title == title }
    /** Issues for a security string (mirrors classify(): the first entry is the headline, PMF is unknown on a phone). */
    fun forSecurity(sec: String): List<Issue> {
        val head = when (sec) {
            "open" -> "No RSN/WPA IE, privacy bit clear"; "owe" -> "OWE (Enhanced Open)"; "wep" -> "WEP (RC4, 24-bit IV, CRC-32 ICV)"; "wpa1" -> "WPA1 / TKIP only"
            "wpa2-tkip" -> "WPA2 with a TKIP cipher"; "wpa2" -> "WPA2-PSK (CCMP), no SAE"; "wpa2-eap" -> "WPA2-Enterprise (802.1X)"; "wpa2/3" -> "WPA3 transition mode (SAE + PSK on one BSS)"
            "wpa3" -> "WPA3-SAE (Dragonfly PAKE)"; "wpa3-eap192" -> "WPA3-Enterprise 192-bit"; else -> "Security not reported"
        }
        val out = ArrayList<Issue>(); byTitle(head)?.let { out += it }
        if (sec == "wpa2" || sec == "wpa2-eap" || sec == "wpa2/3") byTitle("PMF status unknown")?.let { out += it }
        return out
    }
    fun grade(sec: String): String = when (sec) { "open", "wep", "wpa1", "wpa2-tkip" -> "critical"; "wpa2" -> "weak"; "wpa3", "wpa3-eap192" -> "strong"; "" -> "unknown"; else -> "ok" }
}
