# Position estimation

How BeaconFix turns signal-strength samples into positions, for access points and for
itself. The code is `src/estimator.{h,cpp}` (pure functions, no I/O) with a Kotlin twin on
Android; the tests are `tests/estimator_test.cpp` and `tests/estimator_golden.cpp`.

## Why not FFTs

An FFT needs a waveform: phase, channel state information, timing. A Wi-Fi scan gives
one number per beacon, the received power in dBm, and nothing else. Power falls off with
distance, so the useful tool is a **range model plus least squares**: every sample says
"the beacon is about *d* metres from where I was standing", and enough of those, taken
from enough different places, pin it down. That is what this does.

## The model

```
RSSI = P0 − 10 · n · log10(d),   d = √(horizontal distance² + h²)
```

* `P0` is the level at 1 m. Radios differ by 20 dB, so it is fitted per beacon, with a
  Gaussian prior per band (−40 / −47 / −48 dBm at 2.4 / 5 / 6 GHz, ± 8 dB).
* `n` is the path-loss exponent: 2 in free space, 2.4 outdoors with clutter, 3–4 indoors.
  Always fitted, with a prior (2.4 or 2.7 ± 0.5; the anchors' environment calibration sets
  the mean when there is one).
* `h` = 3 m, the AP's height above the observer.

Since 3.9 (estimator 2) every estimate is **graded**: docs/GRADING.md defines the numbers,
the formulas and the letters. This page is the overview.

## The fit (estimator 2)

For one beacon with samples `{lat, lon, acc, dBm, time, device}`:

1. **Places.** Samples within `max(15 m, median fix accuracy)` of each other are one place;
   its level is their median, its variance the shadowing (shared within the place) plus the
   observer's own fix error mapped through the model's slope (errors-in-variables).
   Fixes worse than 100 m are used only when nothing better exists.
2. **Grid posterior.** P0 and n are integrated out in closed form at every cell of a 48 × 48
   grid, with a range prior, Wi-Fi RTT ranges when a sample has one, and the cells this host
   scanned from without hearing the beacon (misses). This finds the global optimum, the
   number of modes and the 95 % region, even for one or two samples.
3. **Robust polish.** Levenberg–Marquardt from the grid maxima, the classic centroids and
   the mirror across the places' principal axis, with a Gaussian + uniform outlier mixture
   (EM weights) at a pooled robust scale.
4. **Uncertainty.** Laplace covariance, widened by the design effect of correlated
   shadowing and the correlated fix error, floored by the Cramér–Rao bound, the
   leave-one-place-out jackknife and a cluster bootstrap, scaled by the anchor calibration.
5. **Kind.** A **fix** (grades A–F) needs ≥ 3 places and R95 ≤ 150 m (and a geometry that
   allows it); otherwise a **region** (R): a disc of radius R95 around the posterior mean;
   an AP that travels is **mobile** (M). A moved AP keeps only its recent epoch.
6. **Grade.** Precision, geometry, evidence, fit, stability, freshness and agreement with a
   WiGLE/Apple placement combine into a 0–100 score and a letter, with hysteresis.

Samples: a new one every ~12 m (or half the fixes' error), or ten minutes later at the same
spot; the estimator clusters them itself. Memory keeps at most ~600 per beacon (the
database keeps every row). BSSIDs of one radio are pooled (docs/GRADING.md §3.1), and each
device's level offset is calibrated against this host's (§3.3).

### Between refits

Beacons with new samples are refit in time-boxed batches ten seconds after the scan
(`refitQueued`). Until then the new sample nudges the existing fit with a 2-D Kalman step
along the observer–beacon line, keeping the full covariance, and the grade follows.

`beaconfix --refit` (D-Bus `Refit()`, Settings → Positioning) refits everything;
`beaconfix --estimator` prints grade counts, calibration, device offsets, groups and where
to sample next.

## Precedence on the map

| have | shown as |
|---|---|
| our fix with acc ≤ 25 m, or a fix and no placement | **trilat** (95 % ellipse in the grade colour) |
| a WiGLE / Apple placement (±25 m) | **wigle** |
| a position a synced device worked out (tighter than our region) | **peer** |
| our region only | **region** (disc of radius R95, grade R) |
| it travels | **mobile** (grade M) |
| two vantage points only, not yet refit | **centroid** (wide, honest) |
| one place we heard it, not yet refit | **observed** |
| nothing but the current scan | **ring** (RSSI distance, pseudo-bearing) |

When a placement and our fit disagree by more than 3× their accuracy the card shows both
(`alt` in the JSON).

## Locating ourselves

The internal tier (before BeaconDB) uses the same maths the other way round: each heard
beacon with a known position says "you are `d` metres from me", `d` from *its* fitted
`P0`/`n` when it has a fix (the default model otherwise). The range variance is 6 dB of
shadowing mapped to metres plus the beacon's own covariance along the line of sight, and
each beacon is weighted by its grade. Weighted least squares on those ranges, Huber
weights, then an integrity check: a χ² test on the normalised range residuals excludes the
worst beacon and solves again (up to twice); the result says `ok`, `repaired`, `failed` or
`unverified` (fewer than four beacons). Regions and mobile APs are never used.

## Sync

Every stored AP position, observation and fix carries a change sequence number
(`seq`, from the `kv` counter). `GET /api/v1/db/changes?since=<cursor>` returns what
changed, oldest first; `POST /api/v1/db/sync` accepts a peer's observations (deduplicated
on `(bssid, time, device)`), positions and fixes, queues refits for the touched beacons and
returns the new cursor. Observations remember which device heard them, so a phone's, a
laptop's and the desktop's samples all count towards the same fit, and nothing is echoed
back to the device it came from.

## Tests

```
cmake -S . -B build -DBEACONFIX_TESTS=ON && cmake --build build -j3 && (cd build && ctest)
```

`tests/estimator_test.cpp`: synthetic geometry (a ring fits within 15 m and grades A/B; one
spot or one sample gives a region that covers the truth; loud outliers are rejected; a
straight road is capped for extrapolation/ambiguity; misses shrink a region; a moved AP
keeps its recent epoch; a device offset, RTT ranges, hysteresis, external agreement; R95
covers the truth ≥ 85 % of the time on random geometry). `tests/estimator_golden.cpp`
checks the golden vectors shared with the Android twin (docs/GRADING.md §7).
