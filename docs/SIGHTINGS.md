# Plate events: ALPR camera passes and plate searches

What BeaconFix records about **your vehicle and surveillance cameras**, where every number comes from, and how
to check it. Two kinds of event:

| kind | what it means | where it comes from |
|---|---|---|
| `camera_pass` | your route passed close to a known ALPR camera, so the camera **probably read your plate** | your own fixes (desktop, phone, synced trips) against the camera map (OSM / DeFlock, community, detected) |
| `plate_search` | an agency **searched for your plate** in Flock's system | Flock audit logs released through public-records requests and transparency portals, indexed by [HaveIBeenFlocked](https://haveibeenflocked.com) |

Nothing public holds photos of *your vehicle being read*: Flock keeps those. The images stored with an event
are therefore (1) the frame **your own phone's dash cam** captured as you passed the camera, and (2) public
photos **of the camera** (OSM `image` / `wikimedia_commons` / `panoramax` / `mapillary` tags, or the nearest
Panoramax street picture). Alerts say exactly this — "passed an ALPR camera, your plate was likely read" — never
"you were recorded in public records".

## 1. Storage (MapDb, SQLite; the hub runs the same code)

```sql
CREATE TABLE plate_events (
  id INTEGER PRIMARY KEY,
  uid TEXT NOT NULL UNIQUE,          -- deterministic, see §1.1: the merge key between desktop, phone and hub
  kind TEXT NOT NULL,                -- camera_pass | plate_search
  plate TEXT,                        -- display plate ("ABC-1234"); '' when unknown
  time TEXT NOT NULL,                -- local ISO (MapDb::localIso convention): closest approach / search time
  lat REAL, lon REAL, acc REAL,      -- our position at the closest approach (camera_pass)
  camera_id TEXT, camera_lat REAL, camera_lon REAL,
  distance_m REAL, speed_kmh REAL, heading_deg REAL,
  approach_bearing_deg REAL,         -- bearing camera → us at the closest approach
  camera_dir_deg REAL,               -- the camera's facing (OSM direction / camera:direction), NULL unknown
  facing INTEGER,                    -- 1 the camera saw our front or rear plate (§2.3), 0 it did not, NULL unknown
  operator TEXT, agency TEXT, model TEXT,
  camera_type TEXT,                  -- alpr | webcam | ptz | cctv | enforcement | not_camera (§2.0)
  source TEXT NOT NULL,              -- live_route | route_backfill | phone_live | dashcam | haveibeenflocked
  source_url TEXT,                   -- "View source"
  source_name TEXT,
  confidence INTEGER,                -- 0–100 (§2.4, §4.3)
  leaky INTEGER DEFAULT 0,           -- the camera's agency publishes its audit logs (§4.4)
  details TEXT,                      -- one honest sentence
  metrics TEXT,                      -- JSON object: every other number (§2.2, §4.2)
  raw TEXT,                          -- JSON: the source record as received (OSM tags, the audit-log row)
  device TEXT,                       -- who recorded it: '' this host, else the device name
  created_at TEXT, updated_at TEXT,
  seq INTEGER DEFAULT 0              -- sync feed
);
CREATE INDEX plate_events_time ON plate_events(time);
CREATE INDEX plate_events_camera ON plate_events(camera_id, time);

CREATE TABLE plate_event_media (
  id INTEGER PRIMARY KEY,
  uid TEXT NOT NULL UNIQUE,          -- first 32 hex of SHA-256 of the stored bytes
  event_uid TEXT,                    -- dash-cam frames: the event they belong to
  camera_id TEXT,                    -- camera photos: shared by every pass of that camera
  kind TEXT NOT NULL,                -- dashcam | camera_photo | webcam
  mime TEXT NOT NULL,                -- image/jxl | image/webp
  data BLOB NOT NULL,
  width INTEGER, height INTEGER, bytes INTEGER,
  original_url TEXT, original_mime TEXT, original_bytes INTEGER, original_sha256 TEXT,
  jpeg_reconstructible INTEGER DEFAULT 0,  -- JXL made by lossless JPEG recompression: djxl gives the original back bit for bit
  attribution TEXT, license TEXT,
  captured_at TEXT, created_at TEXT
);
CREATE INDEX plate_event_media_event ON plate_event_media(event_uid);
CREATE INDEX plate_event_media_camera ON plate_event_media(camera_id);
```

The media of an event = its dash-cam frames (`event_uid`) ∪ the photos of its camera (`camera_id`).
`plate_audits` (the old route-cross-reference table) is no longer written; `/api/v1/flock/audits` maps plate
events to its old shape for older apps.

### 1.1 uid and merging
* `camera_pass`: `pass:<camera_id>:<unix minute of the closest approach>`. A pass arriving from another source
  for the **same camera within ±10 min** of an existing one is the same pass: it merges into it (keeps the
  existing uid; fills NULL fields; keeps the higher-confidence metrics; prefers `phone_live`/`dashcam` metrics
  over `route_backfill`, since the phone saw it live; media attach to the surviving uid).
* `plate_search`: `hibf:` + first 24 hex of SHA-256 of
  `org_id|search_time_utc|license_plate_hash|case_number|reason|upload_id` (missing fields as empty strings).

## 2. Camera passes

### 2.0 What kind of camera
**Where the cameras come from** ([DATABASE.md](DATABASE.md) has the sync):
- **DeFlock** (`https://data.dontgetflocked.com/cameras.geojson.gz`, daily; ODbL): OSM `man_made=surveillance`
  + `surveillance:type=ALPR` only — rows `osm:<type>/<id>`, source `deflock`, with the real OSM version /
  timestamp, every direction, the brand as `manufacturer`. Tagless, they are `alpr`.
