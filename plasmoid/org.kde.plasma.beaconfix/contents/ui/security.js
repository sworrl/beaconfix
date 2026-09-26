.pragma library
// Wi-Fi security classification for the beacon map, from NetworkManager's
// NM80211ApSecurityFlags (WpaFlags / RsnFlags), the AP flags (privacy bit) and,
// for our own UniFi SSIDs, the controller's WLAN settings (PMF, WPA3, transition).
var PAIR_WEP40 = 0x1, PAIR_WEP104 = 0x2, PAIR_TKIP = 0x4, PAIR_CCMP = 0x8
var GROUP_WEP40 = 0x10, GROUP_WEP104 = 0x20, GROUP_TKIP = 0x40, GROUP_CCMP = 0x80
var KM_PSK = 0x100, KM_8021X = 0x200, KM_SAE = 0x400, KM_OWE = 0x800, KM_OWE_TM = 0x1000, KM_EAP_B192 = 0x2000

var GRADES = {
    critical: { rank: 4, color: "#ff4d4d", glyph: "☠", label: "insecure" },
    weak:     { rank: 3, color: "#ff9f43", glyph: "⚠", label: "weak" },
    ok:       { rank: 2, color: "#ffd166", glyph: "◐", label: "acceptable" },
    strong:   { rank: 1, color: "#6cff8a", glyph: "🛡", label: "strong" },
    unknown:  { rank: 0, color: "#8a93a6", glyph: "?", label: "unknown" }
}

function issue(title, nerd, severity) { return { title: title, nerd: nerd, severity: severity } }

// Derive a security string when the scanner did not provide one
function derive(ap) {
    if (ap.security) return ap.security
    var priv = !!(ap.secFlags & 0x1), wpa = ap.wpaFlags || 0, rsn = ap.rsnFlags || 0
    if (ap.wpaFlags === undefined && ap.rsnFlags === undefined && ap.secFlags === undefined) return ""
    if (!priv && !wpa && !rsn) return "open"
    if (rsn & KM_OWE || rsn & KM_OWE_TM) return "owe"
    if (priv && !wpa && !rsn) return "wep"
    if (rsn & KM_EAP_B192) return "wpa3-eap192"
    if ((rsn & KM_SAE) && (rsn & KM_PSK)) return "wpa2/3"
    if (rsn & KM_SAE) return "wpa3"
    if (rsn & KM_8021X) return "wpa2-eap"
    if (rsn && ((rsn & PAIR_TKIP) || (rsn & GROUP_TKIP))) return "wpa2-tkip"
    if (rsn) return "wpa2"
    if (wpa) return "wpa1"
    return ""
}

