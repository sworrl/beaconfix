# BeaconFix

**Where am I?** A KDE Plasma 6 / Qt 6 desktop locator for computers without GPS: a
workstation in a motorhome, a laptop on a satellite link, a desktop that moves.
BeaconFix works out where the machine is, shows the Wi-Fi beacons around it on a live map,
keeps a trip log, and shares the position with the other devices on your local network
through a secured API. Everything it learns goes into an encrypted personal map database,
so places you have been before resolve again with no network at all.

- **Position** from the Starlink dish's GPS, your own map database, [BeaconDB](https://beacondb.net),
  Apple's Wi-Fi positioning service, or the IP address as a last resort, in that order.
- **Live beacon map** with Wi-Fi names, signal rings, multilaterated and mapped positions,
  events as motion (new, lost, louder, placed), a cinematic tour mode, and a security grade
  for every access point with the reasoning spelled out.
- **Trip log**: stops, dwell times, distance, speed and heading, elevation, sun times,
  places visited, milestones, GPX export.
- **LAN API** with pairing codes and bearer tokens so a photo frame, a phone or a second
  computer can ask "where are we?".
- **Home networks**: mark the networks that travel with you; they are never used for
  positioning and give an "at home / 12 km from home" state.
- A **tray icon**, a **Qt window**, a **Plasma widget**, a **D-Bus** interface and a **CLI**.

> Screenshots: `docs/img/` (map, trip, widget, security panel). Not included yet.

## Install

Requirements: KDE Plasma 6 (for the widget; the app and tray work on any Qt 6 desktop),
Qt 6.4+ (Core, Gui, Widgets, Network, DBus, Sql with the SQLite driver), OpenSSL, CMake 3.16+,
a C++17 compiler, a Wi-Fi interface managed by NetworkManager. Optional: `grpcurl` for the
Starlink dish GPS tier, `avahi-utils` to advertise the API on the LAN.

**One line** (clones into `~/.local/src/beaconfix`, offers to install the build packages,
builds, installs to `~/.local`, starts the tray, installs the widget):

```sh
curl -fsSL https://raw.githubusercontent.com/sworrl/beaconfix/master/get.sh | bash
```

**From a checkout:**

```sh
git clone https://github.com/sworrl/beaconfix.git && cd beaconfix
./install.sh            # -y --prefix DIR --no-widget --no-autostart --no-deps --no-restart
./uninstall.sh          # --purge also deletes the database, key, settings and tokens
```

**Debian package** (system-wide, includes the widget under `/usr/share/plasma/plasmoids`):

```sh
cmake -S . -B build-pkg -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
cmake --build build-pkg -j"$(nproc)" && (cd build-pkg && cpack -G DEB)
sudo apt install ./build-pkg/beaconfix_*.deb
```

Then add **BeaconFix** through *Add Widgets* on the desktop or a panel. The tray starts at
login; the first start migrates any older JSON state into the database.

### Optional: device ranging and the Pi agent

- **Wi-Fi RTT responder** (the phone measures its distance to this computer to about a metre):
  needs a Wi-Fi card whose driver supports `ENABLE_FTM_RESPONDER` (e.g. Intel AX210) and
  `hostapd` (the script installs it). Run once as root:
  `sudo ~/.local/share/beaconfix/setup-rtt-responder.sh` (`/usr/share/beaconfix/…` with the
  package); add `--country CC` if BeaconFix has no fix yet. `--status` shows its state,
  `--remove` takes out everything it installed (hostapd stays). See
  [docs/RANGING.md](docs/RANGING.md) §6.
- **BLE ranging** needs BlueZ with LE advertising; some kernel + BlueZ 5.7x combinations reject
  every advertisement, BlueZ ≥ 5.8x works ([docs/RANGING.md](docs/RANGING.md) §9.1).
- **Raspberry Pi agent** (a Pi with a GPS HAT as a precise position witness, BLE radio and NTP
  server): `agent/install-on-pi.sh <user>@<pi>` from the desktop. See [docs/AGENT.md](docs/AGENT.md).

## How it gets a fix

