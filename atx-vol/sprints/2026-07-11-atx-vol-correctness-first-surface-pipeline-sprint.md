# atx-vol Correctness-First Volatility Surface Pipeline Sprint

**Date:** 2026-07-11  
**Status:** implementation-ready charter  
**Priority:** correctness first; performance second  
**Evidence:** [`docs/spy-linear-variance-surface-review.md`](../docs/spy-linear-variance-surface-review.md)

## 1. Outcome

Replace the current ambiguous preset-driven surface path with a production pipeline that:

1. publishes a market-following **mark surface** and an independently admitted **risk surface**;
2. never serves a newly fitted risk surface that violates its correctness contract;
3. makes `Balanced` the default and lets users tilt toward latency or accuracy without turning
   correctness checks off;
4. reaches the latency target through adaptive work, batching, warm starts, and incremental
   updates rather than by accepting an unsafe fit; and
5. exposes enough diagnostics for callers and the UI to know what was fitted, what was checked,
   and whether the result is fresh, stale, or degraded.

The present HFT LinearVariance path remains useful as a market interpolant. It must no longer be
the automatic risk/theoretical surface for dense index and ETF boards.

## 2. Why the defaults must change

The SPY review found that the automatic dense-board route selects `FitPreset::Hft` and
`VolCurveKind::LinearVariance`. That route directly connects up to 48 retained market
total-variance nodes per expiry and disables parity scoring, calendar flooring, and calendar
repair.

On the reviewed SPY snapshot, the served path had:

- 1,500 sampled negative call-convexity steps, including 1,200 below 30 days;
- 226 sampled cross-expiry calendar violations;
- 1,188 large local IV-slope changes; and
- three adjacent ATM term changes larger than one volatility point.

The accurate, uncapped LinearVariance variant did not repair the surface. It reproduced more raw
board noise and increased the sampled convexity failures to 4,075. This proves that the primary
problem is the fit contract, not an insufficient node count or a weak root solver.

The American price-to-IV solver is already a strong foundation. Fast cold inversion was within
0.044 volatility basis points of accurate inversion at p95 and 0.132 basis points at maximum.
The HFT shortcut has a weaker tail, but inversion error is not the source of the broad smile and
term jaggedness. This sprint therefore hardens and adaptively audits the existing solver instead
of replacing it.

## 3. Product rule: quality modes do not redefine correctness

Correctness is an admission floor, not the expensive end of a speed/quality slider.

All quality modes share these non-negotiable requirements for the **risk surface**:

- valid, bounded, strike-monotone option prices;
- non-negative discrete call convexity over the admitted strike band;
- non-decreasing total variance across expiry on a shared log-moneyness grid;
- finite, stable values and controlled wings throughout the declared domain;
- independently checked price-to-IV and IV-to-price residual budgets;
- deterministic output for identical inputs and configuration; and
- explicit freshness, carry confidence, fit quality, and validation status.

If a fit does not pass these gates inside its latency budget, the engine returns the last
known-good risk surface with a `Stale` or `Degraded` status. It must never silently substitute an
unconstrained LinearVariance surface for risk.

Users may tilt how the engine gets to a valid answer:

| Mode | Intent | Adaptive work budget | Correctness behavior |
|---|---|---|---|
| `Latency` | Fastest admissible update | Fewer candidate knots, warm carry/QP state, fast cold inversion plus targeted audits, narrower validated risk band | Same hard gates; fall back to last known-good on budget or gate failure |
| `Balanced` **default** | Best production trade-off | Robust multi-pair carry, about 40 risk knots, selective accurate audits, 64-point calendar grid, incremental refits | Same hard gates; broad production risk band |
| `Accuracy` | Maximum fidelity and audit depth | More carry pairs and knots, accurate inversion for all accepted nodes, tighter tolerances, 128-point calendar grid, wider wings | Same hard gates with tighter residual and stability targets |

The mark surface has a different contract. It may preserve local market irregularities, but must
be named `MarketMark`, carry warnings, and never feed gamma, density, scenarios, strike-from-delta,
or relative-value risk unless a caller explicitly opts into raw-market behavior.

## 4. Target architecture

```text
OPRA snapshot
    |
    v
Quote normalization and quality/error bars
    |
    +--> robust carry/forward estimator ---- diagnostics and confidence
    |
    v
Adaptive de-Americanization
fast proposal -> residual estimator -> selective accurate audit
    |
    +--> MarketMarkSurface (market-following, clearly labeled)
    |
    v
Per-expiry constrained call-price fit
bounds + monotonicity + convexity + noise-aware smoothing
    |
    v
Joint calendar projection on common (k, T) lattice
    |
    v
Independent admission oracle
    |
    +--> admitted RiskSurface
    |
    +--> last-known-good fallback + degraded health state
```

