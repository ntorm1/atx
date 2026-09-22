# Equity platform status — checkpoint 18 (Stage 3, batch 2) — 2026-09-21

Worktree `C:\atx\.worktrees\equity-platform`, branch `feat/equity-platform-20260920`. Nothing committed (no commit authorized); all equity work remains uncommitted/untracked as at cp17. Nothing in flight.

## Result in one line

Seven new families (three mirrored signs + four literature families) measured on the cp16 13-cell universe. **No candidate** under R16-8. `low_dollar_volume_21` is the best family measured so far (bar 1 PASS on both cuts, positive at every horizon on top-1000) and fails bar 2 (6/7, 5/6); it is the same illiquidity-tail exposure as `amihud_21` (rank-IC ≈ 0, long decile mean ADV $2–25M). `max_ret_21` is significantly *negative* on both cuts. Deflated Sharpe at N = 310 clears nothing (SR0 ≈ 2.4 annualised).

## Measured numbers (headline: IncludeAuditedTerminalV1 / full, h = 21, pooled 2013–2019 NET decile-spread Sharpe, [2.5 %, 97.5 %] block bootstrap, sign stability k/n)

| family (pre-registered sign) | top-1000 | top-3000 | gross t1000 / t3000 | verdict |
|---|---|---|---|---|
| low_dollar_volume_21 (+low $vol) | +0.90 [+0.57, +1.86] 6/7 | +1.14 [+0.11, +1.95] 5/6 | +1.05 / +1.44 | bar1 PASS, bar2 FAIL |
| amihud_21 (retained, +illiquid) | +0.73 [+0.23, +1.27] 5/7 | +1.00 [+0.03, +1.89] 4/6 | +0.87 / +1.22 | bar1 PASS, bar2 FAIL (= cp17) |
| residual_momentum_252 (+) | +0.38 [−0.23, +1.04] | +0.36 [−0.39, +1.22] | +0.57 / +0.57 | FAIL |
| momentum_volscaled_252 (+) | +0.23 [−0.46, +0.87] | +0.15 [−0.63, +1.08] | +0.40 / +0.36 | FAIL |
| high_vol_63 (mirror, +high vol) | +0.53 [−0.29, +1.29] | +0.57 [−0.31, +1.07] | +0.70 / +0.65 | FAIL |
| continuation_5 (mirror) | −0.04 [−0.63, +1.00] | +0.51 [−0.31, +1.13] | +0.11 / +0.78 | FAIL |
| volume_shock_neg_63 (mirror) | −0.16 [−1.03, +0.55] | −0.51 [−1.19, −0.05] | +0.43 / −0.39 | FAIL (sign is cut-dependent) |
| max_ret_21 (+low MAX) | −0.93 [−1.41, −0.44] | −1.23 [−2.12, −0.33] | −0.70 / −1.01 | FAIL (negative both cuts) |
| momentum_252 / 126 / blend (cp14) | −0.04 / −0.19 / −0.23 | −0.26 / +0.08 / −0.10 | as cp16 | FAIL (identical to cp16) |

low_dollar_volume_21 per-year (h=21): t1000 2013 +2.53~, 2014 +1.72, 2015 +0.27, 2016* +2.13, 2017* −1.23, 2018 +1.17, 2019 +1.54; t3000 +2.45~, −0.04, +0.57, +3.32*, NOT FIT, +1.57, +0.08 (`*` hole years, `~` n_obs < 20 — every per-year cell at h=21). Rank-IC +0.013 / −0.000; implied turnover 0.0014 / 0.0008.

Deflated Sharpe (N = 310 = 30 + 140 + 140): SR0_ann 2.39 (t1000) / 2.37 (t3000); max DSR 0.08 (h=63 only).

Cost/capacity view (new, R17-8; a caveat, not a bar): mean 21-day dollar ADV of the long / short decile and the pooled net Sharpe after an illiquidity surcharge s_k = k bps × one-way turnover × ½(1/advrank_long + 1/advrank_short):

| signal | cut | ADV long | ADV short | turnover | k=0 | k=1 | k=2 | k=5 bps |
|---|---|---|---|---|---|---|---|---|
| amihud_21 | 1000 | $34.9M | $826M | 0.74 | 0.73 | 0.72 | 0.70 | 0.66 |
| amihud_21 | 3000 | $3.5M | $467M | 0.64 | 1.00 | 0.98 | 0.96 | 0.90 |
| low_dollar_volume_21 | 1000 | $25.3M | $916M | 0.87 | 0.90 | 0.88 | 0.86 | 0.80 |
| low_dollar_volume_21 | 3000 | $2.4M | $494M | 0.72 | 1.14 | 1.11 | 1.08 | 0.98 |

Both survive the surcharge arithmetically. They do not survive the reading: the long leg is the least-liquid decile of the universe by construction (advrank 0.10), rank-IC is zero, the 2013 partial year and 2016 hole year carry the largest per-year values, materiality FIRES on both cuts, and a $2–25M-ADV long book has no platform capacity. Ruling R18-5: one economic finding (an illiquidity/size premium inside the top-1000/3000 that the flat cost model cannot price), two declared trials, zero candidates.

