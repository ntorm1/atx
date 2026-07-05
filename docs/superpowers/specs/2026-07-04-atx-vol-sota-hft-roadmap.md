# atx-vol → SOTA HFT / market-maker American-equity analytics: roadmap

**Goal (session /goal).** Build `atx-vol` out from the Vola-parity skeleton into a
state-of-the-art, high-performance C++ options pricing + analytics library usable
by market makers and HFT desks: better library design, data structures, fit
configuration, fit/pricing performance and accuracy, and a better API — pushing
to parity and above Vola Dynamics for **equity American** options.

This roadmap decomposes that open-ended goal into focused, independently-shippable
sprints, each with a hard acceptance gate. It is a living document: sprints are
re-ordered as measurements land. Every code change holds the standing quality
gate (mean fair-value-in-bid-ask ≈98.5%, reduced χ²≈0.21, vol-RMSE≈0.019 on the
XOM OPRA slice) and the 557-test suite green, `/W4 /permissive- /WX` clean.

---

## Where we are (audit, 2026-07-04)

`atx-vol` is already a mature port: 40 modules (Black-76 + Greeks, implied vol,
eSSVI/SVI/C8/CStar calibrators, Andersen-Lake + BAW American pricers, correction
cache, curves, universe, portfolio/risk, projection, surface archive, de-Am +
borrow, parity harness, `VolaSession` facade, real OPRA loader). Cold single-
thread whole-surface fit ≈0.36 s (≈25× off the 9.0 s baseline); cached query
≈6.6 µs (≈14× the cold AL query). SIMD/AVX2 of the AL kernel was investigated and
is a measured negative result in the clang-cl toolchain (xsimd 6.6× slower; SVML
unavailable).

**What a market-maker / HFT desk needs that is missing or weak** (the gap list
this roadmap attacks):

1. **Calendar-arbitrage-free surfaces at held quality** — Vola's headline. The
   raw independent-per-slice fit crosses in total variance; there is machinery to
   *check* it but the *repair* was never wired, and the naive projection wrecks
   quality (see Sprint 1 finding). **Accuracy / parity gap.**
2. **Coherent fit configuration** — knobs are scattered across `DeAmOptions`,
   `CalibOpts`, `AlOpts`, `band_k`, cache flags, per-call tols. No named presets
   (fast / accurate / robust / HFT), no single config object. **Config / API gap.**
3. **One ergonomic entry point** — 40 headers, no umbrella include, no
   single "build a surface from a chain and query it" front door beyond
   `VolaSession`. **API gap.**
4. **Low-latency book/quote engine** — a SoA quote book, strike-ladder batch
   pricing, and nanosecond-scale per-op latency benchmarks. `batch.hpp` exists
   but is scalar-loop-backed and unused by the pipeline. **Data-structures / perf gap.**
5. **Tick-to-quote incremental update** — MMs reprice on every tick without a full
   refit. No incremental surface-update path exists. **Perf / latency gap.**

---

## Sprint 1 — Calendar-arbitrage repair: wiring + measured verdict  ✅ (this session)

**Shipped (non-regressing infrastructure):**
- `run_surface_parity` refactored to **fit all slices → (optional) repair → score
  parity off the FINAL surface**. Deferring the per-expiry scoring out of the fit
  loop is a correctness improvement in its own right: the parity number now
  reflects the surface the caller receives, so any post-assembly transform is
  scored honestly. Default path is byte-identical (proven by a bit-identical
  test on the clean synthetic panel).
- `CalendarRepair {None, MonotoneFit, Project}` config on `SurfaceParityInputs` +
  `SessionInputs.calendar_repair`; `n_calendar_viol_pre` on the report /
  `SessionDiagnostics`. Benchmark prints a raw→MonotoneFit→Project comparison and
  a crossing localization by |k| window.
- `essvi_fit_slice` gains an optional `theta_floor` (default 0 = byte-identical):
  the calendar-monotone seam, mirroring `essvi_calib_surface_sequential`'s
  internal floor. `MonotoneFit` threads the previous slice's theta through it so
  the ATM term structure is theta-monotone by construction.
- Tests: `SurfaceParity.CalendarRepair_HoldsQualityAndDefersScoring` (no-op /
  deferred-scoring invariants), `EssviFitSlice.ThetaFloor_RaisesAtmTotalVariance`
  (the seam), plus the pre-existing `ArbProjectCalendarEssvi.RestoresMonotonicity`.

