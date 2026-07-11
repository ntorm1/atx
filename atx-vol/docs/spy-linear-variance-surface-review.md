# SPY Linear-Variance Surface Fitting Review

**Review date:** 2026-07-11  
**Market snapshot:** `SPY`, Databento OPRA CBBO-1m, `2026-06-05T19:55:00Z`  
**Reviewed path:** `OpraVolSurface -> OptionChain -> PricerFitter(Hft) -> VolaSession -> LinearVarianceCurve -> atx-ui`  
**Scope:** fit shape, term structure, carry/forward estimation, American price-to-IV inversion, diagnostics, and tests. This is a code review and diagnostic analysis; it does not change the fitting implementation.

## Executive conclusion

The unusual term line and short-expiry smile kinks are real properties of the served HFT surface, not plotting defects.

The main cause is the model contract: the HFT path is a **direct, unsmoothed interpolation of up to 48 de-Americanized market total-variance nodes per expiry**. It has no butterfly constraint, no slope regularization, no calendar floor, no calendar repair, and no parity scoring. This is a useful low-latency *mark interpolant*, but it is not currently a suitable smooth risk/theoretical surface. Its excellent 99.8% price-in-band score is largely expected because it is designed to pass through selected market observations.

The core American price-to-IV solver is high quality. On the reviewed production nodes, fast cold inversion differed from accurate cold inversion by only 0.044 volatility basis points at p95 and 0.132 basis points at maximum. The HFT early-exercise shortcut is less exact: it affected 524 of 1,680 selected nodes and produced a p95 accurate repricing residual of 0.955 cents, with a 13.35-cent worst case. That is worth tightening, but it is not large enough to explain the broad jaggedness.

The surface should be described and exposed as two products:

1. a low-latency, market-following interpolation surface for marking/quoting; and
2. a constrained, smooth, calendar-aware surface for Greeks, scenarios, relative value, and term-structure display.

Using the current HFT interpolant as both products is the central quality problem.

## Severity-ranked findings

### High — HFT LinearVariance is interpolation, not a regularized fit

The UI explicitly pins `FitPreset::Hft` and `VolCurveKind::LinearVariance` in `atx-ui/src/vol/spy_opra_surface.cpp:85-86`. The preset then sets a 48-observation cap and disables parity scoring and calendar enforcement in `src/session.cpp:240-252`.

`fit_slice_curve` does not optimize a loss for LinearVariance. It sorts observations, copies every retained `k` and `w_mkt` into nodes, and linearly connects them (`src/vol_curve.cpp:220-275`). Observation weights are only used to resolve an exact duplicate `k`; they do not smooth or regularize the curve. `LinearVarianceCurve::w` linearly interpolates total variance and clamps both wings flat (`src/vol_curve.cpp:81-99`).

Consequences:

- every retained quote perturbation becomes a model feature;
- first derivatives jump at every node by construction;
- flat variance wings introduce additional derivative discontinuities;
- no constraint ensures convex call prices or positive risk-neutral density;
- 99-100% in-band quality is not evidence of smoothness or generalization.

The full SPY board itself is not locally arbitrage-clean: `spy_bidask_bench` reported 1,296 of 5,235 interior liquid observations (24.8%) as butterfly-violating. A direct interpolator necessarily imports a material part of that microstructure noise.

On a fine `k in [-0.35, 0.35]` diagnostic grid, the served HFT surface produced:

- 1,500 negative discrete call-convexity steps across all expiries;
- 1,200 of those in expiries shorter than 30 days;
- 1,188 large local IV-slope changes (threshold `0.05 sigma / unit k`);
- maximum adjacent IV-slope change of `5.117 sigma / unit k` on the front expiry.

These are grid-step counts, not 1,500 independent arbitrage trades, but they conclusively show that the short-end kinks are structural and economically relevant for density/Greek calculations.

Increasing the node count is not a repair. An accurate, uncapped LinearVariance fit raised the same diagnostic from 1,500 to 4,075 call-convexity failures because it reproduced more of the raw board noise. The missing ingredient is shape control, not more observations.

### High — The jagged term structure is expected under independent expiry fits

HFT explicitly sets:

- `enforce_calendar_floor = false`;
- `calendar_repair = None`;
- `score_parity = false`.

Each expiry is therefore fit independently. The UI then plots every serial, weekly, monthly, and quarterly expiry on one ATM-IV line (`atx-ui/src/vol/vol_workspace.cpp:416+`). It does not distinguish standard maturities, event maturities, or confidence/liquidity buckets.