Caveats beside every number: year-union membership (not as-of); 2016–2017 hole flags on all signals and 2018 on 252-lookback signals; 2013 partial year; no borrow-availability or impact model; 2017 top-3000 NOT FIT; sealed 2023–2025 never read, validation 2020+ never read; mirrored families are not exact negations of their originals (per-date gross spread corr 0.998–0.9999) because floor(p·Q/n) bucketing is asymmetric under reversal when n is not a multiple of 10 (R18-4).

Files: `C:/atx/data/equity_scorecard18_scorecard_20260921/{scorecard.csv (3,584 rows), capacity.csv (22 rows), scorecard.md, receipt.json}`; archived `atx-engine/reviews/2026-09-21-equity-alpha-scorecard-cp18.md`; cells `C:/atx/data/equity_scorecard18_ic_{Y}_t{CUT}_20260921` (13); receipt `build-equity/audits/iteration16-cells-attempt1-cp18-full.json`; ledger `atx-engine/reviews/trial-ledger-cp18-batch2.jsonl` (26 lines: 13 pre-registered + 13 completed, checkpoint 18, trial_count_declared 140 each, 0 failed).

## Pipeline changes (what was built)

- `atx-impl/src/equity_baseline_views.hpp`: family list is now 8 entries (amihud_21 retained + 7 new) with `kEquityFamilyRetainedCount = 1` (leading entries declared at an earlier checkpoint are re-measured, not counted); `kEquityDollarAdvDsl` / `kEquityDollarAdvName` ancillary series. Adding a family is still one DSL line + one name.
- `atx-engine/{include/atx/engine/eval,src/eval}/cross_section_ic.{hpp,cpp}`: optional `CrossSectionIcInput::aux` span averaged over each decile's members inside `decile_spread` → `QuantileBucketStat::mean_aux` / `n_aux_dates`. IC, spread, turnover and bootstrap paths untouched; validator rejects a wrong-extent aux.
- `atx-impl/src/stage_equity_ic.cpp`: `kCheckpoint = 18`, `iteration18-` trial ids, `kTrialCountDeclared = (families − retained) × 5 × 2 × 2 = 140`; second family evaluation for dollar ADV fed as `input.aux`; `quantile_spread.csv` gains a 28th, last column `mean_dollar_adv` (blank on SPREAD rows); request.json `families.dollar_adv` + `retained_count`; ic_summary `family_retained_count`.
- Tests: `EquityBaselineViews` family test rewritten for the new list (warmup 252 derived) + `DollarAdvAncillaryEvaluatesOnBaselineMask`; `StageEquityIc` literals → checkpoint 18, 28 spread columns. `ctest -R "StageEquityIc|EquityIc|TrialLedger|EquityBaselineViews"`: 48/48 pass (one pre-existing skip).
- `build-equity/audits/iteration16_equity_scorecard.py`: cost/capacity view (`capacity.csv`, md section, RECIPE entry, receipt sha256s); degrades to one line when the column is absent. Re-run over the cp17 cells reproduces cp17's scorecard.csv byte-for-byte.

## Identity and throughput

- momentum_252/126/blend rows of ic.csv, quantile_spread.csv (first 27 columns) and signal_autocorr.csv byte-identical to cp17 in 13/13 cells. amihud_21 (same DSL): spreads, rank-IC and decile means identical; pearson_ic drifts ≤ 3e-11 relative (family warmup 251 → 252 shifts the rolling-sum start); engine-level bootstrap CIs differ because one RNG stream is consumed in block order and amihud's block index changed (R18-3: engine CIs are not compared across checkpoints; the scorecard's pooled CIs use their own per-key seed and reproduce cp17 exactly).
- Sweep: 13 cells ic-only, Release, `--parallel 4`, per-cell 18–104 s, peak WS 463 MB, ~4 min wall. Scorecard script ~5.5 min for 11 signals.

## Rulings I made (R18-1 … R18-6, full text in progress.md)

R18-1 long-term reversal not declared (1,260-row lookback vs 256-row context warmup; a panel-phase change) · R18-2 dollar ADV enters the engine only as an optional aux span; identity re-checked · R18-3 engine bootstrap CIs are stream-order dependent and not compared across checkpoints · R18-4 mirrored families are not exact negations (bucket asymmetry); reported as measured · R18-5 low_dollar_volume_21 and amihud_21 are one illiquidity-tail finding, not candidates; no further illiquidity variants until the universe carries a liquidity floor · R18-6 the DSR variance is the realised cross-configuration dispersion including wrong-signed families; stated, not tuned.

Deferred (nothing here blocked a number): hole remediation attempt 2; as-of membership predicate; stdlib oracle suite; receipt writer; design/end reviews.

## Traps hit this session

- `--atx-impl` must be an absolute path when the runner is launched from Bash (relative path → `WinError 2` inside `Popen`); the five empty `*_ic-cp18-full.log` files from that launch are kept, and per-phase logs open in `x` mode, so a relaunch needs `--log-tag`.
- Nested `powershell -Command` loses `|` inside an unquoted `-Ctest -R` regex; quote it.
- `atx-impl-tests` does not rebuild `atx-impl.exe`; the Release exe is a separate `-Preset equity-rel build atx-impl`.
