# atx-vol → QIS-Grade Vega-Flat Dispersion Northstar — Work Module (pf2)

> **For agentic workers:** This is a MULTI-SPRINT WORK MODULE, not a single plan.
> Each sprint is a self-contained unit sized for **superpowers:subagent-driven-development**.
> Execute one sprint at a time in dependency order: (1) optionally expand a sprint's
> task list into a bite-sized TDD plan via **superpowers:writing-plans**; (2) run the
> SDD loop (fresh implementer per task → task review spec+quality → fix → next);
> (3) close the sprint against its **hard acceptance gate**; (4) advance. Hand each
> implementer its task, not this whole file. Track state in
> `.superpowers/sdd/progress.md` so the loop survives compaction.

**Date:** 2026-07-08  **Branch:** `feat/atx-vol-carry-deam`  **Base HEAD:** `5b46b74`

---

## Northstar (locked)

Produce a **backtest of vega-flat equity dispersion that replicates, as exactly as
the public methodology allows, how a bank QIS desk runs the strategy**, driven by
**real Databento OPRA data**. The backtest is the forcing function; shipping it
proves two things the library must have to compete as a SOTA American-equity
options analytics/pricing engine:

1. **Correctness + a robust fitting pipeline** across MANY diverse single-name
   boards (not just SPY/XOM) — thin, one-sided, low-price, hard-borrow, event-month.
2. **High-performance throughput of fitting-surface serialization** at corpus scale
   (many symbols × many dates), with a committed, asserted regression gate.

Everything below is scoped to advance that northstar. Where a task also advances the
parent roadmap (`docs/superpowers/plans/2026-07-07-atx-vol-sota-engine-workmodule.md`,
Sprints A–H), that overlap is noted so the two threads stay coherent.

## Mission boundary (explicit — prevents scope creep)

Dispersion and variance/vol swaps are **equity volatility derivatives** and are IN
scope — the library already ships the model-free variance strip and Carr-Lee vol
strike (`derivatives.hpp`), and dispersion is the northstar. The following stay OUT
unless a later spec reopens them: exotic payoffs; stochastic-vol models (Heston /
SABR / rough vol); Monte-Carlo / LSMC / RV-distribution variance-swap engines *as a
production path* (the `derivatives.hpp` `McQe` / `RvDistribution*` engines stay
`NotImplemented`); jump-diffusion / Lévy; GPU. The dispersion legs are priced
through the **existing** de-Am → arb-free-fit → re-Am → Andersen-Lake spine and the
model-free strip — no new pricing math is required by this module.

---

## Current state (audit 2026-07-08)

The full spine exists, is tested, and is green (685/685 fast gate, ~140s; Sprint A
calendar-no-arb-by-construction landed at `5b46b74`):

- **Fitting:** de-Americanization (`deamer.hpp`), arb-free parametric fit
  (eSSVI / SVI / ConvexDense / C8 / CStar), Andersen-Lake American pricing + Chebyshev
  correction-cache hot path (~21k inv/s/core; whole SPY board reprice 0.40s).
- **Corpus:** `build_corpus` (`corpus.hpp`) fits many `(date, symbol)` boards in
  parallel, auto-selects the curve family per board, writes one `SurfaceArchive`
  per date, emits a deterministic TSV manifest with per-board `oos_in_band`, and one
  failing board never sinks the corpus.
- **Archive:** ATXVSA v3 binary (`surface_archive.hpp`), hardware CRC-32C,
  schema-hash guard, concurrent-read-safe, round-trip bit-identical.
- **Backtest engine:** `run_backtest` (`backtest.hpp`) — `MarketSnapshot` load-once,
  resolve-today → `pnl_explain` forward → move-swap; StrategySpec DSL; `FrictionModel`
  (modeled spread + per-contract cost + hedge slippage); `CashLedger` (financing /
  borrow / shares carry); engine-owned `DeltaToZero` hedge overlay; `TearSheet`
  (standard + vega-scaled); deterministic across thread counts; PnL attribution wired
  to the **American** greeks path and closes to 1e-6.
- **Dispersion:** `build_dispersion_book` (`dispersion.hpp`) — vega-weighted ATM
  straddle dispersion, index vs basket, with the **clean** implied-correlation closed
  form `ρ = (σ_I² − Σŵᵢ²σᵢ²) / ((Σŵᵢσᵢ)² − Σŵᵢ²σᵢ²)`.
- **Vol derivatives (unwired):** `derivatives.hpp` ships the model-free variance-swap
  fair strike (Demeterfi-Derman-Kamal-Zou log-strip), Carr-Lee vol strike, and a
  realized-variance tracker — **but only instantiated for `EssviSurface` / `SviSurface`,
  not the served `PricedSurface` / ConvexDense path, and not wired into the dispersion book.**
- **Profiles (unwired into corpus):** `profile.hpp` ships a 7-bucket underlier
  taxonomy + classifier + per-bucket calib/filter knobs — not yet consumed by the
  corpus fit loop.

### The gap between "exists" and "northstar" (audit findings, file:line)

Two independent reviews (a code-path audit of the dispersion→backtest→serialization
spine, and a web-research pass on how banks actually run vega-flat dispersion + SOTA
benchmarks) converge on the following. This is the backlog the sprint ladder closes.

**Blocking correctness (the multi-name path has never actually run):**

1. **uid collision — multi-symbol archives are unloadable.** `Universe` reserves slot
   0 then `intern_ticker` assigns `uid = unders_.size()`, so the sole ticker in every
   board's fresh `Universe` gets **uid=1** (`src/universe.cpp:34,71`; `chain.cpp:17`;
   `session.cpp:427`). Every corpus-fit single-name `PricedSurface` therefore carries
   uid=1; `MarketSnapshot::load` → `SurfaceSet::create` over a date's surfaces →
   **"duplicate uid" `InvalidArgument`** → the whole backtest fails
   (`backtest.cpp:184-189`; `portfolio_pricer.cpp:146-150`). Masked to date because every
   test is single-symbol-per-date, reads per-symbol via `map_symbol`, or manually
   `with_uid`-stamps 1/2/3. **This blocks the entire northstar.**
