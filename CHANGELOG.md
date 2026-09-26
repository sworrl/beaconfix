# Changelog

All notable changes to BeaconFix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [3.3.0] — 2026-09-26

### Added
- **LAN API** on port 47822: bearer tokens (256-bit, stored only as SHA-256), a pairing flow
  with 4-digit codes, per-address rate limiting, LAN-only peers, Server-Sent Events, TLS when a
  certificate is present, `_beaconfix._tcp` advertisement when avahi is available.
  Endpoints for the fix, state, events, beacons, places, track, trip, home networks, a
  `locate` service for other devices, database statistics/observations/export.
- **Known devices**: an allowlist of your own client devices; tokens only work from known
  peers and pairing requests from known devices are approved automatically.
- **Home networks**: SSID/BSSID patterns for the networks that travel with you. They are never
  used for positioning, get their own colour and status, and give an at-home / distance-from-home
  state that is exported everywhere (JSON, D-Bus, API, tray, fix card).
- **Internal mapping database**: one SQLite database replaces the JSON state files, migrated
  automatically on first start. Stored on disk as an AES-256-GCM blob with the key in KWallet or a
  0600 key file. A **self-locate tier** runs before BeaconDB, so places you have been before
  resolve offline. Export, import and rebuild from the app or the CLI.
- **Wi-Fi security classification** of every beacon from NetworkManager's RSN/WPA flags
  (open, WEP, WPA1, WPA2 with TKIP, WPA2-PSK, WPA2-Enterprise, WPA2/3 transition, WPA3-SAE,
  WPA3-Enterprise 192, OWE); `ap_insecure` events, a Security column, and a widget audit panel
  that explains each weakness.
- Devices tab in the app; CLI and D-Bus management for the API, home networks, known devices and
  the database.

### Fixed
- Pattern matching for home/ignore lists compiled a regular expression per pattern per paint;
  the caches are now rebuilt only when a list changes.

## [3.2.0] — 2026-09-26

### Added
- **Cinematic map mode** in the widget: eased fly-ins on events and periodic overviews at city
  and state scale with a caption; any interaction cancels the tour.
- Header click on the widget invokes the desktop's *Show Desktop* action.

### Changed
- The event ticker collapses to the newest line and unfolds on hover, so it no longer hides the map.
- Performance: animations paint on a separate canvas at 30 fps; the idle pulse stops after two idle
  minutes; the radar repaints at 20 fps only while visible; flights run on a 16 ms clock without
  label layout.

## [3.1.0] — 2026-09-26

### Added
- **Apple Wi-Fi positioning** tier (keyless) between BeaconDB and the IP fallback; each fix carries
  its `provider`.
- **Live beacon scan** every 45 s with an event log (`ap_new`, `ap_lost`, `ap_up`, `ap_down`,
  `ap_placed`, `fix`, `stop`, `achievement`, `region`, `prefetch`, `error`) in `--json` and over D-Bus.
- Wi-Fi names beside the beacons with collision avoidance, band badges, and event animations on
  both the app map and the widget map; an event ticker; pinch-to-zoom and long-press in the widget.

### Fixed
- The map no longer auto-fits to an IP fix's error circle.
- Average speed is only computed over legs of a few hours; "moving" needs a recent short leg.
- Today's distance requires a departure today.

## [3.0.0] — 2026-09-26

### Added
- **Trip intelligence**: distance today / this trip / all time, moving vs stopped time, speed and
  heading of the last leg, dwell at the current stop, longest leg and stay, cities / regions /
  countries visited, an explicit *new trip* action.
- **Elevation** per precise stop (Open Topo Data SRTM) and locally computed **sun times**.
- A ten-tier rank ladder with 24 persistent milestones.
- Desktop **notifications** for new stops, new regions and milestones.
- **Sharing**: coordinates, `geo:` URI, map links, GPX export with elevation and dwell.
- **Offline tile prefetch** around each new stop.
- Widget: compact chip, per-tab toggles, Trip tab, Share menu.

## [2.0.0] — 2026-09-25

### Added
- Four keyless map styles (dark, streets, satellite, topographic) with a 500 MB tile cache.
- Places of interest from OpenStreetMap/Overpass with categories aimed at life on the road; a
  **Nearby** tab in the app and the widget.
- The trip log drawn as a track.
- A localhost tile server for the widget, because OpenStreetMap blocks QML's generic User-Agent.

## [1.1.0] — 2026-09-17

### Added
- **Beacons view**: a night-mode map with every heard access point placed (WiGLE lookup,
  signal-weighted centroid, or RSSI ring at a stable pseudo-bearing), a radar HUD with ranks.
- Widget radar canvas.

### Fixed
- BeaconDB's GeoIP fallback is no longer mistaken for a Wi-Fi fix.

## [1.0.0] — 2026-09-17

### Added
- Initial release: Starlink dish GPS → BeaconDB Wi-Fi → IP locate chain, tray icon, Qt window,
  Plasma widget, D-Bus interface `org.sworrl.BeaconFix`, `--once` / `--json` / `--refresh`.

[3.3.0]: https://github.com/sworrl/beaconfix/releases/tag/v3.3.0
[3.2.0]: https://github.com/sworrl/beaconfix/compare/v1.1.0...v3.3.0
[3.1.0]: https://github.com/sworrl/beaconfix/compare/v1.1.0...v3.3.0
[3.0.0]: https://github.com/sworrl/beaconfix/compare/v1.1.0...v3.3.0
[2.0.0]: https://github.com/sworrl/beaconfix/compare/v1.1.0...v3.3.0
[1.1.0]: https://github.com/sworrl/beaconfix/releases/tag/v1.1.0
[1.0.0]: https://github.com/sworrl/beaconfix/releases/tag/v1.0.0
