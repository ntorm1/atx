# atx-vol Core Fitting / Calibration Numerics — Code Review (01-fit-core)

Reviewer: senior quant C++ (read-only audit). Date: 2026-07-19.
Scope: `calib.{cpp,hpp}`, `svi_calib.cpp`, `essvi_calib.cpp`, `cstar_calib.cpp`,
`c8_calib.cpp`, `curve_fit.cpp`, `curve.cpp`, `spline_curve.cpp`,
`curve_selector.cpp`, `fit_metrics.cpp`, `fit_policy.cpp`, and the shared detail
headers (`robust.hpp`, `resid_basis.hpp`, `calib_shared.hpp`). Wiring proven with
`Grep` across `src/ include/ tests/ bench/ examples/` (python/build excluded).

## Verdict

The calibration numerics are unusually careful and heavily reviewed. Analytic
Jacobians (SVI-MM 5-col, eSSVI cube→natural, C8 JW→x, CStar modal) are all
**correct** (verified by hand). The quasi-explicit SVI box provably guarantees
non-negative variance; LM damping schedules, Illinois regula-falsi de-Am solver,
and the shared-boundary certification are sound. **No Critical correctness bug
was found.** The material findings are (a) a large dead/unwired calibrator family
(CStar), (b) a no-arb gate that lives only at the serving seam and is bypassed by
the parametric-SVI surface driver, (c) several units/robustness inconsistencies
between calibrators, and (d) SOTA feature gaps that are documented-but-unshipped.

## Top 5 highest-leverage fixes

1. **[High] SVI surface driver serves butterfly-arbitrage smiles.**
   `svi_calib_surface` (svi_calib.cpp:1417-1420) only *tallies* Mingone butterfly
   violations; it never projects/rejects. Unlike the `fit_slice_curve(Svi)`
   serving seam (vol_curve.cpp:404-413) which projects+refuses. `calib_pool`
   dispatches `Parametrization::Svi` here directly (calib_pool.cpp:181). Fix:
   call `svi_project_mm` + re-check (or reject) inside the driver, not only at the
   downstream seam.
2. **[Medium] Entire CStar (C16M) calibrator is dead in production.**
   `cstar_calibrate_slice` / `cstar_seed_from_essvi` / `cstar_lm_inner_block_w`
   (cstar_calib.cpp) have no production caller — no `VolCurveKind::CStar`, and
   `param_supported` rejects `CStar16M` (calib_pool.cpp:160-163, 186-189). Only
   tests + `examples/cstar_panel.cpp`. Either wire a surface driver or move it out
   of the shipped library surface.
3. **[Medium] Interval (bid/ask-band) loss is NotImplemented** (calib.cpp:862;
   `CalibLossKind::Interval`). This is the objective that directly maximizes the
   project's headline metric (% within bid-ask); every fitter currently minimizes
   squared distance to a point anchor. Highest-leverage *feature* gap.
4. **[Medium] C8 reports a total-variance RMSE in a vol-space diagnostic field**
   (c8_calib.cpp:437: `diag.rmse_vol_vega_weighted = params.rmse_price`, where
   `rmse_price` is `sqrt(Σ((w_model−w_mkt)/sd)²/n)` in w-units). Surface-level RMSE
   aggregation mixes units across calibrators. (Impact bounded: `c8_calib_slice`
   is test-only; served C8 recomputes in vol_curve.cpp.)
5. **[Medium] Quasi-explicit SVI Nelder-Mead is capped at `max_inner_iter`
   (default 12), not the C's 200** (svi_calib.cpp:787-788, 812). The `:200`
   fallback is unreachable at defaults, so the 2-D (m,σ) simplex gets ~12 moves
   per outer pass — a truncated search on skewed/wide smiles (mitigated only by
   IRLS re-seeding across outer passes).

---

## Critical

None found in the reviewed files.

---

## High

