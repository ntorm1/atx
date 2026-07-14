# atx-vol → SOTA American-Equity Options Analytics Engine — Work Module

> **For agentic workers:** This is a MULTI-SPRINT WORK MODULE, not a single plan. Each
> sprint below is a self-contained unit of work sized for **superpowers:subagent-driven-development**.
> Execute one sprint at a time in a loop: (1) expand the sprint's task list into a
> bite-sized TDD plan via **superpowers:writing-plans** if fuller step-level detail is
> wanted, or dispatch its tasks directly (they are already task-sized with interfaces +
> acceptance gates); (2) run the SDD loop (implementer → task review → fix → next);
> (3) close the sprint against its **hard acceptance gate**; (4) advance. Sprint A is
> already fully specced in its own plan file and is execution-ready NOW.

**Mission (locked, inherited from `docs/superpowers/specs/2026-07-04-atx-vol-sota-hft-roadmap.md`):**
Push `atx-vol` to a state-of-the-art, high-performance C++ options pricing + analytics
library for **market makers / HFT desks on American & index EQUITY options** — parity
and above Vola Dynamics. Better library design, data structures, fit configuration,
fit/pricing performance and accuracy, and API.

**Scope boundary (explicit — prevents scope creep):** This engine's edge is *American /
index equity vanilla* analytics done better than anyone. The following are OUT of scope
for this module unless a later spec deliberately reopens them: exotic payoffs (barrier /
Asian / digital / lookback / basket), stochastic-vol models (Heston / SABR / rough vol),
Monte-Carlo / LSMC pricing *as a production path* (LSMC appears only as a cross-check
oracle in Sprint H), jump-diffusion / Lévy models, and GPU. Adding those would dilute the
mission, not advance it. Every sprint below deepens the American-equity core.

**Tech stack:** C++20; `atx::core::linalg` (Eigen 3 via FetchContent) `MatX`/`VecX`,
`solve`/`solve_spd`; GoogleTest (`gtest_discover_tests`, label `atx_vol`); CMake +
clang-cl (Ninja presets). Error vocabulary: `atx::core::Result`/`Status`, `Ok`/`Err`,
`ATX_TRY`. Namespace `atx::vol`.

---

## Global Constraints (bind EVERY task in EVERY sprint)

- **Language / warnings:** C++20; warnings-as-errors under clang-cl (`atx_warnings`, `/W4
  /permissive- /WX`). Zero warnings. Follow the house style in `.agents/cpp/agent.md` and
  the existing file/idiom conventions exactly. No new external dependencies without an
  explicit decision in the sprint (the repo pulls deps via vcpkg + FetchContent only).
- **Namespace / vocabulary:** `atx::vol`; `Result`/`Status`/`Ok`/`Err`/`ATX_TRY`.
- **No-regression / bit-identity:** every change that touches a served/fitted path must be
  **slack on the paths it does not intend to change** — i.e. byte-identical output where
  no new behavior is requested (the standing "bit-identical across thread counts" and
  "default path unchanged" gates). New behavior is opt-in behind a flag/preset until a
  deliberate default flip.
- **Standing quality gates (must hold at every sprint close):**
  - XOM real OPRA slice: mean fair-value-in-bid-ask ≈ 98.5%, reduced χ² ≈ 0.21, vol-RMSE ≈ 0.019.
  - SPY real OPRA: median vega-weighted vol-RMSE ≤ 0.015 (~1 vol pt); ≥ 75% of liquid
    slices within 2 vol pts (`spy_real_test.cpp`, `spy_bidask_regression_test.cpp`).
  - `vola_parity`: `frac_fv_within_bidask ≥ 0.95`, `rmse_mid_vol ≤ 5e-3`,
    `fit_chi2_reduced ≤ 2.0`.
  - Full `atx_vol` test gate green (see command below); `/WX` clean.