**Measured verdict (XOM OPRA slice):**
- The 18-slice surface has **55 calendar crossings**, localized by |k| window:
  **11 within |k|≤0.3** (near-money), 26 ≤0.5, 40 ≤1.0, 55 ≤3.0. So crossings are
  NOT only deep-wing — there is genuine near-money non-monotonicity between
  adjacent independently-fit slices.
- **ATM (k=0) theta is already monotone** on this slice, so `MonotoneFit` is a
  no-op here (quality bit-identical: 98.5%/98.5%, χ² 0.207/0.207). It remains the
  correct cheap guarantee where the term structure DOES invert (event months).
- `Project` (post-hoc `arb_project_calendar_essvi` θ-bump) makes it strictly
  arb-free (NO→yes) but **collapses fit quality** — mean in-bid-ask **98.5% →
  20.4%**, reduced χ² **0.21 → 749**, RMSE **0.019 → 0.25** — because θ == w(0)
  and the far-wing bump lifts the whole slice off market.

**Conclusion.** The crossings are off-ATM (near-money + wing), driven by the φ/ρ
smile-shape term structure — neither a θ-floor nor a θ-bump addresses them without
destroying the fit. The correct, quality-preserving fix is a **φ/ρ-coupled
calendar-floor calibration**: fit each slice subject to `w_i(k) ≥ w_{i-1}(k)` over
the data k-range, giving up only the minimal fit error needed to stay monotone.
**This is now implemented** as `MonotoneFit` (see the flagship section below) —
Sprint 1 grew into the flagship once the mechanism proved tractable as an
active-set wrapper. Sprint 1 also ships the scoring/telemetry/seam scaffolding and
the strict-but-costly `Project` mode at the other end of the trade-off.

---

## Sprint 2 — Fit configuration + presets (config / API)  ✅ (this session)

**Problem.** Tuning knobs were scattered and unnamed; the "fast" logic was buried
inside `VolaSession::build`. A caller could not say "fit this fast/robustly"
without hand-assembling the DeAmOptions / CalibOpts / cache / repair knobs.

**Delivered.** `FitPreset {Fast, Accurate, Robust, Hft}` + `apply_fit_preset(
SessionInputs&, FitPreset)` (sets only the fit-policy fields, preserves the market
snapshot) + `make_session_inputs(preset, S, r, now)` convenience (session.hpp).
- **Fast** — al_fast_opts, iv_tol 1e-5, n_atm 1, cache on (the historical default).
- **Accurate** — al_default_opts, iv_tol 1e-7, n_atm 3, cache on.
- **Robust** (market-maker default) — Accurate + `CalendarRepair::MonotoneFit`:
  calendar-arb-free near-money at held quality.
