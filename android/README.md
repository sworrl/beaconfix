# BeaconFix for Android

The phone-side companion to the BeaconFix desktop app: it records Wi-Fi beacons at your GPS
position while you move, fits each beacon's position from those samples on the device, and
syncs both ways with any BeaconFix desktop you have paired with over the LAN API.

- **Works offline.** Everything lands in a local Room database; the estimator runs on the phone.
- **Syncs when it can.** Every 15 minutes (WorkManager) or on demand: pushes your observations,
  pulls the desktop's beacons, home networks, fix, track and places; incremental sync is
  feature-detected (`hello.features` containing `sync`), with the full export as the fallback.
- **Pairs with the desktop's own flow.** `POST /api/v1/pair` → 4-digit code on both screens →
  bearer token stored in Android's Keystore-backed encrypted preferences. 401 forgets the token.
- **Honest positions.** A beacon gets a position only after samples from at least two places
  (robust weighted least squares on a log-distance path-loss model, Huber weights); the phone
  can locate itself from known beacons when GPS is silent, and says so.
- No analytics, no crash reporting, no servers other than your desktops and OpenStreetMap tiles.

## Build

Requirements: JDK 17, Android SDK (platform 35, build-tools 35), the Gradle wrapper (included).

```sh
cd android
export JAVA_HOME=/path/to/jdk17
./gradlew assembleDebug            # unsigned debug build
./gradlew testDebugUnitTest lint   # estimator + sync tests, lint
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

## Permissions

Fine location (Wi-Fi scan results are location on Android), background location (for the
collector while the screen is off), nearby Wi-Fi devices (Android 13+), notifications (the
collector's persistent notification). Android 10+ throttles foreground scans to 4 per 2 minutes;
for a survey turn off *Developer options → Networking → Wi-Fi scan throttling*.

## Screenshots

_(to be added: Home, Map, Beacons, Survey, Sync, Pairing)_

## License

GPL-2.0-or-later, like the desktop app.
