# platform v8: goal prompt for the next parent agent (after status 2)

Paste the block below into `/goal`. The files it names are in the root worktree `C:/atx-wt/pool-2`
(branch `feat/platform-v8-20260929`), not in `C:/atx`.

```
Finish atx-impl v8 (the mega alpha US equity long/short book) from the status at
`C:/atx-wt/pool-2/docs/plans/2026-09-30-platform-v8-status-2.md`, following the sprint plan
`docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md` and the ledger `.superpowers/sdd/platform-v8-20260929/progress.md`
(rulings E-1..E-34, W0-a..W0-m, R2-a..h, R7-a..c are binding; `integration-log.md` has every merge, build and test).

Working method (owner directive, unchanged): subagent-driven development with parallel Opus 5.5 lanes, one pool
worktree per lane (pools 3, 4, 7, 8, 9, 10, 11; root is `C:/atx-wt/pool-2`); no test-driven development; no review
agent after each task; adversarial reviews only at logical break points (end of a wave, the optimiser, the mining
verb), read-only, on the most capable model, split by area when the diff is too large for one reader. Children never
build or run data; one integrator agent at a time merges, builds, tests and runs data in root; the PM only
coordinates and rules. You are approved for every recommendation in the plan, handoff 1 and status 2; make rulings
yourself as "decision -- why -- cost if wrong" and write them to the ledger before the measurement they could bias.
The bounded runner needs a clean root tree: park ledger edits outside the repository while an integrator runs.
Merge lane work by commit SHA, not by branch name. Read `lane-rules.md` and `integrator-rules.md` before dispatching.

State at the start: root code head 237486fe, build v8-5 clean, suites pass (one pre-sprint failure). No role, field,
IC pass or cell exists on 2020-2023; trial count N = 37; admission trials 0. Unmerged lane heads: R45 126a5f5f
(pool 11, NAV `--label-role`, work in progress), A2 79440cfa (pool 8, add-alpha on a v8 parent, specs and templates,
`ew-theme-std-aim-v1`), H3 339c07b1 (pool 4, mining verb, never compiled).

Do, in this order:
1. Read status 2 sections 4, 6 and 7, the ledger from "PM session 2" down, and `review-w1-A.md`, `review-w1-B.md`,
   `review-w1-C.md` in the sprint directory.
2. Review fixes. One fix lane for every I and M finding of the Wave 1 review, first C-1 (verdict and DSR must use the
   pre-registered cross-trial variance), C-2, C-9, C-10, then C-3..C-7, C-13, B-2, B-3, B-4, A-2, A-3, A-4, A-1, B-1,
   C-11, C-12, and Ruling E-33 (ledger kind `mining-campaign`). Verify the findings marked "unverified" before
   fixing. Beside it, finish the review that the owner stop cut short: every test file, the files listed under "Not
   reviewed" in status 2 section 6, and everything merged after 7af37e9d. Then one scoped re-review of the fixes.
3. Integration 5. Lane R45 finishes Ruling E-25 (the three items in `task-E25-report.md`). Merge R45, then A2, then
   H3; build; run every suite; run the mining verb's fixture acceptance; one review of the mining verb and one of the
   optimiser (`solve_tracking`, spo-v3). Decide whether the pooled (era) fit must support `ew-theme-std-v1`.
4. Identities on the existing 3-year role, none run yet: NAV with every new flag absent (compare holdings against
   `build-equity/v7-w4-holdings`); `--hold-band 0`; `--adv-hold-q 1e9`; composition v8 with re-rank and cap off; H3
   warm u pass (48 of 48 hits); field reuse step 2 (expect 49 reused, 14 recomputed); spo-v2 side files;
   `--label-role` equal to `--role`. Capture the spo-v2 digest pin. A mismatch is a finding; never edit a golden.
5. Wave 0 data build on TRAIN 2020-2023 per `w0-2-runbook.md` R1-R13 (root only, bounded runner, clean tree, caps as
   ruled: IC 300 s / 2,560 MiB, preparation 600 s / 2,560 MiB, weighted pass with the theme block 3,072 MiB). Check
   free disk first (43 GB at the stop; about 23 GiB needed through B0c). Field overlap report first, then signal,
   then daily IC, under ruling W0-a. Pins into `v8-prereg.md`; the chained `protocol` ledger line; commit before
   every run.
6. Cells B0a, B0b, B0c through `research_cycle.py run scripts/specs/v8/*.json` (lock, plan, run; command order in
   `task-A2-report.md`). B0c = the winner with `--label-role` on the delisting-returns role, `--warm-start-sessions
   60` and the capacity curve (Rulings E-10, E-25, E-29); check gross at row `score_begin` (review A-3). Year tables
   and the Appendix A block on each. Diagnostics on B0c in `--only` splits (E-18). Release A/B.
7. Research cells in registered order, identity first, one cell each, parent = last accepted: R-1; R-2 (7 READY
   members plus the 8 `_f49` re-screens through `add-alpha` and `run --screen`; fields v10 first); R-3
   (`ew-theme-std-aim-v1` if R-1 was accepted, Ruling E-27); R-4; R-5; R-6 (risk model on the 4-year role first;
   S_prior 20 and the correlation criterion of E-14; read convergence counts and the tripwire before any return,
   E-31; shaping flags per E-26); R-7 (fields v11). A rejected cell is never retried with other parameters. Hard
   budget N <= 51 construction cells, admission trials <= 15 plus the 8 re-screens.
8. V8-F cumulative test and freeze gate (one-sided p, Ruling E-34; print both). H-2 measurement. Scorecard v8, pitch
   config filled and rendered with 0 unavailable blocks, the v7 pitch re-rendered. Pitch claims follow status 2
   section 9: B0c is the baseline; an improvement is claimed only where the cumulative test or a registered
   mechanical criterion supports it; if the freeze gate is unmet the scorecard says so and names OD-3. Then handoff 2
   and the merge command for the owner.

Commit or push nothing outside pool worktrees; never write in `C:/atx` or `atx-db/` (reading the pinned atx-db stage
files the runbook names is allowed). Hidden data rule: nothing dated 2024-01-01 or later is opened by any tool or
agent; record any slip as a disclosure. Stop only for an irreversible action (deleting the superseded caches for
disk space is one: ask the owner), a security-sensitive action, a merge to main, or a plan defect that leaves every
path a guess. Otherwise rule, log, and continue.
```

## Notes for the owner

- The prompt assumes the three unmerged lane branches stay as they are in pools 4, 8 and 11.
- If disk stays at 43 GB, Wave 0 through B0c fits (about 23 GiB [est]). The research cells after it add fields v10
  and v11 and one candidate cache per library; whether the remaining 20 GB is enough is not measured. Deleting the
  superseded caches (about 25.6 GiB, up to v6.1) frees the margin. That deletion is your decision.
- The first performance number of v8 appears at step 6 (B0c). Status 2 section 9 explains why no pitch with improved
  metrics is justified before step 8.