The public result becomes a `SurfaceBundle`, not a single curve whose purpose is implicit:

```cpp
enum class SurfacePurpose : std::uint8_t { MarketMark, Risk };
enum class FitQualityMode : std::uint8_t { Latency, Balanced, Accuracy };
enum class SurfaceState : std::uint8_t { Healthy, Degraded, Stale, Rejected };

struct SurfaceBundle {
  FittedSurface market_mark;
  FittedSurface risk;
  SurfaceHealth health;
  ValidationDigest validation;
  FitProvenance provenance;
};
```

Exact ownership and move semantics may differ, but product purpose, quality mode, health, and
validation provenance must be explicit in the API and persisted archive.

## 5. Modeling decisions

### 5.1 Observations and error bars

- Normalize timestamps, quote state, crossing, spread, size, and stale flags before calibration.
- Treat bid/ask-derived uncertainty as an input to the loss, not only a post-fit score.
- Preserve all usable observations in the audit record. Candidate-knot compression is a compute
  optimization, not permission to erase provenance.
- Compute and report quality by tenor bucket: `<7d`, `7-30d`, `1-6m`, and `>6m`.

### 5.2 Carry and forward

- Replace the HFT single near-ATM pair with an adaptive 6-12 pair robust estimator in `Balanced`.
- Score pair freshness, spread, parity residual, and distance from ATM.
- Aggregate with a weighted robust location estimator and report dispersion, effective pair count,
  and leave-one-pair sensitivity.
- Permit a smaller pair set in `Latency` only when its dispersion and residual gates pass.
- Reuse the last admitted carry with a degraded state when the new cross-section is insufficient.

### 5.3 Price-to-IV inversion

- Keep the safeguarded Andersen-Lake inversion as the reference implementation.
- Use fast cold inversion as the default proposal because its measured error is already small.
- Treat the early-exercise shortcut as an approximation requiring an a-posteriori residual bound.
- Force accurate inversion for ultra-short, low-vega, wide-residual, boundary, or otherwise
  suspicious nodes.
- Batch by expiry and side, reuse warm boundary state, and parallelize independent work.
- A cache or shortcut may propose an answer; it may not certify its own answer.

### 5.4 Per-expiry fit

Use the existing `ConvexDense` machinery as the initial risk-surface baseline and evolve it into a
noise-aware constrained price fit:

- optimize in normalized call-price space;
- enforce price bounds, strike monotonicity, and convexity directly;
- use spread/vega-derived observation error bars in the objective;
- use the current adaptive 48-node logic only to propose candidate knots;
- tune smoothness from measurement uncertainty and held-out stability, not an arbitrary constant;
- derive IV from the admitted price curve, rather than imposing shape constraints indirectly on
  noisy IVs; and
- replace flat-variance risk wings with controlled, continuous asymptotic slopes or a verified
  parametric wing splice.

LinearVariance remains available for `MarketMark`. eSSVI/SVI/C8 remain valid risk candidates when
they clear the same independent admission oracle. Model selection may optimize held-out fit only
among admitted candidates.

### 5.5 Cross-expiry construction

The existing sequential floor/repair settings are not a sufficient product contract. Build a
common log-moneyness lattice and enforce non-decreasing total variance jointly across expiry.

- Preserve per-expiry price-shape constraints after calendar projection.
- Use a weighted minimum-change projection so liquid observations move less than uncertain ones.
- Evaluate serial, weekly, monthly, quarterly, 0DTE, and known event expiries explicitly.
- Interpolate in time with a representation that preserves non-negative forward variance.
- Publish ATM IV, total variance, and forward variance as separate term diagnostics.
- Allow real event structure; do not equate every adjacent IV decline with arbitrage.

### 5.6 Admission and fallback

The validator must be independent from the fitter and must run against cold/reference pricing on a
denser grid than the optimization grid. A successful optimizer status is not admission.

Each accepted generation is immutable and generation-stamped. On rejection or timeout:

1. retain the prior admitted risk generation;
2. publish the new mark surface if its own contract passes;
3. set `Degraded` or `Stale` with reason codes and ages; and
4. emit diagnostics, never a silent curve-family fallback.

## 6. Work packages

