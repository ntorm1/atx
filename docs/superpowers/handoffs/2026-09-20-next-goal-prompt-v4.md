Goal prompt for the next parent agent (paste into /goal) — v4, post-scorecard

Read C:\atx\.worktrees\equity-platform\docs\superpowers\handoffs\2026-09-20-equity-platform-status-cp16.md
first, then the v4 handoff docs/superpowers/handoffs/2026-09-20-equity-platform-parent-goal-v4.md
in full (§1–§7), the v1 handoff's "Critical workspace rules" and "Integration constraints", and
.superpowers/sdd/equity-platform-parent-goal/progress.md from "Session start (v3 goal" to the end.
Exact stop point: checkpoint 16 COMPLETE on the LEAN path (user decision R16-13): scorecard
published (atx-engine/reviews/2026-09-20-equity-alpha-scorecard-cp16.md, SHA b36ab30b…), design
a834840…, addendum 7173cc33…, sidecar ledger 26 lines, canonical ledger 4 lines untouched, docs
done, v4 handoff written; nothing in flight; no commit authorized. Preserve all work and evidence;
version failed attempts, never overwrite; never mutate a receipt or a frozen design — add an
addendum.

THE MEASURED FACT that now drives everything: the momentum family (momentum_252, momentum_126,
blend_equal; cp14 configurations unchanged) has pooled 2013–2019 net decile-spread Sharpe ≈ 0
at every horizon on both top-1000 and top-3000 (h=21: −0.26 … +0.08, every CI straddles 0), and
GROSS ≈ 0 too — the failure is the signal, not the cost model. Per-year: 2013–2015 and 2019
positive, 2016 and 2018 negative, 2017 ≈ 0. All pre-registered bars FAIL; NO CANDIDATE.
Checkpoint 14's single-year 2013 numbers are NOT alpha evidence and must not be quoted as such.

MISSION (unchanged): real engine + pipeline work, atx-impl driving atx-engine, robust tradeable
alphas combined into one mega-alpha portfolio; robust is measured, never asserted.

DECISION FOR THE USER (ask ONCE with three options, then execute the chosen one; do not build
all three):
(A) Stage 3 — new signal families, same pre-registered machinery. Cheapest next number.
    Candidates in order of literature support and data availability on the existing panels
    (close, volume, OHLC, ADV): short-term reversal (1-month, 5-day), 52-week-high proximity,
    idiosyncratic-vol / low-vol, Amihud illiquidity, volume-shock; momentum with the last
    month skipped is ALREADY what cp14's ts_mean(delay(close,21)/delay(close,{252,126}))
    computes — do not re-run it. Each family = ONE new baseline-views signal column + the same
    equity-ic run over the 13 cells (≈ 45 min machine per family) + the same scorecard;
    pre-register the family list and N BEFORE the first run (each family adds 5 horizons × 2
    variants × 2 restrictions = 20 configurations to N; declare them in the sidecar ledger
    with the real count, not the binary's constant — implement the cp16 ledger mode first if
    you add signals). Bars identical to R16-8.
(B) Data quality — hole remediation attempt 2 (authorised by R16-26): tickerhistory-qa-v2
    (accept rows with valid close/volume, flag missing OHL) for the 19 corrupted pre-holiday
    sessions 2016-01-15..2018-02-16, re-ingest the span, re-run the universe as attempt 2, re-run
    the 13 cells as attempt 2, compare 2016/2017/2018 rows only. ≈ 2 h machine. Only worth it
    if the user believes 2016–2018 momentum is a data artefact; the gross ≈ 0 pooled result
    says the other four years already fail to carry the family.
(C) Retire the lean shortcuts before anything is called a candidate: as-of membership
    predicate in equity-ic, cp16 ledger mode (checkpoint 16, non-trial purpose, real declared
    counts), stdlib oracle suite for the scorecard, receipt writer, design/end reviews. No new
    number; ≈ 4–6 h.
Recommend (A) unless the user says otherwise; (C)'s ledger mode is a prerequisite of (A).

HARD RULES (unchanged). Everything pre-registered in a hash-chained ledger before it runs;
every declared trial counts toward deflated Sharpe; sealed 2023–2025 never read (validation
begins 2020-01-01; code-level seal stays); no live trading; synthetic fixtures never evidence;
every caveat printed beside every number; PCS 2013-05-01 admission stays rejected; frozen
design SHAs cp12 0a964aeb…, cp14 888c726b…, cp15 4810fda2…, cp16 a834840… unchanged;
kMaxIcInstruments 4096 respected via per-year contexts (2017 top-3000 stays NOT FIT unless
remediated); the cp16 baseline dirs (status complete-signals-only) are NOT book baselines;
touch StageRunSyntheticSmoke only if it blocks, ledger the diagnosis first.

PROCESS. Subagent-driven Research → (design if new pre-registration) → Implement → Measure →
Report; preserve your context (delegate reads; the ledger is memory); fresh edit-only
implementers; native configure/check/build/test/run parent-only via scripts/atx-build.ps1
(equity-dev, clang-cl 18, /W4 /WX, CCACHE_DISABLE=1, ≤ 3 wrapper calls per PowerShell process,
absolute ctest output paths); detach long runs with Start-Process and infer progress from
output dirs (python stdout is block-buffered); runner --dry-run before every real run; Bash
heredocs for python edits (PowerShell here-strings do not pipe into `python -`); Edit/Write
tools for edits; every ruling, measurement, failed attempt and deviation into progress.md
before the next dispatch. Reuse: iteration16_ingest_span.py, iteration16_run_cells.py
(--only-year/--only-cut/--resume/--receipt-tag=-x/--attempt), iteration16_equity_scorecard.py
(--cells/--anchor/--out/--self-test). End with a scorecard for whatever was measured, "Rulings
I made", a status file and a v5 handoff in the v4 shape. Use sub-agent-driven development and
preserve your context window.
