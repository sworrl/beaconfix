# Device ranging and anchors

How BeaconFix measures the distance between *your own* devices (the desktop, a phone, a
laptop), how it turns raw radio numbers into metres with an honest error bar, and how
surveyed **anchors** (an antenna you place on a map) turn those metres into absolute positions.

Everything here is implemented twice, in C++ (`src/ranging/rangemath.*`, `src/ranging/anchors.*`)
and in Kotlin (`android/…/ranging/RangeMath.kt`), and both must reproduce the test vectors in §11.

Status: spec v1 (2026-09-27). Protocol names in §4 (anchors) and §7 (endpoints) are frozen.

---

## 1. Why the old number was wrong

Before this, the distance between two devices was `|fix_A − fix_B|`: two independent position
fixes, each good to about ±40 m, subtracted. A phone lying 0.6 m from the desktop came out as
39.7 m. The pairing check that compared "shared beacons" also threw away the RV's own access
points, which are the best evidence two devices sit in the same place, and reported 0 shared.

Two devices that can talk to each other can do far better: they can **measure each other**.

## 2. What each side can measure

| Signal | Who measures | What it tells | Typical quality |
|---|---|---|---|
| Wi-Fi RTT (IEEE 802.11mc FTM) to the desktop's responder | phone (initiator) | time of flight → range | ±0.5–1 m at 80 MHz, ±1.5–3 m at 20 MHz per burst; NLOS adds a **positive** bias |
| BLE advertisement RSSI, desktop → phone | phone | received level of the desktop's beacon | ±6 dB raw; ±2 dB after fading averaging + calibration |
| BLE advertisement RSSI, phone → desktop | desktop (BlueZ) | the reverse link | same, independent antennas and TX power |
| Wi-Fi scan RSSI of every AP both hear (home APs **included**) | both | a signature of *where* each device is | 1 dB steps, ±5 dB shadowing, fading |
| GNSS / fused fix + accuracy | phone (desktop: Wi-Fi fix or an anchor) | absolute positions | phone ±3–20 m, desktop ±40 m, anchor ±`accM` |
| Barometer | phone (and any other device that has one) | relative height, ~8.4 m per hPa | ±0.5 m relative between two barometers |
| IMU (accelerometer) | phone | moving / still | binary state for the process noise |
| Channel / bandwidth / time | both | fading independence, coherence | — |

The Pixel 10 Pro XL also has UWB and BLE Channel Sounding. They need a capable peer; the
desktop's AX210 (Bluetooth 5.3) has neither, so they are reserved for phone ↔ phone (§9).

## 3. Radio models

### 3.1 Path loss

Received level in dBm for a transmitter at distance `d` (m) on frequency `f` (MHz):

```
r = P0(f) − 10·n·log10(d / 1 m) + b + ε_fast
P0(f) = EIRP + G_rx(device, band) − FSPL(1 m, f) − L
FSPL(d, f) = 20·log10(d) + 20·log10(f) − 27.55            (Friis, d in m, f in MHz)
FSPL(1 m, 2426 MHz) = 40.15 dB    FSPL(1 m, 2437) = 40.19 dB    FSPL(1 m, 5180) = 46.74 dB
```

* `n` path-loss exponent: 2.0 in free space, 1.6–1.8 in corridors, 2.5–4 through walls/bodies.
  Priors: BLE link `n0 = 2.0 (σ 0.4)`, Wi-Fi AP `n0 = 2.4 (σ 0.5)` (same as `src/estimator.h`).
* `G_rx(device, band)`: a per-device, per-band receive gain offset. Different radios disagree by
  several dB about the same field; this term cancels in *differential* measurements (§5.4).
* `L`: body/case loss, 3 dB prior for a handheld phone, 0 for a desktop.
* `b`: **shadowing**, log-normal: `b ~ N(0, σ_s²)`, `σ_s = 5 dB` (Wi-Fi), `6 dB` (BLE near a body).
  For a *static* link, `b` is a constant, not noise: averaging does not remove it. The filter
  therefore carries it as a state (§5.2), and calibration learns it.
* Spatial correlation of shadowing (Gudmundson): `ρ(Δ) = exp(−Δ / d_c)`, `d_c = 3 m` indoors.
  Two devices a few decimetres apart see almost the **same** shadowing from every AP.

Priors for `P0` when nothing is calibrated yet:

| Transmitter | Prior `P0` (dBm at 1 m) | σ |
|---|---|---|
| BLE beacon with advertised TX power `t` | `t − 41` | 6 dB |
| BLE beacon, TX power unknown (`127`) | −59 | 8 dB |
| Wi-Fi AP, 2.4 GHz | −40 | 8 dB |
| Wi-Fi AP, 5 GHz / 6 GHz | −46.5 / −48.2 (Friis ratio to 2.437 GHz) | 8 dB |

### 3.2 Fast fading, and why not to average dB

Multipath makes the instantaneous power `x = 10^(r/10)` fluctuate around its local mean `x̄`.
Without line of sight it is Rayleigh: `x / x̄ ~ Exp(1)`. In dB this is badly skewed:

```
E[10·log10(x/x̄)] = −10·γ/ln10 = −2.51 dB     (γ = 0.5772, Euler–Mascheroni)
std              = (10/ln10)·π/√6 = 5.57 dB
median           = 10·log10(ln 2) = −1.59 dB
quantile at p = 1 − 1/e = 0.632  →  exactly 0 dB
```

So the *mean of dB samples* is biased 2.5 dB low (that alone is a 25 % distance error at `n = 2`),
and deep fades drag it further. The unbiased estimator of the local mean power is the mean in the
**linear** domain (it is the maximum-likelihood estimate for exponential power). BeaconFix uses a
winsorised linear mean, robust to the rare interference spike:

```
level(r_1..r_N):
  x_i = 10^(r_i / 10)
  if N ≥ 8:  t = Q_0.95(x),  c = 0.95      (for Exp(1): E[min(X, −ln 0.05)] = 0.95 exactly)
  else:      t = +∞,          c = 1
  L = 10·log10( mean(min(x_i, t)) / c )
  σ_L = (10/ln10) / √N_eff = 4.343 / √N_eff  dB
```

`Q_p` is the "type 7" quantile (linear interpolation, the NumPy default): sort ascending,
`h = (N−1)·p`, `Q = s[⌊h⌋] + (h − ⌊h⌋)·(s[⌊h⌋+1] − s[⌊h⌋])`.

**Effective sample count.** Fading only averages out when samples are *independent*. For two
static devices in a static room the fade on one channel is frozen; only other channels and
people moving decorrelate it:

```
N_eff = min(N, K_ch · (1 + T / τ_c))
K_ch  = distinct channels in the window (BLE: 3 advertising channels; Wi-Fi scan: 1 per BSSID)
T     = window span (s),  τ_c = coherence time: 30 s still, 0.2 s moving
```

This is what keeps the reported error bar honest instead of shrinking forever.

### 3.3 Wi-Fi RTT

```
d_rtt = d + c_pair + e_nlos + w,   w ~ N(0, σ_rtt²),  e_nlos ≥ 0
```

* `σ_rtt` = the burst's reported standard deviation (Android `distanceStdDevMm`), floored at 0.3 m.
* `c_pair`: a constant offset per initiator/responder pair (antenna delay, chipset calibration),
  prior 0 ± 0.5 m, learned from a known distance (§8).
* `e_nlos`: multipath makes the first arrival look **late**, never early. The likelihood has a
  **symmetric Gaussian core** (`R = σ_rtt²`, so clean line-of-sight bursts stay unbiased) and
  asymmetric tails on the normalised innovation `z = y/√S`:
  * `z > 2` (late, NLOS): `R = (σ_rtt² + σ_nlos²)·(z/2)²`, `σ_nlos = 1 m` — a redescending weight,
    so a 5 m multipath outlier barely moves a 0.6 m estimate;
  * `z < −3` (early, which physics does not produce): plain Huber, `R = σ_rtt²·(−z/3)`.

  (An earlier draft gave every positive residual the NLOS variance; that biased clean data low —
  0.52 m for a true 0.6 m — and was replaced by the rule above.)
* Resolution scales with bandwidth: 20 MHz ≈ ±2.5 m per measurement, 80 MHz ≈ ±0.7 m.

## 4. Anchors

An **anchor** is a transmitter or place whose position you surveyed: "my AX210 antenna is
here", "the router is on this shelf". Anchors are ground truth; everything else is estimated.

### 4.1 Object (frozen contract — every app uses exactly these names)

```json
{
  "id": "0b8f1c9e-6f0a-4c52-9f6e-2d9a1c3b4e5f",
  "name": "AX210 antenna",
  "kind": "this-computer",
  "lat": 40.0029370, "lon": -75.0680600,
  "alt": 120.0, "heightM": 1.1, "floor": 0,
  "accM": 1.0,
  "bssids": ["02:00:00:00:00:01"],
  "ble": "28c9f0bf-a089-4a95-b632-5e8ede1b03b6",
  "rv": true,
  "rvOffset": {"eastM": 0.0, "northM": 0.0, "upM": 0.0},
  "ref": true,
  "headingDeg": 212.0,
  "placedBy": "widget",
  "placedAt": "2026-09-27T03:10:00Z",
  "source": "map-pick"
}
```

| Field | Type | Meaning |
|---|---|---|
| `id` | string (UUID) | stable identity; create/update is by id |
| `name` | string | shown in lists |
| `kind` | `this-computer` \| `wifi-ap` \| `ble` \| `rtt-responder` \| `gnss` \| `custom` | what is there (`gnss`: a GNSS receiver whose averaged position is ground truth, e.g. a Pi with a GPS HAT; `accM` = its reported σ, normally `rv: true`) |
| `lat`, `lon` | number | WGS-84 degrees |
| `alt` | number? | metres above sea level |
| `heightM` | number? | height above the floor |
| `floor` | int? | storey |
| `accM` | number | placement accuracy, 1-σ metres, **default 1.0** |
| `bssids` | string[] | **upper-case** `AA:BB:CC:DD:EE:FF`; every BSSID this anchor transmits |
| `ble` | string? | BLE service UUID or address the anchor advertises |
| `rv` | bool | moves with the RV |
| `rvOffset` | `{eastM, northM, upM}`? | offset from the RV reference anchor, set when placed |
| `ref` | bool | this is the RV reference (default: the `this-computer` anchor) |
| `headingDeg` | number? | RV heading when placed, degrees clockwise from north |
| `placedBy` | `desktop` \| `android` \| `widget` \| `laptop` \| `pi` | which app placed it (`pi`: the headless agent, docs/AGENT.md); unknown values are accepted and kept |
| `placedAt` | ISO-8601 | when it was placed or last moved |
| `source` | `map-pick` \| `gps-average` \| `rtt` \| `import` | how the position was obtained |

Output-only (added by the service, ignored on input): `headingAssumed: bool`, `seq: int`.
Sync rows may carry a tombstone: `{"id": …, "deleted": true, "deletedAt": ISO, "seq": …}`.

Normalisation on write: BSSIDs upper-cased and de-duplicated; `accM` clamped to `[0.05, 500]`;
missing `accM` → 1.0; missing `kind` → `custom`; at most one `ref: true` anchor per site
(setting one clears the others within 100 m); if no anchor is `ref`, the nearest
`this-computer` anchor acts as the reference.

### 4.2 Storage and protocol

* Map DB table `anchors(id TEXT PRIMARY KEY, json TEXT NOT NULL, kind TEXT, lat REAL, lon REAL,
  rv INTEGER, ref INTEGER, deleted INTEGER NOT NULL DEFAULT 0, updated TEXT, seq INTEGER)`,
  index on `seq`. `json` holds the whole object (forward compatible); the columns are for queries.
  Every write bumps `seq`, so anchors travel through `/db/changes` and `/db/sync` like other rows
  (`anchors:[…]` arrays). Merge rule: newest `placedAt` (or `deletedAt`) wins.
* `GET /api/v1/anchors` (read) → `[anchor…]`
* `POST /api/v1/anchors` (control) → create or update by `id` (an absent `id` creates one) → the stored anchor
* `DELETE /api/v1/anchors/<id>` (control) → `{"deleted": id}`
* D-Bus `Anchors()` → JSON array, `SetAnchor(json)` → id, `RemoveAnchor(id)`
* CLI `--anchors`, `--anchor-set <json | b64:<standard base64 of the UTF-8 JSON>>`, `--anchor-remove <id>`
* `--json` / StateJson gain `anchors:[…]` and `features:["anchors", …]`

### 4.3 How anchors are used

1. **`this-computer` replaces the desktop's own fix** while the desktop's best estimate is within
   100 m of it: position = the anchor, accuracy = `accM`, source `"anchor"`. It is also the
   position of the RTT responder and the BLE advertiser.
2. **`wifi-ap` pins its BSSIDs.** They are never refitted, they enter self-location as known
   transmitters with position error `accM`, and they are excluded from the "travelling" logic.
3. **Calibration.** Every RSSI sample taken at a known distance from an anchor (the desktop hearing
   a `wifi-ap` anchor from its own anchor; the phone at an RTT-ranged position) is a point
   `(x = log10 d, y = level)`. A recursive least-squares fit (§5.3) per band gives the
   environment's `P0` and `n`, which become the priors of every other AP's fit.
