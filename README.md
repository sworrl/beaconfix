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

When BeaconDB has never heard of the access points around you (rural places, hollows), BeaconFix asks
**Apple's Wi-Fi positioning service** (`gs-loc.apple.com`, keyless) for the mapped position of each
BSSID and takes the signal-weighted centroid of the ones it knows. It is honest about what it is:
the fix is labelled "Apple Wi-Fi", and it can be switched off in Settings (it sends the BSSIDs you
hear to Apple). Only after that does it fall back to the IP guess.

## The map

Pan by dragging, zoom with the wheel (anchored at the cursor), double-click to zoom in,
◎ to follow the fix again. Four styles, all keyless: **Dark** (OpenStreetMap through a
night filter: dark background, slate roads, light labels, road colours kept), **Streets**
(OSM), **Satellite** (Esri World Imagery + place labels) and **Topographic** (OpenTopoMap).
Tiles go through one disk cache (500 MB), so the map keeps working offline between stops.
The trip log is drawn as a track: solid between precise fixes, dashed where IP was involved.

## Places

Points of interest within 6 km (adjustable: 2–25 km) come from OpenStreetMap via Overpass
and are picked for life on the road: fuel (diesel / propane / truck lanes noted), propane,
EV charging, groceries, food, cafés, camping / RV sites (free, hookups, dump, showers
noted), drinking water, dump stations, showers, toilets, laundry, hospitals, pharmacies,
auto / tyres, hardware, public Wi-Fi, libraries, post offices and rest areas. They
cluster when zoomed out and get name labels when zoomed in; hover for details (hours, phone),
right-click for directions (OSM or Google), website, OSM page. Categories are toggled from
the filter button. The **Nearby** tab lists them nearest first with a filter box.
Cached in `pois.json`; refetched after moving a third of the radius or after 7 days.

## Where the beacons are drawn

| marker | meaning |
|---|---|
| gold diamond | real position from **WiGLE** (optional API token in Settings; looked up 1.5 s apart, cached per BSSID) |
| gold dot + dashed ring | **multilaterated** from RSSI ranges at two or more vantage points; observations whose fixes overlap (a parked rig, BeaconDB jitter) count as one vantage point |
| cyan dot on a dotted orbit | heard, **distance only** from RSSI (band-aware log-distance model) at a stable pseudo-bearing — the direction is *not* known |
| green / magenta / grey | connected · travels with you · ignored |

Access points that travel with you (your own router, hotspots) are excluded: the
connected network by default, anything heard at two places further apart than both fixes'
error (IP fixes count, so this works on Starlink too), phone-hotspot / car / dashcam
SSIDs by name, plus a glob ignore list. `_nomap` SSIDs are honoured.

A HUD shows your rank (Newcomer → Wanderer → Scout → Pathfinder → Navigator → Cartographer
→ Beaconmaster → Surveyor → Wayfinder → Lighthouse, by beacons logged), counters and an XP bar.

## The trip

Every stop in the log now records when you were last confirmed there, so the **Trip** tab
(app and widget) can tell you: distance today / this trip / all time, time moving vs stopped,
speed and heading of the last leg (from consecutive precise fixes — IP fixes are the Starlink
ground station and are ignored), how long you've been at this stop, longest leg and stay, and the
places visited (distinct cities, states/provinces and countries from the reverse geocode).
A trip starts at the last gap of a week or more between stops, or wherever you press
*Start a new trip here* (app, tray, widget, or `beaconfix --new-trip`).

Each precise stop is tagged with its **elevation** (Open Topo Data SRTM 30 m, cached per ~100 m,
off-switchable) and the **sun** for today at the fix — sunrise, sunset, solar noon, golden hour,
civil dawn/dusk, day length — computed locally, no network.

**Milestones** are a small persistent set of display-only badges (first Wi-Fi / GPS fix, first
WiGLE placement, first multilateration, 100 / 1,000 / 5,000 beacons, 10 / 50 stops, 5 / 10 states,
two countries, 1,000 / 10,000 km, a 500 km day, a week at one stop, night owl, early bird,
above 2,000 m, at sea level, offline map saved, top of the ladder). Nothing to click, nothing to win.