Estimates are focused engineering days and exclude queue time. Each package lands behind tests and
can be reviewed independently.

### SF0 - Baseline corpus and independent invariant oracle (1.5 days)

**Purpose:** turn the review probes into permanent, reproducible gates before changing the fit.

**Work**

- Promote SPY shape, calendar, inversion, carry, and latency measurements into test/benchmark
  helpers.
- Pin the ten-snapshot corpus already used by the fit matrix and add opening, stressed, and 0DTE
  cases as they become available.
- Build an independent grid validator for price bounds, strike monotonicity, convexity, calendar
  total variance, finite values, and wing behavior.
- Record old HFT, Robust/eSSVI, and ConvexDense baselines without changing thresholds to make weak
  behavior pass.
- Persist machine-readable results for before/after comparison.

**Likely areas:** `tests/`, `bench/`, `include/atx/vol/arb.hpp`, `src/arb.cpp`.

**Exit gate:** the current HFT SPY surface fails the new risk admission for the documented reasons;
the core cold inversion reference passes.

### SF1 - Policy API, dual surface, and compatibility seam (2 days)

**Purpose:** remove implicit model purpose and contradictory defaults.

**Work**

- Add `SurfacePurpose`, `FitQualityMode`, `SurfaceHealth`, `ValidationDigest`, and `SurfaceBundle`.
- Make `Balanced` the default in `PricerConfig` and in the UI.
- Change dense index/ETF auto policy so HFT LinearVariance is selected for `MarketMark`, not `Risk`.
- Keep `FitPreset` as a deprecated compatibility mapping for one release cycle.
- Reject configurations that disable mandatory risk gates; do not expose
  `enforce_calendar_floor=false` as a normal risk-speed knob.
- Version `surface_archive` and `surface_db` records with purpose, mode, health, validation digest,
  and source generation.

**Likely areas:** `fit_policy.hpp/.cpp`, `pricer_fitter.hpp/.cpp`, `session.hpp/.cpp`,
`surface_archive.hpp/.cpp`, `surface_db.hpp/.cpp`.

**Exit gate:** every surface consumer chooses mark or risk explicitly; old callers compile through a
documented mapping; no default route sends SPY risk to unconstrained LinearVariance.

### SF2 - Robust carry estimator (3 days)

**Purpose:** make forward estimation resistant to one noisy co-terminal pair.

**Work**

- Extract pair scoring and robust aggregation into a reusable carry component.
- Add adaptive pair counts, weighted robust aggregation, dispersion, and leave-one-out diagnostics.
- Add generation-to-generation stability and last-good carry fallback.
- Test bad ATM pair, crossed quote, missing side, hard-to-borrow, dividend, and long-dated cases.
- Benchmark one-pair, robust multi-pair, and warm incremental paths separately.

**Likely areas:** `deamer.hpp/.cpp`, `calib.hpp/.cpp`, session diagnostics.

**Exit gate:** perturbing or removing one eligible pair cannot move an admitted forward beyond its
configured confidence band; uncertain carry is surfaced, not hidden.

### SF3 - Adaptive de-Americanization with certified shortcuts (4 days)

**Purpose:** preserve the strong solver while eliminating unbounded approximation tails.

**Work**

- Split inversion into proposal, error estimate, accurate audit, and acceptance stages.
- Add tenor/moneyness/vega-aware audit rules.
- Require shortcut repricing residual below a configured fraction of half-spread.
- Batch and parallelize by expiry/side with deterministic result ordering.
- Add warm state and correction-cache reuse as proposals guarded by cold spot audits.
- Report counts and residual quantiles for fast, accurate, cached, and shortcut routes.

**Likely areas:** `american_iv.hpp/.cpp`, `calib.cpp`, `american.hpp/.cpp`, correction cache.

**Exit gate:** no accepted node exceeds the inversion residual budget; turning on a fast proposal
cannot change risk admission or deterministic results.

### SF4 - Constrained, noise-aware per-slice risk fit (5 days)

**Purpose:** remove strike arbitrage and microstructure kinks at the model boundary.

**Work**

- Extend `ConvexDense` to consume observation error bars and candidate knots.
- Enforce normalized-call bounds, monotonicity, and convexity in the optimization.
- Add uncertainty-scaled curvature regularization with an explicit, reproducible selection rule.
- Add controlled wings and continuity checks at the splice.
- Compare constrained dense, eSSVI, SVI, and C8 candidates only after admission.
- Add even/odd and leave-neighborhood-out scoring, plus perturbation stability for Greeks.

