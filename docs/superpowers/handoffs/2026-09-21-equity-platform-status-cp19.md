# Equity platform status — checkpoint 19 (liquidity-floored universe) — 2026-09-21

Worktree `C:\atx\.worktrees\equity-platform`, branch `feat/equity-platform-20260920`. Nothing committed (no commit authorized); all equity work remains uncommitted/untracked. Nothing in flight.

## Result in one line

On a tradeable universe (21-day mean dollar ADV ≥ $50M at baseline admission) **no signal clears bar 1 on either cut**. The two illiquidity families that passed bar 1 at cp17/cp18 invert (`low_dollar_volume_21` −1.13 [−1.83, −0.51] 7/7 on top-1000): their "premium" lived entirely below the floor. Plain 12-1 momentum is now the best family (+0.47 [−0.21, +1.17], 6/7) but is not significant. The one new trial (`mom_resid_vs_blend`) is +0.05 / +0.35 with CIs spanning zero. Cumulative N = 350.

## Measured numbers (headline: IncludeAuditedTerminalV1 / full, h = 21, pooled 2013–2019 NET decile-spread Sharpe, [2.5 %, 97.5 %] block bootstrap, sign stability k/n; floored universe, breadth ≈ 730–880 names on BOTH cuts)

| signal | top-1000 (floored) | top-3000 (floored) | gross t1000 / t3000 | verdict |
|---|---|---|---|---|
| momentum_252 | +0.47 [−0.21, +1.17] 6/7 | +0.43 [−0.24, +1.12] 5/6 | +0.68 / +0.63 | FAIL (best) |
| momentum_126 / blend_equal | +0.42 / +0.42 | +0.45 / +0.45 | +0.66 / +0.68 | FAIL |
| momentum_volscaled_252 | +0.24 [−0.50, +0.91] | +0.36 [−0.47, +1.17] | +0.44 / +0.56 | FAIL |
| residual_momentum_252 | −0.02 [−0.95, +0.80] | +0.36 [−0.35, +1.11] | +0.17 / +0.55 | FAIL |
| mom_resid_vs_blend (NEW, N_19) | +0.05 [−0.81, +0.81] 4/6 | +0.35 [−0.39, +1.12] 4/5 | +0.25 / +0.55 | FAIL |
| low_dollar_volume_21 | −1.13 [−1.83, −0.51] 7/7 | −0.79 [−1.82, +0.09] 5/6 | −0.34 / −0.01 | INVERTED vs cp18 |
| amihud_21 | −0.46 [−1.21, +0.15] | −0.65 [−1.55, +0.08] | −0.17 / −0.35 | INVERTED vs cp17/18 |
| volume_shock_63 / _neg_63 | −1.04 [−1.88, −0.28] / −0.08 | −1.13 [−2.02, −0.20] / −0.13 | −0.46 / +0.51 | FAIL (turnover 1.7 eats both signs) |
| reversal_5 / continuation_5 | −0.60 / −0.01 | −0.73 [−1.35, −0.07] / +0.15 | −0.27 / +0.32 | FAIL |
| reversal_21, high52, low_vol_63, idio_vol_63, high_vol_63, max_ret_21 | all within [−0.6, +0.1] | all within [−0.6, +0.1] | ≈ 0 | FAIL |

Deflated Sharpe (N = 350): SR0_ann 1.95 (t1000); every DSR ≤ 1e-4.

Cost/capacity view (caveat): momentum_252 long-decile mean ADV $218M, short $175M, one-way turnover 0.94; pooled net Sharpe after a 1/2/5 bps inverse-ADV-rank surcharge: 0.44 / 0.41 / 0.32 (t1000). The floored universe is tradeable at institutional scale; the signal is simply not there at this power (74 pooled h=21 observations per cut).

Per-year momentum_252 h=21 (t1000): 2013 +1.85~, 2014 +0.47, 2015 +0.92, 2016* −1.06, 2017* +0.44, 2018* +0.70, 2019 +0.73 (`*` hole years, `~` n_obs < 20; every per-year cell at h=21 is `~`).

