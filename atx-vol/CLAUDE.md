# atx-vol — module facts for agents

Vol surface / vol-derivatives library. Source groups: `core pricing fitting marketdata storage analytics backtest` (+ `simd`, `server`). Targets: `atx-vol` (`atx::vol`), `atx-vol-tests` (one monolithic gtest exe → `build/bin/atx-vol-tests.exe`), `atx-vol-tools`, `atx-vol-research`. Public API: `include/atx/vol/api/**` with umbrella `include/atx/vol/api/vol.hpp` (Tier-A manifest, contract-tested — public-surface changes must keep the umbrella suite green).

## Test lanes

- `ctest -L atx_vol_fast` — the inner loop (~2.9k gtest cases; count is measured, not invariant). Preset shortcut: `ctest --preset ninja-fast`.
- `ctest -L atx_vol_slow` — slow suites split out by `ATX_VOL_SLOW_FILTER` (tests/CMakeLists.txt:345).
- `-L atx_vol` regex-matches BOTH labels plus script/aux lanes (python, reference reports, ForceScalar).
- Python wheel: `python/` is a standalone scikit-build-core + pybind11 project (`atxvol`), 26 pytest files, driven into ctest as `atx-vol-python` (SKIP_RETURN_CODE 77).
- ~63 tests SKIP on a bare host (AVX2/counters/market-data/env classes — enumerated in README `## Build & test`).

## Release/pre-merge gates outside the oracle loop

1. Full fast suite green: `powershell scripts\atx-build.ps1 -Ctest -Preset vol-rel -L atx_vol_fast` (from repo root, after building every exe the label covers in `build-vol-rel/`).
2. Include hygiene: `-Preset vol-hygiene` (PCH OFF; the default build masks include rot) — scope it to the changed module's targets.

The commands above are never run by `vol-oracle-iter` or its `vol-sprint` child.
The oracle loop uses only its exact changed-file closure: affected anchored unit
tests, hypothesis-specific OracleBench tests, aggregate smoke/tune scorecards,
quiet pinned speed, and owning PCH-off targets for changed headers. Labels, broad
ctest/builds, full-repository hygiene, and release regression suites are separate
release qualification work.
3. Module gates: `powershell atx-vol\ci\run_all_gates.ps1` — determinism (1-thread vs N-thread bit-identical NAV), golden replay (82-session SPY corpus, fails closed if licensed corpus absent), lakehouse-off link, pool soak. All on `dev` preset — correctness only.
4. Perf claims ONLY from `vol-rel-avx2` (`build-vol-rel-avx2/`, ThinLTO) against `bench/baselines/` pins.

**Trap:** `dev-shared` preset is for iteration relinks ONLY, NEVER the test gate — per-DLL counter globals split and SolveLedger/BacktestExec observer suites fail deterministically.

## Build lanes

Measured basis: `sprints/2026-10-16-build-infra-sprint.md`. The rules, verbatim:

1. **Preset `vol-rel` for every lane `configure` / `check` / `build` / `-Ctest`** (`build-vol-rel/`: Release + `/arch:AVX2` + PCH, no ThinLTO). `vol-rel-avx2` (ThinLTO) is for `-Bench` perf pins only; `vol-dev` (Debug) only when you need assertions; `vol-hygiene` for PCH-off checks of changed headers. ThinLTO on/off is bit-identical in results (`rel` preset description, commit 63f6f29), so a `vol-rel` pass stands for `vol-rel-avx2`; cite timings only from `vol-rel-avx2`.
2. **Every vol preset is vol-only** (`ATX_VOL_ONLY=ON`): atx-core + atx-tsdb + atx-vol. atx-engine and atx-impl are not configured, so they cannot be compiled by accident. The two engine consumers — `atx-vol-vrp-train` and `atx-vol-vrp-model-tests` (VrpModel/VrpTrain suites) — exist only with `configure -Preset vol-rel -DATX_VOL_ONLY=OFF`.
3. **Never build `atx-vol-tests` in a lane.** Build the focused target that owns your suite (table below) and run it anchored: `-Ctest -Preset vol-rel -R "^<anchor>"`. The monolith (≈215 PCH test TUs, PCH objects never shared between worktrees by ccache, one huge link) is for integration/release qualification only.
4. **Lease with the default `-ConfigurePreset auto`** — a vol branch gets `vol-rel` configured on a cold slot and is skipped on a warm one.

Focused targets (the stem lists in `tests/slices.cmake` are authoritative; configure prints `atx-vol test slices: <target>(<files>)`):

