# Configuration

BeaconFix keeps settings in `~/.config/sworrl/beaconfix.conf` (QSettings, INI). Most are set
from the app's Settings and Devices tabs; all can be edited by hand while the tray is stopped.

## Settings keys

| key | default | meaning |
|---|---|---|
| `intervalMinutes` | 15 | minutes between full position checks (also the D-Bus property) |
| `moveThresholdM` | 250 | a fix further than this from the last stop starts a new stop |
| `liveScanSeconds` | 45 | Wi-Fi rescan interval between checks for the live map and events; 0 = off |
| `useStarlink` | true | try the Starlink dish GPS first |
| `starlinkHost` | 192.168.100.1 | the dish's address (gRPC on port 9200) |
| `useApple` | true | ask Apple's Wi-Fi positioning when BeaconDB has no match |
| `useIp` | true | fall back to IP geolocation |
| `useElevation` | true | look up elevation for precise stops (Open Topo Data) |
| `ignoreActiveAp` | true | exclude the network you are connected to from positioning |
| `ignorePatterns` | (empty) | globs of SSIDs/BSSIDs never used for positioning |
| `homeNetworks` | (seeded) | globs of the networks that travel with you (see below) |
| `homeFix` | (state) | last precise fix taken while a home network was heard |
| `wigleToken` | (empty) | optional WiGLE API token for real beacon positions |
| `poiRadiusKm` | 6 | radius for places of interest (1–30) |
| `prefetchTiles` | true | warm the tile cache around each new stop |
| `prefetchedTiles` | 0 | counter (state) |
| `notifyStops` / `notifyRegions` / `notifyAchievements` | true | desktop notifications |
| `tripStart` | (state) | explicit start of the current trip |
| `apiEnabled` | true | serve the LAN API |
| `apiPort` | 47822 | its port (the next nine are tried if busy) |
| `apiKnownOnly` | true | tokens only work from known devices; unknown pairings need manual approval |
| `apiPairingUntil` | (state) | pairing window end |

The Plasma widget has its own settings (see WIDGET.md).

## Files

| path | contents |
|---|---|
| `~/.config/sworrl/beaconfix.conf` | settings above |
| `~/.config/sworrl/beaconfix.key` | database key (hex, 0600) when KWallet is not used — **back it up** |
| `~/.config/sworrl/beaconfix.crt` + `beaconfix.key`\* | optional TLS certificate and key for the API |
| `~/.config/sworrl/beaconfix-devices.json` | paired API devices: token hashes, scopes, last seen (0600) |
| `~/.config/sworrl/beaconfix-known.json` | known (your own) devices for the API allowlist |
| `~/.config/sworrl/home-networks.json` | optional seed for the home networks (read once, never written) |
| `~/.config/sworrl/known-devices.json` | optional seed for the known devices (read once, never written) |
| `~/.local/state/beaconfix/beaconfix.db` | the encrypted map database |
| `~/.local/state/beaconfix/state.json` | the current fix, for quick readers (`beaconfix --json` without the tray) |
| `~/.local/state/beaconfix/*.migrated` | the pre-3.3 JSON state, kept after migration |
| `$XDG_RUNTIME_DIR/beaconfix/live.db` | the decrypted working copy (tmpfs, 0600) while the tray runs |
| `~/.cache/beaconfix/` | map tiles (500 MB) |

\* If both a TLS key and a database key are wanted, note that they share the `beaconfix.key`
name: the TLS pair is only read when `beaconfix.crt` exists next to it, and then the database
key must live in KWallet. Rename one if this bites.

## Home networks

Patterns are case-insensitive globs matched against the SSID and the BSSID
(`Example Wi-Fi*`, `AA:BB:CC:?D:EE:F?`). A home network is never used for positioning or
multilateration, never produces "appeared"/"faded" events (one `home` event marks coming into
or going out of range), and has status `home` in `aps[]`. Hearing one means *at home*; the last
precise fix taken at home is where home is (`homeFix`), so a laptop that walks off reports
`stats.awayKm` / `awayText` ("12 km NE of home").

Manage: Settings → Home networks (with *Suggest* for the router you are connected to), the
access-point table or map context menu (*Mark as home network*), `beaconfix --home-list`,
`--home-add <pattern>`, `--home-remove <pattern>`, `--home-import <file>`, or pull a second
machine's list from the API: `beaconfix --home-sync http://<beaconfix-host>:47822 --home-token <token>`.

Seed file `~/.config/sworrl/home-networks.json` (read on first start when the list is empty,
or on `--home-import`):

```json
{
  "ssids": ["Example Wi-Fi", "Example IoT"],
  "bssids": [{"bssid": "AA:BB:CC:1D:EE:F0", "ssid": "Example Wi-Fi", "radio": "na", "channel": 48}],
  "patterns": ["Example*", "AA:BB:CC:?D:EE:F?"],
  "wlans": [{"ssid": "Example Wi-Fi", "wpa_mode": "wpa2", "wpa_enc": "ccmp", "pmf_mode": "required",
             "wpa3_support": true, "wpa3_transition": false, "hide_ssid": false, "is_guest": false, "l2_isolation": false}]
}
```

`ssids`, `bssids[].bssid` and `patterns` all become home patterns. `wlans[]` is optional: per
SSID, the access point's own security settings (as exported from a UniFi controller, for
example) so the widget can grade your own networks precisely (`homeWlan` in `aps[]`).

## Known devices

Your own client devices, for the API allowlist and auto-approval. Stored in
`~/.config/sworrl/beaconfix-known.json`; seeded from `known-devices.json` on first start or with
`--known-import <file>`:

```json
{
  "devices": [
    {"mac": "aa:bb:cc:dd:ee:01", "name": "Photo frame", "hostname": "frame", "fixed_ip": "192.0.2.10",
     "ip": "192.0.2.10", "online": true, "network": "Example IoT", "wired": false,
     "last_seen": 1790000000, "ours": true, "scopes": ["read"]}
  ]
}
```

Matching order for an API peer: `fixed_ip` / `ip`, then MAC via the neighbour table (only for
peers on one of this computer's own subnets), then MAC globs. A device on another VLAN or
behind a router is only recognisable by IP, so give it a fixed address. Manage with
`--known-list`, `--known-add <mac> --known-name <name>`, `--known-remove <mac>`, or the
Devices tab.
