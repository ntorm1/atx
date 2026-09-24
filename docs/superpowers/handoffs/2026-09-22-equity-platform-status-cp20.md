# Equity platform status — cp20 complete (2026-09-22)

Worktree `C:\atx\.worktrees\equity-platform`. No commit authorized; everything preserved on disk. Memory of record: `.superpowers/sdd/equity-platform-parent-goal/progress.md` L182–L186 (R20-4, cp20 SWEEP r2, cp20 SCORECARD, R20-5, cp20 DECISION).

## What happened this session
1. Stale test: `atx-impl/tests/equity_baseline_views_test.cpp` already expected `256U`; a stray half-comment line above it was removed. Four suites 60/60, 1 pre-existing skip.
2. earnFlag encoding (R20-4): NaN on non-announcement sessions (ORATS `parse_f64` maps an empty cell to quiet_NaN; panel is raw passthrough; the tbltickerhistory validation shows finite 260 / nan 6343). No registry op maps NaN to 0, but `(ts_count_nans(earnFlag, 1) > 0 ? 0 : earnFlag)` is exact with existing ops. The three DSLs in `atx-impl/src/equity_baseline_views.hpp` were rewritten in that form; no engine change; warmup still 256; same 7 configurations; N unchanged.
3. Sweep r2: 13/13 cells exit 0 (2017_t3000 NOT FIT), ledger `atx-engine/reviews/trial-ledger-cp20-batch3a-r2.jsonl` (26 lines), receipt `build-equity/audits/iteration16-cells-attempt1-cp20-full-r2.json`. Attempt-1 cell `equity_scorecard20_ic_2014_t1000_20260921` stays on disk as the failed attempt (record, not N).
4. Identity vs cp19r2 (18 retained signals, point series, CIs skipped per R18-3, numeric tol rel 1e-9): 12/13 cells 18/18; 2017_t1000 17/18 with amihud_21 the only exception (root cause R20-5 below). cp20 attempt-1 vs cp20r2 on 2014_t1000 is byte-identical (deterministic).
5. Scorecard: `C:/atx/data/equity_scorecard20_scorecard_20260922/` (scorecard.csv 7,960 rows, capacity.csv 50 rows, scorecard.md), archived to `atx-engine/reviews/2026-09-21-equity-alpha-scorecard-cp20.md`, `--declared-n 490`.

## Numbers (h=21, pooled NET Sharpe [2.5%, 97.5%], sign stability; t1000 | t3000; all pre-registered +)
| family | t1000 | t3000 | note |
|---|---|---|---|
| abr_21 | −0.43 [−1.32, +0.38] 5/7 | +0.03 [−1.11, +0.83] 1/6 | gross +0.39 / +0.79, turnover 0.08 |
| abr_63 | −0.42 [−0.88, +0.38] 4/7 | −0.46 [−1.10, +0.12] 4/6 | coverage 89.5 % (63-window straddles holes) |
| earn_premium | −0.64 [−1.37, +0.02] 5/7 | −0.46 [−1.18, +0.33] 3/6 | turnover 0.53 |
| iv_rv_log | −1.00 [−1.84, −0.24] 5/7 | −1.60 [−2.39, −0.89] 5/6 | significant, wrong sign |
| iv_slope | +0.15 [−0.67, +0.91] 3/7 | −0.38 [−1.14, +0.31] 4/6 | |
| d_iv_21 | −1.48 [−2.27, −0.79] 7/7 | −1.63 [−2.69, −0.83] 6/6 | significant, wrong sign |
| mom252_sector_neutral | +0.37 [−0.31, +1.04] 5/7 | +0.44 [−0.23, +1.13] 5/6 | ≈ momentum_252 (+0.47 / +0.43) |

Coverage (finite admitted cells / admitted, all 13 cells, 2,380,064 admitted): abr_21 100 %, abr_63 89.5 %, earn_premium 100 % (abr/earn_premium are 100 % by construction after R20-4), iv_rv_log 98.6 %, iv_slope 98.6 %, d_iv_21 98.2 %, mom252_sector_neutral 100 %. Breadth 723–797. Caveats: R19-5 (both cuts are the same floored set, "both cuts" met vacuously), R16-8 bars unchanged, 2016/2017 hole-flagged.

