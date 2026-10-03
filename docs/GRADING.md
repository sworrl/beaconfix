# Grading: how sure are we where an access point is?

Every access point BeaconFix positions from its own samples gets a **graded estimate**: a
position, a calibrated uncertainty (the 95 % region, the chance of being within 25 m), a
0–100 **score** and a **letter**. This page defines every number, the formula behind it and
where it comes from. The code is `src/estimator.{h,cpp}` and its line-by-line Kotlin twin
`android/app/src/main/java/org/sworrl/beaconfix/estimate/Estimator.kt`; both are pinned by
the golden vectors in `tests/fixtures/estimator_golden.json` (§7).

| letter | score | meaning |
|---|---|---|
| **A** | ≥ 85 | excellent: tight, surrounded, consistent |
| **B** | 70–84 | good |
| **C** | 55–69 | fair |
| **D** | 40–54 | weak (also the ceiling for an ambiguous fit) |
| **E** | 20–39 | poor |
| **F** | < 20 | unreliable |
| **R** | ≤ 39 | region only: we know the area, not the spot (one or two places, or R95 > 150 m) |
| **M** | – | mobile: it travels (with us, or it was heard more than 5 km apart) |

Colours everywhere (desktop map, widget, Android), Okabe–Ito so they survive colour-blindness:
A `#009E73` · B `#56B4E9` · C `#F0E442` · D `#E69F00` · E `#D55E00` · F `#CC79A7` ·
R `#8A93A6` · M `#0072B2`. Maps draw the **95 % ellipse** (dashed when extrapolated or
ambiguous), regions as quiet discs of radius R95, mobile APs as an "M".

## 1. The model and the fit

### 1.1 Signal model
Samples taken within a few metres of each other are one **place**: a sample joins the first
place whose seed is within `max(5 m, 1.5 × the better fix accuracy of the two)`; a place's level
is the median of its samples. Two precise vantage points (a smoothed walk, 4–8 m) a few metres
apart stay apart, so a walk around a building keeps its geometry; poor fixes (a phone indoors,
20–40 m) still merge, since their separation is noise. (Until estimator 3 one radius,
`max(15 m, median accuracy)`, collapsed a 60–120 m walk around a building to 6–8 places.)

```
y_k = P0 + g_dev + δ_dev − 10·n·log10(d_k),   d_k = √(|p − q_k|² + h²)
```

* `P0` level at 1 m, Gaussian prior per band (2.4 GHz −40 ± 8 dBm; 5 GHz −47; 6 GHz −48:
  the free-space loss at 1 m is `20·log10(f MHz) − 27.55` = 40.2, 47.3, 48.2 dB).
* `n` path-loss exponent, prior 2.4 ± 0.5 (2.4 GHz) or 2.7 ± 0.5 (5/6 GHz); the anchors'
  environment calibration (docs/RANGING.md §4.3.3) replaces the mean when it exists.