| Target | Owns `tests/<stem>_test.cpp` | ctest anchor |
|---|---|---|
| `atx-vol-quote-slice-tests` | quote_slice, american_price_slice, quote_fit_policy | `^atx-vol-quote-slice$` |
| `atx-vol-american-markov-tests` | american_markov{,_batch,_resolvent,_pwl,_grid}, pwl_strike | `^atx-vol-american-markov$` |
| `atx-vol-american-markov-{fit,seed,market,policy}-tests` | american_markov_{fit,seed,market,policy} | `^atx-vol-american-markov-<x>$` |
| `atx-vol-umbrella-tests` | vol_umbrella | `^atx-vol-umbrella$` |
| `atx-vol-oracle-convention-tests`, `atx-vol-discrete-div-tests` | oracle_conventions; discrete_div_american, oracle_dividends | gtest suite name |
| `atx-vol-alpha-tests`, `varswap-compare-columns-tests`, `spx-wilmott-repro-tests` | alpha_*; varswap_compare_columns; spx_wilmott_repro | `^AlphaRegistry$`, `^VarswapCompareColumns$`, `^SpxWilmottReproUnit$` |
| `atx-vol-core-slice` | version, log_sink, counters, phase_profile, solve_ledger, vol_time, parallel_for, process_scratch, test_paths | `^core/<Suite>\.` |
| `atx-vol-pricing-slice` | black76, greeks, implied_vol, iv_*, scalar_erfc, american (non-Markov), adjoint/adjusted greeks, dividend, rates_curve, derivatives, deriv_{distribution,fixtures,greeks}, cboe_strip, swap_leg, theo, pricing_executor, batch, *_ok_stamp | `^pricing/<Suite>\.` |
| `atx-vol-simd-slice` | simd_* | `^simd/` |
| `atx-vol-fitting-slice` | surface/vol_surface, calib*, c8*, essvi*, svi, cstar*, s3, dense_slice, curve_*, session, pricer_fitter, deamer, parity, arb, correction, … | `^fitting/` |
| `atx-vol-marketdata-slice` | universe, data, opra_*, corpus, listed_opra, listed_definitions_cache, occ_ess, catalog, spy_real, spy_fit_corpus, breadth, spy_bidask_regression | `^marketdata/` |
| `atx-vol-storage-slice` | run_archive, run_diagnostics, run_report, surface_db*, backtest_db*, research_db, writer_lock, surface_archive*, track_*, sweep_driver, chain_export | `^storage/` |
| `atx-vol-analytics-slice` | projections, var*, event_vol, scenario_grid, pnl_attribution, deriv_pnl, analytics_*, earnings_*, amzn_earnings, realized_vol, breakeven | `^analytics/` |
| `atx-vol-backtest-slice` | backtest*, strategy*, portfolio/priced_surface, margin, deriv_book, panels, tearsheet, research_validation, bev_label_factory_gate, … | `^backtest/` |
| `atx-vol-dispersion-slice` | dispersion*, listed_dispersion*, mag7/spy dispersion, multiname_pipeline | `^dispersion/` |
| `atx-vol-oracle-slice` | oracle_bench | `^oracle/` |

Which slice owns suite `Foo`? `-Ctest -Preset vol-rel -N -L atx_vol_slice -R "/Foo\."` (needs the slice built once; PRE_TEST discovery) or find `TEST*(Foo,` in `tests/` and look its file stem up in `tests/slices.cmake`. A new `*_test.cpp` added to `atx-vol-tests` lands in a slice automatically (stem table, then prefix rule, then `atx-vol-misc-slice`). Slice cases carry only the `atx_vol_slice` label, so `-L atx_vol_fast`/`atx_vol_slow` still run each case once, through `atx-vol-tests`.

## Memory protocol

- `docs/LEDGER.md` — append-only fact ledger. Grep it before re-deriving build traps, measured numbers, or past decisions. Append one line on gate-pass, trap discovery, or decision. Never rewrite history lines; corrections get a new line referencing the old.
- Sprint narratives: `sprints/YYYY-MM-DD-<topic>-sprint.md` (38 exist — search before starting overlapping work).
- Oracle RSI loop (SpiderRock parity): dashboard `docs/oracle/NORTHSTAR.md`,
  bootstrap charter `bench/oracle/CHARTER.md`, scorecards/cohorts under
  `bench/oracle/`. `vol-oracle-iter` advances exactly one ordered capability state
  (`missing_data → missing_mode_a → missing_conventions → missing_mode_b → ready`).
  Bootstrap runs one fixed lane and no holdout; only ready runs Measure → Analyst →
  vol-sprint → Ratchet.
  The fixed capability probe internally validates and hashes the committed holdout
  manifest but exposes no membership. The tool-less Analyst gets a workflow-derived
  aggregate payload, and Ratchet alone benchmarks holdout or opens licensed rows.

## Harness isolation

- Every lane uses `lease-worktree.ps1` with a frozen base SHA, workflow `RunId`,
  run-unique branch, and explicit durable heartbeat whose independent keeper renews
  continuously and is proven by typed acquisition/release receipts.
- Every Fix is freshly reviewed at its new SHA; all mandatory lanes must finish
  APPROVE before integration.
- Release lane leases before acquiring a new isolated integration lease. Merge,
  gate, and ledger only in `C:\atx-wt\pool-N`, never `C:\atx`.
- Deep docs index: `docs/` (surface-db-build, backtest-lakehouse, adjoint_greeks_design, simd_fastpath, api-placement, …). Bench anchors: `bench/ANCHORS.md`.

## Editing rules

- House style first: `../.agents/cpp/agent.md` (mandatory).
- New test → correct suite file under `tests/`; slow suites belong in the `ATX_VOL_SLOW_FILTER` glob, not the fast lane.
- CHANGELOG.md entries follow existing prose-heading structure (`### NEW — …` / `### FIXED — …`); cite code, don't restate it.
