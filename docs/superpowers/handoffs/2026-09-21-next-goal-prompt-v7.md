Goal prompt for the next parent agent (paste into /goal) — v7, post-cp18, alpha-first

PROCESS (unchanged from v5/v6): no hashing/TDD/format ceremony; alpha generation and pipeline
building only; a short progress.md entry per ruling, measurement or failed attempt is the memory.
v5/v6 stay on disk as the record.

CONTEXT TO LOAD (delegate the reads; keep only conclusions): read
docs/superpowers/handoffs/2026-09-21-equity-platform-status-cp18.md (whole), then
.superpowers/sdd/equity-platform-parent-goal/progress.md from "Session start (v6 goal" to the end,
then the v5 prompt docs/superpowers/handoffs/2026-09-21-next-goal-prompt-v5.md for the standing
guard-rails and the "DROPPED" list. Worktree C:\atx\.worktrees\equity-platform. Stop point:
checkpoint 18 COMPLETE. Scorecard atx-engine/reviews/2026-09-21-equity-alpha-scorecard-cp18.md.
Frozen design files unchanged (addendum only). No commit authorized. Preserve work; version
failed attempts.

THE MEASURED FACTS: 18 price/volume families measured over cp14/cp17/cp18 (N = 310) on the 13
cp16 cells (2013–2019, top-1000 + top-3000) — NO CANDIDATE under R16-8. The only bar-1 passes
are the two illiquidity-tail families (low_dollar_volume_21 +0.90 [+0.57, +1.86] / +1.14
[+0.11, +1.95]; amihud_21 +0.73 / +1.00): both long the least-liquid decile (mean ADV $2–35M),
rank-IC ≈ 0, sign stability 6/7 and 5/6 at best, materiality FIRES, and the cost/capacity view
(capacity.csv) shows a $2–25M-ADV long book. Ruling R18-5: one economic finding, zero candidates,
no further illiquidity variants until the universe carries a liquidity floor. Residual momentum
(+0.38 / +0.36, gross +0.57) and vol-scaled momentum (+0.23 / +0.15) are positive but inside
their CIs; MAX is significantly inverted (−0.93 / −1.23); short-term reversal/continuation,
vol, volume-shock have no consistent sign across cuts. DSR at N = 310: SR0_ann ≈ 2.4 (R18-6).

WHAT THE FACTS SAY: price/volume-only cross-sectional families on this universe are exhausted
at the single-signal level; what remains positive is (a) a size/illiquidity premium the
platform cannot trade at capacity and (b) weak residual/vol-scaled momentum. The next
measurable step is NOT another single-family batch.

WHAT TO DO — cp19 (default; do not ask):
1. Universe with a liquidity floor (pipeline, not a trial): add a panel-side admission
   predicate "21-day mean dollar ADV >= $50M as-of t" (the ADV series already exists as the
   ancillary kEquityDollarAdvDsl). This changes the universe, so it is a NEW cell set
   (equity_scorecard19_{ctx,base,ic}); re-run panel + baseline + ic phases for the 13 cells
   (Release; ~1 h budget with --parallel 4 including panel phases — profile one cell first).
   Pre-register that the SAME 18 configurations re-measured on the floored universe are
   RE-MEASUREMENTS (N unchanged) and say so in the ledger rule text; only a new family adds
   to N. Report the full scorecard on the floored universe: if the illiquidity families
   vanish, that is the finding; if residual momentum clears bar 1 on the floored universe,
   it is NOT a candidate until re-declared on a held-out span (see 3).
2. First multi-signal configuration (declared, N += 20): equal-weight rank blend of
   residual_momentum_252 and momentum_volscaled_252 ("mom_resid_vs_blend"), using the blend
   path that already produces blend_equal (combo.bin `alpha` column) or a DSL expression if
   rank/zscore ops suffice — one pass, same IC engine. This is the only new trial in cp19.
3. Held-out discipline: the cells are 2013–2019 (training); validation 2020–2022 is still
   unread. Any family that clears R16-8 on training must then be declared ONCE on the
   validation span (2020–2022, same 13-cell recipe by year) and clear bar 1 there before
   Stage 4. Build the runner support (year list + eval starts for 2020–2022) but do NOT run it
   unless a training-span candidate exists.
4. Fundamentals batch (explicitly): value/quality/accruals need atx-db (tblFundamentals or
   equivalent). Spend at most one delegated read to establish whether atx-db exposes a
   point-in-time fundamentals table for the 2013–2019 universe; report yes/no with the table
   and as-of column names. Do not build the join in cp19.
5. Throughput: sweep is ~4 min ic-only; panel+baseline re-run is the new cost — measure it.
   Scorecard script ~5.5 min for 11 signals; fine.

DEFERRED unless it blocks a number: hole remediation attempt 2; as-of membership predicate
beyond the liquidity floor; stdlib oracle suite; receipt writer; design/end reviews; any DSR
variance model other than the realised cross-configuration dispersion (R18-6).

GUARD-RAILS THAT STAY: every configuration counted toward N before it runs (mirrored signs
included); no post-hoc sign flips (R17-7); no further illiquidity variants (R18-5); sealed
2023–2025 never read; validation 2020–2022 read only under item 3; no live trading; synthetic
fixtures are never evidence; caveats beside numbers; PCS 2013-05-01 stays rejected; frozen
design files unchanged; kMaxIcInstruments 4096 via per-year contexts (2017 t3000 NOT FIT
unless the liquidity floor brings it under the cap — then it becomes a 14th cell, say so);
cp16 baseline dirs are not book baselines; never time anything in build-equity (Debug);
ledger files are per checkpoint and immutable; receipt and log tags are checkpoint-specific;
--atx-impl is an absolute path.

PIPELINE POINTERS: family list atx-impl/src/equity_baseline_views.hpp (kEquityFamilyDsl /
kEquityFamilySignalNames; kEquityFamilyRetainedCount marks re-measured leading entries);
per-checkpoint constants in atx-impl/src/stage_equity_ic.cpp (kCheckpoint, kTrialIdPrefix,
kTrialCountRule; kTrialCountDeclared derives) + the StageEquityIc test literals; runner
build-equity/audits/iteration16_run_cells.py (--phases, --ic-name-prefix, --ic-stamp,
--parallel, --log-tag, --receipt-tag, absolute --atx-impl, Release exe
build-equity-rel/bin/atx-impl.exe); scorecard build-equity/audits/iteration16_equity_scorecard.py
(--declared-n, --n-note, --title; capacity.csv from quantile_spread.csv mean_dollar_adv).

END WITH: the scorecard for whatever was measured, "Rulings I made", a short status file, and a
v8 goal prompt in this shape. Preserve your context window.
