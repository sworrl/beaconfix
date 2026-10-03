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
| `pois` | `osm_type`, `osm_id`, `cat`, `name`, `detail`, `lat`, `lon`, `wifi`, `hours`, `phone`, `website`, `address`, `wheelchair`, `emergency`, `scope`, `peds`, `er`, `campus`, `drive_s`, `drive_m` | places of interest cache: the places query around the fix (`scope` = `near`). `peds` = pediatric tier (API.md "Pediatric ER"), `drive_s`/`drive_m` a routed drive time (0 = use the estimate) |
| `pois_far` | the same columns as `pois` | the pediatric ER search's answer (`scope` = `far`, 3.8). Its own table so that neither save touches the other's rows: sharing `pois`' key, a near save replaced every far row inside the near radius and a restart lost the closest ERs. An object both searches found is stored in both and merged in memory (near wins). Opening a database written by an earlier 3.8 build moves its `far` rows here |
| `estimates` | `bssid` PK, `lat`, `lon`, `acc`, `semi_major`, `semi_minor`, `orient`, `rms`, `p0`, `pathloss`, `fitted_n`, `n`, `vantage`, `rejected`, `quality`, `updated`, `seq`; 3.9: `kind`, `grade`, `score`, `r95`, `cep50`, `p_within25`, `cxx`, `cxy`, `cyy`, `metrics` (every field of the fit as JSON), `version` | our own graded estimate per AP ([GRADING.md](GRADING.md)); rows written by an older engine (no `metrics`) are recomputed at start-up, never deleted |
| `estimate_history` | `id`, `bssid`, `time`, `lat`, `lon`, `cxx`, `cxy`, `cyy`, `r95`, `score`, `grade`, `kind` | 3.9: the last 20 estimates per AP (drift, trends) |
| `scan_cells` | `cell` PK (`y:x` of a ~15 m grid), `lat`, `lon`, `count`, `first`, `last` (epoch s) | 3.9: where this host scanned from, so the estimator knows where an AP was *not* heard |
| `elevation` | `cell`, `elev`, `time` | elevation cache per ~100 m cell |
| `achievements` | `key`, `unlocked` | milestones |
| `flock_cameras` | `id` PK (`osm:<node\|way>/<n>`, `flock:<n>`, `det:<mac>`), `lat`, `lon`, `source` (`deflock`, `osm` = Overpass, `community` / `3rd Party / Suspected` = flocklocations community reports, `wifi_scan` / `ble_scan`), `model` (the source's, "" unknown), `operator` (the agency, "" unknown), `direction` (the tag text; DeFlock's directions joined with `;`), `bssid`, `ble_mac`, `confidence`, `detection_method`, `first_seen`, `last_seen`, `sighting_count`, `vetted`, `vetted_at`, `notes`, `seq`, `pass_count`; 3.10: `camera_type` (`alpr` · `webcam` · `ptz` · `cctv` · `enforcement` · `not_camera`), `tags` (the raw OSM tags, compact JSON, when known); 2026-10: `manufacturer` (OSM `manufacturer` / `brand`, DeFlock's `brand`), `osm_version`, `osm_timestamp` (the OSM element's, from DeFlock or Overpass meta), `stale` (1: no longer confirmed by its source, kept only for its passes); Phase D: `trust` (0–1, NULL = not computed yet; [SIGHTINGS.md](SIGHTINGS.md) §2.7), `trust_detail` (JSON: logit, terms, computed), `verdict` (`present` · `absent` · ''), `verdict_at`, `watched_way` (the OSM way id the camera reads, §2.6), `watched_detail` (JSON: basis, distance, bearing, layer, oneway, the camera position it was chosen for), `ways_fetched` (when its roads were fetched; refetched after 90 days), `agency_portal` (the Eyes on Flock portal slug, `~slug` when the state was not verified, §4.6). These columns survive the `INSERT OR REPLACE` of a camera refresh | the surveillance-camera map ([SIGHTINGS.md](SIGHTINGS.md) §2.0: DeFlock, Overpass, flocklocations community reports, our own RF detections, [DETECTION.md](DETECTION.md)). `camera_type` is decided at import from the tags; every row is reclassified once per classifier version (kv `camera_type_v3`); an OSM row without stored tags is `alpr` when DeFlock listed it, else classified from its model until a pass fetches its node. `pass_count` = its camera passes. A stale camera gets no new passes |
| `plate_events` | `id`, `uid` UNIQUE, `kind`, `plate`, `time`, `lat`, `lon`, `acc`, `camera_id`, `camera_lat`, `camera_lon`, `distance_m`, `speed_kmh`, `heading_deg`, `approach_bearing_deg`, `camera_dir_deg`, `facing`, `operator`, `agency`, `model`, `camera_type`, `source`, `source_url`, `source_name`, `confidence`, `leaky`, `details`, `metrics` (JSON), `raw` (JSON), `device`, `created_at`, `updated_at`, `seq` | 3.10: ALPR / camera passes and plate searches ([SIGHTINGS.md](SIGHTINGS.md) §1); synced through `/db/changes` (records only) |
| `plate_event_media` | `id`, `uid` UNIQUE (32 hex of SHA-256 of `data`), `event_uid`, `camera_id`, `kind` (`dashcam` · `camera_photo` · `webcam`), `mime` (`image/jxl` · `image/webp`), `data` BLOB, `width`, `height`, `bytes`, `original_url`, `original_mime`, `original_bytes`, `original_sha256`, `jpeg_reconstructible`, `attribution`, `license`, `captured_at`, `created_at` | 3.10: the images, stored losslessly (§3.1); never synced (they stay on the node that has them); `webcam` stills never leave the device |
| `plate_event_alias` | `uid` PK, `target` | 3.10: a pass uid merged into another (§1.1) → the survivor, so a phone's later media upload under its own uid lands on the right event |
| `osm_ways` | `id` PK (the OSM way id), `highway`, `name`, `ref`, `layer`, `bridge`, `tunnel`, `oneway` (0 · 1 along the node order · −1), `nodes` (JSON ids), `geom` (JSON `[[lat, lon]…]`), `min_lat`, `max_lat`, `min_lon`, `max_lon`, `fetched` | the drivable OSM ways within 80 m of passed cameras, from Overpass (ODbL), for road snapping ([SIGHTINGS.md](SIGHTINGS.md) §2.6); cached 90 days; not synced |
| `eof_portals` | `slug` PK, `url`, `city`, `county`, `state`, `type` (`PD` · `SD`), `population`, `cameras`, `searches`, `retention_days`, `vehicles`, `hotlist_hits`, `hotlist_rate`, `shared_with`, `received_from` (counts), `prohibited_uses`, `public_audit`, `updated`, `tokens` (the place-name tokens of the join), `fetched` | Eyes on Flock's Flock transparency portals (**CC BY-SA 4.0**, kept apart and attributed; [SIGHTINGS.md](SIGHTINGS.md) §4.6), replaced by the weekly fetch; not synced |
| `plate_audits`, `camera_encounters` | (3.9) | no longer written; `/flock/audits` and `/flock/encounters` are fed from `plate_events` |
| `kv` | `key`, `value` | misc: `poi_lat`/`poi_lon`/`poi_radius`/`poi_time` (where and when the places were fetched), `peds_lat`/`peds_lon`/`peds_radius`/`peds_time` (the same for the pediatric ER search), `countryCode`, `environment`, `sync:<peer>:pulled`/`pushed`, `seq`, `schema`, `created`; 3.9: `estimator_version`, `estimator_kappa`, `estimator_calibration` (JSON), `device_offsets` (JSON, device → dB); 3.10 ([SIGHTINGS.md](SIGHTINGS.md)): `plate_events_backfill` (the backfill's progress), `hibf_watch` (the plate-search watcher's schedule), `hibf_sources` (the leaky-agency list), `camera_photo_checked:<camera_id>` (`{"checked","found"}`), `camera_type_v2`; 2026-10: `camera_type_v3`, `camera_clean_v1` (the one-time camera cleanup's summary), `camera_source_deflock` (when DeFlock last replaced the bulk rows), `camera_sync` (the last camera sync: counts before / after, DeFlock added / updated / unchanged, reconcile deleted / stale, community rows added); Phase D: `eyesonflock` (the Eyes on Flock fetch: status `ok` · `error` · `blocked`, error, fetched, lastAttempt, portals, bytes, summary, camerasMatched, license, attribution) |

## Migration

On the first start of 3.3 or later the pre-existing JSON state (`aps.json`, `history.jsonl`,
`pois.json`, `elev.json`, `achievements.json`) is imported once and the files are renamed
`*.migrated`. `state.json` (the current fix) stays a plain file so `beaconfix --json` can
answer without the tray. If the database cannot be opened (no key), the tray keeps working
without persistence and says so in the fix card and the log.

### The camera map (2026-10)
The nationwide camera sync (Telegram `/sync`, `POST /api/v1/flock/sync-us`, D-Bus `SyncNationwideUsCameras`; once by
itself, two minutes after the first start, when the old bulk import's rows are present) runs three steps:
1. **DeFlock** — `https://data.dontgetflocked.com/cameras.geojson.gz` (plain JSON, ~38 MB, ~143k ALPRs; data ODbL,
   pipeline MIT) is streamed to `~/.cache/sworrl/beaconfix/cameras/` and read one feature at a time on a worker
   (`src/cameraimport.cpp`); rows are upserted in batches of 5000. A row with OSM tags (from Overpass) keeps them
   and only gains the OSM version / timestamp / manufacturer; a tagless row takes DeFlock's values.
2. **Reconcile** — only after a complete DeFlock list (≥ 10 000 cameras): the old bulk import's OSM rows (`source`
   `3rd Party / Suspected`, `openstreetmap`, `flocklocations`) and earlier DeFlock rows that DeFlock no longer
   lists — deleted nodes, gunshot detectors, anything not an ALPR — are **deleted**, or only marked **stale** when
   `plate_events` has a pass at them. Overpass rows, community reports and RF detections are not touched.
3. **flocklocations.com** — only its community submissions (no `osm_id`, CC BY 4.0) are added; its OSM-derived rows
   are DeFlock's.

If DeFlock cannot be downloaded or read, the sync falls back to Overpass, US sector by sector (ALPRs only).

Once, on the first start with this change (kv `camera_clean_v1`): rows with OSM tags take operator / manufacturer /
model from them; "Flock Safety" as the operator of a tagless row (written by the old bulk import and the RF detector,
never data) is cleared — `manufacturer` keeps Flock where the model or the detection said so; RF detections made with
the retracted signatures are re-checked against the current rules and deleted (or marked stale when they have
passes). Then every row is reclassified (`camera_type_v3`).

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
