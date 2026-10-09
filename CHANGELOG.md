# Changelog

All notable changes to BeaconFix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [lite 1.0.0] — 2026-10-09

The first release of **BeaconFix Lite** ([lite/README.md](lite/README.md)): BeaconFix's Wi-Fi locator as a small
library for devices that only need to know where they are. Lite releases are tagged `lite-vX.Y.Z` and versioned
apart from the desktop and app.

- **Estimator:** BeaconFix's `selfLocate` (robust range trilateration, Levenberg–Marquardt, Huber weights, a
  chi-squared integrity check that drops disagreeing APs), with a 1-sigma and a 95 % radius on every fix. New
  over the desktop's: a median start and a screen for APs mapped far beyond the rest. A loud AP mapped 2 km away
  (your own router after a move) used to outvote ten honest ones; now it's dropped and the fix stays put.
- **Reuse:** the same APs as the last fix (a weighted overlap of the strong ones) under 6 hours old returns that
  fix with no solve, network or disk: about 1 ms on the MikuOS M500.
- **Offline first:** a binary AP cache (20 bytes an AP, an append-only journal compacted when it reaches a
  quarter of the main file). Apple's neighbours (~100 per request) are all kept, so most looks need no network.
  APs nobody knows are remembered for 30 days instead of being asked about each time.
- **Moved and travelling APs:** opt-out SSIDs, travelling names (hotspots, cars, trains, Starlink) and the
  caller's own SSIDs are never sent or used. An AP the integrity check drops twice is treated as moved for 30 days.
- **Learning:** `teach()` a trusted fix (GPS, a BeaconFix desktop) or `import()` a desktop's placements; the APs
  around a good fix are learned at low weight, so a place visited before is solved offline.
- **Android:** `BeaconFixLite` uses the system's scan when it is under 5 minutes old instead of asking for one,
  and `Pacer` spaces looks out to 2 hours while nothing changes.
- **Targets:** Android 5.0+ (API 21), Kotlin 1.9+, Java 8 bytecode, no dependencies; replaces the locator
  copies in the MikuOS M500 launcher and the Frameo weather app.
- Artifacts: `beaconfix-lite-1.0.0.aar`, `beaconfix-lite-core-1.0.0.jar` (JVM, with a CLI that reads
  `cmd wifi list-scan-results`), `beaconfix-lite-src-1.0.0.zip`.

## [3.10.0] — 2026-10-03

Desktop 3.10.0, Plasma widget 3.10.0, Android 1.6.0 and the hub. BeaconFix is now **Apache-2.0**
(relicensed from GPL-2.0-or-later by its sole author; [docs/LICENSING.md](docs/LICENSING.md)).

### Positioning and estimation
- **Estimator 4** (desktop and Android, identical, golden-vector checked): per-device, per-AP level
  deviations δ (σ 10 dB; places never mix devices), an explicit fit of the mirror solution across the
  places' axis with the mirror mixture folded into the covariance (one-sided data: R95 coverage 70 % →
  96 % in Monte Carlo), device offsets in the incremental update, and a floor on robust weights that
  stopped a NaN posterior from silently dropping an AP. Measured on surveyed ground truth:
  [docs/GRADING.md](docs/GRADING.md).
- Estimator 3: a scaled-inverse-χ² noise scale over effectively independent places, a sandwich
  covariance for correlated shadowing, a local-posterior R95, adaptive place radius.
- Site lock, Wi-Fi fingerprinting, provider-accuracy calibration, a Kalman/RTS track smoother with a
  Gauss–Markov GPS bias, and Wi-Fi RTT (802.11mc/az) ranges to access points.

### Map
- Satellite hybrid by default with the newest free imagery (Esri Clarity, USGS, NASA VIIRS), road and
  label overlays, contour lines in feet from elevation tiles, zoom to z23, a cm/ft scale bar, area
  grouping and decluttered labels on desktop, widget and phone. Render caching and partial repaints
  (desktop frames 700–1200 ms → 5–12 ms); the widget zooms at 60 fps; a heat layer of every route.

### ALPR cameras and plate events (new: [docs/SIGHTINGS.md](docs/SIGHTINGS.md), [docs/DETECTION.md](docs/DETECTION.md))
- Camera map from DeFlock's daily ALPR-only dataset (with real directions and edit dates) plus community
  reports; every camera classified (ALPR, webcam, PTZ, CCTV, enforcement, not a camera). Non-ALPRs no
  longer raise "plate read" alerts (a traffic webcam used to).