- **Overpass** (around the fix, and the fallback when DeFlock fails): every branch requires an ALPR
  `surveillance:type` token (ALPR, ANPR; ALRP, APLR, AMPR at reduced confidence); the raw tags,
  `manufacturer` / `brand` and `contact:webcam` are stored, nothing is defaulted.
- **flocklocations.com** community reports (rows without an `osm_id`, CC BY 4.0): `flock:<id>`.
- **Our own RF detections** ([DETECTION.md](DETECTION.md)): `det:<mac>`, manufacturer Flock Safety, operator
  unknown; a Raven is `not_camera`.

Older maps also hold non-ALPR OSM objects from earlier, broader queries (CCTV domes, a WV511 webcam, a Condor).
`camera_type` is decided from the raw tags, **in this order**:
1. `enforcement` — `highway=speed_camera`, or member of a `type=enforcement` relation.
2. `not_camera` — `surveillance:type` = `gunshot_detector` | `guard`, or a traffic counter
   (`man_made=monitoring_station` + `monitoring:traffic`).
3. `alpr` — a `surveillance:type` token in {ALPR, ANPR} (the misspellings ALRP, APLR, AMPR at reduced confidence),
   or `camera:type=ALPR`, or `surveillance=ANPR`. (`contact:webcam` on an ALPR is a tagging conflict: flag it.)
4. `webcam` — `surveillance:type=camera` with `contact:webcam` (e.g. WVDOT CAM064 near Morgantown, a WV511
   traffic camera an earlier version wrongly alerted on).
5. `ptz` — `manufacturer=Flock` + `surveillance:type=camera` (Flock Condor PTZ): not a plate reader.
6. `cctv` — anything else.
Non-OSM rows (community / suspected lists, RF detections of Flock hardware) are `alpr` with their source's
confidence — except a gunshot detector (a Raven detection, a "gunshot" label: `not_camera`) and a list's
"other surveillance camera" (`cctv`). An OSM row without stored tags is `alpr` when DeFlock listed it, else
classified from its model until a pass fetches its node. Never default an untagged camera to operator
"Flock Safety" / model "ALPR". `surveillance:zone`
(traffic, street, parking) says nothing about the type: most Flock ALPRs are zone=traffic.
**Only `alpr` passes claim "your plate was likely read" and alert.** Other camera passes are recorded silently
(confidence ≤ 40, details e.g. "passed a traffic webcam — it does not read plates").

**Public webcams.** For `webcam` cameras, a **live** pass (never backfill — nobody archives these feeds) may fetch
the feed's current still. wv511.org has no still images: the camera list is
`https://wv511.org/wsvc/gmap.asmx/buildCamerasJSONjs` (id = `md5`, e.g. `CAM064`) and each camera is an HLS stream
`https://vtc{1,2,3}.roadsummary.com/rtplive/<id>/playlist.m3u8` (host from the list); a still = the first frame of
one segment (`ffmpeg -frames:v 1` on the desktop). **WV511's terms forbid storing its images "in any … retrieval
system"**, so webcam stills are **off by default** and per-provider opt-in (setting `webcamStills`), kept on the
device only, never served onward or synced. Match an OSM camera to a DOT camera by `ref`, then the id in
`contact:webcam`, then the nearest within ~100 m.

### 2.1 Detection (desktop live, desktop backfill, phone live — one algorithm)
A **pass** is a run of consecutive route fixes within **65 m** of a camera, split where fixes are > 5 min apart.
Its **closest approach** is the minimum distance from the camera to the *polyline* of the run (each segment
between consecutive fixes, not just the fix points), with the time interpolated along that segment.

### 2.2 Metrics (columns + `metrics` JSON)
`distance_m` at the closest approach; `speed_kmh` = segment length / Δt of the segment holding it (a fix's own
speed when it has one); `heading_deg` = that segment's bearing; `acc` = the interpolated fix accuracy;
`approach_bearing_deg` = bearing camera → us. `metrics`: `dwellS` (time within 65 m), `fixes` (in the run),
`fixDevices`, `fixSources`, `enterDistanceM`, `exitDistanceM`, `frontBearingDeg` / `rearBearingDeg` (camera → us
10 s before / after the closest approach), `frontVisible`, `rearVisible`, `cameraDirections` (all parsed),
`plateInferred` (true: the active plate, not a per-trip record), `cameraSource` (`flock_cameras.source`).