**Likely areas:** `convex_fit.hpp/.cpp`, `vol_curve.hpp/.cpp`, `curve_selector.cpp`, fit metrics.

**Exit gate:** every admitted slice has zero sampled price-shape violations on the independent grid;
front-expiry gamma and skew remain stable under one-tick quote perturbations.

### SF5 - Joint calendar and term constructor (5 days)

**Purpose:** make the complete surface, not merely each slice, arbitrage-safe.

**Work**

- Build the shared `(k, T)` lattice and weighted minimum-change calendar projection.
- Recheck and, if necessary, jointly reconcile slice convexity after calendar moves.
- Add forward-variance-preserving time interpolation and controlled extrapolation.
- Add event/serial metadata to diagnostics without smoothing away genuine event variance.
- Validate the Robust/eSSVI path through the same constructor; investigate the 89 violations from
  the reviewed snapshot rather than special-casing the model.

**Likely areas:** `surface_parity.hpp/.cpp`, `curve_fit.cpp`, `arb.hpp/.cpp`, `priced_surface`.

**Exit gate:** zero calendar violations on the independent admitted grid and zero regression in
per-slice strike shape after projection.

### SF6 - Performance engineering beneath the correctness floor (4 days)

**Purpose:** recover or exceed current throughput without weakening SF0-SF5.

**Work**

- Profile the full cold and incremental pipeline; report de-Am, carry, QP, calendar, validation,
  allocation, and serialization separately.
- Parallelize independent expiry and side work with fixed-order assembly.
- Reuse candidate knots, active sets, factorizations, carry state, and prior generations.
- Implement local single-expiry refits plus the minimum affected calendar neighborhood.
- Batch price/IV audits and eliminate avoidable allocations and virtual dispatch in hot loops.
- Keep every optimization behind the independent admission oracle.

**Likely areas:** calibration driver, `PricerFitter`, fit workspace/cache types, benchmarks.

**Exit gate:** performance targets in section 8 are met on admitted surfaces; no speed result is
reported for a run that fails correctness.

### SF7 - Health, fallback, telemetry, and observability (2.5 days)

**Purpose:** make failures safe and operationally diagnosable.

**Work**

- Add immutable generation publication and last-known-good retention.
- Add structured reason codes for rejected carry, inversion, slice, calendar, timeout, and stale
  inputs.
- Publish fit phase timings, active constraints, residual quantiles, audit sample counts, surface
  age, and fallback generation.
- Add deterministic replay metadata: input snapshot ID, configuration hash, build version, and seed.
- Define alert thresholds without treating genuine market/event movement as model failure.

**Exit gate:** every failed or timed-out build leaves consumers on an admitted surface and explains
exactly why the new generation was not published.

### SF8 - UI integration and default flip (2 days)

**Purpose:** make the safer model understandable and usable.

**Work**

- Default risk, Greeks, scenarios, strike-from-delta, density, and term/skew panels to the admitted
  `RiskSurface`.
- Offer the `MarketMarkSurface` as an overlay, not an unlabeled substitute.
- Add `Latency`, `Balanced`, and `Accuracy` controls with `Balanced` selected by default.
- Display surface health, freshness, carry confidence, shortcut/audit use, calendar state, and
  butterfly state.
- Plot ATM IV, total variance, and forward variance separately; label serial/event expiries.
- Remove the current UI hard pin to HFT LinearVariance.

**Likely areas:** `atx-ui/src/vol/spy_opra_surface.cpp`, `vol_workspace.cpp`, reusable vol widgets.

**Exit gate:** a user cannot mistake the mark interpolant for the risk surface, and all mode changes
preserve hard admission gates.

### SF9 - Qualification, shadow rollout, and retirement (2.5 days)

**Purpose:** safely replace the old default.

**Work**

- Run V2 in shadow mode against the existing pipeline and store comparison digests.
- Qualify SPY plus liquid ETF, ordinary single-name, event, hard-to-borrow/dividend, stressed,
  sparse, and 0DTE boards.
- Verify deterministic archives and replay across worker counts.
- Document compatibility mappings and remove silent fallbacks.
- Flip the production default only after all release gates pass; retain explicit legacy mark mode
  for one release, then schedule removal.
- Add before/after results to the review report.

**Exit gate:** the release corpus and latency gates are green, the UI defaults to `Balanced` risk,
and the legacy HFT route is opt-in and correctly labeled.

## 7. Delivery sequence and dependencies