Measured HFT ATM changes included:

| Transition | ATM IV before | ATM IV after | Change |
|---|---:|---:|---:|
| 2026-06-08 -> 2026-06-09 | 15.901% | 16.942% | +1.041 vol points |
| 2026-06-12 -> 2026-06-15 | 18.322% | 16.478% | -1.844 vol points |
| 2026-06-18 -> 2026-06-26 | 17.596% | 16.462% | -1.134 vol points |

Three adjacent transitions exceeded one volatility point; the maximum was 1.844 points.

This does **not** mean that ATM total variance is calendar-arbitrage violating. The reviewed ATM sequence had zero negative adjacent forward-variance intervals. Implied volatility may decline with maturity while total variance continues to increase. However, the surface diagnostic found **226 calendar violations** on its broader `k` grid, so the surface is not calendar-safe away from ATM.

The accurate, uncapped LinearVariance build also had 226 calendar violations and three greater-than-one-point ATM jumps. This is strong evidence that the term behavior is not caused primarily by fast IV inversion. It comes from the expiry-specific market/carry inputs plus independent direct interpolation.

The Robust eSSVI alternative removed the sampled butterfly violations in the review grid, but it still reported 89 pre-existing calendar violations and was not marked calendar-arbitrage-free on this snapshot. It also retained three greater-than-one-point ATM jumps. Robust is substantially smoother, but it is not a complete calendar repair for this board as currently configured.

### High — A surface with known shape failures is served without a prominent quality state

`VolaSession` measures calendar violations for curve-override surfaces in `src/session.cpp:357-369`, but HFT intentionally continues serving the surface. The UI fit inspector reports “CALENDAR FLOOR OFF / HFT” and emphasizes price-in-band/RMSE; it does not make 226 calendar violations or butterfly-unsafety a first-class warning.

This is risky because the same curve feeds:

- theoretical prices;
- delta/gamma/vega/theta;
- strike-from-delta and scenario calculations;
- term/skew relative-value views.

An interpolating mark curve may be acceptable for quote anchoring while being unacceptable for second derivatives and scenario risk. The UI and API do not currently communicate that distinction.

### Medium — The HFT carry solve deliberately uses the least robust mode

The general de-Americanization code documents why one near-ATM call/put pair is quote-noise fragile and why up to 12 pairs are normally averaged (`src/deamer.cpp:226-264`). The HFT preset overrides that policy with `n_atm = 1` and `max_borrow_pairs = 1` (`src/session.cpp:243-244`).

Against the accurate multi-pair carry configuration, HFT forward differences were small at the short end (generally below $0.05) but reached $0.74 at the longest expiry. The corresponding HFT-versus-accurate LinearVariance ATM levels remained close, so this is not the main cause of the observed front-term jumps. It remains a fragile cross-snapshot choice: one noisy co-terminal pair can shift the forward, move every strike's `k = log(K/F)`, change the OTM-side selection boundary, and create an apparent smile step.

The latency rationale is real, but the UI cold fit is already approximately 80-300 ms across the measured corpus. A robust weighted/trimmed carry solve should be evaluated as a separate latency-quality trade rather than silently inheriting the single-pair mode.

### Medium — The core IV root solve is strong; the HFT shortcut creates the meaningful tail errors

The American IV implementation has appropriate no-arbitrage bounds, a safeguarded Newton/bisection root, warm boundary reuse, and a final cold-pricer polish (`src/american_iv.cpp:86-351`). The standalone 544-contract known-truth benchmark reported:

| Inversion mode | Maximum sigma error | RMS sigma error |
|---|---:|---:|
| Accurate cold | `8.00e-10` | `1.46e-10` |
| Fast-options cold | `4.69e-06` | `1.79e-06` |

On the 1,680 actual HFT fit nodes, using the same carry:

| Comparison | p50 | p95 | Maximum |
|---|---:|---:|---:|
| Full fast inversion vs accurate | 0.0045 vol bp | 0.0440 vol bp | 0.1319 vol bp |
| Production HFT node vs accurate | 0.0229 vol bp | 2.7508 vol bp | 14.5306 vol bp |

The difference between those rows is mostly the OTM shortcut in `src/calib.cpp:267-291`. The shortcut reuses the raw European IV when a BAW-estimated early-exercise premium is no more than half the spread. It changed 524 of 1,680 selected nodes (31.2%). Repricing production HFT sigmas through the accurate American pricer gave:

| Accurate repricing residual | p50 | p95 | Maximum |
|---|---:|---:|---:|
| Cents | 0.004 | 0.955 | 13.350 |
| Fraction of half-spread | 0.002 | 0.238 | 1.059 |

Most shortcut errors are economically small, but the worst case is outside its quote half-spread. The shortcut should therefore be treated as a bounded approximation with explicit quality telemetry, not as equivalent to inversion.

The UI's selected-slice bid/ask IV bands use a separate accurate cold inversion call (`atx-ui/src/vol/spy_opra_surface.cpp:216+`), while the fit nodes use the HFT fast/shortcut policy. That makes the displayed market bands higher fidelity than part of the data used to construct the curve, which is defensible but should be documented.

### Medium — The 48-node cap can preserve microstructure curvature as model curvature

Before American inversion, `cap_observations_for_deam` reduces each dense expiry to 48 observations (`src/calib.cpp:198-265`, invoked at line 350). It recursively retains the point with the largest noise-normalized miss from linear total-variance interpolation.

This is a sensible compression algorithm for preserving a market polyline. It is not a denoiser. When the market contains isolated non-convex prints or side-switch discontinuities, the algorithm may deliberately spend nodes preserving them. All 35 reviewed HFT slices used exactly 48 nodes, for 1,680 total.

The fit-matrix corpus confirms the intended behavior:

- HFT cap 48: worst clean price-in-band 98.61%, roughly 71-327 ms;
- cap 128: worst 99.31%, roughly 133-508 ms;
- uncapped HFT: worst 99.22%, roughly 273-700 ms;
- full accurate LinearVariance: 100% in-band, roughly 483-1,031 ms.

These results measure reproduction. They do not show improved surface quality as node count increases.

### Medium — Existing acceptance tests reward reproduction and do not gate shape

The HFT SPY regression requires at least 98% clean price-in-band (`tests/spy_bidask_regression_test.cpp:124`). The LinearVariance unit tests validate interpolation/clamping and optional calendar-floor behavior. No HFT regression test asserts:

- non-negative call-price convexity / density;
- bounded first-derivative jumps;
- calendar monotonicity on the served HFT configuration;
- stable ATM term changes across adjacent expiries;
- production-shortcut repricing error by tenor and moneyness;
- carry robustness to perturbing the selected ATM pair.

The current tests correctly prove the documented interpolation and inversion mechanics, but the headline price-in-band gate can improve when the economic surface becomes less suitable for risk.

## Pipeline walkthrough

1. `OpraVolSurface` loads the OPRA snapshot and builds an `OptionChain`.
2. The UI pins HFT LinearVariance rather than using a risk-oriented selector.
3. Per expiry, the HFT carry solver uses one near-spot call/put pair to infer borrow and forward.
4. `build_observations` filters flags/spreads, keeps one preferred OTM leg per strike, and creates a Black-76 IV seed.
5. The adaptive cap retains at most 48 observations.
6. `build_observations_european` either performs fast cold American inversion or takes the half-spread early-exercise shortcut.
7. LinearVariance copies the retained `(k, sigma^2 T)` values directly into its nodes.
8. Each expiry is independent because HFT disables the calendar floor.
9. Between expiries, `CurveSurface` linearly interpolates total variance. Within a slice it linearly interpolates total variance between market nodes.
10. The UI samples this surface densely, so piecewise-node slope changes become visible as smile kinks and every listed expiry becomes a point on the term line.

## Quality assessment

| Dimension | Assessment | Rationale |
|---|---|---|
| Selected-node price fidelity | Excellent | 99.78% clean in-band on the reviewed HFT regression; direct interpolation makes this expected. |
| Held-out price generalization | Good for constrained dense model | Existing convex-QP leave-every-other-strike test is 98.59% OOS, but this does not directly validate HFT LinearVariance shape. |
| Core price-to-IV root solve | Excellent | Accurate and fast cold solvers have tight known-truth and production-node errors. |
| HFT de-Am shortcut | Acceptable median, weak tail | p95 below one cent, but maximum 13.35 cents / 1.06 half-spreads. |
| Short-expiry smile smoothness | Poor | Large node slope changes and concentrated convexity failures. |
| Butterfly/no-density-arbitrage quality | Poor | No constraint; 1,500 sampled negative-convexity steps, 1,200 under 30 days. |
| Calendar quality | Poor | 226 sampled calendar violations and enforcement disabled. |
| ATM term-line interpretability | Mixed | Total variance remained monotone at ATM, but serial expiry IV jumps are visually large and confidence/event state is absent. |
| Suitability for quote anchoring | Good with warnings | Closely tracks selected marks and is fast. |
| Suitability for Greeks/risk/scenarios | Not production-ready | Kinks and convexity failures contaminate gamma/density and scenario stability. |