### 2.3 Facing (field of view)
Directions: `direction` wins over `camera:direction`; a number in degrees, a cardinal (N … NNW), spelled out
(NORTHEAST) or bound (NB, EB, SB, WB), `a-b` = the clockwise field of view from a to b (its centre; `0-360` = all
round), `;` or `,` = several cameras (DeFlock's grammar, `deflock-data` `lib.mjs`, MIT); **`direction=0` means
unknown** (the DeFlock app writes 0 when the mapper gave none; DeFlock's dataset also turns `N` into 0, so a row
from it whose only direction is 0 has none); `camera:angle` is tilt, ignore it. The cone (below) also looks at
the row's `manufacturer`.
An ALPR reads only inside a narrow cone and range (vendor datasheets, DHS ALPR market survey 2025):

| model / manufacturer | half-angle | range |
|---|---|---|
| Flock Falcon (default for Flock) | 10° | 6–25 m |
| Flock Falcon LR | 7° | 15–76 m |
| Motorola / Vigilant L5F, L6Q | 12° | 8–23 m |
| Genetec AutoVu SharpV | 12° | 3–45 m |
| Verkada | 22° | 3–20 m |
| unknown fixed ALPR | 12.5° | 5–35 m |

plus a 3° margin. `frontVisible` / `rearVisible`: the bearing camera → us is inside the cone **and** the distance
inside the range at some point of the run **before** / **after** the closest approach (a camera pointing along the
direction of travel sees rear plates — Flock Falcons are usually mounted so). `facing` = frontVisible ∨
rearVisible; NULL when no direction is known. `metrics.coneHalfDeg`, `metrics.rangeM`, `metrics.inConeS` (time
inside the cone and range).

### 2.4 Confidence
`P(read) = P(in cone and range, integrating the fix error) × 0.97 (capture) × 0.93 (read) × snap (§2.6) × trust (§2.7)`;
with no known direction `P(in cone)` = the fraction of the run within range × 0.5. `confidence = round(100 × P(read))`.
`metrics.pReadBase` (before the two factors), `metrics.snapFactor`, `metrics.trustFactor`, `metrics.pRead`. The camera's
trust replaces the old × 0.7 for a camera known only from a "suspected" list (a suspected prior is −0.5 log-odds,
trust 0.38); a detector that knows no trust (the phone, until it has the desktop's) keeps the × 0.7. Non-ALPR camera
types: ≤ 40 (§2.0). A misspelt ALPR tag × 0.85 (§7).

### 2.5 Backfill
Every route fix ever recorded (`MapDb::routeFixes()`: every device and trip, synced phone trips included) is
run through §2.1 on a worker thread, in time order, in chunks; idempotent by uid; progress in kv
`plate_events_backfill` (`{"version":1,"status":"running|done","through":"<ISO>","passes":N,"started","finished"}`).
After the first full run it continues **incrementally** every 5 minutes from `through` — which is also how phone
trips that sync hours later become passes (the desktop follows the fixes' `seq` for this, `throughSeq`, since late
fixes carry old times — §7). `POST /api/v1/plate-events/backfill {"restart":true}` reruns it.
Backfill passes notify once as a summary ("Backfill: N camera passes since <date>"), never one by one.

### 2.6 Road snapping
A pass counts the camera's 65 m circle, but an ALPR reads one road: a parallel street, the other carriageway of a
divided highway or a bridge over (a tunnel under) the camera's road pass the circle without being read. The desktop
matches the trip to the OpenStreetMap roads (`src/roadsnap.cpp`, pure; `src/platewatch.cpp` fetches and caches):
- **Roads.** For every ALPR with passes (at start-up and after a backfill) and every ALPR a live pass is near, the
  drivable ways within **80 m** come from Overpass — `way(around:80)["highway"~"^(motorway|trunk|primary|secondary|
  tertiary|unclassified|residential|living_street|service|road)(_link)?$"]["area"!="yes"]; out body geom;` — with
  their node ids, geometry, `layer` (a bridge without one is 1, a tunnel −1), `bridge`, `tunnel`, `oneway` (`yes` /
  `-1`; implied for motorways, motorway links and roundabouts). Cached in `osm_ways` for **90 days**
  (`flock_cameras.ways_fetched`); **≤ 1 Overpass request per 2 s**, a 429 / 504 cools down for a minute (the camera is
  tried once more), a failure is retried after 6 h; the two Overpass mirrors of the POI search.
- **The watched way** (`flock_cameras.watched_way`, `watched_detail`): the nearest way within **40 m** whose axis at
  the camera's projection agrees with one of its directions **±35°** (ALPRs point along the road), on the camera's
  layer (its `layer` tag; a camera without one is on the ground, but may watch a bridge when nothing on the ground
  qualifies). No direction: the nearest way. A `service` way (driveway, parking aisle) wins only when it is 10 m nearer
  than a public road (store-lot ALPRs do watch them). `basis` = `direction` | `nearest` | `nearest_no_direction_match`
  | `none`.
- **Matching the pass.** The fixes of the closest approach's device within **±90 s** and **100 m** of the camera, plus
  the closest-approach point, each with the heading of its neighbouring segments, are matched by a small HMM
  (Viterbi forward and backward): states = the ways near each sample (and always the watched way); emission =
  `max(0, d − 2.5 m)² / 2σ²` (σ = max(4 m, accuracy / 1.515)) + 4 when the heading is more than **40°** off the way's
  axis + 6 when it runs against a oneway; transition = 0 on the same way, 0.3 to a way sharing a node, 5 to another
  layer, 2 otherwise. GPS errors are correlated over a pass, so the samples together weigh as at most ~3 independent
  looks (each emission × min(1, 3 / n)).
- **The verdict.** At the closest approach a way *counts as the watched one* when it is the watched way, or on its layer
  with its geometry ≤ **12 m** from the watched way's axis (a lane, a split way) — unless the trip runs **against the
  watched way's oneway** (> 120°: the other carriageway). Two hypotheses: *on the watched road* = the best path on such
  ways at every sample of the stretch around the closest approach (≤ 30 m from it and ±10 s: one instant would price
  the alternative by its transitions alone, a longer stretch would forbid turning onto the road at the camera);
  *on another road* = the best path whose state at the
  closest approach is any other way. `pWatched = 1 / (1 + e^(cost_on − cost_other))` and
  **`snap factor = pWatched + (1 − pWatched) × 0.1`**: the trip on
  another road multiplies P(read) by about 0.1; an undecidable match (coarse fixes) costs little. Verdict
  `on_watched_way` / `near_axis` (pWatched ≥ 0.8), `parallel_road` / `different_layer` / `opposite_direction`
  (≤ 0.2), `ambiguous` between.
- `metrics.snap`: `{status (ok | no_roads | no_watched_way | no_samples), verdict, factor, pWatched, watchedWay,
  matchedWay, offsetM (the matched way's distance from the watched axis), trackOffsetM (the closest-approach point's),
  watchedLayer, matchedLayer, layer (same | different), oppositeOneway, samples, candidates, watchedBasis,
  watchedDistanceM}`. A pass whose camera has no cached roads yet keeps factor 1 and is rescored when they arrive
  (stored passes: `MapDb::updatePassScore`, seq bumped only when something changed). Passes that phones send are
  rescored the same way when their fixes are on the desktop.
- Unit tests (`tests/roadsnap_test.cpp`, synthetic geometry): a correct pass (factor 1), a parallel road 22 m away
  (0.1), a bridge crossing the cone (0.1, `different_layer`), the other carriageway 9 m away (0.1,
  `opposite_direction`), a lane 8 m off the axis (1), 80 m Wi-Fi fixes on the parallel road (`ambiguous`, ≈ 0.5).
- **Heavier option for later:** a real map matcher on the whole trip — Valhalla `trace_attributes` / Meili or OSRM
  `match` (both open source, self-hosted on an OSM extract) — would add turn restrictions, routability between
  samples and lane-level geometry. It needs a routing graph (gigabytes for a US state) and a server; the HMM above
  needs only the ways around the cameras one has passed.

### 2.7 Camera trust
How likely the mapped camera is really there and working, as log-odds (`src/cameratrust.cpp`); `trust = 1 / (1 + e^−x)`
is stored per camera (`flock_cameras.trust`, `trust_detail` = the terms) and **multiplies P(read)** (§2.4).

| term | log-odds |
|---|---|
| prior: OpenStreetMap / DeFlock (`osm:` rows) | +2.2 (≈ 0.90) |
| prior: community report (flocklocations, `flock:` rows) | +1.0 (≈ 0.73) |
| prior: a "suspected" list only | −0.5 (≈ 0.38) |
| prior: our own RF detection (`det:`) / anything else | 0 |
| per year since the OSM edit (`osm_timestamp`) | −0.25 |
| Flock hardware detected by RF within 60 m at tier ≥ 2 ([DETECTION.md](DETECTION.md)): a `det:` ALPR row there, or the camera field-confirmed by a detection | +1.5 |
| … at a lower tier | +0.7 |
| the user: "it's there" | +1 |
| the user: "not there" | −1.5 |

**These weights are suggested starting values, not measured**: nobody has published how often a mapped ALPR is gone,
moved or switched off. Calibrate them against verdicts once there are enough. A detection's tier is read back from
its confidence (15 / 40 / 65 / 85 / 98). The trust is computed when first needed, again after 30 days (the age term
drifts), after a verdict, and after an RF detection within ~100 m (which clears it). The verdict:
`POST /api/v1/cameras/<id>/verdict {"verdict":"present"|"absent"|"clear"}` (control scope) or the event dialog's
**It's there / Not there / Clear** buttons; every pass of the camera is rescored. Stored in `flock_cameras.verdict`,
`verdict_at`. `metrics.trust` = the camera's `trust_detail` at scoring time.

## 3. Images

### 3.1 Lossless, smallest
* JPEG in (camera photos, almost always): **JPEG XL lossless JPEG recompression** —
  `cjxl in.jpg out.jxl --lossless_jpeg=1 -e 7` — ~20 % smaller and `djxl out.jxl back.jpg` returns the original
  file bit for bit (`jpeg_reconstructible = 1`; verified by a round trip before storing).
* Anything else (PNG, WebP, the phone's WebP-lossless frames): decode, then encode **both** `cjxl -d 0 -e 7`
  (from PNG) and WebP lossless (Qt's `webp` writer at quality 100); keep the smaller one whose decoded pixels
  equal the input's. Never a lossy result. Without `cjxl` (it is a runtime tool, libjxl ≥ 0.7): WebP lossless.
* Display: `?as=display` serves the reconstructed JPEG (JPEG-recompressed JXL), else a PNG decoded through Qt's
  `jxl` image plugin (kimageformats) — Android cannot decode JPEG XL itself. `?as=stored` serves the stored bytes.

### 3.2 Camera photos
For cameras with a pass (backfill and live), once per camera (kv `camera_photo_checked:<camera_id>`, retried after
30 days when nothing was found):
1. `osm:node/<n>` → `https://api.openstreetmap.org/api/0.6/node/<n>.json` (≤ 1 request/s): the tags are kept in
   the passes' `raw`; images from `image` (an http(s) URL to an image file), `wikimedia_commons` (`File:…` →
   Commons `imageinfo` for the original URL, MIME, size, author and licence), `panoramax` (picture id → the
   Panoramax API's HD asset), `mapillary` (only with a Mapillary token configured).
2. Else the nearest **Panoramax** picture within 25 m of the camera (STAC search).
3. Else, with a Mapillary token, the nearest **Mapillary** images within 25 m (Graph API `images?bbox=…`), those whose
   compass angle points within 60° of the camera first; two at most (CC BY-SA 4.0). Mapillary's US coverage is far
   better than Panoramax's: 8 of 9 test cameras got photos, often 360° panoramas of the camera's surroundings.
Attribution and licence are stored and shown with the image.

### 3.3 Dash-cam frames (phone)
While the ALPR dash cam runs, the phone keeps its last ~3 s of analysed frames. When a pass completes, the frame
nearest the closest approach (and, when the camera direction is known, the frame nearest the moment the camera
saw the front plate) is encoded **WebP lossless** (`Bitmap.CompressFormat.WEBP_LOSSLESS`) at its analysed
resolution, stored with the phone's pass and uploaded to the desktop, which re-encodes it per §3.1 (WebP lossless
→ pixels → the smaller lossless of JXL / WebP). The phone deletes its copy once the desktop has it (cache cap
300 MB, oldest first).

## 4. HaveIBeenFlocked plate searches

### 4.1 Request
`POST https://haveibeenflocked.com/api/search/text` `{"plates":[h…],"cursor":null|<nextCursor>}`, where each `h`
is the first 8 hex of SHA-256 of a **variant** lowercased and trimmed — the site's own scheme (a k-anonymity
prefix: the plate itself never leaves the device). Variants per registered plate: the display form and the
letters-and-digits-only form, each expanded over O↔0 and I↔1 like the site (≤ 10 per form). Answer:
`{results:[…], nextCursor, hasMore, count, total, limit}`; page while `hasMore`. User-Agent
`BeaconFix/<version> (personal plate watch)`.

The site's robots.txt disallows `/api/` for crawlers; the owner chose a low-rate scheduled check of their own
plates (§4.5). Its data arrives months to years after the searches; a hit means a search, not a stop.

### 4.2 Results
The prefix matches other plates too: a result is ours when its `license_plate_hash` (full SHA-256 hex) equals the
full hash of one of our variants. Results without a full hash are kept with `confidence 50` and
`metrics.hashVerified = false`. Stored as `plate_search`: `time` = `search_time_utc`; `agency` = `org_name`
(`org_state`, `org_locality` in metrics); `details` = "<agency> searched <plate> — <reason>"; `source_url` = the
row's `source_url` when it is http(s), else `https://haveibeenflocked.com/`; `source_name` =
"HaveIBeenFlocked · <source_org_name> audit log"; `metrics` = every field of the row (case number, timeframe,
devices / networks searched, search type, text prompt, filters, moderation …); `raw` = the row.

### 4.3 Confidence
100 with a verified full hash, 50 without.

### 4.4 Leaky cameras
An agency is **leaky** when HaveIBeenFlocked holds its audit logs. The list is `GET
https://haveibeenflocked.com/api/sources/stats` (weekly; ~2 MB; `{sources:[{upload_id, filename, source_url,
earliest_search_time, latest_search_time, total_records, …}], updatedAt}` — 2 639 files on 2026-10-02). There is
no organisation field: the agency is in the **file name** ("Chehalis WA PD_Network_Audit_12_1_2024….csv") and
often the **source URL** (a MuckRock request "…/flock-safety-search-audits-…-chehalis-police-department-…/", a
Flock transparency portal "transparency.flocksafety.com/<agency-slug>"). Keep a token set per file: the file name
up to the first `_`/digit run, plus the URL's last path slug, lowercased, words only, stop words removed
("pd", "police", "department", "sheriff", "sheriffs", "office", "county", "city", "of", "the", "network",
"audit", "organization", "search", "csv", "flock", "safety", "foi") and the two-letter state kept separately.
A camera is leaky when its `operator` (same normalisation) shares its place-name tokens with a file's tokens
and, when both have a state, the state matches. Store the derived list in kv `hibf_sources`
(`{"fetched","updatedAt","agencies":[{"tokens":[…],"state":"WA","files":N,"records":N,"latest":"…"}]}`).
Passes store `leaky` and `metrics.leakyMatch` (the file / URL that matched). (The desktop tokenises the whole file
name and every URL path segment: file names rarely start with the agency — §7.)

### 4.5 Schedule (each device runs its own; results merge by uid)
| state | interval |
|---|---|
| idle (desktop) | 7 days |
| driving (any route fix > 20 km/h in the last 30 min; on the phone: in-vehicle activity or speed) | 24 h |
| within 72 h after passing a Flock-network camera | 12 h |
| within 72 h after passing a **leaky** camera | 3 h |
Floors: ≥ 10 s between requests, ≤ 30 requests a day per device; 429 → honour `Retry-After`, else back off ×2
from 1 h up to 24 h; 5xx/network errors back off the same way. The first run checks immediately (all pages:
that is the backfill — the index is cumulative). State in kv `hibf_watch`
(`{"lastCheck","nextCheck","mode","lastStatus","lastError","hits"}`). New hits alert at once.

### 4.6 Agency facts: Eyes on Flock
[Eyes on Flock](https://eyesonflock.com) collects the public Flock **transparency portals** (1 528 on 2026-10-03):
per agency its cameras, data retention, searches, hotlist hits, vehicles captured and the agencies it shares with
and receives from. Data **CC BY-SA 4.0**: kept in its own table, shown with the attribution, never mixed into other
data. `src/eyesonflock.cpp` (pure), fetched by `src/platewatch.cpp`:
- `GET https://eyesonflock.com/api/v1/data` (~16.5 MB JSON: `{summary, portals:[{portal_url, slug, city, county,
  state, type: PD | SD, population, total_cameras, total_searches, data_retention, vehicles_captured, hotlist_hits,
  organization_count, organizations_shared_with[], receiving_organization_count, organizations_received_from[],
  prohibited_uses, public_search_audit, data_last_updated}]}`) **weekly** on the desktop (not the hub), 40 s after
  start and every 6 h a check; a failure is retried after 6 h. Stored in `eof_portals` (replaced as a whole; the
  shared-with lists are kept as counts only); kv `eyesonflock` = `{status: ok | error | blocked, error, fetched,
  lastAttempt, portals, bytes, summary, camerasMatched, license, attribution}`. A NextDNS block (an answer with a
  `blocked-by` header) is reported as `blocked`; any failure leaves the last good list in place, and without one
  the agency facts are simply absent ("Eyes on Flock unavailable (…)" in the Sightings status line and dialog).
  `BEACONFIX_NO_EOF=1` switches the fetch off.
- **The join** (the §4.4 token normaliser, `Hibf::nameTokens`): a portal's tokens are its city (PD) or county (SD)
  name; a camera's agency is its OSM `operator`, a plate search's its `agency` (`org_name`, state `org_state`). Match =
  every token of the name in the portal's tokens, the states equal when both are known, and a sheriff ("Sheriff",
  "SO") never matched to a police department ("Police", "PD", "DPS") or the other way round. Without a state a match
  is taken only when no other state's portal has the same tokens, else the state of the newest fix that has one is
  tried; both are marked `stateVerified: false` ("state not verified"). Matches are kept on the camera
  (`flock_cameras.agency_portal`, `~slug` when unverified) and redone after every fetch.
- Shown: the Sightings table's **Agency (Eyes on Flock)** column, the event dialog (retention, searches, sharing,
  prohibited uses, the portal link, the attribution) and `agencyFacts` on `GET /plate-events/<uid>`,
  `GET /plate-events?latest=1` and `GET /cameras/<id>` (`{agency, slug, portal, state, type, retentionDays, cameras,
  searches, sharedWith, receivedFrom, vehiclesCaptured, hotlistHits, publicSearchAudit, prohibitedUses, updated,
  stateVerified, source, sourceUrl, license, attribution}`). Not in the sync feed (`?since=`): it is a view.

## 5. API (LAN `/api/v1`, token scopes as noted)
| route | scope | |
|---|---|---|
| `GET /plate-events?since=<seq>&limit=&kind=` | read | `{events:[…], cursor, more}`; each event = all columns but `raw`, plus `media:[{uid, kind, mime, width, height, bytes, attribution, license}]` |
| `GET /plate-events/<uid>` | read | the full event, `raw` included |
| `GET /plate-events/media/<mediaUid>?as=display\|stored` | read | the image |
| `POST /plate-events` | control | `{events:[…]}` from a phone (passes, searches): merged per §1.1 → `{accepted, uids}` |
| `POST /plate-events/<uid>/media` | control | `{kind, mime, data (base64), width, height, capturedAt}` ≤ 40 MB → stored per §3.1 → `{uid}` |
| `GET /plate-events/status` | read | `{backfill:{…}, hibf:{…}, hibfSources:[…], counts}` |
| `POST /plate-events/backfill` | control | start / resume (`{"restart":true}` reruns) |
| `GET /cameras/<id>` | read | the camera row, `cameraType`, `trust`, `trustDetail`, `verdict`, `roads` (`fetched`, `watchedWay`, `watched`), `agencyFacts` (§2.6, §2.7, §4.6) |
| `POST /cameras/<id>/verdict` | control | `{"verdict":"present"\|"absent"\|"clear"}` → `{camera, verdict, verdictAt, trust, trustDetail, passesRescored}` (§2.7) |
| `POST /route/avoid` | read | a route around the ALPR cameras (§8) |
| `GET /route/status` | read | which routing providers have a key (never the keys) |

Hub (`/api/v3`): `db/changes` carries `plateEvents` (records only) and `db/sync` accepts them (merge per §1.1).
**Media stay on the nodes** (the desktop): the hub host's disk is small; `/plate-events/media` is served by the node.

## 6. Alerts
* `camera_pass` of an **ALPR** (live / phone_live): "Passed an ALPR camera · <operator> <model> · <d> m" — body: "Your plate
  was likely read (camera faced you | facing unknown | camera faced away). Confidence <c> %." Actions:
  **Details** (opens the event) and **Source**.
* `plate_search`: "Your plate was searched in Flock · <agency>" — body: "<date>: <reason> (case <n>). From a
  released Flock audit log." Actions: **Details**, **Source**.
* `camera` passes: no alert (listed in Sightings; a webcam still, when captured, is shown there).
* Backfill: one summary notification.
* Desktop: the notification's Details opens the event dialog (metrics, images, raw record, View source, Show on
  map); the main window's **Sightings** tab lists every event. Phone: the notification opens
  `beaconfix://sighting/<uid>` (the event screen: the same content, images fetched from the desktop);
  More → Sightings lists them. Telegram `/audits` lists the latest events.

