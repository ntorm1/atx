# Task T4 — construction options in target + NAV replay: price-risk neutralization and no-trade band

Owner: the T2 implementer (pool-5), continuing on its branch after T2 is integrated (root gives base).
Files: `atx-impl/src/strategy_target_replay.hpp/.cpp`, `strategy_target_replay_detail.hpp`,
`strategy_nav_replay.hpp/.cpp`, `atx-impl/tools/equity_strategy_targets.cpp`, tests
`strategy_target_replay_test.cpp` / `strategy_nav_replay_test.cpp`. Uses (does not modify) the T3 module
`atx-impl/src/strategy_price_exposures.hpp/.cpp` (API: `PriceExposureConfig`, `PriceExposureInput`,
`compute_price_exposures`, `neutralize_target`, `neutralize_price_risk`, scratch/stats types — read it
and its review `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T3-review.md`).

## Why (root TRAIN-only studies; validation untouched)
The blend carries negative market beta and style tilts; neutralizing the desired target against trailing
beta/vol/log-ADV lifts gross Sharpe materially. Under a <= 30%/month turnover budget, partial adjustment
(fraction .25 every 5 sessions) throws away alpha; a no-trade band on weight changes delivered similar
gross Sharpe at half the turnover.

## Requirements
1. `TargetReplayConfig` gains two explicit options (defaults = today's behaviour, bit-identical):
   - `neutralize`: `none` | `price-risk-v1`. At each rebalance decision d, after `desired_target`, call
     the T3 neutralization with exposures computed ONCE per decision (T3 review M7) from role prices;
     requires `--role` (error otherwise). Guard (T3 review M1): if neutralization returns an error, or
     the amplification ratio input_gross/residual_gross > 5, or the excluded-row gross share > 0.5, then
     SKIP rebalancing on that decision (keep current weights; forced exits still apply) and count it in
     `neutralize_skipped_decisions` with a reason histogram. Record `neutralize_used_names` min/median.
   - `band_multiple` (f64 >= 0; 0 = off): on a rebalance decision, for member i with
     |desired_i - current_i| <= band_multiple / N_d (N_d = number of members with a desired weight at d),
     keep current_i unchanged; other members move by the rule's fraction. For monthly-budget-v2, banded
     names are excluded from `distance`. Report `banded_names` per day.
2. CLI: `--neutralize none|price-risk-v1`, `--band-multiple X` for both `targets` and `nav` modes; both
   recorded in recipe.json and summary (canonical recipe hash changes only when non-default).
3. Rule ids in outputs: compose as `<rule>+neutral-price-risk-v1` / `+band-<X>` suffixes so runs are
   self-describing.
4. The NAV replay applies the same construction via its single extension point.

## Constraints
- House style `.agents/cpp/agent.md`. Do NOT compile or run; root builds. Not TDD: implement, then
  fixtures: (a) defaults bit-identical (existing fixtures unchanged); (b) neutralized target has ~0
  exposure and the recorded amplification; (c) skip path on forced error keeps weights and counts;
  (d) band keeps small moves, moves large ones, and v2 distance excludes banded names;
  (e) nav + neutralize + band end-to-end on a tiny role.
- No subagents. Commit (messages end with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`). No push.

## Report
`C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T4-report.md`. Return only: status, commit
SHAs, one-line summary, concerns.
