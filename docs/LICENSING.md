# Licensing

## BeaconFix's own code: Apache-2.0
Every file written for BeaconFix is licensed under the [Apache License 2.0](../LICENSE): use it, change it,
ship it in open or closed products, with attribution and the patent grant. (Relicensed from GPL-2.0-or-later on
2026-10-02 by the project's sole author.) New source files may carry `SPDX-License-Identifier: Apache-2.0`.

Apache-2.0 was chosen as the most open license that still lets BeaconFix take in other people's work: permissive
code (MIT, BSD, ISC, Apache-2.0) can be mixed in freely, and Apache-2.0 is one-way compatible with GPL-3.0 and
AGPL-3.0, so BeaconFix's code can live inside copyleft components too.

## Third-party components keep their licenses
| kind of license | how it is integrated |
|---|---|
| MIT / BSD / ISC / Apache-2.0 / zlib / MPL-2.0 (file-level) | directly in the tree, under `src/third_party/<name>/` or as a dependency, with its LICENSE and an entry below |
| GPL-2.0 / GPL-3.0 / LGPL / AGPL-3.0 (code or model weights) | a **separate module**: its own directory with its LICENSE, built or downloaded as an optional component (a plugin, a helper process, a model pack). The core stays Apache-2.0; a *build or distribution that includes the component* is covered by that component's license as a whole, and its obligations apply to that build (source availability; for AGPL also to users over a network). Each such module is marked in the table below. |
| Non-commercial, research-only, or no license (code, weights, datasets) | **never bundled.** At most an optional download the user starts, with the terms shown first. |
| Data (ODbL, CC-BY, CC-BY-SA …) | attributed in the app and here; share-alike data stays in its own tables / files and is redistributed under its license |

Android packaging must keep third-party notices: the app shows them under Settings → Open-source licenses
(dependency licenses and NOTICE files are collected at build time, not dropped).

## Inventory
| component | license | where | bundled? |
|---|---|---|---|
| QR Code generator (Project Nayuki) | MIT | `src/third_party/qrcodegen.{hpp,cpp}` | yes |
| open-image-models plate detector `yolo-v9-t-640-license-plates-end2end.onnx` | code MIT (github.com/ankandrew/open-image-models); the **weights** were trained with the GPL-3.0 YOLOv9 code (WongKinYiu fork; maintainer: a "grey zone", open-image-models #22, fast-alpr #30/#47) — treated as a GPL-3.0 model pack | Android ALPR | downloaded at first use, SHA-256 pinned; not in the APK. A clean MIT retrain (MultimediaTechLab/YOLO + CC-BY data) is planned |
| planned clean plate detector (YOLOv9-t, 640 + 416 px, same end2end I/O as the open-image-models one) — *not trained yet*; pipeline in `tools/train-plate-detector/` | training code MIT (MultimediaTechLab/YOLO at a pinned commit); trained **from scratch** (no COCO or other pretrained weights) on Open Images V7 "Vehicle registration plate" (optionally pretrained on more Open Images street classes): images CC BY 2.0, annotations CC BY 4.0 (Google LLC); every image's author and licence in the generated `ATTRIBUTION.csv` | Android ALPR, replacing the GPL-treated detector above | will be a downloaded / build-bundled model, SHA-256 pinned, released with `ATTRIBUTION.csv` + `ATTRIBUTION.md` (CC BY attribution) and the YOLO code's MIT notice |
| fast-plate-ocr `cct_xs_v2_global.onnx` | MIT (github.com/ankandrew/fast-plate-ocr) | Android ALPR | downloaded at first use, SHA-256 pinned |
| open-image-models plate detector `yolo-v9-t-416-license-plates-end2end.onnx` (tiles, burst windows, hot full-frame pass) | as the 640 detector above: code MIT, weights treated as a GPL-3.0 model pack | Android ALPR | downloaded at first use, SHA-256 `0469d81f…2173fe4` pinned; not in the APK |
| fast-plate-ocr `cct_s_v2_global.onnx` ("Accurate OCR" setting) | MIT (github.com/ankandrew/fast-plate-ocr) | Android ALPR | optional; downloaded when the setting is first used, SHA-256 `384bbbd2…bc6e47b` pinned |
| ONNX Runtime 1.30 | MIT | Android | dependency |
| Plate tracking / fusion / exposure / burst ideas: ByteTrack two-stage association (MIT, ifzhang/ByteTrack), OC-SORT observation-centric re-update (MIT, noahcao/OC_SORT), C-BIoU buffered IoU (paper, Yang et al. WACV 2023), Frigate LPR group vote (MIT, blakeblackshear/frigate), plates-tracker `DedupEngine` accept rules, `ExposureController` short shutter and `BurstPlanner` (MIT, mssdvd/plates-tracker-public) | ideas only, reimplemented | `android/…/alpr/core/{Tracker,TrackFusion,ShutterPolicy}.kt` | own Apache-2.0 code, credited in comments |
| Candidate-lattice scoring of plate strings (OpenALPR's top-N character permutations) | idea only; OpenALPR is AGPL-3.0 and none of its code is used | `android/…/alpr/core/Lattice.kt`, `HotlistMatcher.kt` | own Apache-2.0 code |
| US plate formats per state | public DMV facts, own compilation | `android/…/alpr/core/PlateFormats.kt` | own code |
| Qt 6, KDE Frameworks | LGPL-3.0 (dynamically linked) | desktop | system libraries |
| libjxl `cjxl`/`djxl` | BSD-3-Clause | desktop image pipeline (docs/SIGHTINGS.md §3) | runtime tool, not bundled |
| FFmpeg (`ffmpeg`) | LGPL-2.1+ / GPL (as the distribution builds it) | desktop: one frame of a public webcam's HLS stream, opt-in (docs/SIGHTINGS.md §2.0) | runtime tool, optional, not bundled |
| KDE kimageformats (`kimg_jxl`) | LGPL-2.1+ | desktop: decoding stored JPEG XL images | system plugin |
| Camera photos (Wikimedia Commons, Panoramax, OSM `image` tags) | each image's own licence (CC0, CC BY, CC BY-SA …) | `plate_event_media` (docs/SIGHTINGS.md §3.2) | fetched per camera; attribution + licence stored and shown with each image; served only to your own devices |
| HaveIBeenFlocked index (released Flock audit logs) | public records, the site's terms | `plate_events` (docs/SIGHTINGS.md §4) | fetched at a low scheduled rate for your own plates (hashed prefixes); each row links its source |
| DeFlock ALPR dataset (`data.dontgetflocked.com/cameras.geojson.gz`), OpenStreetMap-derived | ODbL 1.0 (© OpenStreetMap contributors) | `flock_cameras` (source `deflock`, docs/DATABASE.md) | fetched on a camera sync; attributed on every camera map (desktop, web dashboard, phone) |
| OpenStreetMap camera tags via Overpass | ODbL 1.0 | `flock_cameras` (source `osm`) | fetched; attributed |
| flocklocations.com community reports (rows without an `osm_id`) | CC BY 4.0 | `flock_cameras` (`flock:<id>`) | fetched on a camera sync; attributed with the camera data |
| deflock-data camera pipeline (github.com/FoggedLens/deflock-data, `data/cameras/lib.mjs`) | MIT | its direction grammar re-implemented in `PlateEvents::parseDirections` / `PassDetector.parseDirections` (no code copied) | no (ideas only) |
| Surveillance signatures (OUIs, Bluetooth company IDs / UUIDs, SSIDs) | facts from the IEEE RA registry, Bluetooth SIG assigned numbers, published field captures — not copyrightable; the file itself is Apache-2.0 | `data/signatures/surveillance.json` (docs/DETECTION.md) | yes |
| Eyes on Flock transparency-portal data (`eyesonflock.com/api/v1/data`: the agencies' Flock portals — cameras, retention, searches, sharing) | **CC BY-SA 4.0** (Eyes on Flock) | `eof_portals` (docs/SIGHTINGS.md §4.6): its own table, joined to cameras / plate events only for display | fetched weekly; attributed wherever shown (Sightings table and dialog, `agencyFacts.attribution` in the API); share-alike: redistributed, if at all, under CC BY-SA 4.0 and never merged into BeaconFix's own data |
| OpenStreetMap roads via Overpass (road snapping) | ODbL 1.0 (© OpenStreetMap contributors) | `osm_ways` (docs/SIGHTINGS.md §2.6) | fetched around passed cameras, cached 90 days; attributed with the map data |
| OpenRouteService directions API (HeiGIT) | its terms of service; the user's own free API key and quota; routes are OSM-derived (ODbL) | camera-avoidance routing (docs/SIGHTINGS.md §8) | not bundled: called only with the user's key; the route is attributed ("© openrouteservice.org by HeiGIT · map data © OpenStreetMap contributors") |
| GraphHopper Directions API | its terms of service; the user's own API key and credits; routes are OSM-derived (ODbL) | camera-avoidance routing (docs/SIGHTINGS.md §8) | not bundled: called only with the user's key; attributed ("Powered by GraphHopper · map data © OpenStreetMap contributors") |
| Map matching (road snapping) | own HMM / Viterbi in the spirit of Newson & Krumm 2009 (paper); Valhalla (MIT) and OSRM (BSD-2) named only as a future option | `src/roadsnap.cpp` | own Apache-2.0 code, no third-party code |
| BeaconDB, WiGLE, Esri, OpenTopoMap, Open Topo Data | their terms | positioning, tiles | fetched; attributed |
| EFF short wordlist | CC BY 3.0 US | identity codes | yes |

Add a row for every component integrated from now on (camera/ALPR engine upgrades included), with the module
directory for anything copyleft.