## 7. Desktop implementation notes (3.10)
Where the desktop (`src/plateevents.cpp` pure detection, `src/hibf.cpp` the watcher's pure parts, `src/imagestore.cpp`
the image pipeline, `src/platewatch.cpp` the moving parts, `MapDb` storage) had to pin down or differ from the text
above. Unit tests: `tests/plateevents_test.cpp`, `tests/hibf_test.cpp` (fixtures `tests/fixtures/hibf_*.json`),
`tests/imagestore_test.cpp`.

**Storage (§1).** JSON keys are the column names (`camera_id`, `distance_m` …); `metrics` and `raw` are objects (JSON
text is accepted on input, and so are camelCase aliases such as `cameraId`, `distanceM`, `cameraType`). A uid merged
into another pass is remembered in `plate_event_alias(uid, target)`, so `GET /plate-events/<uid>` and a later media
upload under the phone's own uid land on the survivor. Merge (§1.1) precisely: the same uid, else the same camera
within ±10 min (the nearest in time). A re-run on this host (`device` '' and the same `source`), or the recording
device updating its own record, replaces the row; otherwise the incoming numbers win only with a higher source rank
(`phone_live`/`dashcam` 3 > `live_route` 2 > `route_backfill` 1) or the same rank and a higher confidence, and
anything else only fills empty fields. `seq` moves only when something changed, so peers converge.

