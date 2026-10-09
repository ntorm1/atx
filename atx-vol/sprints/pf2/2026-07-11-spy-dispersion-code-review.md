# SPY listed-options dispersion — pre-merge code review

**Branch:** `feat/atx-vol-spy-listed-dispersion`
**Reviewed at:** `1d68bec` (10 commits above `4f92180`), 2026-07-11
**Scope:** full branch diff (85 files, ~16k insertions): listed-contract selection,
projection, vega-flat schedule, reconciliation, dispersion strategy, and the
backtest hot-path optimization.

## Verdict

Build is clean (clang-cl + Ninja, `atx-vol-tests` and `atxvol_spy_dispersion_backtest`
link). Core numerics were verified correct: listed ATM selection (`min|K-F|`, lower-strike
tie-break), monthly-only expiry selection (`DTE∈[21,60]`, order by `|DTE-30|` then earlier),
vega-flat sizing signs and the single `*0.01` per-vol-point conversion, nearest-rank
VaR/ES, and the model-vs-quote-mid reconciliation (quote-mid stays an independent series,
never fed back into pricing). Merged after fixing the one blocking regression below.

## Blocking (fixed in this review)

**Diagnostic-signal opt-in regression — FIXED.** The optimization commit made
`DispersionStrategy` diagnostics opt-in (`DispersionConfig::record_diagnostics`, default
`false`) so `dispersion_signal` is no longer recomputed every backtest row. `strategy_test`
and `tearsheet_test` were updated to opt in, but three pre-existing `multiname_pipeline_test`
cases and one `corpus_test` scoreboard case still asserted the `implied_corr` /
`n_names_dropped` series are recorded and failed (`series not recorded`). Fix: set
`record_diagnostics = true` in those four configs (matching the intended opt-in design).
Verified: the four now pass; no library code changed.

- `atx-vol/tests/multiname_pipeline_test.cpp` (CorpusWithMissingNameOnOneDate, AllNamesMissing, HeldNameGoesMissingMidRun)
- `atx-vol/tests/corpus_test.cpp` (SyntheticThirteenNameThreeDateBreadthScoreboard)

## Follow-ups (non-blocking; not fixed here)

None affect the default/shipped CLI path or the surface-only acceptance run. Deferred to
avoid risky last-minute edits to just-optimized concurrent code.

1. **`SnapshotCache` has no eviction** (`src/snapshot_cache.cpp`). Retains one
   `MarketSnapshot` per distinct archive path for the cache's lifetime, though the backtest
   only needs a 3-wide window (prev/current/next). Bounded and fine for the 61-date run;
   a multi-year run would retain everything. Memory-scaling, not correctness or determinism.
   Fix later: bound to a small LRU window (respecting the shared-across-phases design).

2. **Reconcile requires first snapshot == first roll date**
   (`src/listed_dispersion_reconciliation.cpp:240`; CLI `examples/spy_dispersion_backtest.cpp`).
   `build-schedule` may defer the first roll past `clock.refs()[0]` (selection/coverage gate),
   but `run-backtest` feeds reconcile every clock date and it rejects a leading flat date.
   Fix later: either assert the first roll lands on the first clock date, or teach reconcile
   to skip leading flat dates while keeping row-count alignment.

3. **`has_raw_mid` couples quote-mid coverage to model status**
   (`src/listed_dispersion_reconciliation.cpp:199`). In non-default `strict_model=false`, a
   held leg with a valid quote but missing surface (`NoSurface`) is dropped from the quote-mid
   series and serialized `NA`. The shipped CLI uses the strict default and is unaffected. Fix
   later: track raw-quote presence independently of model status.

### Low severity (hardening)

- `src/listed_opra.cpp:317` — strict PIT join does not reject `ts_ns < 0` (only `==0` is
  remapped); caught downstream, but a direct `listed_quotes_from_opra` consumer keeps it.
- `src/occ_ess.cpp:116` — a well-formed ESS report with zero non-standard rows is rejected
  as `ParseError`; confirm intentional or accept empty.
- `src/occ_ess.cpp:104` — whitespace tokenization of a fixed-width report is fragile to a
  blank column; parse by column offsets if the feed can blank fields.
- `src/contract_projection.cpp:74` — the `long double` tenor-overflow guard is a plain
  `double` on MSVC (unreachable ~292-year tenor); do the bound check in the integer domain.
- `examples/spy_dispersion_backtest.cpp:405` — `traded_weight / requested_weight` lacks a
  `requested_weight == 0` guard (NaN coverage passes the min-coverage check).
- `tools/download_occ_ess.py:16` / `tools/reference_spy_dispersion.py:238` — accept
  impossible dates / silently de-dup duplicate reconciliation dates; tighten validation.

## Not a bug (verified intentional)

- `historical_projection.cpp:91` — the strict `leg.definition.valuation_ts_ns == scenario.ts_ns`
  check is a deliberate consistency guard (the surface's `now_ts_ns` must match the scenario
  timestamp); the workflow derives `scenario.ts_ns` from the same surfaces, so it holds.
  Loosening it would weaken the guard.
- Backtest close-path mark switched from `fair_value()` to `greeks_analytic().price`: the
  analytic AL/cold-FD path documents `greeks().price == fair_value()`, so this is bit-identical
  by design (and saves a redundant solve). Confirmed by the unchanged bit-identical anchor tests.