- Camera passes on the track polyline with vendor field-of-view cones, P(read) integrating the fix
  error, road snapping (parallel roads, overpasses, the opposite carriageway), and log-odds camera trust
  with "it's there / not there" verdicts. Backfilled over all route history, incremental afterwards.
- Plate searches from released Flock audit logs (HaveIBeenFlocked), checked with 8-hex SHA-256
  prefixes on an adaptive schedule; agency context from Eyes on Flock.
- Images in the smallest lossless form (JPEG XL recompression / lossless JXL or WebP): camera photos
  (OSM, Wikimedia Commons, Panoramax), dash-cam frames, opt-in traffic-webcam stills.
- Sightings tab and event dialog (metrics, images, raw record, View source, Show on map); honest alerts.
- One shared, tiered surveillance-signature file for desktop and phone; generic ESP32 and consumer OUIs
  that caused false alerts removed; Flock's own OUI added.
- Camera-avoidance routing through OpenRouteService or GraphHopper (your key).

### Android 1.6.0
- Sightings screens, live pass detection, dash-cam pass frames, the phone-side plate watcher, plate
  events over the LAN and through the hub (Room v8), Open-source licenses screen.
- ALPR engine: per-vehicle tracking and multi-frame fusion, hotlist matching on the per-character
  candidate lattice, short-shutter capture, bilinear crop sampling (40 px plates read 21 % → 83 %),
  US plate-format rules with a GPS state prior, 416 px detector tiles, thermal-headroom steps, an
  optional larger OCR model, ONNX Runtime 1.30. The plate detector is downloaded on first use (its
  weights' license is unclear); a clean retraining pipeline is in `tools/train-plate-detector/`.
- Estimator 4, the desktop's calibration (κ, path-loss, device offsets, misses), AP RTT ranging.

### Hub, linking and security
- The hub (`beaconfix --server`): master database, device registry, live positions and a job queue;
  nodes do the processing. BFS3 sealed API (X25519, HKDF-SHA512 ratchet, ChaCha20-Poly1305).
- Linking v3: QR or LAN discovery with a six-digit numeric comparison and a key commitment; one link
  gives the phone a LAN token and a hub enrolment. Nothing to type.
- LAN API: a CSRF guard on loopback scopes; authenticated traffic no longer counts against the rate limit.

## [3.9.0] — 2026-09-28

Desktop 3.9.0, Plasma widget 3.9.0 and Android 1.5.0: every access point BeaconFix positions
itself now comes with an honest, graded answer: where it is, how sure we are (the 95 % region,
the chance of being within 25 m), a 0–100 score and a letter, on every map. The definitions are
in [docs/GRADING.md](docs/GRADING.md).

### Desktop

#### Added
- **Estimator 2** (`src/estimator.{h,cpp}`). Samples are clustered into places (median level, the
  observer's own fix error folded in as an errors-in-variables term); a grid posterior with P0 and
  n integrated out in closed form (Gaussian priors per band, a range prior, Wi-Fi RTT ranges, and
  the places this host scanned from without hearing the AP) finds the global optimum, its modes and
  the 95 % region; a robust Levenberg–Marquardt polish (Gaussian + uniform outlier mixture with EM
  weights, AP height 3 m) runs from the grid maxima, the classic centroids and the mirror across
  the places' principal axis. The covariance is widened by the design effect of correlated
  shadowing and the correlated fix error, floored by the Cramér–Rao bound, a leave-one-place-out
  jackknife and a cluster bootstrap, and scaled by the anchor calibration.
- **Grades.** Each estimate is a *fix* (A–F), a *region* (R: one or two places, or R95 > 150 m,
  shown as a disc) or *mobile* (M: it travels, was heard 5 km apart, or its level does not fall
  with distance). The score is a weighted geometric mean of precision, geometry, evidence, fit,
  stability, freshness and agreement with a WiGLE/Apple placement, capped for ambiguous or
  extrapolated fits, with hysteresis. A moved AP keeps only its recent epoch (flag `moved`).
- **Calibration.** Anchored APs are fitted with their pin hidden (leave-one-out); the mean NEES
  sets κ, and the median error per letter is reported. Per-device level offsets are learned from
  APs heard by this host and another device. BSSIDs of one radio (same MAC but the
  locally-administered bit and the last nibble, a steady level difference in shared scans) are
  pooled into one fit.
- **Where to sample next**: the spot around each weak estimate whose sample adds the most
  information (matrix determinant lemma), in the AP JSON (`fit.suggest`) and the ten best nearby
  in `GET /api/v1/estimator` / D-Bus `EstimatorJson()` / `beaconfix --estimator`.
- The AP JSON (D-Bus, `/api/v1/aps`, `/state`) gains `grade`, `score`, `r95` and a richer `fit`
  object (`kind`, R95/CEP50/P(<25 m), the covariance, `components`, `metrics`, `flags`, `suggest`,
  `group`); `kind` gains `region` and `mobile`; `/db/changes` carries the graded fields; `hello`
  lists `grades`; `ap_refit`/`ap_placed` events add `grade`, `prevGrade`, `score`, `r95`.
- Database (additive): `estimates` gains `kind`, `grade`, `score`, `r95`, `cep50`, `p_within25`,
  the covariance, a `metrics` JSON and `version`; new tables `estimate_history` (the last 20 per
  AP) and `scan_cells`; export/import carry them. Estimates from 3.8 (or imported ones) are
  recomputed once at start-up in 150 ms batches; nothing is deleted.
- Map and beacon table: 95 % ellipses in the grade colour (Okabe–Ito, colour-blind safe), dashed
  when extrapolated or ambiguous, region discs, M markers, a grade column and the grade line
  "B · 72% within 25 m · 9 places · 3 devices" in tooltips and details.
- Tests: `cmake -DBEACONFIX_TESTS=ON` builds `estimator_test` (synthetic geometry, coverage of
  R95 on random layouts, drive-by, misses, moved/mobile APs, device offsets, RTT, hysteresis) and
  `estimator_golden`, which checks `tests/fixtures/estimator_golden.json`, the vectors the
  Android twin must reproduce exactly (`ctest`).

#### Changed
- Sampling: a new sample every ~12 m (or half the fixes' error) instead of 60 m, or ten minutes
  later at the same spot; the estimator clusters places itself. In memory an AP keeps at most
  ~600 samples (the oldest of the most crowded cell go first); the database keeps every row.
- Travelling and home APs are graded M instead of being skipped silently; one or two samples give
  a region instead of nothing.
- Self-location weighs known APs by grade and by their covariance along the line of sight, never
  uses regions or mobile APs, and runs an integrity check that excludes an inconsistent AP
  (`ok` / `repaired` / `failed` / `unverified`).
- Refit batches are time-boxed (150 ms) so hundreds of refits never freeze the tray.

#### Fixed
- The error ellipse's `orient` was a mathematical angle (anticlockwise from east) while every
  consumer read it as a bearing; it is now the bearing of the major axis in [0, 180) (0 for a
  circle).
- The old fit claimed tiny ellipses with three samples (no residual degrees of freedom, a 2 dB
  floor); the noise level now has a prior and the Cramér–Rao bound is a floor.

### Plasma widget 3.9.0
- 95 % ellipses in the grade colour (dashed when extrapolated or ambiguous, a ghost at the
  alternative), region discs batched into one path, M markers, the grade line first in the hover
  card, flags and the grade on its way in the pinned card, a "sample here" target for the
  hovered or pinned AP, an A–F legend button, and grade badges in the security list.

### Android 1.5.0
- The estimator is the desktop's, ported line by line (`estimate/Estimator.kt`) and checked
  against the same golden vectors (`EstimatorGoldenTest`); the phone grades its own fits and
  shows the desktop's when synced.
- Room 5 (migration 4→5, additive): each AP gains the grade, score, R95/CEP50/P(<25 m), the
  covariance and ellipse, places, devices and a metrics JSON; a new `estimate_history` table. Every
  AP is recomputed once after the update.
- Map: 95 % ellipses in the grade colour (dashed when extrapolated or ambiguous), region discs,
  M markers. Beacon details: grade and score, "72% within 25 m", R95/CEP50, places, devices, the
  score components and flags.
- Self-location uses the desktop's integrity-checked solver, weighing known APs by grade.
- The collector refits an AP at most every two minutes (sync, import and "refit all" always do);
  the desktop's graded fit is taken over on sync when it is the AP's shown position.

## [3.8.0] — 2026-09-27

Desktop 3.8.0, Plasma widget 3.8.0, Android 1.4.0 and Pi agent 1.1.0: finding the right emergency
room for a child far from home, and an Android app that keeps working when the connection to the RV
drops.

### Desktop

#### Added
- **Pediatric ERs.** Two civic categories, `peds_er` (Pediatric ER) and `peds_urgent` (Pediatric
  urgent care), from a separate, rarer Overpass search out to the pediatric radius (default 150 km,
  50–300: `pedsRadiusKm`, or the map's "Pediatric ER search" submenu). Each place gets a tier: 1
  dedicated pediatric ER, 2 children's hospital with the ER not confirmed, 3 general ER with a
  pediatrics department, 4 pediatric urgent care (not an ER). A children's hospital with an ER
  hospital within 600 m reads "ER on campus: <name> — call ahead"; the same place mapped twice under
  one name is merged. The classifier (`src/poiclassify.{h,cpp}`) is shared with the Android app line
  by line, and both are tested against `tests/fixtures/pediatric_tags.json` (real OpenStreetMap tags
  plus traps: pediatric dentists, behavioural health, Shriners, `emergency=no`, clinics that are not
  urgent care).
- `GET /api/v1/emergency` gains `pediatric` (the nearest confirmed pediatric ER or children's
  hospital with an ER on campus; else the nearest children's hospital or ER with a pediatrics
  department), `pediatricCloser` (a nearer but less certain site), `pediatricUrgent` (`notEr:
  true`), `pediatricNote`, `pediatricSearchKm`, `pediatricTime` and `origin`. `hospital` is now the nearest *general* ER. Every place gains
  `website`, `osm` and an estimated drive time (`driveS`, `driveM`, `driveEst`: the straight line ×
  1.4 at 70 km/h, rounded to 5 minutes).
- `/api/v1/pois` items gain `osmType`, `osmId`, `peds`, `er`, `campusEr`, `scope` (`near`/`far`) and
  the drive time; the top level gains `categories` (each with `reachKm`), `note`, `origin` and
  `pedsOrigin`; `hello` lists the `pediatric` feature. All additive: 1.3 phones and 3.7 widgets
  ignore the new fields.
- Nearby tab: Pediatric ER, Closer and Pediatric urgent care (not an ER) rows below the general ER,
  each with its confidence, distance, drive estimate and a clickable phone number, and the pediatric
  note in italics. Map clusters show their most urgent member (pediatric ER, then ER, then police or
  fire) and pediatric ERs draw on top. Tray: "Nearest help (police, fire, ER, pediatric ER)…".
  `beaconfix --nearby emergency` prints the pediatric lines; `--nearby pediatric` lists them with
  their tier.

- `GET /api/v1/devices/me` (feature `whoami`): the calling token's name, kind and current scopes, so a
  phone paired with `read` sees a later `beaconfix --grant-control`.

#### Fixed
- A hospital tagged only `healthcare=hospital` (no `amenity=hospital`) was not treated as a
  hospital.
- Overpass etiquette: the near and the pediatric query run one at a time, 5 s apart, with a minute's
  pause after HTTP 429/504, and only in the tray (`--once` from the weather widget every 15 minutes
  posted the pediatric query and quit, leaving the server busy with it in one of our two slots). Failed
  pediatric searches back off 10, 20, 40 … minutes (at most 4 h) and keep the saved answer ("Saved … —
  may be incomplete"); it is skipped for IP-only fixes.
- **The pediatric search never finished on the live server**: its six `[name~…,i]` filters made
  Overpass time out after 79 s (the same query without them: 11 s). It now asks only exact key=value
  sets (every hospital, hospital building and clinic in the boxes) and matches names locally; the client
  waits 30 s longer than the server's own timeout so a server error is read, not cut off.
- **The Urgent care pick could be a chiropractor**: `urgent` in `/emergency` (the Nearby tab, the widget,
  the phone) is now the nearest actual urgent care — tagged `urgent_care` or named like one — with
  `urgentCare: true`, or none; the `urgent` category still lists every clinic.
- **A psychiatric centre, a medical office building and a hospital department were listed as
  children's ERs** (tier 2, two of them "ER on campus"), and being nearer they pushed a confirmed
  pediatric ER inside the radius off the kept five. A hospital is no longer a pediatric ER when all
  its `healthcare:speciality` values are non-emergency ones (psychiatry, rehabilitation, dentistry, …)
  or its name reads like a residential or adolescent centre, an office or medical building, or a single
  department (cardiology, oncology, imaging, …); the same rules on the phone, with four new traps in the
  shared fixture. The far list keeps the nearest 5 confirmed pediatric ERs first, then the nearest 5
  other sites. A saved list classified by the old rules is searched again once.

#### Changed
- The map database keeps near places in `pois` with `scope`, `peds`, `er`, `campus`, `drive_s` and
  `drive_m` columns (added, no migration step), and the pediatric search's far places in their own
  table `pois_far` with `peds_*` kv keys: sharing `pois`' key, a near save replaced every far row inside
  the near radius and a restart lost the closest ERs. The window's Reload and the map's "Reload places"
  refresh both searches.

### Plasma widget 3.8.0

#### Added
- The Nearby help card lists the nearest pediatric ER after the general ER (which always keeps its
  row), a closer but less certain site and pediatric urgent care marked "Not an ER", each with
  distance, drive estimate, call button and a confidence line, plus the desktop's pediatric note. A
  **Kids ER** chip narrows the list to pediatric ERs and pediatric urgent care. Map clusters take
  the icon of the most urgent help inside them; the place card adds the ER status, campus ER, drive
  time and address. New categories reach the Places menu without re-adding the widget; with a 3.7
  desktop nothing new shows.

#### Fixed
- Fast zoom-outs uncovered the map's edges between overlay paints: the overlay now covers half a
  view beyond each edge (up to 512 px) and repaints at once when the zoom leaves [0.8, 1.25].
- With Follow off, tours, event glides and re-fits (also after a resize) still moved the camera.
- A press on a place marker, a map button, the security chip or panel, or a card button did not stop
  a running tour or glide (the Cinematic button still flies home when switched off mid-tour).
- The automatic zoom ran more often than the documented once per 10 minutes; synthesised beacon
  refits fired on moves inside the fit's own error.
- After a desktop restart (event ids start again at 1) new events were ignored until the ids caught
  up.
- The context menu did not hold automatic motion while opening and closing, and the Places menu
  logged "Menu.qml:30:26: TypeError: Cannot read property 'width' of null" at start.
- **The widget crashed plasmashell** (panel, desktop and every widget; twice on 2026-09-27, each
  after a tray restart). The radar's flash timer deleted keys from a property-var object that its
  flash() then added keys to, which crashes Qt 6.11's V4 engine (`QV4::Object::insertMember`). The
  object is now replaced, never changed in place; a stress copy of the old code crashed within seconds
  under `qml6`, the new code ran for minutes. The map's tile refresh no longer deletes keys either. (The
  pattern dates from 3.3.0.)
- Zooming in and back out before the new level loaded left the map blank until the view moved.

### Android 1.4.0

#### Added
- **Help** ("Nearest help"): the local emergency number first (always the dialer, never an automatic
  call), then what a dispatcher asks (coordinates with accuracy and age, address, town, county,
  state; Copy and Share), the nearest children's ER with its confidence in words and "call ahead"
  where the ER is not confirmed, a closer but less certain one, the nearest general ER (never hidden
  behind a pediatric one), urgent care marked "Not an ER" with open/closed, police, fire, Poison
  Control in the US, pharmacy and vet, and where the answer came from and how old it is. Distances
  and drive estimates are measured from you when your fix is fresh and precise, else from the RV.
  Works with a 3.7 desktop (children's hospitals then come from their names). Rows expose "Call …"
  and "Directions to …" to TalkBack.
- **Quick access**: a Help widget (Glance, 110×48 to 320×260), Quick Settings tiles for nearest help
  and the collector (both unlock first), launcher shortcuts (Help now, Kids ER: <name>, Share my
  location, Find the RV) with a pin-the-Kids-ER button, `beaconfix://help`, and an optional heads-up
  with the nearest help after a move of more than 25 km (at most every 3 h, never 22:00–07:00).
- **Offline mirror**: every desktop answer (location, trip, track, places, nearest help, devices,
  hello, home) is kept in the database per desktop, and screens start from it — marked as saved,
  with its age — while the desktop is out of reach. A failed or empty fetch never deletes saved
  data.
- **Phone-side help search**: with no desktop answering, opening Help or Places searches
  OpenStreetMap from the phone for the help categories only (the desktop's classifier, ported), one
  request at a time with back-off; never in the background, and it can be limited to Wi-Fi or
  switched off.
- **Places** rebuilt on the offline cache (every desktop and this phone): chips that match the
  desktop's groups (Help & medical, Kids ER, Civic, Kids & fun, Services, Open now), badges (ER, no
  ER, kids ER tier, wheelchair, Wi-Fi, open / closed / closes at), and Call / Directions / Share /
  Website / Show on map on every row.
- **Map**: places from the offline cache with pediatric ERs on top, a place sheet with the
  confidence line and drive time, a filter row, four keyless styles (streets, dark, topo, satellite
  with labels), the RV's last known position, "Show on map" from any screen, and "Ask the RV to save
  map tiles here" (control access).
- **Where's the RV** card (distance, direction and age, Navigate back, Share RV spot, our other
  devices) and **Share my location** as plain text with `geo:`, OpenStreetMap, Google and Apple
  links; positions shared *to* BeaconFix (maps links, `geo:`, "lat, lon",
  `beaconfix://map?lat=&lon=&label=`) open on the map.
- **Trip journal**: the trip live or as saved ("as of HH:MM"), the phone's own day, and stops
  grouped by day from the desktop's track or the phone's fixes; tap a stop to see it on the map; GPX
  1.1 export through the system file picker.
- **Backup**: the phone's own beacons, observations, fixes and anchors to a file, or straight to the
  RV's desktop (`/api/v1/db/import`, control access), in the desktop's export format; never the
  identity, tokens, paired desktops or settings. Restore through Import.
- **Connected Wi-Fi** card graded like the beacon audit (open, WEP and TKIP are not safe), with an
  optional alert for an open network (once per network per day, never for home networks).
- **Units** (auto from the desktop trip's country or the phone's region, metric, imperial) and a
  **System health** card (precise and background location, notifications, nearby Wi-Fi, battery
  optimisation, battery saver); the compact Home version appears only when something needs fixing.

#### Fixed
- **An update could wipe the identity**: the database fell back to a destructive migration. 1.4
  migrates 1 → 2 → 3 → 4 for real (the SQL is checked on the JVM against the exported schemas) and
  has no destructive fallback.
- **Desktop fixes were stored again on every sync**, so the fixes count grew by the track's length
  each time; each desktop fix is stored once per timestamp now, and existing duplicates collapse in
  the migration and after every sync.
- **Home networks edited on the phone were overwritten** by the next sync; they are now pushed to a
  desktop with control access first (an empty list too) and never pulled over before that.
- **The Places help card never showed an ER** (it looked for a `hospital` category the desktop does
  not have), and the Help, Food, Camping and Fuel & EV chips never matched the desktop's groups.
- The status notification offered four actions but Android shows three, so "Map" never appeared: now
  Help, Scan now and Pause/Resume, with the nearest help in the expanded text and only "BeaconFix ·
  running" on the lock screen.
- Desktop refresh: a timeout was reported as success, a second paired desktop discarded the first
  one's answers, one unreadable endpoint failed the whole refresh, and cancellation was swallowed.

- **The Identity screen crashed every time it opened** (since 1.3.2): its first frame, before the
  record loads, nested the scrolling onboarding screen inside its own scroll. It shows a spinner now.
- **A launch action replayed on rotation**, the scheduled dark theme or a restore: the tile, widget,
  notification and shortcut actions (Help, Kids ER, Share my location, Where's the RV) and shared map
  links ran again each time the activity was recreated. They are handled once.
- **Help "For the dispatcher" could show the last campground's address** with only "(saved)" after it
  when the phone moved and had no signal. A saved address now stands in only within 500 m of where it
  was resolved; otherwise the card says to read the coordinates. Copy / Share mark a saved address, say
  "RV position (phone has no recent fix)" when the origin is the RV, and add "Fix taken HH:MM (N min
  ago)" to an old fix; Share my location does the same for its last-fix fallback and never adds a saved
  address.
- The Home and Places help cards now always show the confidence text ("call ahead", "Not an ER").
- "No pediatric ER mapped within 150 km" was shown when the search had not finished (Overpass busy); it
  is shown only after a completed search, else "Children's ER search didn't finish — go to the nearest
  ER".
- The Help widget's largest layout lost its last lines ("saved … ago"): Glance drops everything after
  10 children of one container, so the rows are grouped now.
- Urgent care on Help, the widget and the tile is an actual urgent care, not the nearest clinic.
- A token upgraded with `beaconfix --grant-control` after pairing stayed `read` on the phone (no push,
  no "Send to the RV"); sync re-reads the scopes from desktops with `whoami`, and the incremental-sync
  cursor is kept (every sync overwrote it with the old one).
- **The phone's own children's ER search never finished**: it still sent the desktop's old query, whose
  `[name~…,i]` filters made Overpass give up after 79 s while the phone hung up at 75 s, on both mirrors
  back to back, every time Help or Places opened after the back-off. It now sends the desktop's
  exact-tag query (`[timeout:90]`, the phone waits 120 s) and matches names locally with the same
  classifier rules as the desktop, waits 5 s before trying the second mirror, and backs off 10, 20, 40 …
  minutes (at most 4 h) after failures in a row.
- **"For the dispatcher" said the fix was about 20 000 days old** while a refresh ran after a cold
  start (tile, widget and shortcut launches): the age was counted from the previous refresh, 0 then.
  The snapshot now carries the fix's own time; Copy / Share use it for "Fix taken HH:MM".
- **After the phone moved with no new data, the Help widget, the tile, the notification and the Kids ER
  shortcut kept the old spot's nearest places**: they took the newer data, not the newer computation.
  They now take the most recently computed answer.

#### Security
- `MainActivity` is exported, so any app could send `--es action forget_identity` (wiping the
  identity) or the `import_host` / `import_code` extras. Those, and the new `sim_offline` /
  `sim_no_desktop` test switches, now work only on a debug build or with Settings → Developer
  automation on.
- **`show_when_locked` is one-shot**: it used to keep the activity over the keyguard until it was
  destroyed, so a locked phone left on BeaconFix by an adb capture opened Help, Settings and identity
  export to anyone. It now holds for that launch only; the activity finishes when it stops.
- **A link offer or statement from outside the app** (a tapped `beaconfix://link/…`, another app's
  `link_payload` extra) no longer links on its own — linking lets that device sign in as you and
  receive your history. It waits on the Identity screen for Link / Cancel; only the in-app QR scanner
  acts at once. A place's "Website" opens http(s) addresses only (an OSM `website` tag could hold
  `beaconfix://…`).
- **Any link or app could pair the phone with a host of its choosing** (`beaconfix://pair?host=&port=`,
  a browsable link, or the `pair_host` extras): the Pair screen connected and paired without asking,
  and a rogue host approves itself. A paired desktop receives the phone's unsynced history and feeds
  Help, the widget, the tile and the Call button. An outside request now only fills in the address and
  asks ("Pair with …? Check this desktop / Cancel"); only adb automation (a debug build or Developer
  automation on) pairs at once. The host must look like a host name or address and is encoded into the
  route (it could add `&…` route parameters). (Since 1.3.)
- **The red Call buttons dialled whatever number a paired desktop sent** (Help, the Home and Places help
  cards, the Help widget). A desktop's number is used only when it is made of the emergency codes the
  desktop's own table has for the country (e.g. "911", "112 / 999", "110 police / 119
  fire+ambulance"); anything else falls back to the phone's own number for the country.

#### Changed
- No new permissions; background location is asked for only from the System health card. Predictive
  back is on (`enableOnBackInvokedCallback`).
- Database version 4: `pois` becomes a per-source place cache and `snapshots` is new; the other
  tables keep their schema.

### Ranging (desktop, Pi agent 1.1.0, Android 1.4.0)

#### Fixed
- **Long-running range filters were overconfident** (a desktop that had ranged for hours read 1.0 m
  [0.87, 1.15] for a true 0.6 m). A still BLE link's offset was allowed to drift only 0.6 dB per √hour;
  it is now 3 dB per √hour (`P_bb += 2.5·10⁻³·Δt`), identically in C++ and Kotlin. In a new 2-hour
  simulation (RTT for 10 min, then BLE only, links drifting) the 16–84 % interval now holds the truth
  in 82 % of runs (38 % before) and the RMS error drops from 0.238 m to 0.137 m. The shared test vectors
  are regenerated (`filter` changes, `filter_drift` is new; RANGING.md §11).
- **Calibration: the RTT offset's uncertainty came from the outliers.** It used the largest burst σ of
  the whole window, rejected bursts included; it now uses the cluster's bursts only (the larger of their
  RMS reported σ and their scatter). When three or more bursts do not agree (fewer than 3, or under
  30 %, in one 2 m cluster) the calibration **fails and changes nothing** — no offset, no BLE model, not
  marked calibrated — instead of calibrating the BLE links against a distance RTT disputed; the event
  log and the new `calib.last` say why. Fewer than 3 bursts (a phone in Doze) still calibrate BLE alone.
- **Automatic BLE learning** is gated on an explicit `calib.rttCalibrated` (kept in `ranging.json`)
  instead of inferring it from the offset's variance.
- **Pi agent: no TX power in its advert** (byte 8 was 127, no TX-power AD), so the desktop's up link
  from it used the −59 dBm "unknown" reference. The agent now requests 7 dBm through BlueZ when
  `CanSetTxPower` is offered, carries the level (or the one BlueZ reports as selected) in byte 8, and
  always asks for the TX-power AD so scanners can fall back to it. Its kind bits come from its config
  (`gnss` → 4, else 3) and are never a desktop's. Agent version 1.1.0; redeploy it with
  `agent/install-on-pi.sh --keep-token`.
- **The desktop dropped an old agent's advert as its own**: an advert with a desktop's tag now counts as
  that desktop (or as us) only when its kind bits say desktop or laptop; an agent from before 3.7 (our
  tag, kind pi) is resolved like an unknown advert and bound to the Pi that is posting `/ranging`.
- **Down-link TX reference without `CanSetTxPower`**: while the desktop's byte 8 says 127, its down-link
  prior uses the TX power the peer reports for our advert (its TX-power AD fallback) instead of −59 dBm.
- **A drifted RTT offset rewrote a manual calibration.** Three hours after a 0.6 m calibration the
  Pixel's bursts read 1–6.5 m below the calibrated offset; the range slid to 2 cm [1, 5 cm] and the
  automatic BLE learning, which only asked "was RTT ever calibrated?", rewrote both BLE models. The
  desktop now marks the offset stale when the median of 10+ recent bursts puts the range below
  `−max(1 m, 3σ)` (`calib.rttStale`, an event, a line on the phone's range card): RTT stays out of the
  fusion until the next calibration, the range's interval widens, and learning also needs the range to
  have held within ±12 % for 30 s with no RTT outlier. Recalibrate a pair that shows it.
- **A stale RTT offset left the BLE models it had taught at full confidence**: only the range's variance
  was widened, once, and BLE updates through those models narrowed it again within minutes (0.21–0.33 m
  ± 0.1 m 55 minutes later, models at P0 −68 / −83 dBm against the calibration's −79 / −94). A manual
  calibration now keeps a copy of both links' models (`rlsDownCal` / `rlsUpCal` in `ranging.json`); a
  stale offset puts them back and returns both BLE offsets to their prior, so the interval stays wide
  (×/÷ 2) until a recalibration.
- **The desktop's BLE advert never recovered from a failed registration**: a `RegisterAdvertisement`
  that bluetoothd answered late (NoReply, then AlreadyExists) was read as "TX power not supported" and
  given up for good, leaving a stale advert on the air with `txPower` 127. Only shape errors drop the
  TX-power AD or TxPower now; other errors unregister and retry (2 s doubling to 60 s), and a bluetoothd
  restart is noticed and both advertising and scanning start again. The Pi agent does the same.
- **A wedged scanner looked healthy**: `/ranging/info` said `scanning: true` for ~50 minutes of silence.
  A minute of scanning without any LE advert now reports `scanState: "stalled"` (and `scanning: false`),
  logs an event and restarts discovery once a minute until adverts arrive again.
- **BLE on the desktop**: `UnregisterAdvertisement` (every tag rotation, interval or flag change) was a
  3 s blocking D-Bus call on the tray's GUI thread; it is asynchronous now. The once-a-second repeat of a
  held RSSI (BlueZ reports only changes) no longer counts as a new sample: it weights the level by time
  but `N_eff` counts reported samples only, and a window of repeats alone is no update.

#### Changed
- **Android 1.4.0: RTT bursts are paced.** With the app open or the collector on, 1.3.x fired a burst every
  2.5 s for as long as the session lasted. Once the fused distance has held for 2 min with neither side
  moving, the phone now sends one burst every 30 s (`rttState` `slow`), and posts with each burst and at
  least every 10 s (BLE samples in between are kept for the next post). Back to a burst every tick when the
  phone or the desktop moves, the desktop's BLE level moves by more than 6 dB, the distance leaves its band,
  or the user opens the app or a ranging view. Unit-tested (`RttPacerTest`).
- `GET /api/v1/ranging` `calib` gains `rttCalibrated`, `last`, `rttStale`, `rttStaleByM` and `rttStaleAt`;
  `/ranging/info` `ble` gains `scanState` (additive).

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