**Classification (§2.0).** Rule 1's relation membership is checked with one more OSM request
(`/api/0.6/node/<n>/relations.json`) when a passed camera's tags are fetched; a hit is kept as `"_enforcement": true`
in the stored tags. `not_camera` objects produce no passes at all. An OSM row stored before 3.10 has no tags: it is
classified from its imported model (a plate-reader name → `alpr`, otherwise `cctv`) until its first pass fetches the
node and reclassifies it — together with its passes (type, confidence, details, `raw.osmTags`). A misspelt ALPR
type multiplies the confidence by 0.85 (`metrics.typeNote`); `contact:webcam` on an ALPR is noted there too. A
non-OSM row whose model says it is no plate reader ("Other surveillance camera") is `cctv`; the list's own
confidence is kept in `metrics.sourceConfidence`, not multiplied in. Overpass imports keep every raw tag, take
`direction` before `camera:direction`, and leave operator / model empty when untagged.

**Webcam stills (§2.0).** QSettings `webcamStills` = `true` (every provider) or a list of host names
(`wv511.org`); the Sightings tab has the switch (off by default). WV511: the camera list is matched by the OSM `ref`,
then the `CAMID` of `contact:webcam`, then the nearest within 100 m; the stream host comes from the list record
when it names one, else the camera page, else `vtc1…3` are probed; one frame through `ffmpeg` (absent: no still).
HLS runs some seconds behind live, so `captured_at` is the moment of the grab and the attribution says how far it
was from the closest approach. Stills are stored with `kind = webcam` on the event, never listed by the API, never
served by `/plate-events/media`, never synced.

