Finish atx-impl v8 (the mega alpha US equity long/short book) from the status at
`C:/atx-wt/pool-2/docs/plans/2026-10-01-platform-v8-status-5.md`, following the sprint plan
`docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md` and the ledger `.superpowers/sdd/platform-v8-20260929/progress.md`
(rulings E-1..E-45 with their sub-rulings, W0-a..n, R2-a..h, R7-a..c, PM3-1..10, PM4-1..15, PM5-1..27 are binding;
`integration-log.md` has every merge, build, test and data run).

Working method (owner directive, unchanged): subagent-driven development with parallel Opus 5.5 lanes, one pool
worktree per lane (pools 3, 4, 7, 8, 9, 10, 11; root is `C:/atx-wt/pool-2`); no test-driven development; no review
agent after each task; adversarial reviews only at logical break points, read-only. Children never build or run
data; one integrator at a time merges, builds, tests and runs data in root; the PM only coordinates and rules and
keeps its own context small. You are approved for every recommendation in the plan, handoff 1 and statuses 2 to 5;
make rulings yourself as "decision -- why -- cost if wrong" and write them to the ledger before the measurement
they could bias. Park ledger edits outside the repository while an integrator holds root. Merge lane work by commit
SHA. Read `lane-rules.md` and `integrator-rules.md` before dispatching. Owner directive of 2026-10-01: extra agents
go to alpha-generation features of atx-engine and atx-impl; the v8 trial program itself is unchanged (N <= 51, 15
admission trials + 8 re-screens, no mined campaign in v8 without an owner ruling on OD-7).

State at the start: root code head `1cc4c6c9` (build `v8-12`, every research executable on one tag; PM5-21: no
executable and no cycle or tool script changes until the freeze gate). Wave 0 closed; base-lo1 and base-lo3
locked. Ledgered: B0a (N 38), B0b (N 39, accepted), B0c (N 40, the baseline, S2 net Sharpe +1.133). R-1 was run to
the mechanics read and failed on gross (1.0672, limit [.90, 1.05]); none of its returns was read; it is not
ledgered. Admission trials 0; history reads 0. Unmerged: FIX-6 `702cf051` (pool 10, tests only), the mining
branch `1bd448cd` (pool 8, C++ uncompiled), FIELDS-V9 / LIB3 `834d5a05` (pool 11). An interim report exists.

Do, in this order:
1. Read status 5 sections 2, 3, 5 and 7 and the ledger from "PM session 5" down. Merge FIX-6 `702cf051` (tests
   only) and run `scripts/tests`.
2. Rule on R-1 before anything reads its returns (status 5 section 5): one read-only code look at why all-rows
   gross rose from .982 to 1.067 under the re-rank; then either ledger it rejected at N 41 (R-10 and R-11
   undefined by E-45; R-3 runs `ew-theme-aim-v2`, E-27a) or, if a defect is shown, prereg rule 7 (defect line,
   blind fix, re-run, no new trial). L is not re-derived.
3. Cells per `task-CELLS-brief.md`, in batches, one integrator at a time, parent = the last accepted cell: fields
   v10 (49 reused / 21 computed) then R-2; R-3; R-4; R-5; the risk model on the 4-year role then R-6 (S_prior 20;
   convergence counts and the tripwire before any return; a void under E-31a is a blind fix and a re-run); fields
   v11 (70 / 3) then R-7; R-8. Then by E-38 / E-45: R-10, R-11, R-12 (fields v12) if R-6 was accepted, with R-10
   and R-11 only if R-1 was accepted too; R-9 if R-6 was rejected. One-sided p per PM5-23. A rejected cell is
   never retried. Hard budget N <= 51.
4. The W0-4 re-runs (PM5-22): v6.1, v7.0, v7.0-lo3, C1-C3, spo-v1, spo-v2 on the 4-year role, adding 0 to N;
   their gates log and do not stop (PM5-18).
5. V8-F cumulative test and freeze gate (one-sided p, E-34; print both). OD-3 history read only if the gate asks
   for it. H-2 measurement. Scorecard v8 (criteria complete through R-12), pitch config filled and rendered with 0
   unavailable blocks, the v7 pitch re-rendered. B0c is the baseline; an improvement is claimed only where the
   cumulative test or a registered mechanical criterion supports it; if the freeze gate is unmet the scorecard
   says so and names OD-3.
6. Integration 8 (after the freeze gate): merge the mining branch `1bd448cd` and FIELDS-V9 `834d5a05`; the C++
   halves of R6C-3 and R6C-7; build the mine, factory, alpha and impl test targets; the golden
   `0x889874a3b9b29c55` must hold at 1 and 4 workers; the fixture's `rung_failed == 0`; `AlphaVmSlotReuse.*`.
   Then handoff 2 with the owner decisions (OD-7 and the mined-campaign pre-registration draft, the four data
   asks to atx-db, the v9 library draft's open points) and the merge command.
