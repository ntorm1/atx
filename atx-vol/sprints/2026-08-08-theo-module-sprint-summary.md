# Theo Module Sprint — Summary

**Dates:** 2026-08-08 (single day)
**Branch:** `feat/theo-module` (worktree `C:\atx-wt\pool-9`), base `5338455` on `main` `92f26e0`
**Plan:** `docs/superpowers/plans/2026-08-08-atx-vol-theo-module.md` · **Ledger:** `.superpowers/sdd/2026-08-08-atx-vol-theo-module/progress.md`
**Method:** subagent-driven development — fresh implementer per task, spec + quality review per task, fix loops, controller as project manager; closeout (this task) run under the lifted "focused suites only" directive (full gate is this task's job).

## Goal

Ship the theo-vol module: realized-vol estimators, a breakeven-vol replay/
solve/batch/loader label pipeline (research-doc system stages), a theo
overlay engine with an identity contract against the served surface mark,
fair-vol/event-variance overlays, an `IFairVolModel` ML seam, a batch sheet
API, and read-only edge signals wired into the backtest engine — with no
positions taken and no change to the existing engine's step loop.

## Shipped surface

### `atx-vol/include/atx/vol/realized_vol.hpp` + `src/realized_vol.cpp` (Task 1)

Greenfield Tier-B module (outside the `vol.hpp` umbrella), five OHLC
realized-vol estimators plus a trailing-window panel:

- `OhlcBar`, `RvEstimator` (`CloseToClose`, `Parkinson`, `GarmanKlass`,
  `RogersSatchell`, `YangZhang`).
- `Result<double> realized_vol(bars, est, annualization = 252.0)`.
- `RvPanel{vol: array<double,4>, window: array<uint16_t,4>{5,21,63,252}}` +
  `Result<RvPanel> realized_vol_panel(bars, est = YangZhang, annualization)`.

Pure functions, no shared state, no exceptions across the API boundary.
`RvPanel` is consumed verbatim by Task 8's RV-blend overlay and Task 9's
fair-vol feature vector.

### `atx-vol/include/atx/vol/breakeven.hpp` + `src/breakeven.cpp` + `src/backtest.cpp` (Tasks 2–5)

The label-factory pipeline, four layers, each building on the one below
without touching it:

- **Replay** (Task 2): `BevDayState`, `BevSpec`, `BevReplayConfig` (arity 5),
  `BevReplayResult`, `Result<BevReplayResult> bev_replay_pnl(path, spec,
  sigma, dividends, cfg)` — fixed-sigma delta-hedged single-option replay.
  Implementation lives in `src/backtest.cpp` (see "Engine-extension
  architecture" below), not a new `.cpp`.
- **Root-find** (Task 3): `BevSolveConfig` (arity 4), `BevFlag`
  (`Ok`/`NoBracket`/`ExercisedEarly`/`MaxIter`), `BevLabel`,
  `Result<BevLabel> solve_breakeven_vol(path, spec, dividends, cfg)` —
  bounded bisection over `bev_replay_pnl`, strict-sign-change bracket test
  (a boundary exact-zero PnL is `NoBracket`, not a resolved root).
- **Batch** (Task 4): `BevJob` (non-owning path/dividends spans + spec),
  `BevLabelFrame` (SoA, `status_ok`-gated), `Result<BevLabelFrame>
  solve_breakeven_batch(jobs, cfg, n_threads)` — deterministic
  `parallel_for` fan-out, bit-identical across thread counts, no allocation
  inside the parallel region.
- **Path loader** (Task 5): `BevExpirySnap` (`Exact` /
  `LastSessionAtOrBefore`), `BevPath` (`days`, `settle_ts_ns`, `snapped`),
  `Result<BevPath> load_bev_path(clock, uid, entry_ts_ns, expiry_ns,
  tenor_probe_years, snap)` — builds a replay-ready path from real
  `PricedSurface` corpora via `Clock`/`MarketSnapshot`, carry recomputed
  fresh per session against the *requested* (not snapped) expiry.

### `atx-vol/examples/bev_label_factory.cpp` (Task 6)

`BevFactoryArgs` + `Result<int> run_bev_label_factory(const BevFactoryArgs&)`
+ a `main()` guarded by `ATX_BEV_LABEL_FACTORY_NO_MAIN` (so the gate test
reaches it by textual include, ODR-safe, single TU). Walks entry dates,
resolves a delta-lattice strike ladder (5%-step delta grid, re-checked
against each resolved strike's actual `|delta|`), loads paths, and solves
one deterministic `solve_breakeven_batch` call across every accumulated job.
Emits a byte-deterministic TSV: `entry_ts_ns, expiry_ns, strike, side,
sigma_be, sigma_entry_iv, log_ratio, ...` (label factory feature/target
columns feeding `IFairVolModel` training, Task 9's schema). Carry-only —
no earnings/event-calendar code in the driver. All-skip is fail-loud (a
zero-row run is `Err`, not a silently-valid empty TSV).

### `atx-vol/include/atx/vol/theo.hpp` + `src/theo.cpp` (Tasks 7–10)

Tier-B engine (outside `vol.hpp`), the theo-vol overlay measure beside the
served surface mark:

- **Core vocabulary + engine** (Task 7): `TheoQuery{strike, tenor_years,
  side}`, `TheoFlagBits` (`Extrapolated` reserved, `OverlayClamped`,
  `ModelMissing`, `FastTierRoute`), `TheoValue{theo_vol, theo_price,
  market_vol, market_price, edge_vol, band_vol, flags}`, `TheoContext{surface,
  events, rv}`, `OverlayAdjust{dvol, band, flags}`, `ITheoOverlay` (pure
  `(ctx, query) -> adjustment` interface), `TheoConfig` (arity 3:
  `band_floor_vol`, `max_abs_dvol`, `price_theo`), `kTheoMaxBatch = 256`,
  `TheoEngine::create/value/value_into`. Zero-overlay identity holds
  bit-for-bit by construction (`theo_vol == market_vol`, `theo_price ==
  market_price`, `edge_vol == 0.0` — literally the same doubles, not a
  tolerance match). `FastTierRoute` names the cached-correction-vs-cold-
  reprice residual on fast-tier surfaces rather than folding it silently
  into the price-space edge.
- **RV-blend + event-variance overlays** (Task 8): `RvBlendConfig` (arity 3)
  + `make_rv_blend_overlay` — `dvol = weight * exp(-T/tenor_damp_years) *
  (rv_anchor - market_vol)`, anchored on `RvPanel::vol[rv_window_idx]`.
  `EventVarConfig` (arity 2) + `make_event_var_overlay` — strips the
  market's implied event move via `event_vol.hpp`'s censoring/recombination
  machinery and re-injects a forecast eMove. Both degrade to `{dvol=0,
  ModelMissing}` on missing/out-of-domain context rather than failing the
  batch.
- **`IFairVolModel` ML seam** (Task 9): `kFairVolFeatureCount = 8`,
  `kFairVolFeatureSchemaV1 = 1`, fixed feature order (`log_moneyness,
  tenor_years, market_vol, rv_21d, rv_63d, iv_minus_rv,
  n_events_to_expiry, delta_abs`), `IFairVolModel::feature_schema()` /
  `predict(features_row_major, n_rows, log_ratio_out)`,
  `load_linear_fair_vol_model(coef_tsv_path)` (v1 linear model, `#
  schema=<n>` TSV), `make_fair_vol_model_overlay(model)` — schema-checked
  at construction (a model trained against another schema is refused, not
  silently wired in), batches one `predict` call per `kTheoMaxBatch`-sized
  chunk, fails open per-row (`ModelMissing`) but fails loud on a broken
  `predict` call.
- **Batch sheet API + perf pass** (Task 10): `Result<std::vector<TheoValue>>
  compute_theo_sheet(ctx, engine, queries)` — one-line allocate-then-
  `value_into` convenience (mirrors `compute_surface_analytics`). Perf pass
  on `value_into` itself: M1 fuses the `iv()`+`fair_value()` double-resolve
  into one `surface.evaluate(...)` call; M2 skips the overlay scratch/band
  arrays entirely when zero overlays are engaged. Both verified against the
  pre-existing Task 7 identity/chunk-boundary tests, unmodified — old tests
  stayed green, not new tests tailored to the new code.

### `atx-vol/examples/spy_leaps_strangle_backtest.cpp` — `TheoEdgeSignalStrategy` (Task 10)

A read-only `IStrategy` decorator (mirrors `SwapSignalProbe`'s split): every
virtual except `signals()` forwards unchanged to the wrapped strategy — no
order/hedge/NAV path of its own. `on_step` mirrors one degenerate daily OHLC
bar (`O=H=L=C=spot`) into an unbounded `RvPanel`-feeding history (`CloseToClose`
estimator, since a once-daily snapshot has no real intraday range);
`signals()` queries the theo engine ATM at the strategy's target LEAPS tenor
and emits `theo_edge_atm`/`theo_band_atm` (always both, NaN when
unmeasurable). New `--no-theo-signals` CLI flag (default off — signals on
by default). NAV byte-identity proven on real SPY 2019 data: 899/899
shared `track.tsv` cells identical between signals-on/off runs (see Task
10's report for the full evidence trail).

## Measured perf

Bench target: `atx-vol-theo-bench` (`bench/theo_bench.cpp`), built and run
under the `rel` preset (Release, x64 default SSE2, i7-1260P, 16 logical
CPUs @ 2496 MHz) — the two conceptual bench groups this sprint added:

| case | median | CPU median | throughput | CV |
|---|---:|---:|---:|---:|
| `bev/solve/126d_al_fast` (Task 4) | 109 ms/label | 107 ms | 9.33 labels/s | 7.76% (time) / 7.35% (cpu) |
| `bev/batch/64jobs`, auto workers (Task 4) | 1225 ms/batch | — (`UseRealTime`) | 52.26 labels/s | 8.51% |
| `theo/sheet_200q/price_theo_true` (Task 10) | 11.6 ms/sheet | 10.9 ms | 18.29k queries/s | 15.85% (time) / 17.49% (cpu) |
| `theo/sheet_200q/price_theo_false` (Task 10) | 5.29 ms/sheet | 5.30 ms | 39.82k queries/s | 13.82% (time) / 13.58% (cpu) |

Extrapolated: single-threaded `bev/solve` ⇒ ~33,600 labels/hour; auto-worker
`bev/batch` ⇒ ~188,100 labels/hour (this host's 16 logical, P/E-hybrid core
mix, static contiguous-block partition — no rebalancing by design).
`price_theo=true` pays a second cold Andersen-Lake reprice at the shifted
`theo_vol` on top of the market-price solve every query already pays;
`price_theo=false` skips it (the vol-space-only screening path) — ~2.2x
faster here, consistent with Task 10's original clean-window measurement
(2.03x at `price_theo_true` 26.18k/s, CV 2.87%; `price_theo_false` 53.19k/s,
CV 1.51%).

**Dedup disclosure (`bench/ANCHORS.md` §5):** the 200-query sheet covers
exactly 40 unique `(K, T, side)` triples (5 strikes × 4 tenors × 2 sides),
each repeated 5x — `dedup_ratio=5`, `n_unique=40`, `n_queries=200` (exact
integer counters, confirmed every run). `items_per_second` above is
therefore a **query** rate over a 5x-duplicated set, not a unique-contract
rate.

**Build config / CV caveat.** All four rows above were captured on a
shared host with the full `atx_vol_fast`/`atx_vol_slow`/forcescalar ctest
gate running concurrently in the background (this same closeout task) — the
box was not quiescent, so every CV here exceeds the repo's 5% quiet-window
gate and none of these rows are checked in as a `bench/baselines/*.json`
citable baseline. They are reported honestly, per this repo's own noisy-host
disclosure precedent (`bench/README.md`, `bench/ANCHORS.md` §5, Task 10's
own fix-round rerun). Medians and the true/false ratio are consistent in
order of magnitude with the two source tasks' own numbers: Task 4's ad hoc
**Debug** smoke run put `bev/solve/126d_al_fast` at ~248 ms/label — this
sprint's **Release** (`rel`/SSE2) measurement at 109 ms/label is ~2.3x
faster, the expected Debug-vs-Release direction (Task 4 flagged that its
Debug number came out unusually close to the plan's Release-class estimate;
this Release measurement resolves that in the expected direction). Task
10's original clean-window `theo/sheet_200q` numbers (26.18k/53.19k
queries/s) remain the more citable reference for that bench; this run's
15.85–13.82% CVs are contention noise from the concurrent gate run, not a
regression.

## Residual-work register

House convention: this is a roadmap/product residual list, not code-review
minors (those are handled by the final branch review).

**From the brief (verbatim in intent):**

1. **B3 rule unification post-lakehouse-merge.** `bev_replay_pnl`'s
   early-exercise decision (`bev_should_exercise_early`, B3-style optimal
   exercise for the long side) is a replay-path-only pure rule, duplicated
   in spirit from — but not unified with — any future book-level
   exercise/assignment model. Unification is deferred to whenever the
   lakehouse merge lands a real book-level exercise model to unify against.
2. **Quantile heads on `IFairVolModel`** — `OverlayAdjust::band` from the
   fair-vol model overlay is `|dvol| * 0.5`, a fixed placeholder multiplier,
   not a model-reported uncertainty. Ships once the model interface grows a
   second `predict`-like entry point returning a distributional spread.
3. **Dividend/borrow inputs for single names.** The corporate-actions store
   is Python-side; the C++ consumer is `atx-engine` only. `breakeven.hpp`'s
   `DividendEvent` plumbing accepts caller-supplied events but has no C++-side
   corporate-actions source of its own.
4. **Label storage beyond TSV.** Parquet deferral is a house rule — the
   label factory (Task 6) writes TSV only; revisit with the lakehouse.
5. **Event-days-excluded label variant** (research doc §8.2 item 4) — not
   implemented; the label factory has no earnings/event-calendar exclusion
   knob.
6. **Purged-CV/embargo tooling** lives Python-side with the trainer,
   out of C++ scope for this module.

**Sprint-accumulated:**

7. **`TheoFlagBits::Extrapolated`** is reserved but never set — no surface
   extrapolation predicate (`PricedSurface::extrapolates_tenor` or similar)
   is exposed to the engine yet to drive it.
8. **`FairVolModel` band = `|dvol| * 0.5` placeholder** until quantile heads
   ship (same item as #2 above — recorded once, cross-referenced here since
   both the brief and the sprint's own accumulated-residuals list named it).
9. **The theo signal probe (`TheoEdgeSignalStrategy`) assumes a SPY corpus**
   regardless of `--db-prefix` — `theo_edge_atm`/`theo_band_atm` are NaN for
   any other underlier. No symbol-plumbing was added (out of Task 10's
   scope by the review's own ruling).
10. **`dev-counters` `SurfaceScalarPriceRoutes`** now additionally counts
    theo-sheet resolves via Task 10's M1 fused `evaluate()` route change
    (compiled out by default — `dev-counters` is an opt-in instrumentation
    preset, not part of the default build).
11. **`RvPanel` rebuilt per recorded row** inside `TheoEdgeSignalStrategy::
    signals()` — O(n²) over a run's full `bars_` history, rebuilt fresh every
    step rather than incrementally. Negligible next to the run's own pricing
    cost (single-surface-snapshot-per-step American solves dominate), so not
    optimized in this sprint.

## Engine-extension architecture decision

Per user mandate, `bev_replay_pnl` (the breakeven replay's fixed-sigma
delta-hedged single-option P&L) is appended to the END of
`atx-vol/src/backtest.cpp` — not a new `.cpp`, not a parallel replay engine —
specifically so it can reuse that translation unit's file-local
`HedgeLedger` share ledger and mirror the existing engine's rebalance,
expiry-settlement, and cash-financing accounting verbatim rather than
re-deriving a second copy of that bookkeeping. The extension is genuinely
append-only: `git diff` on that file is exactly two hunks, a single new
`#include "atx/vol/breakeven.hpp"` line inserted in alphabetical order among
the existing includes, and a pure append after the file's closing
`} // namespace atx::vol`. `run_backtest`'s own step loop, `RunConfig`, and
`HedgeLedger`'s class definition are byte-untouched by this sprint — the
replay is an independent function that happens to borrow the same ledger
type, not a hook into the engine's execution path.

## Validation state

See `.superpowers/sdd/2026-08-08-atx-vol-theo-module/task-11-report.md` for
the full gate evidence (hygiene lane, targeted ctest run, bench numbers,
commit SHAs). Summary:

- **Hygiene (PCH-off) lane**: the three new modules' library and test TUs
  (`realized_vol.{hpp,cpp}`, `breakeven.{hpp,cpp}`, `theo.{hpp,cpp}` plus
  their `*_test.cpp` TUs) compile clean under `build-hygiene/` — zero
  PCH-masked missing includes, no code changes required.
- **Full `atx_vol_fast` lane — attempted, host-interrupted.** A full
  `ctest -L atx_vol_fast` run (2,779 tests) was launched for this closeout
  and progressed to test 2,288/2,779 before the run was killed by a host
  interruption (the shell/process did not survive an overnight gap in this
  session). Zero non-baseline failures were observed in that partial sweep;
  two of the three pre-registered known-baseline failures were directly
  captured within it (`SurfaceV2Provenance.
  ValidationFallbackAdmissionRecordsTheServedFamily` at test #402,
  `SurfaceDbPopulate.PropagatesStoredSurfacePolicyAndPersistsServedProvenance`
  at test #1993) — consistent with them being pre-existing and unrelated to
  this branch, not a new discovery. Per an amended user directive issued
  after the interruption, the full fast/slow/forcescalar sweep was **not**
  relaunched; the shipped gate for this closeout is the targeted-suite run
  below instead.
- **Shipped gate: targeted `ctest -R` run**, one combined invocation
  covering every suite this sprint touched or that exercises the touched
  files — 115 tests (114 executed + 1 pre-existing unrelated skip), **0
  failures**: `RealizedVol` (5), `Breakeven` (15), `BevPathLoader` (4),
  `BevLabelFactoryGate` (4), `TheoEngineTest` (49) plus 2 incidental
  substring matches on "Theo" (`SurfaceDbPartition.
  WriteOpenLoad_TheoBitIdentical`, `ContractProjection.
  FortyDeltaThreeCalendarMonthCallBecomesConcreteTheo`), `VolUmbrella` (7,
  confirms the Tier-B 35 pin), `BacktestExec` (28, regression insurance on
  `backtest.cpp`'s pre-existing step-loop/ledger code around the
  `bev_replay_pnl` append). The one skip
  (`SpyArchiveRoundTrip.ConvexDense_Serialize_Reload_ReproducesTheoAndAccuracy`)
  is the same pre-existing, `atx_vol_slow`-labelled, unrelated skip every
  task in this sprint recorded in its own gate evidence.
- **Forcescalar lane — skipped, not applicable.** `atx-vol-pricing-forcescalar`
  is a single CTest entry whose `--gtest_filter` is fixed at CMake
  registration time (`ScalarLegEnv.*:American*:AmericanIv*:CorrectionCache*
  :Bulk*:VolaSession*:*Deam*:*Parity*:-*Batch*:AvxBoundary*:VectorMath*
  :Simd*`, `tests/CMakeLists.txt`); `ctest -R` selects among registered
  tests by name, it cannot rewrite that baked-in filter, so there is no way
  to run "just the theo + BEV suites" under this lane — it is all-or-nothing
  by construction (confirmed also in Task 4's report: `Breakeven*` was
  already absent from that filter). None of this sprint's new files
  (`realized_vol`, `breakeven`, `theo`) are in scope for it, and the one
  shared file it touches transitively (`backtest.cpp`, via the append) is
  not one of the scalar-path files that filter exercises either. Skipped
  per the amended directive's explicit fallback.
- Three pre-existing, verified-not-ours baseline failures excluded from
  scope throughout: `SpxWilmottReproUnit` (missing
  `spx-wilmott-repro-tests.exe`), `SurfaceV2Provenance.
  ValidationFallbackAdmissionRecordsTheServedFamily`,
  `SurfaceDbPopulate.PropagatesStoredSurfacePolicyAndPersistsServedProvenance`.

## Commit map

`5338455` plan doc → `0c8d3ec` realized-vol suite (Task 1) → `4d33565`
breakeven replay in backtest.cpp (Task 2) → `1faff07` Tier-B umbrella pin
sync → `faf265d` theo-plan day-count doc fix → `868f7f9` replay
put-exercise/slippage fix → `b687f44` breakeven root-find (Task 3) →
`3c582e8` batch label runner + bench (Task 4) → `c504c05` theo-plan Task 2
amendments doc → `9bd3c05` path loader (Task 5) → `369963a` label factory
driver (Task 6) → `190c827` label factory gate/CLI fix → `156f6f3` theo
vocabulary + engine (Task 7) → `8b06330` fast-tier route flag + band
quadrature/chunk tests (Task 7 fix round) → `8057239` RV-blend + event-var
overlays (Task 8) → `d9e053d` `IFairVolModel` seam (Task 9) → `d280d25`
fair-vol schema enforcement (Task 9 fix round) → `3bacca3` theo sheet batch
+ bench + backtest signals (Task 10) → `a1fa5ba` theo-signals backing gate +
bench dedup fix (Task 10 fix round).