**Detection (§2.1).** Every device's fixes are run separately (a polyline never joins two devices), then passes of the
same camera within ±10 min are merged (the closest approach wins; `fixDevices` / `fixSources` / `fixes` combined).
A segment is interpolated only when its fixes are ≤ 5 min and ≤ 1500 m apart. A run is a chain of segments that
come within 65 m (so a pass is found even when no fix lies inside the circle); a lone fix inside 65 m with no usable
neighbour is a pass of its own.

**Facing and confidence (§2.3, §2.4).** The run's polyline is sampled every 0.2 s and every metre. A lone fix sets
both `frontVisible` and `rearVisible` from the one sample. The cone is chosen from the model, the tags'
manufacturer / brand / camera:model / model and the operator (Flock rows from the lists are "Flock Falcon"). An
`a-b` range uses its centre with the vendor cone. "Integrating the fix error": the whole track is shifted by a
Gaussian offset (fix errors are correlated over a pass), σ per axis = accuracy / 1.515 (accuracy = the 68 % radius;
≥ 1 m; 8 m when unknown), 7 × 7 Gauss–Hermite quadrature; P(in cone and range) = the weight of the shifts whose
track enters the cone inside the range. Without a direction, "the fraction of the run within range" is time-weighted
over the part inside 65 m. `metrics` adds `coneModel`, `coneMarginDeg`, `pInCone`, `confidenceAlpr` (the value as
an ALPR, so a reclassification can recompute), `startTime`, `endTime`, `suspectedOnly`.

