Goal prompt for the next parent agent (paste into /goal) — v10, cp20 complete, cp21 gated on the warehouse lock

PROCESS (unchanged from v5–v9): no hashing/TDD/format ceremony; alpha generation and pipeline
building only; one short progress.md line per ruling, measurement or failed attempt is the memory.
Sub-agent driven (opus): delegate reads and bounded edits; keep only conclusions in your own context.
Web research before guessing on any construction or data-encoding question.

CONTEXT TO LOAD (delegate the reads; keep only conclusions): read
docs/superpowers/handoffs/2026-09-22-equity-platform-status-cp20.md (whole), then
.superpowers/sdd/equity-platform-parent-goal/progress.md L182–L186 (R20-4, SWEEP r2, SCORECARD, R20-5,
DECISION), then the v5 prompt docs/superpowers/handoffs/2026-09-21-next-goal-prompt-v5.md for the
guard-rails and the DROPPED list, then the v8 prompt §1–§2 for the data-lane plan. Worktree
C:\atx\.worktrees\equity-platform. Stop point: cp20 complete (13 cells, scorecard archived, N = 490).
No commit authorized. Preserve work; version failed attempts.

THE MEASURED FACTS: 25 signals on the liquidity-floored universe clear nothing; best net h=21 stays
momentum_252 +0.47 [−0.21, +1.17] (cp19). cp20: mom252_sector_neutral +0.37/+0.44 (≈ momentum_252);
d_iv_21 −1.48 [−2.27, −0.79] 7/7 and iv_rv_log −1.00 [−1.84, −0.24] are significant with the WRONG
pre-registered sign → rejected under R17-7, not carried (the + signs were literature-grounded; the
ORATS ATM-centre IV contradicts). Engine defect R20-5: ±inf poisons the online ts_sum/ts_mean kernel
permanently (amihud_21 exposed via division by raw_close*volume on zero-volume days).

WHAT TO DO (default; do not ask):
1. Lock check first (Get-CimInstance Win32_Process, Name python.exe, CommandLine matching
   warehouse_activate or warehouse.duckdb). If a loader holds C:\atx\atx-db\data\warehouse.duckdb
   → atx-db is untouchable (R20-1); never kill it; go to step 2. If clear → cp21 = fundamentals batch 3b
   exactly per v8 §1–§2: scope ~900 floored CIKs, global standardization ladder, PIT via available_at,
   dedup earliest filed; time 20 CIKs first (> 2 h → background). Pre-register families
   (book_to_market +, earnings_yield +, gross_profitability +, asset_growth −, accruals −, roe +),
   N += 20 each, BEFORE any run; fresh ledger atx-engine/reviews/trial-ledger-cp21-batch3b.jsonl;
   --ic-name-prefix equity_scorecard21; new contexts equity_scorecard21_ctx_* via one generic as-of
   scalar field ingest; kCheckpoint → 21 and the StageEquityIc test literals; identity of the 18
   retained signals against cp20r2 (skip CI columns, R18-3; tolerate last-ulp).
2. If the lock is held: do NOT start batch 3c on your own (new families → N → user ruling; the status
   doc lists it as the unblocked alternative). Instead, under a declared ruling R21-1, fix R20-5: treat
   non-finite like NaN in ts_online_sum_family (atx-engine/include/atx/engine/alpha/ts_ops.hpp:372–406),
   keep the batch oracle bit-for-bit consistent, add one unit test (an inf enters and leaves a
   21-window; the column recovers; NaN count semantics unchanged), rebuild equity-dev atx-impl-tests +
   the engine tests that cover ts ops, then equity-rel atx-impl. Re-run ONLY cell 2017_t1000 under
   --ic-name-prefix equity_identity21 --ic-stamp 20260922 with a scratch ledger and confirm amihud_21
   n_used equals cp19r2 on date_index 192–223 while the other 17 retained signals stay identical. Log the
   re-measured amihud_21 h=21 line beside the cp19 record; N unchanged (re-measurement).
3. Promote the scratch tooling into the repo so it stops being rewritten every checkpoint:
   build-equity/audits/iteration16_identity_check.py (18 retained signals; ic.csv all columns;
   quantile_spread.csv without *_lo/*_hi; signal_autocorr.csv; numeric tol rel 1e-9 / abs 1e-12;
   report byte-identical count beside; PASS/FAIL + JSON) and iteration16_family_coverage.py
   (families.finite_admitted_cells from request.json, denominator = max over families).

DEFERRED unless it blocks a number: hole remediation attempt 2; as-of membership beyond the floor;
stdlib oracle suite; receipt writer; design/end reviews; overlapping-offsets power addendum (v8 §3);
cell-set design addendum for the collapsed cut (v8 §4).

GUARD-RAILS THAT STAY: every configuration counted toward N before it runs; no post-hoc sign flips
(R17-7); no further illiquidity variants (R18-5); the floor is not tuned (R19-1); sealed 2023–2025 never
read; validation 2020–2022 never read (runner --allow-validation exists; the 2020-01-01 seal needs a
declared ruling and a code change — never for a non-candidate); no live trading; synthetic fixtures
are never evidence; caveats beside numbers; PCS 2013-05-01 stays rejected; frozen design files
unchanged (addenda only); kMaxIcInstruments 4096 via per-year contexts (2017_t3000 NOT FIT); never
time anything in build-equity (Debug); ledger files per checkpoint, immutable; receipt/log tags
checkpoint-specific; --atx-impl absolute; the IC stage takes the universe from the baseline recipe;
atx-db untouched while another process holds the lock (R20-1).

PIPELINE POINTERS: families atx-impl/src/equity_baseline_views.hpp (kEquityFamilyDsl 22,
kEquityFamilyRetainedCount 15; kOptionalFields in the .cpp already borrows market_cap, sector,
earnFlag, nEarnCnt_5d, atmCenI_21d, atmCenI_126d, high, low, open); constants in
atx-impl/src/stage_equity_ic.cpp (kCheckpoint, kTrialIdPrefix, kTrialCountRule) + StageEquityIc test
literals; runner build-equity/audits/iteration16_run_cells.py (--phases ic --base-name-prefix
equity_scorecard19 --base-stamp 20260921 --ic-name-prefix <prefix> --ic-stamp <stamp>
--min-dollar-adv 50000000 --ledger <file> --receipt-tag=<tag> --log-tag=<tag> --atx-impl
C:/atx/.worktrees/equity-platform/build-equity-rel/bin/atx-impl.exe --parallel 4; --dry-run first;
13 cells ≈ 2 min); scorecard build-equity/audits/iteration16_equity_scorecard.py (--glob --out
--cells-receipt --declared-n --n-note --title; ≈ 15 min). Build/test only via scripts/atx-build.ps1:
powershell -NoProfile -File scripts\atx-build.ps1 -Preset equity-dev build atx-impl-tests;
powershell -NoProfile -Command "scripts\atx-build.ps1 -Preset equity-dev -Ctest -R 'StageEquityIc|EquityIc|TrialLedger|EquityBaseline'"
(60/60 + 1 pre-existing skip); powershell -NoProfile -File scripts\atx-build.ps1 -Preset equity-rel build atx-impl.
Preserve your context window. Use opus for sub-agent driven dev. Prioritize alpha generation; prune
with the IC stage before any book-level work.