4. **Absolute rings.** A phone's RTT or BLE range to an anchored responder is a ring around the
   anchor: `p_phone ∈ circle(anchor, d)`. Combined with the phone's own fix (§5.5 on an absolute
   grid) it pins the phone far better than GNSS indoors.
5. **The RV frame.** When an `rv: true` anchor is placed, store its offset from the reference in
   east/north/up metres and the RV heading at that moment:
   ```
   (e, n, u)  = ENU(anchor − ref)                         (local tangent plane, §9.2)
   body frame = R(−h_placed)·(e, n)                        (forward/right of the RV)
   ```
   After a move (a new stop more than 250 m from where the reference was), the reference gets its
   new position (its own fix, or its anchor re-placed), and every RV anchor is re-projected:
   ```
   (e', n') = R(h_new − h_placed)·(e, n),     u' = u
   R(θ)·(e, n) = (e·cosθ + n·sinθ,  −e·sinθ + n·cosθ)       (θ clockwise, compass sense)
   anchor'  = ref_new ⊕ (e', n', u')
   ```
   If the new heading is unknown, `h_new = h_placed` (the offset is kept as is) and the anchor is
   flagged `headingAssumed: true` until someone re-places or rotates it in a picker. A known
   heading may come from the last travel leg's bearing or the user.
6. **`gnss` anchors.** A GNSS receiver that averages its fix (a Raspberry Pi with a GPS HAT, kind
   `pi`/`gnss` as a linked device) *is* an anchor whose position updates: `lat/lon/alt` = the
   averaged position, `accM` = its reported σ, `source: "gps-average"`, `rv: true`. When the RV
   moves, the gnss anchor is re-measured rather than re-projected, and it can serve as the RV
   reference (`ref: true`) — then every other RV anchor follows it with the §4.3.5 rotation.
7. **The `rv-gnss` positioning tier** (ahead of BeaconDB, Apple and IP in the locate chain).
   When a linked device of kind `pi` or `gnss` reports an averaged GNSS position with σ ≤ 5 m and
   the desktop is **at home** (hears at least one home AP), the desktop's own position is:
   ```
   if a gnss anchor and a this-computer anchor are both placed:
       p_desktop = p_gnss ⊕ (rvOffset(this-computer) − rvOffset(gnss))     (rotated per §4.3.5 if the heading changed)
       σ_desktop = √(σ_gnss² + accM_gnss² + accM_this²)
   else:
       p_desktop = p_gnss,  σ_desktop = √(σ_gnss² + 5²)                   (assumed 5 m RV extent)
   provider = "rv-gnss", source = "gnss"
   ```
   Observations the Pi pushes carry its averaged position and σ; the AP estimator weighs them as
   vantage points by `1/(1 + (σ/25)²)` (the same fix-accuracy weight as every other sample, so a
   2 m GNSS sample counts ≈ 1, a 40 m Wi-Fi fix ≈ 0.28).

## 5. Estimators

### 5.1 Level from faded samples
§3.2: winsorised linear mean, `σ_L = 4.343 / √N_eff`.

### 5.2 Range filter with offset states (cross-calibration built in)

One filter per peer device, state

```
x = [u, b_1, …, b_M, e_c]   u = log10(distance / 1 m),  b_k = offset of RSSI link k (dB),
                            e_c = error of the RTT pair offset c handed to the update (m)
```

