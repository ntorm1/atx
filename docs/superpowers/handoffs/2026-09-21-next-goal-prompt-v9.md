Goal prompt for the next parent agent (paste into /goal) — v9, cp20 in flight, alpha-first

PROCESS (unchanged from v5–v8): no hashing/TDD/format ceremony; alpha generation and pipeline
building only; a short progress.md entry per ruling, measurement or failed attempt is the memory.
Sub-agent driven: delegate reads and bounded edits; keep only conclusions in your own context.
Web research before guessing on any construction or data-encoding question.

CONTEXT TO LOAD (delegate the reads; keep only conclusions): read
docs/superpowers/handoffs/2026-09-21-equity-platform-status-cp20-inflight.md (whole), then
docs/superpowers/handoffs/2026-09-21-equity-platform-status-cp19.md (numbers table + rulings),
then .superpowers/sdd/equity-platform-parent-goal/progress.md from "PRE-REGISTRATION cp20" to
the end, then the v5 prompt docs/superpowers/handoffs/2026-09-21-next-goal-prompt-v5.md for the
standing guard-rails and the "DROPPED" list. Worktree C:\atx\.worktrees\equity-platform. Stop
point: cp20 pre-registered + built, ONE cell run, one test expectation stale. Frozen design files
unchanged (addendum only). No commit authorized. Preserve work; version failed attempts.

THE MEASURED FACTS: 18 price/volume configurations on the liquidity-floored universe clear
nothing (cp19; momentum_252 +0.47 [−0.21, +1.17] is the best); the options-implied-vol families
of cp20 have ~98 % coverage on real data; the three earnFlag families have ZERO finite cells on
the one cell run (earnFlag is almost certainly NaN on non-announcement sessions). Cumulative
declared N = 350 (+140 for cp20 = 490).

WHAT TO DO — finish cp20, then decide (default; do not ask):
1. Fix the stale test: atx-impl/tests/equity_baseline_views_test.cpp:298 expects warmup 252U;
   the derived value is 256U (mom252_sector_neutral = ts_mean(.., 5) over delay(close, 252)).
   Change the literal and comment; rebuild atx-impl-tests (equity-dev); the four suites must be
   60/60 (one pre-existing skip).
2. earnFlag encoding (one delegated read, then one amendment line BEFORE any run): determine
   from atx-engine/include/atx/engine/data/{history_panel,orats_history}.hpp and the panel stage
   how earnFlag is stored on non-event sessions (NaN vs 0). If NaN: add ONE NaN-safe form —
   prefer an existing registry op (grep atx-engine/src/alpha/registry.cpp for ts_backfill,
   ts_count_nans, sign, or a comparison); if none maps NaN→0, add a unary `nan_to_zero` op in
   the registry (+ its ts/cs kernel, + one oracle/unit test) and rewrite the three DSLs as
   `ts_sum(delay(nret, 1) * min(1, nan_to_zero(earnFlag) + delay(nan_to_zero(earnFlag), 1) +
   delay(nan_to_zero(earnFlag), 2)), W)` and `ts_sum(delay(nan_to_zero(earnFlag), 178), 21)`.
   Log ruling R20-4 (encoding + rewrite; same 7 configurations, N unchanged). The attempt-1 cell
   (equity_scorecard20_ic_2014_t1000_20260921, 2 ledger lines) stays on disk as the failed
   attempt; re-run everything under --ic-name-prefix equity_scorecard20r2 with a fresh ledger
   atx-engine/reviews/trial-ledger-cp20-batch3a-r2.jsonl.
3. Sweep (ic-only over the cp19 floored base dirs; Release; absolute --atx-impl; --parallel 4):
   python build-equity/audits/iteration16_run_cells.py --phases ic --base-name-prefix
   equity_scorecard19 --base-stamp 20260921 --ic-name-prefix equity_scorecard20r2 --ic-stamp
   20260921 --min-dollar-adv 50000000 --ledger <fresh ledger> --receipt-tag=-cp20-full-r2
   --log-tag=-cp20-full-r2 --atx-impl C:/atx/.worktrees/equity-platform/build-equity-rel/bin/atx-impl.exe
   --parallel 4 (--dry-run first). Identity check: the 18 retained signals' point series must
   equal cp19r2 (diff script pattern in progress.md "cp18 IDENTITY"; skip CI columns, R18-3).
   Report per-family coverage from request.json beside the numbers.
4. Scorecard: iteration16_equity_scorecard.py --glob "C:/atx/data/equity_scorecard20r2_ic_*"
   --out C:/atx/data/equity_scorecard20_scorecard_<stamp> --cells-receipt <receipt>
   --declared-n 490 --n-note "<how N was counted>" --title "cp20 batch 3a ...". Archive
   scorecard.md to atx-engine/reviews/2026-09-21-equity-alpha-scorecard-cp20.md. Bars R16-8
   unchanged; caveat that the two cuts are the same floored set (R19-5).
5. If a family clears R16-8 on both cuts → Stage 4 (risk/portfolio path in atx-engine driven by
   atx-impl, same measurement) and ONLY THEN a single declared read of validation 2020–2022
   (runner --allow-validation exists; the binaries' 2020-01-01 seal needs a declared ruling and a
   code change — do not lift it for a non-candidate). If none: cp21 = fundamentals batch 3b via
   atx-db, gated on the warehouse lock clearing (check for a python process holding
   C:\atx\atx-db\data\warehouse.duckdb; the user runs loads there — never kill it, never write
   while it runs). The v8 prompt §1–§2 has the data-lane plan (scope to ~900 floored issuers,
   standardization ladder is global, PIT via available_at, dedup earliest filed).

DEFERRED unless it blocks a number: hole remediation attempt 2; as-of membership beyond the
floor; stdlib oracle suite; receipt writer; design/end reviews; overlapping-offsets power
addendum (v8 §3, report-only); cell-set design addendum for the collapsed cut (v8 §4).

GUARD-RAILS THAT STAY: every configuration counted toward N before it runs; no post-hoc sign
flips (R17-7); no further illiquidity variants (R18-5); the floor is not tuned (R19-1); sealed
2023–2025 never read; validation 2020–2022 never read; no live trading; synthetic fixtures are
never evidence; caveats beside numbers; PCS 2013-05-01 stays rejected; frozen design files
unchanged; kMaxIcInstruments 4096 via per-year contexts; never time anything in build-equity
(Debug); ledger files per checkpoint, immutable; receipt/log tags checkpoint-specific;
--atx-impl absolute; the IC stage takes the universe from the baseline recipe; atx-db untouched
while another process holds the lock (R20-1).

PIPELINE POINTERS: family list atx-impl/src/equity_baseline_views.hpp (kEquityFamilyDsl 22,
kEquityFamilyRetainedCount 15, kOptionalFields in the .cpp); constants in
atx-impl/src/stage_equity_ic.cpp (kCheckpoint 20, kTrialIdPrefix, kTrialCountRule) + the
StageEquityIc test literals (already 20); runner and scorecard flags as in v8. Build/test only
via scripts/atx-build.ps1 (-Preset equity-dev for check/tests, -Preset equity-rel build
atx-impl for the sweep binary; quote the -Ctest -R regex inside powershell -Command).

END WITH: the scorecard for whatever was measured, "Rulings I made", a short status file, and a
v10 goal prompt in this shape. Preserve your context window.