function classify(ap, wlan) {
    var sec = derive(ap), issues = [], grade = "unknown"
    var rsn = ap.rsnFlags || 0
    var pmf = wlan ? (wlan.pmf_mode || "") : ""            // required | optional | disabled | "" (unknown)
    switch (sec) {
    case "open":
        grade = "critical"
        issues.push(issue("No RSN/WPA IE, privacy bit clear",
            "Every 802.11 data frame is plaintext at layer 2. Anyone in radio range can passively capture DHCP, DNS, ARP and unencrypted application traffic, inject frames into existing sessions, and stand up an evil twin with the same SSID. There is no PMK, so no PTK is ever derived; only TLS above it protects anything.", "critical"))
        break
    case "owe":
        grade = "ok"
        issues.push(issue("OWE (Enhanced Open)",
            "Per-association ECDH gives confidentiality without authentication: a passive sniffer sees CCMP ciphertext, but nothing proves the AP is who it says, so an active attacker can still impersonate it. Better than open, weaker than a PSK you actually know.", "info"))
        break
    case "wep":
        grade = "critical"
        issues.push(issue("WEP (RC4, 24-bit IV, CRC-32 ICV)",
            "Key recovery is statistical and fast: PTW needs roughly 40–85k unique IVs for a 104-bit key, minutes with aircrack-ng on a busy AP, and ARP replay manufactures IVs on a quiet one. The CRC-32 ICV is linear, so chop-chop and fragmentation attacks forge frames without the key at all. Deprecated by 802.11-2012.", "critical"))
        break
    case "wpa1":
        grade = "critical"
        issues.push(issue("WPA1 / TKIP only",
            "TKIP is RC4 with per-packet key mixing and the 64-bit Michael MIC. Beck–Tews and Ohigashi–Morii recover the MIC key and inject short frames (ARP/DNS), and a captured 4-way handshake still allows an offline PSK attack. Removed from 802.11-2012; modern clients refuse to associate.", "critical"))
        break
    case "wpa2-tkip":
        grade = "critical"
        issues.push(issue("WPA2 with a TKIP cipher",
            "The pairwise or group cipher is TKIP, so at least broadcast traffic is RC4 under a GTK every station shares, and TKIP's MIC-key recovery applies. This is a mixed-mode AP kept for a legacy client; CCMP-only is the fix.", "critical"))
        break
    case "wpa2":
        grade = "weak"
        issues.push(issue("WPA2-PSK (CCMP), no SAE",
            "AES-CCMP itself is sound, but the PMK is PBKDF2-SHA1(passphrase, SSID, 4096 rounds): one captured 4-way handshake, or a PMKID from the first EAPOL frame, is enough for an offline dictionary or mask attack (hashcat -m 22000). No forward secrecy: a recovered PSK decrypts every past capture.", "weak"))
        break
    case "wpa2-eap":
        grade = "ok"
        issues.push(issue("WPA2-Enterprise (802.1X)",
            "Per-station PMKs from the RADIUS exchange, so no shared secret to crack. Strength hinges on the supplicant validating the server certificate: PEAP/MSCHAPv2 without validation hands the challenge/response to an evil-twin RADIUS (hostapd-wpe) for offline NetNTLM-style cracking.", "info"))
        break
    case "wpa2/3":
        grade = "ok"
        issues.push(issue("WPA3 transition mode (SAE + PSK on one BSS)",
            "SAE and PSK share the BSS. A client that does not honour the Transition-Disable indication can be coaxed to WPA2-PSK, reopening the offline dictionary attack (the Dragonblood downgrade). Fine as a bridge, not an end state.", "weak"))
        break
    case "wpa3":
        grade = "strong"
        issues.push(issue("WPA3-SAE (Dragonfly PAKE)",
            "Password-authenticated key exchange: an attacker gets one online guess per handshake, no offline dictionary attack, forward secrecy per association, and PMF is mandatory so deauth spoofing fails. Residual: Dragonblood timing and cache side channels on early implementations.", "info"))
        break
    case "wpa3-eap192":
        grade = "strong"
        issues.push(issue("WPA3-Enterprise 192-bit", "Suite-B-192: GCMP-256, HMAC-SHA384, ECDH/ECDSA P-384. About as good as consumer Wi-Fi gets.", "info"))
        break
    default:
        grade = "unknown"
        issues.push(issue("Security not reported", "The scanner has not sent the RSN/WPA flags for this BSS yet (needs BeaconFix 3.3+). Nothing can be said about it.", "info"))
    }
    // PMF (802.11w): mandatory with SAE; otherwise from the controller if it is ours, else unknown
    if (sec === "wpa2" || sec === "wpa2-eap" || sec === "wpa2/3") {
        if (pmf === "required")
            issues.push(issue("PMF required (802.11w)", "Management frames carry a MIC, so spoofed deauth/disassoc frames are dropped. That removes the cheap forced-reconnect trick used to harvest handshakes.", "info"))
        else if (pmf === "optional") {
            issues.push(issue("PMF optional", "Clients that do not negotiate 802.11w stay unprotected: a spoofed deauth kicks them off, the reconnect handshake is captured, or they are shepherded onto an evil twin. Set MFPR (required) once the legacy clients are gone.", "weak")); if (grade === "ok") grade = "weak"
        } else if (pmf === "disabled") {
            issues.push(issue("PMF off", "Deauthentication and disassociation frames are unauthenticated. Anyone can kick any client with a one-line aireplay-ng deauth, capture the reconnect 4-way handshake, or push the client to an evil twin. 802.11w (MFPR) fixes it.", "weak")); if (grade === "ok") grade = "weak"
        } else
            issues.push(issue("PMF status unknown", "NetworkManager does not expose the RSN capabilities MFPC/MFPR bits, so assume management frames are unauthenticated unless the AP is WPA3 (where PMF is mandatory).", "info"))
    }
    if (wlan && wlan.wpa3_transition && sec !== "wpa2/3")
        issues.push(issue("Controller: WPA3 transition enabled", "The UniFi WLAN advertises SAE alongside PSK; see the downgrade note above.", "weak"))
    if (wlan && wlan.hide_ssid)
        issues.push(issue("Hidden SSID", "Every client probes for the name wherever it goes, leaking the SSID (a trackable fingerprint) to any AP it passes; it stops nobody with a sniffer.", "info"))
    if (wlan && wlan.is_guest && wlan.l2_isolation === false)
        issues.push(issue("Guest network without L2 isolation", "Guests can talk to each other at layer 2 (ARP spoofing, mDNS discovery, lateral movement).", "weak"))
    if (ap.adhoc) { issues.push(issue("IBSS / ad-hoc", "No AP; peers share one GTK and there is no centralised key management. Rare on purpose.", "weak")); if (grade === "ok" || grade === "strong") grade = "weak" }
    if (ap.maxKbps && ap.maxKbps <= 11000)
        issues.push(issue("802.11b rates", "1–11 Mbit/s DSSS: a legacy radio that drags the whole channel's airtime down; usually old IoT with old firmware.", "info"))
    var g = GRADES[grade]
    return { grade: grade, rank: g.rank, color: g.color, glyph: g.glyph, label: g.label, security: sec || "unknown", issues: issues }
}

