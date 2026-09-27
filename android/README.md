# BeaconFix for Android

The phone-side BeaconFix: it records Wi-Fi beacons at your GPS position while you move, fits
each beacon's position on the device, mirrors everything the desktop knows (its fix, trip,
places, events, beacon audit), measures the distance to your desktop in metres, and syncs both
ways with every BeaconFix desktop or laptop that shares your identity.

- **Works offline.** Everything lands in a local Room database; the estimator runs on the phone.
  Every answer a desktop gives (its fix, trip, track, places, nearest help, devices) is mirrored
  there too, so Home, Places, the map, the trip journal and Help show the last known picture —
  with its age — when the RV's desktop is out of reach.
- **Nearest help.** One screen for an emergency on the road: the local emergency number first
  (always the dialer, never an automatic call), your position and address as a dispatcher asks
  for them, the nearest **children's ER** with its confidence in words (dedicated pediatric ER;
  children's hospital with an ER on campus, or not confirmed — call ahead; ER with a pediatrics
  department), a closer but less certain one, the nearest general ER (never hidden), urgent care
  marked "Not an ER", police, fire and Poison Control (US), each with distance, an estimated drive
  time, Call, Directions and Share. Measured from you when your fix is fresh, else from the RV.
  With no desktop in reach the phone searches OpenStreetMap itself (help categories only, only
  when you open Help or Places, never in the background; it can be switched off).
- **Quick access.** A Help widget, Quick Settings tiles (nearest help, collector), launcher
  shortcuts (Help now, Kids ER, Share my location, Find the RV) and an optional heads-up with the
  nearest help after a move of more than 25 km.
- **One identity, every device.** An Ed25519 key pair moved between apps with an encrypted
  bundle (`BFID1:` QR / text / file / a 6-digit LAN code), or two identities *linked* by scanning
  a `beaconfix://link/…` QR. Same identity ⇒ no pairing code, ever (challenge/response auth).
- **Adjacency first.** Onboarding, linking and pairing start from the BeaconFix instances found
  on the LAN (mDNS `_beaconfix._tcp`, `/api/v1/peers`, a subnet probe): "That's me — import" and
  "Link my identity with it" are one tap each.
- **Pairing v2.** X25519 ephemeral keys → three pictures that must match on both screens, a
  proximity verdict (shared beacons, fixes, and the measured range), cancel from either side;
  older desktops fall back to the 4-digit code automatically.
