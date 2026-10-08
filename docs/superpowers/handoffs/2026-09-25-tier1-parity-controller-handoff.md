# Tier1-parity controller handoff — 2026-09-25 01:07 UTC

Branch `feat/tier1-parity`, HEAD `7080a478` (no new commits this session). Goal: the atx-db production fundamentals `/goal` (pasted prompt; same scope as the 2026-09-20 handoff). This session was a thin coordinator. Subagent work went to Codex CLI.

Controller workspace: `.superpowers/sdd/tier1-parity/claude-ctl/` (ledger `progress.md`, briefs, reports, logs).

## 1. Live at handoff: check these first

| Item | State at 01:06 UTC | Where to look |
|---|---|---|
| **CompanyFacts archive19** (heavy, warehouse writer) | RUNNING under `run_memory_guarded.py`, guard child PID **10272**, run-id `activation-companyfacts-archive19`. Progress: `processed=2975/20390 loaded=2474 empty=476 unavailable=25 failed=0 rows=0`. Rate is about 225 members/min, so the ETA was about 02:25 UTC. | `.superpowers/sdd/tier1-parity/activation-companyfacts-archive19{-memory.json,.err,.log}`; launcher `claude-ctl/launch-archive19.ps1` (copied verbatim from the resume doc) |
| **L1 desk-SQL pack + gap analysis** (Codex, light) | IN PROGRESS. It has written untracked files, **not yet committed or reviewed**. | `claude-ctl/l1.log`; report goes to `claude-ctl/l1-report.md`, final summary to `claude-ctl/l1-final.md` |
| **L2 historical identity source qualification** (Codex, light) | Report written (DONE_WITH_CONCERNS); final message pending | `claude-ctl/l2-report.md`, probe `claude-ctl/l2_source_probe.py` |

**Caveat:** all three were started as background tasks of this Claude session. Closing the session may kill them. Before doing anything else:
1. Check that PID 10272 and its guard parent are still alive. Read the `status` in `activation-companyfacts-archive19-memory.json`, then tail the `.err` file.
2. If archive19 died mid-run, close its ledgers terminally using the archive18 recovery pattern (`close_companyfacts_archive18_headroom_stop.py` plus its receipt). Then resume as `archive20`, resuming from the right predecessor. Do not re-run recovery for archive18; it is already done.
3. **Verify `rows=0`.** The progress counter shows `rows=0` while `loaded` is above 2,400. Acceptance needs actual new-write results. At the terminal state, confirm with bounded SQL (fact/point counts by run-id, distinct CIKs) that facts were written. If this is only a flush-time counter, record that; otherwise treat it as a defect.
4. If L1 died, its untracked files are incomplete. Read `l1.log` before keeping or deleting them. Never delete them blindly.

## 2. What this session established

- **State reconciliation** (Codex read-only): `claude-ctl/t00-reconcile-final.md` is the current source of truth and supersedes lagging docs. Key points:
  - Pinned snapshot `2026-09-20` is incomplete.
  - Standardized, derived, market, universe and survivorship tables all have **0 rows**.
  - `sec_company_facts` has 47.9M rows across 12,959 CIKs.
  - `equity_daily_bars` has 32.0M rows, covering 2012-03-26 to 2026-09-18.
  - All 12 SLOs are unmeasured; 375 of 379 DQC entries are unrun.
  - CVX desk2 returns 0 qualified observations.
  - The validated command sequence is in its §1; resources and guard profiles are in its §3.
- **Headroom:** window4 was ready at 00:53 UTC (5.3 GiB physical / 7.7 GiB commit sustained 120 s). Receipt: `source-archive19-lowmemory-window4.json`. The other session's C++ build and risk tests (C:\atx-wt pool-6/7) were running at 00:43 and had finished by 00:50.
- **L2 headline:**
  - No local source qualifies the full historical US-common universe.
  - TickerHistory3.parquet (32.3M rows, 71 columns, 2012-03-26 to 2026-09-18) has no CIK, security class, primary exchange/MIC, listing dates or delisting fields. Its CIK link is current-ticker only and unverified.
  - Empty `exchange_listings` venues are by design: the loaders insert NULL `exchange_code`/`mic`.
  - The downstream tables are empty for two reasons: the activation suffix never ran after `statement_points` failed, and listing-status/listing-event ingestion is missing from activation's stage order.
  - Correctness defects found (two reproduced with the existing SQL on tiny fixtures):
    - universe input clocks / current-entity joins
    - inconsistent Nasdaq action vocabulary
    - issuer-level SEC forms promoted into security delist events
    - first-run terminal-return dependency ordering
  - See `l2-report.md` §§1–4 for the task list.

