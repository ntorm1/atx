# atx-vol fitting/calibration pipeline code review

**Review target:** repository HEAD `01d88b8` (2026-07-11)  
**Scope:** fitting and calibration, from `QuoteFrame`/`OptionChain` ingestion through curve selection, de-Americanization, per-slice optimization, cross-expiry construction, diagnostics, incremental refit, corpus construction, and fitted-surface consumption. Pricing/Greeks kernels are reviewed only where the fitter calls them.  
**Method:** static end-to-end call-path tracing plus focused tests. The repository asks reviewers to prefer `codebase-memory-mcp`; no graph tools were exposed in this agent session, so this review used symbol-oriented `rg`, targeted source reads, call-site inspection, and executable tests.  
**Production edits:** none.

## Executive assessment

The codebase has a strong set of fitting primitives: consistent ownership, deterministic per-expiry fan-out, a genuinely wired SIMD eSSVI residual/Jacobian kernel, warm-start support, multiple curve families, a constrained dense QP, explicit carry resolution, and substantial synthetic/real-board tests. The current production facade is nevertheless not one coherent fitting pipeline. It dispatches eSSVI through a legacy parity-specific driver and every other curve through a newer generic driver. The split changes filtering, acceleration, calendar handling, diagnostics, and incremental-update semantics by curve family.

No P0 defect was found. The highest-priority risks are:

1. The canonical eSSVI path bypasses the shared quote filter/cap/shortcut machinery and stays serial, so profile and `CalibOpts` behavior is family-dependent.
2. Slice failures are silently skipped; a one-slice surface counts as a successful build and suppresses the configured fallback ladder.
3. Curve selection compares candidates on different surviving expiry populations and ignores coverage, while its de-Am preprocessing ignores the configured method/tolerances/cache.
4. The dense active-set QP reports success when it hits its iteration cap and never populates its advertised active-constraint diagnostic.
5. The incremental eSSVI refit is not exposed by `PricerFitter`, documents the wrong observation builder for American options, can cross the next expiry, and leaves most diagnostics stale.
6. Corpus construction attempts to disable nested fit parallelism with the valuation-thread knob, which does not control fitting; exceptions escaping the generic fit's worker lambda terminate the process.

The immediate engineering objective should be to make one canonical observation/fit/admission pipeline and require explicit surface admission before publishing a fit. Performance work should follow that unification; otherwise optimization continues to improve different paths with different semantics.

## Evidence and validation

### Repository state

The review was performed against `git rev-parse --short HEAD == 01d88b8`. The worktree also contained unrelated or separately in-flight changes in `atx-vol/CMakeLists.txt`, `include/atx/vol/correction.hpp`, `include/atx/vol/counters.hpp`, `src/correction.cpp`, and `tests/correction_test.cpp`, plus untracked documents/examples. None of the core fitting files cited by the confirmed findings was modified. The checked-in fitting baseline and current benchmark source do differ, which is itself finding F-10.

### Focused test run

Command:

```powershell
.\build\bin\atx-vol-tests.exe --gtest_filter="CalibOpts.*:EssviFitSlice.*:EssviCalibSurface.*:EssviCalibSurfaceSequential.*:SviCalib.*:SviMmCalib.*:SviCalibSurface.*:SviMmCalibSurface.*:CalibRobustness.*:DenseSlice.*:ProfileClassifier.*:ProfileRegistry.*:FitPolicy.*:CalibratePool.*:VolaParity.*:SurfaceParity.*:FitPreset.*:CurveSurfaceNoArb.*:CurveFitParallel.*:CurveFitParity.*:PricerFitterTest.*:PricerFitterPolicy.*:LinearVarianceCurve.*:C8Calib.*:C8Capability.*" --gtest_brief=1
```

Result: **100 tests passed**, 0 failed, 38.138 s. The cached real-SPY informational cases printed 10.515 s serial versus 4.148 s auto-worker generic fit (2.53x) and 3.359 s parity-on versus 1.598 s parity-off (2.10x). The real fixture was present on this machine. No fresh full benchmark was run: the review build was not rebuilt from a clean tree, the worktree had unrelated CMake changes, and the checked-in baseline does not cover most currently registered fitting cases.

Green tests do not contradict the findings below: most findings are cross-path contract gaps, admission policy omissions, or failure modes not represented by the current tests.

## True production pipeline map

```text
QuoteFrame + MarketEnv
  -> OptionChain::from_frame
     -> data_install (SoA board, expiries sorted by T)
  -> PricerFitter::fit
     -> select_fit_policy (unless curve pinned / Hft pinned)
     -> optional select_curve OOS search
     -> materialize SessionInputs
     -> VolaSession::build
        -> build correction caches (unless disabled / term-rate board)
        -> if curve == Essvi:
             run_surface_parity                         [legacy path]
             -> resolve_chain_forward
             -> build_aligned_obs                       [private builder]
             -> essvi_fit_slice                         [SIMD LM is wired]
             -> optional eSSVI calendar repair
             -> parity score
           else:
             fit_curve_surface                          [generic path]
             -> parallel run_deam_prepass
                -> resolve_chain_forward
                -> build_observations_european          [shared builder]
                -> optional parity-data inversion
             -> sequential fit_slice_curve
                -> ConvexDense | SVI | LinearVariance | C8
                -> optional previous-slice floor for ConvexDense/LinearVariance
             -> parity score
        -> retain context + diagnostics + fitted surface
  -> FittedSurface/VolaSession queries
  -> VolaSession::to_priced_surface
     -> corpus/archive/portfolio consumers
```

