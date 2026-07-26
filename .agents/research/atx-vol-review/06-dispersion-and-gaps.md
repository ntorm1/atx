# atx-vol Review 06 — DISPERSION stack + fitting/backtest gaps

Reviewer: senior quant (read-only audit). Repo `C:/atx`, module `atx-vol/`.
Scope: Part A = the vega-flat dispersion benchmark end-to-end; Part B = cross-cutting
dead-code + feature-gap sweep. No code was built or edited.

---

## 0. Orientation — there are TWO dispersion families (both "vega-flat")

| Family | Sizing engine | Structure | Driver(s) | "vega-flat" mechanism |
|---|---|---|---|---|
| **Straddle book** | `build_dispersion_book` (`dispersion.cpp`) → `DispersionStrategy` | ATM-fwd straddle (call+put) | `atxvol_spy_dispersion_backtest run-surface-backtest` | explicit `target_vega / straddle_vega` per leg |
| **Strangle DSL** | `make_dispersion_strangle_spec` (`dispersion_strangle.cpp`) → `DeclarativeStrategy` | 40Δ strangle | `mag7_dispersion_backtest`, `spy_dispersion_pnl` | `CrossLegConstraint::FlatVega` scales index gross vega to basket gross vega |
| **Listed proxy** | `select_listed_dispersion` + schedule/reconciliation | listed 100-share straddle | `atxvol_spy_dispersion_backtest run-backtest` | schedule `gross_index_vega_target_per_vol_point` |

The **SOTA speed benchmark (fit→price→risk hot path with `backtest_profile.tsv`)** is the
**straddle book** path: `run-surface-backtest`. The strangle drivers time `run_backtest`
wall-clock (steps/s) but only against a pre-fitted `SurfaceDb`.

---

## PART A — DISPERSION STACK FINDINGS (by severity)

### CORRECTNESS

**A1 (PASS / positive) — vega-flat neutralization math is correct.**
`dispersion.cpp:488-496`. `index_leg.straddle_qty = ±target_vega/(straddle_vega·mult)`;
`name.straddle_qty = ∓ (w_i/Σw)·target_vega/(straddle_vega·mult)`. Index gross vega = `target_vega`;
Σ name gross vega = `target_vega·Σŵ = target_vega`; opposite signs → net vega ≈ 0 at entry.
Renormalization is over **survivors** (`sum_w` at `:479-486`), so a DropRenormalize drop preserves
neutrality. Leg construction (A-2): names weighted by normalized **index weight**, index unweighted —
standard dispersion allocation. Put is forced onto the call's exact K/expiry
(`dispersion.cpp:193-199`) rather than independently re-resolving ATM — correct (one concrete
listed-style K/expiry pair). Implied-correlation closed form + degenerate-denominator guard
(`:381-388`) correct.

**A2 (Medium) — "fit→price→risk" is split across two processes; the surface benchmark does NOT
fit.** The FIT happens once in `build-corpus` (`spy_dispersion_backtest.cpp:232`, HFT preset,
LinearVariance curve, calendar floor, admission gate) and is serialized to `.atxvsa` archives.
`run-surface-backtest` **reloads** priced surfaces and exercises **price + risk only** (American
Andersen-Lake solves per query). So "how fast are we" for `run-surface-backtest` measures
price+risk throughput on cached fits, not fit throughput. This is correct for a backtest but must be
stated when quoting the number — the fit hot path is measured by `build-corpus`, not the backtest.
The engine passes `cfg.price` (PriceOptions) to `on_step` (`backtest.cpp:1771`), so the **full-greek
seed path** (`full_greek_seed` per side) IS exercised — the benchmark is not short-circuiting price/risk.
`entry_risk_seeds` reuse (`backtest.cpp:1503-1507`) avoids re-solving entry-day legs — an
optimization, not a short-circuit.

**A3 (Low) — implied-correlation signal is inert in the speed benchmark.**
`run_surface_backtest_command` (`spy_dispersion_backtest.cpp:523`) never sets
`config.record_diagnostics`, so `DispersionStrategy::signals()` returns `{}` early
(`dispersion_strategy.cpp:192`) and `dispersion_signal`/`resolve_atm_iv` never run in the benchmark.
Harmless, but the signal cost is not part of the measured path.

### PERFORMANCE

**A4 (High) — two full-book American-pricing passes per step.** With daily delta-hedge
(`make_dispersion_backtest_strategy` sets `HedgeSpec::Cadence::Daily`, `dispersion_backtest.cpp:33`)
`hedge_fires` every day, so each step runs:
(1) `Execution` `price_into(FullGreeks)` (risk + hedge delta + row greeks) `backtest.cpp:1505`, AND
(2) `StepPnl` `pnl_totals(base,shifted)` `backtest.cpp:786-790`.
Both evaluate the **same base-date surfaces**; base greeks are solved twice. Feeding the FullGreeks
base frame into the pnl base leg would remove one American solve per position per day — the single
biggest throughput lever in the loop. (Row greeks already reuse `ex->book_greeks`, `:1798` — good.)