## Recommendations

### P0 — Separate mark and risk surfaces

- Keep LinearVariance as an explicitly named **market interpolant** if the desk needs near-exact quote anchoring.
- Serve a constrained `ConvexDense` or smooth parametric surface for Greeks, scenario risk, strike-from-delta, density, and term/skew analytics.
- Record both model identifiers and make panel consumers choose intentionally.
- Do not label the interpolant merely “SERVED MODEL” without a shape-quality state.

### P0 — Make quality failures visible and enforce consumer-specific admission

- Display calendar violation count, butterfly/convexity state, stale/wide quote counts, carry confidence, and whether shortcuts were used.
- Add an amber/red surface-health badge to the symbol and term panels.
- Block or fall back for risk consumers when convexity/calendar gates fail; quote-mark consumers may continue with an explicit degraded state.

### P1 — Add shape constraints rather than increasing node count

- Fit in call-price space with monotonicity and convexity constraints, or project LinearVariance marks onto an arbitrage-safe price curve before deriving IV.
- If total-variance interpolation remains, add a smoothing/regularization penalty and a bound on adjacent slope changes.
- Treat the 48-node RDP result as candidate knots, not final values that must be passed through exactly.
- Avoid flat-variance wing clamps for risk; use controlled asymptotic slopes or a parametric wing splice.

### P1 — Revisit cross-expiry construction

- Enforce a calendar floor over a shared `k` grid or fit total variance jointly across expiry.
- Separate standard expiries from event/serial expiries in the term view.
- Plot total variance and forward variance alongside ATM IV so an IV decline is not mistaken for calendar arbitrage.
- Show confidence weights or liquidity for each term point.
- Investigate why Robust still reports 89 calendar violations on this board before presenting it as calendar-safe.

### P1 — Harden carry and inversion policy

- Use a robust weighted or trimmed aggregation over several near-ATM pairs; report dispersion across pair-implied forwards.
- Retain single-pair mode only when a latency SLA requires it and the pair passes freshness/spread/parity checks.
- Disable the OTM shortcut for ultra-short or low-vega nodes, or require an accurate repricing residual below a configured fraction of half-spread.
- Emit counts and residual quantiles for shortcut use per fit.

### P1 — Add shape and inversion acceptance tests

For fixed real SPY corpus snapshots, gate:

- zero negative call-convexity steps on the risk surface over the tradeable `k` band;
- zero calendar violations over the admitted risk band;
- maximum/p95 IV-slope jump by tenor bucket;
- p95 accurate repricing residual below 0.25 half-spreads and maximum below one half-spread;
- term-forward sensitivity to removing or perturbing one ATM carry pair;
- quality separately for `<7d`, `7-30d`, `1-6m`, and `>6m` buckets;
- mark-surface price fidelity separately from risk-surface smoothness.

## Reproduction evidence

Commands used against the current worktree:

```powershell
# Production HFT acceptance
.\build\bin\atx-vol-tests.exe `
  --gtest_filter=SpyBidAskRegression.PricerFitterHftColdStartInBand:LinearVarianceCurve.*

# Independent fit and raw-board diagnostics
.\build-rel\bin\spy_diag.exe
.\build-rel\bin\spy_bidask_bench.exe
.\build-rel\bin\spy_oos_check.exe

# Known-truth price-to-IV accuracy
.\build-rel\bin\american_iv_bench.exe

# HFT cap/shortcut matrix
.\build-rel\bin\spy_fit_matrix_bench.exe hft 1
.\build-rel\bin\spy_fit_matrix_bench.exe hft-nocap 1
.\build-rel\bin\spy_fit_matrix_bench.exe linear-hft-cap128 1
.\build-rel\bin\spy_fit_matrix_bench.exe linear-full 1
```

A temporary review probe also compared HFT, accurate uncapped LinearVariance, and Robust sessions on the same loaded board; it was removed after collecting the figures so no diagnostic-only production target remains.

## Bottom line

The observed UI behavior is consistent with the implementation. The term line is exposing independently fit serial-expiry marks, and the short-smile kinks are the expected output of a high-degree piecewise-linear market interpolant applied to a noisy, locally non-convex board.

The price-to-IV solver is fundamentally sound. Improvements should focus first on product separation, shape constraints, calendar construction, carry robustness, and honest surface-health diagnostics—not on replacing the root finder.


