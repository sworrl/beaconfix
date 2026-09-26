# Security

## The LAN API

Threat model: another host on the same local network (a guest, a compromised IoT device, a
neighbour on an open segment) tries to read your position, or to make BeaconFix do something.

| control | what it stops |
|---|---|
| private/link-local peers only, decided before the body is read | anything routed in from outside |
| known-device allowlist (`apiKnownOnly`, default on) | tokens presented from an address that is not one of your devices; the request fails with `403` even with a valid token |
| bearer tokens: 256-bit random, stored as SHA-256 only, constant-time compare, shown once | token theft from the config file; timing side channels |
| scopes `read` / `control` | a read-only device triggering refreshes, prefetches, home-list changes or database writes |
| pairing: only while a window is open (10 min), at most 5 pending, 4-digit code shown on both ends, expiry, manual approval for unknown devices | silent pairing |
| 60 req/min per address (401/403 count double), 32 connections, 8 streams, 4 KB bodies | brute force, flooding |
| access log (last 100) in the Devices tab | invisible probing |
| optional TLS (`~/.config/sworrl/beaconfix.crt` + `beaconfix.key`) | passive sniffing of tokens on the LAN |

What it does not do: authenticate the *server* to the device without TLS (a rogue host could
answer to a device that is not pinned to a certificate), or resist an attacker who already has
root on this machine (the token hashes and the decrypted database are readable by that user).

Turn it off entirely with `apiEnabled=false`; pairing is closed by default.

## The map database

`~/.local/state/beaconfix/beaconfix.db` is not a SQLite file. It is an AES-256-GCM blob:
header `BFDB\1`, a 12-byte random nonce, the ciphertext and a 16-byte authentication tag. The
working copy is decrypted into `$XDG_RUNTIME_DIR/beaconfix/live.db` (tmpfs, mode 0600, gone at
logout) and re-encrypted a few seconds after each change and on exit.

Key handling: the 256-bit key is stored in **KWallet** (folder `BeaconFix`, entry `mapdb-key`)
when a wallet daemon is already running on the session bus and the wallet can be opened;
otherwise in `~/.config/sworrl/beaconfix.key` (hex, mode 0600). A key file, when present, is
used in preference to the wallet. BeaconFix never starts a wallet daemon itself.

An attacker with the blob but no key gets nothing: the ciphertext is indistinguishable from
random, and the tag rejects any modification. An attacker with your user session gets
everything, as with any local data. Back up the `.db` **together with** the key: without the
key the database cannot be opened, and the tray then runs without persistence and says so.

Standalone processes (`beaconfix --once`, `--json` without the tray) open a private read-only
decrypted copy and remove it on exit.

## Wi-Fi classification

Every beacon carries NetworkManager's access-point `Flags` (privacy bit), `WpaFlags` and
`RsnFlags` (cipher and key-management bitmasks), `Mode` and `MaxBitrate`. The scanner derives a
`security` string; the widget's `security.js` turns it into a grade and an explanation. Where
the beacon is one of your own networks and the seed file carries the controller's WLAN
settings (`wlans[]`), the PMF, WPA3-transition, hidden-SSID and guest-isolation facts are
used too; otherwise PMF is reported as unknown, because NetworkManager does not expose the RSN
capabilities bits.