2. **Backtest aborts on one missing/failed name on any date.** `build_dispersion_book`
   requires every member every date (`dispersion.cpp:128-131`); `on_step` propagates the
   `Err` (`dispersion_strategy.cpp:42-44`). Over 10–50 names × many dates a single
   `Unavailable` name kills the run. No drop-and-renormalize.
3. **No per-name fit-quality gate.** `oos_in_band` is recorded but never thresholded, so
   a poorly-generalizing single-name surface silently enters the archive and the book
   (`corpus.cpp:113-123`).

**QIS-methodology fidelity (the book is not how a desk sizes it):**

4. **ATM-straddle legs, not variance-swap / log-strip.** One strike `K=forward_at(T)`
   per name (`dispersion.cpp:30-47`). QIS dispersion indices (CBOE DSPX / COR) and bank
   desks use **variance swaps / VIX-style log-strips** precisely because variance vega
   is spot-invariant, so both legs stay vega-flat across the roll without re-striking;
   straddle vega peaks ATM and decays off-strike (BNP QIS; CBOE DSPX methodology).
5. **Vega-flat only at cohort inception; not delta-hedged.** `Σŵᵢ=1` makes basket vega
   == index vega at open (`dispersion.cpp:203-206`) but held lots are fixed
   (`dispersion_strategy.cpp:30-70`) and `DispersionStrategy` inherits `HedgeSpec{None}`
   (`strategy.hpp:227,268`). A desk delta-hedges daily and re-flattens vega each roll,
   scaling the single-name aggregate vega by **√ρ_implied** to match the index leg.
6. **One flatness mode only.** Real QIS books run **vega-flat / gamma-flat / theta-flat**
   sizings with a P&L decomposition that must sum to total. Only vega-weighted exists.
7. **No correlation-swap cross-check.** The canonical desk validation — dispersion-implied
   correlation minus the modeled correlation-swap strike equals the vega/volga/vanna
   convexity residual, and P&L ≈ (ρ_impl − ρ_real)·avg-single-name-variance — is absent.
8. **Cost model not calibrated to reality.** `FrictionModel` exists but the pilot must set
   single-name bid/ask in **vol points**, basket-crossing (~300 bps), carry/funding
   (~15–50 bps), and borrow, or the correlation premium is illusory.

**Data reality:**

9. **No real multi-name option data on disk; no per-name dividend plumbing.** Only SPY
   (multi-date YTD) + one XOM snapshot are cached; `OpraBatchSpec` carries no dividend
   field and the batch path passes zero divs (`opra_batch.cpp:186-192`), so discrete
   cash divs fold into the PCP-implied forward and mis-time American early exercise near
   ex-div. The pilot needs an operator-gated paid pull.
10. **Yield curve hand-fed, not per-date from data** (`opra_panel.cpp:346-361`;
    `opra_batch.hpp:144-151`); default `r=0` flat; per-expiry rate into carry still a
    "documented enhancement."

**Serialization throughput (pillar 2):**

11. **No committed throughput number and no regression gate;** single-threaded blob-CRC
    write loop (`surface_archive.cpp:458-536`), per-surface / per-node heap-alloc churn
    on reload (`:873-878`), whole-file read into a `vector<byte>` with no mmap / zero-copy
    (`:727-741`). The SOTA bar for a surface archive is mmap-able columnar zero-copy at GB/s.

---

## SPRINT LADDER

Dependency order. Each sprint is independently shippable and holds all Global Constraints.

| # | Sprint | Theme | Pillar | Depends on | Parent-roadmap overlap |
|---|---|---|---|---|---|
| **S0** | Fit-path + test-suite perf (de-Am fan-out, dedup, shared fixture) | unblock velocity | perf | — | G (perf), R4 (test-perf) |
| **S1** | Multi-name pipeline correctness | unblock end-to-end | correctness | S0 | — |
| **S2** | QIS methodology: variance-swap dispersion + flatness modes | fidelity | — | S1 | — |
| **S3** | Execution realism: hedging, cost model, corr-swap cross-check | fidelity/realism | — | S2 | D (risk) |
| **S4** | Fitting robustness at scale + per-name quality scoreboard | **correctness/robustness (pillar 1)** | correctness | S1 (∥ S2/S3) | B (breadth), F (divs) |
| **S5** | Serialization throughput + asserted perf gate | **throughput (pillar 2)** | perf | S1 (∥ S2–S4) | G (perf gate) |
| **S6** | Real-OPRA pilot + QIS reconciliation (the deliverable) | northstar | both | S1–S5 | — |

**S0 first — it unblocks developer velocity for every sprint below** (each SPY-touching
task currently pays a 10–50s full-board cold de-Am). Then **S1 unblocks the multi-name
path.** S4 and S5 are the two northstar verification pillars and can run in parallel with
the S2→S3 methodology thread once S1 lands. S6 is the capstone that consumes all of them
and is the only sprint that spends money.

---

## Sprint S0 — Fit-path + test-suite performance (resolve before moving on)

**Goal.** Kill the redundant, single-threaded, doubled-up cold de-Americanization that
makes every SPY-touching test (and the served ConvexDense production path) take 10–50s.
Get the SPY-family test wall-clock down ~3–5× with **no loss of coverage** and
**bit-identical fitted surfaces**, and make a single served-surface fit fan out across
cores so it is fast in production too.

