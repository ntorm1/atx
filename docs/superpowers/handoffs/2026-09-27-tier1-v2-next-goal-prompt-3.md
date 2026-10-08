# Goal prompt: resume atx-db tier-1 v2 (session 5 controller)

Paste the block between the rules as the next parent agent's `/goal`. Session 4 ended when the account hit its **weekly
API limit** (resets 2026-09-29 04:00 America/New_York). Nine of the ten agents that were running died mid-wrap-up. Most
of them never wrote their `## Stopped (session 4)` section, so the next controller reconciles each lane from `git`
and the files. The "State at handoff" section below the block is reference only.

---

/goal Continue building atx-db toward the user's goal: production-quality, large cross-sectional, deep-history characteristics that are statistically significant over a forward horizon N (1–12 months), for a downstream equity long/short book. Build the tools, infra and pipelines this needs. Use large subagent swarms working together. No test-driven development; go depth-first for high-quality production outputs.

The spec is docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md, with the s0–s5 and sx sprint files next to it. Its main product IS this goal: a graded characteristic library with 1–12 month evidence (ruling C-89). Read the index §2, §3, §6 and §9 first. Then read the "State at handoff" section of docs/superpowers/handoffs/2026-09-27-tier1-v2-next-goal-prompt-3.md. For older context, read docs/superpowers/handoffs/2026-09-26-tier1-v2-status-2.md (TL;DR and rulings only).

Resume, do not restart. The ledger is .superpowers/sdd/tier1-v2/progress.md, and it plus `git log` is the source of truth. Workspace (.superpowers/sdd/tier1-v2/):
- CONTEXT.md holds the binding rules that every agent reads first.
- REVIEW-CONTEXT.md is the reviewer lens.
- CARRY.md holds per-node must-fix items; an implementer treats its node's CARRY section as part of its brief.
- pkg.sh builds review packages: `bash .superpowers/sdd/tier1-v2/pkg.sh OUT [SHA...] [--tag NODE]`.
- Briefs live in task-<node>-brief.md and reports in task-<node>-report.md. Extract a missing brief with `bash C:/Users/natha/.claude/plugins/cache/claude-plugins-official/superpowers/6.4.1/skills/subagent-driven-development/scripts/task-brief <sprint file> <N>` and copy it to task-<node>-brief.md.
- Append every ledger line with Edit, immediately BEFORE the final line `- 11:50Z HOLD 0.12 fix round until rev-0-4 returns`.
- Every node with a `Task <node>: complete` line is DONE; never re-dispatch it. Complete: M0, 0.1–0.15, 1.1, 1.2, 1.7, 1.9, 1.10, 1.11, X.3.
- Rulings R-1..R-14 and C-1..C-93 are binding. Add new ones as C-94 onward, in the form `Ruling: <decision> — <why> — <cost if wrong>`.
- C-42 is the user's grant ("All access requested is granted, prioritize real data evaluation"):
  - The OPS agent may open and write the prod warehouse, only through run_memory_guarded.py.
  - U2, U3, U5, U6, U8 and U9 are granted: files only, SEC ≤ 5 req/s, exact UA.
  - U1, U4, U7 and U10 are not executed.

First actions, in order:

1. Reconcile.
   a. Commits and WIP:
      - Run `git log --oneline d4de104e..HEAD`. Expect 20734bce (X.5), 1e438796 (X.4) and 8aba655c (1.4), plus any other sessions' commits.
      - Run `git status --short -- atx-db`. Map every uncommitted file to its lane with the WIP table in the handoff. Keep all of it; never discard it.
   b. Processes and memory slots:
      - Confirm that no atx-db Python job is running; VS Code's own python processes are not ours.
      - Clear dead slots with `run_memory_guarded.py --reclaim-stale` (run it under its own guard, cap 0.2).
   c. Disk:
      - Free space was 110 GB at session start and 93 GB at the stop.
      - impl-1-3 (HEAVY holder) may have left a prod-warehouse drill copy and spill directories. It died before cleanup.
      - Find them in the session scratch dirs and atx-db/data (large recently written .duckdb / .wal files) and delete ONLY 1.3's scratch copy.
      - Confirm that prod warehouse.duckdb (12,277 MB) and warehouse.duckdb.pre-migrate.20260924-225023.bak (12,286.5 MB) are untouched.
      - HEAVY is free.
   d. X.4: check whether the stager-v2 re-stage (1e438796, accepted_utc) actually completed all 69 quarters (data/staging/fsds, manifest). The agent died mid-run.
   e. Ledger: add a reconcile line plus one ruling that pins the WIP-to-lane mapping.
