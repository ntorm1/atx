# Task 5 Report — apply_symbol_config pipeline binding + end-to-end test

NOTE (controller-authored): the implementer subagent was cancelled by the operator mid-verification
(while waiting on the full `-L atx_vol` gate) and never wrote this report. This report is the
controller's factual reconstruction from the implementer's interim messages plus the controller's
own verification run. Treat unattributed claims below as controller-verified.

## What was implemented (commit 8d99c49)

- `apply_symbol_config(const SymbolFitConfig&, SessionInputs&)` — free function in
  `atx-vol/include/atx/vol/surface_db.hpp` / `atx-vol/src/surface_db.cpp`. Order per brief:
  `apply_fit_preset(in, cfg.preset)` first, then `pin_curve` → `in.curve` + `in.calib`,
  then `al_override` → `in.deam.al_opts`, then the six scalars/flags unconditionally.
  Market-snapshot fields (S, r, expiry rates, cash_divs, now_ts_ns) untouched.
- `symbol_config_from_preset(FitPreset)` — captures `apply_fit_preset`'s effective policy into
  a `SymbolFitConfig` (pin_curve=false identity starting point).
- Tests appended to `atx-vol/tests/surface_db_test.cpp`:
  `SurfaceDbApply.PinnedConfig_OverridesPreset`, `SurfaceDbApply.UnpinnedConfig_PresetCurveStands`,
  `SurfaceDbEndToEnd.ConfigureStoreReloadServe` (two-session create/configure/store → reopen/serve
  → external-writer refresh flow, ConvexDense surface through the partition path).

## Test evidence

- TDD RED: implementer reported the expected failure (missing `apply_symbol_config` /
  `symbol_config_from_preset` symbols) before implementing. RED output lives in the implementer's
  transcript; not re-extracted here.
- GREEN (controller-run, post-cancellation, before commit):
  `ctest --test-dir build -R "SurfaceDb|SurfaceArchive" --output-on-failure`
  → **100% tests passed, 0 tests failed out of 33** (includes all 3 new Task 5 tests, all prior
  SurfaceDb*/SurfaceDbManifest*/SurfaceDbPartition* tests, and the SurfaceArchive regression suite).
  Total time 2.26 s.

## Deviations / process notes

- **Full-module gate (`-L atx_vol`) deliberately skipped per operator instruction** — the operator
  killed the in-flight gate run and will run the full suite themselves later. Brief Step 4's
  whole-module sanity run is therefore DEFERRED TO THE OPERATOR, not evidence-backed here.
- Implementer's baseline full-gate run (pre-task) was in flight when cancelled; no pass-count
  comparison exists.
- Files changed: exactly the three task files (+155 lines, no deletions elsewhere).

## Self-review

Not performed by the implementer (cancelled). Covered instead by the task review gate.
