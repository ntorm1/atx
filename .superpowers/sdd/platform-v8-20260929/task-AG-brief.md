# Wave AG (alpha generation): four parallel lanes, disjoint files (owner directive 2026-10-01, PM session 5)

Owner directive: more parallel lanes, and the sprint design may change to prioritise real progress in the core
alpha-generation features of `atx-engine` and `atx-impl`. The root (cells, builds, data) stays serial; these lanes
run beside it and never touch it.

Binding first read: `C:/atx-wt/pool-2/.superpowers/sdd/platform-v8-20260929/lane-rules.md` (no C++ build, no real
data, nothing dated 2024-01-01 or later, no subagents, no push, pytest on synthetic data only, Write / Edit tools
for files, one commit per task with the trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`).
C++ lanes also read `.agents/cpp/agent.md` first.

## What does not change (binding on every lane)

- The v8 trial program is untouched: N <= 51, prereg rule 10 ("v8 runs no mined campaign") stands, no new v8
  cell. Everything here is code, derivation or registration text for v9 (or for an owner ruling on OD-7).
- Nothing in this wave reads a return, an IC or any statistic of real data. Derivations are analytic or on
  synthetic draws with fixed seeds.
- Identity discipline: with every new flag absent, accepted outputs stay byte-identical. The mining determinism
  golden (registry chain head `0x889874a3b9b29c55` on the fixture, equal at 1 and 4 workers) is never edited.
  If a task cannot be done without moving the golden, STOP that task and report it; the PM rules.
- Plan section 13 ("Do not build in v8") binds: no float32, no reordered evaluation, no free-form search over
  all operators, no language model as judge, no fitted theme or sleeve weights.
- Merge: these lanes merge after the V8-F freeze gate (integration 8), by commit SHA. Root compiles your C++ for
  the first time under `/W4 /WX`.

Context every mining lane reads: `task-MINE-FIX-report.md` and `review-mine.md` in your worktree's sprint
directory (`.superpowers/sdd/platform-v8-20260929/`), the plan's Task H-3
(`docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md`, section 10), and in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-v8-20260929/progress.md` the entries E-6, E-32, E-32a, E-33, PM4-13
(grep the id, read the whole entry).

## Rulings that bind this wave

- PM5-7: wave AG runs now, beside the v8 cells, registered for v9; merge after the freeze gate.
- PM5-8 (MINE-15, declared before any campaign): a candidate whose rho against a member is undefined (no date
  with at least `min_names` joint names, or fewer than `min_dates` joint dates) FAILS the rho rule; "to every
  member" means every member is checked and passes. Rule id stays `mined-v1` (nothing was run under it).
- PM5-9 (MINE-14, same standing): the greedy rho step runs over the whole above-hurdle list, then the shortlist
  cap applies. The Benjamini-Yekutieli m is the count that reaches the confirm read, as before.
- PM5-10 (memory target): the default 4-year campaign (1,405 x 6,100, 16 fields, 3 regressors, 32 members, 4
  workers, shortlist 16) must fit 2,560 MiB (the IC cap of OD-2) or, failing that, the lane states the smallest
  footprint reachable without changing a bit of any result and what stands in the way. No owner memory ruling
  is assumed.

## Lane MINE-MEM (pool 8, branch feat/platform-v8-minemem-20261001, from 20e7bd19)

Owns: `atx-impl/src/strategy_mine.{hpp,cpp}`, `strategy_mine_detail.hpp`, `strategy_mine_pool.hpp`,
`strategy_mine_trials.cpp`, `atx-engine/src/factory/search_driver.{hpp,cpp}` and the factory engine sources it
allocates through, the memory model (`mine_working_bytes` and its table), `atx-impl/tests/strategy_mine_test.cpp`
(memory and rung-failure tests), `atx-engine` factory tests for what you change.

1. Memory (PM5-10). Today: 10.85 GiB = engines 3.72 GiB + pool 2.29 GiB + role 1.32 GiB + rest (MINE-10 table in
   `task-MINE-FIX-report.md`). Read the allocation sites and the model term by term, then cut the footprint
   without changing any computed bit: one read-only role panel shared by all workers; engine scratch sized by
   the slots a program uses and not by the configuration bound; per-worker buffers reused across trials; the
   pool stored once; fields mapped rather than copied where the IC runner already does so. Keep float64 and the
   evaluation order. `mine_working_bytes` must stay an exact term-by-term model of what is allocated, and the
   verb must still refuse before any payload when the model exceeds `--max-memory-mib`.
   Tests: the model equals the sum of the allocation sizes on the fixture; the table in the report is
   recomputed for the 4-year geometry at 1 and 4 workers.
2. MINE-16: a compile or evaluate failure at a racing rung gets its own trial status (not racing-rejected); the
   registry identity becomes count = evaluated + racing-rejected + screen-rejected + rung-failed; the fixture
   asserts rung-failed == 0. If this moves the golden chain head on the fixture, STOP this task and report.
3. Report `task-MINE-MEM-report.md`: the new memory table (bytes per term, before and after), why no bit can
   move (argument per change), what root builds, gtest filters, and the golden check at 1 and 4 workers.

## Lane MINE-STAT (pool 9, branch feat/platform-v8-minestat-20261001, from 20e7bd19)

Owns: `atx-impl/src/strategy_mine_rule.{hpp,cpp}`, `strategy_mine_promote.cpp`,
`atx-impl/tools/mine_overlap_factor.py` and its test, the rule tests in `strategy_mine_test.cpp` (your tests
only; MINE-MEM also edits that file: keep to your own test cases, appended at the end of the rule section).

1. Overlap factor at the campaign's own level (lifts PM4-13). Today F = 1.55 is checked to the 99.8% tail, so a
   budget above 1,000 is refused. Re-derive F as a function of the budget: for budgets 100, 1,000, 10,000 the
   factor that makes the per-trial rejection rate nominal at the Bonferroni level alpha / N on the label-overlap
   null, at the 504-row floor and at the 4-year length, with enough null draws that the tail estimate has a
   stated standard error (say how many draws and why). Output: a table budget band -> F in the rule header,
   read by hurdle and by the confirm's BY p; `kMinedMaxBudget` rises to the largest budget the table covers;
   the recipe and `campaign.json` record the F used. State plainly if the confirm-window ratio (1.70 at 200
   rows) needs its own factor and add it if so.
2. MINE-14 (PM5-9) and MINE-15 (PM5-8) as ruled, with fixture tests that fail under the old behaviour.
3. If 1 or 2 moves the golden chain head on the fixture, STOP that task and report (do not edit the golden).
4. Report `task-MINE-STAT-report.md`: the derivation (method, seeds, draws, the table with standard errors),
   what changed in the rule text, what root builds, gtest filters, pytest files.

## Lane MINE-RUN (pool 3, branch feat/platform-v8-minerun-20261001, from 20e7bd19)

Owns: new files only: `docs/plans/2026-10-01-v9-mine-campaign-prereg.md`, `scripts/specs/v9/mine-c1.json`
(template), a runbook `docs/plans/2026-10-01-v9-mine-campaign-runbook.md`, and tests for the spec template. If
`scripts/research_cycle.py` / `research_spec.py` cannot yet carry a `mine` cell, make the smallest plumbing
edit and list it under cross-lane edits (several lanes touch those files).

Goal: everything the owner needs to grant OD-7 with one decision, and everything root needs to then run the
first mined campaign through `research_cycle.py` without designing anything after a read.
1. Read the verb as coded (`equity_strategy_mine.cpp`, `strategy_mine*.{hpp,cpp}`, `research_ledger.py`,
   `backtest_integrity.py` campaign lines): CLI, inputs, outputs, ledger lines, refusals.
2. Pre-registration draft (text only, declared before any read): campaign id; the one theme all mined members
   share; operator grammar and field list as the catalogue allows (no free-form search); budget (<= the ceiling
   in force; state the hurdle t it implies); discover and confirm windows inside TRAIN [2020-01-01, 2024-01-01)
   with the label-overlap gap; `mined-v1` promotion rule as coded (PM5-8, PM5-9 included); how promoted members
   enter the book (one `add-alpha` wave, judged whole like R-2 / R-7, one construction trial); how the campaign
   counts in N and in the DSR (prereg rule 10, E-32, E-33); the memory cap it needs (from the MINE-10 table;
   lane MINE-MEM is reducing it, so state it as a parameter); what voids a campaign. Every number that is a
   choice is listed in one table "decisions for the owner" with a recommended value and its reason.
3. Spec template and runbook: exact commands (lock, plan, run), caps, the order of reads (counts and mechanics
   before any statistic), ledger checks, and the acceptance on the fixture that must pass first.
4. Report `task-MINE-RUN-report.md`: files, open questions, anything in the verb that blocks a real campaign.

## Lane LIB3 (pool 11, branch feat/platform-v8-lib3-20261001, from 67f04389)

Owns: new files only: `docs/plans/2026-10-01-v9-library-draft.md` and, if the registry supports draft entries
that are off by default, a draft file beside the existing v8 drafts (follow exactly how LIB2 did it:
`task-LIB2-report.md`, and the plan's Tasks R-2 and R-7). No registry default list changes.

Goal: the v9 prior-class library wave, drafted and ready for root's `--plan-only` compile check.
1. Read the registry of record (48 alphas, 43 fields), the fields v9 / v10 / v11 / v12 field lists, the DSL
   operator catalogue, the theme list (`PRIOR_THEMES`), the R-2 / R-7 / LIB2 drafts and their withdrawal notes,
   and plan sections 5 and 13.
2. Draft at least 10 candidates of origin class `prior`: each with the canonical definition from the
   literature, the sign fixed by the literature (never by our data), full citation (authors, year, journal,
   sample), the post-publication and post-2004 evidence that it is not dead or spanned (plan section 13 lists
   families that are), its theme, the exact DSL in registered operators and existing fields (or the new field
   it needs, with the stage it is built from and the point-in-time rule), expected overlap by construction with
   existing members, and a reason it adds breadth rather than a variant of a member. Prefer families the book
   does not hold yet. State for each: READY (existing fields), NEEDS-FIELD, or NEEDS-DATA (an ask to atx-db).
3. No data is read; no candidate is ranked by anything measured. You may use the web for literature; every
   claim carries its source; mark anything you could not verify as unverified.
4. Report `task-LIB3-report.md`: the table of candidates, the fields to build, what root runs to compile-check
   the DSL, open questions.

## Final reply (every lane)

Short: status (DONE / DONE_WITH_CONCERNS / BLOCKED), commit per task, head SHA, one line of pytest results,
concerns. No long prose.
