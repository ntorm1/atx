# SPY Dispersion Backtest Hot Path — Deep-Dive Review Synthesis

PM-collected compressed summaries from 4 deep-dive agents. Full reports: 01 (correctness+example→lib), 02 (performance), 03 (config/features/modularity), 04 (web refs).
Axes: 1 correctness · 2 performance · 3 configurability · 4 features/unwired · 5 example→library + API/robustness/modularity.
Context: WS-S mmap load merged; AVX2 marks default-ON (user override); greeks ForceAvx2; two-pass fused. 82-sess best 218ms, 135-sess best 405ms (~333 sess/s). Remaining wall is PRICING-bound.

---

## 03 — CONFIG / FEATURES / MODULARITY  [report: 03-config-features-modularity.md]

**Config verdict: typed at the leaf, TSV-soup at the seam.** RunSpec (flat `map<string,string>`, ~20 keys, dispersion_workflow.cpp:67) → DispersionBacktestConfig (9) → DispersionConfig + RunConfig, hand-mapped field-by-field. Surface-backtest CLI copies only **6** RunSpec keys (spy_dispersion_backtest.cpp:533-539); ~7 more honored only on other subcommands → silently inert. No unknown-key rejection, no typed round-trip. Leaf structs (DispersionConfig, RunConfig, BacktestResult, TearSheet) are clean/typed.

Hardcoded-should-be-configurable: (1) DispersionSide locked ShortIndexLongNames (dispersion_backtest.cpp:16); (2) frictions always OFF; (3) financing/borrow OFF, flat_rate misrouted to fit only; (4) hedge locked DeltaToZero/Daily (:31-33); (5) fit preset/curve/admission locked in driver (spy_dispersion_backtest.cpp:251-271); (6) strike rule locked ATM-fwd straddle; (7) surface_provenance_policy locked Compatibility; (8) multiplier hardcoded 100.

Top missing/unwired (severity):
- [HIGH] Frictions+financing on RunConfig but unexposed to dispersion — always frictionless mid-fills (backtest.hpp:257,269).
- [HIGH] No risk limits / capital / drawdown-stop anywhere in loop.
- [MED] entry_every_n plumbed but ignored (dispersion_backtest.cpp:28 vs strategy.cpp:816-824).
- [MED] min/max_dte_days, min_weight_coverage parsed+validated but consumed only by listed path (dispersion_workflow.cpp:131-144).
- [MED] verify needs reference_reconciliation.tsv written only by external Python (spy_dispersion_backtest.cpp:447).
- [MED] run-projected-var half-wired (no verify gate, no test, re-hardcodes side, :586).
- [MED] Surface path never calls tearsheet() → no headline stats; TearSheet absolute-only, no benchmark-relative (:580, tearsheet.hpp:40).
- [LOW] record_diagnostics never enabled by CLI → implied-corr signal inert (dispersion_strategy.cpp:192).
- Single weighting scheme + single strike rule (dispersion.cpp:488-496); no scenario/config sweep. NO true dead code (every public symbol has example/test caller).

**API/modularity: usable as library on OUTPUT side, not INPUT side.** Typed entry `run_dispersion_backtest(Clock, DispersionUniverse, DispersionBacktestConfig) -> Result<BacktestResult>` exists (dispersion_backtest.hpp:37); BacktestResult = rich typed SoA (PnL track + 8-axis attribution + greeks + signals). Sizing pure/stateless; missing-name DEGRADES cleanly (DropRenormalize), index-leg fatal, well-tested. But config seam lossy; corpus-build/Clock/reconcile/persistence trapped in 761-LOC example main.

5 highest-value moves: (1) one typed DispersionRunConfig the TSV deserializes into STRICTLY (kill 4-struct hand-wiring); (2) expose plumbed side/frictions/financing/provenance/hedge-cadence; (3) return typed DispersionBacktestOutcome{track,sheet,drops,per_roll} + write_dispersion_artifacts + compute tearsheet on surface path; (4) lift corpus/Clock/reconcile orchestration into header; (5) add WeightingScheme/StrikeRule/RiskLimits/typed FitConfig + scenario-sweep driver.

