# GOAL: MAG7 vs SPY dispersion-strangle backtest, end-to-end on surface_db, with an HTML/SVG report

You are a fresh agent in the `atx` repo (`C:\atx`, Windows). Your mission is to make the
following strategy backtestable **from a small example file**, running entirely off the new
`surface_db` on-disk surface database, on **real YTD 2026 market data**, and to produce a
self-contained HTML report. All new functionality lands in the `atx-vol` library — the example
file only composes library pieces.

## The strategy (the acceptance example)

- Universe: MAG7 single names — AAPL, MSFT, GOOGL, AMZN, NVDA, META, TSLA — versus SPY.
- Every trading day, open one new cohort:
  - **Long** one 40-delta strangle (|Δ|=0.40 call + |Δ|=0.40 put) per MAG7 name, ~3 months to
    expiry, sized so each name contributes **equal theta** to the cohort.
  - **Short** one SPY 40-delta ~3-month strangle sized so the cohort is **vega-flat at entry**
    (net cohort vega ≈ 0).
- Close each cohort when it reaches **10 days to expiry**.
- **Projection path only**: strikes and expiries are synthetic — resolved and marked off the
  fitted surfaces every day (constant-maturity/projected-expiry pricing on `PricedSurface`s).
  No listed-contract snapping, no OPRA quote marks for P&L. Real data is used to FIT the
  surfaces, not to price the lots.
- Backtest window: 2026-01-02 through the most recent available date (YTD).

Defaults you may pin without asking (document them in the report header): per-name theta budget
per cohort (e.g. $10/day per name), option multiplier 100, frictions off by default (flag to
enable), NAV base for return/Sharpe scaling, expiry = entry date + 90 calendar days snapped to
a calendar expiry so DTE decays naturally.

## Two build goals (both required)

1. **surface_db ↔ backtest engine integration.** Today `MarketSnapshot::load` opens a bare
   `.atxvsa` file and `Clock::from_manifest` only consumes a `CorpusManifest`. There is NO glue
   from `SurfaceDb` to the backtest clock. Build it: a `SurfaceDb`-backed clock/snapshot source
   (date-keyed partitions → ordered `SnapshotRef`s, snapshots loaded via the db, per-symbol
   `SymbolFitConfig` honored via `apply_symbol_config` where fitting happens). The backtest
   must run *from a `SurfaceDb` root*, not from loose archive files.
