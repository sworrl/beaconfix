# Changelog

All notable changes to BeaconFix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Android 1.3.2] — 2026-09-27

### Fixed
- **Ranging drained the battery when it could not range.** With the collector on, the loop fired an
  8-frame RTT burst every 2.5 s whenever `/ranging/info` had once advertised a responder, with no
  check that the desktop was around and no backoff, and it kept the stale info after leaving home.
  Bursts now need the desktop to have answered in the last 5 min (`rttState` `away` otherwise) and
  radio evidence that it is near (a burst answered in the last minute, its BLE advert heard in the
  last minute, or the responder's beacon in a scan of the last 3 min; without any, one probe burst a
  minute). Bursts that measure nothing back off: two quick retries, then 5 s doubling to 2 min
  (`backoff`). Unanswered POSTs back off from 5 s to 5 min instead of retrying every tick.
- **Deep Doze kept the loop busy.** RTT is off in Doze, yet the app still posted every ~5 s. In Doze it
  now waits a minute between ticks (or until `rttState` changes), stops the motion sensors, advertises
  at 1 s and scans at the low-power rate; Doze ending still wakes it at once. The minute only runs
  while the CPU is awake, so live in deep Doze the phone posted every ~1.7–2 min, or not at all
  (09:47–10:13), even with adb polling it every few seconds, while the desktop kept hearing its advert.
- **The phone stopped hearing the desktop whenever its screen was off.** The Bluetooth stack filed the
  scan (`ScanFilter.setServiceData(uuid, null)`) as unfiltered and suspended it screen-off (`dumpsys
  bluetooth_manager`: no "(Filter)", "Suspended Time" 907 s of 1117 s), so the down link only grew
  while the screen was on. The filter now matches any service data under the UUID (`ByteArray(0)`),
  and the scan restarts every 9 min before the stack's scan timeout can downgrade it. Live (locked,
  deep Doze): the scan shows as "(Filter)", 102 of its first 105 results arrived screen-off, and the
  desktop's down-link count grew 17 → 107 in 8 min.
- **The first RTT burst after Doze could wait a minute.** The Doze ticks call `range()` (it returns at
  once with `doze`) and counted as the minute's probe, and a failed first burst (it often times out right
  after Doze) had to wait for the next probe minute; a Doze maintenance window lasts about a minute. Only
  a request that went on the air counts now, and a failed probe is retried at once (twice) before the
  one-a-minute limit applies.
- `POST /api/v1/ranging` bodies had the shape of docs/API.md only in part: the API `Json` does not
  encode defaults, so empty `rtt`/`ble`/`wifi`, `moving: false`, `txPower: 127` and `source: "gps"`
  were left out. Those fields no longer have defaults, so they are always sent.

## [Android 1.3.1] — 2026-09-27

### Fixed
- **"0 RTT samples" with no reason.** Android switches Wi-Fi RTT off for the whole device in deep
  Doze, which a locked, still, unplugged phone reaches a minute or two after the screen goes off
  (showing the app over the lock screen does not end it; unlocking, a charger or motion does). The
  app said "Wi-Fi RTT unavailable (Wi-Fi off?)", and the home screen hid even that as soon as any
  BLE sample existed. `RttRanging` now tells Doze, Wi-Fi off, Location off, no permission, no
  response, timeouts and `onRangingFailure` codes apart (`RTT_NOT_AVAILABLE` re-checks Doze), the
  home screen shows the RTT state on its own line ("paused: the phone is in Doze… unlock it or put
  it on a charger"), and every `POST /api/v1/ranging` carries it as `rttState` so the desktop can show
  why a count stays at 0.
- The ranging loop re-ranges the moment `ACTION_DEVICE_IDLE_MODE_CHANGED` /
  `ACTION_WIFI_RTT_STATE_CHANGED` say RTT is back, instead of waiting for the next 2.5–20 s tick.
- **Another device's advert counted as the desktop.** A desktop's rotating tag is now accepted only
  from an advert of kind `desktop` or `laptop`; the Pi agent advertised the desktop's own tag (kind
  `pi`) and polluted the phone → desktop down link.
- Outside a ranging session the BLE advert used `INTERVAL_LOW` (100 ms, the most aggressive setting)
  although the docs say 1 s: now `INTERVAL_HIGH` (1 s).

## [3.7.0] — 2026-09-27

