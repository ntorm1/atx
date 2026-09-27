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
beta/vol/log-ADV lifts gross Sharpe materially. The alpha decays within days: daily full rebalancing was
the best net construction (gross/net 1.19/.56 at ~8%/day on the 2022 TRAIN holdout).

**Owner ruling 2026-09-27 (supersedes the earlier 30%/month framing):** the shipped construction is
daily (`--cadence 1`). The 30%/month budget is retired. The combined book must satisfy mean daily one-way
turnover <= 20% of GMV and p95 <= 30% of GMV, deployment excluded. Inside the daily cadence, a no-trade
band is the first lever: it beat partial adjustment in the studies. Handoff §2a has the definitions.

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
5. Everything above must work at `--cadence 1` (a decision every session). At cadence 1 the band applies
   on every session.
6. Daily turnover in GMV units (NAV replay):
   - daily CSV column `one_way_turnover_gmv = traded$ / pre-trade GMV` (GMV = long$ + short$ before
     trading); keep the existing NAV-denominated `one_way_turnover`;
   - per scenario in `summary.json`: `daily_turnover_gmv` {mean, median, p95, max} over scored
     sessions, deployment session excluded (forced exits included);
   - declared ceilings from new CLI flags `--daily-turnover-mean-max` (default 0.20) and
     `--daily-turnover-p95-max` (default 0.30), recorded in recipe.json. Add flags
     `meets_daily_turnover_mean` and `meets_daily_turnover_p95`;
   - leave the existing monthly fields (`months_le_0.30`, `meets_turnover_target_*`) byte-compatible,
     documented in `limitations` as legacy reporting-only. They are no longer the target.

## Constraints
- House style `.agents/cpp/agent.md`. Do NOT compile or run; root builds. Not TDD: implement, then
  fixtures: (a) defaults bit-identical (existing fixtures unchanged); (b) neutralized target has ~0
  exposure and the recorded amplification; (c) skip path on forced error keeps weights and counts;
  (d) band keeps small moves, moves large ones, and v2 distance excludes banded names;
  (e) nav + neutralize + band end-to-end on a tiny role at cadence 1;
  (f) GMV turnover column and the mean/p95 stats hand-checked on a tiny role, with deployment excluded
      and the ceiling flags flipping at the declared thresholds.
- No subagents. Commit (messages end with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`). No push.

## Report
`C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T4-report.md`. Return only: status, commit
SHAs, one-line summary, concerns.