**Nothing clears R16-8** (no h=21 pooled net ci_lo > 0 for any of the 25 signals on either cut). Cumulative N = 490. Best remains momentum_252 (cp19).

## Rulings
- **R20-4** earnFlag NaN-to-0 form via existing ops; same configurations; N unchanged.
- **R20-5** amihud_21 2017_t1000 drift is an engine defect, not span aliasing. `amihud_21 = ts_mean(abs(close / delay(close, 1) - 1) / (raw_close * volume), 21)` divides by dollar volume with no guard; `observed_numeric` admits volume == 0 cells; a zero-volume day with nonzero return gives +inf. `ts_online_sum_family` (atx-engine ts_ops.hpp:372–406) treats only NaN specially, so `sx += inf` then `sx -= inf` leaves NaN for the rest of that instrument column (the batch oracle would recover; the online kernel never resets). cp20's derived warmup 256 (vs 252 in cp19: request.json `warmup_observations_derived`) pulls 4 earlier rows into one thin name's window; that name is admitted under the floor only on dates 192–223, hence n_used exactly 1 lower there. Fix deferred to a declared ruling (it changes amihud_21's recorded series). Any family dividing by a possibly-zero field is exposed.
- **Decision**: no Stage 4; validation 2020–2022 unread; 2020-01-01 seal untouched. d_iv_21 / iv_rv_log are NOT flipped (R17-7). Web check: the + signs were literature-grounded (Bali–Hovakimian 2009: realized-minus-implied spread negatively related to expected returns, i.e. IV−RV positive; An–Ang–Bali–Cakici 2014: rising call IV predicts higher returns); the ORATS ATM-centre IV on this floored 2013–2019 set contradicts. A flipped re-registration would have no independent grounding and is rejected, not carried to cp21.

## Next
- **cp21 default** = fundamentals batch 3b via atx-db (v8 §1–§2). **GATED**: warehouse lock held from 18:13 by `scripts/warehouse_activate.py --only companyfacts_load` (PID 16672, ~1.2 GB, launched by `run_memory_guarded.py` from the tier1-parity sprint). Never kill it; never write while it runs (R20-1).
- **Unblocked alternative for the user to rule on** (not started, N not incremented): batch 3c from fields already borrowed into the context (`high`, `low`, `open`, `nEarnCnt_5d`): overnight-vs-intraday return decomposition (Lou–Polk–Skouras 2019), Parkinson / Garman–Klass range vol, Corwin–Schultz high-low spread. Zero data-lane cost; same runner; needs a pre-registration line and N += 20 per family.
- **Engine fix candidate** (declared ruling required): non-finite guard in `ts_online_sum_family` + oracle parity + one unit test; re-measure amihud_21 on 2017_t1000 once.

## Files touched this session (uncommitted)
`atx-impl/src/equity_baseline_views.hpp` (3 DSLs + comment), `atx-impl/tests/equity_baseline_views_test.cpp` (comment only), `.superpowers/sdd/equity-platform-parent-goal/progress.md` (+5 lines), `atx-engine/reviews/2026-09-21-equity-alpha-scorecard-cp20.md` (new), `atx-engine/reviews/trial-ledger-cp20-batch3a-r2.jsonl` (new), `build-equity/audits/iteration16-cells-attempt1-cp20-full-r2.json` and `-dryrun.json` (new), this doc, `2026-09-22-next-goal-prompt-v10.md`. Data: `C:/atx/data/equity_scorecard20r2_ic_*` (13 dirs), `C:/atx/data/equity_scorecard20_scorecard_20260922/`. Scratch only (not in repo): `identity_cp20_vs_cp19r2.py`, `coverage_cp20r2.py` in the session scratchpad; the v10 prompt asks to recreate them under `build-equity/audits/`.
