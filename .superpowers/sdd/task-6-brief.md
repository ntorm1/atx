### Task 6: `examples/mag7_dispersion_backtest.cpp` + gate test

The acceptance example: open db → configure strategy → run → emit data files. Small (≤ ~300 lines) because Tasks 1-5 carry the machinery.

**Files:**
- Create: `atx-vol/examples/mag7_dispersion_backtest.cpp`
- Create: `atx-vol/tests/mag7_dispersion_backtest_test.cpp`
- Modify: `atx-vol/CMakeLists.txt` (examples block: `add_executable(mag7_dispersion_backtest examples/mag7_dispersion_backtest.cpp)`, link `PRIVATE atx::vol atx::core atx_warnings`, comment naming gate test `Mag7DispersionBacktest`)
- Modify: `atx-vol/tests/CMakeLists.txt` (test source)

**Interfaces:**
- Consumes: `Clock::from_surface_db` (T1), `make_dispersion_strangle_spec` + `DeclarativeStrategy` (T2/T3), `run_backtest`, `tearsheet`, all T4 emitters, `SnapshotCache`.
- Produces: the run-output file contract the Python renderer (T7) reads. Output dir layout (pinned):

```
<out>/series.csv             write_backtest_series_csv
<out>/strategy_metrics.csv   write_metrics_csv(meta, strategy_metrics(ts) + result_summary_metrics(r))
<out>/engine_metrics.csv     write_metrics_csv(meta, engine_metrics(stats))
<out>/db_stats.csv           write_surface_db_stats_csv
<out>/populate_stats.csv     copied from <db>/populate_stats.csv when present (byte copy)
```

Shared meta block written into EVERY file (keys exactly): `strategy=mag7_dispersion_strangle`, `names` (comma-joined), `index_symbol`, `data_source=surface_db`, `db_root`, `db_generation`, `window_start`, `window_end` (first/last clock date), `n_steps`, `delta_target`, `tenor_days`, `close_dte_days`, `theta_per_name_daily`, `entry_every_n_days`, `multiplier=100`, `frictions` (`on`/`off`), `missing_policy` (`error`/`drop_renormalize`), `min_names`.

**CLI:**

```
mag7_dispersion_backtest --db DIR [--out DIR] [--names AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA]
                         [--index SPY] [--theta-per-name 10.0] [--delta 0.40]
                         [--tenor-days 90] [--close-dte 10] [--min-names 4]
                         [--frictions] [--threads N]
```

Defaults are the Global Constraints' pinned strategy defaults. `--out` default `<tmp>/atx-mag7-dispersion/`. `--frictions` toggles a simple nonzero `FrictionModel` (copy the existing example's B2 defaults if `spy_strangle_tradeable` has one; otherwise spread_frac-style single knob — keep trivial). Flow: open db → `Clock::from_surface_db` → `make_dispersion_strangle_spec` → `DeclarativeStrategy` → `RunConfig` with a `SnapshotCache` (`std::make_shared`) and `UnpricedLotPolicy::ExcludeAndReport` → time `run_backtest` with `std::chrono::steady_clock` → `tearsheet(r)` → emit the five files → print a short console summary. Exit codes: 2 bad args, 1 runtime error.

- [ ] **Step 1: Write the failing gate test.** `atx-vol/tests/mag7_dispersion_backtest_test.cpp`, suite `Mag7DispersionBacktest`. Fixture: synthetic `SurfaceDb` with 8 symbols (7 fake MAG7 + "SPY"), 12 daily partitions, per-symbol vol bumps/spots, `make_surface` pattern (as Task 1's test); TEST-scale config: `tenor_days=6`, `close_dte_days=2.5`, `theta_per_name_daily=10`, `min_names=4`.
  - `EndToEnd_DbToEmittedFiles`: run the full library pipeline the example composes (db → clock → spec → strategy → `run_backtest` → all emitters into a temp out dir); assert every output file exists, meta lines parse, `series.csv` row count == 12.
  - `FortyDeltaOnDbSurfaces`: resolve the spec against the FIRST db snapshot (`MarketSnapshot::load` of `clock.refs()[0]`); every leg reprices to |Δ|≈0.40 within 1e-3 (mirror T3's assertion, now through db-loaded surfaces).
  - `CohortMechanics`: `n_open_lots` ramps by 16/day (8 symbols × 2 legs). Same residual arithmetic as T2's `CloseAtHorizonOverlappingCohorts`: with tenor 6d and close below 2.5d, a cohort lives ages 0..3, so 4 live cohorts at steady state → plateau 64 lots. Expected series over 12 steps: `{16, 32, 48, 64, 64, 64, 64, 64, 64, 64, 64, 64}`. Assert the exact vector, and `pnl_settlement` all zero.
  - `VegaFlatAtEntry`: at each step's entry, net book vega change from the new cohort ≈ 0 → assert `|gross_vega|` (signed net, see Global Constraints note) stays ≤ 1e-6 × cumulative absolute leg vega proxy; simplest robust check: resolve the spec directly on each snapshot and assert per-entry net vega ≤ 1e-9 × gross (as T3) for all 12 dates.
  - `DeterminismAcrossThreads`: run twice with `RunConfig.price.n_threads` 1 vs 4 → bit-identical result (copy `expect_result_bit_identical` from `spy_strangle_backtest_test.cpp:297`'s pattern).
- [ ] **Step 2: Build; verify failure** (helpers/example glue not yet present — the test drives library code only, so failures should be assertion-level once T1-T5 exist; if all pass immediately, the test must be strengthened until it exercises something new — at minimum the emit-files integration).
- [ ] **Step 3: Implement the example** (thin CLI over the tested pipeline). Line count target ≤ ~300; if it exceeds, move reusable glue INTO the library, not the reverse.
- [ ] **Step 4: Build + run.** `& .\scripts\atx-build.ps1 build atx-vol-tests mag7_dispersion_backtest` then `-Ctest -R "Mag7DispersionBacktest|DispersionStrangle|SurfaceDbBacktest"` — ALL PASS.
- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): mag7_dispersion_backtest example + gate test"
```

---