### H1. `svi_calib_surface` does not enforce butterfly no-arb before serving
- **File:** svi_calib.cpp:1417-1420 (and the SVI-MM twin at 1499-1502).
- **What:** The driver computes `arb_check_butterfly_svi_mm(slice,T).n_violations`
  and only accumulates it into a diagnostic counter; the raw slice is written via
  `surface.set_slice_svi` regardless. The in-code comment ("no rejection here — the
  serving-seam gate does that") assumes every consumer routes through
  `fit_slice_curve(VolCurveKind::Svi)` (vol_curve.cpp:404-413), which *does*
  `svi_project_mm`+re-check+refuse. But `calib_pool::dispatch_calib`
  (calib_pool.cpp:181) produces a `Parametrization::Svi` `VolSurface` directly from
  this driver.
- **Why it matters:** The quasi-explicit fit is only guaranteed w≥0 (Lee/rho box),
  *not* butterfly-admissible. If a `Parametrization::Svi` surface built via the pool
  is queried/served without passing through `fit_slice_curve`, static-arbitrage
  smiles reach pricing/risk. This contradicts the "most robust" goal.
- **Severity note:** Rated High conditional on the direct-serve path existing;
  eSSVI (the primary path) is butterfly-free by construction, so blast radius is the
  SVI/SVI-MM pool parametrizations. **Verify** whether a `Parametrization::Svi`
  VolSurface is ever served without the `fit_slice_curve` seam.
- **Fix direction:** Move the `svi_project_mm` + re-check/reject logic from
  vol_curve.cpp into `svi_calib_surface`/`svi_mm_calib_surface` (skip-the-slice on
  residual violation), so the surface is admissible independent of the serving path.

---

## Medium

### M1. CStar (C16M) calibrator is unreachable in production (dead calibrator)
- **File:** cstar_calib.cpp (all three public entry points); gated out at
  calib_pool.cpp:160-163 & 186-189; no `VolCurveKind::CStar` (vol_curve.hpp:73-89).
- **What:** `cstar_calibrate_slice`, `cstar_seed_from_essvi`,
  `cstar_lm_inner_block_w` are called only from `tests/cstar_calib_test.cpp` and
  `examples/cstar_panel.cpp`. The header itself documents that the surface
  orchestration was deferred (no in-tree eSSVI seed-surface source at port time),
  but the seed surface now exists (`essvi_calib_surface`).
- **Why it matters:** ~67 KB of calibrator+model code (cstar_calib.cpp +
  cstar.cpp) ships with no reachable path; C16 is the highest-DoF family and would
  be the accuracy ceiling for a SOTA fitter. Dead weight + missed capability.
- **Fix direction:** Add a `cstar_calib_surface` driver looping
  `cstar_calibrate_slice` over the eSSVI seed surface, plus a `VolCurveKind`/
  selector entry; or explicitly quarantine it out of the shipped target.

### M2. Parametric interval/band loss not implemented
- **File:** calib.cpp:859-867 (`CalibLossKind::Interval` → `NotImplemented`);
  enum at calib.hpp:86-89.
- **What:** Only `Mid` (squared distance to a point anchor) is executable.
  `CalibAnchorKind::{Bid,Ask}` are honored (calib.cpp:143-148) but still fit a
  *squared* penalty to that one-sided anchor — not a zero-inside-band interval loss.
- **Why it matters:** The stated goal metric is % within bid-ask. A band loss
  (zero residual inside [bid,ask], quadratic outside) is the direct objective for
  that metric; its absence forces the fitters to over-tighten to mids and pay
  accuracy where the band is wide.
- **Fix direction:** Implement the interval residual in the shared obs loss + the
  SVI-MM/eSSVI price/w residual paths; it is already a persisted vocabulary.

### M3. C8 fit-diagnostic units inconsistency across calibrators
- **File:** c8_calib.cpp:424-425, 437 (`params.rmse_price = sqrt(fit_sse/n)` where
  `fit_sse` is Σ((w_model−w_mkt)/sd)² in **total-variance** units; then
  `diag.rmse_vol_vega_weighted = params.rmse_price`).
- **What:** SVI (`svi_calib.cpp:892`) and eSSVI (`essvi_calib.cpp:930-936`) fill
  `rmse_vol_vega_weighted` with a **σ-space** RMSE (`sqrt(w/T)−σ_mkt`). C8 fills the
  same field with a **w-space** RMSE. `stamp_surface`/`calib_surface_impl` aggregate
  this field into a surface-level `rmse_vol`.
- **Why it matters:** Any surface mixing C8 and non-C8 slices (or comparing C8 to
  eSSVI on the same field) mixes units; the number is not a vol RMSE. Bounded
  because `c8_calib_slice` is test-only and the served C8 path (vol_curve.cpp)
  computes its own metric — but the field contract is violated.
- **Fix direction:** Report a σ-space RMSE from C8 too (convert w→σ per obs), or
  rename the field to a domain-agnostic weighted-SSE-RMSE.

### M4. Quasi-explicit SVI Nelder-Mead iteration cap collapses to 12
- **File:** svi_calib.cpp:786-789 (`max_inner = (opts.max_inner_iter>0) ?
  opts.max_inner_iter : 200`), passed as the NM `max_iter` at 812.
- **What:** `max_inner_iter` defaults to 12 (calib.hpp:145), which is a sensible
  *LM* inner count but a very small *2-D simplex* budget; the `:200` C-parity
  fallback only triggers if a caller explicitly sets `max_inner_iter=0`.
- **Why it matters:** On wide/skewed smiles the (m,σ) outer search may stop far
  from optimum, degrading the raw-SVI fit that seeds SVI-MM and the served SVI. It
  is partly rescued by IRLS re-seeding (~`outer_cap` restarts), so this is quality,
  not a crash.
- **Fix direction:** Give the quasi-explicit NM its own cap (e.g. keep 200), or
  document that `max_inner_iter` intentionally double-duties as the NM budget.

### M5. SVI IRLS uses a non-robust (weighted-RMS) scale for Huber reweighting
- **File:** svi_calib.cpp:837 (`sigma_resid = sqrt(sumwr2/sumw)`) and 852
  (`r_norm = |resid|/sigma_resid`).
- **What:** The quasi-explicit outer IRLS normalizes residuals by a *weighted RMS*
  scale, unlike the eSSVI/C8/CStar reweighters which anchor on the q90 order
  statistic (`detail::huber_weights_strided`) and the SVI-MM path which uses a q90
  of half-spread-normalized residuals (svi_calib.cpp:1096-1107).
- **Why it matters:** RMS scale is itself inflated by the very outliers Huber is
  meant to down-weight, so a few bad quotes weaken the whole slice's rejection —
  the opposite of robust. (Documented divergence in calib_shared.hpp, but it is a
  robustness weakness for the SOTA goal.)
- **Fix direction:** Anchor the quasi-explicit IRLS on a median/q90 scale (reuse
  `quantile_sorted_lower` / `huber_weights_strided`).

---

## Low

### L1. Dead exported helpers (test-only)
- `avg_abs_error_e5` (fit_metrics.cpp:70): referenced only by
  `tests/fit_metrics_test.cpp` — no production caller (parity.cpp uses
  `reduced_chi_square`/`minimum_edge`/`band_violation_stats`, not this).
- `svi_raw_to_jw` / `svi_jw_to_raw` / `SviJwParams` (svi_calib.cpp:1180-1278):
  test-only.
- `c8_calib_slice` (c8_calib.cpp:361): test-only; served C8 uses `c8_fit_slice_lm`
  (vol_curve.cpp:522). Its IRLS also uses raw `opts.max_outer_iter` (c8_calib.cpp:399)
  instead of `detail::outer_cap(opts)`, so it ignores `optimization_level` — a minor
  inconsistency with SVI/eSSVI, invisible while test-only.
- **Fix direction:** Keep if genuinely reserved API, else prune; wire `c8_calib_slice`
  through `detail::outer_cap` for consistency if it becomes live.

### L2. C8 inner LM grows λ without the giveup break on a failed SPD solve
- **File:** c8_calib.cpp:223-226 (`if (!dx) { lambda *= 4.0; continue; }`).
- **What:** The rejected-step branch has `if (lambda>1e8) break;` (249-251); the
  solve-failure branch does not. Bounded by `max_inner_iters` so not a hang, but λ
  can run away to overflow across iterations with no progress.
- **Fix direction:** Add the same `>1e8` giveup on the `!dx` path.

### L3. `noise_sigma = 1.0` magic fallback on near-zero vega
- **File:** calib.cpp:198 and 1212 (`o.noise_sigma = (vega>kVegaFloor) ? spread/vega : 1.0`).
- **What:** A 1.0 price-unit "σ-equivalent" is arbitrary and enormous; it feeds
  C8's `sd_w` (c8_calib.cpp:377) and `cap_observations_for_deam` (calib.cpp:258).
  Rarely reached (such rows are usually dropped by the LowVegaWeight filter), so
  impact is small, but the constant is a silent placeholder.
- **Fix direction:** Derive from the price mid or drop the row explicitly.

### L4. eSSVI `n_butterfly_viol` hardcoded to 0 even with the dense-C2 residual layer
- **File:** essvi_calib.cpp:1235-1237.
- **What:** True for backbone-only (by construction), and the dense residual layer
  best-effort-projects to arb-free (leaving backbone-only on failure, essvi_calib.cpp:583-611),
  so the *served* surface is safe — but the diagnostic can't reflect a residual-layer
  near-violation. Reporting blind spot only.

---

## Performance notes

- **P1** (calib.cpp:178-179): `weight_w = min(obs_weight_w(...), max_weight)` (two
  divisions) is computed before the `weight_sigma < min_vega_weight` reject test —
  wasted for rejected rows. Reorder the cheap scalar compare first.
- **P2** (svi_calib.cpp:551, 1068, 1146; cstar_calib.cpp price path): price-domain
  LM inner/backtracking loops call `black76_price`/`black76_value_and_vega` (erf/exp)
  per strike per trial (`kLmTrialCap×6` price evals/step). Inherent to a price-domain
  objective, but this is the hottest cold-math site; a vega/price cache keyed on the
  batched w_pred could hoist part of it.
- **P3** (curve_selector.cpp:559-561): OOS scoring calls cold `american_price`
  (Andersen-Lake) per holdout row per candidate per expiry — O(candidates×expiries×
  holdout) cold American solves. Caches are threaded, but this dominates selector
  latency on dense boards.
- **P4** (essvi_calib.cpp / svi_calib.cpp): batched `simd::*_batch` kernels are used
  for w and w-grad (good); the residual/Jacobian *accumulation* stays scalar in
  source order (intentional, for bit-reproducibility) — acceptable.

## SOTA feature gaps (beyond M2/M3)

- **Joint surface eSSVI:** only per-slice fit + post-hoc calendar projection;
  `EssviRhoMode::Shared`/`TermStructure` are `NotImplemented` (calib.cpp:868-877).
  A globally-coupled SSVI/eSSVI term-structure fit is the SOTA reference.
- **Asymmetric eSSVI ρ:** `NotImplemented` (calib.cpp:878-881) — leaves put/call
  wing asymmetry to the additive residual layer only.
- **Outlier handling:** soft Huber IRLS only; no hard trimming / RANSAC / one-sided
  crossed-quote rejection beyond the flag filter. Fine for clean boards, thin on
  penny-noise wings (the dense-C2 comment even notes ~1/4 of a raw SPY board is
  locally arb-violating).
- **Wing extrapolation control:** flat-wing spline / model-implied SVI-eSSVI wings;
  no configurable wing slope/curvature clamp exposed to the caller.
- **Warm-start coverage:** eSSVI has warm-start + Tikhonov prior (essvi_calib.cpp:138-168,
  777-785); SVI/CStar have none (SVI-MM re-seeds cold from quasi-explicit each call).

## Positively verified (no defect)

- SVI-MM analytic price Jacobian `svi_w_grad_at` (svi_calib.cpp:504-514) — all 5
  partials correct.
- eSSVI cube→natural Jacobian `cube_grad_from` (essvi_calib.cpp:220-227) — chain rule
  through θ(ψ), φ(θ,ρ,p), ρ(λ) correct incl. the φ_hi branch switch at s=4.
- Quasi-explicit (u,v) rotation + back-map (svi_calib.cpp:824-867) — algebra exact;
  the 0≤a, 0≤d_uv,c_uv box provably yields w_min = a+bσ√(1−ρ²) ≥ 0.
- Illinois regula-falsi de-Am bracket (`SharedLaneBracket`, calib.cpp:716-825) —
  invariant preserved, deflation sign-safe, terminates within max_iter.
- Natural cubic spline Thomas solve + penalized WLS (spline_curve.cpp:35-72, 470-543)
  — correct, flat-wing clamped, monotone calendar lift terminates.
- `CurveSet::set_yield` (curve.cpp:228-232) — `ATX_TRY(yield,…)` expands to an
  *assignment* to the member (not a shadowing local); the member is updated (test
  CurveSet.SetYield_ConstructsUsableYieldCurve confirms). NOT a bug.