The public ingestion path is clear and owns its lifetime correctly: `OptionChain` owns a one-underlier `Universe` and documents the reader/writer contract ([`include/atx/vol/chain.hpp:14`](../../include/atx/vol/chain.hpp#L14)); `from_frame` installs the data, resolves spot/time/rate, and returns a move-only handle ([`src/chain.cpp:14`](../../src/chain.cpp#L14)); ingestion sorts chains by maturity before fitting ([`src/data.cpp:490`](../../src/data.cpp#L490)). `PricerFitter::fit` leaves the previous fitted surface intact until a replacement build succeeds ([`src/pricer_fitter.cpp:165`](../../src/pricer_fitter.cpp#L165), [`src/pricer_fitter.cpp:187`](../../src/pricer_fitter.cpp#L187)).

There is also an **alternate, noncanonical pipeline**: `calibrate_pool -> essvi_calib_surface/svi_calib_surface -> VolSurface`. The corpus deliberately does not use it because it cannot produce the canonical `PricedSurface` or select all current families ([`src/corpus.cpp:487`](../../src/corpus.cpp#L487)). This alternate pipeline owns several features that the canonical path does not: surface-level warm priors, per-expiry eSSVI parallelism, shared filtering, and optional de-Am.

## Severity model

- **P0:** immediate correctness/data-loss/process-safety failure in normal supported use; stop-ship.
- **P1:** high-impact correctness, reliability, or performance defect likely to affect a supported production configuration.
- **P2:** material risk, misleading contract, significant feature/algorithm gap, or performance debt.
- **P3:** localized maintainability, diagnostics, or low-risk optimization opportunity.

Confidence is **High** when the behavior follows directly from reachable code or a direct test; **Medium** when impact depends on market data/configuration even though the mechanism is confirmed.

## Findings

### F-01 — P1 — Canonical eSSVI bypasses the shared observation policy and parallel prepass

**Type:** confirmed configuration/correctness inconsistency and performance defect  
**Confidence:** High

`VolaSession::build` special-cases eSSVI into `run_surface_parity`; every other curve goes through `fit_curve_surface` ([`src/session.cpp:349`](../../src/session.cpp#L349), [`src/session.cpp:411`](../../src/session.cpp#L411)). The generic path uses `build_observations_european`, which first runs the shared `build_observations` filter cascade, then applies `max_obs_per_slice`, the OTM shortcut, the configured method/tolerances, and optional caches ([`src/curve_fit.cpp:188`](../../src/curve_fit.cpp#L188), [`src/calib.cpp:336`](../../src/calib.cpp#L336)).

The eSSVI path instead calls private `build_aligned_obs`, which checks only a positive/non-crossed quote and successful de-Am. It does not read `CalibOpts`: flags, spread-to-mid, spread-vol, minimum weight, anchors, observation cap, and shortcut are absent ([`src/surface_parity.cpp:71`](../../src/surface_parity.cpp#L71), [`src/surface_parity.cpp:96`](../../src/surface_parity.cpp#L96), [`src/surface_parity.cpp:376`](../../src/surface_parity.cpp#L376)). It also computes its own weights and replaces invalid weights with `1.0`, rather than applying the shared acceptance rule ([`src/surface_parity.cpp:138`](../../src/surface_parity.cpp#L138)). The code then fits expiry-by-expiry in one serial loop ([`src/surface_parity.cpp:347`](../../src/surface_parity.cpp#L347)); `SurfaceParityInputs::fit_workers`, `score_parity`, and `use_deam_cache_for_fit` explicitly do not apply to this path ([`include/atx/vol/surface_parity.hpp:111`](../../include/atx/vol/surface_parity.hpp#L111), [`include/atx/vol/surface_parity.hpp:122`](../../include/atx/vol/surface_parity.hpp#L122)).

**Impact:** selecting eSSVI changes which quotes enter the fit and invalidates the stated idea that profile calibration policy is shared across curve families. Stale/flagged/wide/low-vega observations can influence the eSSVI production surface even when the profile says otherwise. Dense-board caps/shortcuts and the generic 2.5x real-SPY expiry fan-out are unavailable. Results and latency therefore change for reasons other than model family.

**Remediation:** replace `build_aligned_obs` with one canonical `PreparedSliceObservations` builder that returns European fit rows plus aligned raw NBBO/parity rows and rejection reason counts. Use it from both drivers. Then either make eSSVI a `fit_slice_curve` family in the generic driver with its calendar strategy injected, or move the generic prepass into a common surface builder. Preserve an explicit compatibility mode if bit identity is required.

**Tests:** parameterized all-family test proving the same accepted `(expiry,K,side)` keys under identical `CalibOpts`; inject each kill flag and filter boundary; prove `max_obs_per_slice`, anchor, method, and shortcut behavior for eSSVI; bit-identity across `fit_workers`; parity-off must not alter the surface for eSSVI.

### F-02 — P1 — Partial fits are admitted as success and prevent the fallback ladder

**Type:** confirmed admission defect  
**Confidence:** High

Both surface drivers silently `continue` on forward, observation, or per-slice fit failures. The generic path skips a failed slice and succeeds if any slice remains ([`src/curve_fit.cpp:272`](../../src/curve_fit.cpp#L272), [`src/curve_fit.cpp:303`](../../src/curve_fit.cpp#L303), [`src/curve_fit.cpp:367`](../../src/curve_fit.cpp#L367)); the eSSVI path does the same ([`src/surface_parity.cpp:347`](../../src/surface_parity.cpp#L347), [`src/surface_parity.cpp:381`](../../src/surface_parity.cpp#L381), [`src/surface_parity.cpp:397`](../../src/surface_parity.cpp#L397)). `PricerFitter` walks fallback rungs only when the entire `VolaSession::build` returns an error ([`src/pricer_fitter.cpp:165`](../../src/pricer_fitter.cpp#L165), [`src/pricer_fitter.cpp:171`](../../src/pricer_fitter.cpp#L171)).

**Impact:** a primary family that fits one easy expiry and fails the rest is reported as successful, `used_fallback` stays false, and queries clamp/extrapolate from an incomplete tenor set. The more robust fallback family is never tried. Operators receive aggregate `n_slices/n_quotes`, but no required-coverage gate or rejection reasons.

**Remediation:** introduce `FitAdmissionPolicy` with minimum fitted-expiry fraction, required tenor buckets/front expiry, maximum consecutive gaps, and maximum rejection fraction. Return a structured `SurfaceBuildReport` containing every attempted slice and reason. Treat admission failure as a primary build failure so the existing fallback ladder can run. Preserve partial surfaces only behind an explicit degraded/advisory mode.

**Tests:** force selected expiries to fail de-Am and optimization; verify primary rejection, fallback activation, and reason codes; verify one-slice success is rejected for a multi-expiry board; test required front/30d/90d buckets.

### F-03 — P1 — OOS curve selection is not population-comparable and ignores configured de-Am policy

**Type:** confirmed algorithm/configuration defect  
**Confidence:** High

The selector calls bare `build_observations_european`, so it defaults to Andersen-Lake, accurate tolerances, no caches, and ignores `in.deam.method`, `al_opts`, `iv_tol`, `iv_max_iter`, and supplied caches ([`src/curve_selector.cpp:116`](../../src/curve_selector.cpp#L116), [`src/curve_selector.cpp:127`](../../src/curve_selector.cpp#L127), defaults at [`include/atx/vol/calib.hpp:315`](../../include/atx/vol/calib.hpp#L315)). The production fit that follows does honor those inputs. Selector training data can therefore differ from the final fit data; scoring does use the configured pricing method, creating a mixed policy within one candidate evaluation ([`src/curve_selector.cpp:167`](../../src/curve_selector.cpp#L167)).

Candidates that fail a slice simply skip it. `CandidateScore` records `n_slices/n_holdout`, but selection ranks only `oos_vw` and average DoF; there is no common-expiry intersection or coverage penalty ([`src/curve_selector.cpp:151`](../../src/curve_selector.cpp#L151), [`src/curve_selector.cpp:190`](../../src/curve_selector.cpp#L190), [`src/curve_selector.cpp:207`](../../src/curve_selector.cpp#L207)). A curve fitting one easy expiry can beat a curve fitting all expiries. The cap takes the first eligible expiries in ascending chain order, despite the header saying liquid near-money expiries are preferred ([`src/curve_selector.cpp:106`](../../src/curve_selector.cpp#L106), [`include/atx/vol/curve_selector.hpp:58`](../../include/atx/vol/curve_selector.hpp#L58)).

**Impact:** auto selection can choose the wrong family and pay unnecessary cold de-Am cost, especially on irregular/negative-carry boards or when families have different failure domains. Selection is not an honest paired OOS comparison.

**Remediation:** prepare one keyed, configured observation/holdout dataset once. Score all candidates on the same required expiry/strike keys; reject candidates below a coverage floor. Use a lexicographic score: admission, coverage, weighted loss/in-band, then complexity/stability. Pass the exact production de-Am policy. Select expiries by deterministic liquidity/tenor stratification, not prefix order.

**Tests:** candidate A fits one expiry while B fits all; B must win or A must be inadmissible. Verify selection changes neither prepared rows nor chosen family when the same configured BAW/AL method is passed through. Add missing standalone selector tests (current selector coverage is mostly real-SPY integration).

### F-04 — P1 — Dense active-set QP silently returns a non-optimal iterate on iteration exhaustion

**Type:** confirmed optimizer contract defect  
**Confidence:** High

`qp_active_set` returns `Ok(x)` after its loop exhausts ([`src/dense_slice.cpp:42`](../../src/dense_slice.cpp#L42), [`src/dense_slice.cpp:130`](../../src/dense_slice.cpp#L130)). The public contract says a failure to converge returns `Internal` ([`include/atx/vol/dense_slice.hpp:114`](../../include/atx/vol/dense_slice.hpp#L114)). `max_iter <= 0` therefore returns the handcrafted initial feasible curve as a successful fit. Neither Mid nor Interval branch verifies KKT residuals after return ([`src/dense_slice.cpp:523`](../../src/dense_slice.cpp#L523), [`src/dense_slice.cpp:548`](../../src/dense_slice.cpp#L548)). `ConvexSliceFit::n_active` is advertised as an optimum diagnostic but is never assigned in either result construction ([`include/atx/vol/dense_slice.hpp:55`](../../include/atx/vol/dense_slice.hpp#L55), [`src/dense_slice.cpp:525`](../../src/dense_slice.cpp#L525), [`src/dense_slice.cpp:550`](../../src/dense_slice.cpp#L550)).

**Impact:** a capped/ill-conditioned QP can publish a feasible but materially suboptimal surface with a success status, defeating fallback and quality logic. Diagnostics cannot reveal cap exhaustion or constraint activity.

**Remediation:** return `{x, converged, iterations, active_count, stationarity, primal_violation, complementarity}`. Reject nonconvergence by default; optionally accept it only through an explicit approximate-fit policy plus a quality gate. Validate `max_iter`, `node_cap`, `lambda`, and all finite inputs before allocation/solve.

**Tests:** `max_iter=0/1` must fail unless convergence is proven; construct a known constrained QP and assert KKT tolerances; assert `n_active > 0` on a binding fixture; fuzz extreme weights/spreads and compare to a trusted convex-QP oracle.

### F-05 — P1 — Incremental refit is stranded and can corrupt American/calendar consistency

**Type:** confirmed unwired feature plus correctness defect  
**Confidence:** High

`VolaSession::refit_slice` is implemented and warm-starts eSSVI, but `FittedSurface` exposes only `const VolaSession&`, so a `PricerFitter` owner cannot call it ([`include/atx/vol/pricer_fitter.hpp:158`](../../include/atx/vol/pricer_fitter.hpp#L158), [`include/atx/vol/pricer_fitter.hpp:165`](../../include/atx/vol/pricer_fitter.hpp#L165)). The facade's documented quote-update flow reprices bands without refitting the model ([`include/atx/vol/pricer_fitter.hpp:7`](../../include/atx/vol/pricer_fitter.hpp#L7)).

For direct `VolaSession` users, the docs tell callers to build `new_obs` using `build_observations`, which treats the American premium as European ([`include/atx/vol/session.hpp:248`](../../include/atx/vol/session.hpp#L248)). The initial canonical session fit de-Americanizes observations. The incremental update can therefore switch objective semantics. The refit floors only against the prior expiry, not the following expiry ([`src/session.cpp:698`](../../src/session.cpp#L698)); an upward move can cross the next slice. It detects calendar violations only after committing and leaves the invalid surface in place ([`src/session.cpp:714`](../../src/session.cpp#L714), [`src/session.cpp:719`](../../src/session.cpp#L719)). It updates `ctx_.n_used` and a boolean but not parity, quote totals, mean quality, worst quality, or violation counts ([`src/session.cpp:717`](../../src/session.cpp#L717)).

**Impact:** the intended live warm path is unavailable from the main API. If used directly as documented, an American surface can drift due to inconsistent observation construction, and the update can leave a calendar-crossed surface with stale quality telemetry.

**Remediation:** add `PricerFitter::refit_expiry(const OptionChain&, expiry/id)` that internally rebuilds canonical configured European observations, warm-starts the family-specific state, repairs/validates both adjacent calendar relationships, re-scores affected diagnostics, and commits transactionally only after admission. Generalize beyond eSSVI or clearly restrict by curve kind. Keep a last-known-good surface.

**Tests:** American known-truth cold-full-fit versus incremental-refit equivalence; upward middle-slice move must not cross the next expiry; failure leaves surface and all diagnostics bit-identical; `PricerFitter` quote update changes model IV after refit; parity and quote counters refresh.

### F-06 — P1 — Corpus fit parallelism is accidentally nested; worker exceptions can terminate

**Type:** confirmed performance/reliability defect  
**Confidence:** High

Corpus construction says each board fits single-threaded and sets `cfg.n_threads = 1` ([`src/corpus.cpp:527`](../../src/corpus.cpp#L527)). `PricerConfig::n_threads` controls only `value_chain`, not fitting ([`include/atx/vol/pricer_fitter.hpp:138`](../../include/atx/vol/pricer_fitter.hpp#L138)); `PricerFitter::fit` never maps it to `SurfaceParityInputs::fit_workers`. Generic fits therefore use `fit_workers=0` and fan each board across all auto workers while the corpus simultaneously fans across boards.

The generic prepass uses `parallel_for_dynamic` ([`src/curve_fit.cpp:151`](../../src/curve_fit.cpp#L151)). Its thread body does not catch exceptions ([`include/atx/vol/parallel_for.hpp:125`](../../include/atx/vol/parallel_for.hpp#L125)); an allocation exception from observation vectors escaping a `std::jthread` invokes `std::terminate`, bypassing the corpus-level `try/catch` ([`src/corpus.cpp:517`](../../src/corpus.cpp#L517), [`src/corpus.cpp:587`](../../src/corpus.cpp#L587)). The alternate eSSVI calibrator already catches worker exceptions, demonstrating the intended pattern ([`src/essvi_calib.cpp:1188`](../../src/essvi_calib.cpp#L1188)).

**Impact:** CPU oversubscription, tail-latency instability, and excess memory on multi-board builds; rare allocation failures become process death rather than a failed fit.

**Remediation:** add an explicit `fit_workers` to `PricerConfig` and propagate it. Use a shared executor/budget so board-level and expiry-level parallelism cannot multiply. Make `parallel_for*` capture worker exceptions and rethrow/report on the caller thread, or require `noexcept` lambdas that record slot errors. Corpus should choose one parallel dimension from board count and expiry cost.

**Tests:** instrument maximum concurrent tasks for 20 boards and assert it never exceeds the budget; force an exception in a worker and verify a returned error; benchmark board/expiry hybrid scheduling with p50/p95 and peak memory.

### F-07 — P1 — “No-arb” and Robust guarantees are not uniform across served curve families

**Type:** confirmed contract gap; market impact is configuration-dependent  
**Confidence:** High mechanism / Medium occurrence

For generic fits, the previous-slice callback is applied only to ConvexDense and LinearVariance. eSSVI/SVI explicitly ignore it in `fit_slice_curve`; C8 also has no calendar floor ([`src/vol_curve.cpp:199`](../../src/vol_curve.cpp#L199), [`src/vol_curve.cpp:207`](../../src/vol_curve.cpp#L207), [`src/vol_curve.cpp:282`](../../src/vol_curve.cpp#L282)). `fit_curve_surface` does not apply `CalendarRepair`; it only passes the local previous-slice callback ([`src/curve_fit.cpp:282`](../../src/curve_fit.cpp#L282)). Thus a generic SVI/C8 surface selected under a repair-oriented configuration is checked but not repaired. `VolaSession` merely records the calendar result ([`src/session.cpp:357`](../../src/session.cpp#L357)).

LinearVariance is direct interpolation of total variance with flat wings ([`src/vol_curve.cpp:81`](../../src/vol_curve.cpp#L81)); it imposes no strike convexity. The Hft preset disables calendar floor and repair ([`src/session.cpp:240`](../../src/session.cpp#L240)). Dense QP enforces positivity/monotonicity/convexity of node call prices, but the lower slope bound is optional and off by default, and its wing extrapolation clamps call price flat ([`include/atx/vol/dense_slice.hpp:69`](../../include/atx/vol/dense_slice.hpp#L69), [`src/dense_slice.cpp:145`](../../src/dense_slice.cpp#L145)). That is butterfly-convex on the node domain, not a full global call-price boundary guarantee.

**Impact:** `calendar_arb_free=false` is possible on a successfully served “Robust”/Hft surface; arbitrary-strike risk queries can return NaN outside the fitted dense domain; Hft marks are unsuitable as a risk surface without separate admission. Existing real-SPY dense test deliberately accepts two residual calendar crossings ([`tests/curve_noarb_test.cpp:60`](../../tests/curve_noarb_test.cpp#L60)).

**Remediation:** define consumer-specific surface contracts: mark, quote, and risk. Enforce price bounds, monotone slopes, convexity, calendar monotonicity, finite-domain coverage, and forward-variance bounds over an independent common grid before publishing a risk surface. Add a family-agnostic joint calendar projection or reject/fallback SVI/C8/LinearVariance when the risk contract fails. Add principled wing boundary nodes/asymptotics to ConvexDense.

**Tests:** all families through the same independent invariant oracle; Hft must be explicitly mark-only or pass the requested risk band; generic SVI/C8 with a constructed crossing must repair or reject; dense extrapolation must respect intrinsic/upper bounds and remain invertible.

### F-08 — P2 — Profile and calibration configuration contains material inert or misrepresented fields

**Type:** confirmed dormant configuration  
**Confidence:** High

`PricerFitter` copies only `profile.calib`; it never applies `profile.filter`, cadence, economic precision, pricing route, EWMA, or subtick fields ([`src/pricer_fitter.cpp:80`](../../src/pricer_fitter.cpp#L80), profile fields at [`include/atx/vol/profile.hpp:102`](../../include/atx/vol/profile.hpp#L102)). `prefit_filter_underlier` is implemented but called only by tests, not the canonical fit ([`include/atx/vol/arb.hpp:270`](../../include/atx/vol/arb.hpp#L270)).

Several `CalibOpts` are serialized/configurable but have no fitter read in current HEAD: `max_weight`, `essvi_fallback_rmse_threshold`, `n_butterfly_grid`, `essvi_rho_mode`, `loss_kind` for parametric fits, and `essvi_asymmetric_rho` ([`include/atx/vol/calib.hpp:143`](../../include/atx/vol/calib.hpp#L143), [`include/atx/vol/calib.hpp:163`](../../include/atx/vol/calib.hpp#L163), [`include/atx/vol/calib.hpp:200`](../../include/atx/vol/calib.hpp#L200)). Profile residual types `Fengler` and `WingBspline` silently take the generic non-C2 branch and are fit as `HingeQuad` ([`src/profile.cpp:66`](../../src/profile.cpp#L66), [`src/profile.cpp:171`](../../src/profile.cpp#L171), [`src/essvi_calib.cpp:910`](../../src/essvi_calib.cpp#L910)).

`SymbolFitConfig` and `apply_symbol_config` are described as the persisted fitting-pipeline binding ([`include/atx/vol/surface_db.hpp:102`](../../include/atx/vol/surface_db.hpp#L102)), but all call sites are tests; neither `PricerFitter` nor corpus reads the DB binding ([`src/surface_db.cpp:270`](../../src/surface_db.cpp#L270)). `enabled` therefore does not itself disable canonical fitting.

**Impact:** operator configuration can round-trip through storage and tests without affecting production. Profile labels overstate the actual residual model and filtering/cadence policy.

**Remediation:** maintain a generated/central option-consumption table and fail validation for unsupported nondefault fields. Wire `SymbolFitConfig -> PricerConfig/SessionInputs` at the actual service/corpus entry. Either implement distinct residual bases and rho/loss modes or remove/mark them unsupported. Apply prefit filters on a prepared snapshot rather than mutating the live chain.

**Tests:** toggle every public/persisted field and assert an observable plan/result change or an explicit `NotImplemented/InvalidArgument`; integration test DB config through corpus fit; profile filter rejection counts by reason.

### F-09 — P2 — Failure diagnostics are lossy and sometimes internally misleading

**Type:** confirmed diagnostics gap  
**Confidence:** High

Per-slice errors are discarded by `continue` in both drivers (F-02). Generic parity failures become zero reports, and when parity is disabled the same zero sentinel is used ([`src/curve_fit.cpp:313`](../../src/curve_fit.cpp#L313)). Generic dense parity hardcodes `n_curve_params=3`, even for 5-, 8-, or many-node curves ([`src/curve_fit.cpp:336`](../../src/curve_fit.cpp#L336)); reduced chi-square is therefore not comparable across families. The session diagnostic averages only scored reports, but exposes no scored-slice count or rejection breakdown ([`src/session.cpp:370`](../../src/session.cpp#L370)). C8 can silently return its eSSVI seed with `bumps_active=false` while the selected kind remains C8 ([`src/vol_curve.cpp:315`](../../src/vol_curve.cpp#L315)). Correction-cache build failures also silently fall back, although that is primarily in the pricing review.

**Impact:** operators cannot distinguish “diagnostic intentionally off,” “parity failed,” “slice failed,” “C8 reverted,” and a genuine zero metric. Cross-family chi-square can guide the wrong decision.

**Remediation:** use explicit status enums/optionals, actual `slice->dof()`, per-stage counts/timings/reasons, and surface health/admission state. Record primary and fallback per-slice outcomes and whether shortcuts/caches/seeds were used.

**Tests:** parity failure versus disabled status; correct DoF for all families; C8 seed-revert flag; serialized diagnostics round-trip.

### F-10 — P2 — The fitting benchmark baseline is stale and does not benchmark the canonical facade

**Type:** confirmed performance-gating gap  
**Confidence:** High

The current benchmark registers surface de-Am cold/cached, slice cold/warm, SVI slice, and American-IV cases ([`bench/fitting_throughput_bench.cpp:607`](../../bench/fitting_throughput_bench.cpp#L607)). The checked-in fitting baseline contains medians for only `fit/surface_cold/spy_synth`, `corpus/build_20boards`, and `fit/surface_cold/spy_real` ([`bench/baselines/i7-1260p-clang18-sse2-fitting.json:132`](../../bench/baselines/i7-1260p-clang18-sse2-fitting.json#L132), [`bench/baselines/i7-1260p-clang18-sse2-fitting.json:303`](../../bench/baselines/i7-1260p-clang18-sse2-fitting.json#L303), [`bench/baselines/i7-1260p-clang18-sse2-fitting.json:474`](../../bench/baselines/i7-1260p-clang18-sse2-fitting.json#L474)). The surface cases time `essvi_calib_surface`, not the blessed `OptionChain -> PricerFitter -> VolaSession` path ([`bench/fitting_throughput_bench.cpp:138`](../../bench/fitting_throughput_bench.cpp#L138), canonical corpus statement at [`src/corpus.cpp:487`](../../src/corpus.cpp#L487)).

**Impact:** regressions in policy selection, correction-cache construction, borrow resolution, generic prepass, calendar enforcement, fallback, and ownership are not gated. Several newly wired C0/C1/C2 features have source benchmarks but no checked-in comparison row.

**Remediation:** add canonical end-to-end cases by preset/family and phase timings/counters; regenerate the baseline from current HEAD on the pinned clean host; make expected benchmark-name coverage a CI check. Separate microbenchmarks from product latency and report p50/p95, CPU, peak allocations, slices/s, observations/s, and rejected-slice count.

**Tests/gates:** baseline manifest equals registered benchmark names (allow explicit ungated annotation); clean-tree benchmark provenance includes commit/config/ISA; accuracy/admission gate must pass before a latency row is accepted.

### F-11 — P2 — Dense Interval loss has an unbounded dense-matrix complexity cliff

**Type:** confirmed data-structure/algorithm risk  
**Confidence:** High mechanism / Medium operational reach

Mid loss keeps the QP at `N <= node_cap`, but Interval loss expands the variable count to `N + 2M` and materializes dense Hessian/constraint matrices ([`src/dense_slice.cpp:416`](../../src/dense_slice.cpp#L416), [`src/dense_slice.cpp:434`](../../src/dense_slice.cpp#L434), [`src/dense_slice.cpp:479`](../../src/dense_slice.cpp#L479)). Active-set iterations then build and solve dense KKT systems ([`src/dense_slice.cpp:65`](../../src/dense_slice.cpp#L65)). `M` is the surviving observation count and can be large when `max_obs_per_slice=0`.

**Impact:** enabling Interval on a dense index can change memory from node-cap bounded to O(M²) and repeated solve work toward O(M³), creating latency and allocation cliffs. This option is public even though parametric `CalibOpts::loss_kind` is inert and dense uses a separate field.

**Remediation:** formulate interval loss with a bound-constrained/separable hinge objective evaluated without 2M explicit slack variables, or use a sparse QP solver and a hard observation budget. Reuse a per-worker QP workspace and exploit banded interpolation/third-difference structure. Make complexity limits part of config validation.

**Tests/benchmarks:** 50/500/5,000-row scaling, peak resident memory, interval-vs-reference solution parity, and explicit rejection above configured workspace capacity.

### F-12 — P3 — Several hot loops and containers can be made cheaper after correctness unification

**Type:** improvement ideas  
**Confidence:** Medium

- `CurveSurface::locate` and session rate/forward lookup linearly scan maturities on every query ([`src/vol_curve.cpp:119`](../../src/vol_curve.cpp#L119), [`src/session.cpp:88`](../../src/session.cpp#L88), [`src/session.cpp:470`](../../src/session.cpp#L470)). Use `lower_bound` or pre-resolved expiry groups for large batch consumers.
- The selector sorts the same prepared expiry indices and allocates fit vectors on each selection run; cache a `PreparedBoard` keyed by snapshot and reuse it across candidates/refits ([`src/curve_selector.cpp:135`](../../src/curve_selector.cpp#L135)).
- LinearVariance copies full `FitObs` records merely to sort by `k`; sort indices or construct compact `{k,w,weight}` nodes in the preparation stage ([`src/vol_curve.cpp:220`](../../src/vol_curve.cpp#L220)).
- The dense QP creates vectors of dense row vectors before copying them into `G`; assemble the final sparse/banded structure directly ([`src/dense_slice.cpp:342`](../../src/dense_slice.cpp#L342)).
- Profile median spread uses the first 256 accepted quote legs, not a representative reservoir, so classification depends on chain/strike order ([`src/profile.cpp:477`](../../src/profile.cpp#L477), [`src/profile.cpp:523`](../../src/profile.cpp#L523)). Use deterministic stratified/reservoir sampling or a streaming quantile.

These are subordinate to F-01/F-02: optimizing duplicated preparation paths would entrench divergent behavior.

## Implemented/wired versus dormant inventory (current HEAD)

This table supersedes stale statements in planning documents; status is based on reachable current code, not sprint labels.

| Capability | Current status | Evidence / limitation |
|---|---|---|
| AVX2 eSSVI backbone + Jacobian in LM | **Wired** | `essvi_calib.cpp` calls batch kernels in the objective/Jacobian ([`src/essvi_calib.cpp:146`](../../src/essvi_calib.cpp#L146), [`src/essvi_calib.cpp:230`](../../src/essvi_calib.cpp#L230)). Runtime dispatcher selects AVX2 when available. |
| eSSVI per-slice warm seed and prior regularization | **Wired primitive** | Public `essvi_fit_slice(..., warm)` and tests; used by `VolaSession::refit_slice` and alternate surface driver ([`include/atx/vol/essvi_calib.hpp:98`](../../include/atx/vol/essvi_calib.hpp#L98)). |
| Surface-level prior warm start | **Wired only in alternate driver** | `essvi_calib_surface(..., prior)` uses nearest-tenor prior ([`include/atx/vol/essvi_calib.hpp:139`](../../include/atx/vol/essvi_calib.hpp#L139)); canonical `PricerFitter/VolaSession::build` has no prior input. |
| Intra-name expiry parallelism | **Wired generic + alternate independent eSSVI** | Generic de-Am prepass is parallel; alternate eSSVI full independent fit is parallel. Canonical eSSVI parity path remains serial; sequential calendar floors remain serial by design. |
| Generic de-Am cap and OTM shortcut | **Wired for non-eSSVI canonical curves** | Shared builder applies both ([`src/calib.cpp:350`](../../src/calib.cpp#L350), [`src/calib.cpp:369`](../../src/calib.cpp#L369)); canonical eSSVI bypasses them. |
| Correction-cache de-Am | **Wired but intentionally off for generic dense default** | Generic path can opt in via `use_deam_cache_for_fit`; term-rate boards force it off ([`src/pricer_fitter.cpp:112`](../../src/pricer_fitter.cpp#L112)). Selector ignores it. |
| ConvexDense served by production facade | **Wired** | Non-eSSVI dispatch stores a `CurveSurface` override ([`src/session.cpp:354`](../../src/session.cpp#L354), [`src/session.cpp:405`](../../src/session.cpp#L405)). |
| LinearVariance Hft route | **Wired** | Hft pins LinearVariance and cap/shortcut policy ([`src/pricer_fitter.cpp:122`](../../src/pricer_fitter.cpp#L122), [`src/session.cpp:240`](../../src/session.cpp#L240)). It is a mark interpolant, not a constrained risk surface. |
| C8 event route | **Wired, with silent seed reversion** | Policy can route C8 and `fit_slice_curve` runs it; no generic calendar repair and seed reversion is not elevated to decision diagnostics. |
| SVI/SviMm low-level surface calibration | **Wired alternate path** | `calibrate_pool` dispatches them, but they ignore its de-Am option ([`src/calib_pool.cpp:165`](../../src/calib_pool.cpp#L165)). SviMm is not a `VolCurveKind` in the canonical facade. |
| Fallback ladder | **Wired at whole-build level** | All curve kinds have rungs, but partial-surface success prevents fallback (F-02). |
| Calendar floor on common knots | **Wired for LinearVariance** | Previous LinearVariance knots are unioned into the new grid ([`src/vol_curve.cpp:250`](../../src/vol_curve.cpp#L250)). ConvexDense still has known between-node residuals; SVI/C8 ignore floor. |
| eSSVI MonotoneFit/Project | **Wired legacy eSSVI path** | Active-set-like pseudo-observations plus post-check/project ([`src/surface_parity.cpp:184`](../../src/surface_parity.cpp#L184), [`src/surface_parity.cpp:429`](../../src/surface_parity.cpp#L429)). Not family-generic. |
| Incremental canonical model refit | **Dormant/partial** | Mutating method exists on `VolaSession`, but main owned facade exposes only const session; American observation and next-neighbor issues remain (F-05). |
| Profile prefit `FilterOpts` | **Dormant in canonical fit** | Implemented `prefit_filter_underlier`, no production caller. |
| Profile cadence/economic precision/pricing route | **Dormant in canonical fit** | Stored registry data, no `PricerFitter` consumption beyond `profile.calib`. |
| Persisted `SymbolFitConfig` pipeline binding | **Test-only** | `apply_symbol_config` call sites are tests; corpus uses `PricerConfig` template directly. |
| `EssviRhoMode::{Shared,TermStructure}` | **Unimplemented/inert** | Public/serialized enum, no calibration dispatch read. |
| `essvi_asymmetric_rho` | **Inert** | Stored and profiled; no fitter read. |
| Parametric interval loss | **Inert** | `CalibOpts::loss_kind` is not consumed by eSSVI/SVI; ConvexDense has a separate `ConvexFitOpts::loss`. |
| Fengler/WingBspline residual identities | **Miswired** | Anything non-C2 maps to HingeQuad in eSSVI fitter. |
| `essvi_fallback_rmse_threshold`, `n_butterfly_grid`, `max_weight` | **Inert** | Serialized/configured without implementation reads. |
| Vectorized multi-slice calibrator | **Not implemented** | The LM evaluates observations in SIMD within a slice; there is no lane-packed multi-slice optimizer. |
| Joint common-lattice calendar constructor | **Not implemented** | Current implementation is sequential local floors/post-projection; no global weighted minimum-change solve. |
| Independent risk-surface admission oracle / last-known-good | **Not implemented in facade** | Checks produce diagnostics, but most failures do not block publication. Corpus has separate admission policy after fit. |
| Structured per-stage fit telemetry | **Partial** | Environment-driven stderr timings exist in generic/legacy paths; no stable structured API and no per-slice failure reasons. |

## Strengths worth preserving

1. **Ownership and transactional replacement:** the public chain/fitter/session types are move-only where appropriate; a failed rebuild does not destroy the last surface.
2. **Deterministic concurrency:** generic de-Am and alternate eSSVI expiry fits write disjoint slots and reduce in stable order. Current tests prove bit identity across worker counts.
3. **Correct model coordinates:** fitting and serving consistently use per-expiry forward log-moneyness and retain carry context for queries.
4. **Configured term rates disable the scalar correction cache:** this avoids silently using a cache built at one scalar rate ([`src/pricer_fitter.cpp:112`](../../src/pricer_fitter.cpp#L112)).
5. **SIMD is genuinely in a fitting hot loop:** this is no longer a bench-only primitive.
6. **The generic preparation phase is sensibly separated from sequential calendar-dependent fitting:** this is the right shape for deterministic parallel cold work.
7. **Tests include real-board, determinism, parity-off, warm-start, policy, fallback-shape, and robustness cases.** The gap is contract integration/admission, not absence of testing discipline.

## Prioritized recommendations

### Now: correctness and one canonical contract

1. Implement a keyed `PreparedBoard/PreparedSlice` used by selector, all curve families, parity scoring, and incremental refit. Delete the private duplicate eSSVI observation builder after compatibility tests.
2. Add structured per-slice results and surface admission. Make fallback respond to inadequate partial coverage, not only total failure.
3. Fix dense QP convergence status/KKT diagnostics before trusting its quality or timing.
4. Make surface publication transactional with a consumer-specific invariant oracle and last-known-good behavior.
5. Repair incremental refit semantics and expose them from `PricerFitter`, or remove the misleading facade narrative until it is safe.

### Next: scheduling and performance

6. Add explicit fit worker budget and a shared executor to prevent corpus/expiry nesting; make worker exception handling process-safe.
7. Reuse prepared observations, forward/borrow results, QP workspaces, and prior slice state across selector/final fit/refit.
8. Replace Interval loss's explicit dense slacks and exploit sparse/banded QP structure.
9. Regenerate a complete current-HEAD benchmark baseline and add canonical end-to-end product cases with correctness gates.

### Then: product/model completeness

10. Separate mark and risk surfaces or make the requested surface contract explicit. LinearVariance Hft should not silently masquerade as a risk surface.
11. Implement or remove inert public configuration; especially rho modes, residual identities, loss modes, profile filters, and persisted symbol config.
12. Build a family-agnostic joint calendar constructor and a deterministic expiry/strike-stratified selector with stability penalties.

## Proposed acceptance matrix

| Gate | Required assertions |
|---|---|
| Observation parity | Same accepted keys/reasons across all families for one policy; explicit compatibility exception only. |
| Carry/de-Am | Configured method/tolerance/cache identical in selector, final fit, parity, and refit; known-truth round trip. |
| Slice optimization | Finite parameters, optimizer converged, KKT/gradient diagnostics, price/variance residual within policy. |
| Strike no-arb | Intrinsic/upper price bounds, monotone slope bounds, convexity, finite IV over admitted risk band. |
| Calendar | Zero violations on independent common grid over admitted band; finite/nonnegative forward variance. |
| Coverage | Required tenor buckets and minimum expiry/quote fraction; no silent partial success. |
| Fallback | Primary rejection reason retained; fallback produces admitted surface or last-known-good remains. |
| Incremental | Transactional, both-neighbor-safe, diagnostics refreshed, full cold-fit parity within tolerance. |
| Determinism | Bit-identical results across worker schedules where promised; bounded executor concurrency. |
| Performance | Canonical p50/p95 cold/refit latency, observations/s, slices/s, allocations/peak memory, and accuracy gate. |

## Bottom line

Current HEAD has moved several previously planned C0/C1/C2 items into real code: eSSVI SIMD, surface warm priors in the low-level driver, expiry parallelism, generic de-Am acceleration, direct LinearVariance, C8 serving, and a first-class fitting benchmark target all exist. The key remaining problem is integration quality. Features land in different drivers, while the public facade suggests one coherent policy. The next sprint should not add another curve or optimizer first; it should unify preparation, admission, incremental state, scheduling, and telemetry so every family is selected, fit, validated, and published under the same explicit contract.
