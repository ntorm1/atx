Goal prompt for the next parent agent (paste into /goal) — v5, post-scorecard, alpha-first

CHANGE FROM v4 (user note, 2026-09-21): reduce reliance on file hashing, test-driven
development, word-count / brief-format checking, and exact-precision ceremony. Focus on alpha
generation and pipeline building. v4 stays on disk as the record of the old process; this file
supersedes it for process only — the measured facts, mission and scientific guard-rails are
unchanged.

CONTEXT TO LOAD (delegate the reads; keep only conclusions). Read
docs/superpowers/handoffs/2026-09-20-equity-platform-status-cp16.md, then the v4 handoff
docs/superpowers/handoffs/2026-09-20-equity-platform-parent-goal-v4.md (§1–§7), the v1
handoff's "Critical workspace rules" and "Integration constraints", and
.superpowers/sdd/equity-platform-parent-goal/progress.md from "Session start (v3 goal" to the
end. Worktree: C:\atx\.worktrees\equity-platform. Stop point: checkpoint 16 COMPLETE on the lean
path. Scorecard at atx-engine/reviews/2026-09-20-equity-alpha-scorecard-cp16.md; cp16 design
and addendum are frozen (read them, do not edit them — add an addendum file if something must
change). Nothing in flight; no commit authorized. Preserve work and evidence; version failed
attempts, never overwrite.

THE MEASURED FACT that drives everything: the momentum family (momentum_252, momentum_126,
blend_equal; cp14 configurations) has pooled 2013–2019 net decile-spread Sharpe ≈ 0 at every
horizon on both top-1000 and top-3000 (h=21: −0.26 … +0.08, every CI straddles 0), and GROSS
≈ 0 too — the signal fails, not the cost model. Per-year: 2013–2015 and 2019 positive, 2016 and
2018 negative, 2017 ≈ 0. NO CANDIDATE. Checkpoint 14's single-year 2013 numbers are not alpha
evidence.

MISSION (unchanged): real engine + pipeline work, atx-impl driving atx-engine, robust tradeable
alphas combined into one mega-alpha portfolio. Robust is measured, never asserted. The output
of this session is NEW MEASURED ALPHA NUMBERS and the pipeline changes needed to produce them
— not reviews, receipts, hashes or process artefacts.

WHAT TO DO — Stage 3, new signal families (default; do not ask unless something blocks it):
1. Pick the first batch of families from the existing panels (close, volume, OHLC, ADV), in
   this order of literature support: short-term reversal (1-month and 5-day), 52-week-high
   proximity, idiosyncratic-vol / low-vol, Amihud illiquidity, volume-shock. Momentum with the
   last month skipped is already what cp14's ts_mean(delay(close,21)/delay(close,{252,126}))
   computes — do not re-run it. Run as many families per sweep as the pipeline allows; batch
   them into ONE equity-ic run over the 13 cells rather than one run per family where the
   binary permits.
2. Pipeline work required first (this is the real engineering, do it well, keep it small):
   make the baseline-views signal set data-driven or at least easily extended — today
   kEquityBaselineDsl / kEquityBaselineSignalNames, kSignalNames (3), kVariantNames (2),
   kFields (3), the 256-row warmup and the 2-signal readiness check are hard-coded across
   equity_baseline_views.cpp and the IC stage. Extend them so adding a family is one DSL line
   + one name, warmup derives from the program's required lookback, and the IC stage's ledger
   entry carries checkpoint 17 and the REAL configuration count (families × 5 horizons × 2
   variants × 2 restrictions) instead of the constant 30. Compile-check with
   scripts/atx-build.ps1 check <file>, build the owning test target, run the existing suite
   once so nothing regresses. Write new tests only where they catch a pipeline break you
   actually hit; no TDD ritual, no coverage targets.
3. Pre-register, lightly: before the first real run, append one line to the sidecar ledger
   naming the families, the DSL for each, the horizons/variants/restrictions and the resulting
   N added to deflated-Sharpe accounting. That line is the pre-registration. No hash audit, no
   receipt writer, no design review round.
