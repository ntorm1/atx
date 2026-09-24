# Equity platform status — checkpoint 17 (Stage 3, batch 1) — 2026-09-21

Worktree `C:\atx\.worktrees\equity-platform`, branch `feat/equity-platform-20260920`. Nothing committed (no commit authorized); all equity work remains uncommitted/untracked as at cp16. Nothing in flight.

## Result in one line

Seven new signal families measured on the cp16 13-cell universe (2013–2019, top-1000 + top-3000). **No candidate** under the pre-registered R16-8 bars. `amihud_21` is the only family to clear bar 1 (pooled net 2.5 % ci_lo > 0 at h=21 on both cuts) and fails bar 2 (sign stability 5/7, 4/6). Three families are significantly *negative* under their pre-registered sign. Deflated Sharpe at N = 170 clears nothing (max DSR 0.09).

## Measured numbers (headline: IncludeAuditedTerminalV1 / full, h = 21, pooled 2013–2019 NET decile-spread Sharpe, [2.5 %, 97.5 %] block bootstrap, sign stability k/n over reportable years)

| family (pre-registered sign) | top-1000 | top-3000 | gross t1000 / t3000 | verdict |
|---|---|---|---|---|
| amihud_21 (+illiquid) | +0.73 [+0.23, +1.27] 5/7 | +1.00 [+0.03, +1.89] 4/6 | +0.87 / +1.22 | bar1 PASS, bar2 FAIL |
| reversal_21 (+past losers) | −0.42 [−1.23, +0.27] 5/7 | −0.56 [−1.19, +0.15] 5/6 | −0.18 / −0.29 | FAIL |
| reversal_5 (+past losers) | −0.30 [−1.50, +0.41] 5/7 | −1.06 [−1.61, −0.46] 6/6 | −0.13 / −0.79 | FAIL (negative) |
| high52_proximity (+near high) | −0.25 [−1.02, +0.45] 3/6 | −0.17 [−1.01, +0.77] 3/5 | −0.07 / +0.02 | FAIL |
| low_vol_63 (+low vol) | −0.90 [−1.66, −0.15] 4/7 | −0.70 [−1.27, +0.01] 4/6 | −0.73 / −0.62 | FAIL (negative) |
| idio_vol_63 (+low idio vol) | −0.73 [−1.48, +0.02] 4/7 | −0.61 [−1.15, +0.20] 4/6 | −0.53 / −0.53 | FAIL |
| volume_shock_63 (+abnormal volume) | −1.08 [−1.81, −0.38] 6/7 | +0.29 [−1.47, +0.71] 2/6 | −0.51 / +0.41 | FAIL (negative t1000) |
| momentum_252 / 126 / blend (cp14, re-measured) | −0.04 / −0.19 / −0.23 | −0.26 / +0.08 / −0.10 | as cp16 | FAIL (identical to cp16 to 6 dp) |

Per-year (h=21) amihud_21: t1000 2013 +2.57~, 2014 +0.18, 2015 −0.24, 2016* +1.37, 2017* −0.79, 2018 +1.07, 2019 +0.88; t3000 +2.69~, −0.24, −0.24, +1.67*, NOT FIT, +1.69, +0.70 (`*` hole years, `~` n_obs < 20). Rank-IC ≈ 0 (+0.009 / −0.005): the spread lives in the illiquid tail decile. Implied turnover 0.0015 / 0.0007 (persistent), breadth 998 / 2,772.

Deflated Sharpe (Bailey–López de Prado, N = 170 = N_14 30 + N_17 140, V = cross-configuration Sharpe variance per cut): SR0_ann 2.18 (t1000) / 1.73 (t3000); every DSR ≤ 0.09. The bar is high because the dispersion across the 200 configurations per cut (including the wrong-signed families) is ~0.6–0.8 annualised.

Caveats beside the numbers: 2013 is a partial year (from 2013-04-04) and carries amihud's largest per-year value; the flat 2 × trade_bps + 365 bps borrow cost model is least credible exactly where amihud's spread sits; membership is year-union, not as-of; 2016–2017 (all signals) and 2018 (252-lookback signals) carry the archive-hole flag; reversal_5 materiality FIRES on t1000; 2017 top-3000 NOT FIT (5,072 > 4,096 cap); no capacity/impact/borrow-availability model; sealed 2023–2025 never read, validation 2020+ never read.

Files: `C:/atx/data/equity_scorecard17_scorecard_20260921/{scorecard.csv (3,600 rows), scorecard.md, receipt.json}`; archived `atx-engine/reviews/2026-09-21-equity-alpha-scorecard-cp17.md`; cells `C:/atx/data/equity_scorecard17_ic_{Y}_t{CUT}_20260921` (13); receipts `build-equity/audits/iteration16-cells-attempt1-{measure2014,cp17-full}.json`; ledger `atx-engine/reviews/trial-ledger-cp17-families.jsonl` (26 lines: 13 pre-registered + 13 completed, checkpoint 17, trial_count_declared 140 each, 0 failed).