`k` enumerates the RSSI links to that peer: `b_1` = `ble-down` (the phone hears the desktop's
advert), `b_2` = `ble-up` (the desktop hears the phone's). Prior `u₀ = log10(10 m)`, `P_uu = 1.0²`
(±1 decade); `b_k ~ N(0, 82.34 dB²)` = `σ_P0² + σ_s² + σ_ff²` = `36 + 36 + 5.57²/3` — the last term
is the **frozen multipath fade**: a still device sees one Rayleigh draw per advertising channel,
averaged over BLE's three channels that is a constant error of variance `5.57²/3`, which belongs in
the link offset, not in the per-sample noise. `e_c ~ N(0, 0.5²)` before calibration.

* **Predict** over `Δt` seconds: `P_uu += q·Δt`, `q = 0.002²/s` still, `0.05²/s` moving.
  Still: offsets drift by `P_bb += 10⁻⁴·Δt`. **Moving**: moving more than λ/2 draws a new fade, so
  each offset's variance is re-inflated by `σ_ff²` (capped at the prior). `P_cc += 10⁻⁶·Δt`.
* **RSSI update** (link `k`, level `L`, `σ_L`, model `P0_k`, `n_k`):
  `h = P0_k − 10·n_k·u + b_k`, `H = [−10·n_k, …, 1 (at b_k), …]`, `R = σ_L²`, Huber `k = 2.5`
  (`R ← R·|z|/k` when `|z| > k`).
* **RTT update** (`d_rtt`, `σ_rtt`, offset `c`): **iterated EKF to convergence** (≤ 20 iterations,
  stop when `u` and `e_c` move < 10⁻¹⁰; three iterations could not bridge a 17× jump from the
  prior), `h = 10^u + c + e_c`, `H = [ln10·10^u, 0, …, 1]`, `R` from §3.3's rule, re-evaluated
  every iteration.
* **`e_c` is a Schmidt "consider" state**: its gain is forced to 0 in every update, so its variance
  is carried (the calibrated offset keeps its own uncertainty and widens the range interval) but its
  mean is never estimated. Estimating it let the log-domain linearisation drift leak into the
  distance (+5 cm at 0.6 m); as a consider state the bias is +1.6 cm.
* **Log-range update** (`u_m`, `σ_u`): `H = [1, 0, …]`, `R = σ_u²` (used at the end of a
  calibration window: "they are D apart now").
* Every covariance update is in **Joseph form** `P = (I−KH)P(I−KH)ᵀ + KRKᵀ`, symmetrised; `u` is
  clamped to `[−2, 4]` (1 cm … 10 km).

Because `b_k` is a state, an RTT burst that pins `u` also **calibrates** every RSSI link to that
peer: the RSSI keeps tracking range changes when RTT drops out. This is RTT-supervised
cross-calibration, done in one place.

Outputs: `d = 10^u` (median), `σ_d ≈ ln10 · d · √P_uu`, interval `[10^(u−√P_uu), 10^(u+√P_uu)]`.

### 5.3 Recursive least squares for `P0` and `n`

Per link type and band, parameters `θ = [P0, n]`, regressor `h = [1, −10·x]`, `x = log10 d_true`:

```
S = h Σ hᵀ + R;   K = Σ hᵀ / S;   θ ← θ + K (y − h θ);   Σ ← (Σ − K h Σ) / λ
prior θ₀ = [P0_prior, n0],  Σ₀ = diag(8², 0.5²),  λ = 0.999,  n clamped to [1.5, 4.5]
```

With a single distance only `P0` moves (the prior holds `n`); with distance diversity both are
learned. This fits anchors (§4.3.3), user calibration (§8) and RTT-supervised pairs.

### 5.4 Differential shared-AP RSSI

For an AP `i` heard by both devices, desktop at `A`, phone at `B = A + δ`:

```
Δ_i = L_i^B − L_i^A = −10·n_i·log10(|B − a_i| / |A − a_i|) + Δg + (b_i(B) − b_i(A)) + noise
var(b_i(B) − b_i(A)) = 2·σ_s²·(1 − ρ(|δ|))        (Gudmundson: ≈ 0 when |δ| ≪ d_c)
```

The device gain difference `Δg` is common to every AP; nearby devices cancel both the transmit
powers and most of the shadowing. Home APs are **included**: the RV's own router, a couple of
metres away, has the steepest gradient (`10·n/(ln10·r)` ≈ 1.7 dB/m at 5 m) and is the best
co-location witness. BSSIDs of one physical transmitter (a router's several radios) form a *group*
that shares one position; their Δ are averaged per group.

**(a) Fingerprint distance — no geometry needed.**
```
Δg  = median_i Δ_i
e_i = Δ_i − Δg,   D² = (1.4826 · MAD(e))²    (robust dispersion; mean of e_i² if fewer than 5)
noise²  = mean_i (σ_L,A,i² + σ_L,B,i² + σ_dev²),   σ_dev = 1.5 dB
excess  = max(0, D² − noise²)
E(δ) = 2·σ_s²·(1 − exp(−δ/d_c)) + g²·δ²,   g = 10·n̄ / (ln10 · r̄ · √2),  r̄ = 15 m, n̄ = 2.4
δ̂ = the root of E(δ) = excess (bisection on [0, 200] m, 60 steps); δ̂ = 0 when excess = 0
bounds: the same inversion at excess·m/χ²_m(0.84) and excess·m/χ²_m(0.16)
        (Wilson–Hilferty: χ²_m(p) ≈ m·(1 − 2/(9m) + z_p·√(2/(9m)))³, z_0.84 = 0.9945, z_0.16 = −0.9945)
```
Requires `m ≥ 3` shared groups. It does **not** enter the range filter (that would count the same
fading twice as time goes on); it enters the §5.5 posterior directly as the χ² log-likelihood of
the observed dispersion at each candidate range `r`:

```
s(r) = E(r) + noise²,     log L(r) = −(m/2)·ln s(r) − m·D² / (2·s(r))
```

so a tight shared-beacon picture pulls the posterior towards small ranges and a scattered one away
from them, with the right weight for how many groups were compared.

**(b) Geometric solve — when ≥ 3 groups have fitted positions spanning > 60° as seen from A.**
Unknowns `[δ_E, δ_N, Δg]`, Gauss–Newton from `δ = 0` with Levenberg damping `10⁻³`, 20 iterations,
weights `1/(σ_L,A² + σ_L,B² + σ_dev² + 2σ_s²(1 − ρ(|δ|)) + (∇_i · σ_pos,i)²)` recomputed each
iteration, covariance `(JᵀWJ)⁻¹ · max(1, χ²/(m − 3))`. This gives a *direction* as well as a range.

### 5.5 Relative-position fusion (deterministic point-mass filter)

The quantity the map needs is `δ = p_phone − p_desktop` in metres (east, north). Its posterior is

```
p(δ) ∝ LogNormal(|δ| ; û, P_uu)                    ← the range filter's marginal (§5.2)
     · L_fp(|δ|)                                    ← §5.4(a): the fingerprint's χ² likelihood
     · N(δ ; μ_geo, Σ_geo)                          ← §5.4(b), when available
     · N(δ ; p_B − p_A, Σ_A + Σ_B)                  ← the two fixes (anchors make Σ_A tiny)
```

It is evaluated on a **polar grid** — 64 radii log-spaced over `û ± 4√P_uu`, 180 bearings — which
is a particle filter with deterministic particles: identical to 10⁻⁹ in both apps, no random
numbers, cheap (11 520 cells). Time recursion already happens inside the range filter and the
windowed direction sources, so nothing is double counted. Outputs:

* `distanceM` = weighted median of `|δ|`; `lowM`, `highM` = 16th / 84th percentiles; `sigmaM = (highM − lowM)/2`
* `bearingDeg` = weighted circular mean (clockwise from north, desktop → phone),
  `bearingSigmaDeg = √(−2 ln R̄)·180/π`; bearing omitted when `R̄ < 0.5` (direction unobservable,
  which is normal at sub-metre ranges: the map then shows a ring, not a point).

### 5.6 Error floors (Cramér–Rao)

* RSSI range: `σ_d / d ≥ ln10 · σ_L,tot / (10·n)`, `σ_L,tot² = 5.57²/N_eff + σ_offset²`.
  Uncalibrated BLE (`σ_offset ≈ 7.8 dB`, `n = 2`): ×/÷ 2.5 — useless for "2 ft".
  Calibrated (`σ_offset 1.5 dB`, 3 channels × 60 s still → `N_eff = 9`): ±31 %, i.e. 0.61 m → 0.47–0.80 m.
* RTT: `σ_d ≥ σ_rtt / √N_eff` + calibration of `c`: 20 MHz, 30 bursts → ≈ ±0.5 m.
* Fused: the product of the two; RTT dominates when present, BLE when not.

### 5.7 Classifier

```
evidence = no range measurements and no fix pair → "unknown"
highM ≤ 2.0             → "adjacent"
highM ≤ 6.0             → "room"
lowM  > 30.0            → "far"
highM ≤ 30.0            → "near"
otherwise               → "unknown"     (the interval spans near and far)
```
Pairing policy "required" accepts `adjacent` and `room` only.

## 6. The desktop's Wi-Fi RTT responder

The AX210 supports `ENABLE_FTM_RESPONDER`. `data/setup-rtt-responder.sh` gives it a second,
AP-type interface `bfrtt0` (a locally administered MAC derived from `wlp1s0`'s), runs `hostapd`
with `ftm_responder=1` on a hidden SSID with a random 63-character WPA2/WPA3 passphrase (nobody
joins it; RTT needs no association), and keeps NetworkManager away from `bfrtt0` so `wlp1s0` stays
managed for scanning. `beaconfix-rtt-responder.service` recreates it at boot. `--status`, `--remove`.

Interface combinations on the AX210 allow one AP next to the managed interface on **one channel**
(`#{managed} ≤ 1, #{AP} ≤ 1, #channels ≤ 1`).

**Channel choice.** Home routers usually sit in UNII-1 (5 GHz channels 36–48, one 80 MHz block,
5170–5250 MHz); a responder beaconing inside that block costs the router's clients airtime. The
script therefore prefers **UNII-3 channel 149, 80 MHz (centre index 155: `freqMHz 5745`,
`centerFreq0MHz 5775`), VHT preamble, 100 TU beacons, TX power 12 dBm** (set with `iw` once the AP
is up, because hostapd has no TX-power option): outside the router's block, and 12 dBm is plenty for
a ranging partner a few metres away. When the regulatory domain or the radio does not allow 149 it
falls back to channel 36 with **300 TU** beacons (a third of the airtime), and then to 2.4 GHz
channel 1 / 20 MHz.
`noscan` is not used (Ubuntu's hostapd 2.10 rejects it).

**Robustness (2026-09-27).** At boot the unit can start before the PCIe card and its firmware are
up, and the radio's phy index is not stable (`phy0` can come back as `phy1` after the card is
re-attached or the driver reloads). The helper therefore finds the radio by the managed interface's
MAC (then by name, then by the `ENABLE_FTM_RESPONDER` capability), waits up to 90 s for it, sets
the regulatory domain and waits until the kernel reports it, and retries adding `bfrtt0`. The
country is also made persistent (`/etc/modprobe.d/beaconfix-regdom.conf`:
`options cfg80211 ieee80211_regdom=<CC>`, the `--country` code or the one BeaconFix detected; nothing
is written for the world domain `00`), and a udev rule (`/etc/udev/rules.d/90-beaconfix-rtt.rules`)
starts the unit whenever a Wi-Fi radio appears (boot, hot-plug). A scan on `wlp1s0` while hostapd
starts makes the radio `EBUSY` and hostapd then gives up **with exit status 0**, so the helper aborts
any running scan right before hostapd starts and the unit restarts on any exit
(`Restart=always`, `RestartSec=10`, `StartLimitBurst=30` per 600 s, `TimeoutStartSec=180`).

The phone builds a `ResponderConfig` from `GET /api/v1/ranging/info` (it does not need the hidden
AP in its scan results) and ranges at 1 Hz while a session is active.

## 7. Protocol

All under `/api/v1`, JSON, LAN-only like the rest of the API.

`GET /ranging/info` (read):
```json
{
  "rtt": {"bssid": "02:00:00:00:00:01", "freqMHz": 2412, "centerFreq0MHz": 2412, "bandwidthMHz": 20,
          "channel": 1, "preamble": "ht", "enabled": true, "anchorId": "…"},
  "ble": {"serviceUuid": "28c9f0bf-a089-4a95-b632-5e8ede1b03b6", "txPower": 7, "txPowerConfirmed": true, "enabled": true, "intervalMs": 200},
  "anchor": {"…": "the this-computer anchor, if any"}
}
```

`POST /ranging` (read scope, from the peer; the server uses the **authenticated** device, the
`device` field is advisory):
```json
{
  "device": "Pixel 10 Pro XL", "time": "2026-09-27T03:12:00.000Z",
  "rtt":  [{"bssid": "02:00:00:00:00:01", "distMm": 1180, "stdMm": 640, "rssi": -38, "burst": 8, "n": 7, "time": 1790478720000}],
  "ble":  [{"rssi": -52, "channel": 38, "txPower": -7, "time": 1790478720100}],
  "wifi": [{"bssid": "06:11:22:33:44:5B", "rssi": -55, "freq": 5240}],
  "baro": {"hPa": 978.41},
  "moving": false,
  "fix":  {"lat": 40.00291, "lon": -75.06808, "acc": 12.0, "time": 1790478719000, "source": "gps"},
  "rttState": "ok"
}
```
`ble[]` is what the **phone heard from the desktop** (`channel` omitted when unknown — Android does
not expose it). `time` values are epoch milliseconds. The reply is that device's current estimate
(the object below). `rtt`, `ble`, `wifi` (possibly empty), `moving`, `ble[].txPower` (127 = unknown)
and `fix.source` are always sent (Android 1.3.2+; 1.3.1 and older left out empty arrays,
`moving: false`, `txPower: 127` and `source: "gps"`, which the desktop reads as those same values);
`channel`, `baro`, `fix` and `rttState` are sent only when known.

`rttState` (optional, Android 1.3.1+) says why `rtt[]` is or is not empty, so a count of 0 comes with
a reason: `ok`, `doze`, `wifi-off`, `location-off`, `unavailable`, `unsupported`, `no-permission`,
`no-response`, `not-80211mc`, `timeout`, `bad-config`, `no-responder`, `idle` (not in a session:
the collector is off and the app is in the background), `away` (1.3.2: the desktop has not answered
for 5 min, or neither the responder's beacon nor the desktop's BLE advert was heard in the last few
minutes, in which case one probe burst a minute), `backoff` (1.3.2: the last bursts got no answer; two quick
retries, then 5 s doubling to 2 min), or `failed:<code>`.
In deep Doze the app waits a minute between ticks (or until `rttState` changes) instead of 2.5 s,
advertises at 1 s and scans at the low-power rate; unanswered POSTs back off from 5 s to 5 min. The
minute is a coroutine timer on the monotonic clock, which stops while the CPU sleeps and the app holds
no wakelock, so in deep Doze the posts are rarer still: on 2026-09-27 (1.3.2; adb polled the phone every
few seconds, which itself wakes it) none between 09:47:01 and the maintenance window at 10:13:32,
then one every ~1.7–2 min (10:17–10:25, each carrying the
~20 samples of the desktop's advert it heard meanwhile). Its own advert keeps going (the Bluetooth
controller sends it), so the desktop's up link keeps ranging throughout.
**`doze` is the common one.** Android switches Wi-Fi RTT off for the whole device in deep Doze
(AOSP `RttServiceImpl.isAvailable()` is false while `PowerManager.isDeviceIdleMode()`), and a
locked, still, unplugged phone reaches deep Doze a minute or two after the screen goes off. Showing
the app over the lock screen does not end it, and neither do the battery-optimisation allowlist or a
wakelock; unlocking, a charger or moving the phone does, and so does each Doze maintenance window
(30 s – 5 min, roughly every 1–6 h while the phone lies still). The app re-ranges the moment
`ACTION_DEVICE_IDLE_MODE_CHANGED` / `ACTION_WIFI_RTT_STATE_CHANGED` say RTT is back (seen live on
2026-09-27: maintenance window 08:12:36, first answered burst 7 s later, 16 bursts in 56 s; with 1.3.2
the window 10:13:32–10:15:42: first burst timed out, retried at once, answered 10:13:39, then 42
answered bursts, one every ~2.65 s).

`GET /ranging` (read):
```json
{"updated": "…", "anchor": {"…": "…"}, "devices": [
  {"device": "Pixel 10 Pro XL", "distanceM": 0.63, "sigmaM": 0.14, "lowM": 0.50, "highM": 0.78,
   "method": ["rtt", "ble", "wifi-diff"], "bearingDeg": null, "bearingSigmaDeg": null, "dz": null,
   "class": "adjacent", "updated": "…",
   "samples": {"rtt": 30, "ble": 412, "bleDown": 200, "bleUp": 212, "wifiDiff": 11,
               "since": "2026-09-27T07:39:43.159", "total": {"rtt": 139, "bleDown": 200, "bleUp": 7354}},
   "lastRtt": "…", "rttState": "doze", "rttStateAt": "…",
   "calib": {"rttOffsetM": 0.57, "bleP0": -47.2, "bleN": 2.0, "bleP0Up": -49.8}}]}
```
`samples` counts since the tray started (`since`); `total` adds earlier runs (kept in
`ranging.json`, written at most once a minute). `rttState` is the peer's last `rttState`.

`POST /ranging/calibrate` (control): `{"device": "…", "distanceM": 0.61, "durationS": 20}` — §8.

## 8. Calibration

* **One tap: "these two are D metres apart".** (`POST /api/v1/ranging/calibrate`,
  `beaconfix --ranging-calibrate "<device>@<metres>[@<seconds>]"`.) Collect `durationS` (default
  20 s, 5–120) of RTT and BLE in both directions without feeding the filter, then:
  * RTT: `c = m − D`, where `m` is the median of the **densest 2 m cluster** of the window's bursts
    (the nearest one on a tie: time-of-flight errors are late). It needs `N ≥ 3` bursts in that
    cluster and at least 30 % of all bursts; otherwise `c` is left as it was. A plain median was not
    robust enough: in the 0.6 m calibration of 2026-09-27 the Pixel reported 8 bursts at 14.4–14.7 m
    and 7 at 196–408 m, the median landed on the cluster's edge (c 14.11 m instead of 13.95 m), and one
    more bad burst would have made it ~200 m. The median of the `N` cluster bursts has
    `σ ≈ 1.2533·σ_burst/√N`, plus 5 cm for the tape measure: `e_c` is reset to
    `N(0, (1.2533·σ_b/√N)² + 0.05²)`. The event log says how many bursts agreed.
  * BLE, each link: an RLS update at `x = log10 D` with the window's level (`R = σ_L²`), which
    moves `P0` and leaves `n` at its prior; the link offset `b_k` is reset to 0 with variance
    `max(σ_L², σ_ff²) + σ_ff²`. The calibration pins `P0` and the shadowing, **not** the fade at
    the next spot: putting the phone down again moves it by more than λ/2 (6 cm) and draws a new
    fade, and a still calibration cannot know even its own spot's fade better than `σ_ff`.
    *Nudge the phone a few centimetres now and then during the window* so the fade averages out.
  * Finally a log-range update `u = log10 D`, `σ_u = 0.01`: they are D apart when the window closes.
  Calibrations persist per device in `~/.local/state/beaconfix/ranging.json`; the filter state does not.
* **Automatic.** Whenever the range filter's `√P_uu < 0.05` with RTT in the last 30 s, each BLE
  level is also an RLS sample at `x = u` (RTT-supervised) — **only once the pair's RTT offset has
  been calibrated** (a calibration window that had RTT bursts). An uncalibrated pair is not a known
  distance: the Pixel 10 Pro XL ↔ AX210 pair read ~11 m at 0.6 m on 2026-09-27 and the automatic
  step had taught both BLE links that bias. Until then `e_c` starts at `N(0, 2²)` (σ 2 m, was 0.5 m),
  which also keeps `√P_uu` above the gate. A calibration restarts each link that has samples in the
  window from its TX-power prior, and only calibrated peers' RLS state is restored at start.
  Anchors feed §4.3.3.
* **The first calibration point.** On 2026-09-27 the Pixel lay 0.61 m (2 ft) from the desktop.
  That distance is the seed for `c_pair` and the BLE `P0`s (§10).

## 9. Implementation notes

### 9.1 BLE advertisement (both directions)

Legacy, non-connectable, non-scannable. Payload (31 bytes max). Any BeaconFix device advertises
this — desktop, phone, laptop, the Pi agent — so every pair of devices can measure the link both ways:

* Service Data – 128-bit UUID (AD type `0x21`): UUID `28c9f0bf-a089-4a95-b632-5e8ede1b03b6`
  (little-endian on air) followed by 10 bytes:

| Bytes | Field |
|---|---|
| 0–7 | `tag` = first 8 bytes of `SHA-256("beaconfix-ble-v1|" + identityId + "|" + ⌊unix_seconds / 900⌋)` |
| 8 | TX power, int8 dBm; `127` = unknown |
| 9 | flags: bit0 RTT responder available, bit1 API reachable, bits 2–4 kind (0 desktop, 1 android, 2 laptop, 3 pi, 4 gnss, 5–6 reserved, 7 other), bit5 calibrating, bits 6–7 version (0) |

**BlueZ note (2026-09-27).** Ubuntu's `bluetoothd` 5.72 sizes `MGMT_OP_ADD_EXT_ADV_DATA`
with the legacy `mgmt_cp_add_advertising` header (8 stray bytes). Kernel 7.0.0-30 carries the stable
fix `d3f7d17960ed` ("validate Add Extended Advertising Data length", an exact-length check) but not
its follow-up `149324fc762c` (relaxed to "not shorter"), so **every** LE advertisement failed with
`Invalid Parameters (0x0d)` and `ActiveInstances` stayed 0 — the kernel accepted the parameters and
rejected the 28-byte data. BlueZ ≥ 5.8x sizes the command correctly. **Requirement:** BLE ranging
needs a `bluetoothd` from BlueZ 5.8x or newer on kernels that carry the exact-length check without
its follow-up; with an older distribution BlueZ, install a newer one (a source build of BlueZ ≥ 5.8x,
started through a `systemd` drop-in for `bluetooth.service` that points `ExecStart` at the new
`bluetoothd`; remove it once the distribution ships a fixed BlueZ or kernel).
`/usr/libexec/bluetooth/bluetoothd --version` shows the distribution's version, and
`sudo btmgmt advinfo` lists an instance while BeaconFix advertises. The advert then registers with
the TX-power include (31 bytes, legacy PDU).

**TX power (byte 8).** The desktop requests a fixed level through `LEAdvertisement1.TxPower` when
BlueZ offers `CanSetTxPower` (7 dBm, clamped to `SupportedCapabilities` Min/MaxTxPower; the AX210
reports −34…+7) and puts that value into byte 8 and `/ranging/info` `ble.txPower`. Once the controller
has accepted the parameters, BlueZ writes the level it actually **selected** (HCI LE Set Extended
Advertising Parameters → `Selected_TX_Power`, via MGMT Add Ext Adv Params) back into the
advertisement's `TxPower` property (`src/advertising.c` `add_adv_params_callback`); the desktop's
property is writable for that, adopts the selected level for byte 8 (re-registering once if it
differs from the request) and reports `ble.txPowerConfirmed: true`. The capability probe is
asynchronous (the advert is registered when BlueZ has answered or after 2 s). Before 3.7.0's
fix it said 127 while the controller picked 10 dBm on its own, and every down link used the −59 dBm
"unknown" prior instead of `tx − 41` — a 25–28 dB wrong reference. Without `CanSetTxPower` byte 8
stays 127. Scanners that see 127 fall back to the TX-power AD (BlueZ `Device1.TxPower`).

**Who is who.** The desktop resolves a tag by computing the tags of every identity it knows —
paired devices that signed in with an identity, linked identities, and **pending link requests**
(a phone that asked to link is recognised before the link completes) — for windows `w−1…w+1`.
The **Pi agent** has no identity; its advert's tag is made from
`beacon_id = hex(SHA-256("beaconfix-agent-beacon-v1|" + desktopIdentityId + "|" + deviceName))[:26]`,
which the desktop derives for every paired device without an identity. (It used to advertise the
desktop's own identity with kind `pi`: phones counted it as the desktop, and the desktop dropped it
as its own advert.) A phone attributes a desktop's tag only to an advert of kind `desktop` or
`laptop`.
An unknown tag of the same kind as the one device that is posting `/ranging` right now is bound to
that device for the window (session binding). The kind bits also tell the desktop what a device is
(`linkedDevices[].kind`).

`identityId` is the canonical 26-character lower-case id. The tag **rotates every 15 minutes** so
a passer-by cannot track the device by a fixed number; a peer that knows the identity (itself, or
linked) checks the current, previous and next window. Android also randomises its BLE address.
Interval 100–200 ms during a ranging session, 1 s otherwise. Android: `AdvertisingSetParameters`
(legacy, non-connectable, `TX_POWER_MEDIUM`), and put the TX power reported by
`onAdvertisingSetStarted` into byte 8. Scanning: filter on the service-data UUID,
`SCAN_MODE_LOW_LATENCY` during a session.

### 9.2 Local frame, units, determinism

* Local tangent plane (same constants as `src/estimator.h`): `east = (lon − lon₀)·111320·cos(lat₀)`,
  `north = (lat − lat₀)·111320`, inverse likewise. Bearings are degrees clockwise from north.
* All maths in IEEE double; quantiles type 7; no random numbers anywhere (§5.5 is a grid).
* Kotlin (`RangeMath.kt`) mirrors the C++ function by function; the vectors in §11 are asserted
  to 1e-9 relative in both.

### 9.3 Phone ↔ phone

Two Android devices can use Wi-Fi Aware RTT (`WIFI_FEATURE_D2D_RTT`), BLE Channel Sounding
(`bluetooth_le.channel_sounding`, cm-level) or UWB when both have it. They enter §5.2 as range
updates with their own `σ`; nothing else changes.

## 10. Field record

**Hardware (2026-09-26/27).** Desktop: KDE neon in a VM, Intel AX210
passed through (PCIe; Bluetooth half on USB 8087:0032), phone Pixel 10 Pro XL (Android 17).

| Item | Value |
|---|---|
| Responder interface | `bfrtt0` on the AX210's phy, beside managed `wlp1s0` |
| BSSID | `02:00:00:00:00:01` (wlan MAC with the locally-administered bit) |
| Channel | 149, 80 MHz, `freqMHz 5745`, `centerFreq0MHz 5775`, VHT |
| TX power / beacon | 12 dBm / 100 TU, hidden SSID, WPA2/WPA3 with a random passphrase, empty MAC allow-list |
| Desktop scanning with the AP up | 44 APs before, 53 after (dual-band scanning unaffected) |
| Restarts | 6 of 6 `systemctl restart` came up on channel 149 after the EBUSY fix |
| BLE advert | active (`ActiveInstances 1`) with BlueZ 5.87; phone's advert heard and resolved |

**First BLE look (phone reported ≈ 0.6 m away).** The desktop heard the phone's advert (TX power
byte −7 dBm) at −80 … −85 dBm. Free space predicts ≈ −44 dBm at 0.6 m for −7 dBm, so this link is
~40 dB below the textbook — an antenna/placement loss that only calibration can remove (it is a
constant offset: exactly what `P0` absorbs). Uncalibrated the filter therefore said 16 m
[6, 43] — the honest answer for an uncalibrated BLE link (§5.6: ×/÷ 2.5); after the one-tap
calibration the same link gives ±30 %.

**First RTT and calibration (2026-09-27, phone ≈ 0.6 m, locked, in deep Doze; RTT only in Doze
maintenance windows).** Window 10:13:32–10:15:42, ch 149 / 80 MHz VHT, responder RSSI −77 … −80 dBm:
42 answered bursts of 8 (4–7 successful FTM exchanges each), 2 timeouts. 36 bursts read
14.16–15.07 m (median 14.44 m, sd 0.20 m); 6 (14 %) were gross outliers at 26, 196, 196, 200, 210
and 251 m (the 08:12 window had 7 of 16 at 196–408 m). The cluster sits ~13.8 m above the true
range at every look (07:07–07:12: all 131 in AOSP's 5–15 m bucket; 08:12: 14.4–14.7 m; 10:13:
14.2–15.1 m), i.e. a constant pair offset, not a path: time of flight measures the first arrival.
A 30 s calibration at 0.6 m (10:13:41–10:14:12, 9 of 11 bursts in the densest 2 m cluster) gave
`c_pair` = 13.767 m (σ 0.13 m); the 26 bursts after it read 0.68 m median (sd 0.21 m, 0.42–1.30 m)
and the fused estimate 0.61 m [0.58, 0.64]. The same window calibrated the BLE links:
`P0up` −92.2 dBm (phone at −7 dBm; ~50 dB below free space) and `P0` (down, the desktop's 7 dBm
advert, TX power confirmed by BlueZ) −77.5 dBm, n ≈ 1.6; RTT-supervised learning then moved them to
−93.9 / −78.8 within the window. BLE only afterwards (tray restarted, so a fresh filter; phone back
in Doze), 10:17–10:26: 0.53–0.63 m with 16–84 % intervals of about [0.35, 0.81] to [0.41, 0.96],
every one containing 0.6 m. Before this calibration the same links read 10.7–15 m.

**Accuracy in simulation** (`tests/ranging_math_test.cpp`, deterministic SplitMix64; realistic
Rayleigh fades, shadowing, device gain, NLOS bursts; the fade is redrawn when the phone is put
down after calibrating):

| Scenario | Estimate | 16–84 % interval | Error |
|---|---|---|---|
| 0.6 m, BLE only, uncalibrated | 0.85 m | [0.42, 1.72] | +41 % |
| 0.6 m, BLE only, calibrated at 0.61 m | 0.67 m | [0.47, 0.97] | +12 % |
| 0.6 m, RTT 80 MHz (+0.55 m pair offset) + BLE, calibrated | 0.63 m | [0.54, 0.74] | +5 % |
| 3 m, RTT 20 MHz + BLE, calibrated at 0.61 m | 3.30 m | [3.04, 3.59] | +10 % |
| 12 m, RTT 20 MHz, 30 % NLOS bursts + BLE | 12.19 m | [11.85, 12.54] | +2 % |
| 4 m, BLE link offset learned away by RTT | 4.01 m | [3.51, 4.58] | 0 % |

| Monte Carlo (200 runs) | RMS error | Interval covers the truth |
|---|---|---|
| BLE calibrated, 0.6 m | 0.216 m | 76 % |
| RTT + BLE, 0.6 m | 0.066 m | 86 % |
| 3 m | 0.244 m | 72 % |
| 12 m, NLOS | 0.362 m | 67 % |

(The ideal coverage of a 16–84 % interval is 68 %.) Trilateration from RTT rings to three
anchors: RMS 0.63 m against a reported 0.63 m. Rayleigh: the mean of dB is biased −2.49 dB, the
winsorised linear mean 0.00 dB. Differential RSSI with 12 APs, true δ 1.5 m: geometric 1.63 m
± 2.64 m, gain −3.92 dB (true −4).

## 11. Test vectors

Generated by `tests/ranging_math_test.cpp --vectors`; the C++ test and
`android/app/src/test/java/org/sworrl/beaconfix/ranging/RangeMathTest.kt` both assert every value
to 1e-9 relative.

```json
{
 "anchor_calibration": [
  -37.295802015063686,
  2.4287654109893175,
  0.7143219926188046
 ],
 "ble": [
  "898131b88e0457b0",
  7,
  "898131b88e0457b0f907",
  -7,
  1,
  true,
  true,
  0
 ],
 "ble_window_neg": "cf6fff450eef869b",
 "chi2_6_+": 9.229109314951645,
 "chi2_6_-": 2.7560888499578042,
 "classify": [
  "unknown",
  "adjacent",
  "room",
  "far",
  "near",
  "unknown"
 ],
 "enu": [
  1.7054479876969342,
  2.0037599996868494,
  1.2000000000000028
 ],
 "filter": [
  0.07689093392987602,
  -2.346415535374766,
  -3.216278090901163,
  0.031994050882637924,
  0.6099683097169435,
  15.463545696660228,
  11.340825811962223,
  16.87439220342253,
  1.1936882912146383,
  0.49163323972359524,
  0.7907189095276757,
  1.8020205656066342,
  -0.051605713228050784,
  -0.12249847699217876,
  -0.1944921230282392,
  -0.8251850909005902
 ],
 "filter_drift": [
  0.6298835297924379,
  2.2340875206714412,
  0.18792407716665965,
  3.578842109814038,
  82.34179352734864,
  91.34179352734864,
  0.253602,
  0.4333269413222595
 ],
 "filter_rtt_robust": [
  1.3733860610084303,
  0.00012094918043716869,
  23.62577485657404
 ],
 "fingerprint": [
  -3.450000000000003,
  2.6597122993644633,
  13.321666666666667,
  0,
  0,
  0,
  0,
  6,
  -10.255926479189677,
  -13.444994423861454
 ],
 "fingerprint_far": [
  4,
  54.952733457943324,
  44.702733457943324,
  4.666438293955714,
  1.9159647199595962,
  17.307883242697173
 ],
 "fit": [
  -39.853867193347156,
  2.271109132634109,
  6.269943059867058,
  0.09800952168791119,
  0.5619772550236338,
  0.4124708581136841
 ],
 "fspl_0.61m_5180": 42.44319189512001,
 "fspl_1m_2437": 40.18711058369449,
 "geo": [
  1.1999999997307187,
  -0.7999999997493872,
  -2.9999999999804126,
  4,
  242.26182037161277,
  18.70447781205538,
  20.18114356805259,
  5.554126396376508,
  4.885697637654346e-21
 ],
 "level1": [
  -50.42527605252986,
  1.9422239675774466,
  5
 ],
 "level2": [
  -59.92358370267848,
  2.507400360344115,
  3
 ],
 "median": 4,
 "model_distance": 0.6095368972401694,
 "model_level": -45.70659670021534,
 "prior_ble_-7": -48,
 "prior_ble_127": -59,
 "prior_wifi_5180": -46.549484611210175,
 "q7_0.632": 5,
 "q7_0.95": 7.5,
 "rel_fp": [
  7.186558590674563,
  1.7593465916496707,
  17.065026528572087,
  "near"
 ],
 "rel_range": [
  0.61,
  0.48511734778731724,
  0.7670309084950191,
  0.14095678035385092,
  0,
  5.4745342114606975e-17,
  "adjacent"
 ],
 "rel_range_fix": [
  45.09074549867884,
  31.860582597534947,
  60.591585411392664,
  36.86989764584402,
  26.80350483842413,
  0.8963513400468486,
  "far"
 ],
 "reproject_known": [
  40.0029712540424,
  -75.06801759070221,
  120.4,
  false
 ],
 "reproject_unknown": [
  40.002986525332375,
  -75.0679624731686,
  120.4,
  true
 ],
 "rls": [
  -49.265931839945814,
  1.9142909075068142,
  5.806407120458949,
  0.3608712634841288,
  0.14915744844330686
 ],
 "rotate": [
  4,
  -2.9999999999999996
 ],
 "rv_gnss": [
  40.0029820337765,
  -75.06816449924145,
  1.8920887928424501,
  true,
  40.003,
  -75.0681,
  5.314132102234569
 ],
 "trilat": [
  40.00291088304175,
  -75.06803931836865,
  0.4455765733186116,
  1.3221886849534632,
  14
 ]
}
```