**Backfill (§2.5).** It does not read `routeFixes()` (no device there) but the fixes table per chunk: accuracy
≤ 500 m, the route filter's teleport spikes dropped per device. Chunks are local days, each read with 30 min on both
sides; a pass belongs to the chunk holding its closest approach. Progress is the kv object above plus `phase`
(`full` | `incremental`), `throughSeq`, `chunks`, `alprPasses`, `cameraPasses`, `lastNew`, `lastIncremental`.
Incremental runs follow the **fixes' `seq`**, not their time: synced phone fixes arrive hours later with old times,
so each 5-minute run takes the time ranges of fixes stored after `throughSeq`, widens them by 30 min and re-runs all
devices' fixes there. Fixes arriving from a peer also schedule a run a minute later. An interrupted full run resumes
from `through`. The first start of 3.10 runs it 15 s after start-up.

**Images (§3).** A JPEG whose recompression cannot be verified (no `cjxl`/`djxl`) goes the pixel way (WebP lossless
of the decoded JPEG: identical pixels, not the identical file). Downloads are capped at 25 MB, at most 3 photos per
camera. Panoramax: `api.panoramax.xyz` first, then `panoramax.openstreetmap.fr` and `panoramax.ign.fr` (a picture id
is looked up on each until one has it; an empty nearest-picture answer from the meta-catalogue is final). `?as=display`
for WebP returns the WebP.

**Leaky agencies (§4.4).** File names rarely start with the agency ("2_1_2026-2_28_2026-Beecher IL PD-Network-Audit…"),
so the tokens are every word of the whole file name plus every path segment of the source URL except structural
ones (`foi`, `news`, `files` …): letters only, ≥ 3 letters, the stop words above plus generic records words
(request, records, public, general, order, sharing, alpr, transparency, portal, cpra, removed …) and month names. The
state: an upper-case two-letter state code in the file name, else the last two-letter state code of a URL slug
("santa-clara-co-ca" → CA). Files with the same tokens and state make one agency (`files`, `records`, `latest`, the
first `file` and `url`). A camera is leaky when **all** of its operator's tokens are in an agency's tokens and the
states agree when both are known; the camera's state is OSM `addr:state`, else a ", TX" at the end of its notes. The
list is refreshed weekly (a failed fetch is retried after an hour) and every pass's `leaky` is recomputed then.

