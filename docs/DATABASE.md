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
  `--db-import <file>` (merge; rows are keyed, so importing twice is harmless).
- D-Bus: `DbStats()`, `DbExport(path)`, `DbImport(path)`.
- API: `GET /api/v1/db/stats` (read), `GET /api/v1/db/export` and
  `POST /api/v1/db/observations` (control) — a laptop can feed its observations into the main
  machine's map, and any device can ask `POST /api/v1/locate` to be positioned from it.

## Backup

Copy `~/.local/state/beaconfix/beaconfix.db` **and** the key: either
`~/.config/sworrl/beaconfix.key` or the `BeaconFix/mapdb-key` entry of your KWallet. One is
useless without the other. A `--db-export` JSON dump is an unencrypted alternative; treat it as
sensitive (it is a history of where you have been).