### Added
- **Device ranging** ([docs/RANGING.md](docs/RANGING.md)): the desktop now measures how far your
  other devices are instead of subtracting two fixes. A per-device robust range filter
  (state: log-distance, one offset per BLE link, the RTT pair-offset error as a Schmidt consider
  state; iterated EKF to convergence for Wi-Fi RTT with a symmetric core and NLOS-aware tails;
  Joseph-form updates) fuses the phone's **Wi-Fi RTT** bursts to our responder, the **BLE**
  advert in both directions, the **differential shared-AP RSSI** fingerprint (a χ² likelihood) and
  geometric solve, and both fixes on a deterministic polar grid → median distance, 16–84 %
  interval, bearing when observable, and a class (`adjacent` ≤ 2 m, `room` ≤ 6 m, `near`, `far`).
  Simulated accuracy at 0.6 m: 0.85 m [0.42, 1.72] uncalibrated BLE, 0.67 m [0.47, 0.97] after
  a one-tap calibration, 0.63 m [0.54, 0.74] with 80 MHz RTT; intervals cover the truth 67–86 %.
- `GET /api/v1/ranging/info`, `POST /api/v1/ranging`, `GET /api/v1/ranging`,
  `POST /api/v1/ranging/calibrate`; `linkedDevices[].range` / `devices/positions[].range`;
  `beaconfix --ranging`, `--ranging-calibrate "<device>@<metres>[@<seconds>]"`; D-Bus `Ranging`,
  `RangingInfo`, `RangingCalibrate`. Calibrations persist in `~/.local/state/beaconfix/ranging.json`.
- **BLE link in the tray** (BlueZ over D-Bus, no extra libraries): the tray advertises the
  BeaconFix service data (rotating 15-minute identity tag, TX power, kind + capability flags) and
  scans for everyone else's; 200 ms adverts and a continuous scan only while a device is ranging,
  1 s / 10-of-30 s otherwise. Tags resolve through paired identities, linked identities and
  pending link requests. Test instances never advertise (`BEACONFIX_NO_BLE=1` also turns it off).
- **Wi-Fi RTT responder** (opt-in, root): `share/beaconfix/setup-rtt-responder.sh` turns the
  AX210 into an 802.11mc FTM responder on a hidden second AP (`bfrtt0`, UNII-3 channel 149 / 80 MHz,
  12 dBm; channel 36 with 300 TU beacons or 2.4 GHz as fallbacks), checks that desktop scanning is
  not harmed, and installs a unit that survives boot ordering, phy renumbering, busy radios and
  restarts (persistent regulatory domain, udev trigger on radio add, scan abort before hostapd).
- **Anchors** — surveyed antennas and places, a frozen JSON contract shared with the Android app,
  the Plasma widget and the Pi agent (kinds `this-computer`, `wifi-ap`, `rtt-responder`, `ble`,
  `gnss`, `custom`; `placedBy` includes `pi`): map-DB table with change sequence, synced through
  `/db/changes` and `/db/sync` (newest `placedAt` / `deletedAt` wins, tombstones);
  `GET/POST /api/v1/anchors`, `GET/DELETE /api/v1/anchors/<id>`; D-Bus `Anchors`, `SetAnchor`,
  `RemoveAnchor`; `--anchors`, `--anchor-set <json | b64:…>`, `--anchor-remove`; `anchors` and
  `features` in `--json`. What they do: a **this-computer** anchor within 100 m replaces the
  desktop's fix (source `anchor`, accuracy = the placement's); **wifi-ap** anchors pin their BSSIDs
  (never refitted, never "travelling", known transmitters for self-location) and calibrate the
  environment's path-loss `P0`/`n` per band (RLS), which becomes the starting `n` of every AP fit;
  **RV anchors** keep their offset from the RV reference and are re-projected (rotated by the new
  heading when known, flagged `headingAssumed` otherwise) after a move of more than 250 m.
- **rv-gnss positioning tier**, ahead of BeaconDB / Apple / IP: while a home network is heard, a
  linked `pi`/`gnss` device's averaged GNSS fix (σ ≤ 5 m) positions the desktop — offset by the
  gnss and this-computer anchors when both are placed (σ combined), else ±5 m for the RV's extent.
  A receiver more than 5 km from a fresh precise fix of ours is ignored.