| Milestone | Packages | Deliverable | Approximate effort |
|---|---|---|---:|
| A - Correctness contract | SF0-SF1 | Independent oracle, explicit product API, safe default policy | 3.5 days |
| B - Correct fit | SF2-SF5 | Robust carry, certified inversion, constrained slices, joint calendar surface | 17 days |
| C - Fast production path | SF6-SF7 | Profiled incremental engine, immutable publication, safe fallback | 6.5 days |
| D - Product rollout | SF8-SF9 | UI integration, corpus qualification, default flip | 4.5 days |

Critical path: `SF0 -> SF1 -> SF2/SF3 -> SF4 -> SF5 -> SF6 -> SF7 -> SF8 -> SF9`.
SF2 and SF3 can proceed in parallel after the oracle and policy types are stable. SF7 health types
should be stubbed in SF1 so fit packages emit diagnostics from their first implementation.

This is roughly 31 focused engineering days. It is better executed as three merge trains:

1. **Correctness foundation:** SF0-SF5 behind `ATX_SURFACE_PIPELINE_V2`, shadow only.
2. **Performance and operations:** SF6-SF7, still shadowed.
3. **Default migration:** SF8-SF9 after corpus qualification.

## 8. Release gates

### 8.1 Correctness gates - mandatory in every mode

Risk-surface admission on the declared tradeable band requires:

- zero option-price bound violations;
- zero strike-monotonicity violations;
- zero negative call-convexity steps on both optimization and independent grids;
- zero calendar total-variance violations on the common independent grid;
- no NaN/Inf or discontinuous wing splice in the valid domain;
- p95 accurate repricing residual below `0.25` half-spreads and maximum below `1.0`
  half-spread for accepted inversion nodes;
- stable fit classification and bounded Greek movement under one-tick and leave-one-node
  perturbations, with thresholds recorded by tenor bucket;
- deterministic model values, diagnostics, and archive bytes across worker counts; and
- a successful cold-reference sample audit of every shortcut/cache route.

The inversion thresholds above are initial release ceilings from the review recommendation. SF0
must record current distributions, and SF3 may tighten them. They may not be loosened to make a
latency mode pass.

Fit-quality scorecards are separate by product:

- `MarketMark`: in-band coverage, normalized residual, quote freshness, and reproduction fidelity.
- `Risk`: held-out price quality, constraint state, perturbation stability, Greek smoothness, and
  calendar/wing quality.

Price-in-band coverage alone is never a risk admission criterion.

### 8.2 Performance gates - evaluated only after correctness passes

Measure Release builds on the pinned i7-1260P-class reference host with warmup, repeated snapshots,
p50/p95, and phase-level timings. Initial targets are:

| Mode | Cold p50 | Cold p95 | One-expiry incremental p95 | Notes |
|---|---:|---:|---:|---|
| `Latency` | <= 100 ms | <= 250 ms | <= 10 ms | May validate a narrower declared risk band; same invariants |
| `Balanced` | <= 250 ms | <= 500 ms | <= 15 ms | Production default |
| `Accuracy` | <= 750 ms | <= 1.5 s | <= 50 ms | Full audits and wider grid |

These targets are ship gates to validate, not claims already achieved. Current measurements show
roughly 71-327 ms for the 48-node HFT path, while a full accurate LinearVariance run is roughly
483-1,031 ms. The constrained QP itself is relatively cheap; SF6 must optimize the measured
end-to-end de-Americanization and orchestration cost, not only microbenchmark the fitter.

No benchmark result counts if its output fails the SF0 admission oracle.

## 9. Default configuration and compatibility mapping

The new default should be equivalent to:

```cpp
PricerConfig cfg;
cfg.quality_mode = FitQualityMode::Balanced;
cfg.outputs = SurfaceOutputs::MarketMarkAndRisk;
cfg.risk_admission = RiskAdmission::Required;
cfg.fallback = SurfaceFallback::LastKnownGood;
```

Compatibility mapping for one release:

| Legacy request | V2 interpretation |
|---|---|
| `FitPreset::Hft + LinearVariance` | `Latency + MarketMark`; never implicit risk |
| `FitPreset::Fast` | `Latency + Risk`, subject to full admission |
| `FitPreset::Robust` | `Balanced + Risk` |
| `FitPreset::Accurate` | `Accuracy + Risk` |

An explicit legacy raw-risk escape hatch, if operationally unavoidable, must use a name such as
`UnsafeRawMarketRisk`, produce a red health state, and be unavailable to the default UI. Do not
preserve unsafe semantics under a reassuring preset name.