| tier | source | accuracy | needs |
|---|---|---|---|
| 1 | **Starlink dish GPS** (`get_location` over gRPC on the dish) | ~10 m | `grpcurl`; *Allow access on local network* enabled in the Starlink app |
| 2 | **Internal map database** (beacons you have positioned before) | 40–150 m | two or more known beacons in range |
| 3 | **BeaconDB** (open Wi-Fi geolocation, the successor to Mozilla Location Service) | 30–100 m | coverage |
| 4 | **Apple Wi-Fi positioning** (`gs-loc.apple.com`, keyless) | 30–150 m | coverage; can be switched off |
| 5 | **IP geolocation** | city | nothing |

Every fix records its `source` (`starlink`, `wifi`, `ip`) and `provider` (`internal`,
`beacondb`, `apple`). Access points that travel with you are excluded: the connected network,
anything heard at two places further apart than the fixes' own error, hotspot-like names, your
**home networks**, and a glob ignore list. `_nomap` SSIDs are honoured.

## Features

**Map** (app and widget): four keyless styles (dark, streets, satellite, topographic) through
one 500 MB tile cache; the trip track; beacons drawn at their mapped position (gold diamond),
multilaterated position (gold dot with a dashed ring), observed position (hollow diamond) or as
a signal-distance orbit (cyan); Wi-Fi names beside the beacons with collision avoidance and band
badges; places of interest from OpenStreetMap aimed at life on the road (fuel, propane, camping,
water, dump stations, laundry, groceries, Wi-Fi, rest areas), **emergency & civic** places
(police, fire stations, hospitals with an ER, urgent care, pharmacies, dentists, vets, libraries,
town halls, courthouses, DMV, schools, community centres) and **kids & fun** places (playgrounds,
parks, dog parks, pools, splash pads, zoos, museums, theme parks, ice cream, cinemas, bowling,
arcades, trampoline and skate parks, beaches, picnic sites, trailheads) — each with its address,
phone number (tap to call), hours and wheelchair access where OpenStreetMap has them — grouped
and filterable, with a **Nearby** list and a **nearest help** card (police, fire, ER, urgent
care and the local emergency number). Events animate: ripples for new beacons, fade-outs for
lost ones, chevrons for level changes, a glide when a beacon gets placed, an arrow for a new
fix, a pin for a new stop; a ticker keeps the last five. **Cinematic mode** glides to events
and periodically zooms out to city and state scale.

**Security**: every beacon carries NetworkManager's RSN/WPA flags and is graded (open, WEP,
WPA1, WPA2 with TKIP, WPA2-PSK, WPA2-Enterprise, WPA2/3 transition, WPA3-SAE, WPA3-192, OWE).
Insecure ones get an event, a red column in the app and a warning ring on the widget map, and
the widget's audit panel explains each weakness. See [docs/SECURITY.md](docs/SECURITY.md).

**Trip**: distance today / this trip / all time, moving vs stopped, speed and heading, dwell,
longest leg and stay, cities / regions / countries visited, elevation (SRTM) and sun times per
stop, a ten-tier rank ladder with milestones, notifications, sharing (coordinates, `geo:` URI,
map links, GPX), offline tile prefetch around each stop.

