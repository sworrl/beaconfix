# BeaconFix

*Where am I?* A KDE Plasma 6 / Qt 6 desktop locator for machines without GPS.
Built for a workstation that moves every day on Starlink, where IP geolocation
only tells you which ground station you're routed through.

## How it gets a fix

1. **Starlink dish GPS** — `get_location` over gRPC on the dish (`192.168.100.1:9200`).
   True position, but only after you enable *Settings → Advanced → Debug data →
   Allow access on local network* in the Starlink app. Needs `grpcurl`.
2. **BeaconDB Wi-Fi** — scans nearby access points through NetworkManager and asks
   [api.beacondb.net](https://beacondb.net) (the open successor to Mozilla Location
   Service). Typically 30–100 m in towns and campgrounds; nothing in the middle of nowhere.
3. **IP geolocation** — last resort, city-level at best.

Access points that travel with you (your own router, hotspots) are excluded: the
connected network by default, anything seen at two stops more than ~5 km apart
automatically, plus a glob ignore list. `_nomap` SSIDs are honoured.

## Where the beacons are drawn

The **Beacons** view is a night-mode OpenStreetMap (tiles darkened in-app, no
WebEngine, no GL) with you at the centre — pulsing halo, accuracy ring, radar
sweep — and every access point you can hear placed by the best estimate available:

| marker | meaning |
|---|---|
| gold diamond | real position from **WiGLE** (optional API token in Settings; looked up 1.5 s apart, cached per BSSID) |
| gold dot + dashed ring | **estimated** from a signal-weighted centroid, once heard from two vantage points further apart than the fixes' own error |
| cyan dot on a dotted orbit | heard, **distance only** from RSSI (log-distance model) at a stable pseudo-bearing — the direction is *not* known |
| green / magenta / grey | connected · travels with you · ignored |

Hover a beacon for its details. A HUD shows your rank (Newcomer → Wanderer → Scout →
Pathfinder → Navigator → Cartographer → Beaconmaster, by beacons logged), counters
and an XP bar. It's a display, not a game: nothing to click, nothing to win.

## Pieces

- `beaconfix` — Qt Widgets app: fix card, the Beacons view above, access-point table
  (with "where" column), trip log with GPX export, settings.
- **Tray icon** (`beaconfix --tray`, autostarted): place, accuracy, age, re-check, copy, open in OSM.
- **Plasma widget** `org.kde.plasma.beaconfix` — panel/desktop applet: place + source chip,
  a live radar canvas of the beacons by distance, and the HUD line. Reads the same fix.
- **D-Bus** `org.sworrl.BeaconFix` at `/org/sworrl/BeaconFix`: properties `valid latitude
  longitude accuracy source place timestamp apCount intervalMinutes`, methods `Refresh()`
  `ShowWindow()` `StateJson()`, signal `FixChanged()`. D-Bus activated.
- **CLI**: `beaconfix --once` (standalone probe → JSON), `--json` (current fix, incl.
  `aps[]` with estimates and `stats{}`), `--refresh` (poke the running instance),
  `--snapshot file.png` (render the window to a file — used for the screenshots).

State: `~/.local/state/beaconfix/` (`state.json`, `history.jsonl`, `aps.json`).
Settings: `~/.config/sworrl/beaconfix.conf`.

## Install

```sh
./install.sh          # builds, installs to ~/.local, autostarts the tray, installs the widget
```

Requires `qt6-base-dev`, `cmake`, a NetworkManager-managed Wi-Fi interface, and
optionally Go (for `grpcurl`).

## Consumers

The [Windy Weather](../kde_widg/windy-weather) widget's `locate.sh` uses
`beaconfix --once` when it's installed, so the weather follows the same fix.