| `security` | grade | title | why |
|---|---|---|---|
| `open` | insecure | No RSN/WPA IE, privacy bit clear | Every 802.11 data frame is plaintext at layer 2. Anyone in radio range can passively capture DHCP, DNS, ARP and unencrypted application traffic, inject frames into existing sessions, and stand up an evil twin with the same SSID. There is no PMK, so no PTK is ever derived; only TLS above it protects anything. |
| `owe` | acceptable | OWE (Enhanced Open) | Per-association ECDH gives confidentiality without authentication: a passive sniffer sees CCMP ciphertext, but nothing proves the AP is who it says, so an active attacker can still impersonate it. Better than open, weaker than a PSK you actually know. |
| `wep` | insecure | WEP (RC4, 24-bit IV, CRC-32 ICV) | Key recovery is statistical and fast: PTW needs roughly 40–85k unique IVs for a 104-bit key, minutes with aircrack-ng on a busy AP, and ARP replay manufactures IVs on a quiet one. The CRC-32 ICV is linear, so chop-chop and fragmentation attacks forge frames without the key at all. Deprecated by 802.11-2012. |
| `wpa1` | insecure | WPA1 / TKIP only | TKIP is RC4 with per-packet key mixing and the 64-bit Michael MIC. Beck–Tews and Ohigashi–Morii recover the MIC key and inject short frames (ARP/DNS), and a captured 4-way handshake still allows an offline PSK attack. Removed from 802.11-2012; modern clients refuse to associate. |
| `wpa2-tkip` | insecure | WPA2 with a TKIP cipher | The pairwise or group cipher is TKIP, so at least broadcast traffic is RC4 under a GTK every station shares, and TKIP's MIC-key recovery applies. This is a mixed-mode AP kept for a legacy client; CCMP-only is the fix. |
| `wpa2` | weak | WPA2-PSK (CCMP), no SAE | AES-CCMP itself is sound, but the PMK is PBKDF2-SHA1(passphrase, SSID, 4096 rounds): one captured 4-way handshake, or a PMKID from the first EAPOL frame, is enough for an offline dictionary or mask attack (hashcat -m 22000). No forward secrecy: a recovered PSK decrypts every past capture. |
| `wpa2-eap` | acceptable | WPA2-Enterprise (802.1X) | Per-station PMKs from the RADIUS exchange, so no shared secret to crack. Strength hinges on the supplicant validating the server certificate: PEAP/MSCHAPv2 without validation hands the challenge/response to an evil-twin RADIUS (hostapd-wpe) for offline NetNTLM-style cracking. |
| `wpa2/3` | acceptable | WPA3 transition mode (SAE + PSK on one BSS) | SAE and PSK share the BSS. A client that does not honour the Transition-Disable indication can be coaxed to WPA2-PSK, reopening the offline dictionary attack (the Dragonblood downgrade). Fine as a bridge, not an end state. |
| `wpa3` | strong | WPA3-SAE (Dragonfly PAKE) | Password-authenticated key exchange: an attacker gets one online guess per handshake, no offline dictionary attack, forward secrecy per association, and PMF is mandatory so deauth spoofing fails. Residual: Dragonblood timing and cache side channels on early implementations. |
| `wpa3-eap192` | strong | WPA3-Enterprise 192-bit | Suite-B-192: GCMP-256, HMAC-SHA384, ECDH/ECDSA P-384. About as good as consumer Wi-Fi gets. |

Additional notes, added when they apply:

| note | grade effect | why |
|---|---|---|
| PMF required (802.11w) | — | Management frames carry a MIC, so spoofed deauth/disassoc frames are dropped. That removes the cheap forced-reconnect trick used to harvest handshakes. |
| PMF optional | acceptable → weak | Clients that do not negotiate 802.11w stay unprotected: a spoofed deauth kicks them off, the reconnect handshake is captured, or they are shepherded onto an evil twin. Set MFPR (required) once the legacy clients are gone. |
| PMF off | acceptable → weak | Deauthentication and disassociation frames are unauthenticated. Anyone can kick any client with a one-line aireplay-ng deauth, capture the reconnect 4-way handshake, or push the client to an evil twin. 802.11w (MFPR) fixes it. |
| PMF status unknown | — | NetworkManager does not expose the RSN capabilities MFPC/MFPR bits, so assume management frames are unauthenticated unless the AP is WPA3 (where PMF is mandatory). |
| Controller: WPA3 transition enabled | — | The WLAN advertises SAE alongside PSK; see the downgrade note above. |
| Hidden SSID | — | Every client probes for the name wherever it goes, leaking the SSID (a trackable fingerprint) to any AP it passes; it stops nobody with a sniffer. |
| Guest network without L2 isolation | — | Guests can talk to each other at layer 2 (ARP spoofing, mDNS discovery, lateral movement). |
| IBSS / ad-hoc | → weak | No AP; peers share one GTK and there is no centralised key management. Rare on purpose. |
| 802.11b rates | — | 1–11 Mbit/s DSSS: a legacy radio that drags the whole channel's airtime down; usually old IoT with old firmware. |

The tray emits an `ap_insecure` event once per BSSID per 24 hours for `open`, `wep`, `wpa1`
and `wpa2-tkip`, marks them `insecure: true` in `aps[]`, and reports counts in
`securitySummary {open, wep, wpa1, tkip, wpa3, total}`.

## Reporting a vulnerability

Open a private security advisory on GitHub (Security → Report a vulnerability) rather than a
public issue.