Proposed API: `read_dispersion_run_config(path)->Result<DispersionRunConfig>` (strict); `run_dispersion_backtest(Clock, DispersionUniverse, DispersionRunConfig)->Result<DispersionBacktestOutcome>`; `write_dispersion_artifacts(dir, outcome)`; `open_dispersion_corpus(run_dir)->Result<Clock>`.

---

## 04 — WEB REFERENCE MATERIAL  [report: 04-web-references.md]  (14 primary sources)

1. **Dispersion methodology**: Bossu/Strasser/Guichard *Variance Swaps* (JPM 2005) — vega-neutral = each single-stock variance notional scaled so its vega notional matches index vega (beta-weighted); validate the leg solver against this. Moontower "Dispersion for the Uninitiated" (2023) — vega-neutral is **short correlation convexity (−gamma to correlation)**; vega-neutral ≠ correlation-neutral → risk board needs a correlation-gamma metric. BNP desk note — weighting mode (vega/theta/gamma-neutral) must be a **configurable leg-sizing policy re-solved each rebalance**, not hardcoded 1:1.
2. **Engine architecture**: NautilusTrader — **ns timestamps + strict chronological event ordering make look-ahead structurally impossible**; one time model shared backtest+live = research-live parity. LMAX/Fowler (2011) — single-threaded core over lock-free ring, allocation-free, event-sourced → bit-exact deterministic replay; separate cache lines vs false sharing. QuantStart — recover event-driven speed by **batching within an event, not across time**.
3. **Columnar/mmap/zero-copy**: Apache Arrow — **64-byte alignment = AVX-512 width** (no tail branch); zero-copy+mmap; store chains as **SoA columns** so the hot loop loads only needed fields. ArcticDB (Man Group) — columnar+compressed+mmap, **versioned time-travel reads → reproducible backtests pinned to an immutable snapshot**.
4. **SIMD pricing**: Andersen-Lake-Offengenden *High-Perf American Option Pricing* (SSRN 2547027, 2015) — Chebyshev exercise-boundary + Gauss-Legendre + Jacobi-Newton, **~100k prices/s/CPU** at FD accuracy; fast/accurate presets. Schadner *Explicit BS Implied Vol* (arXiv 2604.24480, 2026) — closed-form branchless IV, machine precision, **3.4× faster than Jäckel** (single-source/recent — verify before load-bearing). Intel SYCL BS (arXiv 2204.13740, 2022) — AVX-512 ~16 lanes / **~2.5× over SSE**; SoA required; runtime CPU dispatch + portable fallback.
5. **Calibration**: Gatheral-Jacquier *Arbitrage-Free SVI* (arXiv 1204.0646, 2013) — SSVI arb-free by construction; enforce as **hard bounds inside the fit loop**, not post-hoc repair. Global eSSVI (arXiv 2204.00312, 2022) — one global surface fit, **warm-started** from prior params, parallelizable → cuts build-corpus wall + better short-end. Zeliade/Hendriks-Martini — robust per-slice baseline to benchmark correctness before optimizing.
6. **Realism**: BSIC "Transaction Cost Modelling" (2025) — replace mid fills with **spread + square-root market impact** (Almgren β≈0.6 / Obizhaeva-Wang), data-calibrated config coefficients. Days-to-Expiry — $0.10-0.20/leg slippage cuts returns 10-30%; model assignment + **ex-div early-assignment on ITM calls**. Goodwin *Information Ratio* (FAJ 1998)/Grinold-Kahn — report **IR/alpha/beta/tracking-error vs a vol benchmark**.

---

## 01 — CORRECTNESS + EXAMPLE→LIBRARY  [report: 01-correctness-and-example-to-lib.md]
Counts: Crit 0 · High 3 · Med 5 · Low 4. **LOOK-AHEAD: CLEAN** (all marks/decisions/fills at-or-before decision ts; settlement only at exact expiry; VaR strict valuation_ts==scenario.ts). **Vega-flat neutralization CORRECT** on both straddle-book (dispersion.cpp:488-496, survivor renorm) + listed-schedule (validate_roll re-asserts net vega); PnL greek-explain vs independent held-mark reconciled at 1e-8.

