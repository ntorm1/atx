Finish atx-impl v8 (the mega alpha US equity long/short book) from the status at
`C:/atx-wt/pool-2/docs/plans/2026-10-01-platform-v8-status-4.md`, following the sprint plan
`docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md` and the ledger `.superpowers/sdd/platform-v8-20260929/progress.md`
(rulings E-1..E-45 with their sub-rulings, W0-a..n, R2-a..h, R7-a..c, PM3-1..10, PM4-1..15 are binding;
`integration-log.md` has every merge, build, test and data run).

Working method (owner directive, unchanged): subagent-driven development with parallel Opus 5.5 lanes, one pool
worktree per lane (pools 3, 4, 7, 8, 9, 10, 11; root is `C:/atx-wt/pool-2`); no test-driven development; no review
agent after each task; adversarial reviews only at logical break points, read-only, split by area when the diff is
too large. Children never build or run data; one integrator at a time merges, builds, tests and runs data in root;
the PM only coordinates and rules and keeps its own context small (briefs and reports are files; agent replies are
short). You are approved for every recommendation in the plan, handoff 1 and statuses 2, 3, 4; make rulings yourself
as "decision -- why -- cost if wrong" and write them to the ledger before the measurement they could bias. Park
ledger edits outside the repository while an integrator runs data (the bounded runner needs a clean tree). Merge
lane work by commit SHA. Read `lane-rules.md` and `integrator-rules.md` before dispatching. The trial budget
(N <= 51, 15 admission trials + 8 re-screens) is fully allocated (E-38); any further alpha lane builds flag-gated
code registered for v9, never a new v8 cell.

State at the start: root code head 9c5cfa0c (build v8-10), tree clean, all suites green, identities 1, 4, 7, 8
pass, Wave 0 R1-R11 built (fields v9 on lo1 and lo3), field overlap explained (PM4-15), no statistic read. N = 37,
admission trials 0. Unmerged lanes (status 4 section 2): MINE-FIX finished (pool 8, 20e7bd19); FIX-4a stopped
mid-way (pool 4, b8b68f4e, the head is a WIP commit of the unfinished R6B-O-5); FIX-4b stopped mid-way (pool 10,
cf15052d). Neither FIX-4 lane wrote a report.

Do, in this order:
1. Read status 4 sections 2, 3, 5 and 7, the ledger from "PM session 4" down, and `task-FIX-4-brief.md`. Resume
   both FIX-4 lanes in parallel: FIX-4a reviews the WIP diff of b8b68f4e, finishes R6B-O-5, then O-6, O-7, R6B-C-1
   (PM4-7), R6B-C-5, re-runs the Python suites of everything the lane committed, and writes its report; FIX-4b
   does R6B-S-1, R6B-S-2, re-runs its suites and writes its report.
2. Integration 6 part C (root, tag prefix v8-11): merge FIX-4a and FIX-4b by SHA; build; the gtest filters from the
   two reports; every suite (baselines in status 4 section 4); identity 4, and identity 1 if any NAV source
   changed. Then one scoped read-only review of the FIX-4 diff: the tie rule PM4-12 in the Python fitter and the
   C++ kernel (same expression, same order, no-tie bit identity), the theme order PM4-11, the pooled fit PM4-7,
   the ladder criteria PM4-8 / PM4-9 against E-43, E-44, E-45.
3. Wave 0 part 2b (root): `task-W0-run-brief.md`, dispatch 2b: R12 plan-only, the cold u pass, the signal overlap
   (must be bit-identical: no v7.1 candidate reads the one field that changed) then the daily IC overlap under
   W0-a, R14 pins into `v8-prereg.md` (fields v9 manifests are in status 4 section 4), the chained `protocol`
   line, `lock --write` on base-lo1 / base-lo3. v10 from v9 expects 49 reused / 21 computed and v11 70 / 3
   (PM3-5a).
4. Cells per `task-CELLS-brief.md` (procedure, registered criteria, rulings per cell), in batches, one integrator
   at a time: B0a, B0b, B0c (winner with `--label-role` on the delisting-returns role, E-25, E-39;
   `--warm-start-sessions 60`; capacity curve; gross at row `score_begin`, A-3; year tables and the Appendix A
   block with `history reads 0`; diagnostics on B0c in `--only` splits, E-18; Release A/B). Then R-1; R-2 (fields
   v10 first); R-3; R-4; R-5; R-6 (risk model on the 4-year role first; S_prior 20; convergence counts and the
   tripwire before any return; a void under E-31a is a blind fix and a re-run, no new trial); R-7 (fields v11);
   R-8. Then by E-38 / E-45: if R-6 was accepted, R-10, R-11, R-12 (fields v12); if R-6 was rejected, R-9. A
   rejected cell is never retried. Hard budget N <= 51.
5. V8-F cumulative test and freeze gate (one-sided p, E-34; print both). OD-3 history read only if the gate asks
   for it (E-17, E-35, E-41, PM4-7). H-2 measurement. Scorecard v8 (criteria complete through R-12, PM4-8), pitch
   config filled and rendered with 0 unavailable blocks, the v7 pitch re-rendered. Pitch claims follow status 2
   section 9: B0c is the baseline; an improvement is claimed only where the cumulative test or a registered
   mechanical criterion supports it; if the freeze gate is unmet the scorecard says so and names OD-3.
6. Integration 7 (after the freeze gate, PM4-2): merge MINE-FIX 20e7bd19; build `atx-impl-strategy-mine-tests` and
   `atx-engine-factory-tests` (wide rebuild); the golden 0x889874a3b9b29c55 must hold at 1 and 4 workers. Then
   handoff 2 and the merge command for the owner.
