Goal prompt for the next parent agent (paste into /goal) — v8, post-cp19, alpha-first

PROCESS (unchanged from v5–v7): no hashing/TDD/format ceremony; alpha generation and pipeline
building only; a short progress.md entry per ruling, measurement or failed attempt is the memory.

CONTEXT TO LOAD (delegate the reads; keep only conclusions): read
docs/superpowers/handoffs/2026-09-21-equity-platform-status-cp19.md (whole), then
.superpowers/sdd/equity-platform-parent-goal/progress.md from "Session continues (goal hook" to
the end, then the v5 prompt docs/superpowers/handoffs/2026-09-21-next-goal-prompt-v5.md for the
standing guard-rails and the "DROPPED" list. Worktree C:\atx\.worktrees\equity-platform. Stop
point: checkpoint 19 COMPLETE. Scorecard atx-engine/reviews/2026-09-21-equity-alpha-scorecard-cp19.md.
Frozen design files unchanged (addendum only). No commit authorized. Preserve work; version
failed attempts.

THE MEASURED FACTS: on the liquidity-floored universe (21-day mean dollar ADV >= $50M, ~730–880
names, both cuts collapse to the same set) NOTHING clears bar 1 (N = 350, SR0_ann 1.95). The
illiquidity families invert (low_dollar_volume_21 −1.13 [−1.83, −0.51] 7/7): every bar-1 pass of
cp17/cp18 was the sub-$50M tail. Plain momentum_252 is the best tradeable family (+0.47 [−0.21,
+1.17] 6/7, gross +0.68, long-decile ADV $218M, turnover 0.94, +0.32 after a 5 bps surcharge)
and is not significant at 74 pooled h=21 observations. 18 price/volume single-signal
configurations are exhausted on this universe (R19-6). atx-db has the point-in-time fundamentals
SCHEMA but ~18 % ingestion and empty standardized tables (R19-2).

WHAT TO DO — cp20 (default; do not ask). Two lanes, run the data lane first because it gates
the only untested alpha source:
1. DATA LANE (atx-db, delegated to a sub-agent with its own context): make fundamentals usable
   for the floored 2013–2019 universe. Establish from atx-db/docs/TIER1_ACTIVATION_STATUS.md and
   the atx-db CLI how the SEC companyfacts archive load resumes (it is resumable; the last loaded
   CIK is recorded) and what the standardization stage is called; run the load for the CIKs of
   the floored universe FIRST (map security_id -> cik from sec_submissions / the identity tables;
   ~900 issuers, not 20,390), then the standardization stage for those CIKs. Report coverage as a
   number: fraction of floored-universe (year, security) pairs with a PIT book value and a PIT
   trailing-12-month net income available_at <= the first session of the year. Budget: measure
   the per-CIK load time on 20 CIKs before launching the rest; if the full ~900-CIK load exceeds
   ~2 h, run it in the background under a heartbeat and proceed with lane 2. Nothing in this lane
   reads prices after 2019-12-31 or fundamentals filed after that date for the training cells.
2. ALPHA LANE, pre-registered BEFORE lane 1 finishes (one progress.md line; the ledger genesis
   line when the binary runs): batch 3 = fundamentals families on the floored universe, each a
   new trial (N += 20 each): book_to_market (PIT book value / market cap, market cap =
   raw_close × shares_outstanding if shares exist in the DB, else book / dollar-ADV-free proxy
   is NOT allowed — skip and say so), earnings_yield (TTM net income / market cap),
   gross_profitability (Novy-Marx: (revenue − COGS) / total assets), asset_growth (Cooper–Gulen–
   Schill, sign −), accruals (Sloan, sign −), roe. Signs are the literature signs. This needs a
   panel-side fundamentals field join (atx-impl panel phase: a new context field per item with
   as-of forward-fill from available_at) — build it as ONE generic "as-of scalar field" ingest,
   not six; the cp16 contexts are then REBUILT once with the extra fields (new ctx dirs
   equity_scorecard20_ctx_*), re-run baseline (floored) + ic. The 18 existing configurations are
   re-measurements again (N unchanged). If lane 1 coverage < 80 %, run batch 3 anyway on the
   covered names and print coverage beside every number as a caveat.
3. POWER ADDENDUM (report only, never the bar): add to the scorecard script an
   "overlapping-offsets" column at h=21 (mean of the 21 offset Sharpes, Jegadeesh–Titman style;
   the script already prints offset_min/max) beside the offset-0 pooled Sharpe. Bars stay on
   offset 0 (R16-7). One paragraph in the scorecard saying what it would change.
4. DESIGN ADDENDUM (one file, addendum only): the cut dimension collapsed under the floor
   (R19-5). Propose the cp20 cell set as ONE floored universe per year plus a cap-based split
   (top-500 vs 501–1500 by dollar ADV rank) so "both cuts" is meaningful again; do NOT change
   R16-8 text; the parent after you decides.
5. Held-out 2020–2022: untouched. No candidate exists; nothing is read.

DEFERRED unless it blocks a number: hole remediation attempt 2 (note: under the floor each
corrupted session costs 21 sessions of breadth in 2016–2018; if batch 3 needs those years, the
remediation moves up); as-of membership predicate beyond the floor; stdlib oracle suite; receipt
writer; design/end reviews.

GUARD-RAILS THAT STAY: every configuration counted toward N before it runs; no post-hoc sign
flips (R17-7); no further illiquidity variants (R18-5); the floor is not tuned (R19-1); sealed
2023–2025 never read; validation 2020–2022 never read; no live trading; synthetic fixtures are
never evidence; caveats beside numbers; PCS 2013-05-01 stays rejected; frozen design files
unchanged; kMaxIcInstruments 4096 (the floored universe is far under it; 2017 t3000 is FIT
under the floor only if its context is rebuilt — say so); never time anything in build-equity
(Debug); ledger files per checkpoint, immutable; receipt/log tags checkpoint-specific;
--atx-impl absolute; the IC stage takes the universe from the baseline recipe (attempt-1 trap).

PIPELINE POINTERS: family list atx-impl/src/equity_baseline_views.hpp (kEquityFamilyDsl /
kEquityFamilySignalNames, kEquityFamilyRetainedCount); per-checkpoint constants in
atx-impl/src/stage_equity_ic.cpp (kCheckpoint, kTrialIdPrefix, kTrialCountRule) + the
StageEquityIc test literals; floor CLI --min-dollar-adv/--dollar-adv-window (baseline only);
runner build-equity/audits/iteration16_run_cells.py (--phases, --base-name-prefix/--base-stamp,
--ic-name-prefix/--ic-stamp, --min-dollar-adv, --parallel, --log-tag, --receipt-tag, absolute
--atx-impl build-equity-rel/bin/atx-impl.exe); scorecard
build-equity/audits/iteration16_equity_scorecard.py (--declared-n, --n-note, --title;
capacity.csv). Sweep ≈ 3 min ic-only; panel rebuild cost unknown — profile one cell first.

END WITH: the scorecard for whatever was measured, "Rulings I made", a short status file, and a
v9 goal prompt in this shape. Preserve your context window.