function secName(sec) {
    return { open: "Open", owe: "OWE", wep: "WEP", wpa1: "WPA1/TKIP", "wpa2-tkip": "WPA2+TKIP", wpa2: "WPA2-PSK", "wpa2-eap": "WPA2-Ent", "wpa2/3": "WPA2/3", wpa3: "WPA3-SAE", "wpa3-eap192": "WPA3-192" }[sec] || (sec || "?")
}

function summary(aps, wlanFor) {
    var s = { critical: 0, weak: 0, ok: 0, strong: 0, unknown: 0, open: 0, wep: 0, wpa1: 0, tkip: 0, nopmf: 0, total: 0 }
    for (var i = 0; i < aps.length; i++) {
        var a = aps[i]; if (!a || a.kind === "none") continue
        var c = classify(a, wlanFor ? wlanFor(a) : null)
        s[c.grade]++; s.total++
        if (c.security === "open") s.open++
        if (c.security === "wep") s.wep++
        if (c.security === "wpa1") s.wpa1++
        if (c.security === "wpa2-tkip") s.tkip++
        for (var j = 0; j < c.issues.length; j++) if (c.issues[j].title === "PMF off") s.nopmf++
    }
    return s
}
function summaryText(s) {
    var parts = []
    if (s.open) parts.push(s.open + " open")
    if (s.wep) parts.push(s.wep + " WEP")
    if (s.wpa1) parts.push(s.wpa1 + " WPA1")
    if (s.tkip) parts.push(s.tkip + " TKIP")
    if (s.weak) parts.push(s.weak + " weak")
    if (s.strong) parts.push(s.strong + " WPA3")
    return parts.length ? parts.join(" · ") : (s.total ? "no insecure beacons" : "")
}