- **Hft** — Fast + MonotoneFit; a fast arb-free build feeding the cached query
  path (diverges from Robust as Sprint 4's SoA book lands).

Tests: `FitPreset.PopulatesPolicyFieldsPerPreset` (field-level, all four) and
`FitPreset.RobustPresetBuildsSessionOnKnownPanel` (end-to-end build). Follow-on:
pre-fit quote filters (arb.hpp FilterOpts) into Robust; a per-preset cold-ms/quality
table in the README.

## Sprint 3 — Ergonomic API surface (API / design)  ✅ (this session)

**Delivered.** Umbrella header `atx/vol/vol.hpp` — one include for the whole
public surface, grouped by role, with a 10-line quickstart (load chain →
`make_session_inputs(FitPreset::Robust, …)` → `VolaSession` → query iv /
fair_value / greeks / diagnostics). Locked by `vol_umbrella_test.cpp` (a TU that
includes ONLY the umbrella and exercises symbols from four groups).

**Design fix surfaced by the umbrella.** Aggregating the public headers exposed a
latent **ODR conflict**: `PricingRoute` was defined identically in BOTH
`portfolio.hpp` (per-lane diagnostic) and `profile.hpp` (config) in namespace
`atx::vol` — a hard compile error for any TU that included both. Hoisted the
single definition into `types.hpp` (the shared vocabulary header); both now
reference it. All 562 tests green; the umbrella TU proves the surface co-includes
cleanly. Follow-on: name the coordinate conventions once in the facade; expose a
single surface-introspection accessor (per-slice params + arb status + provenance).

## Sprint 4 — Strike-ladder pricing + latency benchmark (perf / data structures)  ✅ (this session; honest negative on the perf premise)

**Delivered.** `VolaSession::fair_value_ladder(T, strikes, sides, out)` and
`greeks_ladder(...)` — reprice a whole expiry's SoA strike ladder in one call,
resolving the per-expiry context (T-bracket forward/carry, cache pointers) once
and reusing it across every strike. Bit-identical to the scalar `fair_value`/
`greeks` (test `VolaSession.FairValueLadder_MatchesScalarAndHandlesBadStrikes`);
per-strike NaN isolation (a bad quote does not sink the chain reprice); structural
errors (bad T, length mismatch) return InvalidArgument. `opra_parity_bench` gains
a ns/option ladder-vs-loop latency section.

**Measured — the perf premise did NOT hold, documented honestly.** Ladder vs.
per-option loop: **~1.01× (≈6.2 µs/option either way)**. The shared-context
amortization is negligible against the per-strike cached pricer (Black-76 +
Chebyshev correction eval), which dominates. So the ladder is an **ergonomics +
robustness** primitive (one-call chain reprice, SoA output, per-strike NaN), not
a throughput win. A genuine latency win requires a cheaper per-strike pricer or
vector transcendentals — the latter unavailable under clang-cl (xsimd 6.6× slower,
SVML MSVC-only; measured last session). The SoA `QuoteBook` (bids/asks/flags) and
batch IV-inversion kernels remain candidates but face the same per-op wall.

## Sprint 5 — Calendar-floor-constrained fit (accuracy / parity — the flagship)  ✅ near-money (this session)

**Delivered.** `CalendarRepair::MonotoneFit` fits expiries short→long with a
calendar floor `w_i(k) ≥ w_{i-1}(k)` enforced over the near-money region, coupling
**θ, φ, and ρ** via an **active set of one-sided floor pseudo-observations** around
the existing cube-space LM: after a normal fit, sample the previous slice over the
grid, add heavily-weighted pseudo-obs (target `w = w_{i-1}(k)`, weight 300× the max
base weight) at each currently-violating k, refit (with the wing-residual layer
disabled so a heavy pseudo-obs is not absorbed by it), iterate ≤8 passes until no
violation remains. The fit lifts only where it must — minimal fit-error surrender,
unlike a global θ-bump.

- **Result (XOM OPRA slice):** near-money window |k|≤0.6 **26 → 0** violations at
  **held quality** — fair-value-in-bid-ask **98.5% → 98.5%**, reduced χ²
  **0.207 → 0.209**, RMSE **0.0190 → 0.0191** (full |k|≤3 also improved 55 → 46).
  Contrast `Project`: strict |k|≤3 arb-free but quality destroyed (98.5% → 20.4%).
- **Seam:** `essvi_fit_slice` gained an optional `theta_floor`; the active set lives
  in `run_surface_parity`'s `fit_slice_calendar_floored`. Tests:
  `SurfaceParity.MonotoneFit_ClearsNearMoneyCrossingAtHeldQuality` (engineered
  crossing panel), `EssviFitSlice.ThetaFloor_RaisesAtmTotalVariance`.

**Remaining (own sub-sprint).** Deep-wing (|k| out to ~3) strict no-arb without a
θ-bump needs a **φ-slope (wing) term-structure constraint** — the eSSVI asymptotic
slope θφ(1±ρ) must be monotone in T. Economically low-value (no quotes ~20σ out),
so deferred; `Project` covers callers who need the strict full-grid guarantee and
accept the fit cost. Also candidate: promote `MonotoneFit` to the session default /
`FitConfig::robust()` preset (Sprint 2) once query-path parity is confirmed.

## Sprint 6 (candidate) — Tick-to-quote incremental update

Re-price / locally re-fit on a single quote change without a full surface rebuild
(warm-started single-slice re-solve + calendar-floor re-check against neighbours).
The genuine HFT latency path. Scoped after Sprints 4–5 land the SoA book and the
constrained fit they build on.

---

## Cross-cutting invariants (every sprint)
- Standing quality gate held on the real XOM OPRA slice; 557+ tests green; zero
  `/W4 /permissive- /WX` warnings.
- No paid Databento pulls (cached slice only). No commits unless the user asks.
- Every performance claim measured with the throttle-canceling A/B discipline
  (interleaved in one binary), never single-shot wall-clock.
- Negative results documented as such (SIMD/AVX2; naive calendar projection).