- **Device events**: `events[]` in `POST /api/v1/devices/position` become `device` events in the
  feed (the Pi agent's GNSS lock / PPS notes); a `kind` in the body is remembered for the token.
- **Map**: anchors drawn as diamonds (accuracy circle, RV reference ring, `?` when the heading was
  assumed); right-click → *Place an antenna here…*; drag a diamond to move it; click to edit.
  Measured ranges drawn as a ring with the 16–84 % band and a label; devices closer than a few
  pixels get a to-scale inset ("0.63 m (0.54–0.74) · adjacent · RTT+BLE"). Settings → Anchors list
  with *At my position…*, *Edit…*, *Remove*.
- **Pairing proximity** uses the ranging posterior when there is evidence (the requester's BLE
  advert found through its identity tag, the shared-beacon fingerprint, both fixes); the
  proximity object carries `ranging` and the replaced `scoreVerdict`.
- `--grant-control <name-or-id>` / D-Bus `GrantControl`: add the control scope to a paired
  device's existing token (no new token).
- `GET /api/v1/aps` paging: `?offset=&limit=` over the beacons heard now, `?all=1&after=&limit=`
  keyset paging over the whole map database.

### Fixed
- **Ranging: the desktop advertised TX power 127 ("unknown")** while the AX210 picked 10 dBm, so
  every down link (a phone or the Pi hearing the desktop) used the −59 dBm "unknown" reference, 25–28
  dB wrong. The tray now requests 7 dBm through `LEAdvertisement1.TxPower` when BlueZ offers
  `CanSetTxPower` (clamped to the adapter's Min/MaxTxPower, falling back to no preference if BlueZ
  refuses), carries it in byte 8 and `/ranging/info` `ble.txPower`, and starts uncalibrated down links
  from `tx − 41`. The level in byte 8 is the one the controller **selected**: BlueZ writes it back into
  the advertisement's `TxPower` property, which is now writable (`ble.txPowerConfirmed`). The
  capability probe no longer blocks the tray's GUI thread (it was a 2 s synchronous D-Bus call).
- **Ranging calibration: one bad RTT burst away from a 200 m offset.** The RTT offset was the plain
  median of the window's bursts; in the 0.6 m calibration the Pixel sent 8 bursts at 14.4–14.7 m and 7
  at 196–408 m, so the median sat at the cluster's edge (14.11 m instead of 13.95 m). It is now the
  median of the densest 2 m cluster (≥ 3 bursts and ≥ 30 % of them, else the offset is kept), and the
  event log says how many bursts agreed.
- **Ranging: automatic BLE calibration learnt from an uncalibrated RTT pair.** The Pixel ↔ AX210 pair
  read ~11 m at 0.6 m, and the RTT-supervised RLS step moved `bleP0Up` to −51 / n 2.26 from it. It now
  runs only after a calibration that measured the RTT offset; an uncalibrated pair's offset prior is
  σ 2 m (was 0.5 m); a calibration restarts each link that has samples from its TX-power prior, and
  only calibrated peers' RLS state is restored at start.
- **Ranging counters reset with every tray restart**: per-device totals and the last RTT time now
  persist in `ranging.json` (written at most once a minute) and show as `samples.total`,
  `samples.since` and `lastRtt`; the phone's `rttState` shows as `rttState` / `rttStateAt` in
  `--ranging` and `GET /api/v1/ranging`.
- **Pi agent: identity/kind collision.** The agent advertised the desktop's own identity (kind `pi`),
  so phones counted it as the desktop and the desktop dropped it as its own advert. It now advertises
  a per-device id derived from the desktop's identity and its device name, which the desktop resolves
  for paired devices without an identity (RANGING.md §9.1). Its kind field follows the 3-bit layout
  (3 = pi, calibrating = bit 5), and when a desktop advert's byte 8 is 127 it falls back to BlueZ's
  `Device1.TxPower`.
- **Linked devices showed kind "laptop"** (e.g. the Pixel): the kind now comes from the pairing /
  identity record, a pending link request, the device's BLE advert kind bits, its `User-Agent`
  (OkHttp / Android), or the name — "laptop" is no longer the catch-all (`device` is).
- **Scalability with ~100k beacons**: `saveApRecords` rewrote every AP record (and every
  sightings/cells row) on every Wi-Fi scan; records now carry a signature of what is stored and
  only changed ones are written (the travelling-flags table only when the sets change).
  `saveRecord` computes one record's flags instead of all of them. `GET /api/v1/aps` no longer
  builds the whole state.
- Build is warning-free with `-Wall -Wextra` (three pre-existing warnings in identity.cpp and
  locator.cpp).

### Notes
- BLE advertising on Ubuntu 24.04's BlueZ 5.72 with kernel 7.0.0-30 fails with `Invalid Parameters
  (0x0d)` (a BlueZ command-size bug that the kernel's new exact-length check rejects); BlueZ ≥ 5.8x
  works. See RANGING.md §9.1.

## [3.6.0] — 2026-09-27

### Security
- **Identity links could be forged from the LAN.** `POST /api/v1/identity/link` is unauthenticated
  by design, and `acceptLink` co-signed any statement the other party had signed: any host on the
  network could mint an identity, link it to yours, then sign in for a read + control token —
  bypassing pairing and proximity. Link payloads now carry a random, single-use offer `ts`
  remembered by the tray for 10 minutes, and a statement is co-signed only when bound to one of
  them (`403 this link was not started from a link QR shown on this device…`). The desktop binds
  its half of a link to the scanned payload's `ts`; `--identity-link-qr` asks the running tray for
  the offer; "Link this identity" on a pending sign-in now shows *our* QR for that device to scan.
- Test instances (a non-default `XDG_CONFIG_HOME`, `--no-mdns`, or `apiMdns=false`) never
  advertise over mDNS, so a test copy of your identity cannot appear on your phone next to the
  real machine.

### Added
- **Pairing v2** (the pairing notification's click now opens a dialog that can finish pairing):
  X25519 + HKDF short-authentication-string **pictures** — the device shows three of 48 icons, the
  desktop shows three rows (one real, two decoys) and a wrong tap denies; a **proximity verdict**
  (`adjacent` / `room` / `near` / `far` / `unknown`) from a differential RSSI test over every
  beacon both sides hear (median gain offset removed, median absolute residual ≤ 4 dB with ≥ 4
  shared → adjacent, ≤ 7 dB → room) plus the distance between the two fixes; policy
  `apiPairProximity` = required (default) / warn / off; a dialog with a mini map, the three rows,
  *Use the code instead…*, *Pair anyway* (type "pair"), Deny; notification actions
  (*Match pictures…* / *Deny*); `POST /api/v1/pair/<id>/cancel`; auto-approved known devices
  are gated by the verdict, the dialog says *Auto-approved (read access)* and offers **Allow
  control too** (identity-linked devices already get control through identity sign-in).
- **Devices on this network** (native Avahi D-Bus mDNS): the tray registers `_beaconfix._tcp`
  on every real interface with TXT `v`/`api`/`id`/`name`/`host`/`kind`/`pair`/`features`/`addr`
  (real LAN addresses; docker/virbr/veth/tun/wg interfaces skipped) and browses for others;
  `GET /api/v1/peers[?scan=1]` (subnet-scan fallback for networks without multicast), D-Bus
  `Peers(scan)`, `beaconfix --peers [--scan] [--all]`, Settings → Identity → *BeaconFix devices on
  this network* with **Link…** / **Sync now**, the tray tooltip's peers line; `hello` gains
  `kind`, `mdns`, `identity`. `install.sh` offers `deny-interfaces=docker0,…` for avahi-daemon
  (docker's bridge was otherwise the address `.local` resolved to).
- `beaconfix --sync <peer name | host | address>` resolves the peer through mDNS and signs in
  with our identity (challenge / Ed25519) when it is the peer's or linked — no token needed; the
  token it yields is cached per peer.
- **History importers** (`src/importers.{h,cpp}`): Google `Timeline.json` (on-device export:
  rawSignals positions + Wi-Fi scans, semantic visits and paths), Takeout `Records.json`,
  Semantic Location History, WiGLE CSV, GPX, KML and BeaconFix exports; streaming parser (a
  700 MB file never sits in memory), Wi-Fi scans paired with the nearest position (±60 s,
  interpolated), positions → history trail of device `timeline`/`wigle`/`gpx`/`kml` (never the
  live fix), visits → stops, one batched refit afterwards. Settings → Map database → **Import…**
  (window, what to take, progress, summary), `beaconfix --import <file> [--from --to --what]
  [--json]`, D-Bus `Import(path, opts)` + `importProgress`, `POST /api/v1/db/import` (control,
  ≤ 200 MB streamed to disk). The Beacons view draws the imported trail (*Show imported history*).
  Unit tests: `tests/importers_test.cpp` + `tests/fixtures/`.
- **Refit animations**: an `ap_refit` event (position moved > max(5 m, 10 %) or accuracy
  improved > 15 %) with `vantagePoints[{lat,lon,dbm,device}]`; the Beacons view pops the vantage
  triangles, expands dashed rings to the new spot, glides the marker with a shrinking uncertainty
  circle, pulses a crosshair, floats a label and sparkles; *Replay last refit* in the context menu.
- **Linked devices on the map**: `linkedDevices[]` in `--json` / `StateJson`,
  `GET /api/v1/devices/positions`, `POST /api/v1/devices/position` (a device reports where it
  is); events `device` (moved > 100 m), `device_online` / `device_offline`; the Beacons view draws
  📱/💻 markers with name and age and a dashed line with the distance (< 2 km); refit rings are
  coloured by the contributing device; *Show my other devices* toggle.
- Payload URIs: QRs and paste boxes use `beaconfix://link/…`, `beaconfix://statement/…`,
  `beaconfix://identity/…` (a phone camera can open them); the old `BFLNK1:` / `BFLINK1:` /
  `BFID1:` texts are still accepted.

### Changed
- Proximity scoring compares against every beacon we hear (home and travelling networks are the
  best co-location evidence), and the verdict is differential (radio gain offset removed) instead
  of a shared-count ratio.
- `MapDb::addObservations` prepares its statements once; an import defers repaints and refits to
  the end (150 k-row Records.json files import in minutes, not hours).

## [3.5.0] — 2026-09-27

### Added
- **OS integration** (Settings → System, each part opt-in): the system **time zone follows the
  fix** (timeapi.io + cache, tzdata fallback; set through `org.freedesktop.timedate1`; a shipped
  polkit rules file makes it prompt-free for admins; never from IP-only fixes, at most every ten
  minutes); the fix is **published to GeoClue** (`/etc/geolocation`) through the root helper
  `beaconfix-osd` with its own polkit action; **KWin Night Light** follows the fix; **locale
  hints** (country, region, units, emergency number, dialling code, zone) in `stats.locale`.
  `beaconfix --tz`, `--apply-os [--dry-run]`, D-Bus `ApplyOs()` / `TimeZoneForFix()`; `tz` events.
- **Emergency & civic** and **kids & fun** places from OpenStreetMap (48 categories in three
  groups; sparse civic categories fetched out to 25 km) with **addresses, phone numbers (tap to
  call), hours and wheelchair access**; a nearest-help card (police, fire, ER, urgent care and the
  local emergency number) in the app, the widget and the API (`/api/v1/emergency`);
  `GET /api/v1/pois?cat=…&group=…&radius=…`; `beaconfix --nearby <what> [--radius km]`;
  grouped *Places to show* menus.
- **Identity**: an Ed25519 identity shared by every BeaconFix you run — create, export (QR /
  text / file, passphrase or 6-word code), import, LAN hand-off with a one-time code, linking of
  independently created identities (co-signed statements), challenge-signature sign-in to the
  API instead of pairing codes, pending sign-ins to link in the Devices tab, `beaconfix
  --identity…` commands, `--identity-selftest` vectors, sync feeds tagged with the identity.
  Specification and implementation notes in [docs/IDENTITY.md](docs/IDENTITY.md).
- Tray: *Nearest help…* and *Identity…* entries; the zone next to the sun times.
- Installer: `--no-polkit`; `install.sh` installs the helper, action and rules with sudo once;
  `uninstall.sh --system` removes them; the .deb ships them.

### Changed
- `docs/API.md` now documents the 3.4.0 sync endpoints and `--refit` / `--sync`.
- The emergency number falls back to a country-name guess until the reverse geocode lands.

## [3.4.0] — 2026-09-26

### Added
- **Position estimation engine**: every beacon's position is refined as samples accumulate —
  robust weighted nonlinear least squares on a log-distance path-loss model (Gauss–Newton with
  Levenberg damping, Huber then Tukey weights, per-AP reference power and exponent), vantage-point
  guards so bad geometry never claims a position, an error ellipse, an incremental Kalman step
  between batched refits, `beaconfix --refit`, D-Bus `Refit()`, Settings → Positioning.
  Self-location from fitted beacons uses the same weighted least squares.
- **Sync API**: `hello.features`, `GET /api/v1/db/changes` (cursor feed with sequence numbers on
  every table), `POST /api/v1/db/sync` (1 MB bodies), larger `/db/observations`, peer positions;
  laptop mode `beaconfix --sync <url> --sync-token <token>` with per-peer cursors and a Settings
  section.
- **Android app** (`android/`): Kotlin / Compose, Room database, foreground Wi-Fi collector with
  on-device estimation, pairing, offline collection with sync on reconnect, signed and minified
  release build.
- Widget: tile levels cross-fade instead of snapping, label layout pauses while zooming, gentler
  wheel steps.

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
