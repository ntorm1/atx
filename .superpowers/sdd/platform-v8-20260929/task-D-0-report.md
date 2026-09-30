# Task D-0 report: NAV warm start (`--warm-start-sessions K`)

Lane D, worktree `C:/atx-wt/pool-4`, branch `feat/platform-v8-d-20260929`. Brief: `task-W0-4-brief.md` step 2.
Not built and not run (lane rules): every claim below is from reading the code; root builds and runs.

## What was built

Files: `atx-impl/src/strategy_nav_replay.{hpp,cpp}`, `atx-impl/tests/strategy_nav_replay_test.cpp`.

Interfaces as coded:

- `NavReplayConfig::warm_start_sessions` (`usize`, default 0) and `NavExecutionOptions::warm_start_sessions`
  (copied into the base config by `run_nav_replay`). CLI: `nav ... --warm-start-sessions K` (integer, <= 4096).
- Semantics (`run_books`, `start_scoring`):
  - Every book runs sessions `[score_begin - K, end)` with the unchanged MARK -> EXECUTE -> DECIDE sequence, the
    same rules, costs, financing, borrow tiers, liquidity and construction as today. Nothing reads a row before 0.
  - The MARK of session `score_begin` closes the warm-up and is not reported. Right after it each book is resized to
    `initial_nav`: one factor on every dollar state (holdings, cash, working orders, delta anchors, written-off
    exposures); weights, marks and the v2 month-to-date plan are unchanged. Every reported accumulator restarts:
    rows, events, the missing/guard buckets, guard sensitivity, participation histogram, per-name rate statistics
    and the accounting-check maxima. Ruling: resize to initial_nav at the boundary -- the scored book is then the
    declared $1bn book (cost scale, capacity multiples and dollar statistics comparable with flat-start cells) --
    cost if wrong: with sqrt impact the scored path differs from an unresized one by the warm-up return's effect on
    trade size (a few percent of cost), nothing else.
  - Row `score_begin` is the scored base row: pre-trade NAV = initial_nav, no mark fields, its fills (the last
    warm-up decision's orders) and its decision are scored. Return rows are `[score_begin + 1, end)` (flat start:
    `[score_begin + 2, end)`, whose row `score_begin + 1` marks an empty book). `summarize_nav` needs no change: it
    already skips row 0 and bases NAV/drawdown/years on `days.front().pretrade_nav`.
  - `deployment_index` keeps the first fill, which under a warm start lies in the warm-up (before the first row),
    so the summary reports no scored deployment and excludes no scored session from the daily GMV statistics.
  - Cadence phase stays relative to `score_begin` (`cadence_day`, exact for rows before it).
  - Output files contain scored sessions only, same schema as today (same CSV columns and events columns).
- Refusal: `K > score_begin` is `InvalidArgument` "nav replay: --warm-start-sessions K exceeds the role's pre-score
  history (score_begin = S sessions)" -- in `admit_and_load` from the pinned role manifest before any payload is
  read or any output exists, and again in `validate_nav_input` for API callers. `K == score_begin` starts at row 0.
- Recipe (only when K > 0): `warm_start_sessions`, `warm_start_rule` (declaration text). Summary (only when K > 0):
  `warm_start {sessions, first_decision_session_ns, scoring_begins_session_ns, rule}`. Console line when on.
- K = 0 path: identical code path to today (`start = begin`, `first_return = begin + 2`, the boundary branch never
  runs, `cadence_day` returns the same boolean as `(t - begin) % cadence == 0`); no floating-point operation or
  summation order changed; recipe and summary unchanged byte for byte.

## Tests (gtest, `atx-impl-tests`)

- `NavWarmStart.FlagOffIsByteIdentical`: `--warm-start-sessions 0` vs no flag, every file byte for byte under
  baseline, the v5 flags and the v7.1-cell shape (v5 + fields + price-risk-v1 + delta + exit rate + locate-in-aim
  + liquidity cache: 12 files); the pre-change recipe SHAs 73cb45f1 / d53f0c09 still hold.
- `NavWarmStart.GrossAtFirstScoredSessionWithin5PctOfSteadyState`: constant prices/aim, theta .05, L 1.247, K 60:
  first scored gross = L(1 - .95^60) = .954 L, within 5% of the steady state; the flat start's is 0 then .05 L;
  base row at 1e9 with no mark; one more return row; no scored deployment.
- `NavWarmStart.RowsAfterTheBoundaryAreTheEarlierStartResized`: with scale-free costs the warm book equals the
  flat start of `begin - K` resized at the boundary (returns, turnover, leverage to 1e-12; dollars x factor;
  events after the boundary identical, none at it).
- `NavWarmStart.RefusesMoreThanTheRolesHistoryAndRecordsTheWarmStart`: API K = begin + 1 refused, K = begin runs;
  pinned CLI K > score_begin exits 1 with the message and no output; bad values exit 2; recipe/summary keys,
  recipe SHA binding, daily header unchanged, scored rows only.

## How root verifies

Build (tracked research build script, E-1): targets `atx-impl-tests` and `atx-equity-strategy-targets`.

gtest filter: `NavWarmStart.*:StrategyNavReplay.*:NavV5.*:NavV6.*:StrategyLive.*`
(StrategyLive covers the decide path, which shares `admit_and_load` and `validate_nav_input`).

Identity (flag off) on the accepted v7.1 cell: rerun its recorded argv (receipt
`build-equity/mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247-run/receipt.json`) into a new output with the
new exe, once as is and once with `--warm-start-sessions 0` appended:

```
build-equity/bin/atx-equity-strategy-targets.exe nav --combined build-equity/mega-v71w-train-ew-1/train_combined.json --combined-sha256 bf1af1276fd4d2cb4bd835f97b27897596f74b5855dfe7cac05c61dac2486fd5 --role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --role-sha256 3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809 --fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9/manifest.json --fields-sha256 8fd00e9f44b475116f483e133c03fe031618390060b8374180278f1cd7b8769b --output <NEW> --rule aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage 1.247 --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824 --order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache [--warm-start-sessions 0]
```

Expected: the five `daily_*.csv` and five `events_*.csv` byte-identical to the accepted cell's, and here also
`recipe.json` and `summary.json` (D-0 adds no always-on key).

B0c: add `--warm-start-sessions 60` to the nav flags of `scripts/specs/v8/base-*.json` (lo roles have
score_begin 399, so 60 is admitted).

## Deviations

- The brief names only "decide from score_begin - 60, score from score_begin". The resize to initial_nav at the
  boundary and the base-row convention are my rulings (above); both are declared in the recipe text.

## Cross-lane edits

None.

## Open risks

- The daily decide path (`strategy_live.cpp`, `load_nav_deploy`) rebuilds the NAV recipe digest from the deploy
  manifest's base, which has no warm-start field: a book whose NAV run used K > 0 would fail the decide path's
  recipe pin until the deploy manifest learns the key. Not needed in v8 (no deployment), noted for the owner.
- With a v7 extension (`--cost-v2`, `--capacity-curve`, aim-partial-v6, spo), the extension's own side files
  (`v7_transfer_coefficient.csv`, spo diagnostics and the spo summary block) record every decision the replay
  makes, warm-up decisions included; the NAV directory itself and the capacity curve (built from the scored
  results) are scored only. R-5's `--capacity-curve` on B0c is therefore fine; a TC or spo statistic under a warm
  start would include the warm-up. Fixing it needs `strategy_nav_v7.cpp` (not in this lane).