## Pipeline changes (what was built)

- `atx-impl/src/equity_baseline_views.{hpp,cpp}`: `kEquityFamilyDsl` / `kEquityFamilySignalNames` (7 entries; adding a family = one DSL line + one name) and `evaluate_equity_families()`: compiles the family list into one program, derives warmup from `Program::required_lookback` (251 here), evaluates in one VM pass over the feature window, admits on the baseline mask (eligible AND both momentum signals ready) with per-signal NaN readiness. Baseline stage, recipes and reuse chain untouched.
- `atx-impl/src/stage_equity_ic.cpp`: signal table = baseline 2 + blend + families; `kCheckpoint = 17`; `kTrialCountDeclared = families × H × V × R = 140` (real count of NEW configurations); trial id `iteration17-cross-section-ic-NNNN`; families block in request.json / ic_summary.json; `append_trial_with_retry` (the ledger never waits; parallel cells retry a live lock 120 × 250 ms).
- `CMakePresets.json`: `equity-rel` (Release, no AVX2, no IPO, `build-equity-rel/`). build-equity was a Debug build (`/Od /RTC1`); every cp14–cp16 sweep ran unoptimised.
- Tests: `EquityBaselineViews.FamilyProgramCompilesDerivesWarmupAndGatesOnBaselineMask` (new); `StageEquityIc` constants now derive from the header. `ctest -R "StageEquityIc|EquityIc|TrialLedger|EquityBaselineViews"`: all pass (one pre-existing skip).
- `build-equity/audits/iteration16_run_cells.py`: `--phases`, `--ic-name-prefix`, `--ic-stamp`, `--parallel`, `--log-tag`; per-cell `run_cell` + thread pool.
- `build-equity/audits/iteration16_equity_scorecard.py`: gross Sharpe (+CI on pooled rows, separate RNG so net draws are byte-identical), deflated Sharpe (`dsr_prob`, `sr0_ann`, `declared_n`), `--declared-n/--n-note/--title`, `high52_proximity` in the 2018 hole set, `math.fsum` Sharpe (7× faster). Re-run over the cp16 cells reproduces cp16's scorecard.csv exactly (1,080 rows × 21 columns) with both the old and the new Sharpe path.

## Throughput (before → after)

| step | cp16 | cp17 |
|---|---|---|
| equity-ic, one cell (2014 t1000) | Debug, 3 signals: 50.9 s | Debug, 10 signals: 171.9 s → Release: **29.6 s** (5.8×), peak WS 156 MB |
| 13-cell family sweep | ~33 min of phase time (panel+baseline+ic, serial, Debug) | **212 s** wall, ic-only, 4 cells in parallel, Release (per cell 18.6–106 s; t3000 peak WS ≤ 428 MB) |
| identity | — | momentum_252/126/blend rows of ic.csv, quantile_spread.csv, signal_autocorr.csv byte-identical to cp16 in all 13 cells; scorecard rows equal to 6 dp |
| scorecard script | statistics.stdev bootstrap (~15+ min for 3 signals) | fsum path, 10 signals in a few minutes |

Where the time goes now: request.json lands ~2 s in (context load + all-family evaluation); the rest is the 40 blocks × 5 horizons IC/decile/bootstrap loop in the engine. The panel/baseline phases were not re-run (contexts reused from disk).

## Rulings I made (R17-1 … R17-8, full text in progress.md)

R17-1 families evaluated inside equity-ic on the baseline admission mask, cp16 universe unchanged · R17-2 warmup derived from required_lookback · R17-3 signal-only sweep reuses cp16 ctx/base dirs · R17-4 Release binary, identity checked by diffing momentum rows · R17-5 parallel cells + stage-side ledger-lock retry · R17-6 fsum Sharpe (verified identical at 6 dp) · R17-7 no post-hoc sign flips; a mirrored sign is a new pre-registered configuration for batch 2 · R17-8 amihud_21 is the batch-2 lead but not a candidate; illiquidity families need a cost/capacity view before "tradeable".

Deferred (nothing here blocked a number): hole remediation attempt 2; as-of membership predicate; stdlib oracle suite for the scorecard; receipt writer; design/end reviews.

## Traps hit this session

- Receipt names are immutable: `--receipt-tag=-full` collided with cp16's receipt; use a checkpoint-specific tag.
- A `%` inside a `%`-formatted markdown header crashed `render_md` after the CSV was written (fixed).
- `atx-impl-tests` does not rebuild `atx-impl.exe`; the Debug exe was rebuilt separately before profiling.
- `build-equity` is Debug; never time anything there.