4. Measure: runner --dry-run, then the 13-cell sweep (per-year contexts; 2017 top-3000 stays
   NOT FIT), detached with Start-Process, progress inferred from output dirs. Then the
   scorecard with the same bars as R16-8 (pooled 2013–2019 net and gross decile-spread Sharpe
   with CI, per-year sign table, deflated Sharpe using the cumulative declared N). Report every
   family, pass or fail, with caveats beside numbers.
5. If any family clears the bars: that is the first candidate. Next step becomes Stage 4
   (combine candidates into the mega-alpha portfolio through the risk/portfolio path in
   atx-engine, measured the same way). If none clears: next batch of families, same loop.
6. Backtest performance and throughput — state of the art, when it is on the critical path.
   The loop above is bounded by machine time (≈ 45 min per family-sweep at cp16). Before the
   first Stage 3 sweep, profile one cell (equity-universe → baseline-views → equity-ic) and
   record wall-clock, peak RSS and where the time goes. If the sweep is the bottleneck for
   iterating families, fix the pipeline, not the plan: evaluate all families in one pass over
   the panel (one DSL program, one LoadField, N signal columns); keep hot loops
   cache-friendly and vectorisable (SoA panels, contiguous per-date rows, no per-cell
   allocation, no NaN-mask recomputation per signal); parallelise across years/cuts/cells
   (independent per-year contexts already exist — run them concurrently, bounded by RAM under
   the 4096-instrument cap); avoid re-ingesting or re-materialising the universe when only
   signals change (cache the point-in-time panel per year/cut on disk and reuse it); make the
   IC/decile-spread and cost model O(dates × instruments) with no quadratic re-sorts. Target:
   a full multi-family 13-cell sweep in minutes, not hours, with identical numbers to the
   serial path on the cp16 configurations (verify once by re-running momentum_252 and
   diffing the scorecard rows). Measure before and after; report the speedup in the status
   file. Do not spend time here if the sweep is already fast enough for the batch you plan.

DEFERRED unless it blocks a number: hole remediation attempt 2 (2016-01-15..2018-02-16, qa-v2
rule; only if a family's failure is concentrated in those years AND the other years carry it);
as-of membership predicate in equity-ic; stdlib oracle suite for the scorecard; receipt
writer; design/end review rounds. Do the minimum of these that a measurement actually needs,
and say which one and why.

GUARD-RAILS THAT STAY (these protect the alpha numbers, not the process): every configuration
counted toward deflated Sharpe before it runs; sealed 2023–2025 never read (validation begins
2020-01-01; code-level seal stays); no live trading; synthetic fixtures are never evidence;
caveats printed beside numbers; PCS 2013-05-01 admission stays rejected; frozen design files
unchanged (addendum only); kMaxIcInstruments 4096 respected via per-year contexts; the cp16
baseline dirs (complete-signals-only) are not book baselines; touch StageRunSyntheticSmoke only
if it blocks, note the diagnosis in progress.md first.

DROPPED FROM THE PROCESS: quoting or verifying SHAs of docs/receipts; hash-chain audits of the
ledger; word-count or format checks on briefs and reports; verbatim-copy rituals; per-task
design and end reviews; TDD as a mandate; exact-count ceremonies beyond the single N line
above. A short progress.md entry per ruling, measurement or failed attempt is still required —
that file is the memory.

PROCESS. Subagent-driven: one research read-out → implement (fresh edit-only implementers, one
reviewer pass on the C++ diff for correctness only) → measure → report. Native
configure/check/build/test/run parent-only via scripts/atx-build.ps1 (equity-dev, clang-cl 18,
/W4 /WX, CCACHE_DISABLE=1, ≤ 3 wrapper calls per PowerShell process, absolute ctest output
paths). Bash heredocs for python edits; Edit/Write tools for file edits, not pathlib scripts.
Reuse iteration16_ingest_span.py, iteration16_run_cells.py
(--only-year/--only-cut/--resume/--receipt-tag=-x/--attempt) and
iteration16_equity_scorecard.py (--cells/--anchor/--out/--self-test); extend them for multiple
families rather than writing new drivers. End with: the scorecard for whatever was measured,
"Rulings I made", a short status file, and a v6 goal prompt in this shape. Preserve your
context window.