- **[H1]** run-backtest ABORTS when build-schedule defers the first roll — reconcile requires snapshots.front().date == rolls.front().roll_date (listed_dispersion_reconciliation.cpp:240) but coverage gate legitimately defers (build_schedule_command:389-413). Fix: skip leading flat dates / start reconcile at rolls.front().roll_date.
- **[H2]** Surface backtest + projected-VaR FREEZE the universe at first clock date (spy_dispersion_backtest.cpp:531,604 use universe_at(front().date)) — membership/weights never updated; NOT point-in-time, inconsistent with listed path's per-roll universe_at(ref.date). **Affects the flagship benchmark path.** Fix: re-resolve universe_at(base.date) in DispersionStrategy::on_step.
- **[H3]** A constituent can never LEAVE the basket — universe_at carries latest row/symbol + read_universe:205 rejects weight≤0 → no removal encoding; basket only grows/reweights (dispersion_workflow.cpp:232-245). Fix: zero-weight removal sentinel OR full-PIT-snapshot rows.
- [M2] read_universe doesn't dedup (effective_date,symbol); std::sort non-stable → nondeterministic weight on differing dup rows. Fix: reject dup keys.
- [M1] verify requires reference_reconciliation.tsv (written only by tools/reference_spy_dispersion.py, never C++) + only checks existence/size, never numbers. Fix: native reference reconcile + numeric compare.
- [M3] Entry-mark reconcile float-EXACT (tol 0.0, reconciliation.hpp:87) between build-route evaluate(Price) and reconcile-route fair_value → silent hard-abort if either shifts 1 ULP. Fix: few-ULP relative tol.
- M4 index "SPY" hardcoded in library (dispersion_workflow.cpp:224,238); M5 clock-gap>roll_dte → synthetic-expiry settlement error; L1-L4.

**EXAMPLE→LIBRARY: ~620 of 761 driver LOC are library workflow;** only main + tiny parse helpers + profile dumps are true glue. Targets (block→API): build_corpus_command → `build_dispersion_corpus` + `DispersionCorpusPolicy` (pinned admission constants/fingerprints move as named defaults) · build_schedule_command → `build_listed_dispersion_schedule` · run_backtest_command → `run_listed_dispersion_backtest` (fold H1) · run_projected_var_command → `run_dispersion_projected_var` + VaR serializers · verify_command → `verify_dispersion_run` (fold M1) · surface config assembly → `dispersion_backtest_config_from_run_spec` · persist/verify_occ_ess_evidence → occ_ess module · load_listed_quotes + inventory/methodology writers. **Common blocker: pinned fingerprint strings/thresholds are inline literals load-bearing for `verify` reproduction.**

---

