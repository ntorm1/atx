# Goal prompt — resume atx-db tier-1 v2 (session 2 controller)

Paste the block below as the next parent agent's `/goal`. It assumes the state recorded in
`docs/superpowers/handoffs/2026-09-26-tier1-v2-status-1.md` and the ledger `.superpowers/sdd/tier1-v2/progress.md`.

---

/goal Resume and finish the atx-db tier-1 v2 DAG sprint series. Spec: docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md. Sprint files: the s0, s1, s2, s3, s4, s5 and sx files next to it. Read the index first (§3 constraints, §4 memory doctrine, §5 swarm rules, §9 rulings), then the status file docs/superpowers/handoffs/2026-09-26-tier1-v2-status-1.md.

Resume, do not restart. The ledger is .superpowers/sdd/tier1-v2/progress.md: every node with a `Task <node>: complete` line is DONE — never re-dispatch it. Rulings R-1..R-14 and controller rulings C-1..C-41 in that ledger are binding; add new rulings as C-42 onward. Keep using the same workspace (.superpowers/sdd/tier1-v2/): CONTEXT.md (standing rules every agent reads first), REVIEW-CONTEXT.md (reviewer lens + output contract), CARRY.md (per-node must-fix items routed from S0 reviews — a node's implementer treats its CARRY section as part of its brief), pkg.sh (review packages from explicit commits: `bash .superpowers/sdd/tier1-v2/pkg.sh OUT [SHA...] [--tag NODE [--since SHA]]`), briefs via the SDD task-brief script with OUTFILE `.superpowers/sdd/tier1-v2/task-<node>-brief.md`.

First actions, in order:
1. Reconcile: `git log --oneline a5c83860..HEAD` and `git status --short -- atx-db`; read the "Stopped" sections of task-0.13-report.md and task-0.14-report.md.
   - 0.14 (P14 incremental partitions): fix round 1/2 is committed and measured (f6aacb45, fix base 3b39b32c) — dispatch its scoped re-review (findings I1, I2 + the C-15 import from task-0.14-review.md; package `pkg.sh OUT --tag 0.14 --since 3b39b32c`).
   - 0.13 (VA1 + A8 follow-up): fix round 1/2 was stopped with finished-but-UNRUN edits left UNCOMMITTED in 8 files (equity_price_metrics.py, _vendor_artifact.py, _forward_return_publication.py, market_daily.py, sql/research/desk-q5-monthly-factor-portfolios.sql, tests/test_desk_question_pack.py, tests/test_fundamental_signal_evaluation.py, tests/test_vendor_artifact_repair.py). They are live in the shared tree and affect other tests. Dispatch a fresh implementer on task-0.13-brief.md + task-0.13-review.md + task-0.13-report.md ("Stopped" section) to measure I1/I2/I4 on the retained parquet, finish the remaining items, rule-check the "< 5% always repaired" deviation, and commit `[v2 0.13]`; then its scoped re-review. Do not discard those edits.
2. Run 0.15 (integration gate) once 0.13 and 0.14 are complete. Its PASS sha is the S1 base.
3. Then follow the DAG: S1 nodes 1.1, 1.7, 1.9, 1.10, 1.11 are unblocked by 0.15 and file-disjoint — dispatch them in parallel.

Run it as subagent-driven development: one Claude Opus 5.5 subagent per task (Agent tool, general-purpose, model opus), then one Opus review. Allow at most 2 fix rounds, then adjudicate and record the ruling. Keep your own context small: dispatch by brief path and read report files; reviewers write full reviews to task-<node>-review.md and reply in ≤15 lines. Never paste history into prompts. Agent names may not contain dots (use impl-1-3, rev-1-3). Every commit subject carries `[v2 <node>]`.

Follow the DAG: run every unblocked node in parallel whenever files and ownership tokens (MIG, ACT, HEAVY, CAT, EVAL, PANEL, FEAT, STD, DMD; item_registry.py is STD per C-29) don't overlap. Keep at most 8 live agents. Only the OPS agent holds HEAVY, and runs one warehouse writer or read-only export slice at a time, always from a fresh git-archive export of current HEAD (C-23).

No test-driven development. Accept every task on real-data measurements recorded in its report. Write tests only for PIT edge cases, migrations on real-shaped data, kill-and-resume drills and one end-to-end check per pipeline.

Memory must never be the reason to stop. No job above a 1.0 GiB guard cap, no transaction over ~2M rows, resumable ledgered slices through the slice runner. Until node 1.3 fixes the guard's page-rounding cap check (CARRY §1.3), only page-aligned caps work (0.5 or 1.0 GiB). If a job is killed or runs out of memory, re-engineer it per §4 M8 (split into buckets, stream, drop indexes, fresh process, threads=1). Never raise caps, lower guard floors or use the retired archive21 watcher. Every DuckDB connection, including throwaway probes, sets memory_limit and threads at connect.

User gates U1-U11 (index §8) are not granted unless I say so (ruling C-1). Build gated code, but fetch nothing gated, and take each task's labeled fallback.

Done when index §2 exit criteria are met with evidence and the 5.7 candidate release is published with its release notes. Also stop if every remaining node is blocked only on an ungranted user gate or a permission denial: write a status file listing what's blocked and why.

Standing rules: edit files only with the Write/Edit tools (the ledger included — append with Edit, never heredoc). Stamp ledger lines with `date -u +%H:%MZ`, never estimated times. SEC user agent is exactly atx-db/0.1 atx-research@example.com. Never push, merge, stash, reset, restore, clean or discard others' work. Keep the prod warehouse and its latest backup. Don't touch atx-engine/, atx-vol/, C:\atx-wt\ or other sessions' processes. OPENBLAS_NUM_THREADS=1 for every Python process. Commit with explicit pathspecs and the trailer Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>.

---

## Notes for the user before launching

- **Prod-warehouse permission.** In session 1 the auto-mode permission classifier denied a second read-only open of
  `atx-db/data/warehouse.duckdb` ("[Production Reads]") after one guarded read-only open was allowed. Nodes 1.2 (full
  copy of prod), 1.8, 2.7, 3.6, 5.1, 5.3 and 5.7 all open or write the prod warehouse through the OPS agent. If you want
  those to run unattended, grant that explicitly (a permission rule for the guarded OPS launches, or a sentence in the goal
  such as "the OPS agent is authorized to open and write atx-db/data/warehouse.duckdb through run_memory_guarded.py").
  Without it, the next controller will reach 1.8 and stop with a blocked-status file.
- **User gates.** Granting U2 (13F + FINRA), U6 (French / JKP / OSAP / q5 benchmark files) and U3 (full FSDS history)
  unlocks the ownership wave (4.5), the external validation gate (4.2) and a stronger fundamentals benchmark (2.8).
  U5 (strict identity filings) is the only path from a `candidate` to an `eligible` release.