2. Add one rule to CONTEXT.md, as ruling C-94: every agent refreshes a `## Progress (session N)` section in its report after each milestone (commit, measured run, stage finished). A sudden API or limit stop then loses at most one step. Session 4 lost 9 agents' state this way.
3. Dispatch fresh Opus implementers. The old ones are gone, so point each one at its brief, its report/review files and its WIP files, and tell it to start with `git diff` on that WIP. Keep live agents ≤ 12 (C-89), with file- and token-disjoint lanes. Priority order:
   a. **1.12 fix round 1/2.**
      - Hold PANEL and CAT.
      - WIP: spine.py, price_natives.py, research_price_wave.py, catalog.py, the catalog CSV, ANOMALY_CATALOG.md.
      - Inputs: task-1.12-review.md I1/I2 and rulings C-78, C-79, C-82; resume the rebuild from bar bucket 15.
      - Report the 4,500 lines/month target as NOT MET where it is not met.
      - Commit, write a `## Relay to 1.13` section, then a scoped re-review.
   b. **1.13**, as soon as 1.12 is approved. This is the first real graded features.
      - Commit the catalog first.
      - Register w1_price (432 cells) before any label join, then commit the anchor.
      - Grade 2013-01..2023-12, holdout sealed.
      - Write docs/research/WAVE1_PRICE_PROVISIONAL.md.
      - Its runner scripts/research_run_wave.py is untracked WIP; its `universe_basis` is hard-coded to the old value.
   c. **4.1 evaluation additions** (ruling C-92).
      - Hold EVAL and FEAT.
      - WIP: research/evaluation.py, factor_returns.py, features.py (a beta-neutral variant was in progress).
      - Use stub labels only until w1 is registered.
      - evaluation.py must never change during a 1.13 real run.
   d. **F.1 design**, redone from scratch (arch-f1 died at its first step; its full prompt is in the ledger line of 23:44Z and ruling C-90). Design and brief a fundamentals research fast path:
      - PIT item vintages from the FSDS v2 staging (accepted_utc, 2009q2 →) plus CF-R Parquet, into the research store.
      - CIK→line link from 3.3's rehearsal Parquet.
      - Then F.2: a fundamental characteristic wave graded under policy v4, covering issuance/net payout, cash-OP/GP, ROE_q/ChTax, EV and CF value, NOA, distress and Piotroski. Rule on its wave id against 2.9/2.10 (avoid double testing).
      - Then implement F.1 → F.2 → grade. This is the biggest lever for the goal.
   e. **3.3 identity layer.**
      - Hold MIG; no edits were made yet.
      - Also write the rehearsal link table and names history as Parquet under data/research/identity_rehearsal/<build_id>/ for F.1.
      - Get HEAVY read-only export slices from the controller; make no second full prod copy.
   f. **1.3 batch runner.**
      - Untracked WIP: batch_runner.py, bars_unit_correction.py, scripts/run_slices.py, scripts/recover_stale_run.py, tests/test_batch_runner_kill_resume.py. The agent was writing the kill/resume test.
      - The drill needs HEAVY for a prod copy: one copy, deleted after use.
      - Then the task review. 1.3 unblocks 1.5, 1.6, 2.1, 3.1, 3.2 and the X.1/X.2 loads.
   g. **2.2 fix round 1/2.**
      - WIP: signal_eval.py, tests/test_asof_no_latest_revision.py, test_signal_eval.py, test_signal_eval_fast_panel.py.
      - The fix restores the retirement filter on fundamental_factor_values, plus m1.
      - The restatement rebuild was re-measured identical (10 issuers, 396 rows).
      - Then a scoped re-review.
   h. **X.4.**
      - Committed: 1e438796, stager v2 with accepted_utc built America/New_York-aware, and exact DECIMAL value storage.
      - Finish and verify the re-stage, including the EST/EDT/DST-day spot checks and the count of filings whose entry session moved, then a scoped re-review.
   i. **X.5.**
      - Committed: 20734bce (C-85: non-fatal rate log, retried limiter I/O, streamed parse).
      - The loop is NOT running, and the memory probe on the late big CIKs is unmeasured.
      - Probe, pick the smallest cap with ≥ 25% headroom, make a fresh export, relaunch the loop (~12,750 / 426,151 candidates done), then a scoped re-review.
   j. **1.4.**
      - Committed: 8aba655c (values above 2^64 stored as float64 plus an exact literal in `value_exact`; RULE_VERSION cf-extract-v2).
      - Uncommitted: scripts/stage_companyfacts.py.
      - Resume the v2 extract: 3/85 batches done; the run stopped on low host commit (exit 137).
      - Then assemble manifest.json and members.parquet, run the 20-member equivalence check and the v1→v2 regression, then review. Commands are in task-1.4-report.md `## Stopped (session 4)`.
   k. **X.1/X.2 fetch** (ruling C-91, files only).
      - WIP: scripts/download_13f.py, finra.py, short_volume.py, thirteenf_archive.py, untracked scripts/fetch_finra_short_data.py. No fetch had been launched.
      - First record the outcome of SR-FINRA-2026-012.