## 02 — PERFORMANCE  [report: 02-performance.md]
**STEP COST MODEL** (daily-hedge, ~22-unique book, Auto ISA, analytic greeks): ≈**6 boundary-solve-equiv/unique/step** = execute FullGreeks bundle (**5 solves**: base+σ±+r±, backtest.cpp:1716) + compute_step shifted-mark (1, AVX2). compute_step base-greek leg = **0 — reused** from execute@prev step (L1 stamp, portfolio_pricer.cpp:1587-1596). **The 5-solve base bundle = ~83% of solve volume and runs SCALAR per-contract in production Auto.** (06's "base greeks solved twice" ALREADY FIXED by L1.)

TOP LEVERS (ranked):
1. **Cross-uid greek packing [BIGGEST]** (portfolio_pricer.cpp:806-817/642-691): dispersion book = 1 straddle/name ⇒ every (uid,side) group is a SINGLETON ⇒ SIMD packs 1-wide (75% empty). AL boundary is uid-agnostic once resolved — pack same-side singletons across names into 4-lane bundles. Two-phase: (A) scalar-resolve each unique→(S,K,T,σ,r,q,side); (B) gather ALL puts→one laned american_put_greeks_batch(n≈11), all calls→call kernel, scatter back by index. 22 scalar bundles→~6 four-lane packed. **~1.6-2.3× total** (~2-4× on the base-greek bundle). Needs #4 call kernel + #2 gate + deterministic pack membership (mirror marks tile schedule :788-801).
2. **Flip laned-greeks Auto gate** (priced_surface.cpp:1070-1073 hard-gated resolved_price_isa==ForceAvx2 though kShipAvx2Greeks=true, american_boundary_batch.cpp:153): prereq for #1; standalone ~1.5-2× on multi-strike books. Needs economic-parity gate (greeks bit-differ AVX2 vs scalar).
3. **rho-drop risk-tier P&L** (portfolio_pricer.cpp:1595,1649 forces base_greek_needs.full()): dr≈0⇒pnl_rho≈0; K4 selectors wired (priced_surface.cpp:648); drop r± → 5→3 solves = −40% ⇒ ~1.3-1.5× (composes with #1 → ~2.5-3×). Output change (pnl_rho→0), PM-gated.
4. **Call-side laned greeks kernel** (priced_surface.cpp:1127-1135 sends calls scalar; kernel PUT-native): dispersion 50% calls — without it #1/#2 halve.
5. Async prefetch (snapshot_cache.cpp:224-255 synchronous, no load/compute overlap despite backtest.hpp:322): single-digit% now (mmap made opens cheap).
6. Subset-load + zero-copy views (strategy overload loads all surfaces, backtest.cpp:1838/1206-1220; still owned-reconstruct not PricedSurfaceView): bounded.
7. Batch surface resolves (priced_surface.cpp:1113,1146 scalar total_variance; essvi_batch_avx2 unused on query path): fold into #1's resolve phase.
8. n_threads sweep / fat AoS ContractPx (~100B gather :830,882): low.

**BUILD-CORPUS = the true wall for long backtests:** ≈0.87 s/session build vs ≈3 ms/session replay (~290× heavier); dominates until ~290 re-runs amortize one build. Per-board single-threaded, **8 E-cores idle** (P-core-only pin, surface_db_populate.cpp:252-254) → +40-70%; scalar per-strike de-Am inversion (calib.cpp:1019-1191) SIMD-batchable → ~1.4-1.7×. Together ≈ **halve build wall (~25 surf/s)**.

---

## ============ CROSS-CUTTING ROLL-UP (PM) ============

### PERFORMANCE (the "boost speed" mandate) — ranked
PA. Cross-uid greek packing (singleton-book pathology) → ~1.6-2.3× replay. [02#1]
PB. Flip laned-greeks Auto gate + call-side kernel (prereqs for PA). [02#2,#4]
PC. rho-drop risk tier (5→3 solves, PM-gated output change) → composes to ~2.5-3×. [02#3]
PD. build-corpus: use E-cores + SIMD de-Am inversion → ~2× build (the real wall for long runs). [02]
PE. Async prefetch + zero-copy views on the strategy load overload (bounded post-mmap). [02#5,#6]

### CORRECTNESS
CA. [H2] surface-backtest FREEZES universe at day-1 (not point-in-time) — flagship path. [01]
CB. [H1] run-backtest aborts on deferred first roll. [01]
CC. [H3] constituents can never leave the basket. [01]
CD. [M2] non-dedup/non-stable universe sort → nondeterministic weights. [01]
CE. Look-ahead CLEAN, vega-flat CORRECT, greek-explain reconciled 1e-8 (preserve). [01]

### CONFIG / FEATURES / UNWIRED
FA. Config TSV-soup seam: surface CLI honors 6/20 spec keys; strict typed DispersionRunConfig needed. [03]
FB. [HIGH] frictions + financing exist but UNEXPOSED to dispersion (frictionless mid-fills). [03]
FC. [HIGH] no risk limits / capital / drawdown-stop in loop. [03]
FD. Hardcoded: side, hedge cadence, strike rule, weighting scheme, fit preset, multiplier=100. [03]
FE. Surface path never calls tearsheet() (no headline stats, no benchmark-relative IR/alpha/beta). [03]
FF. entry_every_n ignored; run-projected-var half-wired; verify's numeric check absent. [01,03]

### EXAMPLE→LIBRARY / MODULARITY
EA. ~620/761 driver LOC = library workflow; extract 8 named APIs. [01]
EB. Typed strict DispersionRunConfig + DispersionBacktestOutcome{track,sheet,drops,per_roll}. [03]
EC. Move pinned admission fingerprints/thresholds to named library defaults (verify reproducibility). [01]
ED. write_dispersion_artifacts + open_dispersion_corpus + verify_dispersion_run library entry points. [01,03]