- **Test gate command (fast, parallel — from the test overhaul, commit bd269f8):**
  `ctest --test-dir build -L atx_vol -j16 --output-on-failure` (~2 min; benches skip by
  default, opt in with `ATX_VOL_BENCH=1`). Focused: `./build/bin/atx-vol-tests.exe
  --gtest_filter='<Suite>.<Case>'`. Build: `cmake --build build --target atx-vol-tests -j`.
- **Performance claims:** measured with the interleaved-A/B, throttle-cancelling discipline
  in one binary (never single-shot wall clock). Negative results are shipped AS negative
  results (documented, not papered over) — the roadmap has several honest negatives already.
- **Git:** branch `feat/atx-vol-carry-deam` (commit directly OK). **Explicit-path staging
  only — never `git add -A`.** Stage only the files a task touches. Data dirs
  (`data/**`) stay gitignored/untracked. End commit messages with
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- **Repo layout note:** repo root is `C:\atx`; atx-vol sources live under `atx-vol/`
  (headers `atx-vol/include/atx/vol/`, src `atx-vol/src/`, tests `atx-vol/tests/`). Sprint
  file paths below omit the `atx-vol/` prefix — prepend it.

---

## Current state (audit 2026-07-07, post test-overhaul)

Mature port: 59 public headers. Served surfaces: **eSSVI / SVI / ConvexDense** (unified
`VolCurveKind`). American pricing: Andersen-Lake (10–11 sig figs) + BAW, Chebyshev
correction-cache hot path, de-Americanization, escrowed-forward dividends. Calibration:
per-family LM/IRLS/active-set + convex-QP dense fit, pool fan-out, OOS curve selector.
Data: Databento OPRA cbbo-1m loader, corpus builder, ATXVSA v3 archive. Backtest: engine +
frictions ledger + strategy DSL + tearsheet. Risk: canonical `PortfolioPricer` (American
mark + FD greeks + Taylor pnl-explain); legacy scenario/shock engine deprecated. Test gate
parallelized (12 min → ~2 min), benches gated, `681` cases.

**Already shipped this session (do not redo):** Sprint A Tasks 1–2 (calendar checker on
`CurveSurface`; honest calendar reporting in `session.cpp` — SPY dense board measures **372**
calendar violations pre-enforcement). Test-suite overhaul (parallel gate + bench gating +
CPU-hammer trims), commit `bd269f8`.

---

# SPRINT LADDER

Dependency order. Each sprint is independently shippable and holds all Global Constraints.

| # | Sprint | Theme | Depends on | Status |
|---|---|---|---|---|
| A | Served-surface no-arb integrity | accuracy / parity | — | **IN FLIGHT** (Tasks 1–2 done) |
| B | Curve-family breadth (C8 + CStar into the unified path) | accuracy / coverage | A | chartered |
| C | Cached/analytic Greeks for ALL curve kinds | perf / risk | A | chartered |
| D | Unified risk engine (scenario/shock + vol-attribution onto canonical path) | risk / API | C | chartered |
| E | Deep-wing strict no-arb (φ-slope term-structure) + Lee-consistent wings | accuracy / parity | A | chartered |
| F | Native discrete-dividend American PDE + multi-curve OIS/SOFR bootstrap | accuracy | A | chartered |
| G | Low-latency SoA quote book + book-level incremental reprice + perf-regression CI gate | perf / data-structures | C | chartered |
| H | Independent-method cross-validation (PDE/LSMC oracle) + property-based & fuzz verification | robustness / verification | B,C,F | chartered |

---

## Sprint A — Served-surface no-arb integrity  **(IN FLIGHT — execution-ready)**

**Goal:** make the served ConvexDense/SVI `CurveSurface` provably static-arbitrage-free
(butterfly **and** calendar), with Lee-controlled wings and an optional bid/ask-band objective.

**SOTA rationale:** calendar-arb-free surfaces at held quality are Vola's headline. The
dense/SVI served path currently only *measures* calendar violations (372 on SPY) — this
sprint makes it arb-free *by construction*, like butterfly.