4. Then follow the DAG (index §6), running every unblocked node in parallel when files and tokens (MIG, ACT, HEAVY, CAT, EVAL, PANEL, FEAT, STD, DMD) don't overlap:
   - after 1.3: 1.5, 1.6, then the 1.8 B0 heavy run (OPS, HEAVY);
   - 2.1 → 2.3–2.6 → 2.7;
   - 3.1, 3.2, 3.4, 3.5 → 3.6 → 3.8;
   - 3.9 events (with X.5);
   - 4.3 full-family, 4.4 composites (pre-register the core composite from the alpha-priors doc), 4.5 ownership, 4.6 alpha export (JKP-layout monthly Parquet for the L/S book).

How to run it:
- One Opus implementer per task (Agent tool, model opus, run_in_background), then one Opus review. At most 2 fix rounds, then adjudicate with a ruling.
- Keep your own context small: dispatch by brief path, read report files, and have agents reply in ≤ 15 lines. Agent names have no dots.
- Commit subjects carry `[v2 <node>]`; explicit pathspecs only.
- Only the OPS agent holds HEAVY: one warehouse slice at a time, from a fresh git-archive export.
- Pace the swarm against the API budget. Prefer fewer, deeper agents over many shallow ones, and make every agent keep its report's Progress section current.
- No TDD: accept tasks on real-data measurements recorded in the report. Tests only for PIT edge cases, migrations on real-shaped data, kill/resume drills and one end-to-end check per pipeline.

Memory model (ruling C-58, implemented by M0, binding):
- Every Python, pytest or DuckDB process, including one-line probes, runs under `run_memory_guarded.py --job-gb <cap> --wait-minutes N`. The cap is ≤ 1.0 GiB: probes, tests and workers 0.6; writer slices 0.8; set-based passes 1.0. Orchestrators run at 0.2 with `--allow-nested-guards`.
- DuckDB memory_limit and threads are set at connect, well below the cap. pytest always gets `-n 0`.
- Admission: free commit ≥ cap + 1.0 GiB; our running caps sum to ≤ 2.0 GiB; one HEAVY at a time.
- Exit 137 is a normal in-run stop; resume from the ledger.
- Never run uncapped, never above 1.0, no whole-stage transactions, no ART indexes on bulk tables, no `.df()` of a full table.
- Read guard receipts with PowerShell, never with an unguarded `python -c`.

Disk: keep ≥ 35 GB free, at most one large DB copy, and clean your own scratch.

Done when the index §2 exit criteria are met with evidence and the 5.7 candidate release is published. The goal-level minimum along the way is:
- the w1_price wave graded (1.13);
- a fundamentals wave graded (F.2 or 2.10);
- 4.3's QUALIFIED_SIGNALS;
- 4.6's alpha export for the L/S book.

Also stop if everything left is blocked only on an unexecuted user gate or a permission denial, and write a status file saying so.

Standing rules:
- Edit files only with the Write/Edit tools, the ledger included.
- Stamp ledger lines with `date -u +%H:%MZ`; never estimate a time.
- The SEC user agent is exactly atx-db/0.1 atx-research@example.com.
- Never push, merge, stash, reset, restore, clean or discard others' work.
- Keep the prod warehouse and warehouse.duckdb.pre-migrate.20260924-225023.bak.
- Don't touch atx-engine/, atx-vol/, C:\atx-wt\, docs/plans/2026-09-24-alpha-engine-*, python/ (repo root), or other sessions' processes.
- OPENBLAS_NUM_THREADS=1 for every Python process.
- Commit with explicit pathspecs and the trailer Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>.

---

## State at handoff (2026-09-27 00:23Z, HEAD 8aba655c)

### Session 4 (23:37Z → 00:23Z)

**What was set up**
- The user set the new goal. Ruling C-89 reads it as the index's own main product, gives the feature paths priority over S5 polish, and raises the live-agent cap to 12.
- **1.2 is complete.** The review approved it with 0 Critical, 0 Important and 9 Minor findings. The reviewer re-ran the slow PIT test (pass, 0.317 GiB) and a 0329 WAL-replay probe (verify_schema clean). Minors are routed by C-93. The drill receipts are archived in receipts/1.2/ (71 files).

**What landed**

| Commit | Node | What |
|---|---|---|
| 20734bce | X.5 | C-85: rate-log failures non-fatal, limiter file I/O retried, streamed per-member parse. Loop NOT relaunched. |
| 1e438796 | X.4 | FSDS stager v2: `accepted_utc` from America/New_York (DST-aware), exact values. Re-stage completeness UNVERIFIED. |
| 8aba655c | 1.4 | Values > 2^64 keep the float64 plus an exact literal `value_exact`; RULE_VERSION cf-extract-v2. The v1 output is kept at `data/staging/companyfacts/ee099c7394a357f1.cf-extract-v1/`. |