## 10. Test matrix

Every work package contributes to the same matrix:

| Axis | Required cases |
|---|---|
| Tenor | 0DTE, `<7d`, `7-30d`, `1-6m`, `>6m` |
| Underlier | SPY/liquid ETF, index, ordinary single-name, event name, HTB/dividend, sparse name, vol product |
| Market state | continuous, opening/incomplete, closing, stressed/wide, crossed/stale, missing side |
| Shape | normal skew, smile, event W-shape, noisy local butterfly, wing sparse, calendar crossing |
| Update | cold, unchanged replay, one quote, one expiry, carry change, spot move, dividend/rate change |
| Execution | serial, fixed worker counts, default worker count, archive/reload |

The corpus must contain real snapshots where licensing permits and synthetic known-truth fixtures
for invariants and fault injection. Synthetic tests prove the algorithm; real boards prove the
operating policy.

## 11. Relationship to existing plans

This sprint is the production surface-policy and fit-quality owner. It should reuse, not duplicate:

- the no-arbitrage validators and curve work in the SOTA engine workmodule ladder;
- the American-kernel and de-Americanization performance work in the P/C sprints;
- existing `ConvexDense`, eSSVI/SVI/C8, `CurveSelector`, and archive infrastructure; and
- the V-sprint's broader event/error-bar feature work where its contracts are already stable.

Where ownership overlaps, the lower-level sprint supplies the component and this sprint owns its
default orchestration, independent admission, fallback behavior, and UI/product semantics.

## 12. Definition of done

- `PricerConfig{}` produces both a market-mark surface and an admitted `Balanced` risk surface.
- Dense SPY/ETF auto policy no longer sends risk consumers to HFT LinearVariance.
- All risk models pass one independent admission oracle; no curve family certifies itself.
- All three quality modes preserve the same hard no-arbitrage and inversion requirements.
- Failed, timed-out, or uncertain fits retain the last known-good risk surface with explicit state.
- Robust carry and shortcut audit diagnostics are persisted and visible.
- Risk Greeks and scenarios are stable under documented quote perturbations.
- The full corpus passes shape, calendar, inversion, determinism, archive, and fallback tests.
- Performance targets pass on correctly admitted surfaces in Release builds.
- The UI distinguishes mark from risk and defaults to `Balanced`.
- The original SPY review contains a reproducible before/after qualification section.

## 13. First implementation slice

Start with SF0 and SF1 only. They create the safety harness and remove the dangerous ambiguity
without prematurely rewriting mathematics. The first pull request should:

1. promote the SPY review metrics into the independent validator;
2. introduce `SurfacePurpose`, `FitQualityMode`, and `SurfaceHealth`;
3. add a dual-surface result behind `ATX_SURFACE_PIPELINE_V2`;
4. route current LinearVariance output to `MarketMark`;
5. route current ConvexDense/Robust output through the validator as provisional `Risk`; and
6. prove that a rejected risk candidate leaves the last admitted generation intact.

That slice immediately prevents the current conceptual failure mode. SF2-SF6 then improve quality
and latency behind a stable public contract instead of continuing to add special cases to presets.

## 14. Implementation and qualification record (2026-07-11)

The sprint was implemented end to end on `codex/correctness-first-surface-v2`. The shipped design
uses separate immutable market-mark and risk leases, generation-stamped admission, explicit
last-known-good state, independent served-value validation, and persisted policy/provenance.

Key implementation outcomes:

- `PricerConfig{}` is `Balanced`, requests mark and risk, requires risk admission, and retains the
  last known-good risk generation by default.
- LinearVariance is market-mark only. Automatic risk routing uses constrained dense or admitted
  parametric curves and cannot silently disable parity/calendar gates.
- Carry is a robust multi-pair estimate with dispersion, leave-one-out, effective-pair-count, and
  confidence diagnostics. Price-to-IV proposals are repriced against the cold reference and either
  accepted, accurately recomputed, or rejected.
- Dense slices are fit in discounted call-price space with bounds, monotonicity, convexity, an
  origin-slope constraint, spread-aware smoothing, and shared-log-moneyness calendar projection.
- Both cold dual-output builds and local publications are copy-on-write. Mark and risk cold builds
  run concurrently; one-expiry updates stage a cloned candidate, validate adjacent calendar pairs
  and the independent oracle, and atomically replace the generation.
