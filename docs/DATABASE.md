# The internal map database

Everything BeaconFix learns lives in one SQLite database, encrypted at rest (see
SECURITY.md): `~/.local/state/beaconfix/beaconfix.db`.

## Tables

| table | columns | what |
|---|---|---|
| `aps` | `bssid` PK, `ssid`, `band`, `ch`, `freq`, `first_seen`, `last_seen`, `times_seen`, `lat`, `lon`, `acc`, `source`, `home`, `travelling`, `ignored`, `wigle`, `wlat`, `wlon`, `wigle_checked`, security columns | one row per access point ever heard; `source` = `placed` (WiGLE/Apple) · `trilat` (two or more vantage points) · `observed` (heard from one place) |
| `observations` | `id`, `bssid`, `time`, `lat`, `lon`, `acc`, `dbm`, `fix_source` | where we stood and how loud it was, precise fixes only |
| `sightings` | `id`, `bssid`, `lat`, `lon`, `acc`, `time` | coarse places it was heard, any fix |
| `cells` | `bssid`, `cell` | ~5 km cells it was seen in (travelling detection) |
| `flags` | `kind`, `bssid` | manual `travelling` / `notTravelling` marks |
| `fixes` | `id`, `time`, `lat`, `lon`, `acc`, `source`, `provider`, `place`, `city`, `region`, `country`, `elev`, `ap_count`, `ap_used`, `departed` | the trip log |
| `pois` | `osm_type`, `osm_id`, `cat`, `name`, `detail`, `lat`, `lon`, `wifi`, `hours`, `phone`, `website` | places of interest cache |
| `elevation` | `cell`, `elev`, `time` | elevation cache per ~100 m cell |
| `achievements` | `key`, `unlocked` | milestones |
| `kv` | `key`, `value` | misc |

## Migration

On the first start of 3.3 or later the pre-existing JSON state (`aps.json`, `history.jsonl`,
`pois.json`, `elev.json`, `achievements.json`) is imported once and the files are renamed
`*.migrated`. `state.json` (the current fix) stays a plain file so `beaconfix --json` can
answer without the tray. If the database cannot be opened (no key), the tray keeps working
without persistence and says so in the fix card and the log.

## Self-location

Before asking BeaconDB, the locator looks up the heard beacons in `aps`: with two or more
non-home, non-travelling, non-ignored beacons whose stored position is better than 150 m, the
fix is their signal-weighted centroid (accuracy = spread + median stored accuracy, at least
40 m), `source: "wifi"`, `provider: "internal"`. Places you have been before therefore resolve
with no network. Observed positions are drawn as hollow diamonds on the map.

## Export, import, rebuild

- Settings → *Map database*: path, cipher, key source, size, row counts, *Export…*, *Import…*,
  *Rebuild from JSON* (re-imports the `*.migrated` files).
- CLI: `beaconfix --db-stats` (JSON), `--db-export <file>` (JSON dump of every table),
  `--db-import <file>` (merge; rows are keyed, so importing twice is harmless), `--import <file>`
  (your location history — see below).
- D-Bus: `DbStats()`, `DbExport(path)`, `DbImport(path)`.
- API: `GET /api/v1/db/stats` (read), `GET /api/v1/db/export` and
  `POST /api/v1/db/observations` (control) — a laptop can feed its observations into the main
  machine's map, and any device can ask `POST /api/v1/locate` to be positioned from it.

## Importing your history

Years of Google Timeline are a map of everywhere you have been — and, for on-device exports and
older Takeouts, of every Wi-Fi network your phone heard there. BeaconFix imports all of it:

| file | where it comes from | what BeaconFix takes |
|---|---|---|
| `Timeline.json` | Google Maps on the phone: avatar → **Timeline** → ⋯ → **Export Timeline data** (Android 14+/iOS with on-device Timeline; a few MB to a few hundred MB) | `rawSignals[].position` → positions; `rawSignals[].wifiScan` (`deliveryTime`, `devicesRecords[{mac (decimal), rawRssi}]`) → Wi-Fi samples; `semanticSegments[].visit` → stops (place id / HOME / WORK…); `semanticSegments[].timelinePath` → track points |
| `Records.json` | Google Takeout → Location History (the old server-side history; can be several hundred MB) | `locations[]` (`latitudeE7`, `longitudeE7`, `accuracy`, `altitude`, `timestamp` or `timestampMs`, optional `wifiScan.accessPoints[{mac, strength}]`) |
| `Semantic Location History/<year>/<year>_<MONTH>.json` | the same Takeout | `placeVisit` → stops (name, address); `activitySegment.simplifiedRawPath` → track points |
| WiGLE CSV (`WigleWifi-1.x` header) | the WiGLE app's export | every `Type=WIFI` row: a Wi-Fi sample at the row's position (`AuthMode` → security); one position per distinct time |
| GPX | any tracker | `trkpt`/`rtept` with `time` → track points (`ele` → elevation, `hdop` → accuracy); `wpt` → stops |
| KML | Google Earth, Maps timeline exports | `gx:Track` (`when` + `gx:coord`) → track points; timed `Placemark` points → stops; untimed lines are skipped |
| BeaconFix export | `--db-export` / `GET /db/export` | merged as before (`--db-import` still works) |

The format is detected from the content, not the name. Files are **streamed**: the top-level arrays
are scanned element by element and only one element is parsed at a time, so a 700 MB `Records.json`
does not need 700 MB of memory. Wi-Fi scans are paired with the nearest position within ±60 s
(interpolated when both neighbours are within two minutes; skipped when that position is worse than
100 m), and become observations of device `timeline` (`wigle`, `gpx`, `kml`) with
`source: "timeline"` (or `"wigle"`), so they refit the beacons like samples from any other device
— **never the live fix**. Positions and visits become history fixes of that device: a dotted trail
and hollow rings on the Beacons view (context menu → *Show imported history*), thinned to one point
per 30 s unless you moved 25 m. Beacons touched are refitted in one batch after the import.

Ways to run it (all share one options set: a time window and what to take):

- Settings → *Map database* → **Import…** (file dialog, from/to dates, positions / Wi-Fi scans /
  visits, a progress bar, then the summary).
- CLI: `beaconfix --import <file> [--from 2024-01-01] [--to 2024-12-31] [--what positions,wifi,places] [--json]`
  (progress on stderr; the tray must be running).
- D-Bus: `Import(path, optsJson)` → summary JSON; `importProgress(json)` signals
  `{"file","percent","stage"}` while it runs and `{"done":true,"ok","summary"}` at the end.
- API: `POST /api/v1/db/import?name=<file>&from=&to=&what=` with the raw file as the body
  (control scope, ≤ 200 MB, streamed to a temporary file; the phone app uses this to hand a file
  it picked to the desktop).

The **summary** is the same object everywhere (the Android app shows the same names):

```json
{"format":"timeline","file":"Timeline.json","device":"timeline",
 "positions":48210,"tracks":9120,"wifiScans":15102,"observations":61233,"beaconsTouched":2941,
 "visits":812,"skipped":37,"errors":0,"bytes":184320011,
 "first":"2019-03-02T08:11:04","last":"2026-09-20T18:40:12","seconds":41.3,"error":""}
```

`positions` = raw positions taken, `tracks` = path points, `wifiScans` = scans read,
`observations` = Wi-Fi samples written (scans × networks, after pairing), `beaconsTouched` = distinct
BSSIDs, `visits` = stops, `skipped` = rows outside the window, track points thinned away, scans without a
usable position within a minute, non-Wi-Fi rows, `errors` = batches the database refused. Importing the same file twice is harmless: samples
and fixes are keyed by (bssid, time, device) and (device, time).

## Backup

Copy `~/.local/state/beaconfix/beaconfix.db` **and** the key: either
`~/.config/sworrl/beaconfix.key` or the `BeaconFix/mapdb-key` entry of your KWallet. One is
useless without the other. A `--db-export` JSON dump is an unencrypted alternative; treat it as
sensitive (it is a history of where you have been).