**Full detailed plan (bite-sized TDD, code included):**
`docs/superpowers/plans/2026-07-07-atx-vol-surface-noarb-integrity.md` — execute it via SDD.

**Remaining tasks (Tasks 1–2 + test overhaul already done):**
- **A3.** Augment the dense QP `Gx≥0 → Gx≥h`; implement the deferred `bound_slope_below`.
- **A4.** Per-node calendar floor in `fit_convex_slice` (`w_prev(k)` callback → `g_j ≥ black76_call(F,u_j,T,√(w_prev/T),df)`).
- **A5.** Sequential calendar-enforcing driver in `fit_curve_surface` (ascending-T, feed each slice's `w(·)` as the next floor) — **by-construction gate: SPY dense board 372 → 0 calendar violations**.
- **A6.** Lee wing extrapolation on the dense slice (linear-in-k total-variance tails, Lee-capped, butterfly-clean).
- **A7.** Band-aware (interval) objective behind a flag (default `Mid` unchanged).
- **A8.** End-to-end verification: corpus rebuild, backtest sanity (≈ pre-change total_return), full gate green, `calendar_arb_free=true`.

**Acceptance gate:** `CurveSurfaceNoArb.SpyDenseIsCalendarArbFree` passes (0 calendar
violations by construction); butterfly stays clean; standing quality gates bit-identical
where no arb existed; full `atx_vol` gate green.

---

## Sprint B — Curve-family breadth: C8 + CStar into the unified path

**Goal:** make the already-built richer curve families **C8** (SVI-JW backbone + 3
compact-support bumps, 8 DoF, admits negative ATM curvature) and **CStar/C16M** (modal
family, nested C5/C8/C12/C16 tiers) reachable through the unified `VolCurveKind` /
`IVolCurve` / session / archive / `PricerFitter` / curve-selector path. Today they are
standalone-only ("deferred (partial/unported)").

**SOTA rationale:** eSSVI/SVI/ConvexDense cannot fit every board well — event-driven
smiles with negative ATM curvature or complex wing shape need the higher-DoF families.
Wiring them + letting the OOS selector choose per board is a direct accuracy win on hard
names. A written-but-unexecuted plan exists
(`docs/superpowers/plans/2026-07-05-atx-vol-breadth-c8-activation.md`) — fold it in.

**Depends on:** A (served surface seam stable).

**Files:** `include/atx/vol/vol_curve.hpp`/`src/vol_curve.cpp` (extend `VolCurveKind`,
`IVolCurve` adapters), `src/curve_fit.cpp` (dispatch), `include/atx/vol/c8.hpp`/`c8_calib.hpp`,
`cstar.hpp`/`cstar_calib.hpp` (adapter wrappers to `IVolCurve`), `curve_selector.hpp`/`.cpp`
(selector menu), `surface_archive.hpp`/`.cpp` (serialize new kinds), `profile.hpp`
(profile-keyed family menu), tests `curve_test.cpp`, `c8_test.cpp`, `cstar_test.cpp`,
`curve_selector`/new `curve_breadth_test.cpp`.

**Tasks (task-sized, TDD; each ends independently testable):**
- **B1. `IVolCurve` adapter for C8.** Wrap `C8Curve` as an `IVolCurve` (implement `w(k)`,
  `iv(k)`, `T()`, `call_price`, arb hooks). Interface: `class C8DenseCurve : public IVolCurve`.
  Gate: a C8 curve round-trips through `IVolCurve::w/iv` bit-identically to the standalone.
- **B2. `IVolCurve` adapter for CStar.** Same for `CStarCurve` (all tiers). Gate: parity vs standalone.
- **B3. Extend `VolCurveKind` + `fit_slice_curve` dispatch.** Add `VolCurveKind::{C8,CStar}`;
  route in `curve_fit.cpp` to `c8_fit_*`/`cstar_fit_*`, assemble into `CurveSurface`. Gate:
  `fit_curve_surface` with a C8/CStar config produces a servable surface; existing kinds unchanged (bit-identical).
- **B4. Archive serialization for C8/CStar.** ATXVSA v3 blob sections for the new params;
  schema-hash bump guarded. Gate: serialize→reload reproduces theo bit-identically (mirror `SpyArchiveRoundTrip`).
- **B5. Curve-selector menu + profile keying.** Add C8/CStar to `curve_selector` OOS menu
  (leave-every-other-strike-out) with parsimony tie-break; profile-keyed family shortlist per `ProfileKind`.
  Gate: on an engineered negative-ATM-curvature board the selector prefers C8 over eSSVI at better held-out in-band.
- **B6. Coverage scoreboard + real-board proof.** A diagnostic (example/test) reporting
  per-family in-band/RMSE across the SPY + XOM boards; document which board each family wins.

**Acceptance gate:** C8 & CStar are servable/selectable/archivable through the unified path;
selector picks the best family per board; **no standing quality-gate regression** (existing
eSSVI/SVI/ConvexDense fits bit-identical); ≥1 board demonstrably improved by a newly-wired family.

---

## Sprint C — Cached / analytic Greeks for ALL curve kinds

**Goal:** extend the near-analytic (correction-cache-accelerated) Greeks path — today
available ONLY for the eSSVI curve kind — to `ConvexDense`/`SVI` (and the Sprint-B
families), so portfolio Greeks stop paying the ~17-solve cold American FD stencil per
unique contract on those kinds.

**SOTA rationale:** the canonical `PortfolioPricer` FD-only Greeks are the single biggest
per-contract cost on dense/SVI books. A market maker reprices Greeks continuously; making
the served-surface Greeks near-analytic on every curve kind is an order-of-magnitude
book-repricing win and removes the "eSSVI-only fast path" asymmetry.

**Depends on:** A (served surface stable).

**Files:** `include/atx/vol/correction.hpp`/`src/correction.cpp` (generalize the
American-minus-European Chebyshev correction beyond eSSVI), `include/atx/vol/american.hpp`
(shared chain-rule greek assembly), `session.hpp`/`.cpp` (route dense/SVI greeks through the
cache), `portfolio_pricer.hpp`/`.cpp` (consume), tests `correction_test.cpp`,
`pnl_greeks_consistency_test.cpp`, `portfolio_pricer_test.cpp`, new `dense_greeks_test.cpp`.

**Tasks:**
- **C1. Curve-agnostic correction-cache builder.** Refactor the Chebyshev
  American-minus-European correction to take an `IVolCurve` (not an eSSVI-specific slice), so
  a dense/SVI slice can seed the same B76+tensor hot path. Interface:
  `build_correction_cache(const IVolCurve&, const SliceContext&, CorrectionOpts) -> CorrectionCache`.
  Gate: eSSVI path bit-identical to today; a dense slice builds a valid cache.
- **C2. Analytic Greeks through the dense/SVI cache.** `ConvexSliceFit`/SVI greeks via
  chain rule through the correction cache (∂price/∂S,σ,T,r from cached tensor), matching the
  FD stencil to tolerance. Gate: `bits`-close to `american_greeks_fd` on a synthetic +
  real slice (tol documented, e.g. ≤ 1e-6 abs on delta/gamma/vega), with ≥5× fewer AL solves.
- **C3. Session + PortfolioPricer routing.** `VolaSession::greeks` and
  `PortfolioPricer::price` use the cached Greeks on dense/SVI when a cache is present; cold
  FD fallback otherwise. Gate: `PnlGreeksConsistency.Session_ConvexDense_*` still bit-equal
  (price==fair_value), and portfolio Greeks on a dense book match the FD reference to tol.
- **C4. Latency proof.** Interleaved-A/B bench: cached vs cold-FD dense-book Greeks
  (ns/contract, speedup). Ship the number (honest if it underwhelms, per roadmap discipline).

**Acceptance gate:** dense/SVI served Greeks are cache-accelerated and match FD to a
documented tolerance; measured book-Greeks speedup reported; the eSSVI path and all
bit-equality gates unchanged.

---

## Sprint D — Unified risk engine (scenario/shock + vol-attribution onto the canonical path)

**Goal:** migrate the richer risk analytics that currently live ONLY in the **deprecated**
`portfolio_risk.hpp` — multi-shock scenario chains, the `SurfaceTwist` shock (currently
`NotImplemented`), and the vol-attribution level/skew/curvature/higher-order sub-split —
onto the canonical `PricedSurface`/`PortfolioPricer` path, and retire the legacy module.

**SOTA rationale:** market makers run scenario grids and decompose vol PnL by
level/skew/curvature. Today that lives on a dead code path; the canonical path only has a
Taylor pnl-explain. Consolidating gives one authoritative, fast, tested risk surface.

**Depends on:** C (cached Greeks make scenario grids affordable).

**Files:** `portfolio_pricer.hpp`/`.cpp` (add scenario + attribution APIs),
`portfolio_risk.hpp` (deprecate → thin shim or remove), new `scenario.hpp`/`scenario.cpp`,
tests `portfolio_risk_test.cpp` (retarget to canonical), new `scenario_test.cpp`.

**Tasks:**
- **D1. Canonical scenario engine.** `PortfolioPricer::scenario_pnl(const ScenarioSpec&)`
  over shock chains `{SpotPct, VolAbs, VolRel, RateAbs, TimeAbs}` on `PricedSurface`
  re-marks. Interface: `struct ScenarioSpec { std::vector<Shock> chain; }`,
  `Result<ScenarioFrame> scenario_pnl(...)`. Gate: single-shock scenarios reproduce
  `pnl_explain` deltas; multi-shock composes correctly on a known-truth book.
- **D2. `SurfaceTwist` shock (implement the reserved shock).** A parametric smile
  twist (level/skew/curvature) applied to a `PricedSurface` producing a shifted surface.
  Gate: a pure-level twist == VolAbs shock; a pure-skew twist moves wings antisymmetrically; arb-free post-twist.
- **D3. Vol-attribution sub-split.** Decompose vega PnL into level/skew/curvature/higher-order
  on the canonical path (the legacy sub-split, re-derived on `PricedSurface`). Gate:
  components sum to total vega PnL to tol on a known-truth twist.
- **D4. Retire `portfolio_risk.hpp`.** Move any still-unique capability (by-uid/expiry/group
  aggregation from deprecated `portfolio.hpp` too, if cheap) onto the canonical path; delete
  or shim the deprecated modules. Gate: no test references the deprecated path; full gate green.

**Acceptance gate:** scenario grids + `SurfaceTwist` + vol-attribution run on the canonical
`PortfolioPricer`; deprecated `portfolio_risk.hpp`/`portfolio.hpp` retired or reduced to a
documented shim; risk numbers match known-truth to tol.

---

## Sprint E — Deep-wing strict no-arb (φ-slope term-structure) + Lee-consistent wings

**Goal:** close the no-arb story to the deep wing (|k| out to ~3) **without** the quality
collapse that a naive θ-bump causes, via a **φ-slope (wing) term-structure constraint** —
the eSSVI asymptotic slope θφ(1±ρ) monotone in T — and make the Lee-capped wing tails
(Sprint A6) consistent across eSSVI/SVI/dense so far-strike total variance is arb-free and
continuous everywhere.

**SOTA rationale:** the prior roadmap explicitly deferred deep-wing strict no-arb as its
own sub-sprint ("economically low-value but the last gap"). Completing it makes the surface
*fully* arb-free to |k|→3 at held quality — a genuine parity-and-above claim over the
`Project` mode that destroys fit quality (98.5%→20.4%).

**Depends on:** A (calendar floor + wings machinery).

**Files:** `essvi_calib.hpp`/`.cpp` (φ-slope monotonicity constraint in the LM),
`arb.hpp`/`arb.cpp` (deep-wing calendar check across kinds), `dense_slice.cpp` (wing
consistency), tests `essvi_calib_test.cpp`, `arb_test.cpp`, `surface_parity_test.cpp`.

**Tasks:**
- **E1. φ-slope term-structure constraint in eSSVI LM.** Add a one-sided constraint that the
  wing asymptotic slope θφ(1±ρ) is non-decreasing in T across the surface, as pseudo-obs /
  projection in the cube-space LM (mirroring the near-money calendar floor). Gate: on the XOM
  slice, deep-wing crossings 46 → 0 with mean in-band held ≥ 98% (contrast `Project`'s 20.4%).
- **E2. Deep-wing calendar check across all kinds.** Extend `arb_check_calendar` sampling to
  |k| out to 3 with the Lee-tail evaluation (Sprint A6), for eSSVI/SVI/dense uniformly. Gate:
  the served SPY + XOM surfaces report 0 calendar violations to |k|≤3.
- **E3. Cross-kind wing consistency.** Ensure eSSVI/SVI/dense wing tails share the Lee-cap
  convention so a `CurveSurface` mixing kinds (per-slice family selection, Sprint B) is
  continuous and arb-free at the seams. Gate: a mixed-family surface is butterfly+calendar clean to |k|≤3.

**Acceptance gate:** served surfaces are static-arb-free (butterfly + calendar) to |k|≤3 at
held quality on real XOM + SPY; the `Project` fallback is no longer needed for the strict guarantee.

---

## Sprint F — Native discrete-dividend American PDE + multi-curve OIS/SOFR bootstrap

**Goal:** replace the escrowed-forward dividend approximation with a **native
discrete-cash-dividend American PDE pricer** (Crank-Nicolson with dividend jump conditions),
and replace pillar-interpolation-of-supplied-rates with a real **multi-curve OIS/SOFR
bootstrap** (strip discount + forward curves from futures/swaps), instead of only
interpolating caller-supplied zero-rate pillars.

**SOTA rationale:** single-name American accuracy near ex-div dates is limited by the
escrowed-forward approximation; a native discrete-div PDE is the correct treatment and a
real accuracy gap vs top desks. The rate side currently only interpolates externally-supplied
pillars despite the module calling it "OIS/SOFR bootstrap" — a genuine instrument-stripping
engine closes that gap.

**Depends on:** A. (Independent of B–E; can run in parallel with them in the loop.)

**Files:** new `pde_american.hpp`/`pde_american.cpp` (production CN PDE with jumps — promote
& harden the test-only `oracle_pricer_pde`), `american.hpp` (route a `PdeDiscreteDiv`
method), `dividend.hpp`/`.cpp` (native discrete-div handling), `curve.hpp`/`.cpp`
(bootstrap), new `rate_bootstrap.hpp`/`.cpp`, tests `american_test.cpp`, `dividend_test.cpp`,
`curve_test.cpp`, new `pde_american_test.cpp`, `rate_bootstrap_test.cpp`.

**Tasks:**
- **F1. Harden the CN PDE into a production pricer.** Promote `oracle_pricer_pde` to a
  library pricer with adaptive grid + Rannacher smoothing; validate vs Andersen-Lake on the
  no-dividend / continuous-yield case to ~1e-6. Interface: `pde_american_price(S,K,T,σ,r,q,side,PdeGrid)`.
  Gate: matches AL to ≤1e-6 on the continuous-yield grid; convergence order verified.
- **F2. Discrete-cash-dividend jump conditions.** Add ex-div jump conditions (S → S − D at
  each ex-date) to the PDE. Gate: vs a known-truth discrete-div benchmark (published or
  self-consistent Richardson-extrapolated), the native price beats the escrowed-forward
  approximation's error on a dividend-heavy single-name case; document the improvement.
- **F3. Route `AmericanMethod::PdeDiscreteDiv`.** Wire it as a selectable method in
  `american.hpp`/session for names with discrete divs. Gate: default paths unchanged; the new
  method is opt-in and de-Americanization round-trips through it.
- **F4. Multi-curve bootstrap.** `bootstrap_curve(instruments) -> YieldCurve` stripping
  discount/forward from deposit/future/swap quotes (single-curve first, then OIS-discount +
  projection). Gate: reprices the input instruments to par to tol; a flat-input bootstrap
  equals the current pillar interpolation (bit-identical fallback).

**Acceptance gate:** native discrete-div American PDE available and more accurate than
escrowed-forward on dividend-heavy names (measured); a real rate bootstrap reprices its
inputs to par; all existing pricing gates unchanged (new paths opt-in).

---

## Sprint G — Low-latency SoA quote book + book-level incremental reprice + perf-regression CI gate

**Goal:** land the SoA `QuoteBook` (bids/asks/flags), a book-level tick-to-quote incremental
reprice (extend the per-slice `refit_slice` to a whole-book update loop), and — critically —
an **asserted, archived perf-regression gate** so throughput/latency numbers are CI-tracked,
not prose in the README.

**SOTA rationale:** MMs reprice a whole book on every tick and cannot tolerate silent perf
regressions. The SoA book + book-level incremental update is the genuine HFT re-quote data
structure; the asserted perf gate turns "we measured X once" into a durable contract.

**Depends on:** C (cached Greeks are the per-op cost the book amortizes).

**Files:** new `quote_book.hpp`/`quote_book.cpp` (SoA), `session.hpp`/`.cpp` (book-level
`refit`/`reprice`), `tests/support/bench_gate.hpp` (extend), new `perf_regression_test.cpp`
(asserted, `ATX_VOL_BENCH`-gated with archived baselines), tests `session_test.cpp`,
`chain_test`/`pricer_fitter_test.cpp`.

**Tasks:**
- **G1. SoA `QuoteBook`.** Struct-of-arrays bids/asks/flags/strikes keyed by `ContractId`,
  cache-friendly, with a tick-update entry point. Interface: `class QuoteBook` +
  `update(ContractId, bid, ask, flags)`. Gate: round-trips a board; tick update is O(1) and
  bit-identical to a rebuild's queryable state.
- **G2. Book-level incremental reprice.** Extend `refit_slice` into a
  `refit_dirty(book) -> repriced` that refits only slices whose quotes changed and reprices
  the affected ladder. Gate: a one-slice tick touches only that slice's cost (measured
  structural win, like the ~4250× single-slice refit); full-book output equals a cold rebuild.
- **G3. Asserted perf-regression gate.** `perf_regression_test.cpp` (bench-gated) that
  measures cold-fit ms, cached-query µs, and book-reprice throughput with the interleaved-A/B
  discipline and asserts against archived baselines (a checked-in `perf_baseline.tsv` with a
  tolerance band). Gate: passes at current numbers; a deliberate 2× slowdown makes it fail (self-test).
- **G4. README perf table from the gate.** Regenerate the README perf numbers from the
  asserted baselines so prose and gate agree.

**Acceptance gate:** SoA `QuoteBook` + book-level incremental reprice land with measured
structural wins; a CI-runnable asserted perf gate protects cold-fit / cached-query /
book-reprice numbers; README perf table sourced from the gate.

---

## Sprint H — Independent-method cross-validation oracle + property-based & fuzz verification

**Goal:** add a second *independent* American pricing method as a continuous cross-check
(the hardened PDE from Sprint F, and optionally a small LSMC oracle used ONLY for
verification — not a production path), and add **property-based** tests (put-call bounds,
monotonicity in σ/T, convexity in K, arb-freeness invariants) plus **fuzz** testing of the
calibrators (random-but-valid boards must never crash, never emit an arb, always converge or
cleanly fail).

**SOTA rationale:** the current suite is example-and-threshold based; a SOTA engine needs
method-independent oracles and invariant/fuzz coverage so accuracy and arb-freeness are
*proven properties*, not spot-checked numbers. This hardens every prior sprint's claims.

**Depends on:** B, C, F (the surfaces/greeks/PDE to cross-check).

**Files:** `tests/support/` (LSMC oracle, property helpers, board fuzzers), new
`property_test.cpp`, `fuzz_calib_test.cpp`, `cross_method_test.cpp`; possibly `american.hpp`
(expose a verification hook).

**Tasks:**
- **H1. PDE cross-method gate.** Continuous `cross_method_test` asserting AL vs the Sprint-F
  PDE agree to ~1e-6 across a strike×maturity×vol grid (ITM/ATM/OTM × short/long). Gate: green
  at documented tol; a planted 1e-3 pricing error is caught.
- **H2. LSMC verification oracle (test-only).** A minimal Longstaff-Schwartz American oracle
  in `tests/support/` (NOT a library path) to cross-check AL on cases the PDE is weak
  (high-dividend, long-dated). Gate: AL within LSMC's confidence band on the target cases.
- **H3. Property-based invariants.** Put-call parity bounds, monotonicity of price in σ and
  T, convexity in K, and served-surface butterfly+calendar arb-freeness, asserted over
  randomized valid inputs. Gate: thousands of randomized cases pass; a deliberately broken
  build fails.
- **H4. Calibrator fuzzing.** Random-but-valid boards (varying spread/skew/term/sparsity)
  fed to every calibrator: must never crash, never emit a served arb, always converge or
  return a clean `Err`. Gate: N fuzz iterations clean; findings (if any) filed + fixed.

**Acceptance gate:** method-independent cross-checks + property/fuzz suites run in the gate
(bench-gated where heavy); every prior sprint's accuracy and arb-freeness claim is backed by
an invariant or an independent oracle, not just a threshold.

---

## Running the loop (how to execute this module)

1. **One sprint at a time, in dependency order** (A → then B/C/F can interleave → D after C →
   E after A → G after C → H last). Within a sprint, use **superpowers:subagent-driven-development**:
   fresh implementer per task, task review (spec + quality) after each, broad review at sprint close.
2. **Expand-then-execute (optional):** for a sprint wanting fuller step-level code, run
   **superpowers:writing-plans** to expand its task list into a bite-sized TDD plan file at
   `docs/superpowers/plans/YYYY-MM-DD-atx-vol-sprint-<x>.md` before dispatching. The task
   lists above are already sized for direct SDD dispatch with interfaces + gates.
3. **Every task** holds the Global Constraints; **every sprint** closes only against its hard
   acceptance gate with the full `atx_vol` gate green and standing quality gates intact.
4. **Progress ledger:** track in `.superpowers/sdd/progress.md` (per the SDD skill) so the
   loop survives compaction — record each task `complete (commits base..head, review clean)`.
5. **Honest-negative discipline:** if a sprint's perf/accuracy premise doesn't hold when
   measured, ship the negative result documented (the roadmap already has several) — do not
   ship a non-improvement as a win.

## Self-review (module coverage vs the gap analysis)

- **Model/accuracy gaps** → A (calendar arb by construction), B (curve breadth), E (deep-wing
  strict), F (native discrete-div PDE + rate bootstrap). ✓
- **Greeks/risk gaps** → C (cached greeks all kinds), D (unified scenario/attribution, retire deprecated). ✓
- **Perf/data-structure gaps** → C (per-op cost), G (SoA book + incremental + asserted perf gate). ✓
- **Verification gaps** (no fuzz/property/CI-perf) → G3 (perf gate), H (cross-method + property + fuzz). ✓
- **Deliberately OUT (mission boundary):** exotics, stochastic-vol, production MC/LSMC,
  jump-diffusion, GPU — none appear as sprints; LSMC is verification-only in H. ✓
- **Type consistency:** `IVolCurve` adapters (B) feed the correction-cache builder (C1) and the
  scenario re-marks (D); `VolCurveKind::{C8,CStar}` (B3) consistent across archive (B4)/selector
  (B5); `ScenarioSpec`/`Shock`/`SurfaceTwist` consistent across D1–D3; `QuoteBook` (G1) consumed
  by G2; PDE (F1) is the cross-method oracle (H1). ✓
