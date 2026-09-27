# Position estimation

How BeaconFix turns signal-strength samples into positions, for access points and for
itself. The code is `src/estimator.h` (pure functions, no I/O) and the tests are
`tests/estimator_test.cpp`.

## Why not FFTs

An FFT needs a waveform: phase, channel state information, timing. A Wi-Fi scan gives
one number per beacon, the received power in dBm, and nothing else. Power falls off with
distance, so the useful tool is a **range model plus least squares**: every sample says
"the beacon is about *d* metres from where I was standing", and enough of those, taken
from enough different places, pin it down. That is what this does.

## The model

```
RSSI = P0 − 10 · n · log10(d)
```

* `P0` is the level at 1 m. Radios differ by 20 dB, so it is fitted per beacon.
* `n` is the path-loss exponent: 2 in free space, 2.4 outdoors with clutter, 3–4 indoors.
  It is fixed at 2.4 until a beacon has 8 samples, then fitted within [1.8, 4].
* `d` is the distance between the observer's fix and the beacon.

## The fit

For one beacon with samples `{lat, lon, acc, dBm, time}`:

1. **Weights.** A sample counts less when the observer's own fix was loose
   (`1 / (1 + (acc / 25 m)²)`), when it is older than 30 days, and when the beacon's status
   says it may move ("travels with you" gets 0.3; home networks are never fitted).
2. **Seeds.** Three starting points: the signal-weighted centroid, the plain centroid, and
   the centroid of the loudest quarter. A single loud outlier captures one seed; the others
   escape it.
3. **IRLS.** From each seed, Gauss–Newton with Levenberg damping on `(x, y, P0[, n])` in a
   local metric frame. The first half of the iterations use Huber weights (6 dB), the second
   half the redescending Tukey biweight (12 dB) so gross outliers end with zero weight. The
   objective is the corresponding ρ function, which is *bounded* for Tukey: a "solution" that
   rejects every sample has the worst cost, not the best. Steps are capped at 300 m.
4. **Pick.** The seed whose solution explains the most samples wins; ties go to the lower cost.
5. **Uncertainty.** `(JᵀWJ)⁻¹ · σ²` at the solution gives the covariance of `(x, y)`; its
   eigen-decomposition is the 1-σ error ellipse. The observers' median fix accuracy is added
   in quadrature. The circular accuracy reported is the semi-major axis (conservative).
6. **Quality.** `good` (≤ 40 m, ≥ 5 vantage points, RMS ≤ 6 dB), `fair` (≤ 120 m, ≥ 3
   vantage points), else `poor`.

### The geometry guard

Standing in one spot and hearing a beacon fifty times tells you nothing about where it is.
Samples closer together than 25 m, or than 1.5× the larger of their fix errors, count as
one **vantage point**. Fewer than three vantage points, or a spread smaller than 1.5× the
worst fix accuracy, and no position is claimed; the map keeps the RSSI ring.

### Between refits

Beacons with new samples are refit in a batch ten seconds after the scan (`refitQueued`).
Until then the new sample nudges the existing fit with a one-dimensional Kalman step along
the observer–beacon line: the sample's range from the beacon's own `P0`/`n` is the
measurement, its variance the dB noise mapped to metres plus the fix accuracy.

`beaconfix --refit` (D-Bus `Refit()`, Settings → Positioning) refits everything.

## Precedence on the map

| have | shown as |
|---|---|
| our fit with acc ≤ 25 m, or a fit and no placement | **trilat** (our fit) |
| a WiGLE / Apple placement (±25 m) | **wigle** |
| a position a synced device worked out | **peer** |
| two vantage points only | **centroid** (wide, honest) |
| one place we heard it | **observed** |
| nothing but the current scan | **ring** (RSSI distance, pseudo-bearing) |

When a placement and our fit disagree by more than 3× their accuracy the card shows both
(`alt` in the JSON).

## Locating ourselves

The internal tier (before BeaconDB) uses the same maths the other way round: each heard
beacon with a known position says "you are `d` metres from me", `d` from *its* fitted
`P0`/`n` when it has a fit (the default model otherwise), with variance from 6 dB of
shadowing plus the beacon's own accuracy. Weighted least squares on those ranges, Huber
weights, accuracy from the covariance and the range residuals. Two beacons give a
weighted midpoint; three or more a real solution.

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
g++ -std=c++17 -O2 -Wall -Wextra -fPIC $(pkg-config --cflags Qt6Core) tests/estimator_test.cpp -o build/estimator_test $(pkg-config --libs Qt6Core)
./build/estimator_test
```

Synthetic geometry: 40 noisy samples around a beacon fit within 15 m (typically 5–12);
twelve samples from one spot claim nothing; three loud samples from 300 m away are rejected
without moving the fit; five known beacons locate the observer within 30 m; incremental
updates converge and never diverge.
