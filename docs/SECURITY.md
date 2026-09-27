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
| pairing v2: only while a window is open (10 min), at most 5 pending, picture match (three rows, one real) or the 4-digit code, proximity verdict, expiry, manual approval for unknown devices | silent pairing, pairing from another room, a guess at the code |
| 60 req/min per address (401/403 count double), 32 connections, 8 streams, 4 KB bodies (1 MB `/db/sync`, 256 KB `/db/observations`, 200 MB `/db/import` streamed to disk) | brute force, flooding, memory exhaustion |
| identity links are co-signed only for an offer this BeaconFix displayed (single use, 10 min) | a LAN host minting an identity, linking it to yours and signing in for control |
| access log (last 100) in the Devices tab | invisible probing |
| optional TLS (`~/.config/sworrl/beaconfix.crt` + `beaconfix.key`) | passive sniffing of tokens on the LAN |

What it does not do: authenticate the *server* to the device without TLS (a rogue host could
answer to a device that is not pinned to a certificate), or resist an attacker who already has
root on this machine (the token hashes and the decrypted database are readable by that user).

Turn it off entirely with `apiEnabled=false`; pairing is closed by default.

### Pairing v2: pictures and proximity

The 4-digit code of earlier versions proves that the person approving can read the device's
screen — but a 1-in-10 000 guess, or a shoulder-surfed code, was enough. 3.6 adds two things:

- **Short-authentication-string pictures.** Both ends make an ephemeral X25519 key, exchange the
  public halves in the pairing request/response and derive `HKDF-SHA256(shared, info =
  "beaconfix-pair-sas-v1|<request id>")`. Bytes 0, 2 and 4 of that pick three of 48 fixed
  pictures. The device shows its three; the desktop shows *three rows* (the real one and two
  decoys that never repeat a picture in the same position) and the user taps the matching row.
  A wrong tap denies the request and logs an `error` event. An attacker who only sees the
  desktop learns nothing (two of the rows are wrong); one who only sees the device cannot tap.
  The secret never leaves either process and is forgotten with the request.
- **Proximity.** The request carries the beacons the device hears and its fix. The desktop
  compares them against everything it hears: with Δ = their dBm − our dBm over the shared
  beacons, the median Δ is the two radios' gain offset and the median absolute residual around it
  says whether they see the same room (≤ 4 dB with ≥ 4 shared → *adjacent*, ≤ 7 dB → *room*).
  Home and travelling networks are included on purpose: they are the strongest evidence of being in
  the same place. `apiPairProximity=required` (default) refuses pictures and auto-approval unless
  the verdict is adjacent / room / near; overriding it needs the word "pair" typed. Proximity
  is evidence, not proof (RSSI can be replayed), which is why it gates but never replaces the
  picture match.
- **Known devices** are still auto-approved (read scope unless the known list grants more), but
  only when they are near — and the dialog says so and offers *Allow control too*.

### Identity links (fixed in 3.6.0)

`POST /api/v1/identity/link` is unauthenticated by design (the statement carries its own
signatures). Before 3.6.0, `acceptLink` co-signed **any** statement the other party had signed —
so any host on the LAN could create an identity, sign a statement linking it to yours, get it
co-signed, and then obtain a read + control token through identity sign-in, bypassing pairing and
proximity entirely. Now every link starts with a link QR (or `--identity-link-qr`, which asks the
running tray) whose `ts` carries nine random fractional digits; the tray remembers its offers for
10 minutes, and a statement is co-signed only if its `ts` is one of them (then forgotten). A forged
statement gets `403 this link was not started from a link QR shown on this device…`. The other
device must therefore *scan our screen* (or we scan its QR and it co-signs a statement bound to
*its* offer); a statement built from a pending sign-in can no longer be completed by itself.

### Payload URIs

QR codes and paste boxes carry URIs (`beaconfix://link/…`, `beaconfix://statement/…`,
`beaconfix://identity/…`), so a phone camera offers to open them in the app instead of "no app
can open this". The older `BFLNK1:` / `BFLINK1:` / `BFID1:` texts are still accepted everywhere.

### Test instances

A second BeaconFix started with a non-default `XDG_CONFIG_HOME` (or with `--no-mdns`, or
`apiMdns=false`) never advertises itself over mDNS: a test copy of your identity must not show
up on your phone next to the real machine.

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