**A5 (Medium/High) — snapshot load maps ALL universe surfaces per date, prices only traded legs.**
`SnapshotLoad`/`ArchiveMap` (`backtest.cpp:971-1028`) materialize every symbol's surface for the
date; only 2·(1+N) straddle legs are queried. The in-file seam note (`backtest.cpp:995`) states the
PortfolioPricer still takes fully-decoded surfaces, not zero-copy `PricedSurfaceView`s — re-pointing
at views is called out as pending. For the 50-name core universe this is a real per-step tax.

**A6 (Low) — daily hedge forces a FullGreeks pass even when net delta is inside the band.** The band
is applied after pricing (`hedge_ledger.hedge_daily`, `backtest.cpp:1604`); the book is always
re-priced to obtain delta. A cheap delta-only mask could skip full-greek work on no-hedge days.

**Throughput bottleneck ranking (surface benchmark):** American solve volume (A4 doubles it) →
SnapshotLoad/ArchiveMap of the full universe (A5) → per-step allocation is already retained/reused.
No re-fit occurs in the loop. AVX2 batch kernels exist (`src/simd/*_avx2.cpp`, runtime CPUID
dispatch) — confirm the per-step pricer actually routes legs through the batched path.

### WIRING

**A7 (PASS) — the vega-flat surface benchmark is fully runnable from `C:/atx-data/spy-dispersion/`
after a populate.** `run-surface-backtest` needs only `run_spec.tsv`, `universe_schedule.tsv`,
`surface_manifest.tsv`, and the `archives/` — all produced by `build-corpus`. OPRA parquet
(`opra/<SYM>/<date>.parquet`), `spy_dispersion_definitions.tsv`, `occ-ess/`, and
`sessions_calendar.txt` are present. See the RUN RECIPE below.

**A8 (Medium) — `verify` requires an artifact the C++ driver never writes.** `verify_command`
(`spy_dispersion_backtest.cpp:447`) demands `reference_reconciliation.tsv`, but no C++ command emits
it — it is written by the external `tools/reference_spy_dispersion.py:389`. The full `verify` gate is
therefore a cross-language step. Does not affect `run-surface-backtest`; flag for the run book.

---

## HOW TO RUN THE VEGA-FLAT DISPERSION BENCHMARK

**Target:** `atxvol_spy_dispersion_backtest` (CMake target, `atx-vol/CMakeLists.txt:283`).
Requires `-DATX_BUILD_EXAMPLES=ON`. For the speed numbers add `-DATX_VOL_PROFILE=ON`
(emits `backtest_profile.tsv`) and optionally `-DATX_VOL_COUNTERS=ON` (emits `backtest_counters.tsv`).

**A populate step IS required first** (surfaces are fitted in `build-corpus`, not in the backtest):

```
# 1. POPULATE: OPRA parquet -> fitted .atxvsa archives + manifest  (the fit hot path)
atxvol_spy_dispersion_backtest build-corpus \
    --spec C:/atx/atx-vol/examples/spy_dispersion_run_spec.tsv \
    --out  C:/atx-data/spy-dispersion/runs/surface-bench

# 2. VEGA-FLAT SURFACE BACKTEST: the SOTA price+risk throughput benchmark
atxvol_spy_dispersion_backtest run-surface-backtest \
    --run  C:/atx-data/spy-dispersion/runs/surface-bench
#   -> surface_backtest.tsv (+ backtest_profile.tsv, backtest_counters.tsv when built with the flags)
```

- Spec `spy_dispersion_run_spec.tsv`: `opra_root=C:/atx-data/spy-dispersion/opra`,
  `date_lo=2026-01-02 date_hi=2026-04-30`, universe = `spy_dispersion_universe.tsv`
  (development EQUAL-weight proxy, `min_names=10`), `target_dte=30 roll_dte=7 gross_index_vega=10000
  delta_band=0`, `fit_workers=0` (all cores). Config surfaced via `DispersionBacktestConfig`
  defaults: `entry_every_n=21`, `project_to_calendar_expiry=true`, `record_diagnostics=false`.
- Core 50-name gate: use `spy_dispersion_core_run_spec.tsv` (`core_mode=1`, `min_names=40`,
  `opra_root=C:/atx-data/spy-dispersion-core/opra`) — a *separate* data root.
- **Fast path (skip populate):** any prebuilt corpus under `C:/atx-data/spy-dispersion/runs/*`
  that has `archives/ + surface_manifest.tsv + run_spec.tsv + universe_schedule.tsv` can be fed
  straight to `run-surface-backtest --run <dir>` (e.g. `runs/listed-dev-v3`). Verify the archive set
  covers the manifest dates first (some dev runs are short slices).
- The strangle-family benchmarks are different binaries over a `SurfaceDb` (no OPRA/parquet step):
  `mag7_dispersion_backtest --db DIR` and `spy_dispersion_pnl --db DIR --universe FILE` (prints
  `steps/s`, writes `pnl_track.tsv`).

---