**Notifications** (Plasma's, through `org.freedesktop.Notifications`, or the tray balloon):
a new stop logged, entering a new state / province / country, a milestone — each switchable
in Settings.

**Sharing** from the app, the tray, the map's context menu and the widget: copy coordinates,
a `geo:` URI, place + link, OpenStreetMap / Google Maps / Apple Maps links, save the trip as GPX
(with elevations and dwell times).

**Offline**: on every new precise stop (and on request) the tiles for ~10 km around it at
zoom 10–15 in the current map style are warmed into the 500 MB cache — at most 400 tiles, two at a
time, only cache misses touch the network. Toggle in Settings.

## Live beacons and events

Between position checks the tray rescans Wi-Fi every 45 s (Settings → "Live beacon scan", 0 = off)
without geolocating, so the map stays alive: beacons that appear ripple in and their name slides
out of the marker, ones that fade (missed twice in a row) shrink away, a level change of 8 dB or
more shows a pulsing ▲/▼ with the delta, a beacon that Apple or WiGLE places glides from its
signal-distance orbit to its mapped spot with a gold flash, a new fix draws a dashed arrow from
where you were and bursts the halo, a new stop drops a pin, and milestones / new regions /
offline saves toast under the HUD. A ticker bottom-left lists the last five events with their
age and fades them out over a minute (hover to hold). If more than half of the usable beacons
change between two live scans you have arrived somewhere, and a real position check runs.

Wi-Fi names are drawn next to the beacons — every one at zoom 15 and up, the twelve loudest from
zoom 13, coloured like their marker ("(hidden)" for cloaked networks, band badge 2.4 / 5 / 6 from
zoom 16), placed right / above / below / left so they never cover each other. Toggle with
"Show Wi-Fi names" in the map's context menu or Settings.

The same events are exported: `--json` and `StateJson()` carry `events[]` (last 60, newest last)
and `lastEventId`, and the D-Bus interface emits `eventLogged(json)` per event:

```
{"id": 12, "type": "ap_new", "time": "2026-09-26T14:20:05", "text": "Barn Wi-Fi appeared · -61 dBm",
 "bssid": "…", "ssid": "…", "dbm": -61, "kind": "ring", "status": "used", "lat": …, "lon": …, "r": 85, "bearing": 212}
```
Types: `ap_new ap_lost ap_up ap_down ap_placed fix stop achievement region prefetch error`.
`ap_up`/`ap_down` add `delta` (dB); `fix`/`stop` add `fromLat`/`fromLon` when you moved further
than the fix's error; `ap_placed` keeps `r`/`bearing` of the orbit point it moved from. For
`kind: ring` the `lat`/`lon` are *your* position and the beacon sits `r` metres away at `bearing`.
Each `aps[]` entry now also has `ch` (channel) and `band` ("2.4" | "5" | "6").

## Pieces

- `beaconfix` — Qt Widgets app: fix card, the map above (QPainter — no WebEngine, no GL),
  Nearby places, access-point table (with "where" column), trip log with GPX export, settings.
- **Tray icon** (`beaconfix --tray`, autostarted): place, accuracy, age, re-check, copy, open in OSM.
- **Plasma widget** `org.kde.plasma.beaconfix` — panel/desktop applet: place + source chip
  (optionally an accuracy chip and speed/heading in the panel) and four tabs, each switchable:
  **Map** (the same map, beacons and places), **Nearby** (places list), **Radar** and **Trip**
  (distances, time, speed, elevation, sun, places visited, milestones, stops). Share menu.
  Reads the same fix. Its tiles come from the tray's localhost tile server
  (`http://127.0.0.1:47821/t/<layer>/<z>/<x>/<y>.png`, tiles only — not a proxy) because OSM
  blocks QML's generic User-Agent; without the tray it falls back to Esri tiles.
- **D-Bus** `org.sworrl.BeaconFix` at `/org/sworrl/BeaconFix`: properties `valid latitude
  longitude accuracy source place timestamp apCount intervalMinutes` (+ v3: `elevation speedKmh
  headingDeg rank geoUri`), methods `Refresh()` `ShowWindow()` `StateJson()` (+ v3:
  `ExportGpx(path)` `CopyToClipboard(what)` `StartTrip()` `PrefetchTiles()`), signal `FixChanged()`.
  D-Bus activated; v2 clients keep working unchanged.
- **CLI**: `beaconfix --once` (standalone probe → JSON), `--json` (current fix, incl.
  `aps[]` with estimates, `pois[]`, `track[]` with dwell/leg data, `stats{}` with the trip figures,
  rank ladder and milestones, `elevation`, `sun{}`, `share{}`), `--refresh` (poke the running
  instance), `--gpx file`, `--copy coords|geo|osm|google|apple|text` (the running instance owns the
  clipboard so it survives), `--new-trip`, `--prefetch`, `--snapshot file.png`.

State: `~/.local/state/beaconfix/` (`state.json`, `history.jsonl`, `aps.json`, `pois.json`,
`elev.json`, `achievements.json`).
Settings: `~/.config/sworrl/beaconfix.conf`.

## Ours: home networks and known devices

Two things travel with the RV and must never be mistaken for the world: its Wi-Fi and its
client devices. Both come from the UniFi controller exports in `~/.config/sworrl/`
(mode 600; BeaconFix reads them, never rewrites them):

- **`home-networks.json`** — `{"ssids": [...], "bssids": [{"bssid","ssid","radio","channel"}, ...],
  "patterns": [...]}`. Every pattern (SSID or BSSID glob, e.g. `AA:BB:CC:?D:EE:F?`) and every
  listed BSSID becomes a **home network**. Home APs are excluded from positioning and from
  multilateration, drawn orange with status `home` (the AP JSON also carries `home: true` and
  `homeSsid`), and never "appear" or "fade" — there is one `home` event when a home network
  comes into or goes out of range. Hearing one means **at the RV**; the last precise fix taken
  at the RV is where the RV is (`homeFix`), so a laptop that walks off can say
  "12 km NE of the RV" (`stats.atHome`, `awayKm`, `awayText`; D-Bus `atHome`, `awayKm`;
  tray tooltip; fix card). Seeded automatically on first run when the home list is empty;
  edit in Settings → Home networks (with *Suggest* for the router you are connected to), or
  from the AP table / map context menu ("Mark as home network").
  CLI: `--home-list`, `--home-add <pattern>`, `--home-remove <pattern>`,
  `--home-import <file>`, and `--home-sync <http://rv-pc:47822> --home-token <token>` to pull
  the RV's list into a laptop's BeaconFix. D-Bus: `HomeNetworks()`, `SetHomeNetworks(list)`.
- **`known-devices.json`** — `{"devices": [{"mac","name","hostname","fixed_ip","ip","online",
  "network","wired","last_seen","ours": true, "scopes"?: [...]}, ...]}`: our client devices.
  Seeded into `beaconfix-known.json` on first run (merges later go there; the export stays
  untouched). With **Only known devices may use tokens** on (`apiKnownOnly`, default), a LAN
  API token only works from a known peer, a pairing request from a known device with
  `ours: true` is **approved automatically** (scope `read`, or the device's `scopes`), and an
  unknown device can still ask while pairing is open — it lands in *Pending* flagged
  "unknown device" for you to approve by hand. Peers are matched in this order: exact
  `fixed_ip` / `ip` from the file, then MAC via the kernel neighbour table (only for peers on
  one of this box's own subnets — across VLANs the router's MAC is all we see), then MAC globs.
  **A device on another VLAN therefore needs its fixed IP in the file.** This computer itself
  (loopback and its own addresses) always counts as known.
  CLI: `--known-list`, `--known-add <mac> --known-name <name>`, `--known-remove <mac>`,
  `--known-import <file>`. D-Bus: `KnownDevices()`. StateJson lists them as `knownDevices[]`.

## Wi-Fi security

Every access point carries NetworkManager's `Flags` / `WpaFlags` / `RsnFlags` plus `Mode` and
`MaxBitrate`, classified into `security`: `open`, `owe`, `wep`, `wpa1`, `wpa2-tkip`, `wpa2`,
`wpa2-eap`, `wpa3`, `wpa2/3`, `wpa3-eap192` (keys `security secFlags wpaFlags rsnFlags maxKbps
adhoc insecure` in `aps[]`, persisted in the database). `open`, `wep`, `wpa1` and `wpa2-tkip`
are **insecure**: they get an `ap_insecure` event once per BSSID per day ("X is open / WEP /
WPA1-TKIP"), a red Security column in the app's AP table and a warning in the beacon card.
`--json` also carries `securitySummary {open, wep, wpa1, tkip, wpa3, total}`.

## Internal mapping database

Everything BeaconFix learns lives in one SQLite database, `~/.local/state/beaconfix/beaconfix.db`:
tables `aps` (BSSID, SSID, band/channel, security, first/last seen, best position + accuracy +
`source` = `placed` (WiGLE/Apple) | `trilat` (≥ 2 vantage points) | `observed` (one place we
heard it), `home` / `travelling` / `ignored` flags), `observations` (where we stood, how loud,
when), `sightings`, `cells`, `flags`, `fixes` (the trip log with departures), `pois`, `elevation`,
`achievements`, `kv`. On the first run with this version the JSON state files (`aps.json`,
`history.jsonl`, `pois.json`, `elev.json`, `achievements.json`) are imported once and kept as
`*.migrated`; from then on the database is the store (`state.json` with the current fix stays
as a plain file for quick readers).

**Encrypted at rest.** There is no SQLCipher on this system, so BeaconFix encrypts the file
itself: the `.db` on disk is an AES-256-GCM blob (OpenSSL; header `BFDB\1`, 12-byte nonce,
16-byte tag) and the working copy lives in the private, tmpfs-backed runtime directory
(`$XDG_RUNTIME_DIR/beaconfix/live.db`, mode 0600) and is re-encrypted back a few seconds after
each change and on exit. The 256-bit key is kept in **KWallet** (folder `BeaconFix`, entry
`mapdb-key`) when a wallet daemon is running and can be opened; otherwise in
`~/.config/sworrl/beaconfix.key` (0600). A key file, when present, takes precedence. Without a
key the tray runs without persistence and says so. Back up the `.db` **together with** the key
(wallet export or the key file) — one is useless without the other.

**Self-location from the map** runs before BeaconDB: two or more heard, non-home, non-travelling
beacons with a database position better than 150 m put the box at their signal-weighted centroid
(`provider: "internal"`), so places you have been before work offline. Observed positions are
drawn as hollow diamonds.

Settings → Map database shows path, size, key source and row counts with *Export…*, *Import…*
and *Rebuild from JSON*. CLI: `--db-stats`, `--db-export <file>` (JSON dump), `--db-import <file>`
(merge). D-Bus: `DbStats()`, `DbExport(path)`, `DbImport(path)`.

## LAN API

Other devices on the local network — a photo frame, a phone, a laptop — can ask "where are
we?" at `http://<this-box>:47822/api/v1/` (advertised as `_beaconfix._tcp` when
`avahi-publish-service` exists; `apiPort` / `apiEnabled` in settings, Devices tab in the app).

Security: only private / link-local peers get an answer at all (others: 403 before the body
is read). Everything but `hello` and pairing needs `Authorization: Bearer <token>`. Tokens are
256-bit random, shown **once**, and only their SHA-256 is stored
(`~/.config/sworrl/beaconfix-devices.json`, 0600; constant-time comparison). 60 requests per
minute per address (failed logins count double), 32 open connections, 8 streams, 4 KB bodies.
If `~/.config/sworrl/beaconfix.crt` + `beaconfix.key` exist the server speaks TLS instead.
Every request is in the Devices tab's access log. See also *known devices* above.

| method | path | scope | what |
|---|---|---|---|
| GET | `/api/v1/hello` | — | name, version, hostname, `pairing` open?, `tls` |
| POST | `/api/v1/pair` `{"name","scopes":["read"]}` | — | 202 `{id, code, expires, poll}` — only while pairing is open (Devices tab, tray, `--pairing 10`) |
| GET | `/api/v1/pair/<id>` | — | `{status: pending\|denied\|approved[, token, scopes]}` — the token comes back exactly once |
| GET | `/api/v1/location` | read | the fix: lat/lon/accuracy/source/provider/place/city/region/country/elevation/time/age_s, `sun{}`, `geo`, `links{osm,google,apple}`, `home{atHome, awayKm, awayText, homeLat, homeLon, homeTime, patterns}` |
| GET | `/api/v1/state` | read | everything (`StateJson`) |
| GET | `/api/v1/events?since=<id>` | read | events newer than `id` |
| GET | `/api/v1/aps` · `/pois` · `/track` · `/trip` | read | beacons · places · trip log · stats |
| GET | `/api/v1/home` | read | the home networks + at-home state |
| PUT | `/api/v1/home` `{"patterns":[...]}` | control | replace the home networks (a laptop syncs from the RV) |
| POST | `/api/v1/locate` `{"wifiAccessPoints":[{"macAddress","signalStrength"}]}` | read | locate *another* device from what it hears, using the internal map (`{location{lat,lng}, accuracy, used}` or 404) |
| GET | `/api/v1/db/stats` | read | database statistics |
| POST | `/api/v1/db/observations` `{"observations":[{"bssid","ssid","dbm","lat","lon","acc","time"}]}` | control | feed a laptop's observations into the map |
| GET | `/api/v1/db/export` | control | JSON dump of the database |
| GET | `/api/v1/stream` | read | Server-Sent Events: `fix`, `beacon`, `ping` every 30 s |
| POST | `/api/v1/refresh` · `/prefetch` | control | re-check now · save map tiles |

Pairing flow: open pairing (10 min) → the device POSTs `/pair` and shows the 4-digit code it
got → the same code appears in the Devices tab / notification → approve the matching one (or
it is auto-approved if the device is one of ours) → the device polls `/pair/<id>` and stores
the token → `GET /location` with `Authorization: Bearer <token>`. Manual tokens: *Create
token…* in the Devices tab or `beaconfix --token "Frameo kitchen" [--control]`.
CLI: `--api-status`, `--devices`, `--approve <id>`, `--deny <id>`, `--revoke <name-or-id>`,
`--pairing <minutes>`. D-Bus: `ApiStatus()`, `ApproveDevice(id)`, `DenyDevice(id)`,
`RevokeDevice(name)`, `CreateToken(name, "read,control")`, `OpenPairing(minutes)`; signals
`pairingRequested(json)`, `deviceApproved(name)`.

```sh
curl http://rv-pc:47822/api/v1/hello
curl -X POST -d '{"name":"Frameo kitchen"}' http://rv-pc:47822/api/v1/pair      # → {"id":"…","code":"4831",…}
curl http://rv-pc:47822/api/v1/pair/<id>                                         # … → {"status":"approved","token":"…"}
curl -H 'Authorization: Bearer <token>' http://rv-pc:47822/api/v1/location
curl -N -H 'Authorization: Bearer <token>' http://rv-pc:47822/api/v1/stream
```

Frameo / Android (plain `HttpURLConnection`, no libraries):

```java
String base = "http://<beaconfix-host>:47822/api/v1";   // or resolve _beaconfix._tcp with NsdManager
String token = prefs.getString("beaconfix_token", null);
if (token == null) {
    JSONObject r = postJson(base + "/pair", "{\"name\":\"Frameo kitchen\",\"scopes\":[\"read\"]}");   // 202
    String id = r.getString("id");                     // show r.getString("code") on screen
    while (true) {                                     // poll until approved (auto-approved when the frame is a known device)
        JSONObject p = getJson(base + "/pair/" + id, null);
        if (p.getString("status").equals("approved")) { token = p.getString("token"); prefs.edit().putString("beaconfix_token", token).apply(); break; }
        if (p.getString("status").equals("denied")) return;
        Thread.sleep(5000);
    }
}
JSONObject loc = getJson(base + "/location", token);   // every N minutes; Authorization: Bearer <token>
if (loc.getBoolean("valid")) showWeatherFor(loc.getDouble("lat"), loc.getDouble("lon"), loc.getString("place"));
// getJson/postJson: HttpURLConnection with setRequestProperty("Authorization", "Bearer " + token) and a JSON body; 401 → forget the token and pair again.
```

## Install

```sh
./install.sh          # builds, installs to ~/.local, autostarts the tray, installs the widget
```

Requires `qt6-base-dev`, `libqt6sql6-sqlite`, `libssl-dev`, `cmake`, a NetworkManager-managed Wi-Fi
interface, and optionally Go (for `grpcurl`) and `avahi-utils` (LAN discovery). Data credits: BeaconDB, Nominatim & Overpass (OpenStreetMap),
Open Topo Data (SRTM), WiGLE (optional), OSM / Esri / OpenTopoMap tiles.

## Consumers

The [Windy Weather](../kde_widg/windy-weather) widget's `locate.sh` uses
`beaconfix --once` when it's installed, so the weather follows the same fix.