The 1.4 fix was checked on the real member that failed before: CIK 1065088's `dei:EntityPublicFloat` (31354367947000000000) was stored as a DOUBLE 512 away from the literal. Only 3/85 v2 batches are done, identical to v1 in rows and dispositions, with peaks 0.349–0.382 GiB. The full run stopped on low host commit.

**How it ended**
- At about 00:20Z the weekly API limit killed impl-1-12c, impl-x-5b, impl-x-4b, impl-2-2b, impl-1-3, arch-f1, impl-x-12, impl-4-1 and impl-3-3 mid-wrap-up.
- At 00:23Z no atx-db Python job was running.

### WIP left in the tree, by lane (keep all of it)

| Lane / node | Uncommitted files | Last known state |
|---|---|---|
| 1.12 (PANEL, CAT) | `research/spine.py`, `research/price_natives.py`, `scripts/research_price_wave.py`, `research/catalog.py`, `seeds/research_anomaly_catalog.csv`, `docs/research/ANOMALY_CATALOG.md` | Fix round 1. It was applying a one-line doc edit when killed. How far the rebuild got past bucket 15 is unknown (see task-1.12-report.md and the scratch receipts). |
| 1.13 | `scripts/research_run_wave.py` (untracked) | Prep only; waits on 1.12. Hard-codes the old `universe_basis`. |
| 4.1 (EVAL, FEAT) | `research/evaluation.py`, `research/factor_returns.py`, `research/features.py` | Mid-implementation; it was adding the beta-neutral variant. Stubs only (C-92). |
| 2.2 | `src/atx_db/signal_eval.py`, `tests/test_asof_no_latest_revision.py`, `tests/test_signal_eval.py`, `tests/test_signal_eval_fast_panel.py` | Fix round 1 coded. The rebuild matched session 3 (10 issuers, 396 rows). The restatement probe on base vs fix had not run. |
| 1.3 | untracked `src/atx_db/batch_runner.py`, `src/atx_db/bars_unit_correction.py`, `scripts/run_slices.py`, `scripts/recover_stale_run.py`, `tests/test_batch_runner_kill_resume.py` | Was writing the kill/resume test. The drill state and any prod copy on disk are unknown: check disk. |
| 1.4 | `scripts/stage_companyfacts.py` | Adds `--include` plus `value_exact` checks; not run. |
| X.1/X.2 | `scripts/download_13f.py`, `src/atx_db/finra.py`, `src/atx_db/short_volume.py`, `src/atx_db/thirteenf_archive.py`, untracked `scripts/fetch_finra_short_data.py` | Smoke probes only; no fetch launched. |
| 3.3 (MIG) | none | Killed while writing its stop note; no edits. |
| F.1 | none (no design file) | Killed at its first step. |

The tree also holds `atx-engine/src/book/replay.cpp`, which is another session's; never touch it.

### Rulings made in session 4

- **C-89:** the goal equals the index's product. Feature paths come first, the agent cap is 12, and the standing rules are unchanged.
- **C-90:** new node F.1, the fundamentals research fast path (FSDS + CF-R Parquet → the research store), designed first.
- **C-91:** X.1/X.2 are split into fetch now and load after 1.3.
- **C-92:** 4.1 runs in parallel with 1.12/1.13 on stub labels only. The policy v4 sha is unchanged, and evaluation.py is frozen during a 1.13 real run.
- **C-93:** 1.2's minors are routed. m1 (legacy writers lost their duplicate-id guard) BLOCKS 3.6 until 3.1, 3.2 and 3.5 add publish-time uniqueness checks.

### Why the fundamentals fast path (F.1/F.2) matters

The strongest priors for an L/S book are all fundamental (docs/superpowers/handoffs/2026-09-26-tier1-v2-alpha-priors.md):
- issuance and net payout;
- cash-based operating profitability and gross profitability;
- ROE_q and ChTax;
- EV and cash-flow value;
- NOA.

On the warehouse path they are graded only after 1.3 → 1.5 → 1.8 (HEAVY) → 2.1 … 2.7 (HEAVY) → 2.9 → 2.10. The staged FSDS already holds per-filing acceptance timestamps from 2009q2, a stronger PIT clock than FC1. Price history is limited to 2012-03 onward (U4 is not granted), so selection runs 2013–2023 and the holdout starts 2024.

### Notes for the user

- **API budget.** A 10–12-agent Opus swarm used up the weekly limit in about 40 minutes of wall time. The next session should run fewer, deeper agents, or a mix of models, and keep report progress sections current.
- **Other-session files.** Both the modified `atx-engine/src/book/replay.cpp` and the file you had open under `C:\atx-wt\pool-2\` belong to other workstreams. This plan never touches them.