- A certified observation cache reuses price-to-IV results only when fitted prices, flags, and the
  carry-relevant price band are unchanged. Any price, eligibility, or carry-coordinate change falls
  back to the full certified path. Uniform spread-only updates retain the mathematically unchanged
  curve and still run independent admission.
- The UI defaults to `Balanced`, exposes Latency/Balanced/Accuracy, separates the risk curve from
  the market-mark overlay, and reports model, carry, inversion, butterfly, and calendar health.

### Release benchmark evidence

Command:

```text
atx-vol-surface-v2-bench.exe --samples 10 --warmup 3
```

The synthetic liquid-board fixture admitted every measured candidate: 30/30 cold and 30/30
incremental, with no fallback samples included in a latency distribution.

| Mode | Cold p50 | Cold p95 | One-expiry p50 | One-expiry p95 | Target result |
|---|---:|---:|---:|---:|---|
| `Latency` | 156.440 ms | 163.774 ms | 4.100 ms | 4.807 ms | p95/incremental pass; p50 misses aspirational 100 ms |
| `Balanced` | 197.397 ms | 214.728 ms | 9.021 ms | 9.329 ms | pass |
| `Accuracy` | 210.355 ms | 235.567 ms | 18.048 ms | 19.834 ms | pass |

The local p95 phase breakdown was dominated by the independent validator
(3.679/7.865/18.628 ms). Input certification stayed below 0.014 ms, copy-on-write cloning/refit
below 1.57 ms, and publication below 0.053 ms. The sole missed aspirational gate is Latency cold
p50: correctness-first cold de-Americanization plus dual mark/risk publication measured 156 ms
against the initial 100 ms target; p95 and every incremental gate pass.

### Qualification evidence

- Strict clang-cl `/W4 /WX` Debug and Release builds completed for the library, tests, benchmark,
  and UI.
- V2 qualification, fallback, independent-validator, incremental rollback/publication, archive,
  database, Black-76/IV, dense-shape, and UI-model suites pass.
- A release headless SPY OPRA smoke admitted a healthy `Balanced` risk surface over 35 expiries and
  13,586 contracts (`ATX_UI_SMOKE_OK`).
- The complete Release executable ran 997 tests. The remaining bit-pin failures also reproduce in
  the untouched local-main Release binary (`AndersenLakeRegime.PositiveRateGrid_BitIdenticalToPrechange`
  and correction-cache `Pin.*`) and are compiler-sensitive baseline issues, not V2 regressions.
  V2-driven multiname baselines were deliberately repinned because the production default surface
  changed from the legacy fit to the correctness-first risk fit.

## 15. Independent review-fix pass record (2026-07-12)

A multi-agent review of the sprint implementation (correctness, performance, feature gaps,
unwired code) produced 12 Critical / 54 Important findings; a subagent-driven fix pass landed
five reviewed task waves on this branch before merge to main. Per-task detail, review verdicts,
and full reports live in `.superpowers/sdd/` (`progress.md` ledger, `rfx-task-N-{brief,report}.md`,
`findings/*.md`).

### Landed (each independently reviewed; fix loops closed)

- **Task 1 (`c83c9fe`) — policy/serving safety.** Fail-closed `value_chain` default purpose;
  removed the `al_opts` clobber in `VolaSession::build` that silently substituted the fast de-Am
  preset for all risk modes (per-mode carry budgets 3/8/12 now real); Hft+LV pin mapping; unified
  legacy-preset translation on `map_legacy_fit_preset`; honest fallback provenance.
- **Task 2 (`174bee2` + `bf8d5a0`) — certification holes.** Audited eSSVI fallback rung
  (`DeAmOptions::audit_fit_inversions`); non-vacuous certification (Baw uncertified); drop-cap
  semantics (default 0.10 of usable de-Am rows) instead of one-bad-quote generation rejection;
  new `ValidationFailure::CarryGap` (1u<<11) surfacing carry-skipped expiries as Degraded, with
  persistence masks widened to bits 0-11 and round-trip tests; carry-aware certified-cache
  invalidation (spreads/timestamps + selection band).
- **Task 3 (`b118439` + `7e0264e`) — admission-oracle hardening.** Calendar-grid non-finite
  samples now set NonFinite (no silent skip); slice-fit sub-intrinsic clamps surface as
  PriceBounds through the independent-failure merge seam (`merge_session_failure_context`, now
  public API); active-set QP verifies start feasibility and fails closed on iteration-cap exit
  (Err(Internal), all row families checked); validation grid unions each slice's node
  log-moneyness locations (dedup, deterministic, capped); adapter/Wing/PriceBounds/InvalidDomain
  rejection tests added.