2. **Engine completeness.** Extend the existing strategy/backtest layer only where this example
   needs it, so the example stays small. Expected gaps (verify, don't assume): equal-theta
   sizing across a multi-name basket combined with a vega-flat index hedge leg in one cohort
   (the DSL has `SizeSpec::TargetTheta`, `CrossLegConstraint::FlatVega` /
   `VegaNeutralBasket` — check they compose for long-names/short-index in one entry);
   close-at-DTE lifecycle for overlapping daily cohorts (`LifecycleSpec`, `entry_every_n`);
   ~60 overlapping cohorts × 16 legs of open lots.

## What already exists (start here, do not reinvent)

- `atx-vol/include/atx/vol/surface_db.hpp` — `SurfaceDb` (create/open, `write_partition`,
  `open_partition`, `load_surface`, `partitions()`, `symbol_config`, `upsert_symbol`,
  `refresh()`), `SymbolFitConfig`, `apply_symbol_config`, `symbol_config_from_preset`.
  Partitions are `.atxvsa` archives under `<root>/partitions/<KEY>.atxvsa` — same format
  `MarketSnapshot` already reads.
- `atx-vol/include/atx/vol/backtest.hpp` — `Clock`, `SnapshotRef`, `MarketSnapshot`,
  `SnapshotCache`, `RunConfig`, `UnpricedLotPolicy`, `BacktestResult` (rich P&L attribution
  columns from `pnl_explain`: delta/gamma/vega/vanna/volga/theta/rho/charm/unexplained, NAV,
  cash, gross greeks, turnover, unpriced-lot counts), `run_backtest(Clock, IStrategy&,
  RunConfig)`.
- `atx-vol/include/atx/vol/strategy.hpp` — `IStrategy`, `DeclarativeStrategy` + spec DSL
  (`StrikeSelector::Delta`, `StructureSpec::Strangle`, `SizeSpec::{TargetVega,TargetTheta,...}`,
  `CrossLegConstraint::{FlatVega,VegaNeutralBasket}`, `LifecycleSpec`, `HedgeSpec`),
  `resolve_strike_by_delta(surface, T, side, target_abs_delta)`.
- `atx-vol/include/atx/vol/dispersion.hpp`, `dispersion_backtest.hpp`, `strategy.hpp`'s
  `DispersionStrategy` — vega-neutral dispersion sizing (`build_dispersion_book`),
  `DispersionBacktestConfig{target_dte_days, roll_dte_days, ...}`, `MissingNamePolicy`.
  This is straddle/vega-weighted dispersion; your strategy is strangle/equal-theta — reuse the
  machinery, don't fork it.
- `atx-vol/include/atx/vol/corpus.hpp` — `build_corpus(CorpusBoards, out_dir)` /
  `build_qualified_corpus`: fits boards, writes one archive per date + manifest. Boards come
  from `opra_panel.hpp::load_opra_cbbo_parquet` / `opra_batch.hpp::load_opra_daterange`
  (parquet on disk).
- Worked precedents: `atx-vol/examples/spy_strangle_backtest.cpp` (40Δ strangle restrike,
  CSV + text tearsheet; gate test `tests/spy_strangle_backtest_test.cpp` asserts resolved
  strikes reprice to |Δ|≈0.40), `examples/spy_dispersion_backtest.cpp` +
  `dispersion_workflow.hpp` (multi-name real-data run spec), `examples/spy_ytd_corpus.cpp`,
  `examples/databento_spy_dispersion_definitions.cpp` (the ONE existing databento call site —
  copies its `MetadataGetCost` cost-gate + `--dry-run` pattern), `tools/tearsheet.py`
  (offline matplotlib PNG — your HTML/SVG writer replaces this dependency for this example).
- Examples convention: each example is its own executable under `if(ATX_BUILD_EXAMPLES)` in
  `atx-vol/CMakeLists.txt`, linked `PRIVATE atx::vol atx::core atx_warnings`, with a comment
  naming its gate test.

## What does NOT exist (the new work)

- SurfaceDb→Clock/MarketSnapshot glue (goal 1).
- Any HTML or SVG report generation anywhere in atx-vol. Division of labor (binding):
  **atx-vol C++ emits data only** — extend the existing CSV/TSV output path
  (`tearsheet.hpp::write_backtest_tsv`, the `# key=value` metadata-header convention from
  `spy_strangle_backtest.cpp`) so the example writes machine-readable run outputs (backtest
  series, strategy metrics, engine timing/cache stats, surface/db stats). **A Python helper
  script in `atx-vol/tools/` renders the report** (follow the `tools/tearsheet.py` precedent):
  reads those outputs, generates the SVG chart(s), and assembles one self-contained `.html`
  (inline SVG + inline CSS, no external assets, no JS required).
- Real MAG7 options data. Only two single-date fixtures exist (`data/spy_opra_cbbo1m_...`,
  `xom_...`). No bulk OPRA pull tool exists (a CMake comment mentions `databento_bulk_opra`
  but it was never built). You must build the pull path.

## Data authorization and guardrails

You ARE approved to spend on live Databento OPRA pulls for real data (8 symbols × YTD daily).
Non-negotiable guardrails:
- Always call the cost/metadata estimate first (follow the existing example's
  `MetadataGetCost` + `--dry-run` gate) and log the estimate before any paid request.
- If the total estimated cost for the full YTD pull exceeds ~$150, STOP and ask the operator
  with the estimate and cheaper alternatives (fewer snapshots/day, coarser CBBO, shorter
  window) before pulling.
- One snapshot per symbol per trading day is sufficient (e.g. 1-minute CBBO window near
  15:55 ET, matching the existing fixtures' shape). Pull once, cache to a parquet hive under
  `data/` (per-symbol/per-date layout compatible with `load_opra_daterange`'s path template),
  and never re-pull data already on disk. Fit once into a `SurfaceDb`; the backtest then runs
  offline from the db.
- API keys: use the existing databento example's key discovery (env var). Never print or
  commit the key.

## Pipeline shape (end state)

1. Pull tool/example: Databento OPRA daily chains for the 8 symbols, YTD → parquet hive
   (cached, resumable, cost-gated).
2. Fit + store: boards → fitted surfaces (reuse corpus/qualification pipeline) →
   `SurfaceDb::write_partition(date_key, items)` under one db root (e.g.
   `data/surfdb/mag7_ytd/`), with per-symbol `SymbolFitConfig` in the manifest
   (`symbol_config_from_preset` + pins where a name needs it).
3. Backtest: `SurfaceDb` root → db-backed clock → `run_backtest` with the dispersion-strangle
   strategy above.
4. Report: example emits data files (`BacktestResult` series + run timing +
   `SnapshotCache::stats` + surface/fit stats collected during step 2, CSV/TSV with metadata
   headers) → `tools/mag7_dispersion_report.py` renders `mag7_dispersion_report.html`.

## Acceptance criteria

- `atx-vol/examples/mag7_dispersion_backtest.cpp` exists, is SMALL (target ≤ ~300 lines — it
  composes library calls: open db → configure strategy → run → emit data files), builds under
  `ATX_BUILD_EXAMPLES`, and has a named gate test.
- Running it against the populated `SurfaceDb`, then running the Python report script on its
  outputs, produces `mag7_dispersion_report.html`: self-contained (inline SVG + inline CSS,
  no external assets, no JS needed), containing:
  - SVG chart(s): YTD cumulative P&L / NAV of the strategy (equity curve; drawdown shading or
    a second panel welcome).
  - Strategy metrics table: total P&L, annualized Sharpe, max drawdown, hit rate, avg daily
    P&L, turnover, average/peak open lots, P&L attribution totals (delta/gamma/vega/theta/
    vanna/volga/rho/charm/unexplained), average net vega and theta after entry (evidence the
    vega-flat and equal-theta constructions actually held).
  - Backtest engine metrics table: wall-clock, steps/sec, snapshot-cache hit stats, unpriced
    lot/greeks counts, per-step timing summary.
  - Surface/db statistics table: dates covered, per-symbol fit success rate, fit quality
    summary (in-band %, arb-check results where available), db partition count/size,
    manifest generation.
- Strike correctness gated by test: resolved strangle strikes reprice to |Δ|≈0.40 on the
  fitted surface (mirror `spy_strangle_backtest_test`'s assertion pattern).
- Equal-theta and vega-flat constructions gated by test on a synthetic multi-name fixture
  (per-name |theta·qty| equal within tolerance; cohort net vega ≈ 0 at entry).
- surface_db→backtest integration gated by test: a db populated with a small synthetic
  multi-date corpus drives `run_backtest` end-to-end (no loose archive paths).
- The C++ data emitters are library code with unit tests (columns/metadata present, parseable).
  The Python report script is validated by running it end-to-end on the example's real output
  (renders without error; output HTML contains the required sections) — no pixel-perfection.
- Determinism: same db + same config ⇒ bit-identical `BacktestResult` across runs and worker
  counts (house invariant — respect `ATX_VOL_FIT_WORKERS`).
- All new tests pass; `/WX` clean; no regression in `SurfaceDb|SurfaceArchive|Backtest|
  Dispersion|Strategy` targeted suites. (Full `-L atx_vol` gate before merge unless the
  operator defers it.)

## Non-goals

- No listed-contract execution realism (that's the separate `listed_dispersion` path).
- No stochastic-vol/exotics/MC/GPU. No UI beyond the single static HTML file.
- No HTML/SVG generation in C++ — atx-vol emits data; Python renders. Keep the script's
  dependencies to what `tools/tearsheet.py` already uses (pandas/matplotlib or stdlib);
  `tools/tearsheet.py` itself stays as-is for PNG workflows.
- Don't rewrite `DispersionStrategy`/`build_dispersion_book` — extend or parameterize.

## Process requirements

- Work in a fresh git worktree on a feature branch; use subagent-driven development with the
  repo's superpowers skills (brainstorm → written plan → per-task implement/review → final
  whole-branch review). Keep the SDD ledger in `.superpowers/sdd/progress.md` (git-ignored;
  `git add -f` for ledger commits, `chore(sdd): ...`).
- Build environment (Windows, no pwsh — Windows PowerShell 5.1): fresh-worktree configure
  needs `git submodule update --init --recursive` (databento-cpp) and pinning
  `-DCMAKE_MT=C:/Program Files (x86)/Windows Kits/10/bin/10.0.22000.0/x64/mt.exe` on the
  `cmake --preset ninja` call. Then `& .\scripts\atx-build.ps1 build <target>` /
  `& .\scripts\atx-build.ps1 -Ctest -R <regex>`. Warnings are errors.
- Commit style: `feat(atx-vol): ...` / `test(atx-vol): ...` / `fix(atx-vol): ...`; commit per
  green task.
- Escalate to the operator instead of guessing on: paid-pull cost above the cap, MAG7 names
  whose chains won't fit cleanly (document per-name fit failures rather than silently
  dropping — `MissingNamePolicy` semantics apply), and any strategy-definition ambiguity that
  materially changes P&L (e.g. theta budget scale, expiry snapping convention).

## Final deliverables checklist

1. surface_db-backed clock/snapshot integration (library + tests).
2. Strategy-layer extensions for equal-theta long basket + vega-flat short index strangle
   cohorts with close-at-10-DTE (library + tests).
3. Cost-gated OPRA pull + fit + `SurfaceDb` population tooling; populated db for 8 symbols
   YTD 2026 (cached raw parquet under `data/`).
4. C++ run-output emitters (library + tests) and `atx-vol/tools/mag7_dispersion_report.py`
   HTML/SVG renderer (validated end-to-end on real output).
5. `examples/mag7_dispersion_backtest.cpp` (small) + gate test + CMake registration.
6. `mag7_dispersion_report.html` generated from the real YTD run, checked-in or archived per
   repo convention for artifacts (follow `tools/spy_strangle_real_tearsheet.png` precedent).
7. Updated ledger/plan docs; final whole-branch review clean.
