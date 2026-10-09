# BeaconFix Lite

BeaconFix's Wi-Fi positioning as a small library, for devices that need to know where they are without GPS, without
Google Play services, and without spending battery, network or flash on it: a music player (the MikuOS M500), a
picture frame (Frameo), a sensor box. Plain Kotlin on the JVM, plus one Android class. Android 5.0 (API 21) and up,
Kotlin 1.9 and up, no dependencies.

## What it does on each look

1. **Same place as last time?** The APs heard now are compared with the last fix's (a weighted overlap of the strong
   ones). Same place and the fix is under 6 hours old: that fix again. No solve, no network, no disk.
2. **Know enough of them already?** APs placed earlier live in a small cache on the device. With three or more
   placed, including half of the six strongest, the position is solved offline.
3. **Otherwise one lookup.** Apple's Wi-Fi positioning is asked about the strongest unplaced APs (ten per request,
   at most two requests). Apple answers with ~100 neighbours of each, and every one goes in the cache, so later
   fixes around town are offline. BeaconDB is the fallback when Apple can place fewer than two.
4. **Learn.** APs that disagree with the rest get a strike; two strikes and the AP is treated as moved (ignored for
   30 days, then asked about again). Unplaced APs near a good fix are learned roughly, at low weight, so a place
   visited before is solved offline even when no service knows its street.

## Accuracy

The position is a robust trilateration (BeaconFix's `selfLocate`): each AP's level gives a range through a
path-loss model, and the fit down-weights ranges that disagree. With four or more APs an integrity check drops the
AP that disagrees most and solves again (`integrity`: `ok`, `repaired`, `unverified`, `failed`). An AP mapped far
from the rest (your own router, heard loud after you've moved house) is screened out before the solve. Every fix
carries a 1-sigma radius (`accM`) and a 95 % radius (`r95M`), widened when the APs disagree, so a weak fix says so.

What it reaches depends on the APs around and how well they're mapped. Measured so far:

| Where | Result |
|---|---|
| Simulated: 10 APs 25–85 m away, 4 dB of noise, 20 random layouts (the unit tests) | worst miss 24 m |
| The same with one loud AP mapped 2 km away | AP dropped, 26 m off |
| A rural campsite, the M500's real scan (33 heard, most of them the RV's own gear and excluded), Apple only | 91 m off, claimed ±93 m |
| The same plus 8 APs a BeaconFix desktop had placed | 49 m off, claimed ±76 m |

Towns, where Apple knows many nearby APs, haven't been measured yet. For metres rather than tens of metres, give it
better AP positions: `import()` a BeaconFix desktop's placements, or `teach()` it a trusted fix (GPS outdoors, a
BeaconFix desktop on the same network) and it learns the APs around it.

## Battery, network, flash

- No scan is requested while the system's last one is under 5 minutes old (Android's background scans, other apps').
- A look that reuses the last fix costs ~1 ms of CPU. A full offline solve: ~18 ms on the M500 (a cold process,
  cache load included). A lookup: one HTTPS request, ~0.7 s, almost all of it network.
- The cache is 20 bytes an AP (a town of 4,000 APs: 80 KB). Changes are appended to a journal at the end of a look,
  only when something changed, and folded into the main file when the journal reaches a quarter of it. A power cut
  mid-write loses at most that last change.
- `Pacer` suggests the next look: doubling up to 2 hours while nothing changes, 15 minutes after a change, 2 minutes
  when the device moved far between two looks.

## Privacy

Never sent anywhere: APs whose SSID ends in `_nomap` or contains `_optout`, anything named like it travels (phone
hotspots, car and train Wi-Fi, Starlink, dashcams), and the SSIDs you say travel with the device. Apple and BeaconDB
see the BSSIDs of the strongest other APs on a lookup, nothing else; learned positions never leave the device.

## Use it

Android:

```kotlin
val lite = BeaconFixLite(context, travelling = listOf("MyHomeNet"))   // joinedTravels = false for a device that stays put
thread {
    val fix = lite.locate()          // null: nothing here could place us (a real answer: don't fake one)
    fix?.let { show(it.lat, it.lon, it.accM) }
    handler.postDelayed(next, lite.nextCheckMs)
}
```

Permissions: `ACCESS_FINE_LOCATION` (Android's rule for reading scan results), `ACCESS_WIFI_STATE`,
`CHANGE_WIFI_STATE`, `INTERNET`.

Anything else (the core has no Android imports):

```kotlin
val loc = LiteLocator(File(dir))
val fix = loc.locate(listOf(Heard("02:3a:c4:09:42:ec", -61, 2437, "Corner Cafe"), /* … */))
```

Try it from a shell, on a desktop or straight on a device:

```sh
adb shell cmd wifi list-scan-results | java -cp beaconfix-lite-core-1.0.0.jar:kotlin-stdlib.jar \
    org.sworrl.beaconfix.lite.Cli /tmp/bflite --repeat 3

# on the device itself: dex the core jar and the Kotlin stdlib (d8), push, then
adb shell 'cmd wifi list-scan-results | app_process -cp /data/local/tmp/bflite.dex /system/bin \
    org.sworrl.beaconfix.lite.Cli /data/local/tmp/bflite'
```

## Getting it

Releases tagged `lite-vX.Y.Z` on this repository carry:

| File | For |
|---|---|
| `beaconfix-lite-X.Y.Z.aar` | Android projects (`implementation(files("libs/beaconfix-lite-X.Y.Z.aar"))`) |
| `beaconfix-lite-core-X.Y.Z.jar` | the JVM core alone (desktop, server, the CLI); add `kotlin-stdlib` |
| `beaconfix-lite-src-X.Y.Z.zip` | the sources, for projects that vendor them (package `org.sworrl.beaconfix.lite`) |

Build from this repository: `cd android && ./gradlew :lite:testDebugUnitTest :lite:assembleRelease :lite:coreJar
:lite:sourcesZip` (artifacts in `lite/build/outputs/aar` and `lite/build/dist`), or `tools/release_lite.sh`.

## Where it came from

The MikuOS M500 launcher and the Frameo weather app each carried their own port of BeaconFix's locator: Apple and
BeaconDB, a signal-weighted centroid, a list of travelling SSIDs, and on the Frameo a beacon log rewritten as JSON
on every fix. Lite replaces both: the desktop's estimator and integrity check instead of a centroid, a binary
journal instead of the JSON rewrite, reuse and offline solves instead of a lookup per look, and the strike system
for APs that moved.