**Diagnosis (measured 2026-07-08, `-j1`, `build/`).** The 15-test SPY family runs in
**207s**. Per-test: `CurveSurfaceNoArb.SpyDenseIsCalendarArbFree` **49.9s**,
`SpyRealCalendarReporting` 25.0s, `SpyBidAskRegression.ConvexDenseServedViaSessionInBand`
23.4s, `BacktestReal.*` ~20s each, `SpyPortfolioPnl` 16.9s,
`PnlGreeksConsistency.Session_ConvexDense` 14.8s, `SpyArchiveRoundTrip` 14.8s (the five
synthetic/reuse tests are ~0.1s). Root cause is **not** the QP surface fit (a dense slice
fits in ~ms; 35 slices ≈ 1s — the user's "<500ms fit" intuition is correct for the fitting
proper). The cost is the **per-strike cold Andersen-Lake de-Americanization of the full
~13.9k-contract SPY board**, which is triply wasteful:

1. **Single-threaded.** `fit_curve_surface` walks 35 chains sequentially and de-Ams each
   strike sequentially — no fan-out, unlike `value_chain` (which block-partitions de-Am
   across `std::jthread` workers, `pricer_fitter.cpp:26-61`, → 93k inv/s on 4 cores).
2. **Done twice per fit.** `build_observations_european` (`curve_fit.cpp:129-131`,
   caches explicitly empty → cold) builds the fit obs, then `build_parity_data`
   (`curve_fit.cpp:161`) cold-inverts the **whole board again** just for the parity
   *diagnostic* (`chain_parity`).
3. **Re-done from scratch in every test.** ~10 SPY tests each fit the whole board; there
   is no shared fitted-surface fixture (contrast the db-suite `built_warehouse` pattern).

The served ConvexDense session path routes through the same `fit_curve_surface`
(`session.cpp:273`), so (1) and (2) are **production** costs, not just test costs.

**Constraint (do not "fix" by trading accuracy).** `curve_fit.cpp:118-128` documents that
the dense fit **must** de-Am cold — the cached-surrogate carry bias knocks the penny-tight
dense fit out of band. So do NOT swap the fit's de-Am to the correction cache. A full
cold-board fit under 500ms is not reachable without the cache; fan-out + dedup targets
~1–2s, and the true sub-500ms full-board path stays the cached/eSSVI serve, not the cold
dense fit. Keep that boundary explicit.

**Depends on:** — (base `5b46b74`). **Parent overlap:** Sprint G (perf gate),
noarb-followups R4 (SPY cold-AL test-perf pass).

