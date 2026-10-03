# Surveillance hardware detection

BeaconFix notices Flock Safety hardware, and other police / surveillance gear, in what its radios already
hear: the desktop in its Wi-Fi scans and in the Bluetooth adverts its ranging link already listens to, the
phone in its Wi-Fi scans. It adds no scanning of its own (no extra BLE scanner, no external sensors).

## 1. One signature file

The rules live in **`data/signatures/surveillance.json`** (versioned: `format` 1, `version` N). Both platforms
read that one file:

| | how it gets the file | code |
|---|---|---|
| desktop | compiled in (Qt resource `:/signatures/surveillance.json`); **overridden** by `~/.local/share/sworrl/beaconfix/signatures/surveillance.json` when that parses and its `version` is at least the built-in one (a broken or older override is ignored with a warning) | `src/flockdetector.{h,cpp}` |
| Android | the asset `signatures/surveillance.json`, copied from the same file at build time (`copySurveillanceSignatures` in `app/build.gradle.kts`) — never duplicated by hand | `collector/SurveillanceSignatures.kt`, `collector/FlockDetectorKotlin.kt` |

The unit tests of both run the same cases, `tests/fixtures/surveillance_cases.json`
(`tests/flockdetector_test.cpp`, `SurveillanceSignaturesTest.kt`), so the two can't drift apart. Facts such as
OUIs, UUIDs and SSIDs are not copyrightable; every entry names its source (IEEE registry, Bluetooth SIG
assigned numbers, published field captures, CVE-2025-59409).

## 2. Tiers

Every rule has a tier, **0 weak … 4 conclusive**. A detection is the strongest tier any rule reached (on a tie,
a Flock class wins). Its confidence is `confidence[tier]` = 15 / 40 / 65 / 85 / 98.

- **Flock** (classes `flock`, `raven`) at tier ≥ `detectTier` (2): a *sighting*. The event log says
  "Flock hardware detected", and the camera map gains or vets a `det:<mac>` row (`camera_type` `alpr`,
  or `not_camera` for a Raven gunshot detector). The phone alerts and reports it to its desktops.
- **Other surveillance / police gear** (Axon, ShotSpotter, WatchGuard, Ubicquia, Verkada, Avigilon Alta,
  Genetec, Neology, Elsag, Perceptics, Fusus) at tier ≥ 2: *informational* — logged as
  `surveillance_gear`, never stored as a Flock camera.
- Tier 0 and 1 alone are never reported. They exist to be combined.

### Wi-Fi
| evidence | tier |
|---|---|
| a frame from the Flock MA-L `B4:1E:52` | 2 |
| the exact factory MACs `00:03:7F:50:00:01`, `00:03:7F:4F:00:16` | 4 |
| a corroborate-only OUI (the Liteon family, UGSI `08:3A:88` `E0:4F:43`, `82:6B:F2`, `B8:35:32`) alone | 0 |
| SSID exactly `Flock`, `Flock Camera net.` or `Flock` + 3 digits | 1 |
| SSID `Flock-` + 6 hex digits | 1; **2** when they equal the last 3 bytes of the BSSID (~97 % of real units) |
| a Flock SSID on a Flock-family MAC (any of the above OUIs) | 3 |
| a `test_flck` probe request (CVE-2025-59409) — only where probe data exists (neither platform has it today) | 3 |
| SSID `AXON-X…` | 2 (axon, informational) |

### Bluetooth LE (desktop: adverts the ranging link already hears)
| evidence | tier |
|---|---|
| company `0x09C8` (XUNTONG) alone | 0 |
| XUNTONG with a Flock name, or a `TN` + 16-character serial in the advert | 3 |
| services `e8ccbb38-9532-46a8-9fe5-1814df172e6f`, `20c944c1-add2-42d7-a638-967ee9a26ff6` | 3 |
| names `FS-` + 6 hex digits, `Penguin-` + 10 digits, `FS Ext Battery` | 3 |
| Raven services `0x3100` `0x3200` `0x3300` `0x3400` `0x3500` | 3 (raven) |
| any other 16-bit service in `0x3101`–`0x35FF` | 1 (raven) |
| Axon company `0x034D`, services `0xFC81` `0xFE6B` `0xFE6C`; Verkada `0xFD3A` `0xFD3B` `0xFC2C` | 2 (informational) |

### MAC prefixes
Prefixes are 24 (MA-L), 28 (MA-M), 36 (MA-S) or 48 bits (one exact address); the longest match wins. A bare
`70:B3:D5` is the IEEE MA-S *block*, not a vendor: the loader refuses it, and only its 36-bit assignments
(Elsag `70:B3:D5:1C:5`, Perceptics `70:B3:D5:88:A`) match.

### Removed (2026-10)
Generic or wrong identifiers that made ordinary devices look like cameras: Android's 53-OUI list (≈28
Espressif OUIs, consumer vendors, `CC:CC:81`, `90:35:6E`, the "battery OUIs"); the desktop's consumer OUIs
(Apple, Samsung, ASUS, Epson, Amcrest, LG, Frontier Silicon, Hon Hai, Espressif, Redpine, 8Devices, Trolink,
SiLabs); the "BLE OUIs" (`CC:09:24` … `EA:5A:98`, random-static device addresses, not OUIs); the retracted
`CC:CC:CC`, `F8:A2:D6`, `3C:71:BF`, `A4:CF:12`, `48:27:EA`; and the SSIDs `pigvision`, `Penguin-…`,
`Falcon-…` and `FS Ext Battery` as Wi-Fi names. Detections made with them are re-checked once by the camera
cleanup ([DATABASE.md](DATABASE.md)).

## 3. Updating the rules
Edit `data/signatures/surveillance.json`, bump `version`, add or adjust cases in
`tests/fixtures/surveillance_cases.json` (false positives too), and run both test suites. A user can try a
newer file without a rebuild by placing it at the desktop override path.