- **Device ranging.** Wi-Fi RTT (802.11mc) to the desktop's responder, BLE advertisements in both
  directions, shared-AP fingerprints, barometer and motion — fused by the desktop and on the
  phone (`docs/RANGING.md`) into "Desktop · 0.6 m (Wi-Fi RTT ±0.3 m)" on the Home card, the
  map (ring / point around the desktop's antenna, a to-scale inset below map resolution), the
  status notification, and the pairing verdict.
- **Anchors.** Long-press the map (or stand next to it and average GNSS for a minute) to place an
  antenna: the desktop's Wi-Fi card, a router's radios (grouped per box), a BLE beacon. Anchors
  are ground truth: fixed transmitters in self-location, the centre of ranging rings, the RV's
  own frame.
  Stored locally, synced with desktops that have `/api/v1/anchors`.
- **Import your history.** Google Timeline / Takeout, WiGLE CSV, GPX/KML, BeaconFix exports —
  via the file picker or *Share to BeaconFix*; nothing is fetched from Google.
- **Where's the RV, share my location.** A Home card with the RV's distance, direction and age
  (Navigate back, Share RV spot), one-time location sharing as plain text with map links, and
  locations shared *to* BeaconFix (Google / Apple / OpenStreetMap links, `geo:`, "lat, lon")
  open on the map.
- **Trip journal** of stops by day (from the desktop's track, or the phone's own fixes), with
  GPX export through the system file picker.
- **Backup** of the phone's own beacons, observations, fixes and anchors to a file or straight
  to the RV's desktop (never the identity, tokens or settings).
- **Connected Wi-Fi safety**: the network you are on, graded like the beacon audit, with an
  optional alert for open networks.
- **Home-screen widgets** (Help, Location, Beacons, Sync, Map) and a permanent, silent **status
  notification** that shows what BeaconFix knows and is doing, with Help / Scan now / Pause
  buttons (the lock screen shows only "BeaconFix · running") — it cannot be swiped away for good
  (Android 14+ lets you try; it comes straight back).
- **Units**: metric, imperial, or automatic (the desktop trip's country, else the phone's region).
- **Honest positions.** A beacon gets a position only after samples from at least two places
  (robust weighted least squares on a log-distance path-loss model, Huber weights); the phone
  locates itself from known beacons when GPS is silent, and says so. Refits are animated on the map.
- No analytics, no crash reporting, no servers other than your desktops, OpenStreetMap tiles and,
  for Help with no desktop in reach, OpenStreetMap's Overpass (places); the address for the
  dispatcher comes from the system geocoder, else one Nominatim lookup.

## Screens

| | |
|---|---|
| **Home** — permission problems (hidden when all is well), the nearest help, where the RV is, this phone's fix (place, accuracy, sun, time zone, emergency number, home distance), each desktop's card with elevation / trip / rank and the **measured range**, the collector switch, the connected Wi-Fi | **Map** — beacons with security colours and labels, both tracks, places from the offline cache (pediatric ERs on top) with a place sheet, a filter row, linked devices and the RV, anchors (⌖, draggable), ranging rings, refit animations; streets / dark / topo / satellite; long-press places an antenna |
| **Beacons** — the security audit (same wording as the desktop's `security.js`), sparklines from the survey | **Places** — every desktop's and this phone's places, offline, with Help & medical / Kids ER / Civic / Kids & fun / Services / Open now chips, badges (ER, no ER, kids ER tier, open / closed), and Call / Directions / Share / Website / Map |
| **Help** — the emergency number, position for the dispatcher, the children's ER, ER, urgent care, police, fire, Poison Control, where the answer came from and how old it is | **Trip / Events** — the trip live or as saved, the journal of stops with GPX export; the live event stream (SSE) |
| **Survey** — continuous scanning with live signal strengths | |
| **Sync** — every desktop, push/pull counts, errors, identity sign-in | **Identity** — id, name, devices, links; export (QR / text / file / LAN code); link (QR both ways); forget |
| **Anchors** — the list; edit / delete; "at my position" | **Import** — file → fixes, beacons and stops on this phone |
| **Widgets** — gallery with live previews, pin to the home screen (and the Kids ER shortcut) | **Settings** — units and places, system health, backup, collector cadence, home networks, desktops, status notification, import, developer automation |

## Identity, linking and the QR codes

The desktop prints `beaconfix --identity-link-qr` (a `beaconfix://link/<payload>` QR); the
phone registers `beaconfix://link|statement|identity|pair` and the bare `BFLNK1:` / `BFLINK1:` /
`BFID1:` schemes, so scanning with the **system camera** opens BeaconFix on the identity screen.
One scan does the whole job: the phone co-signs the offer, finds the desktop that owns it on the
LAN, posts the half-signed statement to its `/api/v1/identity/link`, then signs in with the
challenge/response and stores the control token as a paired desktop. Test vectors (seed 32×0x01)
equal the desktop's `--identity-selftest` numbers (`IdentityTest`).

## Status notification (the "tray")

`CollectorService` is a foreground service (`location|connectedDevice`) that runs whenever the
status notification is enabled — collecting or not. Its notification is `ongoing` + `NO_CLEAR`,
posted with `FOREGROUND_SERVICE_IMMEDIATE`, re-posted by its delete intent the moment it is
swiped, restarted by `START_STICKY` and a `BOOT_COMPLETED` / `MY_PACKAGE_REPLACED` receiver. The
only way to hide it is *Settings → Hide the status notification* (off by default, with the warning
that Android may then stop background collection and ranging: without a foreground service the
collector only gets the 15-minute WorkManager slot).

## Build

Requirements: JDK 17, Android SDK (platform 35, build-tools 35), the Gradle wrapper (included).

```sh
cd android
export JAVA_HOME=/path/to/jdk17
./gradlew assembleDebug                      # unsigned debug build
./gradlew testReleaseUnitTest lintRelease    # identity vectors, SAS/RFC 7748, importers, estimator, sync, anchors, ranging
```

### Signed release

Secrets are never in the repo. Provide the four variables either in the environment or in a
`KEY=VALUE` file referenced by a git-ignored `keystore.properties`
(see `keystore.properties.example`):

```
BEACONFIX_KEYSTORE=/absolute/path/release.jks
BEACONFIX_KEYSTORE_PASSWORD=…
BEACONFIX_KEY_ALIAS=…
BEACONFIX_KEY_PASSWORD=…
```

```sh
./gradlew assembleRelease bundleRelease
apksigner verify --print-certs app/build/outputs/apk/release/app-release.apk
```

Create a keystore once with `keytool -genkeypair -v -keystore release.jks -alias beaconfix -keyalg RSA -keysize 4096 -validity 10000`.

### Automation hooks (adb)

`MainActivity` reads extras so tests can drive the app without touching the screen:

```
am start -n org.sworrl.beaconfix/.MainActivity --es action sync|scan|collector_on|collector_off|map|identity|anchors|…
am start … --es pair_host <desktop-ip> [--ei pair_port 47822]          # identity sign-in / pairing
am start … --es action help|help_peds|share_location|find_rv|nearby|trip|…
am start … --es import_host <host> --es import_code 123456 --es import_pass '<six words>'
am start … --ez sim_offline true|false --ez sim_no_desktop true|false   # offline simulation
am start -a android.intent.action.VIEW -d 'beaconfix://link/<payload>'   # what the camera app does
am start -a android.intent.action.VIEW -d 'beaconfix://map?lat=40.0&lon=-75.0&label=Test'   # or beaconfix://help
```

`--ez show_when_locked true` lets a launch draw over the lock screen for screenshots. The activity
is exported, so on a release build `import_*`, `action forget_identity` and the `sim_*` switches
work only with *Settings → Developer automation* on.

## Permissions

Fine location (Wi-Fi scan results are location on Android), background location (for the
collector while the screen is off; asked for only from the System health card), nearby Wi-Fi
devices (Android 13+, scans and Wi-Fi RTT), Nearby devices / Bluetooth scan+advertise+connect
(Android 12+, BLE ranging), notifications, camera (QR scanning only). 1.4 adds none. Android 10+ throttles foreground scans to 4 per 2 minutes; for a survey turn
off *Developer options → Networking → Wi-Fi scan throttling*.

## Screenshots

_(to be added: Home, Map, Beacons, Places, Trip, Events, Identity, Anchors, Settings, Widgets, status notification)_

## License

GPL-2.0-or-later, like the desktop app.