**Files:** `src/curve_fit.cpp` (fan-out + parity de-Am dedup), `src/pricer_fitter.cpp`
(reuse the existing `parallel_for` helper — hoist it if it is TU-local), `src/session.cpp`
(thread the fit's worker count through), tests `curve_noarb_test.cpp`,
`spy_bidask_regression_test.cpp`, `spy_archive_roundtrip_test.cpp`,
`spy_portfolio_pnl_test.cpp`, `pnl_greeks_consistency_test.cpp`, `backtest_real_test.cpp`,
new `tests/support/spy_fitted_fixture.hpp` (shared cached fit).

**Tasks (TDD; measure with the interleaved-A/B discipline; bit-identity is the gate):**

- **S0-1. Fan out the per-strike de-Am in `fit_curve_surface`.** Block-partition the de-Am
  inversions (across strikes within a slice, and/or across the 35 slices) over
  `std::jthread` workers using the existing `parallel_for` block-partition helper — disjoint
  output slots, pure const reads (the `value_chain` / `calibrate_pool` determinism pattern),
  so the result is **bit-identical for any worker count**. Worker count is a fit option
  (default = hardware concurrency; 1 recovers today's path). *Gate:* fitted surface theo +
  every `SliceContext` is `memcmp`-identical across worker counts 1 vs N on the real SPY
  board; wall-clock drops materially (report the factor honestly).

- **S0-2. Eliminate the double de-Am (parity diagnostic reuse).** `build_parity_data` re-inverts
  every OTM leg to get `market_iv` for the parity score — the same European-equivalent IVs
  `build_observations_european` already computed. Thread the fit-obs market IVs (and their
  strike/side alignment) into the parity scoring instead of re-inverting, OR make the parity
  diagnostic opt-in (`CurveConfig`/`SurfaceParityInputs` flag, default keeps parity for the
  served path but tests that don't assert parity turn it off). *Gate:* `per_expiry` parity
  reports are bit-identical where retained; a fit with parity disabled is bit-identical on the
  fitted surface and skips the second de-Am pass (measured ~2× on the fit).

- **S0-3. Shared cached SPY fit fixture for read-only tests.** Add a test-support fixture that
  fits the SPY board **once** per binary run (static/lazy, or materialize a pre-fit archive
  once) and reuse it in the tests that only *read* a fitted surface —
  `SpyArchiveRoundTrip`, `SpyPortfolioPnl`, `PnlGreeksConsistency.Session_ConvexDense`,
  `SpyRealCalendarReporting`, `BacktestReal.*`. Tests that assert the **fit itself**
  (`CurveSurfaceNoArb`, `SpyBidAskRegression`) keep fitting. *Gate:* the read-only tests
  produce identical assertions against the shared fixture; the fixture is built exactly once
  (open/fit counter == 1); no test mutates shared state (const reuse).

- **S0-4. Subset the fit-asserting tests to a property-preserving slice set.** `CurveSurfaceNoArb`
  and `SpyBidAskRegression` re-fit the full board; trim each to a representative expiry/strike
  subset that preserves its property. **Caution:** the `CurveSurfaceNoArb` residual sits in the
  ~0.4y put wing across adjacent expiries (`curve_noarb_test.cpp:116-133`), so the subset MUST
  retain those adjacent ~0.4y expiries or the 2-crossing baseline changes — subset thoughtfully
  and re-measure the baseline. *Gate:* each subset test still exercises its property (the
  documented calendar residual for noarb; the in-band floor for bidask) and runs materially
  faster; the recorded baselines are re-measured and asserted exactly (never widened to pass).

**Acceptance gate.** The 15-test SPY family runs in **≤ ~70s** (from 207s) with the same
assertions and **no loss of correctness coverage**; a real-SPY served fit is **bit-identical
across worker counts** (S0-1) and skips the redundant de-Am pass (S0-2); read-only SPY tests
reuse one shared fit (S0-3); fit-asserting tests keep their properties on documented subsets
(S0-4). Report the production served-fit wall-clock improvement. Full `atx_vol` gate green;
`/WX` clean; determinism preserved.

---

## Sprint S1 — Multi-name pipeline correctness (unblock end-to-end)

**Goal.** Make an index + N-name corpus build → archive → load → dispersion book →
backtest run to completion, **deterministically, on synthetic multi-name data, with
zero paid pull.** This sprint fixes the correctness blockers that mean the multi-name
path has never executed. It ships no methodology change — just "it runs at all."

**Rationale.** Blocker #1 (uid collision) is a hard correctness bug that fails
`SurfaceSet::create` on any real multi-symbol date; blockers #2/#3 make the run fragile
and silently wrong. Nothing downstream can be measured until this is closed.

**Depends on:** S0 (fast fits make the multi-name iteration tractable; not a hard code dep).

**Files:** `src/corpus.cpp` / `include/atx/vol/corpus.hpp`, `src/dispersion.cpp` /
`dispersion.hpp`, `src/dispersion_strategy.cpp`, `src/backtest.cpp` / `backtest.hpp`,
`src/portfolio_pricer.cpp` (SurfaceSet keying), `include/atx/vol/universe.hpp`
(uid stamping helper if needed), tests `corpus_test.cpp`, `dispersion_test.cpp`,
`backtest_test.cpp`, new `tests/multiname_pipeline_test.cpp`.

**Tasks (TDD; each ends independently testable):**

- **S1-1. Distinct per-symbol uids across a corpus archive.** At corpus write (or archive
  assembly), stamp each surface with a uid derived deterministically from its canonical
  symbol (e.g. a stable hash or a manifest-assigned dense index), so a date's archive
  holds distinct uids. Prefer keying `MarketSnapshot`/`SurfaceSet` by **symbol** and
  deriving uid from symbol, so the mapping is 1:1 and stable across dates.
  *Interface:* `with_uid` already remaps (`dispersion.hpp:154`); add
  `uid_for_symbol(std::string_view) -> uint32_t` (stable) and apply it in `build_corpus`
  and/or `MarketSnapshot::load`. *Gate:* a 2-date synthetic corpus with {index + 3 names}
  per date loads via `MarketSnapshot::load` and `SurfaceSet::create` returns Ok with
  distinct uids; `find(uid_for_symbol("SPY"))` resolves the index surface. **Failing test
  first** reproduces the current "duplicate uid" `InvalidArgument`.

- **S1-2. Symbol↔uid binding for the dispersion universe.** `DispersionUniverse` members
  bind by uid (`dispersion.hpp:56-66`). Make the universe resolvable by **symbol** against
  a snapshot's directory (`MarketSnapshot::uid_of`), so a `DispersionUniverse` authored in
  symbols works across dates regardless of the uid scheme. *Gate:* a universe authored as
  `{index:"SPY", names:["AAPL","MSFT",...]}` resolves every leg on every date of the
  synthetic corpus.

- **S1-3. Drop-and-renormalize on an unavailable name.** When a member is `NotFound` /
  `Unavailable` on a date, drop it and renormalize the surviving basket weights (Σŵ=1
  over survivors), record which names were dropped and why, and continue — never abort the
  run. Guard a minimum surviving-basket size (else the date is a no-trade / flat step,
  logged). *Interface:* `build_dispersion_book` / `dispersion_signal` gain a
  `MissingNamePolicy {Error, DropRenormalize}` (default `Error` preserves existing tests;
  the strategy uses `DropRenormalize`). *Gate:* a corpus where one name is absent on one
  date runs to completion; the dropped name appears in the run's diagnostics; weights on
  that date sum to 1 over survivors.

- **S1-4. Per-name fit-quality gate in the corpus.** Threshold `oos_in_band` (and
  min-strikes / calendar-arb-free) at corpus build; a board below the floor is demoted to
  `Failed`/`Quarantined` with a reason code rather than silently `Ok`. Floor is a
  `CorpusConfig` knob (default off → bit-identical to today; the pilot turns it on).
  *Gate:* a synthetic board engineered to over-fit (thin one-sided wing) is quarantined
  when the floor is on and its reason is recorded; with the floor off, behavior is
  bit-identical to the current build.

**Acceptance gate.** `multiname_pipeline_test.cpp`: a synthetic corpus (index + ≥10 names,
≥3 dates, ≥1 deliberately-thin and ≥1 deliberately-missing name) builds → archives →
loads → runs a (still ATM-straddle) dispersion backtest end-to-end, produces a PnL +
implied-correlation series, is **bit-identical across thread counts**, and emits a
per-name fit-quality + drop scoreboard. No paid pull. Full `atx_vol` gate green; `/WX` clean.

---

## Sprint S2 — QIS methodology: variance-swap dispersion + flatness modes

**STATUS: SUPERSEDED BY THE LOCKED NEXT-SPRINT PLAN.** Do not implement the historical
S2 tasks below. The active implementation plan is
`2026-07-10-atx-vol-spy-listed-options-vega-flat-backtest-sprint.md`: a real OPRA,
traditional vega-flat SPY/component listed-ATM-straddle backtest that proves fitting,
serialization, reload, American pricing/Greeks, daily hedging, P&L reconciliation,
and throughput. Variance strips, correlation signals, and additional flatness modes
move after that basic artifact.

**Goal.** Re-express the dispersion book the way a bank QIS desk sizes it: **variance-swap
/ log-strip legs** (spot-invariant vega), **three flatness modes** (vega-flat / gamma-flat
/ theta-flat), the **√ρ vega-scaling**, and **clean-vs-dirty implied correlation + the
DSPX-form** — keeping the ATM-straddle book as a labeled "dirty" comparator.

**SOTA/QIS rationale.** QIS dispersion indices are built on variance (CBOE DSPX/COR use a
modified VIX log-strip on index and single names) because variance vega does not drift with
spot, giving true vega-flatness across the roll; ATM straddles force constant re-striking.
BNP's QIS decomposition defines the three flatness sizings and requires their P&L to sum to
total. `derivatives.hpp` already ships the model-free strip — this sprint wires it into the
served path and the dispersion sizing.

**Depends on:** S1.

**Files:** `include/atx/vol/derivatives.hpp` / `src/derivatives.cpp` (instantiate the strip
for the served surface), `include/atx/vol/priced_surface.hpp` (a strip-friendly `iv(k,T)`
accessor if needed), `dispersion.hpp` / `src/dispersion.cpp` (variance legs, flatness modes,
weight conventions, DSPX/dirty signals), `dispersion_strategy.cpp`, tests
`derivatives_test.cpp`, `dispersion_test.cpp`, new `variance_dispersion_test.cpp`.

**Tasks:**

- **S2-1. Variance-swap fair strike on the served `PricedSurface` / ConvexDense path.**
  `var_swap_fair_strike` / `vol_swap_fair_strike` are template-instantiated only for
  `EssviSurface`/`SviSurface` (`derivatives.hpp:300-311`). Add an instantiation (or a thin
  `IVolCurve`/`PricedSurface` adapter) so the strip runs on the served surface the corpus
  archives. *Gate:* on a synthetic eSSVI surface the served-path strike matches the existing
  `EssviSurface` strike bit-identically (same `iv(k,T)` samples ⇒ same Simpson integral); on
  a ConvexDense SPY slice it returns a finite, sane K_var with left/right truncation flags.

- **S2-2. Variance-swap dispersion legs.** Add a `DispersionInstrument {AtmStraddle,
  VarianceSwap}` to `DispersionConfig`. For `VarianceSwap`, each leg is a variance-notional
  position whose fair strike + vega come from S2-1; the implied-correlation signal uses the
  **variance-strip / VIX-style** vols, not point-ATM. *Interface:* extend `DispersionLeg`
  with `k_var`, `var_notional`; `build_dispersion_book` branches on instrument. *Gate:* a
  variance-dispersion book on a synthetic universe is vega-flat at inception to a stated
  tolerance and its vega is materially less spot-sensitive than the straddle book (a spot
  bump leaves variance-leg vega ~unchanged vs straddle-leg vega dropping).

- **S2-3. Flatness modes + √ρ vega-scaling.** Add `DispersionFlatness {VegaFlat, GammaFlat,
  ThetaFlat}` sizing. VegaFlat: net vega 0 (short gamma/short theta). GammaFlat: zero gamma
  P&L. ThetaFlat: zero gamma so P&L is pure vol-of-vol/correlation carry. Encode the
  single-name aggregate-vega scaling by **√ρ_implied** to match the index leg. *Gate:* each
  mode zeroes its target book greek at inception to tolerance on a known-truth universe; the
  √ρ scaling reproduces net-vega≈0 for the vega-flat variance book.

- **S2-4. Clean/dirty correlation + DSPX form.** Keep the existing clean ρ; add the labeled
  **dirty** proxy `ρ_dirty = σ_I²/(Σŵᵢσᵢ)²` and the **DSPX** dispersion measure
  `√(Σŵᵢσᵢ² − σ_I²)` as first-class signal outputs, with the weight convention explicit
  (index/cap weights for the correlation index; variance-notional ∝ wᵢ for the trade).
  *Gate:* on a hand-checked 3-name universe, clean/dirty/DSPX match closed-form values;
  clean ≤ dirty (documented bias direction).

- **S2-5. P&L decomposition closes.** The dispersion P&L attribution (delta/gamma/vega/
  theta/…) reported through the engine's `pnl_explain` must sum to total per step for every
  flatness mode. *Gate:* on a synthetic two-date step, Σ axes + unexplained == total to the
  existing 1e-6 closure tolerance, for each of the three flatness modes and both instruments.

**Acceptance gate.** A variance-swap vega-flat dispersion book runs end-to-end on the
synthetic multi-name corpus; the three flatness modes each zero their target greek at
inception; clean/dirty/DSPX signals are exposed and match closed forms; attribution closes;
ATM-straddle path stays bit-identical (opt-in instrument). Full gate green; `/WX` clean.

---

## Sprint S3 — Execution realism: hedging, cost model, correlation-swap cross-check

**Goal.** Make the backtest credible: **daily delta-hedge + per-roll vega re-flattening**,
a **realistic calibrated cost model**, and the **correlation-swap fair-value cross-check**
that is the canonical desk validation of dispersion math.

**QIS rationale.** A vega-flat dispersion product delta-hedges daily and re-flattens vega
each roll; a naive backtest that skips single-name bid/ask (vol points), basket-crossing
(~300 bps), carry/funding (~15–50 bps), and borrow overstates the correlation premium
(which is real but ~6–18 pts). The correlation-swap identity — dispersion-implied corr minus
the modeled corr-swap strike equals the vega/volga/vanna convexity residual, and P&L ≈
(ρ_impl − ρ_real)·avg single-name variance — is how a quant proves they replicate the desk.

**Depends on:** S2. **Parent overlap:** Sprint D (unified risk / attribution).

**Files:** `dispersion_strategy.cpp` (hedge_spec + re-vega), `backtest.cpp`/`backtest.hpp`
(cost-model params, vega-drift-band re-flatten hook), `include/atx/vol/dispersion.hpp`
(correlation-swap fair value + cross-check), `derivatives.hpp` (if a corr-swap helper lands
there), tests `backtest_test.cpp`, `dispersion_test.cpp`, new `dispersion_validation_test.cpp`.

**Tasks:**

- **S3-1. Delta-hedge the dispersion book.** Give `DispersionStrategy` a `DeltaToZero`
  `hedge_spec()` (cadence Daily, configurable band). *Gate:* post-hedge |net delta| ≤ band
  each step; hedge PnL ≈ −gamma rent sign check on a known step.
- **S3-2. Per-roll (and vega-drift-band) re-flattening.** Re-book/re-size legs to restore the
  target flatness at each roll and when |net vega| exceeds a band between rolls. *Gate:* net
  vega returns inside the band after a re-flatten on a spot-drift path; frictionless re-flatten
  is cost-neutral.
- **S3-3. Calibrated cost model.** Expose single-name bid/ask in **vol points** (per-name),
  basket-crossing cost, carry/funding, and borrow as a `DispersionCostSpec` mapped onto the
  engine `FrictionModel` + `FinancingConfig`. *Gate:* zero-cost config is bit-identical to S2;
  drag is monotonic in each cost knob; a documented "realistic" preset produces a plausible
  net-of-cost tearsheet.
- **S3-4. Correlation-swap fair value + cross-check.** Add a modeled correlation-swap strike
  and assert the identity: dispersion-implied corr − corr-swap strike = the vega/volga/vanna
  convexity residual (sign + magnitude band, not zero); and P&L ≈ (ρ_impl − ρ_real)·avg
  single-name variance on a known-truth path. *Gate:* on a synthetic universe with a known
  correlation, the cross-check residual sits in the documented band and the P&L identity holds
  to tolerance.

**Acceptance gate.** The vega-flat variance dispersion backtest runs delta-hedged with per-roll
re-flattening and a realistic cost model; the correlation-swap cross-check and P&L identity pass
on known-truth universes; net-vega and net-delta tolerance bands hold at inception and post-roll;
determinism preserved. Full gate green; `/WX` clean.

---

## Sprint S4 — Fitting robustness at scale + per-name quality scoreboard (pillar 1)

**Goal.** Prove the fitting pipeline is **correct and robust across many diverse single-name
boards**. Wire the profile taxonomy into the corpus fit, produce a per-name fit-quality
scoreboard bucketed by profile, handle discrete dividends + per-date rates, and add
property/fuzz coverage so "robust" is a proven property, not a spot check.

**SOTA rationale.** SPY/XOM are the only standing real gates; a QIS basket spans event names,
low-price names, HTB/dividend names, and thin one-sided boards. Vola markets a dedicated Div
Fitter because discrete cash dividends break naive vol handling. This sprint is the pillar-1
verification of the northstar.

**Depends on:** S1 (parallel with S2/S3). **Parent overlap:** Sprint B (curve breadth — C8/CStar
may be the family a hard board needs), Sprint F (discrete divs / rate bootstrap).

**Files:** `src/corpus.cpp` (profile classification + scoreboard), `include/atx/vol/profile.hpp`
(consume `classify_underlier`), `src/curve_selector.cpp` (profile-keyed family shortlist),
`opra_batch.hpp`/`src/opra_batch.cpp` (per-symbol `cash_divs`, per-date curve), `opra_panel.cpp`
(per-date rate curve from data), tests `corpus_test.cpp`, `curve_selector_test`/new
`corpus_robustness_test.cpp`, new `corpus_fuzz_test.cpp`.

**Tasks:**

- **S4-1. Profile classification in the corpus fit.** Classify each board via
  `classify_underlier[_with_ticker]` and feed the per-bucket `CalibOpts`/`FilterOpts` to the
  fit (index-ultraliquid → ConvexDense recipe; sparse small-cap → parsimonious eSSVI; event
  name → event-aware knobs). *Gate:* a board's chosen family/knobs match its classified profile;
  existing SPY/XOM fits stay bit-identical (their profiles reproduce today's config).
- **S4-2. Per-name fit-quality scoreboard.** Extend the manifest / a diagnostic report with
  per-name in-band, vega-weighted vol-RMSE, reduced-χ², calendar-arb-free flag, n_slices, and
  profile bucket; aggregate a distribution and flag every name below the floor. *Gate:* on a
  synthetic 50-name corpus (mixed profiles, some engineered-hard) the scoreboard reports the
  full distribution and no failure is silent (every non-Ok has a reason code).
- **S4-3. Discrete dividends per name.** Add `cash_divs` to `OpraBatchSpec` → `OpraLoadSpec`
  and thread per-symbol discrete divs through the fit (escrowed-forward is the shipped
  treatment; native discrete-div PDE stays parent-Sprint-F). *Gate:* a dividend-heavy synthetic
  name fits with divs supplied and the de-Am forward matches the injected forward; zero-div path
  bit-identical.
- **S4-4. Per-date yield curve from data.** Source a per-date rate curve (≥2 pillars) rather than
  a single hand-fed flat rate across all dates; thread per-expiry rate into carry. *Gate:* a
  multi-date synthetic corpus uses an evolving curve; flat-input reproduces today's numbers.
- **S4-5. Property + fuzz coverage.** Random-but-valid single-name boards (varying spread / skew /
  term / sparsity / price level) fed to the corpus fit must never crash, never archive a served
  arb, and always converge or return a clean `Err`. *Gate:* N fuzz iterations clean; a planted
  arb-emitting board is caught; findings (if any) filed + fixed.

**Acceptance gate.** A large synthetic multi-profile corpus (≥50 names) fits with a documented
fit-quality distribution; a stated fraction meets the in-band floor; every failure is classified
(no silent drops); discrete-div and per-date-rate paths work with zero-config bit-identity; the
fuzz suite is clean. Full gate green; `/WX` clean.

---

## Sprint S5 — Serialization throughput + asserted perf gate (pillar 2)

**Goal.** Prove **high-performance fitting-surface serialization at corpus scale** with a
committed, asserted regression gate — the pillar-2 verification of the northstar.

**SOTA rationale.** The archive currently has bench-only numbers, a single-threaded blob-CRC
write loop, per-surface/per-node alloc churn on reload, and a whole-file `vector<byte>` read
with no mmap. The SOTA bar for a surface archive is mmap-able, columnar, zero-copy at GB/s. A
backtest streaming many symbols × many dates needs the write to keep up with the fit and the
read to be near-free.

**Depends on:** S1 (parallel with S2–S4). **Parent overlap:** Sprint G (asserted perf-regression CI gate).

**Files:** `src/surface_archive.cpp` / `surface_archive.hpp` (parallel write, alloc pooling,
mmap/zero-copy read), `examples/surface_archive_bench.cpp`, `tests/support/bench_gate.hpp`
(extend), new `tests/serialization_perf_test.cpp` (bench-gated, archived baselines), new
`tests/support/perf_baseline.tsv`.

**Tasks:**

- **S5-1. Measure + commit the baseline.** Under the interleaved-A/B throttle-cancelling
  discipline, measure write MB/s, read surfaces/s, and reload alloc count on a corpus-scale
  fixture (many symbols × dates). Commit `perf_baseline.tsv`. *Gate:* the measurement is
  reproducible within a tolerance band; the number is recorded honestly (including if it
  underwhelms).
- **S5-2. Parallelize the write / blob-CRC loop.** Fan the per-surface blob build + CRC across
  workers (the payload is independent per surface), deterministic output bytes. *Gate:* written
  bytes are bit-identical to the single-threaded writer across thread counts; measured write
  throughput improves (report the factor honestly).
- **S5-3. Pool reload allocations.** Remove per-surface `make_unique` + per-node vector alloc
  churn (`surface_archive.cpp:873-878`) via a slab/arena or reserve-once path. *Gate:* reload
  produces bit-identical surfaces with a measured drop in allocation count; theo round-trip
  unchanged.
- **S5-4. Zero-copy / mmap read path.** Add an mmap-backed `open_file` that lets slice params be
  read as views without a full-file copy (fall back to the buffered path where a platform lacks
  mmap). *Gate:* mmap and buffered reads produce bit-identical surfaces; the corpus-scale reload
  is measurably faster / lower-allocation; round-trip fidelity preserved.
- **S5-5. Asserted perf-regression gate.** `serialization_perf_test.cpp` (ATX_VOL_BENCH-gated)
  asserts write MB/s + read surfaces/s against `perf_baseline.tsv` with a tolerance band. *Gate:*
  passes at current numbers; a deliberately injected 2× slowdown makes it fail (self-test).

**Acceptance gate.** Corpus-scale serialize/reload round-trips **bit-identically** at the target
throughput; a CI-runnable asserted perf gate protects write MB/s and read surfaces/s; the README
perf table is sourced from the gate. Determinism preserved. Full gate green; `/WX` clean.

---

## Sprint S6 — Real-OPRA pilot + QIS reconciliation (the deliverable)

**Goal.** Run the vega-flat variance dispersion backtest **end-to-end on real Databento OPRA
data** for an index + a liquidity-screened basket over a real window, and validate it to
**QIS-replication grade.** This is the only sprint that spends money and it is **operator-gated.**

**Depends on:** S1–S5.

**Files:** `atx-core/examples/databento_bulk_opra.cpp` (bulk pull with free preflight + hard cost
cap), `examples/dispersion_backtest.cpp` (real-corpus driver), new
`tests/spy_dispersion_real_test.cpp` (skips cleanly when the parquet corpus is absent, mirroring
the existing SPY-real skip pattern), a `tools/` reconciliation report generator.

**Tasks (the paid-pull tasks are operator-gated — do NOT pull without approval):**

- **S6-1. Basket + window definition.** Fix the pilot universe (index + ~10–50 names,
  liquidity/PCA-screened, variance-notional weights ∝ wᵢ, reset each roll) and the date window,
  so ingest, corpus, and backtest share one definition. Key ingest on Databento `instrument_id`
  (not raw ticker — OSI symbols recycle on corporate actions). *Gate (free):* the universe +
  window is a committed spec; a free `MetadataGetRecordCount` preflight reports the estimated cost.
- **S6-2. Bulk OPRA pull (operator-gated, cost-capped).** Loop symbols × dates with the free
  preflight and a **hard `$` cap**; refuse above the cap; filter crossed/stale/0DTE-distorted
  quotes before fitting; cache DBN + parquet. *Gate:* the pull completes within cap; per-file
  status recorded; no re-pull of cached slices.
- **S6-3. Build the real corpus + run the backtest.** Corpus-fit the basket with the S4 profile
  path + fit-quality gate; run the S2/S3 vega-flat variance dispersion backtest with the realistic
  cost model. *Gate:* the run completes deterministically; the fit-quality scoreboard is within the
  documented floor for the liquid basket; any quarantined name is reported, not silently dropped.
- **S6-4. QIS-replication-grade validation + artifact.** Produce the northstar artifact: PnL +
  implied-correlation (clean/dirty/DSPX) + attribution tearsheet, plus a reconciliation report:
  vega-neutrality within tolerance at inception + each roll; attribution closes; the
  correlation-swap cross-check residual in-band; where the window overlaps a published index
  (DSPX / COR), a P&L / level reconciliation within a stated tolerance. *Gate:* all validation
  checks pass or are documented as honest negatives with a root cause.

**Acceptance gate.** A real multi-name OPRA vega-flat variance dispersion backtest runs
deterministically end-to-end; QIS validation (vega-neutrality tolerance, attribution closure,
correlation-swap identity, published-index reconciliation where available) passes or ships as a
documented honest negative; the tearsheet + reconciliation artifact is produced. No paid pull
beyond the operator-approved `$` cap.

---

## Global constraints (bind EVERY task in EVERY sprint)

- **Language / warnings:** C++20; clang-cl `/W4 /permissive- /WX` — zero warnings. Follow the
  house style in `.agents/cpp/agent.md`. No new external deps without an explicit sprint decision.
- **Namespace / vocabulary:** `atx::vol`; `Result`/`Status`/`Ok`/`Err`/`ATX_TRY`; linalg via
  `atx::core::linalg`.
- **No-regression / bit-identity:** every change to a served/fitted path is **slack on the paths
  it does not intend to change** — byte-identical output where no new behavior is requested; new
  behavior is opt-in behind a flag/preset until a deliberate default flip. SPY dense in-band
  (94.65% post-Sprint-A strict floor) and XOM parity gates held.
- **Constraints as rows, not variables** (Sprint-A finding): any new fit constraint is expressed
  as QP rows; never as slack variables that blow the KKT dimension (the interval-loss 2M-slack
  regression). If a band/slack formulation is truly needed, eliminate slacks via Schur complement
  so the KKT stays N-dimensional.
- **Determinism:** backtest + corpus + serialization output **bit-identical across thread counts**;
  strike-from-delta / any root-find uses a fixed tol + iteration cap.
- **Performance claims:** measured with the interleaved-A/B throttle-cancelling discipline in one
  binary; **honest negatives shipped as negatives** (documented, not papered over).
- **Test gate (fast, parallel):** `ATX_VOL_FIT_WORKERS=1 ctest --test-dir build -L atx_vol -j16
  --output-on-failure --timeout 900` (~70s post-S0, expect all green; benches gated behind
  `ATX_VOL_BENCH`). The `ATX_VOL_FIT_WORKERS=1` env cap (S0-4′) stops the per-fit de-Am fan-out
  (S0-1/S0-3) from oversubscribing under `ctest -j` — WITHOUT it `-j16` runs ~190s (16 procs × 16
  fit-threads on 16 cores); `-j8` unset is also fine (~83s). Leave the env UNSET when running a
  single heavy fit test at `-j1` (fan-out then uses all cores; SPY family ~61s). Build:
  `cmake --build build --target atx-vol-tests -j`. Run foreground; be patient. If a link fails
  `permission denied` on `atx-vol-tests.exe`, a leftover test process holds it —
  `taskkill //F //IM atx-vol-tests.exe` then relink.
- **Data / git:** data dirs (`data/**`, `data/spy_ytd/**`) stay gitignored/untracked. **No paid
  Databento pulls except the operator-gated S6-2** (free preflight first; cached slices reused).
  **Explicit-path staging only — never `git add -A`;** stage only the files a task touches. Commit
  messages end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

## Running the loop

1. **One sprint at a time, dependency order:** S0 → S1 → (S2→S3) ∥ S4 ∥ S5 → S6. Within a sprint, use
   **superpowers:subagent-driven-development**: fresh implementer per task, task review (spec +
   quality) after each, broad review at sprint close.
2. **Expand-then-execute (optional):** for a sprint wanting fuller step-level code, run
   **superpowers:writing-plans** to expand its task list into a bite-sized TDD plan at
   `docs/superpowers/plans/YYYY-MM-DD-atx-vol-pf2-s<x>.md` before dispatching.
3. **Every task** holds the Global Constraints; **every sprint** closes only against its hard
   acceptance gate with the full `atx_vol` gate green.
4. **Progress ledger:** track in `.superpowers/sdd/progress.md` — record each task
   `complete (commits base..head, review clean)` so the loop survives compaction.
5. **Honest-negative discipline:** if a sprint's perf/accuracy/QIS premise fails when measured,
   ship the negative documented — do not ship a non-improvement as a win.

## Self-review (module coverage vs the northstar)

- **Velocity/perf prerequisite:** S0 (de-Am fan-out + parity dedup + shared fixture) — cuts the
  SPY-family gate ~3–5× and makes every SPY-touching sprint below iterate at reasonable speed;
  also a production served-fit win. ✓
- **Northstar pillar 1 (fitting correctness/robustness across many names):** S1 (multi-name runs at
  all), S4 (profile-keyed fit + per-name scoreboard + divs + rates + fuzz). ✓
- **Northstar pillar 2 (serialization throughput):** S5 (parallel write + alloc pooling + zero-copy
  read + asserted gate). ✓
- **QIS-replication fidelity:** S2 (variance-swap legs + flatness modes + clean/dirty/DSPX), S3
  (daily hedge + re-flatten + realistic costs + correlation-swap cross-check + P&L identity), S6
  (real-data reconciliation vs published index). ✓
- **The deliverable:** S6 (real OPRA end-to-end + tearsheet + reconciliation artifact). ✓
- **Blockers closed:** uid collision (S1-1), abort-on-missing-name (S1-3), no fit-quality gate
  (S1-4/S4-2), ATM-straddle-only (S2), no hedge / vega-flat-only-at-open (S3-1/S3-2), one flatness
  mode (S2-3), no corr-swap check (S3-4), uncalibrated costs (S3-3), no real data / no div plumbing
  (S4-3/S6-2), static yield curve (S4-4), no serialization throughput gate (S5). ✓
- **Deliberately OUT (mission boundary):** exotics, stochastic-vol, production MC/LSMC var-swap
  engines, jump-diffusion, GPU — none appear as sprints. ✓
- **Type consistency:** `uid_for_symbol` (S1-1) resolves the `DispersionUniverse` (S1-2) and
  `SurfaceSet`/`MarketSnapshot`; the served-path strip (S2-1) feeds the variance legs (S2-2), the
  flatness modes (S2-3), and the corr-swap cross-check (S3-4); the corpus fit-quality gate (S1-4)
  and profile path (S4-1) feed the scoreboard (S4-2); the serialization baseline (S5-1) backs the
  asserted gate (S5-5); S6 consumes every prior sprint. ✓