* `h` = 3 m, the AP's height above the observer: no 1/d² blow-up for a sample under the AP.
* `g_dev` the hearing device's level offset (§3.3); this host is 0.
* `δ_dev` that device's deviation for *this* AP (estimator 4): antennas, a body in the way, or simply
  where the device sits make one device hear one AP very differently from its average offset —
  measured: the Steam Deck hears the RV's hidden AP 7 dB quieter than its calibration, and the RV's
  UniFi ~30 dB louder than the phone at the "same" GPS spot (it sits next to it). The device with the
  most places is the reference (`δ = 0`, it carries `P0`); the next four by places (ties: name order)
  each get `δ ~ N(0, 10²) dB`, integrated out on the grid (§1.3) and fitted by LM (§1.4); any further
  device stays at its calibrated offset. A second device therefore informs through how its levels
  *vary* between its places, not through their absolute level, and **places never mix devices**.
  (Until estimator 3 `g_dev` was taken as exact and devices were pooled into one place's median: the
  Deck's 11 readings of the hidden AP dragged it 12 m — 16.9 m → 8.9 m with δ.)
* `ε_k` shadowing, σ0 = 6 dB ([typical 4–8 dB](https://ieeexplore.ieee.org/document/8409563)).

### 1.2 Per-place variance, including the observer's own error (errors-in-variables)
```
σ_k² = σ0²·(ρ_in + (1 − ρ_in)/m_k) + (b·ρ_k/d_k²)² · a_k²,   b = 10·n/ln 10
```
`m_k` samples in the place (ρ_in = 0.6: samples a few metres apart share most of their
shadowing), `ρ_k` the horizontal distance, `a_k = acc/1.515` the per-axis σ of a fix whose
accuracy is a 68 % radius (Android, GeoClue). A near sample with a poor fix therefore
carries little information. Age (`exp(−age/180 d)`, floor 0.15) and status weights are
*relative*: they shift influence between places, not the total information.
([anchor-position uncertainty in RSS localisation](https://onlinelibrary.wiley.com/doi/10.1155/2023/9274297))

### 1.3 Grid posterior
A 48 × 48 grid around the places (half-width 120–600 m). At each cell the model is linear in
`β = (P0, n)`, so β is integrated out in closed form (Gaussian prior, Woodbury on 2×2 sums):
`log L = −½[yᵀD⁻¹y + μᵀΣβ⁻¹μ − ηᵀΛ⁻¹η] − ½ log|Λ| − ½ Σ log σ_k²`, plus

* a **range prior** `−(distance to the nearest place)/150 m`: without it the far field, where
  every place is about equally far away, collects posterior mass by sheer area;
* **Wi-Fi RTT ranges** when a sample carries one: `−½ (r − d)²/σ_r²`;
* **misses** (§1.7).

Outputs: the global maximum (the fit's first seed), the posterior mean and covariance, the
**95 % highest-posterior region** (`R95_hpd = √(area/π)`) and the number of **modes**
(8-connected components of that region holding ≥ 5 % of the mass). After the robust fit the
grid is recomputed with each place weighted as the robust fit weighted it.

### 1.4 Robust Levenberg–Marquardt
From every grid maximum, the signal-weighted, plain and loudest-quarter centroids, and the
**mirror** of the best seed across the places' principal axis (the classic drive-by
ambiguity), Levenberg–Marquardt on `(x, y, P0, n, δ…)` with the priors as pseudo-observations.
The loss is the negative log-likelihood of a **Gaussian + uniform outlier mixture**
(10 % outliers over 80 dB): the IRLS weights are the EM responsibilities, a gross outlier
costs a constant. First at the nominal scale, then at the pooled robust scale
`τ² = (4 + K·MAD²)/(4 + K)` (MAD of the standardised residuals, four pseudo-observations of 1).
The lowest cost wins. Then the **mirror of that solution** is fitted too (estimator 4): from places
along a line the AP and its reflection explain the levels equally, and no seed need have started on the
other side — before, half the drive-by fits sat on the mirror and only half of those were flagged. ([Student-t / mixture losses](https://www.emergentmind.com/topics/student-s-t-distribution-induced-loss-function),
[LOS/NLOS mixtures](https://jwcn-eurasipjournals.springeropen.com/articles/10.1186/s13638-018-1335-7))

### 1.5 Covariance
1. **Noise scale** `τ² = σ²/σ0²`: the posterior mean under a scaled-inverse-χ² prior of
   `ν0 = 6` pseudo-dof, centred so that `E[σ²] = σ0²` without data, updated by the robust-weighted
   residual sum of squares `SS = Σ W_k u_k z_k²` (`z_k = r_k/σ_k`, `u_k` the EM weight) over the
   *effectively independent places*:
   `τ² = (ν0 − 2 + SS·N_eff/K) / (ν0 − 2 + ν_d)`, `ν_d = (K_in − p_eff)·N_eff/K`,
   `K_in = Σ W_k u_k`, `p_eff` the trace of the hat matrix (the P0/n priors count fractionally),
   `N_eff = 1ᵀR⁻¹1` with `R_ij = exp(−|q_i − q_j| / 8 m)` ([Gudmundson](https://www.researchgate.net/publication/3376340_Correlation_model_for_shadow_fading_in_mobile_radio_channels)).
   Samples never count as degrees of freedom, places only as far as they decorrelate: many
   well-fitting places shrink σ, a handful (or one tight cluster) leave it near σ0, a poor fit
   widens it. (Until estimator 3: `max(1, τ²)`, so a good fit could never shrink below σ0.)
2. **Sandwich** Laplace at that scale: `A⁻¹ B A⁻¹`, `A = JᵀΨJ + prior` (the fit weighs places as
   independent), `B = A + Σ_{k≠l} ψ_k ψ_l τ²σ0²ρ_in R_kl J_k J_lᵀ` (their shared shadowing). This
   replaces the scalar design effect `K/N_eff` — the variance inflation of a *mean* — which
   double-counted: the common part of correlated shadowing falls into P0 and does not move the
   position; what moves it is the contrast between places, which the sandwich weighs correctly.
3. **Local posterior R95**: RSS ranging is log-normal — towards the places the likelihood is steep,
   away from them flat — so the curvature at the optimum understates the far side. The marginal
   likelihood of §1.3 (P0, n integrated) at the fitted scale, tempered by the sandwich/Laplace
   variance ratio for the correlation, is evaluated on a 41 × 41 grid over ±4 σ of the major axis;
   when the radius about the estimate holding 95 % of it exceeds the ellipse's R95, the ellipse is
   scaled up (shape kept). Beyond ±4 σ lies the far-field degeneracy (P0 and n trade against
   distance), left to the coarse grid, the region test and `ambiguous`.
4. + **correlated fix error**: `(median acc/1.515)² / sessions` on both axes (GNSS error is
   shared within a trip; a session is one device on one day). The per-place errors-in-variables
   term (§1.2) counts the same variance as independent between places; both are kept on purpose:
   the split between common (a smoothed track: mostly the bias) and independent (raw fixes) is not
   known per sample, and either term alone under-covers the other kind of track.
5. ⊕ the **leave-one-place-out jackknife** `(K−1)/K Σ(p₋ₖ − p̄)(p₋ₖ − p̄)ᵀ` (4 ≤ K ≤ 60).
6. ⊕ a **cluster bootstrap** (24 replicates, K ≥ 6, deterministic xorshift64*).
7. × **κ²**, the anchor calibration (§2.3).

`⊕` keeps, along each principal axis of the current matrix, the larger variance of the two. The
jackknife and bootstrap are empirical floors (a maximum, not a sum: no double counting). The
Cramér–Rao bound at σ0 is no longer a floor (at the fitted scale it is the Laplace without the
robust down-weights, never wider than step 2); it remains the geometry figure `crlbR95` (§2.1).

**Where the constants come from** (the user's data, 2026-10-01, and a synthetic study):
* `d_c = 8 m` (was 30 m): residuals of 15 APs' fits, averaged in 3 m cells of the phone's
  smoothed track, correlate 0.27 at 4.5 m, 0.06 at 7.5 m and ≤ 0 beyond (the fit absorbs part of
  the long-range structure, so the true distance is somewhat longer than the 3.5 m this implies).
  A too long `d_c` is not conservative here: it claims neighbouring contrasts are cleaner than
  they are.
* `ρ_in = 0.6` kept: the same cells split the variance 5.2 dB between / 8.6 dB within (ρ ≈ 0.3),
  but the within-cell part includes the observer's body and orientation, which a single pass
  shares; 0.6 does not let a long dwell at one spot count as many independent looks.
* Synthetic study (300 trials per scenario: a building 12–30 × 8–20 m, the AP inside; a loop
  3–12 m out at 1.2 m/s, a scan every 3 s; P0 −38 ± 4, n 2–3, correlated shadowing 4–7 dB with
  `d_c` 4–15 m, fading 4–9 dB, a wall 3–12 dB lossier on each side with p ½, a smoothed-GPS
  Gauss–Markov bias 1.5–4 m with τ 20 min, its accuracy misreported × 0.8–1.25):

  | scenario | median error (m) 2 → 3 | median R95 (m) 2 → 3 | R95 coverage 2 → 3 | mean NEES 2 → 3 |
  |---|---|---|---|---|
  | one loop | 12.2 → 10.5 | 34.3 → 23.1 | 1.00 → 0.95 | 1.04 → 1.74 |
  | two loops | 12.5 → 9.3 | 38.4 → 20.0 | 1.00 → 0.95 | 0.92 → 1.79 |
  | loop + 60–120 m up a hill | 19.7 → 15.5 | 47.8 → 27.6 | 1.00 → 0.96 | 1.38 → 2.31 |
  | loop + hours indoors (fixes 15–30 m) | 16.8 → 10.0 | 70.0 → 46.1 | 1.00 → 1.00 | 0.42 → 0.39 |
  | a 40–80 m street loop | 12.6 → 12.5 | 45.3 → 27.1 | 0.97 → 0.92 | 1.06 → 2.18 |

  Coverage near the nominal 95 % and NEES near 2 (it was over-conservative: R95 three times the
  median error). The error itself is limited by the physics: with 4–7 dB of correlated shadowing
  and lossy walls, 15–35 % of loops still put the AP outside the walked loop (the P0/n/distance
  trade-off), and the median stays near 10 m — building-level, not metre-level.

**Mirror mixture** (estimator 4): when an alternative solution outside the places' hull costs less than
2 nats more than the best (§1.4), either may be the AP. The covariance about the reported one becomes the
mixture's second moment `C + p·d·dᵀ`, `d` the vector to the alternative, `p = 1/(1 + e^Δcost)` its
probability — also when the ghost lies within 2σ (within 2σ is not within R95; only the ghost marker needs
> 2σ). Monte Carlo, places along a road 30 m from the AP: R95 coverage 70 % → 96 %, NEES 1.81 → 0.91.

### 1.6 Ellipse, CEP, R95
Eigen-decomposition → the 1-σ ellipse (`semiMajor`, `semiMinor`, `orient` = bearing of the
major axis). The 1-σ ellipse holds only 39.35 %; the p-ellipse scale is
`k = √(−2 ln(1−p))` (95 % → 2.4477), which is what the maps draw. For any ellipse:

```
P(|X| < r) = ∫_{−π/2}^{π/2} φ(r sin t/σa)/σa · (2Φ(r cos t/σb) − 1) · r cos t dt     (σa ≥ σb, Simpson, 256 steps)
```
`r95` and `cep50` solve `P = 0.95 / 0.5` by bisection; `pWithin25 = P(25 m)`. Circular
checks: CEP = 1.1774σ, R95 = 2.4477σ ([CEP](https://en.wikipedia.org/wiki/Circular_error_probable),
[NovAtel APN-029](https://www.gnss.ca/app_notes/APN-029_GPS_Position_Accuracy_Measures_Application_Note.html)).
`acc` (kept for older clients) is `max(8 m, semiMajor)`.

### 1.7 Where it was not heard (misses)
This host logs the ~15 m cells it scanned from (`scan_cells`). For an AP, every such cell
within 600 m, scanned while the AP was around (first sample − 7 d … last + 30 d) and with no
sample of it within 15 m, is a miss:
`log L += min(count, 3) · log(1 − (1 − 0.2)·Φ((μ(d) − (−92 dBm))/σ0))` with μ(d) the model
level there. A single "heard once" becomes a disc that shrinks away from where we scanned and
heard nothing. ([censored detectors](https://arxiv.org/pdf/1505.04512))

### 1.8 Kind
* **fix**: ≥ 3 places, R95 ≤ 150 m and the CRLB R95 ≤ 150 m.
* **region**: otherwise; centred on the posterior mean, `R95 = max(ellipse R95, R95_hpd)`.
  Nothing is claimed beyond R95 1500 m.
* **mobile**: the locator already knows it travels (travelling / home networks), it was heard
  at places more than 5 km apart ([Ichnaea's rule](https://ichnaea.readthedocs.io/en/latest/algo/observations.html)),
  or it is a fix with ≥ 5 places over > 300 m whose level does not fall with distance
  (Spearman > −0.1).
* **moved**: samples span > 60 days and a fit of the last 30 days and one of the rest are both
  fixes with Mahalanobis² > 13.82 (χ²₂ 99.9 %): only the recent epoch is kept, flag `moved`.

## 2. The metrics

| field | formula | tells |
|---|---|---|
| `r95`, `cep50`, `pWithin25` | §1.6 | precision |
| `cxx cxy cyy` | §1.5 (m², x east, y north) | the full uncertainty |
| `rssDop` | `√tr(M⁻¹)`, `M = Σ W_k (v_k − v̄)(v_k − v̄)ᵀ`, `v_k = (p − q_k)/d_k²` | geometry-only dilution of precision (m) |
| `crlbR95` | R95 of the CRLB with σ0 | the best this geometry allows at the nominal noise (the fix test) |
| `rbar` | `|Σ W_k e^{iθ_k}| / Σ W_k`, θ the bearing AP → place | 0 surrounded, 1 all on one side |
| `maxGapDeg` | largest gap between those bearings | > 180° = extrapolated |
| `inHull` | estimate inside the convex hull of the places | extrapolation |
| `linRatio` | μ2/μ1 of the places' weighted scatter | 0 = a straight road |
| `ambiguous`, `altLat/altLon` | an alternative outside the hull with Δcost < 2 and > 2σ away (the ellipse already includes it, §1.5) | mirror ambiguity |
| `modes` | §1.3 | multimodality |
| `chi2nu` | `Σ W r²/σ² / (K_in − 2)` over inliers | goodness of fit |
| `sigmaDb` | `τ·σ0` | robust residual scale |
| `outlierFrac` | samples in places with EM weight < 0.5 | contamination |
| `ess` | Kish: `(Σω)²/Σω²`, ω = W/σ_shadow² | effective number of places |
| `sessions`, `devices` | distinct device-days, devices | independence |
| `spearman` | rank correlation of level vs log distance | should be < −0.5 |
| `p0RangeCorr` | |corr(P0, radial position)| from `F⁻¹` | ~1 = no close sample, scale unidentified |
| `dminRatio` | closest / median place distance | same |
| `jackMax` | largest leave-one-place-out shift | fragility |
| `driftD2` | `½·prev + ½·Δᵀ(C + C_prev)⁻¹Δ` between refits | stability |
| `nisEwma` | smoothed `innovation²/S` of the incremental updates | live consistency |
| `extD2` | `Δᵀ(C + (max(acc,50)/1.515)²I)⁻¹Δ` against the WiGLE/Apple placement | outside agreement (never fitted) |
| `fadingDb` | mean within-place spread of the level | fast fading |
| `suggest` | §4 | where to sample next |

Flags in the JSON: `extrapolated` (fix outside the hull), `ambiguous` (ambiguous or ≥ 2 modes),
`moved`, `fragile` (jackMax > 2·semiMajor), `rangeScale` (p0RangeCorr > 0.95).

### 2.1 Cramér–Rao bound and RSS-DOP
With P0 unknown the position information is (Schur complement)
`F_pos = (b²/σ²)·Σ (v_k − v̄)(v_k − v̄)ᵀ`: only the spread of the vectors v_k around their mean
informs, so places all on one side or on one line give almost nothing
([Patwari et al.](https://www.researchgate.net/publication/4187109_Cramer-Rao_Bound_Analysis_of_Quantized_RSSI_Based_Localization_in_Wireless_Sensor_Networks),
[unknown transmit power](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC5038730/)). Four places
surrounding an AP at 50 m: RSS-DOP 50 m, σ/b ≈ 0.58, DRMS ≥ 29 m. `crlbR95` is the full
4-parameter bound with the priors at the nominal σ0: a geometry figure for the fix test (§1.8),
not a floor — a fit whose places agree better than σ0 may claim less (§1.5).

### 2.2 Score
Components in [0, 1]:

```
P  precision   = clamp01( ln(300/R95) / ln 30 )                           10 m → 1, 300 m → 0
G  geometry    = [0.6·(1 − rbar) + 0.4·min(1, 4·linRatio)] · (inHull ? 1 : 0.7)
E  evidence    = 1 − exp(−ess/4)
F  fit         = exp(−max(0, chi2nu − 1.5)/2) · (1 − clamp01((outlierFrac − 0.1)/0.4)) · exp(−max(0, nisEwma − 3)/3)
S  stability   = exp(−driftD2/6) · exp(−(jackMax/semiMajor)²/8) · (ambiguous ? 0.4 : 1)
T  freshness   = max(0.3, 0.5^(age of the newest sample / 180 d))
X  external    = max(0.3, exp(−extD2/8))          (left out, weights renormalised, when there is no placement)

score = 100 · exp( Σ w_i ln max(0.001, C_i) / Σ w_i ),   w = P .30 · G .20 · E .15 · F .15 · S .10 · T .05 · X .05
```

A weighted **geometric** mean: one bad component (a mirror, an extrapolation) drags the score
down instead of being averaged away. Caps: ambiguous or ≥ 2 modes → ≤ 49; outside the hull
with `linRatio < 0.02` → ≤ 59. A region scores `min(39, 100·P)`; mobile 0. The half-life of
180 days reflects AP churn (about 8 % of BSSIDs vanish from Apple's database per month,
[Rye & Levin 2024](https://arxiv.org/html/2405.14975v1)).

**Hysteresis.** A letter changes when the score clears the boundary by 3 points, or when the
same new letter comes back on the next refit (`pendingGrade` holds it meanwhile).

### 2.3 Calibration against anchors
Anchored APs (docs/RANGING.md §4) hide their pin: they are fitted from their own samples
(κ = 1) and compared with the survey. For fixes, `NEES = eᵀC⁻¹e` should average 2; with at
least three, `κ² = clamp(mean NEES / 2, 0.25, 25)` scales every covariance. The result
(`kappa`, `meanNees`, `coverage95` = share inside R95, the median error per letter) is in
`GET /api/v1/estimator`, recomputed at start-up and every 6 hours.

## 3. Grouping and devices

### 3.1 One radio, several BSSIDs
BSSIDs that differ only in the locally-administered bit and the last nibble, heard together
in ≥ 5 scans (same device, within 2 s) at a steady level difference (|median| ≤ 10 dB,
1.4826·MAD ≤ 4 dB), and not two good fixes far apart (Mahalanobis² > 6), form a **group**.
The group is fitted once from all members' samples, each shifted by its median difference to
the member with most samples; every member gets the position, its own P0.

### 3.2 Travelling APs
The locator's travelling/home status grades an AP **M** (the old engine silently skipped
them). The estimator adds the 5 km rule and the distance-decay test (§1.8).

### 3.3 Device offsets
For graded fixes (A–D) heard by this host and another device, the per-AP difference
`median(residual_dev) − median(residual_host)`; per device the mean over ≥ 3 APs, shrunk
(`Σ/(n + 0.25)`), accumulated into `g_dev` (clamped ±25 dB). A change > 1 dB refits that
device's APs.

## 4. Where to sample next
For the solution's Fisher information F (nominal σ0, priors), a new sample at q adds
`Δ log det F = log(1 + J_q F⁻¹ J_qᵀ / σ_q²)` (matrix determinant lemma), weighted by the chance
of hearing the AP there, `Φ((μ(d) + 92)/σ0)`. Candidates: 16 bearings × 30/60/90 m around the
estimate; the best (gain > 0.01 nats) is `suggest` for regions and fixes with R95 > 25 m. The
ten best within 2 km of the fix are listed in `GET /api/v1/estimator` (`suggestions`).
([D-optimal geometry](https://citeseerx.ist.psu.edu/document?repid=rep1&type=pdf&doi=8691abf79837b22879512e5365f54d89681ad735))

## 5. Between refits
One new sample nudges the fit with a 2-D Kalman step: `H = uᵀ` (unit vector observer → fit),
`R = σ_d² + a²` with `σ_d = d·ln10·σ0/(10n)`, `S = uᵀPu + R`, `K = Pu/S`, `P ← P − (Pu)(Pu)ᵀ/S`.
`NIS = innovation²/S`; above 9 the sample is not applied. The position, R95, P, F and T are
updated and re-graded (with hysteresis); the full refit follows within seconds.

## 6. Where the grades live
* **Database** (`estimates`): `kind`, `grade`, `score`, `r95`, `cep50`, `p_within25`, `cxx/cxy/cyy`,
  `metrics` (every field as JSON), `version`; `estimate_history` keeps the last 20 per AP;
  `scan_cells` the misses (docs/DATABASE.md).
* **API / D-Bus**: the AP JSON's `grade`, `score`, `r95` and `fit` object (docs/API.md
  "Graded estimates"); `GET /api/v1/estimator`, `EstimatorJson()`, `beaconfix --estimator`.
* **Android**: Room v5 adds the same fields to each AP and an `estimate_history` table; the
  phone grades its own fits with the Kotlin twin and shows the desktop's when synced.

Estimates from an older engine, or imported ones, are recomputed on the first start (in
150 ms batches); none are deleted.

## 7. Golden vectors
`tests/estimator_golden.cpp --write` generates `tests/fixtures/estimator_golden.json` from
synthetic scenarios (ring, noisy ring, three places, road, one sample, two samples + misses,
one spot, coarse fixes, outliers, two devices, RTT, moved, mobile, stale + external clash,
weights/κ/priors, previous fit), an incremental-update stream, two self-location cases and
the probability helpers. `ctest` (`-DBEACONFIX_TESTS=ON`) and the Android
`EstimatorGoldenTest` both check them: doubles to 1e-6 relative (positions 1e-9°), grades,
kinds and flags exactly. Change both engines together, then regenerate.
