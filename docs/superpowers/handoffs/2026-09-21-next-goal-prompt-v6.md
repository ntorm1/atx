Goal prompt for the next parent agent (paste into /goal) — v6, post-cp17, alpha-first

PROCESS (unchanged from v5): no hashing/TDD/format ceremony; alpha generation and pipeline
building only; a short progress.md entry per ruling, measurement or failed attempt is the memory.
v5 stays on disk as the record.

CONTEXT TO LOAD (delegate the reads; keep only conclusions): read
docs/superpowers/handoffs/2026-09-21-equity-platform-status-cp17.md (whole), then
.superpowers/sdd/equity-platform-parent-goal/progress.md from "Session start (v5 goal" to the end,
then the v5 prompt docs/superpowers/handoffs/2026-09-21-next-goal-prompt-v5.md for the standing
guard-rails and the "DROPPED" list. Worktree C:\atx\.worktrees\equity-platform. Stop point:
checkpoint 17 COMPLETE. Scorecard atx-engine/reviews/2026-09-21-equity-alpha-scorecard-cp17.md.
Frozen design files unchanged (addendum only). No commit authorized. Preserve work; version
failed attempts.

THE MEASURED FACTS: cp17 batch 1 (reversal_21, reversal_5, high52_proximity, low_vol_63,
idio_vol_63, amihud_21, volume_shock_63; pre-registered signs; 13 cells; N_17 = 140, cumulative
N = 170) — NO CANDIDATE under R16-8. amihud_21 passes bar 1 on both cuts (pooled net h=21
+0.73 [+0.23, +1.27] t1000, +1.00 [+0.03, +1.89] t3000) and fails bar 2 (5/7, 4/6); rank-IC ≈ 0
so the spread is the illiquid tail decile, where the flat cost model is least credible.
reversal_5 (t3000 −1.06 [−1.61, −0.46], h=5 −1.38 [−2.05, −0.72], 6/6), low_vol_63 (t1000 −0.90
[−1.66, −0.15]) and volume_shock_63 (t1000 h=5 −1.56 [−2.34, −0.77], 7/7) are significantly
NEGATIVE under their pre-registered signs — i.e. short-term continuation, high-vol premium and
abnormal-volume underperformance on 2013–2019 top-1000. Deflated Sharpe at N=170 clears nothing
(SR0_ann 2.18 / 1.73; max DSR 0.09). Momentum rows identical to cp16.

THE PIPELINE NOW: adding a family = one DSL line + one name in
atx-impl/src/equity_baseline_views.hpp (kEquityFamilyDsl/kEquityFamilySignalNames); warmup and
declared N derive from it. Sweep = equity-ic only over the 13 existing cp16 ctx/base dirs with
the Release binary build-equity-rel/bin/atx-impl.exe (configure/build via
scripts/atx-build.ps1 -Preset equity-rel): 212 s wall for 13 cells at --parallel 4. Runner:
build-equity/audits/iteration16_run_cells.py --phases ic --ic-name-prefix equity_scorecard18
--ic-stamp <YYYYMMDD> --ledger atx-engine/reviews/trial-ledger-cp18-<name>.jsonl
--receipt-tag=-cp18-full --atx-impl build-equity-rel/bin/atx-impl.exe --parallel 4
(--dry-run first; a fresh ledger file per checkpoint; --resume to continue). Scorecard:
iteration16_equity_scorecard.py --glob "C:/atx/data/equity_scorecard18_ic_*" --out <new dir>
--cells-receipt <receipt> --declared-n <cumulative N> --n-note "<how N was counted>" --title
"<cp18 ...>". Per-checkpoint constants still to bump in stage_equity_ic.cpp: kCheckpoint,
kTrialIdPrefix, kTrialCountRule text (kTrialCountDeclared derives). The StageEquityIc test
expects checkpoint 17 / the iteration17 prefix — update those two literals with the bump.

WHAT TO DO — Stage 3, batch 2 (default; do not ask):
1. Pre-register (one progress.md line + the fresh sidecar's genesis line written by the binary):
   (a) the MIRRORED signs of the three significantly-negative families as new configurations
   (continuation_5 = close/delay(close,5)−1; high_vol_63 = ts_std(ret,63); volume_shock_neg_63
   = 0 − log(volume/ts_mean(volume,63))) — each is a new trial, N += 20 per family; (b) the
   next literature families from close/raw_close/volume: 12-1 momentum with vol scaling
   (momentum_252 / ts_std(ret,252)), residual momentum (ts_mean of normalize(ret) over 252
   skipping 21), max-daily-return (0 − ts_max(ret,21)), turnover (0 − ts_mean(volume,21)/…),
   long-term reversal (delay(close,252)/delay(close,1260)−1 needs a 1,260-row warmup — check the
   contexts have it; if not, skip and say so); (c) amihud_21 retained unchanged as the lead
   (already declared; do NOT re-declare). Then bump the checkpoint constants, compile-check,
   build atx-impl (equity-rel) and atx-impl-tests (equity-dev), run the four suites, sweep,
   scorecard with --declared-n = 170 + new N.
2. Cost/capacity view for illiquidity families (R17-8; small, in the scorecard script, not the
   engine): from ic.csv/quantile_spread.csv add per-signal mean dollar-ADV of the top and bottom
   decile if the columns exist; if they do not, add ONE column to quantile_spread.csv in
   stage_equity_ic.cpp (mean raw_close×volume over the decile's members at t) — the identity
   check on momentum rows must still pass — and print it beside amihud's spread. State whether
   the net spread survives a cost of k × (1/ADV-rank) for k in {1, 2, 5} bps — a caveat, not a
   new bar.
3. If a family clears R16-8 on both cuts: Stage 4 — combine the candidate(s) through the
   risk/portfolio path in atx-engine (atx-impl driving it), measured the same way. If none:
   batch 3 (fundamentals need atx-db; say so explicitly before spending time there).
4. Throughput: the sweep is no longer the bottleneck (3.5 min). The scorecard script is a few
   minutes; do not optimise further unless a batch exceeds ~25 signals. Do NOT re-run panel or
   baseline phases unless the universe changes.

DEFERRED unless it blocks a number: hole remediation attempt 2; as-of membership predicate;
stdlib oracle suite; receipt writer; design/end reviews; a DSR variance model other than the
realised cross-configuration dispersion (state the choice; do not tune it).

GUARD-RAILS THAT STAY: every configuration counted toward N before it runs (mirrored signs
included); no post-hoc sign flips (R17-7); sealed 2023–2025 never read, validation 2020+ never
read; no live trading; synthetic fixtures are never evidence; caveats beside numbers; PCS
2013-05-01 stays rejected; frozen design files unchanged; kMaxIcInstruments 4096 via per-year
contexts (2017 t3000 NOT FIT); cp16 baseline dirs are not book baselines; never time anything in
build-equity (Debug); ledger files are per checkpoint and immutable; receipt tags are
checkpoint-specific.

END WITH: the scorecard for whatever was measured, "Rulings I made", a short status file, and a
v7 goal prompt in this shape. Preserve your context window.
