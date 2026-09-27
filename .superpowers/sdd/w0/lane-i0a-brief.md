# Lane W0-I0A: Pipeline look-ahead fixes: discover, combine, optimize, metabook

Host tag: **RAM:light** · Batch: **W0b** · Pool: **`C:\atx-wt\pool-10`** ·
Branch: **`feat/w0-i0a`** (run id `aes-w0-i0a`) · Base: the W0 base commit (head of
`feat/w0-integration` at lease time; see `progress.md`).

Read first: `.superpowers/sdd/w0/RULES.md` (binding), then this brief, then the plan and findings
docs under `docs/plans/` (all inside your pool).

## Goal (plan §7, verbatim)

#### W0-I0a · Pipeline look-ahead fixes: discover, combine, optimize, metabook
- **Closes:** I-01, I-02, I-03, I-04, I-06, I-07, I-08, R-12.
- **Owns:** `atx-impl/src/{stage_discover.cpp, stage_run.cpp, stage_combine.{hpp,cpp}, stage_optimize.cpp, stage_metabook.{hpp,cpp}, dead_alpha_wire.hpp, diag_risk.hpp}`.
- **Build:**
  - Nested splits: discover train < combine fit < final test. Lockbox ranges are stored in the library, and overlap is refused.
  - Conviction scoring window = fit window.
  - Capacity uses a trailing PIT window with a Δw-based cost.
  - The diagonal risk model is fitted per step, PIT.
  - The participation cap uses trailing PIT ADV per rebalance.
  - "Dead" means Dead/Decaying as of each step.
  - Metabook sleeves take their signals from the combo weights.
  - Walk-forward goes through the same fit dispatch as the shipped weights, with an h+delay embargo.
- **Suites:** `ImplNestedSplits_*`, `ImplCombineNoHoldoutRead_*`, `ImplOptimizePit_*`, `ImplDeadAlpha_*`, `ImplMetabookUsesCombo_*`.
- **Accept:**
  - Mutating holdout PnL leaves the shipped weights byte-identical.
  - Mutating future volumes or prices leaves past books byte-identical.
  - Walk-forward with `--method stack` runs.
- **Deps:** none. **Load:** light.
- Combine memory (I-05) is W3-I6.

## Cited findings rows (verbatim; every ID must end CLOSED or explicitly DEFERRED in your report)

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| I-01 | B | `stage_discover.cpp:548-551,610`; `stage_run.cpp:105-106,121` | Discover admits alphas on the last 25%, and combine then reports that same 25% as "OOS". The library reuses the lockbox. | W0-I0a |
| I-02 | B | `stage_combine.cpp:1030` | `apply_conviction` scores DSR and stability over the full stream, holdout included. | W0-I0a |
| I-03 | B | `stage_combine.cpp:327-337,345,289-317,369` | Capacity uses full-period mean PnL, the last book, and last-date ADV. Cost is charged on holdings, not trades, which is biased against low-turnover alphas. | W0-I0a |
| I-04 | B | `stage_optimize.cpp:370-374,217`; `stage_metabook.cpp:537`; `stage_report.cpp:499` | The diagonal risk model uses full-panel variance for every rebalance. | W0-I0a |
| I-06 | M | `dead_alpha_wire.hpp:78-91`; `stage_optimize.cpp:364-367` | The "dead" set is actually the admitted/live alphas, taken as of the last period (inverted, and look-ahead). | W0-I0a |
| I-07 | M | `stage_metabook.cpp:482-483,521-523,352-366` | Metabook sleeves ignore the fitted combiner (they equal-weight). | W0-I0a |
| I-08 | M | `stage_combine.cpp:1416-1422` | Walk-forward fails for stack/RegimeStack, ignores the combiner config, and has no embargo. | W0-I0a |
| R-12 | H | `stage_optimize.cpp:436-451` | The participation cap uses last-date ADV/price for all history (look-ahead). Names delisted before the end get cap 0. | W0-I0a |

## Scope

- Files in scope (the ONLY files you may modify; new test files per RULES §2 are always allowed):
  - `atx-impl/src/stage_discover.cpp`
  - `atx-impl/src/stage_run.cpp`
  - `atx-impl/src/stage_combine.hpp`, `atx-impl/src/stage_combine.cpp`
  - `atx-impl/src/stage_optimize.cpp`
  - `atx-impl/src/stage_metabook.hpp`, `atx-impl/src/stage_metabook.cpp`
  - `atx-impl/src/dead_alpha_wire.hpp`
  - `atx-impl/src/diag_risk.hpp`
  - `atx-impl/src/stage_report.cpp` — ONLY the diagonal-risk call site near line 499 (I-04); B0 owns the rest
- Files forbidden: everything else — in particular files owned by other W0 lanes (see
  `progress.md` ownership table). Needs elsewhere → report "Integration notes".

## Gate closure

- Test groups for `build-equity`: `(n/a — atx-impl)` (reconfigure only if the tree differs).
- Owning targets: `atx-impl-tests (+ atx-shm-worker)`.
- Suites: `ImplNestedSplits_*`, `ImplCombineNoHoldoutRead_*`, `ImplOptimizePit_*`, `ImplDeadAlpha_*`, `ImplMetabookUsesCombo_*`; must stay green: whole atx-impl-tests.
- Anchored runs: `-Ctest -Preset equity-dev -R '^<Suite>'` per suite; whole owning executable once
  before review (`--gtest_brief=1`).

## Done criteria

Every plan **Accept** item above MET with a named test and pasted evidence; every cited ID CLOSED
or DEFERRED with reason; owning targets green; tree clean; report committed at
`.superpowers/sdd/w0/lane-i0a-report.md`.

## Lane notes (orchestrator)

- Do NOT edit `atx-impl/src/config.*` or `dispatch.*` (I0b owns them in W0). New knobs live in stage-private structs with safe defaults; W1-I1 makes stage-private configs reachable from the config file. Record any needed flag in 'Integration notes'.
- Combine memory (I-05) is W3-I6 — out of scope.

## Out of scope

Anything the plan assigns to W1+ lanes; any real-data run; any CMake/preset edit (except O1);
refactors not required by a cited defect.
