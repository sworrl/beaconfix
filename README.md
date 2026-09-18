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

## Pieces

- `beaconfix` — Qt Widgets app: fix card, OpenStreetMap tile map (no WebEngine),
  access-point table, trip log with GPX export, settings.
- **Tray icon** (`beaconfix --tray`, autostarted): place, accuracy, age, re-check, copy, open in OSM.
- **Plasma widget** `org.kde.plasma.beaconfix` — panel/desktop applet reading the same fix.
- **D-Bus** `org.sworrl.BeaconFix` at `/org/sworrl/BeaconFix`: properties `valid latitude
  longitude accuracy source place timestamp apCount intervalMinutes`, methods `Refresh()`
  `ShowWindow()` `StateJson()`, signal `FixChanged()`. D-Bus activated.
- **CLI**: `beaconfix --once` (standalone probe → JSON), `--json` (current fix),
  `--refresh` (poke the running instance).

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