Caveats beside every number: the floored top-3000 is the floored top-1000 plus a handful of names, so the "both cuts" clause of R16-8 is vacuous (R19-5); year-union membership; 2016–2018 hole flags (the floor's "all 21 sessions finite and > 0" rule propagates each corrupted session into 21 rejected sessions: 2017/2018 minimum admitted names 0–1 on those dates, which are then below `min_names_per_date` and excluded, counted); 2013 partial year; no borrow-availability or impact model; sealed 2023–2025 and validation 2020–2022 never read.

Files: `C:/atx/data/equity_scorecard19_scorecard_20260921/{scorecard.csv (5,720 rows), capacity.csv (36 rows), scorecard.md, receipt.json}`; archived `atx-engine/reviews/2026-09-21-equity-alpha-scorecard-cp19.md`; floored baselines `C:/atx/data/equity_scorecard19_base_{Y}_t{CUT}_20260921` (13); valid IC cells `C:/atx/data/equity_scorecard19r2_ic_*` (13); ledgers `atx-engine/reviews/trial-ledger-cp19-floor.jsonl` (attempt 1, unfloored IC by bug, 26 lines) and `trial-ledger-cp19-floor-r2.jsonl` (attempt 2, 26 lines, 0 failed); receipts `iteration16-cells-attempt1-cp19-full{,-r2}.json`; code-identity cell `equity_identity19_*_2014_t1000_20260921` (floor off: every cp18 point series byte-identical).

## Pipeline changes (what was built)

- Liquidity floor as a baseline-admission predicate: `EquityBaselineConfig::{min_dollar_adv, dollar_adv_window}` and `EquityBaselineEvaluation::liquidity_floor_rejected_cells` (`equity_baseline_views.{hpp,cpp}`); CLI `--min-dollar-adv` / `--dollar-adv-window` (`config.{hpp,cpp}`, `stage_equity_baseline.cpp`); the baseline recipe carries `liquidity_floor` only when > 0 (no-floor recipe byte-identical); readiness json carries the rejected count.
- IC stage (`stage_equity_ic.cpp`): takes the floor FROM THE BASELINE RECIPE (never from its own flags) and `bind_fresh_view` now compares admission cell-for-cell with the published baseline — the check that would have refused attempt 1. `kCheckpoint = 19`, `iteration19-` ids, family list 15 (14 retained + `mom_resid_vs_blend`), `kEquityFamilyRetainedCount = 14`, request.json `universe.liquidity_floor`.
- Runner: `--base-name-prefix/--base-stamp`, `--min-dollar-adv/--dollar-adv-window` pass-through, `VALIDATION_YEARS` 2020–2022 table refused without `--allow-validation` (and the binaries still seal at 2020-01-01).
- Tests: `LiquidityFloorRejectsBelowAdvAndKeepsCountsHonest` (4 cases) + re-indexed family test; 60/60 pass.
- atx-db fundamentals read (R19-2): point-in-time schema exists (`fundamental_points`, `fundamental_statement_points`, `fundamental_standardized`, `fundamental_pit_snapshot`, item dictionary 1,059 items) but ingestion covers ~18 % of issuers and every standardized/derived table is empty — not usable for a 2013–2019 universe until the companyfacts archive load and standardization stages run.

## Throughput

Baseline phase ~1 s per cell (Release), IC ≤ 49 s per cell with 18 signals, 13 cells at `--parallel 4` ≈ 3 min; peak WS 630 MB. Scorecard ≈ 8 min for 18 signals.

## Rulings I made (R19-1 … R19-6, full text in progress.md)

R19-1 the floor is an admission predicate, never a trial; changing it is a new checkpoint · R19-2 fundamentals deferred to atx-db · R19-3 (implicit in the caveats) hole sessions propagate 21 rows under the floor; reported, not patched · R19-4 the accidental unfloored IC run of attempt 1 counts toward N (+20) · R19-5 with the floor the cut dimension collapses; flagged for the next design addendum · R19-6 price/volume single-signal families are exhausted on the tradeable universe; new alpha needs a new data source or more power, never a looser bar.

## Traps hit this session

- The IC stage re-evaluates the baseline itself; any universe predicate must be read back from the baseline recipe or it is silently dropped (attempt 1). The mask comparison in `bind_fresh_view` now enforces this.
- Runner `--attempt N` suffixes every dir including the reused context, so a re-run uses a new `--ic-name-prefix` instead.
- Python stdout to a log file is block-buffered; count output dirs, not RUN lines, for progress.