## 3. Tooling facts (learned this session)

- The `codex` on PATH is 0.151.0 and **rejects `gpt-6-astra`**. Use the VS Code extension binary `C:\Users\natha\.vscode\extensions\openai.chatgpt-26.5917.62051-win32-x64\bin\windows-x86_64\codex.exe` (0.155.0-alpha.16.3). The dispatcher is `claude-ctl/cx.sh <id> <read-only|full> <brief>`; it writes `<id>.log` and `<id>-final.md`.
- The Codex `read-only` sandbox cannot `import duckdb` ("DLL load failed … Access is denied"). Any agent that needs DuckDB needs `full`.
- Claude Code's auto-mode classifier **denied** dispatching an autonomous full-access Codex agent that writes production data (OPS-1, brief `claude-ctl/ops1-brief.md`). Light full-access agents (L1, L2) were allowed. To run OPS-1 as an agent, the user must grant permission. Otherwise the controller runs each guarded command itself.
- All subagents share one rules file: `claude-ctl/global-rules.md` (safety, the single heavy slot, resource caps, report contract).

## 4. Next steps (in order)

1. Take archive19 to a terminal state and inspect it against the acceptance criteria in `ops1-brief.md` §Outcome 1: all 20,390 members have a disposition, and real writes are proven.
2. Run the submissions resume. The command is in reconcile §1: resume from `04cf947d-…`, batch 50, no `sec_bulk_download`. Take a fresh 4/6 GiB headroom window first.
3. Run CVX `earnings_release_facts`. The command is in reconcile §1; it needs a 6/8 GiB window.
4. Review L1: read `l1-report.md`, then run one independent Codex review of its diff. Its untracked files are:
   - `atx-db/sql/research/desk-q{1..5}-*.sql`
   - `atx-db/scripts/read_desk_question_pack.py`
   - `atx-db/tests/test_desk_question_pack.py`

   Then commit them. Take its ranked repairs and dispatch generic fixes that must land **before run5**.
5. Turn L2 §4 into implementation tasks:
   - stage-order repair
   - clock/join fixes
   - the delist-event promotion fix
   - terminal-return ordering
   - honest availability labels

   `activation.py`, `registry.py`, `jobs.py` and migrations are serialized.
6. Run5, the full-universe suffix from `statement_points` (command in reconcile §1). It must run after steps 4 and 5 land.
7. Then continue down reconcile §8: desk/research acceptance → coverage/SLO/DQC measurement → first eligible release → full non-slow gate and Codex branch review → incremental refresh.

## 5. Working tree at handoff (do not disturb others' work)

- `M .superpowers/sdd/tier1-parity/production-resume-sequence-2026-09-21.md`: the prior operator's uncommitted edits. SM2 is marked "in progress" there but is actually done (`7080a478`). Update and commit it with the archive19 results.
- Untracked L1 files (listed above): in progress.
- Untracked `docs/plans/2026-09-24-alpha-engine-*`: belong to another session. Do not touch.
- `stash@{0}` must stay.

## 6. Rulings made this session

1. **All subagent work runs on Codex, dispatched via `codex exec`; Claude only coordinates.** Why: the goal mandates "Codex models only". If wrong: switch to the Claude Agent tool; no outputs need rework.
2. **Sandbox: read-only for investigation, full for DuckDB, ops or implementation work, with hard rules in every brief.** If wrong: a full-access agent could run a destructive command. Mitigated by the rules and git history.
3. **After the OPS-1 denial, the controller launches the pre-validated guarded ingestion commands itself, one at a time, after a headroom observation.** If wrong: the user may prefer to grant the Codex ops agent. No data risk, since the commands are the same.