**Schedule (§4.5).** The sources fetch counts toward the floors. "A Flock-network camera" = any `alpr` pass. Paging
stops after 25 pages or at the daily cap. kv `hibf_watch` also holds `failures`, `retryAt`, `lastAttempt`, `day`,
`dayCount`, `lastRows`, `lastNewHits`, `sourcesTried`. `BEACONFIX_NO_HIBF=1` switches the watcher off (tests).
A `POST /flock/crossref` (the dashboard's "Check audit logs now") asks for a check now, still within the floors.

**API (§5).** `GET /plate-events?latest=1&limit=` lists newest first (for views; the feed stays `since`-ordered).
Uids contain `/` and `:` — percent-encode them in paths (the raw form also works). `POST /plate-events/<uid>/media`
also takes `kind: camera_photo` (attached to the camera); the request body may be 40 MB (it is authenticated before
it is buffered); answers `422` for an unreadable image. `/plate-events/status` adds `active` and `tools`
(`cjxl`, `djxl`, `ffmpeg`). `POST plate-events` over the hub needs the `sync` scope.

**Alerts (§6).** Notifications carry Details (the event dialog; the backfill summary opens the Sightings tab) and
Source; Telegram gets the same honest text. A plate search without a full hash says that it may be another plate.

## 8. Routing around ALPR cameras
A route from A to B that avoids the plate readers' fields of view, through a routing provider that takes avoid areas:
`src/avoidroute.cpp` (pure: polygons, request bodies, answers, what the route still passes) and
`src/routeplanner.cpp` (the provider abstraction; network, cameras and keys injectable for the tests).

- **Providers.** [OpenRouteService](https://openrouteservice.org) directions (`POST /v2/directions/driving-car/geojson`,
  `options.avoid_polygons` a MultiPolygon; key in the `Authorization` header) and the
  [GraphHopper](https://www.graphhopper.com) Directions API (`POST /api/1/route?key=…`, profile `car`, a
  `custom_model` whose `areas` are the polygons and `priority: [{"if": "in_cam_0 || in_cam_1 …", "multiply_by": "0"}]`).
  Both need **the user's own free API key** (Settings → *Routing (avoid ALPRs)*, QSettings `routing/orsKey`,
  `routing/graphhopperKey`, `routing/provider`). Without one nothing is sent: the API answers `412 {"error":…,
  "needsKey":true}` and the map says where to get a key. The start, the destination and the polygons go to the
  provider under its terms (ORS: HeiGIT's terms of service and its free-plan quotas; GraphHopper: its terms and
  credit limits); the route is OpenStreetMap data (ODbL) and is attributed. ≥ 1 s between two requests.
- **Avoid polygons.** The ALPRs (camera type `alpr`, not stale) within the **corridor** of the straight line A–B —
  max(1.5 km, 20 % of A–B), ≤ 10 km — nearest the line first. Per known direction a **cone**: the §2.3 vendor
  half-angle + 3°, out to the far end of the range, its apex 8 m behind the mapped pole (the position is a few
  metres off); no direction: a **60 m disc**. Rings closed and counter-clockwise (RFC 7946), [lon, lat].
  **Caps** (conservative, below the providers' own limits): ORS ≤ 100 polygons / 2 000 vertices / 200 km² in all
  (ORS's public API checks the avoid area); GraphHopper ≤ 60 areas / 1 200 vertices. Cameras beyond the caps are left
  out and counted (`avoided.capped`).
- **What it still passes.** Along the returned route every ALPR whose cone (or disc) the polyline enters, sampled
  every 2 m — and, as "near, not facing", cameras within 65 m whose cone it misses. Avoidance is hard (polygons /
  priority 0), so a camera is passed only where no way around it exists (or it was capped).
- **API.** `POST /api/v1/route/avoid {"from":{"lat","lon"}?, "to":{"lat","lon"}, "provider":"ors"|"graphhopper"?}`
  (read scope; `from` defaults to the current fix; `[lat, lon]` arrays accepted) → `200 {provider, providerName,
  route: {type: LineString, coordinates: [[lon, lat]…]}, distanceM, durationS, avoided: {areas, cameras,
  corridorCameras, capped, corridorM}, passes: [{id, lat, lon, operator, model, distanceM, alongM, inCone, trust}],
  passesInCone, attribution}`; `400` bad input, `412` no key (or the key refused), `502` the provider failed (its
  message; a NextDNS block says so). Over the hub (`/api/v3`) the route needs the read scope too.
  `GET /api/v1/route/status` → `{ready, default, providers:[{id, name, hasKey, signup, terms}]}`.
- **Desktop map.** Right-click → *Route avoiding ALPRs* → *To here* (from the chosen start or our position),
  *Start from here*, *Clear the route*. The route is drawn in magenta, the ALPRs it still passes ringed in red, and a
  window lists them (double-click: show on the map).
- Unit tests (`tests/avoidroute_test.cpp`): cone / disc geometry, corridor and caps, both request bodies, mocked
  answers (success, quota, refused key, a DNS block), the cameras a detour still passes, and the planner's whole flow
  on a fake transport (no key → nothing sent). No real provider call was made: no key was available.
