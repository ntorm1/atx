Finish atx-impl v8 (the mega alpha US equity long/short book) from the status at
`C:/atx-wt/pool-2/docs/plans/2026-09-30-platform-v8-status-3.md`, following the sprint plan
`docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md` and the ledger `.superpowers/sdd/platform-v8-20260929/progress.md`
(rulings E-1..E-45 with their sub-rulings, W0-a..n, R2-a..h, R7-a..c, PM3-1..10 are binding; `integration-log.md`
has every merge, build, test and data run).

Working method (owner directive, unchanged): subagent-driven development with parallel Opus 5.5 lanes, one pool
worktree per lane (pools 3, 4, 7, 8, 9, 10, 11; root is `C:/atx-wt/pool-2`); no test-driven development; no review
agent after each task; adversarial reviews only at logical break points, read-only, on the most capable model, split
by area when the diff is too large. Children never build or run data; one integrator at a time merges, builds, tests
and runs data in root; the PM only coordinates and rules. You are approved for every recommendation in the plan,
handoff 1, status 2 and status 3; make rulings yourself as "decision -- why -- cost if wrong" and write them to the
ledger before the measurement they could bias. Park ledger edits outside the repository while an integrator runs
data (the bounded runner needs a clean tree). Merge lane work by commit SHA. Read `lane-rules.md` and
`integrator-rules.md` before dispatching. The owner also asked for more parallel lanes on alpha generation
(atx-engine + atx-impl); the trial budget (N <= 51, 15 admission trials + 8 re-screens) is now fully allocated
(Ruling E-38), so any further alpha lane builds flag-gated code registered for v9, never a new v8 cell.

State at the start: root head 18176b4d (code 48c625fc, build v8-9a), tree clean, all suites green, identities 1-8
pass, Wave 0 R1-R9 built on 2020-2023, no statistic read. N = 37, admission trials 0. Unmerged finished lanes:
FIX-2 (pool 7, de32b9ad, round 1 complete), FIX-3 (pool 10, 75acb091), COMB2 (pool 11,
1e8af5b8), ORTH (pool 4, c1cc57ce); MINE-FIX (pool 8) stopped mid-way (its report says what remains).

Do, in this order:
1. Read status 3 sections 2, 3 and 5, the ledger from "PM session 3" down, and the Round 1 sections of
   `task-FIX-2-report.md`, `task-FIX-3-report.md`, `task-R-10-report.md`.
2. If the owner wants the mining verb finished, resume MINE-FIX (pool 8) for the findings its report lists as
   remaining, per `task-MINE-FIX-brief.md`; it is off the v8 critical path and merges in integration 7. Nothing
   else blocks step 3.
3. Integration 6 part B (root, tag prefix v8-10): merge FIX-2, FIX-3, COMB2, ORTH by SHA; part-B edits per status 3
   section 2 (ERA elif, `ew-theme-aim-v2` alignment and the un-skipped equality test, r10 parent map per E-45);
   build; the gtest filters from the four reports; every suite. Then one scoped re-review (read-only) of the
   round-1 fixes and the two new composition rules against their registrations (E-27b, E-31a, E-44, E-45).
4. Wave 0 part 2 (root): `task-W0-run-brief.md` with the Steps header restored to R10-R14 (fields v9 on lo1 and
   lo3, plan-only, cold u pass, overlap reports in the order field -> signal -> daily IC under W0-a, pins into
   `v8-prereg.md`, chained `protocol` line, `lock --write` on base-lo1 / base-lo3). Re-hash the live stage manifests
   before R10 (W0-n). Fields v9 is built on final code, so v10 from v9 expects 49 reused / 21 computed and v11
   70 / 3 (PM3-5a).
5. Cells B0a, B0b, B0c through `research_cycle.py run scripts/specs/v8/*.json` (lock, plan, run; command order in
   `task-A2-report.md`). B0c = the winner with `--label-role` on the delisting-returns role (E-25; the lo1 path
   exists, E-39; name the delisting stage $DL and build the label role from the same pins and tool commit as the
   winner's role), `--warm-start-sessions 60` and the capacity curve (E-10, E-25, E-29); check gross at row
   `score_begin` (A-3). Year tables and the Appendix A block (now with `history reads 0`) on each. Diagnostics on
   B0c in `--only` splits (E-18). Release A/B.
6. Research cells in registered order, identity first, one cell each, parent = last accepted: R-1; R-2 (7 READY
   members plus the 8 `_f49` re-screens through `add-alpha` and `run --screen`; fields v10 first); R-3
   (`ew-theme-std-aim-v1` if R-1 was accepted, else `ew-theme-aim-v2`, E-27a/b); R-4; R-5; R-6 (risk model on the
   4-year role first; S_prior 20; the run voids itself on limits_unmet > 0, E-31a; read convergence counts and the
   tripwire before any return; shaping flags per E-26; E-14a criterion); R-7 (fields v11; acceptance per E-36);
   R-8 (`r8.json`, E-43; check the risk store's capped_specific count first). Then by E-38 / E-45: if R-6 was
   accepted, R-10 (`r10.json`), R-11 (`r11.json`), R-12 (fields v12, the three LIB2 candidates through `add-alpha`
   and `run --screen`, E-42); if R-6 was rejected, R-9 (three theta cells). A rejected cell is never retried.
   Hard budget N <= 51; admission trials <= 15 plus the 8 re-screens.
7. V8-F cumulative test and freeze gate (one-sided p, E-34; print both). OD-3 history read only if the gate asks
   for it (E-17, E-35, E-41). H-2 measurement. Scorecard v8 (P-2 criteria complete; ladder checks P-3), pitch
   config filled and rendered with 0 unavailable blocks, the v7 pitch re-rendered. Pitch claims follow status 2
   section 9: B0c is the baseline; an improvement is claimed only where the cumulative test or a registered
   mechanical criterion supports it; if the freeze gate is unmet the scorecard says so and names OD-3. Then
   handoff 2 and the merge command for the owner.