**Home networks and known devices**: patterns (SSID or BSSID globs) for the networks that move
with you, and a list of your own client devices for the API allowlist. Both can be seeded from
JSON exports; see [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

**LAN API**: `http://<beaconfix-host>:47822/api/v1/` for the fix, state, events, beacons,
places, trip, home state, a `locate` service for other devices, an SSE stream and database
export. LAN-only, token-only, rate limited, optional TLS. [docs/API.md](docs/API.md).

**Internal map database**: SQLite, encrypted at rest with AES-256-GCM, key in KWallet or a key
file. [docs/DATABASE.md](docs/DATABASE.md). Every beacon's position is refined as samples
accumulate (robust weighted least squares on a path-loss model — [docs/ESTIMATION.md](docs/ESTIMATION.md)),
and two BeaconFix installs, or the Android app, keep each other in step through the sync API.

**Identity**: one Ed25519 identity across the desktop, the laptop and the phone; moved between
devices as an encrypted bundle (QR, text, file, or a 6-digit code on the LAN); independently
created identities can be linked; devices sign in to the API with a challenge signature instead
of pairing codes. [docs/IDENTITY.md](docs/IDENTITY.md).

**OS integration** (each part opt-in): the system **time zone follows the fix** (timedated over
D-Bus; a shipped polkit rule makes it prompt-free for admins), the fix is **published to GeoClue**
through a small root helper so location-aware apps, browsers and Night Light see it, **KWin Night
Light** points at where you are, and **locale hints** (country, units, emergency number, dialling
code) are exposed for other widgets.

## Pieces

| piece | what |
|---|---|
| `beaconfix` | the Qt window: fix card, map, Nearby, access-point table, trip log, Devices, Settings |
| `beaconfix --tray` | the background locator and tray icon; owns the state, the database, the API and the tile server; D-Bus activated |
| Plasma widget `org.kde.plasma.beaconfix` | panel or desktop applet with Map, Nearby, Radar and Trip tabs — [docs/WIDGET.md](docs/WIDGET.md) |
| D-Bus `org.sworrl.BeaconFix` | properties, methods and signals for scripts and other apps — [docs/DBUS.md](docs/DBUS.md) |
| CLI | `--once`, `--json`, `--refresh`, `--gpx`, `--copy`, `--nearby <what>`, `--tz`, `--apply-os`, `--identity…`, `--refit`, `--sync <peer>`, `--peers`, `--import <file>`, API / home / known-device / database management — `beaconfix --help` |
| `beaconfix-osd` | tiny root helper (pkexec) that writes `/etc/geolocation` for GeoClue; installed with its polkit action and rules by `install.sh` / the package |

## Privacy

What leaves the machine, and to whom:

| when | to | what |
|---|---|---|
| a Wi-Fi fix is needed and the internal database cannot answer | BeaconDB (`api.beacondb.net`) | BSSIDs, signal levels and channels of the access points heard (your home networks and the connected network excluded) |
| BeaconDB has no match (tier 4, can be disabled in Settings) | Apple (`gs-loc.apple.com`) | the same BSSIDs, up to 30 |
| everything else failed (can be disabled) | ip-api.com | your public IP address, implicitly |
| a fix is accepted | Nominatim (OpenStreetMap) | the coordinates, for a place name |
| places are refreshed | Overpass (OpenStreetMap) | the coordinates and a radius |
| a precise stop is logged (can be disabled) | Open Topo Data | the coordinates, for elevation |
| the map is shown | the tile servers of the selected style (OpenStreetMap, Esri, OpenTopoMap) | tile coordinates |
| optional, only with a token | WiGLE | BSSIDs, one at a time |
| Starlink tier | the dish on your own LAN | nothing leaves the LAN |

Nothing is uploaded to the author or to any BeaconFix service; there is none. The LAN API is
on by default but answers only private addresses, only with a token, and only from devices you
have paired. Turn it off with `apiEnabled=false`.

## Documentation

- [docs/API.md](docs/API.md) — the LAN API, pairing, tokens, examples
- [docs/WIDGET.md](docs/WIDGET.md) — the Plasma widget
- [docs/CONFIGURATION.md](docs/CONFIGURATION.md) — every setting and file
- [docs/SECURITY.md](docs/SECURITY.md) — threat model, encryption, Wi-Fi grading
- [docs/DATABASE.md](docs/DATABASE.md) — the internal map database
- [docs/DBUS.md](docs/DBUS.md) — the D-Bus interface
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) — building, layout, adding a tier, testing
- [docs/RANGING.md](docs/RANGING.md) — device ranging: Wi-Fi RTT, BLE, anchors, the maths
- [docs/AGENT.md](docs/AGENT.md) — the Raspberry Pi agent (GNSS witness, BLE, NTP)
- [CHANGELOG.md](CHANGELOG.md)

## License

GPL-2.0-or-later. See [LICENSE](LICENSE). Data credits: BeaconDB, OpenStreetMap contributors
(Nominatim, Overpass, tiles), Esri World Imagery, OpenTopoMap, Open Topo Data (SRTM), WiGLE,
and the EFF short wordlist (CC BY 3.0 US, https://www.eff.org/dice) for the identity codes.

## Contributing

Issues and pull requests are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).