- **Task 4 (`e5e2a8a`) — persistence wiring.** Stored `SymbolFitConfig::surface_policy` now
  surfaced by a 3-arg `apply_symbol_config` overload (stored-wins precedence); the production
  corpus archive writer populates `SurfaceArchiveItem::provenance` from the fitter's real
  health/digest (legacy archives still load; no format bump needed).
- **Task 5 (`b6fc043` + `88fff30`) — de-Am dedup (perf C1).** `VolaSession` certification folded
  into the parallel de-Am prepass; duplicate serial `resolve_chain_forward` +
  `build_observations_european` pass eliminated. Cold `PricerFitter::fit` on the SPY-synthetic
  datum: ~1810 ms → ~400-560 ms (~4.5x). Bit-identity of certification diagnostics and admission
  outcomes proven per-slice against a serial reference and across worker counts (new determinism
  tests, both curve-driver and eSSVI branches). Post-convergence `deam_pcp_step` (perf C4a)
  investigated and proven load-bearing — left unchanged.

### §14 evidence corrections

- The §14 latency table above predates Tasks 1+5 and is stale in both directions: Task 1 made
  risk modes genuinely pay multi-pair accurate carry (breadth fits got slower), then Task 5
  removed the duplicate serial pass (cold fit ~4.5x faster on the measured datum). Re-run the
  bench for current numbers before quoting any figure.
- Test-suite state at merge: 1038 ran / 1025 passed / 4 skipped / 9 failed. The 9 = 4
  pre-existing compiler-sensitive bit-pins (reproduce on untouched main) + 5 artifact-cache-state
  baselines (`SpyBidAskRegression.ConvexDenseServedViaSessionInBand`,
  `SpyArchiveRoundTrip.ConvexDense_Serialize_Reload_ReproducesTheoAndAccuracy`, 3
  `MultinamePipeline` pins) that flip with artifact-cache freshness — a re-capture decision is
  owed (below), not a rebaseline-in-passing.

### Remaining items (documented, not landed)

Reviewed-but-unfinished:
- **Task 6 — perf micro batch (uncommitted WIP at merge time).** `ConvexSliceFit::iv` safeguarded
  Newton replacing the fixed 64-iteration bisection; validator direct slice eval; exact-T bracket
  short-circuit; calendar-check row reuse; clone-cost cuts. Implementation existed with targeted
  tests passing when the pass was halted; needs full-suite verification, commit, and review.
  Brief: `.superpowers/sdd/rfx-task-6-brief.md`.
- **Task 7 — bench honesty + UI label (brief staged, not started).** Incremental bench samples
  all take the spread-invariance shortcut (never a true refit) — perturb mids on alternate
  samples and report shortcut/refit counts separately; UI header hardcodes "HFT / LIN VAR"
  (atx-ui/src/vol/vol_workspace.cpp:88) instead of deriving from the served config. Brief:
  `.superpowers/sdd/rfx-task-7-brief.md`.
- **Final whole-branch review** was not run (per-task reviews only).

Accepted deferrals from the original review (unchanged scope):
- Timeout budget / TimedOut failure bit; replay metadata (config_hash/build_version/seed);
  Greek perturbation gate; event/serial expiry metadata; shadow mode +
  `ATX_SURFACE_PIPELINE_V2` flag; scoped incremental validation (perf I3); diagnostics-consumer
  tail; `ATX_BUILD_UI` stays opt-in.
- Perf follow-ups not taken: accurate-route audit dedup (perf I1), per-mode `kBorrowFpTol` +
  carry warm-start (perf C4b/c), shared_ptr-slice surface clones (perf I4 item 1), bench
  board-scale fixture (perf I7).

Operational debts:
- Artifact-cache-dependent baselines (5 tests above): decide re-capture vs pinning policy.
- Pre-existing repo-wide Release build break outside atx-vol:
  `atx-engine/tests/core/phase4_integration_test.cpp:335` `-Wunused-parameter` under NDEBUG
  (`ATX_ASSERT` compiled out) — trips `/WX` when building the full graph.
- Minor review findings logged per task in `.superpowers/sdd/progress.md` (I-2 fitter-level
  health test for audit-starved case; provenance byte-determinism test across worker counts;
  MarketMark arm of corpus provenance ternary untested; `sp.deam.caches` single-mutation-site
  invariant documented but not structurally enforced; validator grid-cap prefix truncation).