## PART B(a) — DEAD-CODE / TODO INVENTORY

**Headline: the dispersion + fitting/backtest module contains no true dead-code stubs.** No `#if 0`
blocks, no empty bodies, no `return {}`/`return 0` placeholder implementations, no unreferenced
exported functions were found in the dispersion stack (every public function is exercised by
`examples/` and `tests/` — verified by cross-repo grep, 181 call sites across 22 files). The only
markers are **fail-closed guards** that reject persisted-but-unsupported options and a handful of
data-sentinel/architectural "placeholder" comments.

| file:line | marker | what it is |
|---|---|---|
| `src/calib.cpp:864` | "not implemented" | rejects parametric interval loss (guarded Err, not a stub) |
| `src/calib.cpp:874` | "not implemented" | rejects requested eSSVI rho mode |
| `src/calib.cpp:880` | "not implemented" | rejects asymmetric eSSVI rho |
| `src/calib.cpp:885` | "not implemented" | rejects configurable eSSVI fallback threshold |
| `src/calib.cpp:889` | "not implemented" | rejects configurable butterfly grid |
| `src/calib.cpp:923` | "not implemented" | rejects requested residual basis |
| `src/vol_curve.cpp:731` | "not implemented" | `refit_slice_curve`: SplineVol local refit unsupported → Err |
| `include/atx/vol/calib.hpp:297` | comment | documents the reject-unimplemented-policy contract |
| `src/session.cpp:1156-1158,1207` | "Placeholder eSSVI VolSurface" | intentional: override path reads override, `surface_` placeholder unused |
| `include/atx/vol/priced_surface.hpp:37` | comment | same override-path placeholder note |
| `src/earnings_forecast_loader.cpp:24,120,188` | "placeholder" | SpiderRock 1970 data sentinels, not code |
| `src/profile.cpp:274` | "placeholder" | HTB_DIVIDEND_NAME routing comment |

Interpretation: the six `calib.cpp` + one `vol_curve.cpp` "not implemented" rows are **feature gaps
enumerated as guards** (see Part B(b)#2) — good engineering (fail-closed), but they mark real missing
calibrator capabilities. No action needed for dead-code cleanup; they belong on the feature backlog.

**Wiring gap (not dead code):** `reference_reconciliation.tsv` is required by `verify`
(`spy_dispersion_backtest.cpp:447`) but produced only by `tools/reference_spy_dispersion.py` — a
C++/Python seam (see A8).

---

## PART B(b) — FEATURE GAPS for a SOTA options backtesting platform (prioritized)

1. **Intraday / multi-snapshot backtest.** The whole pipeline is one snapshot minute per day
   (`snapshot_suffix=T19:55:00Z`). No intraday path, no event-time (open/close/earnings-print)
   snapshots. Limits realistic hedge timing and slippage. *(High)*
2. **Full calibrator option surface.** Seven guarded "not implemented" paths (parametric interval
   loss, eSSVI rho modes, asymmetric rho, configurable fallback/butterfly grid, residual basis,
   SplineVol local refit). A SOTA platform should ship these, not reject them. *(High)*
3. **Risk limits & capital model.** No max-vega/gamma/notional guards, no margin/buying-power model,
   no drawdown circuit-breaker or stop-out in `run_backtest`. Backtests run unconstrained. *(High)*
4. **Market-impact / liquidity-aware costs.** `FrictionModel` = flat bps or vol-ticks half-spread +
   per-contract cost + hedge slippage. No participation/impact, no per-name spread from quoted
   width, no borrow term structure (only `flat_rate`), no assignment/exercise fees. *(High)*
5. **Parameter / scenario sweep harness.** Each run is one config; `run-projected-var` parallelizes
   scenarios but there is no grid-sweep orchestrator over (delta, tenor, vega, universe). SOTA needs
   built-in fan-out + result aggregation. *(Medium/High)*
6. **Index/universe reconstitution realism.** PIT schedule discipline is good, but weights are a
   supplied `development_equal_weight_proxy`; official reconstitution weights and corporate-action
   symbol remap (GOOG/GOOGL dedup is hand-coded in `spy_dispersion_pnl.cpp:144`) are not first-class.
   *(Medium)*
7. **Attribution & tearsheet depth.** Taylor PnL-explain terms (delta/gamma/vega/vanna/volga/theta/
   rho/charm) are computed per step but the tearsheet is headline-only; no per-leg / per-name / factor
   attribution export, no HTML/plot from C++ (all rendering is external Python). *(Medium)*
8. **Live/paper parity path.** The listed reconciliation (`reconcile_listed_dispersion` +
   `validate_listed_reconciliation_backtest`) gives backtest-vs-reference replay parity, but there is
   no order-management / fill-simulation / live-adapter layer, so backtest↔live parity is unproven.
   *(Medium)*

Runner-up gaps: single unified run-manifest (git SHA + config hash + seed) instead of scattered
fingerprints; corporate-actions (splits/special dividends) handling in the dispersion path;
determinism gate depends on external Python (`reference_spy_dispersion.py`).
