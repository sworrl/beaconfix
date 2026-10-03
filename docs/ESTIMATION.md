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

1. **Places.** A sample joins a place whose seed is within `max(5 m, 1.5 × the better fix
   accuracy of the two)`, so a smoothed walk keeps its shape and poor indoor fixes still merge;
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
4. **Uncertainty.** The noise scale is estimated, not assumed: a scaled-inverse-χ² posterior
   (prior σ0 = 6 dB) updated by the residuals of the *effectively independent places*, so many
   well-fitting places shrink the error bars and a handful do not. Then a sandwich Laplace
   covariance with the places' correlated shadowing (8 m), widened to the R95 the local
   posterior shows (ranging is log-normal: the far side is flat), plus the correlated fix error,
   floored by the leave-one-place-out jackknife and a cluster bootstrap, scaled by the anchor
   calibration. Estimator 3; docs/GRADING.md §1.5 has the numbers behind each constant.
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

### The order a scan is resolved in (4.0)

Measured against a surveyed point in rural West Virginia (2026-10-01): the phone's fused GPS
5-15 m, the internal beacon map 85 m median, Apple and BeaconDB 160-525 m while claiming
±50-100 m. So the chain is, first answer wins:

1. **Site lock** — the newest `this-computer` anchor *is* the fix while this host's scan matches
   the neighbourhood learnt there: other people's APs (ours travel with the RV) heard in at least
   half the on-site scans, ≥ 3 of them heard now, ≥ half of them, level RMS ≤ 12 dB; hysteresis
   (two scans under 30 % to leave). A young fingerprint (< 5 scans) trusts the anchor if it was
   placed in the last 2 h or a fresh precise fix of a linked device is within 30 m. On site no
   geolocation service is asked, and RV anchors are never re-projected from a Wi-Fi fix.
2. RV GNSS (the Pi agent).
3. **Fingerprint** (`src/fingerprint.h`) — nearest past scans in signal space: every
   (device, time) scan with a fix ≤ 25 m is an epoch; a scan is scored against epochs sharing
   ≥ 3 APs by the offset-free level mismatch (σ 8 dB), strong one-sided APs and the number shared,
   and the 7 best are softmax-averaged. It needs no AP positions, so it is as good as the GPS the
   epochs were tagged with: 11.8 m median, 16.5 m p90 on the test data.
4. Internal beacon map, 5. BeaconDB, 6. Apple, 7. IP.

**Provider calibration.** Each Wi-Fi provider's claimed accuracy is multiplied by the 68th
percentile of |error| / claimed, measured against the site anchor or a linked device's GPS fix
≤ 15 m and ≤ 2 min old (kv `provider_cal`, last 60 per provider, seeded from history on first run).

### Vantage points: the track smoother (4.0)

Why not FFTs, measured: a parked Pixel's Allan deviation is ~3.5 m at 1 min and flat at ~2 m
from 2 min to an hour — the error wanders slowly, so no filter (frequency or otherwise) averages
it away. Each device's fixes go through `src/tracksmoother.h` (position, velocity and a
Gauss–Markov bias per axis; mode-dependent motion; χ² gating; Rauch–Tung–Striebel smoothing; see
its header) and every observation uses the smoothed position and its honest 68 % radius at its
instant. The site anchor is the phone's base station: while the phone hears our own AP at
≥ −45 dBm (inside the RV) after the anchor was placed, it was at the anchor (±4 m), which makes
the bias observable and carries it into the walk that follows. On site, a mobile AP this host
hears at ≥ −60 dBm is placed on the anchor, the radius its level implies.

### Ranges (4.0)

Phones range any AP answering Wi-Fi RTT (802.11mc; 802.11az from Android 15) during collection;
an observation may carry `rangeM` / `rangeSd` (metres, 1-σ), stored in `observations.range_m /
range_sd` and fitted as a range term beside the level. Few consumer APs answer.

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
covers the truth ≥ 85 % of the time on random geometry; many well-fitting places shrink the
R95, four do not; a walk around a building with correlated shadowing, a lossy wall and a GPS
bias keeps its places, finds the AP within 12 m (median) and R95 covers ≥ 85 %). `tests/estimator_golden.cpp`
checks the golden vectors shared with the Android twin (docs/GRADING.md §7).
